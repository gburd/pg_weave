#!/bin/bash
# bench/aws/readme_job.sh -- run doc/readme_examples.sql on PostgreSQL 17 and 18.
#
#     SCRIPT=bench/aws/readme_job.sh bench/aws/run.sh c7i.2xlarge script
#
# Outputs under /tmp/out (pulled to bench/aws/out/<run>/remote/):
#   readme_pg17.out, readme_pg18.out   psql -a output of the examples file
#   plain_install17.log                the README's install step WITHOUT with_llvm=no,
#                                      into a DESTDIR so the live install is untouched
#   summary.txt                        one line per leg with its exit status
# Exit 0 iff both majors ran the file to its end marker.
set -u
OUT=/tmp/out
mkdir -p $OUT
SRC=$HOME/pg_weave
SQL=$SRC/doc/readme_examples.sql
rc=0
sum() { echo "$*" | tee -a $OUT/summary.txt; }

# --- does a plain `make install` (PGXS default with_llvm) work on this image?
cp -a $SRC /tmp/plain17 && cd /tmp/plain17 && make -s clean >/dev/null 2>&1
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config > $OUT/plain_install17.log 2>&1
mrc=$?
make install DESTDIR=/tmp/stage17 PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config \
	>> $OUT/plain_install17.log 2>&1
irc=$?
nbc=$(find /tmp/stage17 -path '*bitcode*' -name '*.bc' 2>/dev/null | wc -l)
sum "plain17: make rc=$mrc, make install (default with_llvm) rc=$irc, bitcode files staged=$nbc"
cd $SRC

# --- one major: run the examples file against the cluster of that major
run_major() {
	local v=$1 port db=readme$1
	port=$(pg_lsclusters -h | awk -v v=$v '$1==v {print $3; exit}')
	[ -n "$port" ] || { sum "pg$v: no cluster"; return 1; }
	local got
	got=$(psql -X -At -p $port -d postgres -c 'show server_version_num')
	case "$got" in ${v}*) ;; *) sum "pg$v: port $port answered server_version_num=$got"; return 1 ;; esac
	dropdb -p $port --if-exists $db; createdb -p $port $db || { sum "pg$v: createdb failed"; return 1; }
	psql -X -At -p $port -d $db -c 'select version()' > $OUT/version_pg$v.txt 2>&1
	psql -X -a -v ON_ERROR_STOP=1 -p $port -d $db -f $SQL > $OUT/readme_pg$v.out 2>&1
	local prc=$?
	local mark=$(grep -c 'readme_examples: every statement above ran' $OUT/readme_pg$v.out)
	local cos=$(grep -c 'cannot index the cosine metric' $OUT/readme_pg$v.out)
	sum "pg$v: port $port version $got psql rc=$prc marker_lines=$mark cosine_refusal=$cos"
	[ $prc = 0 ] && [ "$mark" -ge 2 ] && [ "$cos" -ge 1 ]
}

run_major 17 || rc=1

# --- PostgreSQL 18: install from PGDG, build and install a separate copy
sudo apt-get -qq install -y postgresql-18 postgresql-server-dev-18 > $OUT/apt18.log 2>&1
arc=$?
pg_lsclusters -h | awk '$1==18' | grep -q . || sudo pg_createcluster 18 main --start >> $OUT/apt18.log 2>&1
sudo pg_ctlcluster 18 main start >> $OUT/apt18.log 2>&1
p18=$(pg_lsclusters -h | awk '$1==18 {print $3; exit}')
sudo -u postgres createuser -s -p $p18 $(whoami) >> $OUT/apt18.log 2>&1
cp -a $SRC /tmp/src18 && cd /tmp/src18 && make -s clean >/dev/null 2>&1
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config > $OUT/build18.log 2>&1
brc=$?
sudo make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config with_llvm=no >> $OUT/build18.log 2>&1
irc=$?
sum "pg18: apt rc=$arc make rc=$brc make install with_llvm=no rc=$irc"
cd $SRC
if [ $arc = 0 ] && [ $brc = 0 ] && [ $irc = 0 ]; then
	run_major 18 || rc=1
else
	tail -30 $OUT/build18.log; rc=1
fi

# Same file, two majors: the outputs should match line for line.
diff $OUT/readme_pg17.out $OUT/readme_pg18.out > $OUT/pg17_vs_pg18.diff 2>&1
sum "pg17 vs pg18 output diff lines: $(wc -l < $OUT/pg17_vs_pg18.diff)"
cat $OUT/summary.txt
exit $rc
