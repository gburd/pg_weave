#!/usr/bin/env bash
#
# normsweep.sh -- how sensitive is fused nDCG to the key-vs-key weight ratio?
#
# WHY THIS EXISTS.  bench/fuse.sh's FUSE_NORMSTUDY arms established that dividing
# each fuse() key by its REALIZED per-query maximum beats RRF on all three BEIR
# datasets, while the shipping raw sum loses on all three (doc/GAPS.md G44).  But a
# single-pass threshold scan cannot know a realized maximum before it starts, and the
# pre-scan substitute -- the key's ceiling -- was measured to leave a median 1.37x
# relative misweighting between the two keys (range 0.88-2.09x).
#
# The question that decides whether the cheap implementation is good enough is
# therefore NOT "how tight is the ceiling" but "how much does nDCG move when the key
# ratio is wrong by that much".  This sweeps the ratio directly.  If nDCG is flat
# across 1.37x, ceiling normalization is safe and the one-pass fix is the whole fix.
# If it is sharply peaked, the ceiling is not good enough and the normalizer has to
# estimate the realized maximum instead.
#
# It also answers a product question nobody has asked yet: how carefully does a user
# have to choose `weights`?  A flat curve means the knob is forgiving; a peaked one
# means the default matters much more than it looks.
#
# COST NOTE, and it is why this is a separate script rather than more arms in
# fuse.sh.  Every ratio shares the SAME two exhaustive per-channel scans -- only the
# final ORDER BY differs -- so materializing the per-channel scores ONCE per query
# and evaluating every ratio from that is one pass instead of N.  On fiqa a single
# study arm takes ~25 minutes; a seven-point sweep done the naive way would take
# three hours.
#
# Usage: bench/normsweep.sh <datadir> <dataset> [ratios...]
#   ratios are lexical:vector multipliers applied AFTER max-normalization, so 1.0 is
#   the `maxn` scheme that beat RRF.
#
set -euo pipefail

DIR=${1:?usage: normsweep.sh <datadir> <dataset> [ratios...]}
DS=${2:?usage: normsweep.sh <datadir> <dataset> [ratios...]}
shift 2 || true
RATIOS=${*:-"0.25 0.5 0.73 1.0 1.37 2.0 4.0"}

DB=${PGDATABASE:?set PGDATABASE to a database bench/fuse.sh has already loaded}
OUT=${OUT:-/scratch/pg_weave/normsweep}
QDEPTH=${QDEPTH:-100}
mkdir -p "$OUT"

PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

NDOCS=$($PSQL -t -A -c "SELECT count(*) FROM fd;")
say "$DS: $NDOCS docs, ratios: $RATIOS"

# fdmap must exist -- bench/fuse.sh builds it and this script deliberately does not,
# so the two cannot disagree about the docid <-> id mapping.
$PSQL -t -A -c "SELECT count(*) FROM fdmap;" >/dev/null 2>&1 \
    || { echo "normsweep: fdmap missing; run bench/fuse.sh on $DS first" >&2; exit 1; }

QLIT=$(mktemp); trap 'rm -f "$QLIT"' EXIT
$PSQL -t -A -F $'\t' -c \
    "SELECT qid, quote_literal(wq), quote_literal(qv::text) FROM fq ORDER BY qid;" > "$QLIT"

# One run file per ratio, in the qid<TAB>docid<TAB>rank format bench/ndcg.py consumes.
i=0; for r in $RATIOS; do i=$((i + 1)); : > "$OUT/run-$DS-i$i.tsv"; done

NQ=0
while IFS=$'\t' read -r qid wq qv; do
    NQ=$((NQ + 1))
    # ONE statement per query, all ratios evaluated from one materialized scan pair.
    # `s` is materialized explicitly: without it the planner is free to re-run both
    # SRFs once per ratio, which is exactly the cost this script exists to avoid.
    $PSQL -t -A -F $'\t' <<SQL >> "$OUT/.sweep.$$"
WITH a AS MATERIALIZED (
    SELECT m.id, s.score FROM weave_search('fd_weave', $wq::wquery, $NDOCS) s
      JOIN fdmap m ON m.rowtid = s.ctid
), v AS MATERIALIZED (
    SELECT m.id, s.score FROM weave_vec_scan('fd_weave', $qv::wvec, $NDOCS) s
      JOIN fdmap m USING (docid)
), mx AS (
    SELECT GREATEST((SELECT max(score) FROM a), 1e-9) AS ml,
           GREATEST((SELECT max(score) FROM v), 1e-9) AS mv
), s AS MATERIALIZED (
    SELECT f.id,
           COALESCE(a.score, 0) / mx.ml AS nl,
           COALESCE(v.score, 0) / mx.mv AS nv
      FROM fd f LEFT JOIN a ON a.id = f.id LEFT JOIN v ON v.id = f.id CROSS JOIN mx
), r AS (
    -- Keyed by the ratio's ORDINAL, not its value.  PostgreSQL renders float8 1.0
    -- as 1, so naming the per-ratio output files after the value silently wrote
    -- r1.tsv where the reader expected r1.0.tsv -- and the missing file scored a
    -- clean 0.0000, which reads as "this ratio is catastrophic" rather than as
    -- "there is no data here".  Three of seven points in the first run were that.
    --
    -- NO BACKTICKS IN THIS COMMENT.  The heredoc below is UNQUOTED, because it has
    -- to interpolate the query literals, so a backtick in an SQL comment is command
    -- substitution: the first version of this note made the shell try to run
    -- r1.tsv as a command, once per query.
    SELECT u.ord, id,
           row_number() OVER (PARTITION BY u.ord
                              ORDER BY u.ratio * nl + nv DESC, id) AS rk
      FROM s, unnest(ARRAY[$(echo "$RATIOS" | tr ' ' ',')]::float8[])
               WITH ORDINALITY AS u(ratio, ord)
)
SELECT ord, id, rk FROM r WHERE rk <= $QDEPTH ORDER BY ord, rk;
SQL
    # Split the one result set into per-ratio run files.
    awk -v OFS='\t' -v qid="$qid" -v out="$OUT" -v ds="$DS" \
        '{ f = out "/run-" ds "-i" $1 ".tsv"; print qid, $2, $3 >> f }' "$OUT/.sweep.$$"
    : > "$OUT/.sweep.$$"
done < "$QLIT"
rm -f "$OUT/.sweep.$$"
say "$DS: swept $NQ queries"

printf '\n### norm_sweep %s\n' "$DS"
printf 'lex_vec_ratio\tnqueries\tndcg@10\trecall@100\tmrr@10\n'
i=0
for r in $RATIOS; do
    i=$((i + 1))
    f="$OUT/run-$DS-i$i.tsv"
    # A missing or empty run file must be an ERROR, not a 0.0000 row: a zero here is
    # indistinguishable from a real collapse, which is how the first run reported
    # three catastrophic ratios that were only absent files.
    [ -s "$f" ] || { echo "normsweep: no rows for ratio $r ($f)" >&2; exit 1; }
    line=$(python3 bench/ndcg.py --qrels "$DIR/$DS/qrels.tsv" \
             --run "$f" --k 10 --label "r$r" | tail -1)
    printf '%s\t%s\n' "$r" "$(echo "$line" | cut -f2-)"
done
