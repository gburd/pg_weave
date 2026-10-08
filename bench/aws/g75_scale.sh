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
#
# THE TWIN (task L22, doc/PHASES.md).  Table `u` gets the same rows, inserted
# after `s`'s in each cycle as t/033 does, and the same VACUUM schedule, but is
# never crashed.  The excess of s_w over u_w is the size bound t/033 asserts, here
# at 1M rows: at most the largest single crash's stranding + 64 + 5 % of the twin,
# at every cycle.  G75_TWIN=0 turns it off.
# Also times the reclaim pass on the 1M-row index (the cost the spec owes).
set -u
OUT=${OUT:-/tmp/out}
mkdir -p $OUT
N=${G75_N:-1000000}
CYCLES=${G75_CYCLES:-8}
TWIN=${G75_TWIN:-1}
# L22 ablation knobs (doc/PHASES.md L22).  ORDER=us inserts the twin first; BURN=1
# spends one xid in its own transaction before the INSERT into s; CKPT=1 runs a
# CHECKPOINT after each cycle's VACUUMs, so a crash cannot revert the free space
# map past the previous cycle.
ORDER=${G75_ORDER:-su}
BURN=${G75_BURN:-0}
CKPT=${G75_CKPT:-0}
BATCH=${G75_BATCH:-60000}
BIN=/usr/lib/postgresql/17/bin
D=/tmp/g75scale${G75_TAG:-}
PORT=${G75_PORT:-5499}
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
checkpoint_timeout = ${G75_CKPT_TIMEOUT:-5min}
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
CREATE TABLE u (LIKE s);
INSERT INTO u SELECT * FROM s;
CREATE INDEX u_w ON u USING weave (body, emb, price int8_docval_ops);
SQL
rc=$?; log "setup rc=$rc"; [ $rc = 0 ] || { tail $OUT/scale_setup.log; exit 1; }

leaked() { q "SELECT count(*) FROM weave_page_info('s_w') WHERE NOT reachable AND coalesce(freed,false)=false AND NOT uninitialized"; }
pages() { q "SELECT pg_relation_size('${1:-s_w}') / 8192"; }
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
# (stdin, not -c: a multi-statement -c string is a transaction block, and
# VACUUM refuses one; the reclaim reports at DEBUG2 for a plain VACUUM)
for r in 1 2; do
	t0=$(date +%s.%N)
	echo "SET client_min_messages = debug2; VACUUM s;" | $PSQL > $OUT/scale_quiet$r.log 2>&1
	t1=$(date +%s.%N)
	line=$(grep -o 'reclaimed .*' $OUT/scale_quiet$r.log | tail -1)
	[ $TWIN = 1 ] && q "VACUUM u" > /dev/null
	log "SCALE quiet VACUUM $r: $(awk "BEGIN{print $t1 - $t0}") s total; $line"
done

sumstranded=0
maxst=0
worst=-1000000000
exs=
for c in $(seq 1 $CYCLES); do
	lo=$((N + (c - 1) * BATCH + 1)); hi=$((N + c * BATCH))
	# each INSERT bracketed by the allocator counters, in its own session
	ins() { q "SELECT weave_alloc_stats_reset(); INSERT INTO $1 SELECT g, to_wdoc('simple', 'common cyc$c w' || g), ('[' || (g % 1000) || ',1,1,1]')::wvec, g % 1000 FROM generate_series($lo, $hi) g; SELECT 'alloc ' || weave_alloc_stats()::text" | grep -o 'alloc (.*' | tail -1; }
	[ $BURN = 1 ] && q "SELECT txid_current()" > /dev/null
	if [ "$ORDER" = us ] && [ $TWIN = 1 ]; then
		ia_u=$(ins u); ia_s=$(ins s)
	else
		ia_s=$(ins s); ia_u=; [ $TWIN = 1 ] && ia_u=$(ins u)
	fi
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
	[ "$st" -gt "$maxst" ] && maxst=$st
	# the twin's schedule mirrors t/033's: one VACUUM where s had the crashed one,
	# s's post-crash VACUUM, then one more
	[ $TWIN = 1 ] && q "VACUUM u" > /dev/null
	echo "SET client_min_messages = debug2; SELECT weave_alloc_stats_reset(); VACUUM s; SELECT 'alloc ' || weave_alloc_stats()::text;" \
		| $PSQL > $OUT/scale_post$c.log 2>&1 \
		|| { log "VACUUM failed after cycle $c"; fail=1; }
	[ $TWIN = 1 ] && q "VACUUM u" > /dev/null
	[ $CKPT = 1 ] && q "CHECKPOINT" > /dev/null
	rline=$(grep -o 'reclaimed .*' $OUT/scale_post$c.log | tail -1)
	lk=$(leaked)
	bad=$(q "SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail,''), '; '), '') FROM weave_check('s_w', true) WHERE NOT ok")
	check cycle$c || fail=1
	[ "$lk" = 0 ] || fail=1
	[ -z "$bad" ] || fail=1
	aline=$(grep -o 'alloc (.*' $OUT/scale_post$c.log | tail -1)
	tline=
	if [ $TWIN = 1 ]; then
		ps=$(pages); pu=$(pages u_w); ex=$((ps - pu)); exs="$exs $ex"
		[ "$ex" -gt "$worst" ] && worst=$ex
		tline=" twin u_w=$pu excess=$ex"
	fi
	log "SCALE cycle $c: in_window=$inwin stranded=$st leaked_after_vacuum=$lk deep_bad='$bad' pages $before -> $(pages)$tline; post-crash VACUUM $aline; INSERT s $ia_s; INSERT u $ia_u; $rline"
done

# a never-crashed reference for the same row count: REINDEX is the minimum
pf=$(pages)
q "CREATE INDEX s_ref ON s USING weave (body, emb, price int8_docval_ops)" > /dev/null
pr=$(q "SELECT pg_relation_size('s_ref') / 8192")
log "SCALE end: s_w=$pf pages, freshly built reference=$pr pages, total stranded by $CYCLES crashes=$sumstranded"
[ "$sumstranded" -gt 0 ] || { log "SCALE: no crash stranded anything -- the window was never hit"; fail=1; }
if [ $TWIN = 1 ]; then
	bound=$((maxst + 64 + $(pages u_w) / 20))
	log "SCALE twin: excess of s_w over u_w per cycle:$exs; worst $worst, bound $bound (max stranding $maxst + 64 + 5% of twin)"
	[ "$worst" -le "$bound" ] || { log "SCALE twin: BOUND FAILED"; fail=1; }
fi

$BIN/pg_ctl -D $D -m fast stop > /dev/null 2>&1
rm -rf $D
log "RESULT fail=$fail"
exit $fail
