#!/usr/bin/env python3
"""tsv2fvecs.py -- turn prepdata.py's corpus.tsv / queries.tsv into .fvecs.

WHY THIS EXISTS.  bench/code_scan.c already answers the question task V13 turns on --
"how many lanes and blocks does a bounded vector scan touch under CLUSTER ordering
versus the natural (docid-ascending) order" -- and it answers it with real k-means
(`order=clustered`) on real vectors.  What it cannot do is read a TSV: it takes
`.fvecs`, the SIFT/GIST convention (int32 dim, then dim float32, per record).  The
BEIR corpora this project measures nDCG on are already on disk as prepdata.py TSV
with MiniLM vectors in column 3, so the entire simulation is this converter plus two
existing-harness runs per dataset.  No on-disk format changes, no server, no EC2.

THREE TRAPS, each of which would produce a confident wrong answer rather than an error:

 1. **Dimension drift.**  Every record in an .fvecs must carry the same dim, and
    code_scan `die()`s on "ragged fvecs" only if the LATER records disagree with the
    first.  A file whose first record is short defines the wrong geometry for the
    whole run.  So the dim is taken from the first row, asserted against --dim when
    given, and every subsequent row is checked against it.

 2. **Normalization.**  pg_weave has no metric='cosine' (V16), so prepdata.py emits
    L2-NORMALIZED vectors and the benchmark queries them with inner product.
    code_scan's own generator normalizes too.  A corpus that arrived unnormalized
    would silently turn every inner product into a magnitude ranking -- the same
    failure prepdata.py's l2_normalize() comment warns about -- so the norm of every
    row is checked against 1.0 and a violation is fatal, not a warning.

 3. **Row order IS docid order, and that is the whole control arm.**  code_scan's
    `order=natural` keeps file order, and that arm only stands in for today's
    ascending-docid weft if this file preserves the TSV's order.  So rows are written
    in the order read, never sorted, and the docid column is checked to be ascending
    (prepdata.py assigns docids densely in emit order); a non-ascending input means
    the control arm would not be the control it is labelled as.

Usage:
    python3 bench/tsv2fvecs.py --in corpus.tsv --out corpus.fvecs [--dim 384]

Copyright (c) 2025-2026, Gregory Burd
"""

import argparse
import os
import struct
import sys

# The inner product of a unit vector with itself is 1; float32 accumulation over 384
# dims plus prepdata.py's own float->text->float round trip put the realistic error
# well inside this.  Loose enough not to fire on formatting, tight enough that an
# unnormalized corpus (norms of 3-20 for MiniLM before normalization) cannot pass.
NORM_TOL = 1e-3


def die(msg):
    print("tsv2fvecs.py: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def parse_vector(field, lineno):
    field = field.strip()
    if not (field.startswith("[") and field.endswith("]")):
        die("line %d: vector column is not bracketed: %r" % (lineno, field[:40]))
    body = field[1:-1]
    if not body:
        die("line %d: empty vector" % lineno)
    try:
        return [float(x) for x in body.split(",")]
    except ValueError as e:
        die("line %d: %s" % (lineno, e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True,
                    help="prepdata.py TSV: id, text, [v0,v1,...]")
    ap.add_argument("--out", dest="out", required=True, help=".fvecs to write")
    ap.add_argument("--dim", type=int, default=None,
                    help="assert this dimension (default: take it from row 1)")
    args = ap.parse_args()

    dim = args.dim
    nrows = 0
    prev_id = None
    ascending = True
    tmp = args.out + ".part"

    with open(args.inp, "r", encoding="utf-8") as f, open(tmp, "wb") as out:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                die("line %d: expected 3 tab-separated fields, got %d"
                    % (lineno, len(parts)))
            vec = parse_vector(parts[2], lineno)
            if dim is None:
                dim = len(vec)
            elif len(vec) != dim:
                die("line %d: dim %d, expected %d (ragged input would define the "
                    "whole run's geometry from row 1)" % (lineno, len(vec), dim))

            ss = 0.0
            for x in vec:
                ss += x * x
            if abs(ss - 1.0) > NORM_TOL:
                die("line %d: |v|^2 = %.6f, expected 1.0 -- this corpus is not L2 "
                    "normalized, so inner product would rank by magnitude"
                    % (lineno, ss))

            try:
                cur = int(parts[0])
            except ValueError:
                cur = None
            if cur is not None and prev_id is not None and cur < prev_id:
                ascending = False
            prev_id = cur if cur is not None else prev_id

            out.write(struct.pack("<i", dim))
            out.write(struct.pack("<%df" % dim, *vec))
            nrows += 1

    if nrows == 0:
        die("%r produced zero rows" % args.inp)
    if not ascending:
        die("the id column is not ascending, so file order is not docid order and "
            "code_scan's order=natural arm would not be the control it is labelled "
            "as (see trap 3 in this file's header)")

    os.replace(tmp, args.out)
    print("%s: %d rows, dim %d, %d bytes"
          % (args.out, nrows, dim, os.path.getsize(args.out)), file=sys.stderr)


if __name__ == "__main__":
    main()
