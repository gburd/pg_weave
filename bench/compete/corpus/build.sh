#!/usr/bin/env bash
#
# bench/compete/corpus/build.sh -- build one corpus, ONCE, identically for every
# engine host.
#
# This file exists because of the single most expensive error in this project's
# lineage.  pg_fts's 5-way comparison had to be retracted because pg_weave and
# VectorChord indexed `title||body` while pg_search and pg_textsearch indexed
# `body` alone: a 48% difference in postings scanned for the common term, spotted
# only when the match counts disagreed (commit 81532f4, recorded by the author as
# "my error").  The correction's *explanation* was then also wrong and required a
# second correction.
#
# The rule that prevents it: there is exactly ONE column, named `content`,
# produced by ONE generator, and every host proves it has the same bytes before
# building an index.
#
# Usage (on an engine host):
#   corpus/build.sh <corpus-id>       # writes /nvme/corpus.tsv + .sha256
#
# Corpora are generated deterministically from a seed rather than downloaded
# where possible.  A generated corpus is reproducible years later; a HuggingFace
# shard URL is not, and pg_fts's Wikipedia loader needed `iconv -c` fixes for
# invalid UTF-8 that silently changed the data.
#
# The exception is `wiki-*`, which must be downloaded because no generator
# reproduces real prose: the synthetic corpus averages 11.6 words per document
# and is therefore blind to an entire class of optimization (see the shape
# comment in the synth branch and bench/RESULTS_PORT_1_5_10.md).  A downloaded
# corpus is NOT deterministic the way a seeded generator is, so it carries an
# explicit cross-host checksum protocol; see the wiki branch.
#
set -euo pipefail

CORPUS=${1:?usage: build.sh <corpus-id>}
DEST=${DEST:-/nvme}
mkdir -p "$DEST"

# Set by a corpus branch that cannot guarantee bit-reproducibility from its id
# alone.  Empty for generated corpora, whose seed is the guarantee.
EXPECT_SHA=""

case "$CORPUS" in
  # -------------------------------------------------------------------------
  # synth-2m: 2,000,000 documents, Zipfian vocabulary, deterministic.
  #
  # Sized to match the 2.19M-article Wikipedia corpus every prior pg_fts 5-way
  # used, so latency bands stay roughly comparable, while being reproducible from
  # a seed on any host with no network dependency.
  #
  # Token shape matters and is a hard-won detail: tokens must contain NO
  # separator characters.  An earlier pg_weave corpus used `word_00042`, and
  # because the analyzer splits on non-word bytes that tokenized as TWO terms --
  # so every "single rare term" measurement was silently a two-term boolean AND,
  # visible only in the EXPLAIN Sort Key.
  # -------------------------------------------------------------------------
  synth-*)
    # Row count comes from the id itself, so the corpus name fully determines the
    # data: synth-200k, synth-2m, synth-10m.  A corpus whose size depends on an
    # environment variable is a corpus you cannot reproduce from a results file.
    case "$CORPUS" in
      *-200k*) N=200000 ;;
      *-1m*)   N=1000000 ;;
      *-2m*)   N=2000000 ;;
      *-10m*)  N=10000000 ;;
      *) echo "cannot parse row count from '$CORPUS'" >&2; exit 1 ;;
    esac
    # DOCUMENT LENGTH, and why it is a first-class corpus dimension rather than a
    # detail.  The default shape averages 11.6 words per document.  That corpus
    # measured the port of pg_fts 1.5.10's FOR-codec optimization at 1.06x on
    # common-term ranked, where upstream measured 1.56x on Wikipedia (avgdl 485).
    #
    # The explanation is the corpus, not the port.  weave_for_get cost `width`
    # branches per call, and width is derived from the largest value in the column.
    # At ~11 words per document almost every term frequency is 1, so the tf column
    # packs at 1 bit and the loop the optimization removed had ONE iteration.  At
    # avgdl 485 tf has real dynamic range, width is 4-8 bits, and the same change
    # removes 4-8 branches on the per-posting hot path.
    #
    # So a short-document corpus systematically UNDERSTATES every per-posting decode
    # optimization -- which is the entire class of work aimed at G13, the project's
    # largest competitive gap.  An instrument that cannot see the change it is
    # measuring is worse than no instrument, because it produces a confident null
    # result.
    #
    #   synth-2m         ~12 words/doc   short: selectivity and boolean behaviour
    #   synth-2m-long   ~120 words/doc   tf dynamic range; codec work is visible
    #
    # `wiki-2m` (avgdl ~485, the corpus every predecessor number is against) now
    # exists in this file and is what makes our numbers directly comparable to
    # pg_fts's published 5-way; see STRATEGY.md sect. 2.
    case "$CORPUS" in
      *-long) WORDS_LO=100; WORDS_HI=140 ;;
      *)      WORDS_LO=8;   WORDS_HI=15  ;;
    esac
    python3 - "$N" "$WORDS_LO" "$WORDS_HI" > "$DEST/corpus.tsv" <<'PY'
import random, sys
n = int(sys.argv[1])
wlo, whi = int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(20260907)          # frozen: the corpus is part of the result
VOCAB = 200000
# Zipf-ish via u^3 so rare terms carry real IDF signal.  A uniform vocabulary
# makes BM25 meaningless: every term gets the same IDF and ranked retrieval
# degenerates into an arbitrary tie-break.
def tok():
    return "word%06d" % (int(VOCAB * rng.random() ** 3) + 1)
# Planted patterns with KNOWN frequencies, so the fuzzy/regex/substring axes have
# ground truth and the analyzer can assert the corpus is what it claims.
for i in range(1, n + 1):
    nw = wlo + (i % (whi - wlo + 1))
    words = [tok() for _ in range(nw)]
    if i % 1000 == 0:
        words.append("zzqrare")                       # 0.1%  high-IDF marker
    if i % 100 == 0:
        words.append("connection refused")            # 1%    multi-word phrase
    if i % 5000 == 0:
        words.append("error E-%04d" % (i % 10000))    # 0.02% char-class regex
    if i % 20 == 0:
        words.append("government")                    # 5%    fuzzy target
    body = " ".join(words)
    print("%d\t%s" % (i, body))
PY
    ;;
  # -------------------------------------------------------------------------
  # wiki-2m / wiki-200k: real Wikipedia prose, from wikimedia/wikipedia
  # 20231101.en on HuggingFace.
  #
  # WHY THIS EXISTS.  `synth-2m` averages 11.6 words per document.  Wikipedia's
  # avgdl is ~485 -- 42x longer.  The port of pg_fts 1.5.10's FOR-codec
  # optimization measured 1.56x upstream (on Wikipedia) and 1.06x here, because
  # at 11.6 words/doc nearly every term frequency is 1, the tf column bit-packs
  # at ONE bit, and the per-bit loop the optimization removed had one iteration.
  # A short-document corpus systematically understates EVERY per-posting decode
  # optimization -- the entire class of work aimed at G13, the largest
  # competitive gap.  See bench/RESULTS_PORT_1_5_10.md.  wiki-2m also makes our
  # numbers directly comparable to pg_fts's published 5-way, which used it.
  #
  #   wiki-200k   first 200,000 articles     fast validation of the harness
  #   wiki-2m     first 2,188,038 articles   the comparable corpus
  #
  # 2,188,038 and not 2,000,000: that is the exact row count of every prior
  # pg_fts Wikipedia run, and matching it keeps IDF (which is a function of N)
  # and therefore the ranked latency bands comparable.
  #
  # THE CHECKSUM PROBLEM, stated honestly.  The harness's cross-host assertion
  # (engines/common.sh load_corpus, and the analyzer's fingerprint comparison)
  # requires every engine host to hold byte-identical data.  For a generated
  # corpus the seed guarantees that.  For a download it does not:
  #
  #   - HuggingFace can re-shard or re-publish a config; the parquet files under
  #     20231101.en/ are not immutable artifacts addressed by content hash here,
  #     they are a directory listing we sort by name.
  #   - The row order of a parquet scan is not contractually stable, and "the
  #     first N articles" is defined BY that order.
  #
  # Two mitigations, and one residual risk that cannot be mitigated here:
  #
  #   1. Output order is made deterministic: rows are sorted by numeric article
  #      id before corpus.tsv is written, so the same SET of articles always
  #      produces the same bytes regardless of shard or scan order.
  #   2. The FIRST host to build records the sha256 (as always, to
  #      $DEST/corpus.sha256).  Every subsequent host must be given that value in
  #      WIKI_SHA256, and this script ABORTS on mismatch rather than proceeding
  #      to build an index over different data.  orchestrate.sh must therefore
  #      pass WIKI_SHA256 to every host after the first.
  #
  # RESIDUAL RISK: mitigation (1) fixes the order of the set, not its
  # membership.  If upstream re-shards, "first 200,000 in scan order" can select
  # a different set of articles, and then every host built after that point
  # disagrees with every host built before it.  The WIKI_SHA256 assertion turns
  # that from a silent wrong comparison into a hard failure -- which is the best
  # available outcome, not a fix.  A corpus built today and a corpus built in a
  # year are not guaranteed identical, so a published wiki-2m result MUST quote
  # its corpus sha256 alongside the latency, and a re-run that cannot reproduce
  # the sha256 is a different corpus and must say so.
  #
  # KNOWN INTEGRATION GAP, which is not this file's to close.  The corpus FORMAT
  # needs no change in engines/common.sh -- load_corpus() consumes exactly
  # `id<TAB>content` plus corpus.sha256, which is what this emits.  But its query
  # BAND selection filters candidate terms with `WHERE w LIKE 'word%'`, a token
  # shape that exists only in the synthetic vocabulary.  On wiki-* that matches
  # only tokens literally beginning "word", so the rare/mid/common bands come out
  # arbitrary, or the strictly-increasing-df assertion fails outright.  Band
  # selection has to become corpus-aware before a wiki-* run can be measured, and
  # that is a change to common.sh, not to the corpus.
  # -------------------------------------------------------------------------
  wiki-*)
    case "$CORPUS" in
      *-200k*) N=200000 ;;
      *-2m*)   N=2188038 ;;
      *) echo "cannot parse row count from '$CORPUS'" >&2; exit 1 ;;
    esac
    # A downloaded corpus cannot self-certify; see the checksum discussion above.
    EXPECT_SHA=${WIKI_SHA256:-}

    # Python deps: pyarrow (parquet reader) and huggingface_hub (shard listing +
    # download).  Nothing else; no `datasets`, which would pull torch-scale
    # dependencies to read a parquet file.  They go in a venv under $DEST so a
    # fresh Amazon Linux 2023 host with only python3 needs no other preparation
    # and nothing is installed system-wide.  Some minimal AL2023 AMIs ship a
    # python3 without ensurepip, hence the dnf fallback.
    WIKI_VENV=${WIKI_VENV:-$DEST/.wikienv}
    if ! "$WIKI_VENV/bin/python" -c 'import pyarrow, huggingface_hub' 2>/dev/null; then
        rm -rf "${WIKI_VENV:?}"
        python3 -m venv "$WIKI_VENV" 2>/dev/null || true
        if [ ! -x "$WIKI_VENV/bin/pip" ]; then
            sudo dnf install -y -q python3-pip >/dev/null 2>&1 || true
            rm -rf "${WIKI_VENV:?}"
            python3 -m venv "$WIKI_VENV"
        fi
        "$WIKI_VENV/bin/pip" install -q --disable-pip-version-check \
            pyarrow huggingface_hub
    fi

    "$WIKI_VENV/bin/python" - "$N" "$DEST" <<'PY'
import os, re, sys
import pyarrow as pa
import pyarrow.parquet as pq
from huggingface_hub import hf_hub_download, list_repo_files

target = int(sys.argv[1])
dest = sys.argv[2]
REPO, CONFIG = "wikimedia/wikipedia", "20231101.en"

files = sorted(f for f in list_repo_files(REPO, repo_type="dataset")
               if f.startswith(CONFIG + "/") and f.endswith(".parquet"))
if not files:
    sys.exit("no parquet shards found for %s/%s" % (REPO, CONFIG))
print("%d parquet shards in %s/%s" % (len(files), REPO, CONFIG), file=sys.stderr)

# TEXT CLEANING, and exactly what it drops.
#
# pg_fts's loader hit invalid UTF-8 and fixed it with `iconv -c` after the fact;
# its bench/NOTE_CORPUS_20M.md records that this silently changed the data.  We
# do it here instead, so the change is visible, counted, and reported:
#
#   - Columns are read as BINARY and decoded ourselves.  A value that is not
#     valid UTF-8 is re-decoded with errors="ignore", which DROPS the offending
#     byte sequences (the same effect as `iconv -c`) and is counted as
#     wiki_rows_invalid_utf8.  Postgres rejects invalid UTF-8 outright, so the
#     alternative is a failed COPY, not fidelity.
#   - Every C0 control byte (0x00-0x1F, which includes the TAB and NEWLINE that
#     would break the TSV, and the 0x08 that COPY's QUOTE E'\b' relies on being
#     absent), plus 0x7F, plus runs of spaces, collapse to a SINGLE space.
#     Collapsing matters for more than tidiness: the harness measures
#     avg_words_per_doc by counting spaces, so uncollapsed runs would inflate the
#     one number this corpus exists to provide.
#   - Rows that clean to the empty string are dropped (content is NOT NULL, and
#     an empty document is not a document): wiki_rows_empty.
#   - Duplicate article ids are dropped, keeping the first: docs.id is a
#     PRIMARY KEY in load_corpus, so a duplicate would abort the COPY.
WS = re.compile(r"[\x00-\x20\x7f]+")

n = 0
seen = set()
bad_utf8 = 0
n_empty = 0
n_dup = 0
n_badid = 0
cache = os.path.join(dest, "hf")
unsorted_path = os.path.join(dest, "corpus.unsorted")


def dec(b):
    global bad_utf8
    if b is None:
        return ""
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        bad_utf8 += 1
        return b.decode("utf-8", "ignore")


with open(unsorted_path, "wb") as out:
    for fi, f in enumerate(files):
        if n >= target:
            break
        p = hf_hub_download(REPO, f, repo_type="dataset", cache_dir=cache)
        t = pq.read_table(p, columns=["id", "title", "text"])
        ids = t.column("id").cast(pa.binary()).to_pylist()
        tis = t.column("title").cast(pa.binary()).to_pylist()
        txs = t.column("text").cast(pa.binary()).to_pylist()
        del t
        for i, ti, bo in zip(ids, tis, txs):
            if n >= target:
                break
            try:
                idv = int(dec(i))
            except ValueError:
                n_badid += 1
                continue
            if idv in seen:
                n_dup += 1
                continue
            # ONE column.  title + ' ' + body, joined, indexed by every engine.
            # This is the retraction rule from STRATEGY.md sect. 2: pg_fts's
            # 5-way had to be withdrawn because two engines indexed title||body
            # and two indexed body alone -- a 48% difference in postings scanned
            # for the common term.  There is exactly one column and no engine
            # gets to choose a different one.
            c = WS.sub(" ", dec(ti) + " " + dec(bo)).strip()
            if not c:
                n_empty += 1
                continue
            seen.add(idv)
            out.write(b"%d\t%s\n" % (idv, c.encode("utf-8", "ignore")))
            n += 1
        # Delete the shard NOW so peak disk is one shard, not the whole dump
        # (~20 GB for 20231101.en).  hf_hub_download returns a symlink into
        # cache/blobs; removing only the symlink -- which the reference fetcher
        # in pg_fts/bench/get_wikipedia.py does -- leaves the blob behind and the
        # bound does not hold.  Remove both.
        real = os.path.realpath(p)
        for q in (p, real):
            try:
                os.remove(q)
            except OSError:
                pass
        print("shard %d/%d done, %d rows" % (fi + 1, len(files), n),
              file=sys.stderr)

if n < target:
    sys.exit("only %d rows available, wanted %d: the config was re-published "
             "with fewer articles, or a shard failed to download" % (n, target))
print("wiki_rows=%d" % n)
print("wiki_rows_invalid_utf8=%d" % bad_utf8)
print("wiki_rows_empty_after_clean=%d" % n_empty)
print("wiki_rows_duplicate_id=%d" % n_dup)
print("wiki_rows_nonnumeric_id=%d" % n_badid)
PY
    # Deterministic output order, which is what makes the sha256 an assertion
    # about the DATA rather than about the shard layout.  Article ids are unique
    # (deduped above), so a numeric sort is a total order and the byte output is
    # independent of scan order, shard boundaries and `sort` parallelism.
    # LC_ALL=C so no locale can reorder anything.
    #
    # External sort rather than sorting in Python: 2.19M Wikipedia articles are
    # ~7 GB of text and holding them in a list to sort would make peak RSS, not
    # peak disk, the thing that fails.  Cost is transient disk of roughly 2x the
    # corpus in $DEST, which is why -T points there and not at /tmp.
    LC_ALL=C sort -t "$(printf '\t')" -k1,1n -S 25% -T "$DEST" \
        "$DEST/corpus.unsorted" > "$DEST/corpus.tsv"
    rm -f "$DEST/corpus.unsorted"
    ;;
  *)
    echo "unknown corpus '$CORPUS'" >&2
    echo "known: synth-200k synth-1m synth-2m synth-10m (deterministic, no network)" >&2
    echo "       synth-2m-long (~120 words/doc; any synth-* accepts -long)" >&2
    echo "       wiki-200k wiki-2m (wikimedia/wikipedia 20231101.en; network)" >&2
    exit 1
    ;;
esac

# Cross-host assertion for a corpus that cannot self-certify from its id.  The
# first host records; every later host is given WIKI_SHA256 and must match it.
# Abort rather than proceed: an index built over silently different data is the
# failure mode that forced pg_fts to retract a whole 5-way comparison.
GOT_SHA=$(sha256sum "$DEST/corpus.tsv" | awk '{print $1}')
if [ -n "$EXPECT_SHA" ] && [ "$EXPECT_SHA" != "$GOT_SHA" ]; then
    echo "FATAL: corpus sha256 mismatch for '$CORPUS'" >&2
    echo "  expected (WIKI_SHA256): $EXPECT_SHA" >&2
    echo "  got:                    $GOT_SHA" >&2
    echo "This host does not hold the same corpus as the reference host, so no" >&2
    echo "comparison against it is valid.  Aborting before any index is built." >&2
    exit 1
fi

printf '%s\n' "$GOT_SHA" > "$DEST/corpus.sha256"
echo "corpus=$CORPUS rows=$(wc -l < "$DEST/corpus.tsv") bytes=$(stat -c%s "$DEST/corpus.tsv")"
# Report the achieved document length: it determines whether this corpus can see a
# per-posting decode optimization at all (see the shape comment above).  For
# wiki-* this number is the entire reason the corpus exists, so it is reported
# identically here rather than being trusted from the dataset card.
awk -F'\t' '{n += gsub(/ /," ") + 1} END {printf "avg_words_per_doc=%.1f\n", n/NR}' \
    "$DEST/corpus.tsv"
echo "sha256=$(cat "$DEST/corpus.sha256")"
if [ -n "$EXPECT_SHA" ]; then
    echo "sha256_asserted=ok"
fi
