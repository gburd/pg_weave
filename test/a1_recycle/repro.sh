#!/usr/bin/env bash
# Deterministic reproduction of the A1 scan-vs-merge page-recycle race.
#
# A scan snapshots the segment directory (metapage), releases the metapage lock,
# then walks segment pages under only per-page SHARE locks. A concurrent merge
# frees those pages and a concurrent insert recycles them -> the scan reads a
# now-stale segment descriptor and returns a WRONG result.
#
# This drives the interleave DETERMINISTICALLY using a test-only pause hook
# (compiled in only under -DPG_FTS_TEST_HOOKS): the scan briefly acquires the
# advisory lock named by pg_weave.test_pause_advisory_key right after snapshotting
# the directory. A blocker session holds that lock, stalling the scan in the
# vulnerable window while a merger frees + an inserter recycles the pages.
#
# Invariant: 500 "anchor" rows are NEVER deleted, so the correct match count is
# ALWAYS 500 regardless of MVCC snapshot. Reader != 500  =>  A1 stale read.
#
# Build the test-hooks prefix first (Nix): see test/a1_recycle/README.md.
# Usage:  JOIN=/path/to/pg+ftstest-prefix  bash test/a1_recycle/repro.sh
set -uo pipefail
J="${JOIN:?set JOIN=<symlinkJoin of the -DPG_FTS_TEST_HOOKS pg_weave and postgresql>}"
export PATH="$J/bin:$PATH"
PORT="${PORT:-5443}"
WORK=$(mktemp -d); PGDATA="$WORK/data"
initdb -D "$PGDATA" --no-locale -E UTF8 >/dev/null 2>&1
printf "unix_socket_directories='/tmp'\nlisten_addresses=''\nfsync=off\nshared_buffers=8MB\nmaintenance_work_mem=1MB\n" >> "$PGDATA/postgresql.conf"
pg_ctl -D "$PGDATA" -w -o "-p $PORT" start >/dev/null 2>&1
trap 'pg_ctl -D "$PGDATA" -w stop -m immediate >/dev/null 2>&1 || true' EXIT
export PGHOST=/tmp PGPORT=$PORT PGDATABASE=postgres

psql -X -qtA -v ON_ERROR_STOP=1 <<'EOF' >/dev/null
CREATE EXTENSION pg_weave;
CREATE TABLE d (id bigserial PRIMARY KEY, kind text, body text);
INSERT INTO d(kind,body) SELECT 'anchor','anchorterm filler'||g FROM generate_series(1,500) g;   -- NEVER deleted
INSERT INTO d(kind,body) SELECT 'churn','churnterm x'||(g%20)||' pad'||g FROM generate_series(1,3000) g;
CREATE INDEX d_weave ON d USING weave (to_wdoc('simple', body));
SELECT weave_merge('d_weave');
EOF
base=$(psql -X -qtA -c "SET enable_seqscan=off; SELECT count(*) FROM d WHERE to_wdoc('simple',body) @@@ 'anchorterm'::wquery")
echo "baseline anchors (invariant 500): $base"

psql -X -qtA -c "SELECT pg_advisory_lock(42); SELECT pg_sleep(30)" >/dev/null 2>&1 & BLK=$!; sleep 1
( psql -X -qtA -c "SET pg_weave.test_pause_advisory_key=42; SET enable_seqscan=off;
   SELECT count(*) FROM d WHERE to_wdoc('simple',body) @@@ 'anchorterm'::wquery" > "$WORK/r.out" 2>&1 ) & RD=$!; sleep 2
stalled=$(psql -X -qtA -c "SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND NOT granted")
echo "reader stalled in window (ungranted advisory locks, want 1): $stalled"
psql -X -qtA <<'EOF' >/dev/null 2>&1
DELETE FROM d WHERE kind='churn';
VACUUM d;
SELECT weave_merge('d_weave');
SELECT weave_vacuum('d_weave');
INSERT INTO d(kind,body) SELECT 'churn','zzz recycled'||g FROM generate_series(1,5000) g;
SELECT weave_merge('d_weave');
INSERT INTO d(kind,body) SELECT 'churn','more zzz'||g FROM generate_series(1,5000) g;
SELECT weave_merge('d_weave');
EOF
psql -X -qtA -c "SELECT pg_advisory_unlock(42)" >/dev/null 2>&1; kill $BLK 2>/dev/null||true; wait $RD 2>/dev/null||true
reader=$(cat "$WORK/r.out")
echo "READER RESULT (must be 500; anything else = A1 stale read): $reader"
if [ "$reader" = "500" ]; then echo "PASS (fixed: generation-recheck restarted the scan)"; exit 0
else echo "FAIL (A1 stale read reproduced)"; exit 1; fi
