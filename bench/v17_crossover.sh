#!/usr/bin/env bash
#
# v17_crossover.sh -- doc/PHASES.md V20, formerly V17 ("switch plan strategy on predicate
# selectivity"), MEASUREMENT ONLY: is there a selectivity below which scoring the
# qualifying set exactly beats the gated fused scan?
#
# THE ARMS.  Every arm answers the same question -- the top 10 rows satisfying PRED
# under  W1 * bm25 + W2 * ip  with pg_weave.fuse_normalize = off, so the weights
# mean the same raw sum on every arm -- by a different route:
#
#   A  the gated fused scan as it ships:
#        SELECT id FROM vx WHERE pred ORDER BY fuse(...) LIMIT 10
#      enable_seqscan = enable_bitmapscan = off, and the plan is ASSERTED to carry
#      `Order By` (and `Index Cond` when there is a predicate), because the failure
#      mode of not checking is a curve that measures the executor (gatesweep.sh
#      threat 2).
#   B  materialize-and-rescore in SQL, as the task specifies: the qualifying ids
#      LEFT JOIN weave_search(index, wq, N) on ctid (exact BM25, the index's own
#      statistics, every matching row because k = N) plus the EXACT inner product
#      from the heap vector, summed, top 10.
#   C  the same exact objective with the lexical half scored PER QUALIFYING ROW by
#      weave_bm25(body, wq, ndocs, avgdl, dfs) instead of the corpus-wide
#      weave_search(k = N).  B's lexical cost is proportional to the query terms'
#      posting lists whatever the selectivity, so B alone would understate what a
#      materializing plan can do at 0.01 %; C's is proportional to the qualifying
#      set, which is the shape zvec's switch has.  ndocs/avgdl/dfs are fetched once
#      per query before timing, as an in-AM switch would have them.
#   D  materialize-and-rescore against the INDEX's codes: weave_search(k = N) plus
#      weave_vec_scan(index, qv, n, docids) restricted to the qualifying docids.
#      Same QUANTIZED objective as A, so it is A's correctness twin, and it is the
#      nearest SQL model of a switch implemented inside the AM (which would score
#      codes, not heap vectors).
#
# CORRECTNESS, per (point, query), before any latency is believed (hard rule 8):
#   A == D as a top-10 SET (same objective), unless D has a tie across rank 10/11;
#   B == C as a set (same objective), unless B has a tie across the cut;
#   |A n B| / 10 is reported -- it is the price of the quantized objective, not a
#   bug, and it is why "the materializing arm is exact" is part of the trade.
#   POSITIVE CONTROL: A with fuse_normalize = ON computes a different objective and
#   must disagree with D somewhere; if it never does, the comparison is vacuous and
#   the run says so (AGENTS.md, eleventh member).
#
# THE POINTS.  `none` (no predicate, 100 %); a docvals facet `price < c` where price
# is a hash RANK 0..N-1 (so c = s*N admits exactly c rows, uncorrelated with docid)
# at 10 / 1 / 0.1 / 0.01 %; and a lexical gate `body @@@ term` at the same targets,
# the term chosen log-nearest in df (gatesweep.sh's method) and its selectivity
# MEASURED by a seqscan count.  Quote `sel`, not `target`.
#
# LATENCY: `EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)` Execution Time, REPS reps per
# statement with the first dropped, per-query median, then p50/p99 over the query
# set.  Every (point, arm) is measured in two slots, interleaved by query so a
# drifting machine cannot land on one arm (hard rule 10).  Buffers come from one
# EXPLAIN (ANALYZE, BUFFERS) per (point, arm) on the first query, top node.
#
# usage:
#   PREP=beir  DB=<db fuse.sh loaded with FUSE_LOAD_ONLY=1>   bench/v17_crossover.sh
#   PREP=synth DB=<new db> N=1000000 DIM=384                  bench/v17_crossover.sh
#   PREP=none  DB=<db already prepared by this script>        bench/v17_crossover.sh
# env: LATN=25 (queries) REPS=6 K=10 W='{0.5,0.5}' OUT=/tmp/out TAG=<db> MATNONE=1
#
# Copyright (c) 2025-2026, Gregory Burd

set -uo pipefail

PREP=${PREP:-none}
DB=${DB:?DB=<database>}
N=${N:-1000000}
DIM=${DIM:-384}
LATN=${LATN:-25}
REPS=${REPS:-6}
K=${K:-10}
W=${W:-'{0.5,0.5}'}
OUT=${OUT:-/tmp/out}
TAG=${TAG:-$DB}
# MATNONE=0 skips the three materializing arms at the no-predicate point ENTIRELY --
# timing and correctness check.  At 1M rows each of them scores the whole corpus per
# statement, and arm D's weave_vec_scan(k = 1M, docids = 1M) exceeded the 300 s
# statement_timeout on the first such run (2026-10-04), which is a regime no switch
# would ever choose.  A at that point is still timed and plan-checked.
MATNONE=${MATNONE:-1}

W1=$(printf '%s' "$W" | tr -d '{}' | cut -d, -f1)
W2=$(printf '%s' "$W" | tr -d '{}' | cut -d, -f2)
mkdir -p "$OUT"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
SETUP="SET jit = off; SET max_parallel_workers_per_gather = 0; SET pg_weave.fuse_normalize = off; SET statement_timeout = '300s';"

say() { printf '%s v17: %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die() { printf 'v17_crossover.sh: FAIL: %s\n' "$*" >&2; exit 1; }

createdb "$DB" 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave;" >/dev/null 2>&1
$PSQL -c "ALTER EXTENSION pg_weave UPDATE;" >/dev/null 2>&1 || true

# ------------------------------------------------------------------ preparation
#
# vx is a FRESH heap in every mode: built by INSERT in a fixed order, never UPDATEd,
# so no tuple is a HOT-chain member and D's docid = blk * 291 + off (include/weave/
# am.h weave_tid_to_docid, MaxHeapTuplesPerPage = 291 at 8 kB) is the AM's own docid.
# Heap order is the corpus order, NOT price order: a price-ordered heap would turn the
# facet into a docid range and flatter every arm.
case $PREP in
beir)
    $PSQL >/dev/null <<SQL || die "beir prep failed"
DROP TABLE IF EXISTS vx, vq, v17pr CASCADE;
CREATE TABLE v17pr AS
  SELECT id, (row_number() OVER (ORDER BY hashint8(id), id) - 1)::bigint AS price FROM fd;
CREATE TABLE vx AS
  SELECT f.id, f.body, f.emb, p.price FROM fd f JOIN v17pr p USING (id) ORDER BY f.ctid;
CREATE TABLE vq AS SELECT qid, wq, qv FROM fq ORDER BY qid LIMIT $LATN;
SQL
    ;;
synth)
    # 8 tokens per doc drawn log-uniformly from w1..w100000 (a Zipf-like df spread
    # so BM25 has real idf variation), plus four gate tokens at ~10/1/0.1/0.01 %
    # chosen by a hash INDEPENDENT of price's.  `WHERE p.id > 0` correlates each
    # sub-select so it is not hoisted into a once-only InitPlan.  The vector is
    # uniform random on the sphere; each query vector is a corpus vector plus an
    # equal-norm perturbation, so every query has genuine near neighbours.
    say "synth: building $N rows x $DIM-d (this is the slow part)"
    $PSQL >/dev/null <<SQL || die "synth prep failed"
SET jit = off;
DROP TABLE IF EXISTS vx, vq, v17pr CASCADE;
CREATE TABLE v17pr AS
  SELECT i::bigint AS id, (row_number() OVER (ORDER BY hashint8(i::bigint), i) - 1)::bigint AS price,
         ((hashint8(i::bigint * 7 + 3) & 2147483647) % 10000)::int AS h
    FROM generate_series(1, $N) i;
CREATE TABLE vx (id bigint, body wdoc, emb wvec($DIM), price bigint);
INSERT INTO vx
SELECT p.id,
       to_wdoc((SELECT string_agg('w' || floor(exp(random() * ln(100000)))::int, ' ')
                  FROM generate_series(1, 8) WHERE p.id > 0)
               || CASE WHEN p.h % 10 = 1 THEN ' gtenth' ELSE '' END
               || CASE WHEN p.h % 100 = 2 THEN ' ghundredth' ELSE '' END
               || CASE WHEN p.h % 1000 = 3 THEN ' gthousandth' ELSE '' END
               || CASE WHEN p.h = 4 THEN ' gtenthousandth' ELSE '' END),
       wvec_l2_normalize(ARRAY(SELECT (random() - 0.5)::real
                                 FROM generate_series(1, $DIM) WHERE p.id > 0)::wvec),
       p.price
  FROM v17pr p ORDER BY p.id;
CREATE TABLE vq AS
  SELECT q AS qid,
         (SELECT string_agg(t, ' | ') FROM
            (SELECT DISTINCT 'w' || floor(exp(ln(10) + random() * ln(1000)))::int AS t
               FROM generate_series(1, 3) WHERE q > 0) s) AS wq,
         wvec_l2_normalize(vx.emb + wvec_l2_normalize(ARRAY(SELECT (random() - 0.5)::real
                                                FROM generate_series(1, $DIM) WHERE q > 0)::wvec)) AS qv
    FROM generate_series(1, $LATN) q
    JOIN vx ON vx.id = 1 + ((hashint8(q::bigint * 31) & 2147483647) % $N);
SQL
    ;;
none) ;;
*) die "PREP must be beir, synth or none" ;;
esac

if [ "$PREP" != none ]; then
    say "building vx_w (body, emb, price int8_docval_ops), metric ip"
    T0=$(date +%s)
    $PSQL -c "SET maintenance_work_mem = '2GB'; CREATE INDEX vx_w ON vx USING weave (body, emb, price int8_docval_ops) WITH (metric = ip);" >/dev/null \
        || die "index build failed"
    $PSQL -c "ANALYZE vx; ANALYZE vq;" >/dev/null
    say "index built in $(( $(date +%s) - T0 )) s"
fi

NROWS=$($PSQL -t -A -c "SELECT count(*) FROM vx")
[ "${NROWS:-0}" -gt 0 ] || die "vx is empty"
NQ=$($PSQL -t -A -c "SELECT count(*) FROM vq WHERE wq IS NOT NULL AND wq <> ''")
NSEG=$($PSQL -t -A -c "SELECT weave_index_nsegments('vx_w')")
IDXMB=$($PSQL -t -A -c "SELECT round(pg_relation_size('vx_w') / 1048576.0, 1)")
HEAPMB=$($PSQL -t -A -c "SELECT round(pg_table_size('vx') / 1048576.0, 1)")
VER=$($PSQL -t -A -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_weave'")
# The docid identity D relies on, checked rather than assumed.
DOCOK=$($PSQL -t -A -c "SELECT count(*) FROM weave_vec_lanes('vx_w') l JOIN vx ON l.docid = ((vx.ctid::text::point)[0] * 291 + (vx.ctid::text::point)[1])::bigint")
[ "$DOCOK" = "$NROWS" ] || die "docid formula covers $DOCOK of $NROWS lanes -- arm D would score the wrong rows"
# How correlated the facet is with heap order (0 = scattered, which is the case meant).
PCORR=$($PSQL -t -A -c "SELECT round(corr(price, (ctid::text::point)[0])::numeric, 4) FROM vx")
say "$DB: $NROWS rows, $NQ queries, $NSEG segments, index ${IDXMB} MB, heap ${HEAPMB} MB, pg_weave $VER, corr(price, blk) = $PCORR"
printf 'db\tnrows\tnq\tnseg\tindex_mb\theap_mb\tweavever\tcorr_price_blk\tweights\treps\n%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$TAG" "$NROWS" "$NQ" "$NSEG" "$IDXMB" "$HEAPMB" "$VER" "$PCORR" "$W" "$REPS" > "$OUT/v17-meta-$TAG.tsv"

# ------------------------------------------------------------------ points
PTS=$OUT/v17-points-$TAG.tsv
printf 'kind\ttarget\tpred\trows\tsel\n' > "$PTS"
addpt() {   # kind target pred
    local rows
    rows=$($PSQL -t -A -c "SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM vx ${3:+WHERE $3}") \
        || die "count failed for $3"
    # An empty field would vanish: tab is IFS whitespace, so `read` collapses "\t\t".
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "${3:--}" "$rows" "$(awk -v a="$rows" -v b="$NROWS" 'BEGIN{printf "%.6f", a/b}')" >> "$PTS"
}
addpt none 1 ""
for t in 0.1 0.01 0.001 0.0001; do
    c=$(awk -v t="$t" -v n="$NROWS" 'BEGIN{c=int(t*n+0.5); if (c<1) c=1; print c}')
    addpt dv "$t" "price < $c"
done
if [ "$PREP" = synth ] || $PSQL -t -A -c "SELECT count(*) FROM vx WHERE body @@@ 'gtenth'::wquery" 2>/dev/null | grep -qv '^0$'; then
    addpt lex 0.1 "body @@@ 'gtenth'::wquery"
    addpt lex 0.01 "body @@@ 'ghundredth'::wquery"
    addpt lex 0.001 "body @@@ 'gthousandth'::wquery"
    addpt lex 0.0001 "body @@@ 'gtenthousandth'::wquery"
else
    # A real corpus: candidate words with their df, from the corpus text fuse.sh left
    # in stage_c, verified through the index's own df (the analyzer is the judge).
    CAND=$(mktemp)
    $PSQL -t -A -F $'\t' >"$CAND" <<'SQL' || die "candidate term query failed"
SELECT w, count(DISTINCT docid) FROM stage_c,
       LATERAL regexp_split_to_table(lower(txt), '[^a-z0-9]+') w
 WHERE w ~ '^[a-z]{3,}$' AND w NOT IN ('and', 'or', 'not')
 GROUP BY w;
SQL
    for t in 0.1 0.01 0.001 0.0001; do
        term=$(awk -v n="$NROWS" -v tgt="$t" -F'\t' '
            { s = $2 / n; d = log(s / tgt); if (d < 0) d = -d;
              if (best == "" || d < best) { best = d; bt = $1 } }
            END { print bt }' "$CAND")
        [ -n "$term" ] || die "no lexical candidate near $t"
        addpt lex "$t" "body @@@ '$term'::wquery"
    done
    rm -f "$CAND"
fi
say "points:"; column -t -s $'\t' "$PTS" >&2

# ------------------------------------------------------------------ queries
QLIT=$(mktemp)
$PSQL -t -A -F $'\t' -c "SELECT qid, quote_literal(wq), quote_literal(qv::text), (SELECT ndocs FROM weave_index_stats('vx_w')), (SELECT avgdl FROM weave_index_stats('vx_w')), quote_literal(weave_index_df('vx_w', wq::wquery)::text) FROM vq WHERE wq IS NOT NULL AND wq <> '' ORDER BY qid" > "$QLIT" \
    || die "query fetch failed"
[ -s "$QLIT" ] || die "no queries"

# arm_sql ARM PRED WQ QV NDOCS AVGDL DFS LIMIT [scores]
# With a 9th argument the exact/quantized arms also emit their score, for the tie check.
arm_sql() {
    local arm=$1 pred=$2 wq=$3 qv=$4 nd=$5 adl=$6 dfs=$7 lim=$8 sc=${9:-}
    local where=${pred:+WHERE $pred}
    case $arm in
    A) printf "SELECT id FROM vx %s ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '%s') LIMIT %s;\n" \
           "$where" "$wq" "$qv" "$W" "$lim" ;;
    B) printf "WITH ids AS (SELECT id, ctid AS rt, emb FROM vx %s), l AS (SELECT ctid, score FROM weave_search('vx_w', %s::wquery, %s)), s AS (SELECT ids.id, %s * COALESCE(l.score, 0) + %s * (-(ids.emb <#> %s::wvec)) AS sc FROM ids LEFT JOIN l ON l.ctid = ids.rt) SELECT id%s FROM s ORDER BY sc DESC, id LIMIT %s;\n" \
           "$where" "$wq" "$NROWS" "$W1" "$W2" "$qv" "${sc:+, sc}" "$lim" ;;
    C) printf "SELECT id FROM vx %s ORDER BY %s * weave_bm25(body, %s::wquery, %s, %s, %s::float8[]) + %s * (-(emb <#> %s::wvec)) DESC, id LIMIT %s;\n" \
           "$where" "$W1" "$wq" "$nd" "$adl" "$dfs" "$W2" "$qv" "$lim" ;;
    D) printf "WITH ids AS MATERIALIZED (SELECT id, ctid AS rt, ((ctid::text::point)[0] * 291 + (ctid::text::point)[1])::bigint AS docid FROM vx %s), l AS (SELECT ctid, score FROM weave_search('vx_w', %s::wquery, %s)), v AS (SELECT docid, score FROM weave_vec_scan('vx_w', %s::wvec, greatest((SELECT count(*) FROM ids), 1)::int, (SELECT array_agg(docid) FROM ids))), s AS (SELECT ids.id, %s * COALESCE(l.score, 0) + %s * COALESCE(v.score::float8, 0) AS sc FROM ids LEFT JOIN l ON l.ctid = ids.rt LEFT JOIN v ON v.docid = ids.docid) SELECT id%s FROM s ORDER BY sc DESC, id LIMIT %s;\n" \
           "$where" "$wq" "$NROWS" "$qv" "$W1" "$W2" "${sc:+, sc}" "$lim" ;;
    esac
}
# GUCs per arm.  A is forced onto the fused index path; B/C/D may use any index path
# for the predicate (a bitmap is the natural way to materialize a set) but not a
# seqscan when there IS a predicate, or the gated points would measure a heap scan.
arm_gucs() {
    if [ "$1" = A ]; then echo "SET enable_seqscan = off; SET enable_bitmapscan = off;"
    elif [ -n "$2" ]; then echo "SET enable_seqscan = off; SET enable_bitmapscan = on;"
    else echo "SET enable_seqscan = on; SET enable_bitmapscan = on;"; fi
}
ARMS="A B C D"

# ------------------------------------------------------------------ plan check
PLANS=$OUT/v17-plans-$TAG.txt
: > "$PLANS"
IFS=$'\t' read -r _ wq1 qv1 nd1 adl1 dfs1 < <(head -1 "$QLIT")
while IFS=$'\t' read -r kind tgt pred rows sel; do
    [ "$kind" = kind ] && continue
    [ "$pred" = - ] && pred=
    for arm in $ARMS; do
        plan=$( { echo "$SETUP $(arm_gucs "$arm" "$pred")"; echo "EXPLAIN (COSTS OFF)"; arm_sql "$arm" "$pred" "$wq1" "$qv1" "$nd1" "$adl1" "$dfs1" "$K"; } \
                | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A) || die "$kind/$tgt/$arm: EXPLAIN failed"
        printf '=== %s %s %s (%s)\n%s\n' "$kind" "$tgt" "$arm" "$pred" "$plan" >> "$PLANS"
        if [ "$arm" = A ]; then
            printf '%s' "$plan" | grep -q 'Order By' || die "$kind/$tgt: A's fuse() did not reach the index: $plan"
            if [ -n "$pred" ]; then
                printf '%s' "$plan" | grep -q 'Index Cond' || die "$kind/$tgt: A's predicate is not an Index Cond: $plan"
            fi
        elif [ -n "$pred" ]; then
            printf '%s' "$plan" | grep -q -E 'Index Cond|Recheck Cond' || die "$kind/$tgt/$arm: predicate did not reach the index: $plan"
        fi
    done
done < "$PTS"
say "plan check passed (A: Order By + Index Cond; B/C/D: indexed predicate)"

# ------------------------------------------------------------------ correctness
CHK=$OUT/v17-check-$TAG.tsv
printf 'kind\ttarget\tqid\tA_eq_D\tD_tie\tB_eq_C\tB_tie\tAB_overlap\tAnorm_eq_D\n' > "$CHK"
ids_of()   { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' '; }
# top-11 "id|score" -> "<tie?> <top-10 ids sorted>"
tie_ids()  { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F '|' \
             | awk -F'|' '{id[NR]=$1; s[NR]=$2} END { t = (NR >= 11 && s[10] == s[11]) ? 1 : 0;
                          printf "%d", t; for (i = 1; i <= NR && i <= 10; i++) printf " %s", id[i]; print "" }'; }
sorted()   { tr ' ' '\n' | grep -v '^$' | sort -n | tr '\n' ' '; }
while IFS=$'\t' read -r kind tgt pred rows sel; do
    [ "$kind" = kind ] && continue
    [ "$pred" = - ] && pred=
    [ "$kind" = none ] && [ "$MATNONE" = 0 ] && continue
    while IFS=$'\t' read -r qid wq qv nd adl dfs; do
        a=$( { echo "$SETUP $(arm_gucs A "$pred")"; arm_sql A "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" "$K"; } | ids_of) || die "check A failed"
        an=$( { echo "$SETUP $(arm_gucs A "$pred") SET pg_weave.fuse_normalize = on;"; arm_sql A "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" "$K"; } | ids_of) || die "check Anorm failed"
        c=$( { echo "$SETUP $(arm_gucs C "$pred")"; arm_sql C "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" "$K"; } | ids_of) || die "check C failed"
        read -r btie bids < <( { echo "$SETUP $(arm_gucs B "$pred")"; arm_sql B "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" 11 sc; } | tie_ids) || die "check B failed"
        read -r dtie dids < <( { echo "$SETUP $(arm_gucs D "$pred")"; arm_sql D "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" 11 sc; } | tie_ids) || die "check D failed"
        # An arm that errored (a timeout, say) leaves an EMPTY id list, and `read < <(...)`
        # does not propagate the failure -- so refuse an empty arm rather than record it
        # as a disagreement.  Every point here admits at least one row.
        [ -n "$a" ] && [ -n "$c" ] && [ -n "${bids// /}" ] && [ -n "${dids// /}" ] \
            || die "$kind/$tgt qid=$qid: an arm returned no rows (A='$a' C='$c' B='$bids' D='$dids')"
        b=$(echo "$bids" | sorted); d=$(echo "$dids" | sorted)
        ov=$(comm -12 <(echo "$a" | tr ' ' '\n' | grep -v '^$' | sort) <(echo "$b" | tr ' ' '\n' | grep -v '^$' | sort) | wc -l)
        na=$(echo "$a" | wc -w)
        [ "$a" = "$d" ] && ad=1 || ad=0
        [ "$b" = "$c" ] && bc=1 || bc=0
        [ "$an" = "$d" ] && nd_=1 || nd_=0
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s/%s\t%s\n' "$kind" "$tgt" "$qid" "$ad" "$dtie" "$bc" "$btie" "$ov" "$na" "$nd_" >> "$CHK"
        if [ "$ad" = 0 ] && [ "$dtie" = 0 ]; then
            printf 'DISAGREE A vs D %s %s qid=%s\n  A: %s\n  D: %s\n' "$kind" "$tgt" "$qid" "$a" "$d" >> "$OUT/v17-disagree-$TAG.txt"
        fi
        if [ "$bc" = 0 ] && [ "$btie" = 0 ]; then
            printf 'DISAGREE B vs C %s %s qid=%s\n  B: %s\n  C: %s\n' "$kind" "$tgt" "$qid" "$b" "$c" >> "$OUT/v17-disagree-$TAG.txt"
        fi
    done < "$QLIT"
done < "$PTS"
awk -F'\t' 'NR > 1 { n++; if (!$4 && !$5) ad++; if (!$6 && !$7) bc++; if ($5) dt++; if ($7) bt++;
                     if (!$9) ctl++; split($8, o, "/"); ov += o[1]; den += o[2] }
            END { printf "CHECK %d (point,query) pairs: A!=D untied %d (D ties %d), B!=C untied %d (B ties %d), mean |A n B|/|A| %.4f, positive control (normalize=on vs D) disagreed %d\n",
                  n, ad, dt, bc, bt, (den ? ov / den : 0), ctl }' "$CHK" | tee "$OUT/v17-checksum-$TAG.txt" >&2
CTLN=$(awk -F'\t' 'NR > 1 && !$9' "$CHK" | wc -l)
[ "$CTLN" -gt 0 ] || say "WARNING: the positive control never fired -- A==D agreement is uninformative on this corpus"

# ------------------------------------------------------------------ latency
pctl() {
    sort -g "$1" | awk '{v[NR]=$1}
        END { if (NR==0) { printf "0\t0"; exit }
              p=int(NR*0.99); if (p<1) p=NR;
              printf "%.3f\t%.3f", v[int((NR+1)/2)], v[p] }'
}
RAW=$OUT/v17-raw-$TAG.tsv
printf 'kind\ttarget\tarm\tslot\tqid\tmedian_ms\tnreps\n' > "$RAW"
say "latency: $(($(wc -l < "$PTS") - 1)) points x 4 arms x $NQ queries x $REPS reps x 2 slots"
while IFS=$'\t' read -r qid wq qv nd adl dfs; do
    for slot in 1 2; do
        while IFS=$'\t' read -r kind tgt pred rows sel; do
            [ "$kind" = kind ] && continue
    [ "$pred" = - ] && pred=
            for arm in $ARMS; do
                [ "$kind" = none ] && [ "$arm" != A ] && [ "$MATNONE" = 0 ] && continue
                med=$( { echo "$SETUP $(arm_gucs "$arm" "$pred")"
                         for _ in $(seq 1 "$REPS"); do
                             echo "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)"
                             arm_sql "$arm" "$pred" "$wq" "$qv" "$nd" "$adl" "$dfs" "$K"
                         done; } \
                       | psql -X -q -d "$DB" -t -A 2>>"$OUT/v17-errors-$TAG.txt" \
                       | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' | tail -n +2 \
                       | sort -g | awk '{v[NR]=$1} END { if (NR) printf "%s\t%d", v[int((NR+1)/2)], NR }')
                [ -n "$med" ] || die "$kind/$tgt/$arm qid=$qid slot=$slot: no timings (see v17-errors-$TAG.txt)"
                printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$tgt" "$arm" "$slot" "$qid" "$med" >> "$RAW"
            done
        done < "$PTS"
    done
    say "latency: qid $qid done"
done < "$QLIT"

LAT=$OUT/v17-lat-$TAG.tsv
printf 'db\tkind\ttarget\trows\tsel\tarm\tslot\tp50_ms\tp99_ms\tqueries\n' > "$LAT"
while IFS=$'\t' read -r kind tgt pred rows sel; do
    [ "$kind" = kind ] && continue
    [ "$pred" = - ] && pred=
    for arm in $ARMS; do
        for slot in 1 2; do
            f=$(mktemp)
            awk -F'\t' -v k="$kind" -v t="$tgt" -v a="$arm" -v s="$slot" '$1==k && $2==t && $3==a && $4==s {print $6}' "$RAW" > "$f"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$TAG" "$kind" "$tgt" "$rows" "$sel" "$arm" "$slot" "$(pctl "$f")" "$(wc -l < "$f")" >> "$LAT"
            rm -f "$f"
        done
    done
done < "$PTS"

# ------------------------------------------------------------------ buffers
BUF=$OUT/v17-buf-$TAG.tsv
printf 'kind\ttarget\tarm\tshared_hit\tshared_read\texec_ms\n' > "$BUF"
while IFS=$'\t' read -r kind tgt pred rows sel; do
    [ "$kind" = kind ] && continue
    [ "$pred" = - ] && pred=
    for arm in $ARMS; do
        [ "$kind" = none ] && [ "$arm" != A ] && [ "$MATNONE" = 0 ] && continue
        ex=$( { echo "$SETUP $(arm_gucs "$arm" "$pred")"; echo "EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)"; arm_sql "$arm" "$pred" "$wq1" "$qv1" "$nd1" "$adl1" "$dfs1" "$K"; } \
              | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A) || die "buffers EXPLAIN failed"
        printf '=== BUFFERS %s %s %s\n%s\n' "$kind" "$tgt" "$arm" "$ex" >> "$PLANS"
        line=$(printf '%s\n' "$ex" | grep -m1 'Buffers: shared')
        hit=$(printf '%s' "$line" | sed -n 's/.*hit=\([0-9]*\).*/\1/p')
        rd=$(printf '%s' "$line" | sed -n 's/.*read=\([0-9]*\).*/\1/p')
        ms=$(printf '%s\n' "$ex" | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p')
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$tgt" "$arm" "${hit:-0}" "${rd:-0}" "$ms" >> "$BUF"
    done
done < "$PTS"

say "summary (p50 slot1/slot2 ms):"
awk -F'\t' 'NR > 1 { key = $2 "\t" $3 "\t" $5; p[key "\t" $6 "\t" $7] = $8; keys[key] = 1 }
    END { printf "kind\ttarget\tsel\tA\tB\tC\tD\n";
          for (k in keys) { printf "%s", k;
              n = split("A B C D", arms, " ");
              for (i = 1; i <= n; i++) printf "\t%s/%s", p[k "\t" arms[i] "\t1"], p[k "\t" arms[i] "\t2"];
              print "" } }' "$LAT" | sort -t$'\t' -k1,1 -k3,3gr | column -t -s $'\t' | tee "$OUT/v17-summary-$TAG.txt" >&2
rm -f "$QLIT"
echo "V17_CROSSOVER_DONE db=$TAG rows=$NROWS points=$(($(wc -l < "$PTS") - 1)) checks=$(($(wc -l < "$CHK") - 1))"
