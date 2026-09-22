#!/usr/bin/env bash
#
# bench/fuse.sh -- doc/specs/FUSED_TOPK.md section 8: fused top-k against
# RRF-with-over-fetch, on a public dataset with relevance judgments.
#
# WHAT THIS IS THE FIRST MEASUREMENT OF.  Every other file in bench/ measures
# latency or size on queries whose answers are gated for equality.  This one
# measures RANKING QUALITY against human judgments, which no benchmark in this
# project or in either sibling project has ever done -- `pg_fts/bench/ndcg.py`
# existed and was never once run because the corpus had no qrels.  Section 8's
# gate is four rows and this produces all four.
#
# THE CONTROL IS THE SAME INDEX, QUERIED TWICE, and that is deliberate and is a
# STRONGER control than the two-index RRF stack section 8 describes.  Both arms read
# one weave index over (body, emb): the fused arm asks for one top-10 through
# fuse(), the control arm asks for a lexical top-100 and a vector top-100 and merges
# them with Reciprocal Rank Fusion.  Nothing differs but the scorer -- same BM25,
# same quantized vectors, same buffer cache, same code -- so a win cannot come from
# storage layout, and the storage cost of a real two-index stack (which the control
# does NOT pay here) would only widen it.  Any margin measured is a LOWER BOUND on
# the margin against a genuine two-index deployment.
#
# THREE HARNESS CONSTRAINTS THAT ARE PROPERTIES OF THE PRODUCT, not of this script,
# and all three must be reported next to the numbers or the numbers mislead:
#
#   1. A FUSED QUERY'S wquery MUST BE A LITERAL.  src/am/fusepath.c refuses a
#      parameterized wquery at plan time, because a lexical key becomes one shuttle
#      per query term and the term count is unknowable until the scan runs -- and an
#      access method that advertises a path then refuses it at run time is G39.  So
#      the correlated `LATERAL` shape (queries in a table, one plan for all of them)
#      CANNOT get the pushdown, and this script generates one statement per query
#      with the query inlined.  That is also the honest shape: it is what a user who
#      wants the fused path has to write today.
#   2. THE CHANNEL CAP IS 64 AND OR OPERATORS COUNT AGAINST IT.  fusepath.c estimates
#      the shuttle count from the query's RPN ITEM count, which includes operators, so
#      an n-term disjunction costs 2n-1 of the budget.  Query terms are therefore
#      capped (MAXTERMS below) and the number of queries that hit the cap is counted
#      and printed.  BOTH arms see the identical truncated query, so the comparison is
#      unaffected; the absolute quality numbers are affected and must say so.
#   3. MULTI-TERM wquery IS CONJUNCTIVE BY DEFAULT.  'a b' is a AND b, which for a
#      natural-language query returns almost nothing, so terms are joined with the
#      OR operator '|'.  A harness that skipped this would have measured a lexical
#      arm that returned no rows and blamed the ranker.
#
# Methodology, per .agent/skills/weave-bench and AGENTS.md hard rules 8/10/11:
#   - CORRECTNESS BEFORE LATENCY.  The fused pushdown's answer is compared against
#     the same fuse() expression evaluated by a sequential scan, per query, as a set.
#     A mismatch aborts the run rather than producing a fast wrong number.
#   - the PLAN of every measured shape is captured and asserted, because a latency
#     that looks flat is usually the planner having ignored the index
#   - warm, reps per query, first dropped, p50 AND p99
#   - A/B ALTERNATED per query rather than all-A-then-all-B, so thermal or
#     neighbour drift cannot land on one arm
#   - parallelism OFF for both arms: the work counters are PARALLEL RESTRICTED
#     backend-local globals, so a parallel worker's counts would be invisible and the
#     ratio would be silently wrong. Neither AM is parallel-capable anyway.
#
# Usage: bench/fuse.sh <workdir> <dataset> [reps]
#   <workdir>/<dataset>/{corpus.tsv,queries.tsv,qrels.tsv,manifest.json} as written
#   by bench/prepdata.py.
#
set -euo pipefail

WORK=${1:?usage: bench/fuse.sh <workdir> <dataset> [reps]}
DS=${2:?usage: bench/fuse.sh <workdir> <dataset> [reps]}
REPS=${3:-7}
DB=${PGDATABASE:-weavefuse}
DIR="$WORK/$DS"
OUT=${FUSE_OUT:-$DIR}

# The over-fetch depth of the control arm.  Section 8 names k'=100.
KP=${KP:-100}
# RRF's rank constant.  60 is Cormack et al. 2009's value and the one every
# implementation uses; it is NOT tuned here, because tuning the control's
# hyperparameter and not the fused arm's weights would bias the comparison.
RRFK=${RRFK:-60}
# Depth the quality pass retrieves.  10 for nDCG@10 and MRR@10, 100 for recall@100,
# so one pass at 100 serves all three.
QDEPTH=${QDEPTH:-100}
# See constraint 2 above.  20 distinct terms is 39 RPN items plus one vector channel,
# comfortably inside the cap; 33 terms would exceed it and be refused.
MAXTERMS=${MAXTERMS:-20}
# Queries used for the LATENCY pass.  Quality uses every query (it is deterministic
# and needs one execution each); latency needs repetitions, so it samples.
LATN=${LATN:-50}
# Queries used for the correctness gate.  Each one costs a sequential scan with a
# per-row fuse() evaluation, which is the most expensive thing here.
CHECKN=${CHECKN:-10}

PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

for f in corpus.tsv queries.tsv qrels.tsv manifest.json; do
    [ -s "$DIR/$f" ] || die "missing or empty $DIR/$f -- run bench/prepdata.py first"
done

# The embedding mode is load-bearing for every quality number, so it is read from
# the manifest and REFUSED if it is the stdlib fake.  bench/prepdata.py's hash mode
# exists to smoke-test the plumbing with no third-party packages; a quality figure
# from it is noise, and the way that mistake reaches a RESULTS file is exactly this
# script running happily and printing a number.
EMBED=$(sed -n 's/.*"embed_mode"[[:space:]]*:[[:space:]]*"\([a-z]*\)".*/\1/p' "$DIR/manifest.json" | head -1)
DIM=$(sed -n 's/.*"dim"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' "$DIR/manifest.json" | head -1)
[ -n "$DIM" ] || die "manifest.json has no dim"
if [ "$EMBED" = "hash" ]; then
    if [ "${FUSE_ALLOW_HASH:-0}" != "1" ]; then
        die "manifest says embed_mode=hash: quality numbers would be meaningless.
     Re-run prepdata.py with --embed minilm, or set FUSE_ALLOW_HASH=1 to smoke-test
     the plumbing only -- in which case the output is labelled and must not be
     recorded as a result."
    fi
    say "SMOKE MODE: embed_mode=hash, every quality number below is MEANINGLESS"
fi

say "$DS: loading (embed=$EMBED dim=$DIM)"
createdb "$DB" 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave;" >/dev/null
$PSQL -c "ALTER EXTENSION pg_weave UPDATE;" >/dev/null 2>&1 || true
WEAVEVER=$($PSQL -t -A -c "SELECT extversion FROM pg_extension WHERE extname='pg_weave';")

# ---------------------------------------------------------------------------
# Load.  Staging tables of text, then one typed table, because COPY cannot parse a
# wvec literal into a typed column without the cast being spelled out.
# ---------------------------------------------------------------------------
$PSQL <<SQL
DROP TABLE IF EXISTS fd, fq, fqrel, stage_c, stage_q CASCADE;
CREATE TABLE stage_c (docid bigint, txt text, vec text);
CREATE TABLE stage_q (qid bigint, txt text, vec text);
CREATE TABLE fqrel (qid bigint, docid bigint, rel int);
SQL
$PSQL -c "\\copy stage_c FROM '$DIR/corpus.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"
$PSQL -c "\\copy stage_q FROM '$DIR/queries.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"
$PSQL -c "\\copy fqrel FROM '$DIR/qrels.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"

# metric='ip' with L2-normalized vectors IS cosine similarity, and it is the only way
# to get cosine here: weave_index_vec_metric() raises on metric='cosine' (see
# VECTOR_CHANNEL.md section 8b), and after F8 a metric mismatch is refused at PLAN
# time. prepdata.py normalizes; if it had not, ip would silently rank by an
# unnormalized dot product and the vector arm would be measuring something else.
$PSQL <<SQL
CREATE TABLE fd (id bigint PRIMARY KEY, body wdoc, emb wvec($DIM));
INSERT INTO fd SELECT docid, to_wdoc(txt), vec::wvec FROM stage_c;

CREATE TABLE fq (qid bigint PRIMARY KEY, txt text, qv wvec($DIM), wq text, nterm int);
INSERT INTO fq SELECT qid, txt, vec::wvec, NULL, NULL FROM stage_q;
SQL

# ---------------------------------------------------------------------------
# Turn each natural-language query into a wquery, in SQL so the rule is one
# expression and both arms cannot diverge.
#
# lower, split on non-alphanumeric, drop length<=2, drop the three words the parser
# treats as OPERATORS when they stand alone ('and', 'or', 'not' -- src/query/parse.c
# line 330), de-duplicate, cap at MAXTERMS, join with ' | '.
#
# DE-DUPLICATION IS NOT COSMETIC: a repeated query term would become a SECOND
# lexical shuttle on the same term, which double-weights it and spends two of the 64
# channels on one word.
#
# The cap keeps the FIRST MAXTERMS distinct terms in order of appearance rather than
# the rarest, because selecting by document frequency would need a df lookup per
# term and would make the query set depend on the corpus -- so two datasets could
# not be compared, and neither could two runs after an ingest.  Order of appearance
# is stable, cheap, and identical for both arms.
# ---------------------------------------------------------------------------
$PSQL <<SQL
WITH t AS (
    SELECT q.qid, s.tok, min(s.ord) AS ord
      FROM fq q,
           LATERAL unnest(regexp_split_to_array(lower(q.txt), '[^a-z0-9]+'))
                   WITH ORDINALITY AS s(tok, ord)
     WHERE length(s.tok) > 2
       AND s.tok NOT IN ('and', 'or', 'not')
     GROUP BY q.qid, s.tok
), r AS (
    SELECT qid, tok, row_number() OVER (PARTITION BY qid ORDER BY ord) AS rn
      FROM t
)
UPDATE fq SET wq = a.wq, nterm = a.n
  FROM (SELECT qid, string_agg(tok, ' | ' ORDER BY rn) AS wq, count(*) AS n
          FROM r WHERE rn <= $MAXTERMS GROUP BY qid) a
 WHERE fq.qid = a.qid;
SQL

# A query whose every token was dropped has no lexical channel and cannot be fused.
# Deleted rather than silently scored as zero, and COUNTED, because a nonzero count
# here changes what the averages are over.
NOWQ=$($PSQL -t -A -c "SELECT count(*) FROM fq WHERE wq IS NULL OR wq = '';")
$PSQL -c "DELETE FROM fq WHERE wq IS NULL OR wq = '';" >/dev/null
TRUNC=$($PSQL -t -A -c "SELECT count(*) FROM fq WHERE nterm >= $MAXTERMS;")

NDOCS=$($PSQL -t -A -c "SELECT count(*) FROM fd;")
NQ=$($PSQL -t -A -c "SELECT count(*) FROM fq;")

say "$DS: building the index over (body, emb)"
BUILD_START=$(date +%s.%N)
$PSQL -c "CREATE INDEX fd_weave ON fd USING weave (body, emb) WITH (metric = 'ip');" >/dev/null
BUILD_END=$(date +%s.%N)
BUILD_S=$(awk -v a="$BUILD_START" -v b="$BUILD_END" 'BEGIN{printf "%.1f", b-a}')
$PSQL -c "ANALYZE fd;" >/dev/null
IDXMB=$($PSQL -t -A -c "SELECT round(pg_relation_size('fd_weave')/1048576.0, 1);")

# Both arms, both passes, no parallelism: see the header.
SETUP="SET max_parallel_workers_per_gather = 0;"

# ---------------------------------------------------------------------------
# The two arms, as SQL text generators.  One statement per query, query inlined.
# ---------------------------------------------------------------------------
fused_sql() {                   # fused_sql <wq> <vec> <limit>
    printf "SELECT id FROM fd ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '{0.5,0.5}') LIMIT %s;" \
           "$1" "$2" "$3"
}

# RRF: two independent over-fetches merged by reciprocal rank.
#
# row_number() is taken OVER (ORDER BY the same distance) inside each branch rather
# than OVER () over an ordered subquery, because a bare OVER () window has no defined
# input order in the standard and would make the control's ranks depend on the plan.
# That is the sort of thing that silently hands the comparison to whichever arm the
# harness got right.
rrf_sql() {                     # rrf_sql <wq> <vec> <limit>
    printf "WITH l AS (SELECT id, row_number() OVER (ORDER BY body <=> %s::wquery) AS r FROM fd ORDER BY body <=> %s::wquery LIMIT %s), v AS (SELECT id, row_number() OVER (ORDER BY emb <#> %s::wvec) AS r FROM fd ORDER BY emb <#> %s::wvec LIMIT %s) SELECT id FROM (SELECT id, 1.0/(%s + r) AS s FROM l UNION ALL SELECT id, 1.0/(%s + r) AS s FROM v) u GROUP BY id ORDER BY sum(s) DESC, id LIMIT %s;" \
           "$1" "$1" "$KP" "$2" "$2" "$KP" "$RRFK" "$RRFK" "$3"
}

# psql literal quoting for a text value, done by the server so a quote or backslash
# in a query string cannot terminate the literal.
QLIT=$(mktemp); trap 'rm -f "$QLIT" "$VLIT"' EXIT
VLIT=$(mktemp)
$PSQL -t -A -F $'\t' -c "SELECT qid, quote_literal(wq), quote_literal(qv::text) FROM fq ORDER BY qid;" > "$QLIT"
[ -s "$QLIT" ] || die "no queries survived preparation"

# ---------------------------------------------------------------------------
# CORRECTNESS GATE, before any timing, AND NOT AGAINST THE fuse() FALLBACK.
#
# The obvious gate -- compare the pushdown against the same fuse() expression
# evaluated by a sequential scan -- is WRONG, and this harness had it wrong first.
# It disagreed on 5 of 5 queries, and neither channel was at fault:
#
#   * The bare `<=>` operator has no corpus.  weave_distance() (src/query/rank.c,
#     and it says so in its own comment) scores with df = 1 and avgdl = |D| because
#     a per-row operator call cannot see the dictionary, so the fallback is A
#     DIFFERENT RANKING FUNCTION from the index's exact BM25 -- not a wrong one.
#     FUSED_TOPK.md sect. 7a (1) records this, and sql/fuse_pushdown.sql already
#     abandoned that comparison after the naive assertion returned f.
#   * The vector side is QUANTIZED in the index and exact in the heap, so even a
#     perfect scorer ranks near-ties differently.  Measured here on one query: the
#     index and an exact sequential scan returned the SAME ten documents with two
#     adjacent pairs transposed.
#
# THE ORACLE THAT IS ACTUALLY EXACT, borrowed from sql/fuse_pushdown.sql sect. 2b:
# one SRF call per channel, both of which enter the scan machinery directly and
# therefore report the INDEX's own per-channel score -- exact BM25 from
# weave_search(), and the quantized metric-domain score from weave_vec_scan(), which
# is the number the fused scan actually sums.  k = ndocs, so it is exhaustive.  What
# is left that could move the answer is the fused scorer's pruning and summation,
# which is exactly what is under test.
#
# A TIE STRADDLING RANK 10 makes "the" top-10 ambiguous however the scorer behaves,
# and the quantized vector term makes ties likelier than in the regression fixture.
# Such queries are SKIPPED and COUNTED rather than failed, because an ambiguous
# oracle is not evidence either way.
# ---------------------------------------------------------------------------
say "$DS: docid map for the oracle"
$PSQL >/dev/null <<SQL
DROP TABLE IF EXISTS fdmap;
CREATE TABLE fdmap AS
  SELECT t.id, t.rowtid, l.docid
    FROM (SELECT id, ctid AS rowtid, row_number() OVER (ORDER BY ctid) AS rn
            FROM fd) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('fd_weave')) l USING (rn);
SQL
MAPOK=$($PSQL -t -A -c "SELECT count(*) = (SELECT count(*) FROM fd) FROM fdmap;")
[ "$MAPOK" = "t" ] || die "the docid map does not cover every row: weave_vec_lanes() and ctid order disagree"

say "$DS: correctness gate on $CHECKN queries (pushdown vs exhaustive per-channel oracle)"
BAD=0
TIED=0
FBDIFF=0
while IFS=$'\t' read -r qid wq qv; do
    got=$( { echo "$SETUP SET enable_seqscan = off;"; fused_sql "$wq" "$qv" 10; } \
           | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' ')

    # The fallback, kept as a DIAGNOSTIC rather than a gate: the size of the
    # disagreement is sect. 7a (1) measured at corpus scale, which no test has done.
    fb=$( { echo "$SETUP SET enable_indexscan = off; SET enable_bitmapscan = off;"; fused_sql "$wq" "$qv" 10; } \
          | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' ')
    [ "$got" = "$fb" ] || FBDIFF=$((FBDIFF + 1))

    read -r tiefree want < <(psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' <<SQL
WITH s AS (
    SELECT m.id,
           0.5::float8 * COALESCE(a.score, 0::float8)
           + 0.5::float8 * COALESCE(v.score::float8, 0::float8) AS score
      FROM fdmap m
      LEFT JOIN weave_search('fd_weave', $wq::wquery, $NDOCS) a ON a.ctid = m.rowtid
      LEFT JOIN weave_vec_scan('fd_weave', $qv::wvec, $NDOCS) v ON v.docid = m.docid
)
SELECT (SELECT count(*) = count(DISTINCT score) FROM s),
       (SELECT string_agg(id::text, ' ' ORDER BY id)
          FROM (SELECT id FROM s ORDER BY score DESC LIMIT 10) k);
SQL
)
    if [ "$tiefree" != "t" ]; then
        TIED=$((TIED + 1))
        continue
    fi
    if [ "$(echo $got)" != "$(echo $want)" ]; then
        printf 'MISMATCH qid=%s\n  pushdown: %s\n  oracle  : %s\n' "$qid" "$got" "$want" >&2
        BAD=$((BAD + 1))
    fi
done < <(head -n "$CHECKN" "$QLIT")
[ "$BAD" -eq 0 ] || die "$BAD of $CHECKN queries disagree with the exhaustive per-channel oracle"
say "$DS: gate passed ($TIED of $CHECKN skipped for a tied oracle; the fuse() fallback differed on $FBDIFF)"

# The plan is asserted, not hoped for: if the pushdown was not chosen, every latency
# number below is measuring the fallback and the comparison is meaningless.
# IFS=$'\t' IS LOAD-BEARING, exactly as in the gate loop above: a query literal
# contains SPACES (multi-term queries are OR-joined, `a | b | c`), so a default-IFS
# read splits one row into the wrong three fields and the EXPLAIN below is then
# handed a malformed statement.  It fails as "the fused arm is NOT using the index",
# which reads as a planner regression and is not one -- the statement never parsed.
IFS=$'\t' read -r qid1 wq1 qv1 < <(head -1 "$QLIT")
PLAN_FUSED=$( { echo "$SETUP"; printf 'EXPLAIN %s' "$(fused_sql "$wq1" "$qv1" 10)"; } \
              | psql -X -q -d "$DB" -t -A | tr '\n' ' ')
case "$PLAN_FUSED" in
    *"Index Scan using fd_weave"*) : ;;
    *) die "the fused arm is NOT using the index -- plan was: $PLAN_FUSED" ;;
esac
PLAN_RRF=$( { echo "$SETUP"; printf 'EXPLAIN %s' "$(rrf_sql "$wq1" "$qv1" 10)"; } \
            | psql -X -q -d "$DB" -t -A | tr '\n' ' ')
case "$PLAN_RRF" in
    *"Index Scan using fd_weave"*) : ;;
    *) die "the RRF arm is NOT using the index -- plan was: $PLAN_RRF" ;;
esac

# ---------------------------------------------------------------------------
# QUALITY PASS.  Every query, once, at depth QDEPTH, ranks taken from psql's output
# ORDER -- which is the row order of a single ORDER BY query and is therefore the
# ranking itself, with no window function to get wrong.
# ---------------------------------------------------------------------------
run_quality() {                 # run_quality <arm>
    local arm=$1 gen=$2 out="$OUT/run-$DS-$1.tsv"
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP"
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

say "$DS: quality pass, fused arm"
RUN_FUSED=$(run_quality fused fused_sql)
say "$DS: quality pass, RRF control arm"
RUN_RRF=$(run_quality rrf rrf_sql)

# ---------------------------------------------------------------------------
# WORK COUNTERS.  One arm's whole quality pass, bracketed by resets.
#
# Read AFTER the quality pass rather than around a single query, because a per-query
# read would need a round trip per query and the totals are what the ratio is over.
# The counters are backend-local, so each arm gets its own psql session.
# ---------------------------------------------------------------------------
work_of() {                     # work_of <gen> -- prints one TSV row
    local gen=$1
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP"
        echo "SELECT weave_fuse_stats_reset(); SELECT weave_work_stats_reset();"
        while IFS=$'\t' read -r qid wq qv; do
            $gen "$wq" "$qv" 10
            printf '\n'
        done < "$QLIT"
        # scores minus the vector adapters' share minus the gates' share IS the
        # lexical contribution count, which is the unit the control's lex_contribs
        # is in.  Derived here rather than assumed, per the 0.19.0 migration note.
        echo "SELECT f.scores - f.vec_scores - f.gate_scores, w.lex_contribs, w.vec_lanes, w.vec_blocks, w.vec_blocks_bound_skipped, f.scores, f.pivots, f.blkskip, f.rqskip, f.passes, f.runs FROM weave_fuse_stats() f, weave_work_stats() w;"
    } > "$sqlf"
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' -f "$sqlf" | tail -1
    rm -f "$sqlf"
}

say "$DS: work counters, fused arm"
W_FUSED=$(work_of fused_sql)
say "$DS: work counters, RRF control arm"
W_RRF=$(work_of rrf_sql)

# ---------------------------------------------------------------------------
# LATENCY.  A/B ALTERNATED per query: fused, rrf, fused, rrf ... so drift over the
# pass cannot land on one arm (weave-bench, and the AWS skill's A/B rule).
#
# AND AN A/A LEG, which is AGENTS.md hard rule 10 and is not optional: the fused
# arm is measured TWICE per query, in the first and third slots, and the gap
# between those two measurements is the WITHIN-ARM SPREAD.  A between-arm delta
# smaller than that spread is not a result.  The sibling project's retracted
# 8.5 % "win" was a single baseline outlier, and the only thing that would have
# caught it is this leg.
#
# The second fused measurement sits AFTER the rrf one deliberately, rather than
# back-to-back with the first.  Back-to-back would measure the best case -- same
# cache, same everything -- and understate the spread.  Third slot means it
# carries whatever the rrf run did to the cache and to the clock, which is the
# conservative estimate and the one that can actually bound a claim.
# ---------------------------------------------------------------------------
say "$DS: latency, $LATN queries x $REPS reps, arms alternated, with an A/A leg"
LAT_F=$(mktemp); LAT_R=$(mktemp); LAT_F2=$(mktemp)
while IFS=$'\t' read -r qid wq qv; do
    for leg in "fused_sql:$LAT_F" "rrf_sql:$LAT_R" "fused_sql:$LAT_F2"; do
        gen=${leg%%:*}
        dest=${leg#*:}
        {
            echo "$SETUP"
            for _ in $(seq 1 "$REPS"); do
                printf 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) %s\n' "$($gen "$wq" "$qv" 10)"
            done
        } | psql -X -q -d "$DB" -t -A \
          | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' \
          | tail -n +2 \
          >> "$dest"
    done
done < <(head -n "$LATN" "$QLIT")

pctl() {                        # pctl <file> -- p50 TAB p99
    sort -g "$1" | awk '{v[NR]=$1}
        END { if (NR==0) { printf "0\t0"; exit }
              p=int(NR*0.99); if (p<1) p=NR;
              printf "%.3f\t%.3f", v[int((NR+1)/2)], v[p] }'
}
P_FUSED=$(pctl "$LAT_F")
P_RRF=$(pctl "$LAT_R")
P_FUSED2=$(pctl "$LAT_F2")
rm -f "$LAT_F" "$LAT_R" "$LAT_F2"

# ---------------------------------------------------------------------------
# SCORE.  bench/ndcg.py excludes queries with no positive judgment and PENALIZES
# queries missing from a run; both counts are in its output.
# ---------------------------------------------------------------------------
Q_FUSED=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_FUSED" --k 10 --label fused | tail -1)
Q_RRF=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_RRF" --k 10 --label rrf | tail -1)

# ---------------------------------------------------------------------------
# Report.  TSV on stdout, one table per concern, for bench/aws/run.sh to tee.
# ---------------------------------------------------------------------------
printf '\n### fuse_setup\n'
printf 'dataset\tembed\tdim\tndocs\tnqueries\tdropped_no_terms\ttruncated_at_%s\tindex_mb\tbuild_s\tweave_version\tkprime\trrf_k\n' "$MAXTERMS"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
       "$DS" "$EMBED" "$DIM" "$NDOCS" "$NQ" "$NOWQ" "$TRUNC" "$IDXMB" "$BUILD_S" "$WEAVEVER" "$KP" "$RRFK"

printf '\n### fuse_correctness\n'
printf 'checked\tmismatched_vs_oracle\tskipped_tied_oracle\tfallback_differed\n'
printf '%s\t%s\t%s\t%s\n' "$CHECKN" "$BAD" "$TIED" "$FBDIFF"

printf '\n### fuse_quality\n'
printf 'label\tnqueries_scored\tndcg@10\trecall@100\tmrr@10\texcluded_no_positive\tmissing_from_run\n'
printf '%s\n%s\n' "$Q_FUSED" "$Q_RRF"

printf '\n### fuse_latency\n'
printf 'arm\tp50_ms\tp99_ms\tqueries\treps\n'
printf 'fused\t%s\t%s\t%s\n' "$P_FUSED" "$LATN" "$REPS"
printf 'rrf\t%s\t%s\t%s\n' "$P_RRF" "$LATN" "$REPS"
# The A/A leg (hard rule 10).  `fused_aa` is the SAME arm measured again in the
# third slot; the fused-vs-fused_aa gap is the noise floor any fused-vs-rrf claim
# has to clear.  Reported as a row rather than folded into the fused numbers so
# that nobody can average the two and lose the only estimate of spread there is.
printf 'fused_aa\t%s\t%s\t%s\n' "$P_FUSED2" "$LATN" "$REPS"

printf '\n### fuse_work\n'
printf 'arm\tlex_contribs_fused_side\tlex_contribs_wand\tvec_lanes\tvec_blocks\tvec_blocks_bound_skipped\tfuse_scores_total\tpivots\tblkskip\trqskip\tpasses\truns\n'
printf 'fused\t%s\n' "$W_FUSED"
printf 'rrf\t%s\n' "$W_RRF"

printf '\n### fuse_plans\n'
printf 'arm\tplan\n'
printf 'fused\t%s\n' "$PLAN_FUSED"
printf 'rrf\t%s\n' "$PLAN_RRF"

say "$DS: done ($NDOCS docs, $NQ queries, correctness gate passed on $CHECKN)"
