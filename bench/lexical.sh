#!/usr/bin/env bash
#
# bench/lexical.sh -- pg_weave vs tsvector+GIN on the same corpus, same host.
#
# tsvector + GIN is the baseline every PostgreSQL user already has, so beating it
# is the minimum bar for anyone to install anything.  It is also the only
# competitor guaranteed installable without third-party packages, which makes it
# the one comparison that is always reproducible.
#
# Methodology, per .agent/skills/weave-bench:
#	 - warm, median of N with the first run dropped
#	 - p50 AND p99 reported; a p50 win with a p99 loss is a regression for
#	   anyone with a latency SLO
#	 - df bands read from the corpus, not assumed
#	 - CORRECTNESS CHECKED BEFORE LATENCY.  A benchmark of a wrong fast path is
#	   worse than no benchmark; this is the mistake that produced a retracted
#	   claim in pg_turbovec.  Match counts must agree with a seq-scan reference
#	   or the run aborts.
#
# Usage: bench/lexical.sh [ndocs] [vocab] [reps]
#
set -euo pipefail

NDOCS=${1:-1000000}
VOCAB=${2:-200000}
REPS=${3:-7}
DB=${PGDATABASE:-weavebench}
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

createdb "$DB" 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave;" >/dev/null
$PSQL -c "ALTER EXTENSION pg_weave UPDATE;" >/dev/null 2>&1 || true

say "corpus: $NDOCS docs, $VOCAB vocabulary"
$PSQL -v ndocs="$NDOCS" -v vocab="$VOCAB" -f "$(dirname "$0")/corpus.sql"

RARE=$($PSQL -t -A -c "SELECT term FROM bands WHERE band='rare'")
MID=$($PSQL -t -A -c "SELECT term FROM bands WHERE band='mid'")
COMMON=$($PSQL -t -A -c "SELECT term FROM bands WHERE band='common'")
say "bands: rare=$RARE mid=$MID common=$COMMON"

# ---------------------------------------------------------------------------
# Build both indexes over the SAME stored, pre-analyzed column.
#
# This is a like-for-like requirement that is easy to get wrong: indexing an
# expression (to_wdoc(body)) makes every query re-analyze the text in the ORDER
# BY, and profiling in pg_fts showed that 40-88% of the measured latency was that
# re-analysis rather than the index scan.  Store the analyzed column once, index
# the column, and the numbers measure the index.
# ---------------------------------------------------------------------------
say "materializing analyzed columns"
$PSQL <<'SQL'
ALTER TABLE docs ADD COLUMN d wdoc;
ALTER TABLE docs ADD COLUMN tsv tsvector;
UPDATE docs SET d = to_wdoc(body), tsv = to_tsvector('simple', body);
VACUUM (ANALYZE) docs;
SQL

build() {
    local name=$1 ddl=$2
    local t0 t1
    t0=$(date +%s.%N)
    $PSQL -c "$ddl" >/dev/null
    t1=$(date +%s.%N)
    printf '%s\t%.1f\t%s\n' "$name" "$(echo "$t1-$t0" | bc)" \
        "$($PSQL -t -A -c "SELECT pg_size_pretty(pg_total_relation_size('${name}_idx'))")"
}

say "building indexes"
BUILD_WEAVE=$(build weave "CREATE INDEX weave_idx ON docs USING weave (d)")
$PSQL -c "SELECT weave_merge('weave_idx'); SELECT weave_vacuum('weave_idx');" >/dev/null 2>&1 || true
BUILD_GIN=$(build gin "CREATE INDEX gin_idx ON docs USING gin (tsv)")

say "prewarming"
$PSQL -c "SELECT count(*) FROM docs" >/dev/null
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm" >/dev/null 2>&1 || true
$PSQL -c "SELECT pg_prewarm('weave_idx'); SELECT pg_prewarm('gin_idx');" >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Correctness gate, before any timing.
# ---------------------------------------------------------------------------
say "correctness: match counts must agree with a seq-scan reference"
FAIL=0
for t in "$RARE" "$MID" "$COMMON" zzqrare; do
    ref=$($PSQL -t -A -c "SET enable_indexscan=off; SET enable_bitmapscan=off;
                          SELECT count(*) FROM docs WHERE body ~ ('\\m' || '$t' || '\\M')")
    wv=$($PSQL -t -A -c "SELECT count(*) FROM docs WHERE d @@@ '$t'::wquery")
    gn=$($PSQL -t -A -c "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$t')")
    if [ "$ref" != "$wv" ] || [ "$ref" != "$gn" ]; then
        printf '  MISMATCH %-12s seqscan=%s weave=%s gin=%s\n' "$t" "$ref" "$wv" "$gn"
        FAIL=1
    else
        printf '  ok %-12s %s rows (all three agree)\n' "$t" "$ref"
    fi
done
[ "$FAIL" = 0 ] || { echo "ABORT: correctness failed; latency numbers would be meaningless" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Latency.  Each query timed REPS times; first dropped; p50 and p99 from the
# rest.  Timing in the client would include psql startup, so use a server-side
# clock via EXPLAIN ANALYZE's execution time.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Timing.
#
# ALL reps run in ONE psql session.  The first version used one psql invocation
# per rep, which made every single measurement a first-scan-in-a-fresh-backend
# and produced a latency that was FLAT at ~87 ms across every selectivity band --
# 32 matching documents cost the same as 179,000.  That is not a posting-scan
# cost, it is per-backend setup (the relcache doclen page directory and the
# segment/dictionary open) being paid inside executor time on every rep.
#
# Both numbers matter and they answer different questions, so both are reported:
#
#	 cold  = first scan in a fresh backend.  What a non-pooled application pays
#			 on every request, and what a pooled one pays after a reconnect.
#	 warm  = steady state within a session.  What a pooled application pays.
#
# Reporting only warm hides a real cost; reporting only cold hides the actual
# steady-state performance.  Conflating them, as the first version did, produces
# a number that is wrong for both.
# ---------------------------------------------------------------------------

# cold: one fresh connection, one execution, take the median over CO independent
# connections so a single unlucky page fault does not define the number.
CO=5
coldq() {                       # coldq <sql>
    local sql=$1 i ms
    local -a t=()
    for i in $(seq 1 $CO); do
        ms=$(psql -X -q -d "$DB" -t -A \
             -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $sql" \
             | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p')
        t+=("${ms:-0}")
    done
    printf '%s\n' "${t[@]}" | sort -g | awk '{v[NR]=$1} END {printf "%.2f", v[int((NR+1)/2)]}'
}

# warm: one session, REPS executions, drop the first, report p50 and p99.
warmq() {                       # warmq <sql>
    local sql=$1 i
    {
        for i in $(seq 1 "$REPS"); do
            printf 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) %s;\n' "$sql"
        done
    } | psql -X -q -d "$DB" -t -A \
      | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' \
      | tail -n +2 \
      | sort -g \
      | awk '{v[NR]=$1}
             END {
                 p99i = int(NR*0.99); if (p99i < 1) p99i = NR;
                 printf "%.2f\t%.2f", v[int((NR+1)/2)], v[p99i];
             }'
}

# Capture the PLAN for every measured query.
#
# Not optional.  A latency that is constant across selectivity bands -- 32
# matching documents costing the same as 179,262 -- is the signature of the
# operator being evaluated per row, i.e. the index not being used at all.  A
# harness that greps only "Execution Time" cannot tell "the index is slow" from
# "the planner ignored the index", and those need completely different fixes.
PLANS=${PLANS:-/tmp/lexical_plans.txt}
: > "$PLANS"

plan() {                        # plan <label> <sql>
    {
        printf '\n===== %s =====\n' "$1"
        psql -X -q -d "$DB" -c "EXPLAIN (ANALYZE, BUFFERS, VERBOSE OFF) $2"
    } >> "$PLANS" 2>&1
}

# One line of plan shape, for the summary table: the top node plus whether any
# weave/gin index scan appears anywhere in the tree.
planshape() {                   # planshape <sql>
    psql -X -q -d "$DB" -t -A -c "EXPLAIN $1" 2>/dev/null \
      | awk 'NR==1{gsub(/^ *->? */,"");sub(/ *\(cost.*/,"");top=$0}
             /Index Scan|Bitmap Index Scan|Custom Scan/{ix=1}
             END{printf "%s%s", top, (ix?"":"[NO-INDEX]")}' \
      | cut -c1-38
}

row() {                         # row <label> <weave-sql> <gin-sql>
    plan "$1 / weave" "$2"
    plan "$1 / gin" "$3"
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" \
        "$(coldq "$2")" "$(warmq "$2")" "$(coldq "$3")" "$(warmq "$3")"
}

# ---------------------------------------------------------------------------
# Parallelism sweep.
#
# pg_weave's AM sets amcanparallel = false: a ranked index scan is serial.  With
# parallel query enabled -- the default on every PostgreSQL since 10 -- the
# planner can cost a 5-way Parallel Seq Scan below a serial index ordering scan
# and choose it, at which point pg_weave evaluates <=> on every row of the table
# and loses by two orders of magnitude.
#
# That is a real competitive gap, not a benchmark artifact, so it is measured
# rather than tuned away: both settings are reported.  Reporting only the
# parallelism-off number would be exactly the kind of flattering methodology this
# project's own benchmark skill warns against.
# ---------------------------------------------------------------------------
for PAR in 4 0; do
export PGOPTIONS="-c max_parallel_workers_per_gather=$PAR"
say "==== max_parallel_workers_per_gather = $PAR ===="

say "latency (ms): cold = first scan in a fresh backend; warm = p50/p99 in-session"
{
printf 'query\tweave_cold\tweave_p50\tweave_p99\tgin_cold\tgin_p50\tgin_p99\n'
for band in rare mid common; do
    case $band in rare) T=$RARE;; mid) T=$MID;; common) T=$COMMON;; esac
    for k in 10 100; do
        row "ranked_${band}_k${k}" \
            "SELECT id FROM docs ORDER BY d <=> '$T'::wquery LIMIT $k" \
            "SELECT id, ts_rank(tsv, to_tsquery('simple','$T')) r FROM docs
               WHERE tsv @@ to_tsquery('simple','$T') ORDER BY r DESC LIMIT $k"
    done
done
row count_common \
    "SELECT count(*) FROM docs WHERE d @@@ '$COMMON'::wquery" \
    "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$COMMON')"
row count_AND \
    "SELECT count(*) FROM docs WHERE d @@@ '$RARE & $MID'::wquery" \
    "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$RARE & $MID')"
row count_prefix \
    "SELECT count(*) FROM docs WHERE d @@@ 'word0001*'::wquery" \
    "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','word0001:*')"
} | column -t

say "plan shapes (a [NO-INDEX] here explains a flat latency curve)"
{
printf 'query\tweave_plan\tgin_plan\n'
for band in rare common; do
    case $band in rare) T=$RARE;; common) T=$COMMON;; esac
    printf 'ranked_%s\t%s\t%s\n' "$band" \
      "$(planshape "SELECT id FROM docs ORDER BY d <=> '$T'::wquery LIMIT 10")" \
      "$(planshape "SELECT id, ts_rank(tsv, to_tsquery('simple','$T')) r FROM docs
                      WHERE tsv @@ to_tsquery('simple','$T') ORDER BY r DESC LIMIT 10")"
done
printf 'count_common\t%s\t%s\n' \
  "$(planshape "SELECT count(*) FROM docs WHERE d @@@ '$COMMON'::wquery")" \
  "$(planshape "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$COMMON')")"
printf 'count_AND\t%s\t%s\n' \
  "$(planshape "SELECT count(*) FROM docs WHERE d @@@ '$RARE & $MID'::wquery")" \
  "$(planshape "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$RARE & $MID')")"
} | column -t

say "full plans"
cat "$PLANS"

done
unset PGOPTIONS

say "segments (a fixed per-scan cost scales with this)"
$PSQL -c "SELECT * FROM weave_index_stats('weave_idx')" 2>/dev/null || \
  $PSQL -t -A -c "SELECT weave_segment_count('weave_idx')" 2>/dev/null || \
  echo "  (no segment introspection function; see doc/GAPS.md)"

say "build time (s) and index size"
printf 'engine\tbuild_s\tsize\n%s\n%s\n' "$BUILD_WEAVE" "$BUILD_GIN" | column -t

say "heap"
$PSQL -t -A -c "SELECT pg_size_pretty(pg_total_relation_size('docs'))"
