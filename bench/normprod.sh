#!/usr/bin/env bash
#
# normprod.sh -- does the per-key normalizer flip section 8's nDCG row IN THE
# PRODUCT, rather than in a study?
#
# WHY A THIRD SCRIPT.  `bench/fuse.sh`'s FUSE_NORMSTUDY arms and `bench/normsweep.sh`
# both evaluated candidate objectives in SQL, from the index's own per-channel scores
# (doc/GAPS.md G44).  They said ceiling normalization should beat RRF on all three
# BEIR datasets.  That is a prediction about a scorer that did not exist yet; this
# script measures the scorer.  The arms are the SAME statement -- the shipping fused
# pushdown -- run twice, differing only in `pg_weave.fuse_normalize`, so the
# difference between them cannot be a difference in how they were measured.
#
# IT IS A QUALITY-ONLY SCRIPT AND THEREFORE NEEDS NO EC2: nDCG is deterministic and
# host-independent, and only latency needs a quiet machine.  That is also why it does
# not belong inside fuse.sh, whose latency passes take ~25 minutes per arm on fiqa.
#
# THE POSITIVE CONTROL IS THE POINT OF THE `raw` ARM.  With the normalizer off the
# fused arm must reproduce the recorded numbers -- scifact 0.6720, nfcorpus 0.3161,
# fiqa 0.2393 (bench/RESULTS_FUSE.md, EC2 c7i.8xlarge, commit d30ee5c) -- and the RRF
# control must reproduce 0.6846 / 0.3422 / 0.3482.  An arm that cannot reproduce the
# thing it claims to improve is measuring something else (hard rule 10).
#
# Usage: bench/normprod.sh <datadir> <dataset>
#   PGDATABASE must name a database bench/fuse.sh has already loaded (it needs `fd`,
#   `fq` and the `fd_weave` index; fdmap is not used here).
#
set -euo pipefail

DIR=${1:?usage: normprod.sh <datadir> <dataset>}
DS=${2:?usage: normprod.sh <datadir> <dataset>}
DB=${PGDATABASE:?set PGDATABASE to a database bench/fuse.sh has already loaded}
OUT=${OUT:-/scratch/pg_weave/normprod}
QDEPTH=${QDEPTH:-100}
WEIGHTS=${WEIGHTS:-'{0.5,0.5}'}
mkdir -p "$OUT"

PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

[ -s "$DIR/$DS/qrels.tsv" ] || die "no qrels at $DIR/$DS/qrels.tsv"

# THESE TWO GENERATORS ARE COPIES OF bench/fuse.sh's AND MUST STAY THAT WAY.  The
# copy is what the positive control above is for: a generator that has drifted
# produces a `raw` arm that does not reproduce the recorded number, which fails the
# run rather than quietly re-baselining it.
SETUP="SET max_parallel_workers_per_gather = 0; SET enable_seqscan = off;"
KP=${KP:-100}
RRFK=${RRFK:-60}

fused_sql() {                   # fused_sql <wq> <vec> <limit>
    printf "SELECT id FROM fd ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '%s') LIMIT %s;" \
           "$1" "$2" "$WEIGHTS" "$3"
}

rrf_sql() {                     # rrf_sql <wq> <vec> <limit>
    printf "WITH l AS (SELECT id, row_number() OVER (ORDER BY body <=> %s::wquery) AS r FROM fd ORDER BY body <=> %s::wquery LIMIT %s), v AS (SELECT id, row_number() OVER (ORDER BY emb <#> %s::wvec) AS r FROM fd ORDER BY emb <#> %s::wvec LIMIT %s) SELECT id FROM (SELECT id, 1.0/(%s + r) AS s FROM l UNION ALL SELECT id, 1.0/(%s + r) AS s FROM v) u GROUP BY id ORDER BY sum(s) DESC, id LIMIT %s;" \
           "$1" "$1" "$KP" "$2" "$2" "$KP" "$RRFK" "$RRFK" "$3"
}

QLIT=$(mktemp); trap 'rm -f "$QLIT"' EXIT
$PSQL -t -A -F $'\t' -c "SELECT qid, quote_literal(wq), quote_literal(qv::text) FROM fq ORDER BY qid;" > "$QLIT"
[ -s "$QLIT" ] || die "no queries in fq"
NQ=$(wc -l < "$QLIT")

# THE GUC MUST EXIST, and `SHOW` cannot tell you that -- a placeholder echoes back
# whatever was SET.  A real GUC has a short_desc, and pg_settings only carries it in
# a session that has LOADED the library, so the index is touched first.  AGENTS.md's
# twelfth member is exactly this mistake, made one day earlier.
GUCOK=$($PSQL -t -A -c "SELECT count(*) FROM fd WHERE false;
                        SELECT count(*) FROM pg_settings
                         WHERE name = 'pg_weave.fuse_normalize'
                           AND short_desc IS NOT NULL;" | tail -1)
[ "$GUCOK" = "1" ] || die "pg_weave.fuse_normalize is not a real GUC in this build \
(count=$GUCOK) -- an absent GUC is indistinguishable from one that is off"

# And the plan is asserted, or every number below describes the Sort fallback.
# IFS=$'\t' is load-bearing: a query literal contains spaces.
IFS=$'\t' read -r qid1 wq1 qv1 < <(head -1 "$QLIT")
PLAN=$( { echo "$SETUP"; printf 'EXPLAIN %s' "$(fused_sql "$wq1" "$qv1" 10)"; } \
        | psql -X -q -d "$DB" -t -A | tr '\n' ' ')
case "$PLAN" in
    *"Index Scan using fd_weave"*) : ;;
    *) die "the fused arm is NOT using the index -- plan was: $PLAN" ;;
esac

# One statement per query, ranks taken from psql's row order -- which IS the ranking
# for a single ORDER BY, with no window function to get wrong.  `pre` is prepended to
# the session so an arm can differ by a GUC and nothing else.
run_arm() {                     # run_arm <label> <gen> <pre>
    local arm=$1 gen=$2 pre=$3 out="$OUT/run-$DS-$1.tsv"
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP $pre"
        while IFS=$'\t' read -r qid wq qv; do
            printf "\\\\echo QID %s\n" "$qid"
            $gen "$wq" "$qv" "$QDEPTH"
            printf '\n'
        done < "$QLIT"
    } > "$sqlf"
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -f "$sqlf" \
      | awk -v OFS='\t' '/^QID /{q=$2; r=0; next} NF{r++; print q, $1, r}' > "$out"
    rm -f "$sqlf"
    [ -s "$out" ] || die "$arm produced no rows"
    printf '%s\n' "$out"
}

say "$DS: $NQ queries, depth $QDEPTH, weights $WEIGHTS"
say "$DS: fused arm, normalizer ON (the product as it now ships)"
R_NORM=$(run_arm norm fused_sql "SET pg_weave.fuse_normalize = on;")
say "$DS: fused arm, normalizer OFF (the recorded baseline)"
R_RAW=$(run_arm raw fused_sql "SET pg_weave.fuse_normalize = off;")
say "$DS: RRF control (same index, two over-fetches)"
R_RRF=$(run_arm rrf rrf_sql "")

score() { python3 bench/ndcg.py --qrels "$DIR/$DS/qrels.tsv" --run "$1" --k 10 --label "$2" | tail -1; }

printf 'label\tnqueries_scored\tndcg@10\trecall@100\tmrr@10\n' | tee "$OUT/quality-$DS.tsv"
for a in "norm:$R_NORM" "raw:$R_RAW" "rrf:$R_RRF"; do
    score "${a#*:}" "${a%%:*}" | tee -a "$OUT/quality-$DS.tsv"
done

# The verdict, computed here rather than by eye: the gate row is "fused nDCG >= RRF".
N=$(awk -F'\t' '$1=="norm"{print $3}' "$OUT/quality-$DS.tsv")
R=$(awk -F'\t' '$1=="rrf"{print $3}' "$OUT/quality-$DS.tsv")
W=$(awk -F'\t' '$1=="raw"{print $3}' "$OUT/quality-$DS.tsv")
awk -v n="$N" -v r="$R" -v w="$W" -v ds="$DS" 'BEGIN{
    printf "\n%s: normalized %.4f  raw %.4f  rrf %.4f  ->  norm/rrf %.3fx  raw/rrf %.3fx  [%s]\n",
        ds, n, w, r, n/r, w/r, (n >= r ? "GATE ROW MET" : "GATE ROW NOT MET")
}'
