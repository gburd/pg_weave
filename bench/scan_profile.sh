#!/usr/bin/env bash
#
# bench/scan_profile.sh -- where does a ranked top-k scan actually spend its time?
#
# G13 (ranked k=10) is the standing competitive loss after L12/L14/L15:
# pg_weave 2.82 / 10.26 / 15.35 ms (rare/mid/common) against pg_textsearch's
# 1.25 / 1.63 / 3.16 ms. doc/PHASES.md L2 asserts the root cause is a
# "decode-bound posting scan on high-df terms" and prescribes impact-ordered
# posting blocks -- an ON-DISK FORMAT CHANGE -- on the strength of that
# assertion. No scan profile has ever been taken.
#
# bench/RESULTS_BUILD_PROFILE.md is the cautionary precedent: it correctly
# identified a hot SYMBOL and then misattributed it to the wrong CAUSE, twice,
# because it aggregated by symbol instead of by caller. L15 spent two of its
# three steps on that error. This script therefore reports callers, not just
# symbols, and pairs every profile with a configuration A/B whose result the
# profile must be consistent with.
#
# The shape of the loss is already known and constrains what the answer can be:
# pg_weave's k=10 and k=100 latencies are within 10% of each other in every
# band, while pg_textsearch's k=100 is 5-7x its k=10. So pg_weave pays a
# near-fixed cost per query that scales with df and NOT with k. The WAND
# initial-k sweep (bench/RESULTS_WAND_K.md, and the wandk* rows of any compete
# run) confirms initial_k is not the floor: mid k=10 is 9.82 ms at initial_k=4
# versus 10.31 ms at 32. Whatever this profile finds must explain a
# k-independent, df-proportional cost.
#
# Run on an engine host provisioned by bench/compete (needs $NVME/pg and a
# loaded corpus + `bands` table in the `bench` database).
#
set -euo pipefail
NVME=${NVME:-/nvme}
export PATH="$NVME/pg/bin:$PATH"
export PGHOST=$NVME/sock PGPORT=${PGPORT:-55432} PGDATABASE=${PGDATABASE:-bench}
OUT=${OUT:-$HOME/compete/out}
REPS=${REPS:-4000}
mkdir -p "$OUT"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $PGDATABASE"

say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

band() { $PSQL -t -A -c "SELECT term FROM bands WHERE band='$1'"; }
R=$(band rare); M=$(band mid); C=$(band common)
say "bands: rare=$R mid=$M common=$C"

# ---------------------------------------------------------------------------
# 1. EXPLAIN (ANALYZE, BUFFERS) per band.
#
# Free, and it separates I/O from CPU before any sampling: a decode-bound scan
# is CPU with a small resident buffer count, while a scan that touches a page
# per scored docid shows it here. Also records rows removed by the top-k, which
# is how "we score far more documents than k" would show up.
# ---------------------------------------------------------------------------
say "EXPLAIN (ANALYZE, BUFFERS) per band"
{
    for spec in "rare:$R" "mid:$M" "common:$C"; do
        b=${spec%%:*}; t=${spec#*:}
        for k in 10 100; do
            echo "=== $b k=$k (term=$t)"
            $PSQL -c "EXPLAIN (ANALYZE, BUFFERS, VERBOSE, TIMING ON)
                      SELECT id FROM docs ORDER BY d <=> '$t'::wquery LIMIT $k" 2>&1
        done
    done
} > "$OUT/scan_explain.txt" 2>&1

# ---------------------------------------------------------------------------
# 2. Configuration A/B: the doclen sidecar's per-scan cost.
#
# doc/PHASES.md L3 claims the v4 sidecar "decodes the whole segment sidecar per
# scan, a fixed ~18 ms" and prescribes a page-directory cursor. pg_weave's
# amscan already has a cursored reader ("O(log pages) + ~1 page read per ~128
# scored docids, with 0 up-front full decode"), so L3 may already be satisfied
# -- or the cursor may be seeking per docid, which would be exactly the
# k-independent df-proportional cost we are looking for.
#
# doclen_sidecar=off falls back to the v3 inline doclen column: same ranking,
# larger index, no sidecar lookups at all. The latency difference IS the
# sidecar's per-scan tax, measured rather than asserted.
# ---------------------------------------------------------------------------
timeq() {
    local label=$1 idx=$2 term=$3 k=$4 i t0 t1
    # warm
    for i in 1 2 3 4 5; do
        $PSQL -c "SELECT id FROM docs ORDER BY d <=> '$term'::wquery LIMIT $k" >/dev/null
    done
    t0=$(date +%s.%N)
    for i in $(seq 1 50); do
        $PSQL -c "SELECT id FROM docs ORDER BY d <=> '$term'::wquery LIMIT $k" >/dev/null
    done
    t1=$(date +%s.%N)
    # per-query ms, including psql round-trip -- the DIFFERENCE between arms is
    # what is being read, and the round-trip is identical in both
    printf '%-34s %8.2f ms/query\n' "$label" "$(echo "($t1 - $t0) * 1000 / 50" | bc -l)"
}

say "A/B: doclen sidecar on (default) vs off (v3 inline)"
{
    $PSQL -c "DROP INDEX IF EXISTS prof_sc_on"  >/dev/null
    $PSQL -c "DROP INDEX IF EXISTS prof_sc_off" >/dev/null
    $PSQL -c "CREATE INDEX prof_sc_on ON docs USING weave (d)" >/dev/null
    echo "sidecar=on  size: $($PSQL -t -A -c "SELECT pg_size_pretty(pg_relation_size('prof_sc_on'))")"
    for spec in "rare:$R" "mid:$M" "common:$C"; do
        timeq "sidecar=on  ${spec%%:*} k=10" prof_sc_on "${spec#*:}" 10
    done
    $PSQL -c "DROP INDEX prof_sc_on" >/dev/null

    $PSQL -c "CREATE INDEX prof_sc_off ON docs USING weave (d) WITH (doclen_sidecar=off)" >/dev/null
    echo "sidecar=off size: $($PSQL -t -A -c "SELECT pg_size_pretty(pg_relation_size('prof_sc_off'))")"
    for spec in "rare:$R" "mid:$M" "common:$C"; do
        timeq "sidecar=off ${spec%%:*} k=10" prof_sc_off "${spec#*:}" 10
    done
    $PSQL -c "DROP INDEX prof_sc_off" >/dev/null
} 2>&1 | tee "$OUT/scan_sidecar_ab.txt"

# ---------------------------------------------------------------------------
# 3. Symbol AND caller profile of a ranked mid-band k=10 scan.
#
# mid is the worst ratio (6.3x) and is not distorted by common's sheer posting
# volume, so it is the band to attribute. One query is ~10 ms, far too short to
# sample, so the same query runs REPS times in ONE backend and perf samples
# that backend -- the same code path the harness measures, just repeated.
#
# Two reports, deliberately:
#   --no-children  self time, "which code is executing"
#   --children -g  cumulative with call chains, "on whose behalf"
# The build profile only ever produced the first kind, which is how one symbol
# with three callers got attributed to one cause.
# ---------------------------------------------------------------------------
if command -v perf >/dev/null 2>&1; then
    say "perf profile of ranked mid k=10, $REPS reps"
    $PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null
    $PSQL -c "CREATE INDEX prof_weave ON docs USING weave (d)" >/dev/null
    $PSQL -c "SELECT pg_prewarm('prof_weave')" >/dev/null 2>&1 || true

    QF=/tmp/scan_rep.sql
    : > "$QF"
    echo "SET pg_weave.wand_initial_k = 32;" >> "$QF"
    for _ in $(seq 1 "$REPS"); do
        echo "SELECT id FROM docs ORDER BY d <=> '$M'::wquery LIMIT 10;" >> "$QF"
    done

    ( psql -X -q -o /dev/null -d "$PGDATABASE" -f "$QF" ) &
    LOOP=$!
    sleep 2
    BPID=$(psql -X -t -A -d "$PGDATABASE" -c \
        "SELECT pid FROM pg_stat_activity
          WHERE query LIKE 'SELECT id FROM docs ORDER BY%' AND pid <> pg_backend_pid()
          LIMIT 1")
    if [ -n "$BPID" ]; then
        perf record -F 499 --call-graph dwarf -p "$BPID" -o "$OUT/scan.perf" -- sleep 45 \
            >/dev/null 2>&1 || true
    fi
    kill $LOOP 2>/dev/null || true
    wait $LOOP 2>/dev/null || true

    if [ -s "$OUT/scan.perf" ]; then
        {
            echo "===== SELF TIME (which code is executing) ====="
            perf report -i "$OUT/scan.perf" --stdio --no-children \
                -F overhead,symbol 2>/dev/null | grep -vE '^#|^$' | head -25
            echo
            echo "===== CUMULATIVE + CALLERS (on whose behalf) ====="
            echo "# read this one first: RESULTS_BUILD_PROFILE.md mis-attributed a"
            echo "# hot symbol because it never looked at who called it."
            perf report -i "$OUT/scan.perf" --stdio --children -g graph,0.5,caller \
                -F overhead,symbol 2>/dev/null | grep -vE '^#|^$' | head -90
        } | tee "$OUT/scan_symbols.txt"
    else
        echo "perf produced no samples" | tee "$OUT/scan_symbols.txt"
    fi
    $PSQL -c "DROP INDEX IF EXISTS prof_weave" >/dev/null
else
    say "perf not available; EXPLAIN and A/B only"
fi

say "artifacts: $OUT/scan_explain.txt $OUT/scan_sidecar_ab.txt $OUT/scan_symbols.txt"
