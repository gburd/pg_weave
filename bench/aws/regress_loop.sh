#!/bin/bash
# bench/aws/regress_loop.sh -- run the full regression suite many times on K
# independent clusters at once, and keep the evidence for every failure.
#
# Built for doc/GAPS.md G60: sql/vecindex.sql's delete-then-merge assertion
# (live_lanes_dropped, expected 6) read 0 once in a local full run and did not
# reproduce.  The suspected mechanism is a snapshot in the same database
# holding the removable horizon back during `VACUUM vw`, so the dead heap
# tuples survive, no tombstone is written, and the merge keeps their lanes.
# The prime suspect is autovacuum/autoanalyze, so the clusters are split into
# three ARMS by i % 3:
#	 on	  the defaults (naptime 1min) -- what CI and every developer has
#	 fast autovacuum_naptime = 1s, so a worker is almost always around during
#		  a ~1 min suite: the amplifier
#	 off  autovacuum disabled
# Failures only in on/fast, and more in fast, pin the mechanism.  A failure in
# `off` refutes it, and then the per-failure server log (log_statement = all,
# log_autovacuum_min_duration = 0, one slice per failing run) is what to read.
#
# Runs ON THE INSTANCE, started detached by run.sh's regressloop job, as the
# login user (passwordless sudo).  Writes under /scratch/rl:
#	 c<i>/summary.tsv  one line per run: cluster arm run rc failed-tests g60 secs
#	 c<i>/run<n>/	   kept only for a failing run: pg_regress output,
#					   results/, regression.diffs, that run's server log
#	 ALLDONE		   when every cluster has finished
#
# Usage: regress_loop.sh [K clusters, default 15] [iterations each, default 12]

set -u
K=${1:-15}
ITERS=${2:-12}
PR=/usr/lib/postgresql/17/lib/pgxs/src/test/regress/pg_regress
SRC=$HOME/pg_weave
R=/scratch/rl
TESTS=$(sed -n 's/^REGRESS = //p' "$SRC/Makefile")
ME=$(whoami)

[ -x "$PR" ] || { echo "no pg_regress at $PR" >&2; exit 1; }
[ -n "$TESTS" ] || { echo "no REGRESS list in $SRC/Makefile" >&2; exit 1; }
mkdir -p "$R"
echo "tests: $TESTS"

arm_of() {
	case $(($1 % 3)) in
		1) echo on ;;
		2) echo fast ;;
		0) echo off ;;
	esac
}

for i in $(seq 1 "$K"); do
	port=$((5500 + i))
	arm=$(arm_of "$i")
	av=on
	[ "$arm" = off ] && av=off
	nap=1min
	[ "$arm" = fast ] && nap=1s
	sudo pg_createcluster 17 "rl$i" -p "$port" >/dev/null || exit 1
	sudo mkdir -p "/etc/postgresql/17/rl$i/conf.d"
	sudo tee "/etc/postgresql/17/rl$i/conf.d/rl.conf" >/dev/null <<EOF
shared_buffers = 512MB
jit = off
autovacuum = $av
autovacuum_naptime = $nap
log_line_prefix = '%m [%p] %d %a %b '
log_statement = 'all'
log_autovacuum_min_duration = 0
log_lock_waits = on
EOF
	sudo pg_ctlcluster 17 "rl$i" start || exit 1
	sudo -u postgres psql -p "$port" -qc "CREATE ROLE $ME SUPERUSER LOGIN" 2>/dev/null
	# assert the arm actually took: a setting nobody reads back is a guess
	got=$(psql -p "$port" -d postgres -tAc "select current_setting('autovacuum') || '/' || current_setting('autovacuum_naptime')")
	want="$av/$nap"
	[ "$got" = "$want" ] || { echo "cluster $i: got $got, wanted $want" >&2; exit 1; }
	echo "cluster $i port $port arm=$arm ($got)"
done

loop() {
	local i=$1 port=$((5500 + $1)) d=$R/c$1 arm n out rc fails g60 t0 t1 log off
	arm=$(arm_of "$i")
	log=/var/log/postgresql/postgresql-17-rl$i.log
	mkdir -p "$d"
	for n in $(seq 1 "$ITERS"); do
		out=$d/run$n
		mkdir -p "$out"
		psql -p "$port" -d postgres -q \
			-c 'DROP DATABASE IF EXISTS contrib_regression' \
			-c 'CREATE DATABASE contrib_regression' >/dev/null 2>&1
		off=$(sudo stat -c %s "$log")
		t0=$(date +%s)
		(cd "$SRC" && "$PR" --use-existing --port="$port" --inputdir=. \
			--outputdir="$out" --dbname=contrib_regression $TESTS) \
			>"$out/pr.log" 2>&1
		rc=$?
		t1=$(date +%s)
		fails=$(sed -nE 's/^not ok +[0-9]+ +- +([a-z_0-9]+).*/\1/p' "$out/pr.log" | paste -sd, -)
		# The G60 assertion's own value, read from the output rather than
		# inferred from pass/fail: evidence that the site ran, and what it said.
		g60=$(grep -A2 'lanes_invented_or_shifted | live_lanes_dropped' \
				"$out/results/vecindex.out" 2>/dev/null | sed -n 3p | tr -d ' ')
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$i" "$arm" "$n" "$rc" \
			"${fails:--}" "${g60:-missing}" "$((t1 - t0))" >>"$d/summary.tsv"
		if [ "$rc" = 0 ]; then
			find "$out" -mindepth 1 -depth -delete
			rmdir "$out"
		else
			sudo tail -c +"$((off + 1))" "$log" >"$out/server.log"
		fi
	done
	touch "$d/DONE"
}

for i in $(seq 1 "$K"); do
	loop "$i" &
done
wait
touch "$R/ALLDONE"
echo "all clusters done"
