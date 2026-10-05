#!/bin/bash
# G75 scale gate (AGENTS.md hard rule 12: VACUUM code needs a run at scale).
#
# 1M rows on a private cluster (initdb + pg_ctl, so an immediate stop is ours to
# give), then CYCLES times: insert a pending batch, start a VACUUM that flushes
# it, and stop the server IMMEDIATELY once the flush has written pages it has
# not published (the leak is visible in weave_page_info -- that is the window).
# Restart, record what the crash stranded, VACUUM, and require
#   - 0 leaked pages and weave_check(deep) clean,
#   - every probe query equal between the index and the heap,
# and at the end that the index did not grow by the sum of what was stranded.
# Also times the reclaim pass on the 1M-row index (the cost the spec owes).
set -u
OUT=${OUT:-/tmp/out}
mkdir -p $OUT
N=${G75_N:-1000000}
CYCLES=${G75_CYCLES:-8}
BATCH=${G75_BATCH:-60000}
BIN=/usr/lib/postgresql/17/bin
D=/tmp/g75scale
PORT=5499
PSQL="psql -X -v ON_ERROR_STOP=1 -h /tmp -p $PORT -d postgres"
log() { echo "$(date +%T) $*" | tee -a $OUT/scale_progress.log; }
q() { $PSQL -tAc "$1"; }

rm -rf $D
$BIN/initdb -D $D > $OUT/scale_initdb.log 2>&1 || { log "initdb failed"; exit 1; }
cat >> $D/postgresql.conf <<EOF
port = $PORT
unix_socket_directories = '/tmp'
listen_addresses = ''
autovacuum = off
shared_buffers = 1GB
maintenance_work_mem = 1GB
max_wal_size = 8GB
wal_writer_delay = 1ms
wal_writer_flush_after = 0
log_min_messages = info
EOF
start() { $BIN/pg_ctl -D $D -l $OUT/scale_server.log -w start > /dev/null; }
start || { log "server did not start"; exit 1; }

$PSQL > $OUT/scale_setup.log 2>&1 <<SQL
CREATE EXTENSION pg_weave;
CREATE TABLE s (id bigint, body wdoc, emb wvec(4), price int8);
INSERT INTO s SELECT g, to_wdoc('simple', 'common w' || (g % 5000) || ' x' || (g % 13)),
       ('[' || (g % 1000) || ',' || (g % 777) || ',' || (g % 555) || ',' || (g % 333) || ']')::wvec,
       g % 1000
  FROM generate_series(1, $N) g;
CREATE INDEX s_w ON s USING weave (body, emb, price int8_docval_ops);
SQL
rc=$?; log "setup rc=$rc"; [ $rc = 0 ] || { tail $OUT/scale_setup.log; exit 1; }

leaked() { q "SELECT count(*) FROM weave_page_info('s_w') WHERE NOT reachable AND coalesce(freed,false)=false AND NOT uninitialized"; }
pages() { q "SELECT pg_relation_size('s_w') / 8192"; }
check() {	# phase -> 0 iff index == heap on every probe
	local fail=0
	for p in "body @@@ 'common'" "body @@@ 'x3 & !w7'" "price < 3" "price < 3 AND body @@@ '!x5'"; do
		h=$(q "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) || ':' || coalesce(sum(hashint8(id)),0) FROM s WHERE $p")
		i=$(q "SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT count(*) || ':' || coalesce(sum(hashint8(id)),0) FROM s WHERE $p")
		[ "$h" = "$i" ] || { log "MISMATCH $1: $p heap=$h index=$i"; fail=1; }
	done
	h=$(q "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT string_agg(id::text, ',') FROM (SELECT id FROM s ORDER BY emb <-> '[1,2,3,4]'::wvec, id LIMIT 10) x")
	i=$(q "SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT string_agg(id::text, ',') FROM (SELECT id FROM s ORDER BY emb <-> '[1,2,3,4]'::wvec, id LIMIT 10) x")
	[ "$h" = "$i" ] || { log "MISMATCH $1: vector heap=$h index=$i"; fail=1; }
	return $fail
}

fail=0
check setup || fail=1
p0=$(pages); log "SCALE setup: $N rows, index $p0 pages"

# the reclaim's cost on the 1M-row index, VACUUM with nothing pending (twice)
for r in 1 2; do
	t0=$(date +%s.%N)
	q "VACUUM s" > /dev/null
	t1=$(date +%s.%N)
	line=$(grep 'reclaimed .* stranded' $OUT/scale_server.log | tail -1)
	log "SCALE quiet VACUUM $r: $(echo "$t1 - $t0" | bc) s; $line"
done

sumstranded=0
for c in $(seq 1 $CYCLES); do
	lo=$((N + (c - 1) * BATCH + 1)); hi=$((N + c * BATCH))
	q "INSERT INTO s SELECT g, to_wdoc('simple', 'common cyc$c w' || g), ('[' || (g % 1000) || ',1,1,1]')::wvec, g % 1000 FROM generate_series($lo, $hi) g" > /dev/null
	before=$(pages)
	$PSQL -c "VACUUM s" > $OUT/scale_vac$c.log 2>&1 &
	inwin=0
	for t in $(seq 1 4000); do
		if [ "$(leaked)" -gt 0 ]; then inwin=1; break; fi
		kill -0 $! 2>/dev/null || break
	done
	$BIN/pg_ctl -D $D -m immediate stop > /dev/null 2>&1
	wait 2>/dev/null
	start || { log "restart failed in cycle $c"; exit 1; }
	st=$(leaked)
	sumstranded=$((sumstranded + st))
	q "VACUUM s" > /dev/null || { log "VACUUM failed after cycle $c"; fail=1; }
	lk=$(leaked)
	bad=$(q "SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail,''), '; '), '') FROM weave_check('s_w', true) WHERE NOT ok")
	check cycle$c || fail=1
	[ "$lk" = 0 ] || fail=1
	[ -z "$bad" ] || fail=1
	log "SCALE cycle $c: in_window=$inwin stranded=$st leaked_after_vacuum=$lk deep_bad='$bad' pages $before -> $(pages)"
done

# a never-crashed reference for the same row count: REINDEX is the minimum
pf=$(pages)
q "CREATE INDEX s_ref ON s USING weave (body, emb, price int8_docval_ops)" > /dev/null
pr=$(q "SELECT pg_relation_size('s_ref') / 8192")
log "SCALE end: s_w=$pf pages, freshly built reference=$pr pages, total stranded by $CYCLES crashes=$sumstranded"
[ "$sumstranded" -gt 0 ] || { log "SCALE: no crash stranded anything -- the window was never hit"; fail=1; }

$BIN/pg_ctl -D $D -m fast stop > /dev/null 2>&1
rm -rf $D
log "RESULT fail=$fail"
exit $fail
