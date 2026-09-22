#!/usr/bin/env python3
"""bench/ndcg.py -- score a retrieval run against qrels.

STDLIB ONLY.  This is the scoring half of the hybrid retrieval benchmark; the
generation half (bench/prepdata.py) produces qrels.tsv, and each arm under test
produces its own run.tsv by querying pg_weave and writing out what it retrieved.
Keeping the scorer independent of prepdata.py and of any particular database
driver means the same script scores a psql \\copy dump, a Python harness, or a
run produced by a competing extension -- the metric definitions cannot drift
between arms because there is only one place they are written down: here.

qrels.tsv : qid<TAB>docid<TAB>rel        (integer ids, graded relevance)
run.tsv   : qid<TAB>docid<TAB>rank       (rank starts at 1, ascending = better)

Usage:
    python3 bench/ndcg.py --qrels qrels.tsv --run run.tsv [--k 10] [--label NAME]
"""

import argparse
import math
import sys
from collections import defaultdict


def die(msg):
    # Every failure mode here is a corrupted benchmark input, not a bug in this
    # script, so the message goes to stderr and the exit code is nonzero: a
    # silently-empty or silently-truncated score line is worse than a crash,
    # because it looks like a real (bad) result instead of a broken harness.
    print("ndcg.py: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def read_qrels(path):
    """Return {qid: {docid: rel}}.

    Only rel > 0 counts as a positive judgment for recall/MRR, but rel == 0
    rows are kept too (graded nDCG needs the full judged set, and a qrels file
    legitimately lists known-irrelevant documents to distinguish "judged
    irrelevant" from "never judged").
    """
    qrels = defaultdict(dict)
    try:
        f = open(path, "r", newline="")
    except OSError as e:
        die("cannot open qrels file %r: %s" % (path, e))
    with f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                die("qrels %r line %d: expected 3 tab-separated fields, got %d"
                    % (path, lineno, len(parts)))
            qid, docid, rel = parts
            try:
                rel = int(rel)
            except ValueError:
                die("qrels %r line %d: relevance %r is not an integer"
                    % (path, lineno, rel))
            qrels[qid][docid] = rel
    if not qrels:
        die("qrels file %r contains no rows" % path)
    return qrels


def read_run(path):
    """Return {qid: [(rank, docid), ...]} sorted ascending by rank."""
    run = defaultdict(list)
    try:
        f = open(path, "r", newline="")
    except OSError as e:
        die("cannot open run file %r: %s" % (path, e))
    with f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                die("run %r line %d: expected 3 tab-separated fields, got %d"
                    % (path, lineno, len(parts)))
            qid, docid, rank = parts
            try:
                rank = int(rank)
            except ValueError:
                die("run %r line %d: rank %r is not an integer" % (path, lineno, rank))
            if rank < 1:
                die("run %r line %d: rank %d is < 1 (ranks start at 1)"
                    % (path, lineno, rank))
            run[qid].append((rank, docid))
    for qid in run:
        run[qid].sort(key=lambda pair: pair[0])
        # Duplicate ranks or duplicate docids at different ranks both mean the
        # arm's own output is inconsistent about what "top-k" means for this
        # query, and every metric below silently assumes one docid per rank.
        ranks_seen = [r for r, _ in run[qid]]
        if len(set(ranks_seen)) != len(ranks_seen):
            die("run %r: qid %r has duplicate ranks" % (path, qid))
        docids_seen = [d for _, d in run[qid]]
        if len(set(docids_seen)) != len(docids_seen):
            die("run %r: qid %r retrieves the same docid more than once" % (path, qid))
    return run


def ndcg_at_k(ranked_docids, judgments, k):
    """Graded nDCG@k.  gain = relevance, discount = 1/log2(rank+1).

    IDCG comes from the IDEAL ranking of THIS query's judged documents
    (sorted by relevance descending), truncated to k -- not from a fixed
    per-dataset constant, because two queries with different numbers of
    positive judgments have different achievable ceilings and a shared IDCG
    would compare queries against the wrong denominator.
    """
    dcg = 0.0
    for i, docid in enumerate(ranked_docids[:k]):
        rel = judgments.get(docid, 0)
        if rel > 0:
            rank = i + 1  # 1-based, matches the log2(rank+1) discount
            dcg += rel / math.log2(rank + 1)
    ideal_rels = sorted(judgments.values(), reverse=True)[:k]
    idcg = 0.0
    for i, rel in enumerate(ideal_rels):
        if rel > 0:
            idcg += rel / math.log2(i + 2)
    if idcg == 0.0:
        # Callers filter out no-positive-judgment queries before this point,
        # so idcg == 0 here would be an internal inconsistency, not a normal
        # case; returning 0.0 quietly would hide that bug inside an average.
        die("internal: ndcg_at_k called on a query with no positive judgments")
    return dcg / idcg


def recall_at_100(ranked_docids, judgments):
    """|retrieved top-100 with rel>0| / |all judged rel>0 for the query|."""
    positives = {d for d, r in judgments.items() if r > 0}
    if not positives:
        die("internal: recall_at_100 called on a query with no positive judgments")
    retrieved_top100 = set(ranked_docids[:100])
    hit = len(retrieved_top100 & positives)
    return hit / len(positives)


def mrr_at_10(ranked_docids, judgments):
    """1 / rank of the first rel>0 doc within the top 10, else 0."""
    for i, docid in enumerate(ranked_docids[:10]):
        if judgments.get(docid, 0) > 0:
            return 1.0 / (i + 1)
    return 0.0


def main():
    parser = argparse.ArgumentParser(
        description="Score a retrieval run against qrels (nDCG@k, recall@100, "
                     "MRR@10).")
    parser.add_argument("--qrels", required=True, help="path to qrels.tsv")
    parser.add_argument("--run", required=True, help="path to run.tsv")
    parser.add_argument("--k", type=int, default=10, help="cutoff for nDCG (default 10)")
    parser.add_argument("--label", default="run", help="label printed in the output row")
    args = parser.parse_args()

    if args.k < 1:
        die("--k must be >= 1, got %d" % args.k)

    qrels = read_qrels(args.qrels)
    run = read_run(args.run)

    # Queries with zero positive judgments are excluded from every average
    # below.  Including them would add a 0 to the nDCG and MRR numerators
    # (fine, they are already bounded in [0,1]) but a 0/0 to recall, and
    # "define 0/0 as 0" or "define 0/0 as 1" are both defensible-sounding
    # choices that push the average in opposite directions depending on how
    # many such queries a dataset happens to have -- i.e. the choice would be
    # scoring the dataset's qrels coverage, not the ranker.
    excluded = 0
    # A qid in qrels but absent from the run is NOT excluded: the arm was
    # asked for that query and returned nothing usable, which is exactly the
    # failure a benchmark needs to catch.  Excluding it would let an arm that
    # crashes on hard queries look like it was only ever tested on easy ones.
    missing_from_run = 0

    scored_ndcg = []
    scored_recall = []
    scored_mrr = []

    for qid, judgments in qrels.items():
        positives = {d for d, r in judgments.items() if r > 0}
        if not positives:
            excluded += 1
            continue
        ranked = [docid for _, docid in run.get(qid, [])]
        if qid not in run:
            missing_from_run += 1
        scored_ndcg.append(ndcg_at_k(ranked, judgments, args.k))
        scored_recall.append(recall_at_100(ranked, judgments))
        scored_mrr.append(mrr_at_10(ranked, judgments))

    n = len(scored_ndcg)
    if n == 0:
        die("every query in %r had zero positive judgments; nothing to score"
            % args.qrels)

    mean_ndcg = sum(scored_ndcg) / n
    mean_recall = sum(scored_recall) / n
    mean_mrr = sum(scored_mrr) / n

    print("label\tnqueries_scored\tndcg@%d\trecall@100\tmrr@10" % args.k)
    print("%s\t%d\t%.4f\t%.4f\t%.4f"
          % (args.label, n, mean_ndcg, mean_recall, mean_mrr))
    print("# excluded (no positive judgments): %d" % excluded, file=sys.stderr)
    print("# scored but absent from run (penalized, not excluded): %d"
          % missing_from_run, file=sys.stderr)
    if missing_from_run > 0:
        # A nonzero count here almost always means qid mapping drifted between
        # prepdata.py's output and whatever produced run.tsv (e.g. re-running
        # prepdata with a different --seed regenerates querymap.tsv), not that
        # the ranker legitimately returned nothing for those queries -- so
        # this is flagged loudly rather than folded silently into the mean.
        print("# WARNING: nonzero missing-from-run count usually means the "
              "harness broke (id mapping mismatch), not that the ranker is "
              "bad -- check qid mapping before trusting this score",
              file=sys.stderr)


if __name__ == "__main__":
    main()
