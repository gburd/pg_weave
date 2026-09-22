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
# ARMS exists for the weights sweep, which runs the normalized arm at a dozen
# weight ratios and would otherwise re-run two arms that do not depend on the
# ratio -- the RRF control does not read `weights` at all.  The default runs all
# three, because a `norm`-only run has no positive control in it: the raw and rrf
# arms reproducing their recorded values is what makes the comparison admissible.
ARMS=${ARMS:-norm raw rrf}
# TAG keeps one sweep point from overwriting another's run files.
TAG=${TAG:+-$TAG}
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
    local arm=$1 gen=$2 pre=$3 out="$OUT/run-$DS-$1$TAG.tsv"
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

say "$DS: $NQ queries, depth $QDEPTH, weights $WEIGHTS, arms: $ARMS"
has_arm() { case " $ARMS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
R_NORM=""; R_RAW=""; R_RRF=""
if has_arm norm; then
    say "$DS: fused arm, normalizer ON (the product as it now ships)"
    R_NORM=$(run_arm norm fused_sql "SET pg_weave.fuse_normalize = on;")
fi
if has_arm raw; then
    say "$DS: fused arm, normalizer OFF (the recorded baseline)"
    R_RAW=$(run_arm raw fused_sql "SET pg_weave.fuse_normalize = off;")
fi
if has_arm rrf; then
    say "$DS: RRF control (same index, two over-fetches)"
    R_RRF=$(run_arm rrf rrf_sql "")
fi

score() { python3 bench/ndcg.py --qrels "$DIR/$DS/qrels.tsv" --run "$1" --k 10 --label "$2" | tail -1; }

printf 'label\tnqueries_scored\tndcg@10\trecall@100\tmrr@10\n' | tee "$OUT/quality-$DS$TAG.tsv"
for a in "norm:$R_NORM" "raw:$R_RAW" "rrf:$R_RRF"; do
    [ -n "${a#*:}" ] || continue
    score "${a#*:}" "${a%%:*}" | tee -a "$OUT/quality-$DS$TAG.tsv"
done

# ---------------------------------------------------------------------------
# WORK COUNTERS, and they belong in a quality-only script for the same reason
# nDCG does: A COUNT OF score() CALLS IS DETERMINISTIC.  It is not a latency, it
# does not care whose machine it runs on, and section 8's `score()` row is
# therefore measurable here rather than on EC2 -- which matters because that row
# was measured on the raw arm and normalization changes the MaxScore partition:
# `suffix[j]` is now a sum of comparable per-key ceilings instead of a BM25
# ceiling of 10-20 beside a vector ceiling of 0.3, so which channels are
# essential can move.  Whether it moves in the useful direction is the question.
#
# The counter line and the derivation are copied from bench/fuse.sh's work_of()
# verbatim, units included: `scores - vec_scores - gate_scores` IS the fused
# arm's lexical contribution count, in the same unit as the control's
# `lex_contribs`, and the vector side is compared in LANES on both arms.
# ---------------------------------------------------------------------------
work_of() {                     # work_of <gen> <pre>
    local gen=$1 pre=$2
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP $pre"
        echo "SELECT weave_fuse_stats_reset(); SELECT weave_work_stats_reset();"
        while IFS=$'\t' read -r qid wq qv; do
            $gen "$wq" "$qv" 10
            printf '\n'
        done < "$QLIT"
        echo "SELECT f.scores - f.vec_scores - f.gate_scores, w.lex_contribs, w.vec_lanes, w.vec_blocks, w.vec_blocks_bound_skipped, f.scores, f.pivots, f.blkskip FROM weave_fuse_stats() f, weave_work_stats() w;"
    } > "$sqlf"
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' -f "$sqlf" | tail -1
    rm -f "$sqlf"
}

W_NORM=""; W_RAW=""; W_RRF=""
if has_arm norm; then
    say "$DS: work counters, normalizer ON"
    W_NORM=$(work_of fused_sql "SET pg_weave.fuse_normalize = on;")
fi
if has_arm raw; then
    say "$DS: work counters, normalizer OFF"
    W_RAW=$(work_of fused_sql "SET pg_weave.fuse_normalize = off;")
fi
if has_arm rrf; then
    say "$DS: work counters, RRF control"
    W_RRF=$(work_of rrf_sql "")
fi

{
    printf 'arm\tlex_fused_side\tlex_wand\tvec_lanes\tvec_blocks\tvec_blocks_bound_skipped\tfuse_scores_total\tpivots\tblkskip\n'
    [ -n "$W_NORM" ] && printf 'norm\t%s\n' "$W_NORM"
    [ -n "$W_RAW" ] && printf 'raw\t%s\n' "$W_RAW"
    [ -n "$W_RRF" ] && printf 'rrf\t%s\n' "$W_RRF"
    :
} | tee "$OUT/work-$DS$TAG.tsv"

# The three ratios section 8's `score()` row is made of, computed here so nobody
# has to divide two table rows by hand and pick the wrong columns.  `blkskip` is
# printed beside them because it is the ONLY honest evidence about block-bound
# pruning inside a fused scan: `vec_blocks_bound_skipped` counts a skip the FUSED
# driver cannot reach at all -- it never calls weave_vec_shuttle_set_threshold(),
# so the shuttle's own floor stays -INFINITY and that counter is structurally
# zero.  `blkskip` is the fused core's own block-bound prune (src/am/fuse.c),
# which is the one that could move.
awk -F'\t' -v ds="$DS" 'NR>1{ for(i=2;i<=NF;i++) c[$1"_"i]=$i }
END {
    split("norm raw", arms, " ");
    printf "\n%s: score() calls vs the RRF control (fused/rrf; gate is <= 0.20x)\n", ds;
    for (a in arms) {
        n = arms[a];
        lex = c[n"_2"]; vec = c[n"_4"];
        rlex = c["rrf_3"]; rvec = c["rrf_4"];
        printf "  %-5s lexical %8.3fx   vector %8.3fx   total %8.3fx   (fused blkskip=%d, vec_blocks_bound_skipped=%d)\n",
            n, (rlex ? lex/rlex : 0), (rvec ? vec/rvec : 0),
            ((rlex+rvec) ? (lex+vec)/(rlex+rvec) : 0), c[n"_9"], c[n"_6"];
    }
}' "$OUT/work-$DS$TAG.tsv"

# The verdict, computed here rather than by eye: the gate row is "fused nDCG >= RRF".
N=$(awk -F'\t' '$1=="norm"{print $3}' "$OUT/quality-$DS$TAG.tsv")
R=$(awk -F'\t' '$1=="rrf"{print $3}' "$OUT/quality-$DS$TAG.tsv")
W=$(awk -F'\t' '$1=="raw"{print $3}' "$OUT/quality-$DS$TAG.tsv")
[ -n "$N" ] && [ -n "$R" ] && [ -n "$W" ] || { printf '\n%s: arms %s -- no RRF/raw control in this run, so no verdict line (see %s)\n' "$DS" "$ARMS" "$OUT/quality-$DS$TAG.tsv"; exit 0; }
awk -v n="$N" -v r="$R" -v w="$W" -v ds="$DS" 'BEGIN{
    printf "\n%s: normalized %.4f  raw %.4f  rrf %.4f  ->  norm/rrf %.3fx  raw/rrf %.3fx  [%s]\n",
        ds, n, w, r, n/r, w/r, (n >= r ? "GATE ROW MET" : "GATE ROW NOT MET")
}'
