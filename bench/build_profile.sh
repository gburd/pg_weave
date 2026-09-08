#!/usr/bin/env bash
#
# bench/build_profile.sh -- where does CREATE INDEX actually spend its time?
#
# G5 (build time) stands at 6.96x behind Timescale pg_textsearch after L12, and
# doc/GAPS.md says "no further route identified". That claim was made WITHOUT a
# profile, which is exactly the mistake doc/PHASES.md warns about for L9 -- attribute
# before changing code. This attributes it.
#
# Method: run the same CREATE INDEX under `perf record`, and separately time the
# build's phases by isolating them, so the profile and the wall-clock accounting can
# be cross-checked against each other. A profile alone cannot tell posting
# construction from the tuplesort that feeds it, because both are hot in the same
# call graph.
#
# Run on an engine host provisioned by bench/compete (needs $NVME/pg and a loaded
# corpus in the `bench` database).
#
set -euo pipefail
NVME=${NVME:-/nvme}
export PATH="$NVME/pg/bin:$PATH"
export PGHOST=$NVME/sock PGPORT=${PGPORT:-55432} PGDATABASE=${PGDATABASE:-bench}
OUT=${OUT:-$HOME/compete/out}
mkdir -p "$OUT"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $PGDATABASE"

say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

$PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null

# ---------------------------------------------------------------------------
# Phase accounting.
#
# CREATE INDEX is one statement, so the phases cannot be timed separately from
# outside. Instead time three builds that do progressively more work and take
# differences. Each is a separate CREATE INDEX on the same heap, so the heap scan and
# analysis cost appear in all three and cancel.
#
#   A. positions=off trigrams=off  -- baseline: scan, analyze, sort, postings, merge
#   B. positions=on  trigrams=off  -- adds the positional 4th FOR column
#   C. positions=off trigrams=on   -- adds the vocabulary trigram map
#
# B-A isolates positions; C-A isolates trigrams; A itself is the irreducible core,
# which is what matters for G5 because A is the DEFAULT configuration.
# ---------------------------------------------------------------------------
time_build() {
    local label=$1 opts=$2 t0 t1
    $PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null
    t0=$(date +%s.%N)
    $PSQL -c "CREATE INDEX prof_weave ON docs USING weave (d) $opts" >/dev/null
    t1=$(date +%s.%N)
    local secs; secs=$(echo "$t1 - $t0" | bc)
    local sz; sz=$($PSQL -t -A -c "SELECT pg_size_pretty(pg_relation_size('prof_weave'))")
    printf '%-28s %8.1f s   %s\n' "$label" "$secs" "$sz"
    echo "$secs"
}

say "phase accounting (differences isolate optional work)"
{
A=$(time_build "A default (pos=off,trgm=off)" "" | tail -1)
B=$(time_build "B positions=on" "WITH (positions=on)" | tail -1)
C=$(time_build "C trigrams=on" "WITH (trigrams=on)" | tail -1)
printf '\n%-28s %8.1f s\n' "positions cost (B-A)" "$(echo "$B - $A" | bc)"
printf '%-28s %8.1f s\n' "trigrams cost (C-A)" "$(echo "$C - $A" | bc)"
printf '%-28s %8.1f s  <- what G5 must attack\n' "irreducible core (A)" "$A"
} | tee "$OUT/build_phases.txt"

# ---------------------------------------------------------------------------
# Symbol-level profile of the DEFAULT build.
#
# perf needs the postgres binary's symbols; we built PostgreSQL from source with -O2
# and no strip, so they are present. --call-graph dwarf because -O2 omits frame
# pointers, and without it the caller attribution is useless -- which is the whole
# point of profiling rather than guessing.
# ---------------------------------------------------------------------------
if command -v perf >/dev/null 2>&1; then
    say "perf profile of the default build"
    $PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null
    PID=$($PSQL -t -A -c "SELECT pg_backend_pid()")
    ( sleep 1; $PSQL -c "CREATE INDEX prof_weave ON docs USING weave (d)" >/dev/null ) &
    BUILDER=$!
    sleep 2
    # profile the backend actually running the build, not the one that reported a pid
    BPID=$(psql -X -t -A -d "$PGDATABASE" -c \
       "SELECT pid FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX prof_weave%' AND pid <> pg_backend_pid() LIMIT 1")
    if [ -n "$BPID" ]; then
        perf record -F 199 --call-graph dwarf -p "$BPID" -o "$OUT/build.perf" -- sleep 60 \
            >/dev/null 2>&1 || true
    fi
    wait $BUILDER || true
    if [ -s "$OUT/build.perf" ]; then
        perf report -i "$OUT/build.perf" --stdio --no-children -F overhead,symbol 2>/dev/null \
            | grep -vE '^#|^$' | head -30 | tee "$OUT/build_symbols.txt"
    else
        echo "perf produced no samples (the build may have finished first)" | tee "$OUT/build_symbols.txt"
    fi
else
    say "perf not available; phase accounting only"
fi

$PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null
say "artifacts: $OUT/build_phases.txt $OUT/build_symbols.txt"
