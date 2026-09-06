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
timeq() {                       # timeq <label> <sql>
    local label=$1 sql=$2 i ms
    local -a t=()
    for i in $(seq 1 "$REPS"); do
        ms=$($PSQL -t -A -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $sql" \
             | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p')
        [ -z "$ms" ] && ms=0
        [ "$i" = 1 ] && continue      # drop the first
        t+=("$ms")
    done
    printf '%s\n' "${t[@]}" | sort -g | awk -v l="$label" '
        {v[NR]=$1}
        END {
            p50 = v[int((NR+1)/2)];
            p99i = int(NR*0.99); if (p99i < 1) p99i = NR;
            printf "%s\t%.2f\t%.2f\n", l, p50, v[p99i];
        }'
}

say "latency (ms, warm, median of $((REPS-1)) after dropping the first)"
{
printf 'query\tweave_p50\tweave_p99\tgin_p50\tgin_p99\n'
for band in rare mid common; do
    case $band in rare) T=$RARE;; mid) T=$MID;; common) T=$COMMON;; esac
    for k in 10 100; do
        w=$(timeq w "SELECT id FROM docs ORDER BY d <=> '$T'::wquery LIMIT $k")
        g=$(timeq g "SELECT id, ts_rank(tsv, to_tsquery('simple','$T')) r FROM docs
                     WHERE tsv @@ to_tsquery('simple','$T') ORDER BY r DESC LIMIT $k")
        printf 'ranked_%s_k%s\t%s\t%s\n' "$band" "$k" \
            "$(echo "$w" | cut -f2-3 | tr '\t' '\t')" "$(echo "$g" | cut -f2-3)"
    done
done

w=$(timeq w "SELECT count(*) FROM docs WHERE d @@@ '$COMMON'::wquery")
g=$(timeq g "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$COMMON')")
printf 'count_common\t%s\t%s\n' "$(echo "$w"|cut -f2-3)" "$(echo "$g"|cut -f2-3)"

w=$(timeq w "SELECT count(*) FROM docs WHERE d @@@ '$RARE & $MID'::wquery")
g=$(timeq g "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','$RARE & $MID')")
printf 'count_AND\t%s\t%s\n' "$(echo "$w"|cut -f2-3)" "$(echo "$g"|cut -f2-3)"

w=$(timeq w "SELECT count(*) FROM docs WHERE d @@@ 'word_001*'::wquery")
g=$(timeq g "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple','word_001:*')")
printf 'count_prefix\t%s\t%s\n' "$(echo "$w"|cut -f2-3)" "$(echo "$g"|cut -f2-3)"
} | column -t

say "build time (s) and index size"
printf 'engine\tbuild_s\tsize\n%s\n%s\n' "$BUILD_WEAVE" "$BUILD_GIN" | column -t

say "heap"
$PSQL -t -A -c "SELECT pg_size_pretty(pg_total_relation_size('docs'))"
