#!/usr/bin/env bash
#
# bench/fuzzy.sh -- Z5 (index-accelerated fuzzy term~1/term~2) and Z6 (character-
# class regex) latency gates, for the record, on a 1M-row corpus.
#
# Both routes are IMPLEMENTED (weave_fuzzy_terms() / weave_regex_terms(),
# src/am/amscan.c) and both were previously measured only on a dev box (see the Z5
# and Z6 rows in doc/PHASES.md): "not for the record" numbers with no bench/aws/run.sh
# run behind them and therefore no commit-tied number.  This is that run.
#
# Methodology, per .agent/skills/weave-bench and copied from bench/lexical.sh:
#	 - warm, REPS reps in one session, first dropped, p50 AND p99
#	 - EVERY ARM RUN TWICE (pass A, pass B) so within-arm spread is visible
#	   before any between-arm or between-query comparison is trusted (hard rule
#	   10) -- an 8.5% "win" in this project's own G20 merge-gate benchmark turned
#	   out to be a single baseline outlier from a one-run arm.
#	 - CORRECTNESS CHECKED BEFORE LATENCY, against a reference that does not use
#	   the index: fuzzy against contrib/fuzzystrmatch's levenshtein_less_equal()
#	   over the row's own tokens, regex against core's `~`.  A benchmark of a
#	   wrong fast path is worse than no benchmark (hard rule 8).
#
# Usage: bench/fuzzy.sh [ndocs] [reps]
#
set -euo pipefail

NDOCS=${1:-1000000}
REPS=${2:-7}
DB=${PGDATABASE:-weavebench}
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

createdb "$DB" 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave;" >/dev/null
$PSQL -c "ALTER EXTENSION pg_weave UPDATE;" >/dev/null 2>&1 || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;" >/dev/null

# Both fuzzy and regex leaves need enable_seqscan off and parallelism off to reach
# the AM's scan machinery at all -- weave_channel_stats() is PARALLEL RESTRICTED
# and a parallel worker's counters never reach the leader, and a seq scan bypasses
# the AM entirely.  This is a session-wide setting for the whole run rather than
# per-query because every timed query and every correctness check in this file
# goes through the same @@@ operator.
export PGOPTIONS="-c max_parallel_workers_per_gather=0 -c enable_seqscan=off"

say "machine facts"
nproc
lscpu | grep -E '^Model name' || true
$PSQL -t -A -c "SELECT version();"
$PSQL -t -A -c "SHOW shared_buffers;"

# ---------------------------------------------------------------------------
# Corpus: 1M rows, ONE stored wdoc column, 8 tokens/row.
#
#   - one id-shaped token 'e' || lpad(((g*7919) % 10000), 4, '0'): 10,000
#     distinct values, NO hyphen -- the analyzer splits 'e-1234' into 'e' and
#     '-1234', which silently turns every regex/fuzzy probe below into a query
#     against the wrong token.  This is what the character-class and literal
#     regex patterns match against.
#   - seven filler tokens 't' || ((g*7 + i*104729) % 250000): ~250,000 distinct
#     values.  This is what the fuzzy terms match against.
#
# ::bigint on every arithmetic operand: g is an int by default in a plpgsql-free
# generate_series, and g*7919 overflows int32 well before g=1,000,000 -- silently,
# producing a table with the WRONG token distribution rather than an error.  Hit
# for real while drafting this script.
# ---------------------------------------------------------------------------
say "corpus: $NDOCS docs, 10k id-shaped terms, ~250k filler terms"
$PSQL <<SQL
\set ON_ERROR_STOP on
\timing off
DROP TABLE IF EXISTS fz CASCADE;
CREATE TABLE fz (id bigint PRIMARY KEY, body text);
INSERT INTO fz
SELECT g,
       'e' || lpad(((g::bigint * 7919) % 10000)::text, 4, '0') || ' ' ||
       (SELECT string_agg('t' || ((g::bigint * 7 + i * 104729) % 250000), ' ')
          FROM generate_series(1, 7) i)
  FROM generate_series(1, $NDOCS) g;
ALTER TABLE fz ADD COLUMN d wdoc;
UPDATE fz SET d = to_wdoc('simple', body);
VACUUM (ANALYZE) fz;
SQL

DISTINCT_ID=$($PSQL -t -A -c "SELECT count(DISTINCT substring(body from '^e[0-9]{4}')) FROM fz")
ROWCOUNT=$($PSQL -t -A -c "SELECT count(*) FROM fz")
say "sanity: $ROWCOUNT rows, $DISTINCT_ID distinct id-shaped terms (want 1000000 / <=10000)"
[ "$ROWCOUNT" = "$NDOCS" ] \
    || { echo "ABORT: row count $ROWCOUNT != requested $NDOCS -- corpus generation failed" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Query list.  Fuzzy terms are filler-shaped so they land in the ~250k-term
# vocabulary; regex patterns are id-shaped so they land in the 10k-term one.
# ---------------------------------------------------------------------------
FUZZY_TERMS=(t123456 t204711 t98765 t31337 t7)

regex_pattern() {
    case $1 in
        class_e12)     printf 'e12[0-9]{2}' ;;
        class_e34)     printf 'e34[0-9]{2}' ;;
        class_e56)     printf 'e56[0-9]{2}' ;;
        literal_e1234) printf 'e1234' ;;
        literal_e5678) printf 'e5678' ;;
        anchored_e12)  printf '^e12' ;;
        fanout_all)    printf 'e[0-9]{4}' ;;
    esac
}
REGEX_LABELS=(class_e12 class_e34 class_e56 literal_e1234 literal_e5678 anchored_e12 fanout_all)

idx_sql() {                      # idx_sql <kind> <arg> <k-or-empty>
    case $1 in
        fuzzy) printf "SELECT count(*) FROM fz WHERE d @@@ '%s~%s'::wquery" "$2" "$3" ;;
        regex) printf "SELECT count(*) FROM fz WHERE d @@@ '/%s/'::wquery" "$2" ;;
    esac
}
ref_sql() {                      # ref_sql <kind> <arg> <k-or-empty>
    case $1 in
        fuzzy) printf "SELECT count(*) FROM fz WHERE EXISTS (SELECT 1 FROM unnest(string_to_array(body,' ')) x WHERE levenshtein_less_equal(x, '%s', %s) <= %s)" "$2" "$3" "$3" ;;
        regex) printf "SELECT count(*) FROM fz WHERE EXISTS (SELECT 1 FROM unnest(string_to_array(body,' ')) x WHERE x ~ '%s')" "$2" ;;
    esac
}

# ---------------------------------------------------------------------------
# Timing: copied verbatim in shape from bench/lexical.sh's warmq() -- one
# session, REPS executions, drop the first, p50/p99 from EXPLAIN's own
# "Execution Time", not client-side wall clock (which would include psql
# connection setup on every rep if it were per-invocation).
# ---------------------------------------------------------------------------
warmq() {                        # warmq <sql>
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

# weave_channel_stats() delta for one query: reset, run once, read the six
# columns the task cares about.  Filtered by field count rather than by line
# position, because a void-returning reset() prints an empty line and the count
# query prints a one-field line -- NF==6 is the only line that can be the stats
# row.
chanstats() {                    # chanstats <sql>
    local sql=$1
    {
        printf 'SELECT weave_channel_stats_reset();\n'
        printf '%s;\n' "$sql"
        printf 'SELECT fuzzy_dict, fuzzy_trgm, regex_dict, regex_trgm, terms_expanded, dict_pages FROM weave_channel_stats();\n'
    } | psql -X -q -d "$DB" -t -A -F $'\t' \
      | awk -F'\t' 'NF==6'
}

build() {                        # build <ddl>
    local ddl=$1 t0 t1
    t0=$(date +%s.%N)
    $PSQL -c "$ddl" >/dev/null
    t1=$(date +%s.%N)
    echo "$t1-$t0" | bc
}

# ---------------------------------------------------------------------------
# One arm: build the index (or not -- 'off' is the default reloption), check
# correctness for every query, then two full passes of warm latency, then the
# channel-stats delta for every query.
# ---------------------------------------------------------------------------
run_arm() {                      # run_arm <arm-name> <index-ddl-suffix>
    local arm=$1 ddl_suffix=$2 buildtime size

    say "arm $arm: building index"
    $PSQL -c "DROP INDEX IF EXISTS fz_idx;" >/dev/null
    buildtime=$(build "CREATE INDEX fz_idx ON fz USING weave (d) $ddl_suffix")
    size=$($PSQL -t -A -c "SELECT pg_relation_size('fz_idx')")
    $PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm" >/dev/null 2>&1 || true
    $PSQL -c "SELECT pg_prewarm('fz_idx');" >/dev/null 2>&1 || true
    printf 'ARM\t%s\tbuild_s\t%.1f\tsize_bytes\t%s\tsize_pretty\t%s\n' \
        "$arm" "$buildtime" "$size" \
        "$($PSQL -t -A -c "SELECT pg_size_pretty($size::bigint)")"

    say "arm $arm: correctness (index count vs no-index reference)"
    local fail=0
    for term in "${FUZZY_TERMS[@]}"; do
        for k in 1 2; do
            local iv rv
            iv=$($PSQL -t -A -c "$(idx_sql fuzzy "$term" "$k")")
            rv=$($PSQL -t -A -c "$(ref_sql fuzzy "$term" "$k")")
            if [ "$iv" != "$rv" ]; then
                printf '  MISMATCH fuzzy %s~%s index=%s ref=%s\n' "$term" "$k" "$iv" "$rv"
                fail=1
            else
                printf '  ok fuzzy %s~%s: %s rows (index and reference agree)\n' "$term" "$k" "$iv"
            fi
        done
    done
    for label in "${REGEX_LABELS[@]}"; do
        local pat iv rv
        pat=$(regex_pattern "$label")
        iv=$($PSQL -t -A -c "$(idx_sql regex "$pat")")
        rv=$($PSQL -t -A -c "$(ref_sql regex "$pat")")
        if [ "$iv" != "$rv" ]; then
            printf '  MISMATCH regex %s (/%s/) index=%s ref=%s\n' "$label" "$pat" "$iv" "$rv"
            fail=1
        else
            printf '  ok regex %s (/%s/): %s rows (index and reference agree)\n' "$label" "$pat" "$iv"
        fi
    done
    [ "$fail" = 0 ] || { echo "ABORT: correctness failed on arm $arm; latency numbers would be meaningless" >&2; exit 1; }

    for pass in A B; do
        say "arm $arm: latency pass $pass (warm, REPS=$REPS, first dropped)"
        {
            printf 'query\tp50_ms\tp99_ms\n'
            for term in "${FUZZY_TERMS[@]}"; do
                for k in 1 2; do
                    printf 'fuzzy_%s~%s\t%s\n' "$term" "$k" "$(warmq "$(idx_sql fuzzy "$term" "$k")")"
                done
            done
            for label in "${REGEX_LABELS[@]}"; do
                printf 'regex_%s\t%s\n' "$label" "$(warmq "$(idx_sql regex "$(regex_pattern "$label")")")"
            done
        } | column -t
        printf 'PASS\t%s\t%s\tdone\n' "$arm" "$pass"
    done

    say "arm $arm: weave_channel_stats() delta per query"
    {
        printf 'query\tfuzzy_dict\tfuzzy_trgm\tregex_dict\tregex_trgm\tterms_expanded\tdict_pages\n'
        for term in "${FUZZY_TERMS[@]}"; do
            for k in 1 2; do
                printf 'fuzzy_%s~%s\t%s\n' "$term" "$k" "$(chanstats "$(idx_sql fuzzy "$term" "$k")")"
            done
        done
        for label in "${REGEX_LABELS[@]}"; do
            printf 'regex_%s\t%s\n' "$label" "$(chanstats "$(idx_sql regex "$(regex_pattern "$label")")")"
        done
    } | column -t
}

say "==== ARM trigrams=off (the default reloption) ===="
run_arm off ""

say "==== ARM trigrams=on ===="
run_arm on "WITH (trigrams = on)"

say "heap size"
$PSQL -t -A -c "SELECT pg_size_pretty(pg_total_relation_size('fz'))"
