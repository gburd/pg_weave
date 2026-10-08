#!/bin/bash
# L22 round 2, step 1 (go/no-go): do the two growths hit ORDINARY workloads?
# doc/PHASES.md L22, bench/RESULTS_G75_RECLAIM.md "L22".  No crashes anywhere.
#
# Arms, each on its own private cluster, two runs each, all concurrent on one host:
#   a  each cycle: INSERT 10 %, DELETE the oldest 10 %, plain VACUUM.  autovacuum off.
#   b  the same INSERT/DELETE, autovacuum ON at default settings, no manual VACUUM;
#      each cycle then waits WAIT_B seconds for autovacuum to act (autovacuum_count
#      is logged per cycle as evidence it ran).
#   c  pure append: INSERT 10 %, plain VACUUM, no deletes.  autovacuum off.
# NOTHING assigns an xid between a cycle's VACUUM and the next cycle's INSERT in a/c
# (no txid_current, no DDL), so growth 2's collision is not masked by the harness.
# For the same reason the fresh-build REFERENCE is built in a SEPARATE cluster
# (`ref`): a CREATE INDEX in the arm's own cluster would spend that xid.  The data
# generator is a pure function of the id, so the reference rebuilds the exact live
# row set of each cycle: ids (cB, N+cB] for a/b, (0, N+cB] for c.
# Serial builds everywhere (max_parallel_maintenance_workers = 0), so the
# reference is the one-segment minimum and deterministic.
set -u
OUT=${OUT:-/tmp/out}
mkdir -p $OUT
N=${N:-1000000}
B=${B:-$((N / 10))}
CYC=${CYC:-14}
WAIT_B=${WAIT_B:-90}
RUNS="${RUNS:-a1 a2 b1 b2 c1 c2}"
BIN=/usr/lib/postgresql/17/bin
ROOT=$HOME/l22o
mkdir -p $ROOT
gen() {	# lo hi -> SELECT producing those rows
	echo "SELECT g, to_wdoc('simple', 'common w' || (g % 5000) || ' x' || (g % 13)),
	       ('[' || (g % 1000) || ',' || (g % 777) || ',' || (g % 555) || ',' || (g % 333) || ']')::wvec,
	       g % 1000 FROM generate_series($1, $2) g"
}
mkcluster() {	# name port extra-conf
	local d=$ROOT/$1
	rm -rf $d
	$BIN/initdb -D $d > $OUT/$1_initdb.log 2>&1 || return 1
	cat >> $d/postgresql.conf <<EOF
port = $2
unix_socket_directories = '/tmp'
listen_addresses = ''
shared_buffers = 1GB
maintenance_work_mem = 1GB
max_parallel_maintenance_workers = 0
max_wal_size = 2GB
checkpoint_timeout = 5min
jit = off
log_min_messages = info
$3
EOF
	$BIN/pg_ctl -D $d -l $OUT/$1_server.log -w start > /dev/null
}

arm() {	# run-name port
	local r=$1 port=$2 kind=${1:0:1} conf="autovacuum = off"
	local L=$OUT/$r.log
	local PSQL="psql -X -v ON_ERROR_STOP=1 -h /tmp -p $port -d postgres"
	q() { $PSQL -tAc "$1"; }
	[ $kind = b ] && conf="log_autovacuum_min_duration = 0"
	mkcluster $r $port "$conf" || { echo "$r: cluster failed" >> $L; return 1; }
	$PSQL > $OUT/${r}_setup.log 2>&1 <<SQL || { echo "$r: setup failed" >> $L; return 1; }
CREATE EXTENSION pg_weave;
CREATE TABLE s (id bigint, body wdoc, emb wvec(4), price int8);
INSERT INTO s $(gen 1 $N);
CREATE INDEX s_w ON s USING weave (body, emb, price int8_docval_ops);
SQL
	echo "OWL $r cycle 0: rows=$(q 'SELECT count(*) FROM s') pages=$(q "SELECT pg_relation_size('s_w')/8192")" >> $L
	for c in $(seq 1 $CYC); do
		local t0=$(date +%s)
		local ia=$(q "SELECT weave_alloc_stats_reset(); INSERT INTO s $(gen $((N + (c - 1) * B + 1)) $((N + c * B))); SELECT 'alloc ' || weave_alloc_stats()::text" | grep -o 'alloc (.*' | tail -1)
		[ $kind = c ] || q "DELETE FROM s WHERE id <= $((c * B))" > /dev/null
		local va=
		if [ $kind = b ]; then
			sleep $WAIT_B
		else
			va=$(echo "SELECT weave_alloc_stats_reset(); VACUUM s; SELECT 'alloc ' || weave_alloc_stats()::text;" | $PSQL -tA 2>&1 | grep -o 'alloc (.*' | tail -1)
		fi
		echo "OWL $r cycle $c: rows=$(q 'SELECT count(*) FROM s') pages=$(q "SELECT pg_relation_size('s_w')/8192") freed=$(q "SELECT count(*) FROM weave_page_info('s_w') WHERE coalesce(freed,false)") avc=$(q "SELECT autovacuum_count FROM pg_stat_user_tables WHERE relname='s'") secs=$(( $(date +%s) - t0 )) INSERT $ia VACUUM $va" >> $L
	done
	$BIN/pg_ctl -D $ROOT/$r -m fast stop > /dev/null 2>&1
	echo "OWL $r DONE" >> $L
}

ref() {	# the fresh-build size of every cycle's live row set, both shapes
	local port=5600 L=$OUT/ref.log
	local PSQL="psql -X -v ON_ERROR_STOP=1 -h /tmp -p $port -d postgres"
	mkcluster ref $port "autovacuum = off" || { echo "ref: cluster failed" >> $L; return 1; }
	$PSQL -c "CREATE EXTENSION pg_weave" > /dev/null
	for c in $(seq 0 $CYC); do
		for kind in ab c; do
			local lo=$((c * B + 1))
			[ $kind = c ] && lo=1
			p=$(echo "DROP TABLE IF EXISTS r; CREATE TABLE r (id bigint, body wdoc, emb wvec(4), price int8);
				INSERT INTO r $(gen $lo $((N + c * B)));
				CREATE INDEX r_w ON r USING weave (body, emb, price int8_docval_ops);
				SELECT 'REFPAGES ' || pg_relation_size('r_w')/8192;" | $PSQL -tA 2>&1 | grep -o 'REFPAGES [0-9]*' | cut -d' ' -f2)
			echo "REF $kind cycle $c: pages=$p" >> $L
		done
	done
	$BIN/pg_ctl -D $ROOT/ref -m fast stop > /dev/null 2>&1
	echo "REF DONE" >> $L
}

port=5610
for r in $RUNS; do
	port=$((port + 1))
	arm $r $port &
	sleep 3
done
ref &
wait

# summary: excess over the fresh build, per cycle
for r in $RUNS; do
	k=${r:0:1}; [ $k = c ] || k=ab
	ex=; for c in $(seq 0 $CYC); do
		p=$(grep -o "OWL $r cycle $c: rows=[0-9]* pages=[0-9]*" $OUT/$r.log | grep -o 'pages=[0-9]*' | cut -d= -f2)
		f=$(grep -o "REF $k cycle $c: pages=[0-9]*" $OUT/ref.log | grep -o 'pages=[0-9]*' | cut -d= -f2)
		ex="$ex $((${p:-0} - ${f:-0}))/${f:-?}"
	done
	echo "SUMMARY $r excess/ref per cycle:$ex"
done | tee $OUT/summary.log
grep -c "DONE" $OUT/*.log | tee -a $OUT/summary.log
rm -rf $ROOT
exit 0
