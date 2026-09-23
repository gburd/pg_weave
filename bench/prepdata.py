#!/usr/bin/env python3
"""bench/prepdata.py -- turn a public retrieval dataset into COPY-able TSVs.

STDLIB-ONLY DOWNLOAD/PARSE/EMIT PATH.  This script must run on a bare Python 3
with no third-party packages installed, because it is the thing that PRODUCES
the benchmark fixtures other tooling (and other people's laptops) consumes --
if it needed numpy or requests just to unzip a corpus, the benchmark could not
be reproduced anywhere that hasn't already been set up to run pg_weave itself.
The only exception is --embed minilm, which needs sentence-transformers to
compute real embeddings; that import is LAZY (inside embed_minilm()) precisely
so `--embed hash` and all the download/parse/emit code stay runnable without
it, and so `--help` and argument errors never require the model to be present.

Supported datasets:
    scifact, nfcorpus, fiqa   -- BEIR (corpus.jsonl / queries.jsonl / qrels)
    msmarco-sub               -- a size-capped SUBSAMPLE of MS MARCO passage
                                  (see build_msmarco_sub for why this is only
                                  valid for A/B comparisons, never leaderboard
                                  comparisons)

For each dataset this writes, under <out>/<dataset>/:
    corpus.tsv    docid<TAB>text<TAB>vec
    queries.tsv   qid<TAB>text<TAB>vec
    qrels.tsv     qid<TAB>docid<TAB>rel      (mapped integer ids)
    docmap.tsv    intid<TAB>origid
    querymap.tsv  intid<TAB>origid
    manifest.json dataset/embed/dims/counts/sha256 of every emitted file

Usage:
    python3 bench/prepdata.py --dataset scifact --out data --embed minilm
    python3 bench/prepdata.py --dataset msmarco-sub --out data --embed hash \\
        --limit 50000
"""

import argparse
import gzip
import hashlib
import json
import os
import random
import re
import sys
import tarfile
import urllib.request
import zipfile

BEIR_DATASETS = ("scifact", "nfcorpus", "fiqa")
BEIR_URL_TMPL = ("https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/"
                  "datasets/{name}.zip")
MSMARCO_BASE = "https://msmarco.z22.web.core.windows.net/msmarcoranking/"

VEC_DIM = 384  # matches all-MiniLM-L6-v2, so hash mode is drop-in comparable
               # in shape (not in quality -- see embed_hash) with minilm mode.

TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def die(msg):
    # A malformed download, a truncated zip, a qrels row with the wrong number
    # of fields -- all of these mean the FIXTURE is broken, and a fixture that
    # silently loses rows produces a benchmark number nobody can trust later.
    # Fail loudly instead of limping on with a partial dataset.
    print("prepdata.py: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# Download / cache
# --------------------------------------------------------------------------

def cache_path(cache_dir, url):
    name = url.rsplit("/", 1)[-1]
    return os.path.join(cache_dir, name)


def fetch(cache_dir, url):
    """Download url into cache_dir, skipping the download if a same-size file
    is already cached.

    Comparing sizes (rather than just "file exists") catches the common case
    of a previous run being killed mid-download: an interrupted transfer
    leaves a truncated file on disk, and treating "exists" as "complete" would
    make every subsequent step parse a truncated zip/tarball and fail in a
    confusing place far away from the actual cause.
    """
    os.makedirs(cache_dir, exist_ok=True)
    dest = cache_path(cache_dir, url)
    print("fetch: %s" % url, file=sys.stderr)
    req = urllib.request.Request(url, headers={"User-Agent": "pg_weave-bench/1"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            remote_len = resp.headers.get("Content-Length")
            remote_len = int(remote_len) if remote_len is not None else None
            if (remote_len is not None and os.path.exists(dest)
                    and os.path.getsize(dest) == remote_len):
                print("  cached (%d bytes), skipping re-download" % remote_len,
                      file=sys.stderr)
                return dest
            tmp = dest + ".part"
            with open(tmp, "wb") as out:
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            os.replace(tmp, dest)  # atomic, so a crash mid-write never leaves
                                   # a file at the final name that looks cached
    except OSError as e:
        die("failed to fetch %r: %s" % (url, e))
    print("  downloaded %d bytes" % os.path.getsize(dest), file=sys.stderr)
    return dest


# --------------------------------------------------------------------------
# Text cleanup / vector formatting (shared by every dataset)
# --------------------------------------------------------------------------

_WHITESPACE_TRANS = str.maketrans({"\t": " ", "\n": " ", "\r": " "})


def clean_text(text):
    """Collapse TAB/CR/LF to a single space so the field cannot break the
    TSV it is destined for, and strip surrounding whitespace so an
    otherwise-empty document doesn't sneak through as a string of spaces."""
    return text.translate(_WHITESPACE_TRANS).strip()


def format_vec(vec):
    """Render as the bracketed literal pg_weave's wvec input accepts, e.g.
    [0.123,-0.456].  6 significant digits keeps files an order of magnitude
    smaller than repr()'s full float precision while staying well under the
    single-precision (float4) storage this vector ends up in, so the extra
    digits repr() would print are not even representable once stored -- ie.
    they would be noise in the file, not signal in the index.
    """
    return "[" + ",".join("%.6g" % x for x in vec) + "]"


def l2_normalize(vec):
    """Manual, stdlib-only L2 normalization (no numpy).

    pg_weave's planner REFUSES metric='cosine' on CREATE INDEX (it only knows
    metric='ip' and metric='l2'), so cosine similarity is obtained the
    standard trick: normalize every vector to unit length up front and then
    query with inner product. Skipping this step does not fail loudly -- it
    just makes `<#>` compute the inner product of un-normalized vectors,
    which is a different (and here, meaningless) ranking, silently.
    """
    norm = sum(x * x for x in vec) ** 0.5
    if norm == 0.0:
        # A true zero vector has no direction; leaving it unnormalized (all
        # zeros) is the only sane behaviour -- dividing by zero would either
        # crash or, worse under naive code, produce NaNs that wvec's input
        # parser rejects far away from this line.
        return list(vec)
    return [x / norm for x in vec]


# --------------------------------------------------------------------------
# Embeddings
# --------------------------------------------------------------------------

def embed_hash(texts):
    """Deterministic, stdlib-only fake embedding.

    THIS EXISTS ONLY so the rest of the pipeline (download/parse/dedup/COPY)
    can be smoke-tested with no third-party packages and no GPU available.
    It hashes each token to a dimension and a sign and accumulates, which
    gives two texts sharing vocabulary a nonzero inner product -- enough to
    exercise the vector channel's plumbing -- but it carries none of the
    distributional signal a real sentence embedding does.

    ANY QUALITY NUMBER (nDCG, recall, MRR) PRODUCED IN THIS MODE IS
    MEANINGLESS AND MUST NEVER BE RECORDED IN A RESULTS FILE. It is only good
    for checking that the harness runs end to end and that ties/plumbing bugs
    aren't hiding behind "the model must just be bad at this query".
    """
    print(
        "\n"
        "########################################################\n"
        "# WARNING: --embed hash is a stdlib-only FAKE embedding.\n"
        "# Retrieval quality numbers from this mode are MEANINGLESS.\n"
        "# Do not record them in any RESULTS_*.md file.\n"
        "# Use --embed minilm for anything that will be reported.\n"
        "########################################################\n",
        file=sys.stderr,
    )
    out = []
    for text in texts:
        vec = [0.0] * VEC_DIM
        tokens = TOKEN_RE.findall(text.lower())
        for tok in tokens:
            digest = hashlib.sha256(tok.encode("utf-8")).digest()
            # 4 bytes for the dimension index, 1 more bit for the sign, taken
            # from disjoint parts of the digest so index and sign don't
            # correlate (a correlated sign would make longer shared substrings
            # bias the resulting vector's magnitude, not just its direction).
            index = int.from_bytes(digest[0:4], "big") % VEC_DIM
            sign = 1.0 if digest[4] & 1 else -1.0
            vec[index] += sign
        out.append(l2_normalize(vec))
    return out


def embed_minilm(texts):
    """Real embeddings via sentence-transformers/all-MiniLM-L6-v2 (384 dims).

    Imported HERE, not at module scope, so `--embed hash` (and --help, and
    every download/parse code path) never requires torch/sentence-transformers
    to be installed -- this is the ONE place in the file allowed to import a
    third-party package, and it is lazy specifically so the rest of the script
    keeps working on a bare stdlib Python.
    """
    try:
        from sentence_transformers import SentenceTransformer
    except ImportError as e:
        die("--embed minilm requires the 'sentence-transformers' package, "
            "which is not installed: %s" % e)
    model = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
    # normalize_embeddings=True does the L2 normalization inside the model
    # (via torch, not numpy we'd have to import ourselves) for the same
    # reason l2_normalize() exists for hash mode: pg_weave has no
    # metric='cosine', so cosine similarity must arrive pre-normalized and be
    # queried with inner product, and an unnormalized vector would silently
    # turn every `<#>` query into a magnitude-sensitive ranking instead.
    embeddings = model.encode(
        list(texts),
        batch_size=64,
        show_progress_bar=False,
        convert_to_numpy=True,
        normalize_embeddings=True,
    )
    dim = embeddings.shape[1]
    if dim != VEC_DIM:
        # Catches "someone swapped the model name" before it produces a
        # corpus.tsv full of vectors of a dimension the manifest doesn't
        # match, which would only surface later as a wvec dimension mismatch
        # at COPY time -- far from the actual mistake.
        die("all-MiniLM-L6-v2 produced dim=%d, expected %d" % (dim, VEC_DIM))
    return [row.tolist() for row in embeddings]


def embed_texts(mode, texts):
    if mode == "hash":
        return embed_hash(texts)
    if mode == "minilm":
        return embed_minilm(texts)
    die("unknown --embed mode %r" % mode)  # argparse choices should prevent this


# --------------------------------------------------------------------------
# BEIR (scifact / nfcorpus / fiqa)
# --------------------------------------------------------------------------

def load_beir(cache_dir, name, limit):
    url = BEIR_URL_TMPL.format(name=name)
    zip_path = fetch(cache_dir, url)

    try:
        zf = zipfile.ZipFile(zip_path)
    except zipfile.BadZipFile as e:
        die("%r is not a valid zip (partial download?): %s" % (zip_path, e))

    # BEIR zips extract to a top-level directory named after the dataset;
    # look members up by suffix rather than assuming the exact prefix, since
    # that prefix has changed between BEIR releases for at least one dataset.
    def find_member(suffix):
        matches = [n for n in zf.namelist() if n.endswith(suffix)]
        if not matches:
            die("%r: no member ending in %r" % (zip_path, suffix))
        if len(matches) > 1:
            die("%r: ambiguous member for suffix %r: %s"
                % (zip_path, suffix, matches))
        return matches[0]

    corpus = {}  # orig_id -> text
    with zf.open(find_member("corpus.jsonl")) as f:
        for lineno, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as e:
                die("corpus.jsonl line %d: %s" % (lineno, e))
            title = rec.get("title", "") or ""
            body = rec.get("text", "") or ""
            # Title carries a disproportionate amount of the topical signal
            # in short scientific/financial documents (scifact, fiqa), so
            # dropping it would make BEIR's own published baselines
            # incomparable to what this script produces.
            corpus[str(rec["_id"])] = (title + " " + body).strip()
            if limit is not None and len(corpus) >= limit:
                break

    queries = {}  # orig_id -> text
    with zf.open(find_member("queries.jsonl")) as f:
        for lineno, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as e:
                die("queries.jsonl line %d: %s" % (lineno, e))
            queries[str(rec["_id"])] = rec.get("text", "") or ""

    qrels = []  # (query_orig_id, doc_orig_id, rel)
    with zf.open(find_member("qrels/test.tsv")) as f:
        lines = f.read().decode("utf-8").splitlines()
    if not lines:
        die("qrels/test.tsv in %r is empty" % zip_path)
    header = lines[0].split("\t")
    if header != ["query-id", "corpus-id", "score"]:
        # The header is the dataset's own contract for column order; trusting
        # positional TSV fields without checking it would silently transpose
        # query and document ids if BEIR ever reordered columns.
        die("qrels/test.tsv header is %r, expected "
            "['query-id', 'corpus-id', 'score']" % header)
    for lineno, line in enumerate(lines[1:], 2):
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            die("qrels/test.tsv line %d: expected 3 fields, got %d"
                % (lineno, len(parts)))
        qid, docid, score = parts
        try:
            score = int(score)
        except ValueError:
            die("qrels/test.tsv line %d: score %r is not an integer"
                % (lineno, score))
        qrels.append((qid, docid, score))

    return corpus, queries, qrels


# --------------------------------------------------------------------------
# MS MARCO passage (subsampled)
# --------------------------------------------------------------------------

def parse_msmarco_qrels(text):
    """MS MARCO's qrels.dev.small.tsv is TREC-style: "qid 0 pid rel" with
    4 tab-separated fields (the "0" is an unused iteration column carried
    over from the TREC qrels format). Accept 3 fields too (qid, pid, rel)
    since some mirrors strip the iteration column, but refuse anything else
    rather than silently reading the wrong columns as qid/pid/rel."""
    qrels = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) == 4:
            qid, _iteration, pid, rel = parts
        elif len(parts) == 3:
            qid, pid, rel = parts
        else:
            die("qrels.dev.small.tsv line %d: expected 3 or 4 fields, got %d"
                % (lineno, len(parts)))
        try:
            rel = int(rel)
        except ValueError:
            die("qrels.dev.small.tsv line %d: relevance %r is not an integer"
                % (lineno, rel))
        qrels.append((qid, pid, rel))
    return qrels


def build_msmarco_sub(cache_dir, limit, seed):
    """Build a size-capped SUBSAMPLE of MS MARCO passage.

    *** NOT COMPARABLE TO PUBLISHED MS MARCO LEADERBOARD NUMBERS. ***
    The full collection is 8.8M passages; embedding all of them for a
    two-arm A/B benchmark is infeasible on a laptop and is not what this
    benchmark needs. This function instead keeps every passage referenced by
    qrels.dev.small.tsv (so nothing judged-relevant silently disappears) plus
    a seeded random sample of the remaining passages, for --limit passages
    total. That shrinks the pool of DISTRACTOR passages an arm has to rank
    against, which mechanically inflates nDCG/recall relative to full-corpus
    numbers -- a smaller haystack makes the needle easier to find regardless
    of ranker quality. The only valid use of the numbers this produces is
    comparing two arms against the IDENTICAL subsample; comparing them to any
    number from the literature, or to a run built with a different --limit or
    --seed, is comparing different benchmarks that happen to share a name.
    """
    if limit is None:
        die("--dataset msmarco-sub requires --limit (subsample size)")

    qrels_path = fetch(cache_dir, MSMARCO_BASE + "qrels.dev.small.tsv")
    queries_tar_path = fetch(cache_dir, MSMARCO_BASE + "queries.tar.gz")
    collection_path = fetch(cache_dir, MSMARCO_BASE + "collection.tar.gz")

    with open(qrels_path, "r", encoding="utf-8") as f:
        qrels = parse_msmarco_qrels(f.read())
    if not qrels:
        die("qrels.dev.small.tsv parsed to zero rows")

    # THE QUERIES ARE RECONSTRUCTED, NOT DOWNLOADED (doc/GAPS.md G45).
    # `queries.dev.small.tsv` used to be fetchable next to the qrels at this same
    # base and now returns 404 -- the surrounding files (`qrels.dev.small.tsv`,
    # `collection.tar.gz`, `queries.tar.gz`) all still return 200, so the source is
    # alive and one file left it.  `queries.tar.gz` carries `queries.dev.tsv`, the
    # FULL 101,093-query dev set, and "dev.small" is by definition the subset of
    # dev whose qids appear in `qrels.dev.small.tsv`.  Filtering therefore
    # reproduces the missing file exactly rather than approximating it: verified
    # 2026-09-23, 6,980 unique qids in the qrels and 6,980 rows matched in
    # queries.dev.tsv, which is the published size of dev.small.
    #
    # The assertion below is the part that matters.  A query id in the qrels with
    # no text would give a scored arm a query it cannot answer and an nDCG row
    # that silently averages in zeros -- the same class of failure as dropping a
    # judged passage, which is what the docstring above exists to prevent.  So a
    # miss is fatal, not skipped.
    needed_qids = {qid for qid, _pid, _rel in qrels}
    queries = {}
    try:
        qtf = tarfile.open(queries_tar_path, mode="r:gz")
    except tarfile.TarError as e:
        die("%r is not a valid tar.gz (partial download?): %s"
            % (queries_tar_path, e))
    with qtf:
        qmember = None
        for candidate in qtf.getmembers():
            if candidate.name.endswith("queries.dev.tsv"):
                qmember = candidate
                break
        if qmember is None:
            die("%r: no queries.dev.tsv member (members: %s)"
                % (queries_tar_path, ", ".join(qtf.getnames())))
        qstream = qtf.extractfile(qmember)
        if qstream is None:
            die("%r: queries.dev.tsv member could not be opened"
                % queries_tar_path)
        for lineno, raw_line in enumerate(qstream, 1):
            line = raw_line.decode("utf-8").rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 2:
                die("queries.dev.tsv line %d: expected 2 fields, got %d"
                    % (lineno, len(parts)))
            qid, text = parts
            if qid in needed_qids:
                queries[qid] = text

    missing_qids = needed_qids - set(queries)
    if missing_qids:
        die("%d of %d qrels query ids have no text in queries.dev.tsv "
            "(e.g. %s); dev.small cannot be reconstructed from this source"
            % (len(missing_qids), len(needed_qids),
               ", ".join(sorted(missing_qids)[:5])))
    print("reconstructed dev.small: %d queries from queries.dev.tsv, "
          "filtered by %d qrels rows" % (len(queries), len(qrels)),
          file=sys.stderr)

    required_pids = {pid for _qid, pid, _rel in qrels}
    if len(required_pids) > limit:
        # If --limit is smaller than the judged set itself, "keep everything
        # judged plus a random sample of the rest" is not satisfiable -- and
        # silently truncating the required set would drop ground truth,
        # exactly the failure mode the docstring above warns about.
        die("--limit %d is smaller than the %d passages referenced by qrels; "
            "raise --limit or the dev-small judgments cannot all be kept"
            % (limit, len(required_pids)))

    rng = random.Random(seed)
    reservoir_capacity = limit - len(required_pids)
    corpus = {}          # required pids, kept unconditionally
    reservoir = {}        # pid -> text, Algorithm R reservoir of the rest
    seen_optional = 0

    print("streaming collection.tar.gz (this is the 8.8M-passage file; "
          "expect this to take a while)...", file=sys.stderr)
    try:
        tf = tarfile.open(collection_path, mode="r:gz")
    except tarfile.TarError as e:
        die("%r is not a valid tar.gz (partial download?): %s"
            % (collection_path, e))
    with tf:
        member = None
        for candidate in tf.getmembers():
            if candidate.name.endswith("collection.tsv"):
                member = candidate
                break
        if member is None:
            die("%r: no collection.tsv member" % collection_path)
        raw_stream = tf.extractfile(member)
        if raw_stream is None:
            die("%r: collection.tsv member could not be opened" % collection_path)
        for lineno, raw_line in enumerate(raw_stream, 1):
            line = raw_line.decode("utf-8").rstrip("\n")
            if not line:
                continue
            parts = line.split("\t", 1)
            if len(parts) != 2:
                die("collection.tsv line %d: expected 2 fields, got %d"
                    % (lineno, len(parts)))
            pid, text = parts
            if pid in required_pids:
                corpus[pid] = text
                continue
            # Algorithm R reservoir sampling: the collection is streamed once
            # and its total size is not known up front (that is the whole
            # point of streaming an 8.8M-line file instead of loading it), so
            # a fixed-size reservoir with replacement probability
            # capacity/i_seen is the standard way to get a uniform sample
            # without a second pass or holding the file in memory.
            seen_optional += 1
            if len(reservoir) < reservoir_capacity:
                reservoir[pid] = text
            else:
                j = rng.randrange(seen_optional)
                if j < reservoir_capacity:
                    # Evict a uniformly random existing entry, not always the
                    # same slot -- otherwise late arrivals could never enter
                    # a "full" reservoir and the sample would be biased
                    # towards whatever passages happened to come first.
                    victims = list(reservoir.keys())
                    evict_key = victims[rng.randrange(len(victims))]
                    del reservoir[evict_key]
                    reservoir[pid] = text

    corpus.update(reservoir)
    print("collection subsample: %d required (from qrels) + %d sampled = %d"
          % (len(required_pids), len(reservoir), len(corpus)), file=sys.stderr)

    return corpus, queries, qrels


# --------------------------------------------------------------------------
# Generic emit pipeline (shared by BEIR and msmarco-sub)
# --------------------------------------------------------------------------

def build_id_map(orig_ids):
    """Dense 1..N ids assigned in SORTED order of the original id.

    Sorting first makes the mapping a pure function of the id set, not of
    whatever order a zip member or a streamed tarball happened to yield them
    in -- otherwise re-running this script against the same cached inputs
    could produce a different docmap.tsv purely from dict/iteration-order
    nondeterminism, and two "identical" runs would silently disagree about
    which integer means which document.
    """
    ordered = sorted(orig_ids)
    return {orig: i + 1 for i, orig in enumerate(ordered)}


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def emit_dataset(out_dir, dataset, embed_mode, seed, limit,
                  corpus_raw, queries_raw, qrels_raw):
    os.makedirs(out_dir, exist_ok=True)

    # --- clean + drop empties (corpus) ------------------------------------
    corpus_clean = {}
    empty_docs = 0
    for orig_id, text in corpus_raw.items():
        text = clean_text(text)
        if not text:
            empty_docs += 1
            continue
        corpus_clean[orig_id] = text
    if not corpus_clean:
        die("every document in %r was empty after cleanup" % dataset)

    doc_id_map = build_id_map(corpus_clean.keys())  # orig -> dense int

    # --- keep only JUDGED queries -----------------------------------------
    #
    # A BEIR queries.jsonl holds every split -- scifact ships 1,109 queries and
    # 339 test judgments over 300 of them -- so emitting all of them would mean
    # 73 % of the benchmark's executions, work counters and latency samples came
    # from queries no metric can score.  The averages would be unaffected
    # (bench/ndcg.py excludes unjudged queries) but every OTHER number would be
    # dominated by them, and "p50 over the queries we scored" and "p50 over the
    # queries we ran" would silently be two different figures.
    #
    # Filtered on presence in qrels, not on rel > 0: a query whose every judgment
    # is negative is still a query the benchmark should run and score as zero.
    judged = {qid for qid, _doc, _rel in qrels_raw}
    queries_clean = {}
    empty_queries = 0
    unjudged_queries = 0
    for orig_id, text in queries_raw.items():
        if orig_id not in judged:
            unjudged_queries += 1
            continue
        text = clean_text(text)
        if not text:
            empty_queries += 1
            continue
        queries_clean[orig_id] = text
    if not queries_clean:
        die("no query in %r survived the judged-only filter" % dataset)

    query_id_map = build_id_map(queries_clean.keys())

    print("%s: %d docs (%d empty, dropped), %d queries "
          "(%d empty, %d unjudged, both dropped)"
          % (dataset, len(corpus_clean), empty_docs,
             len(queries_clean), empty_queries, unjudged_queries),
          file=sys.stderr)

    # --- embed -------------------------------------------------------------
    # Ordered lists so the embedding vector at position i lines up with the
    # id/text at position i -- a dict comprehension zipped against a
    # separately-ordered embed call would silently pair the wrong vector
    # with the wrong document as soon as dict iteration order didn't match.
    doc_ids_sorted = sorted(corpus_clean.keys(), key=lambda o: doc_id_map[o])
    doc_texts_sorted = [corpus_clean[o] for o in doc_ids_sorted]
    doc_vecs = embed_texts(embed_mode, doc_texts_sorted)

    query_ids_sorted = sorted(queries_clean.keys(), key=lambda o: query_id_map[o])
    query_texts_sorted = [queries_clean[o] for o in query_ids_sorted]
    query_vecs = embed_texts(embed_mode, query_texts_sorted)

    # --- write corpus.tsv / docmap.tsv -------------------------------------
    corpus_path = os.path.join(out_dir, "corpus.tsv")
    docmap_path = os.path.join(out_dir, "docmap.tsv")
    with open(corpus_path, "w", encoding="utf-8", newline="\n") as cf, \
            open(docmap_path, "w", encoding="utf-8", newline="\n") as mf:
        for orig_id, text, vec in zip(doc_ids_sorted, doc_texts_sorted, doc_vecs):
            intid = doc_id_map[orig_id]
            cf.write("%d\t%s\t%s\n" % (intid, text, format_vec(vec)))
            mf.write("%d\t%s\n" % (intid, orig_id))

    # --- write queries.tsv / querymap.tsv -----------------------------------
    queries_path_out = os.path.join(out_dir, "queries.tsv")
    querymap_path = os.path.join(out_dir, "querymap.tsv")
    with open(queries_path_out, "w", encoding="utf-8", newline="\n") as qf, \
            open(querymap_path, "w", encoding="utf-8", newline="\n") as mf:
        for orig_id, text, vec in zip(query_ids_sorted, query_texts_sorted,
                                       query_vecs):
            intid = query_id_map[orig_id]
            qf.write("%d\t%s\t%s\n" % (intid, text, format_vec(vec)))
            mf.write("%d\t%s\n" % (intid, orig_id))

    # --- write qrels.tsv, dropping judgments for docs/queries we cut --------
    qrels_out_path = os.path.join(out_dir, "qrels.tsv")
    nqrels_kept = 0
    nqrels_dropped = 0
    with open(qrels_out_path, "w", encoding="utf-8", newline="\n") as rf:
        for q_orig, d_orig, rel in qrels_raw:
            q_int = query_id_map.get(q_orig)
            d_int = doc_id_map.get(d_orig)
            if q_int is None or d_int is None:
                # A judged document missing from the emitted corpus (dropped
                # for being empty, or -- for msmarco-sub -- never selected
                # into the subsample) silently lowers the ceiling every arm
                # can reach for that query. Reporting the count is the only
                # way a reader notices a subsample ate some ground truth.
                nqrels_dropped += 1
                continue
            rf.write("%d\t%d\t%d\n" % (q_int, d_int, rel))
            nqrels_kept += 1
    if nqrels_kept == 0:
        die("every qrel was dropped -- corpus/query subsample shares no ids "
            "with qrels; check that ids were parsed from the right columns")

    # --- manifest ------------------------------------------------------------
    files = ["corpus.tsv", "queries.tsv", "qrels.tsv", "docmap.tsv",
             "querymap.tsv"]
    manifest = {
        "dataset": dataset,
        "embed_mode": embed_mode,
        "embed_model": ("sentence-transformers/all-MiniLM-L6-v2"
                         if embed_mode == "minilm" else None),
        "dim": VEC_DIM,
        "ndocs": len(corpus_clean),
        "nqueries": len(queries_clean),
        "nqrels": nqrels_kept,
        "nqrels_dropped": nqrels_dropped,
        "empty_docs_dropped": empty_docs,
        "empty_queries_dropped": empty_queries,
        "unjudged_queries_dropped": unjudged_queries,
        "limit": limit,
        "seed": seed,
        "sha256": {name: sha256_of(os.path.join(out_dir, name)) for name in files},
    }
    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")

    print(json.dumps(manifest, indent=2, sort_keys=True))
    return manifest


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Produce corpus/queries/qrels TSVs for a hybrid "
                     "(BM25 + vector) pg_weave retrieval benchmark.")
    parser.add_argument("--dataset", required=True,
                         choices=list(BEIR_DATASETS) + ["msmarco-sub"])
    parser.add_argument("--out", required=True,
                         help="output root; files land under <out>/<dataset>/")
    parser.add_argument("--embed", required=True, choices=["minilm", "hash"])
    parser.add_argument("--limit", type=int, default=None,
                         help="cap on corpus size; REQUIRED for msmarco-sub "
                              "(target subsample size), optional smoke-test "
                              "cap for the BEIR datasets")
    parser.add_argument("--seed", type=int, default=42,
                         help="seeds the msmarco-sub reservoir sample "
                              "(default 42, so 'the default run' is a fixed "
                              "benchmark rather than a fresh draw every time)")
    args = parser.parse_args()

    if args.limit is not None and args.limit < 1:
        die("--limit must be >= 1, got %d" % args.limit)

    out_dir = os.path.join(args.out, args.dataset)
    cache_dir = os.path.join(args.out, "_cache")

    if args.dataset in BEIR_DATASETS:
        corpus_raw, queries_raw, qrels_raw = load_beir(
            cache_dir, args.dataset, args.limit)
    elif args.dataset == "msmarco-sub":
        corpus_raw, queries_raw, qrels_raw = build_msmarco_sub(
            cache_dir, args.limit, args.seed)
    else:
        die("unhandled dataset %r" % args.dataset)  # argparse should prevent this

    emit_dataset(out_dir, args.dataset, args.embed, args.seed, args.limit,
                 corpus_raw, queries_raw, qrels_raw)


if __name__ == "__main__":
    main()
