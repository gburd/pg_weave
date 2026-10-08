#!/bin/bash
# G86 fused follow-up on EC2.  Run as
#   SMOKE_TOLERATE_RED=1 SCRIPT=bench/aws/g86f_job.sh bench/aws/run.sh c7i.4xlarge script
# after a smoke that may be red ONLY on score_reuse (its expected file is owed).
#   A. keep the full-run score_reuse output and diffs; list every other differing file
#   B. solo baseline of score_reuse (the mutants are compared against it)
#   C. mutants, each asserted to BUILD and INSTALL, then run against the baseline
#   D. latency, reuse off vs on, two runs per arm, short 200k/1M and long 50k/200k
#   E. PG18 installcheck (regression + isolation) with the full-run output as expected
# STAGES selects (default ABCDE).  Results under /tmp/out.
set -u
cd ~/pg_weave
PGC=/usr/lib/postgresql/17/bin/pg_config
OUT=/tmp/out
mkdir -p $OUT
STAGES=${STAGES:-ABCDE}

# ---- A
cp /tmp/ic.log $OUT/ic.full.log 2>/dev/null
cp regression.diffs $OUT/regression.full.diffs 2>/dev/null
cp results/score_reuse.out $OUT/score_reuse.full.out 2>/dev/null
for f in results/*.out; do b=$(basename $f); cmp -s $f expected/$b || echo "A: differs from expected: $b"; done | tee $OUT/A.txt
grep -E "^(ok|not ok)|^# |^Result" /tmp/ic.log | head -60 > $OUT/ic.full.summary
echo "A: full-run output saved: $(wc -l < $OUT/score_reuse.full.out 2>/dev/null) lines" | tee -a $OUT/A.txt

solo() {	# $1 = tree; runs score_reuse alone; returns its status
	( cd "$1" && make installcheck PG_CONFIG=$PGC REGRESS=score_reuse ISOLATION= TAP_TESTS= > /tmp/solo.log 2>&1 )
}

# ---- B
if [[ $STAGES == *B* ]]; then
	cp $OUT/score_reuse.full.out expected/score_reuse.out
	solo ~/pg_weave; echo "B: solo vs full-run expected: status $? (1 = the NOTICE line only)"
	cp results/score_reuse.out $OUT/score_reuse.solo.out
	diff $OUT/score_reuse.full.out $OUT/score_reuse.solo.out > $OUT/full_vs_solo.diff
	echo "B: full vs solo differ in $(grep -c '^[<>]' $OUT/full_vs_solo.diff) lines (expect the NOTICE only)"
fi

# ---- C
mutant() {	# $1 name, $2 python patch run in the tree
	local name=$1 m=~/mut-$1
	find "$m" -depth -delete 2>/dev/null
	cp -a ~/pg_weave "$m"
	( cd "$m" && python3 -c "$2" ) || { echo "MUTANT $name: PATCH FAILED"; return; }
	( cd "$m" && make clean >/dev/null 2>&1; make -j"$(nproc)" PG_CONFIG=$PGC > $OUT/mut-$name.build.log 2>&1 )
	if [ $? -ne 0 ]; then echo "MUTANT $name: DID NOT BUILD"; grep -m5 error: $OUT/mut-$name.build.log; return; fi
	( cd "$m" && sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/mut-$name.install.log 2>&1 ) \
		|| { echo "MUTANT $name: DID NOT INSTALL"; return; }
	sudo find /usr/lib/postgresql/17/lib/bitcode -maxdepth 1 -name "pg_weave*" -exec find {} -depth -delete \; 2>/dev/null
	echo "MUTANT $name: built .so md5 $(md5sum < $m/pg_weave.so | cut -c1-12), installed $(md5sum < $($PGC --pkglibdir)/pg_weave.so | cut -c1-12)"
	cp $OUT/score_reuse.solo.out "$m/expected/score_reuse.out"
	solo "$m"; st=$?
	cp "$m/regression.diffs" $OUT/mut-$name.diffs 2>/dev/null
	if [ $st -ne 0 ] && [ -s $OUT/mut-$name.diffs ]; then
		echo "MUTANT $name: BUILT, KILLED ($(grep -c '^[-+] ' $OUT/mut-$name.diffs) changed lines)"
	else
		echo "MUTANT $name: BUILT, SURVIVED (status $st)"
	fi
	find "$m" -depth -delete
}
if [[ $STAGES == *C* ]]; then
{
echo "C: unmutated .so md5 $(md5sum < pg_weave.so | cut -c1-12)"
# 1. a VISIBLE fuse() entry substituted too
mutant visible "
p='src/am/customscan.c'; s=open(p).read()
a='''		if (!tle->resjunk ||
			!weave_fuse_is_scan_key(tle->expr, scan->indexorderbyorig))'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''		if (!weave_fuse_is_scan_key(tle->expr, scan->indexorderbyorig))'''))"
# 2. the PREVIOUS row's value published (shared publisher: both routes)
mutant prevrow "
p='src/am/amscan.c'; s=open(p).read()
a='	so->curdistValue = v;\n'; assert s.count(a)==1
open(p,'w').write(s.replace(a,'	{ static double prevv = 0.0; so->curdistValue = so->curdistLive && so->curdistSeq ? prevv : v; prevv = v; }\n'))"
# 3a. wrong padding value published, lexical-only fused padding (0 -> 1.0)
mutant padzero "
p='src/am/amscan.c'; s=open(p).read()
a='''	if (weave_curdist_wanted(scan, so))
		weave_curdist_note(scan, so, dist[0].value, dist[0].isnull);'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''	if (weave_curdist_wanted(scan, so))
		weave_curdist_note(scan, so, so->fuseScan && dist[0].value == 0.0 ? 1.0 : dist[0].value, dist[0].isnull);'''))"
# 3b. wrong padding value published, vector fused padding (+Infinity -> 0)
mutant padinf "
p='src/am/amscan.c'; s=open(p).read()
a='''	if (weave_curdist_wanted(scan, so))
		weave_curdist_note(scan, so, dist[0].value, dist[0].isnull);'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''	if (weave_curdist_wanted(scan, so))
		weave_curdist_note(scan, so, so->fuseScan && isinf(dist[0].value) ? 0.0 : dist[0].value, dist[0].isnull);'''))"
# 4. UNKEYED: any live fused scan answers, whatever its index, row or keys
mutant unkeyed "
p='src/am/amscan.c'; s=open(p).read()
a='''		if (!so->fuseScan || so->curdistIndex != indexoid ||
			scan->numberOfOrderBys != nkeys ||
			!ItemPointerEquals(&scan->xs_heaptid, tid))
			continue;
		if (!weave_curfuse_keys_match(fcinfo, scan, so))
			continue;'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''		if (!so->fuseScan || scan == NULL)
			continue;'''))"
# 5. keys checked but not the weights (clobber A)
mutant noweights "
p='src/am/amscan.c'; s=open(p).read()
a='''				memcmp(ARR_DATA_PTR(a), so->fuseW,
					   so->nfuse * sizeof(float4)) != 0)'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''				false)'''))"
# 6. no ROUTE check in the lexical lookup: a fused scan's WHERE query answers it (clobber D)
mutant noroute "
p='src/am/amscan.c'; s=open(p).read()
a='''		if (so->fuseScan || so->query == NULL || so->curdistIndex != indexoid ||'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''		if (so->query == NULL || so->curdistIndex != indexoid ||'''))"
# 7. a fused scan never publishes (control: reuse silently off)
mutant nopublish "
p='src/am/amscan.c'; s=open(p).read()
a='''	if (so->fuseScan)
		return true;
	return scan->numberOfOrderBys == 1 && !so->vecScan && !so->edistScan &&'''; assert s.count(a)==1
open(p,'w').write(s.replace(a,'''	if (so->fuseScan)
		return false;
	return scan->numberOfOrderBys == 1 && !so->vecScan && !so->edistScan &&'''))"
# the real build back
( make -j"$(nproc)" PG_CONFIG=$PGC >/dev/null 2>&1 && sudo make install PG_CONFIG=$PGC with_llvm=no >/dev/null 2>&1 ) || { echo "reinstall FAILED"; exit 1; }
sudo find /usr/lib/postgresql/17/lib/bitcode -maxdepth 1 -name "pg_weave*" -exec find {} -depth -delete \; 2>/dev/null
echo "C: reinstalled .so md5 $(md5sum < $($PGC --pkglibdir)/pg_weave.so | cut -c1-12)"
cp $OUT/score_reuse.solo.out expected/score_reuse.out
solo ~/pg_weave; echo "C: unmutated solo after the mutants: status $? (expect 0)"
} 2>&1 | tee $OUT/C.txt
fi

# ---- D
if [[ $STAGES == *D* ]]; then
P="psql -X -q -At -d postgres -v ON_ERROR_STOP=1"
$P -c "CREATE EXTENSION IF NOT EXISTS pg_weave" || exit 1
$P -c "ALTER EXTENSION pg_weave UPDATE" || exit 1
QV="'[0.3,0.6,0.2,0.9,0.1,0.5,0.7,0.4]'"
corpus() {	# $1 kind (short|long), $2 n -- the prize job's corpus
	local body
	if [ "$1" = short ]; then
		body="repeat('x' || (g % 101) || ' ', 1 + g % 23)"
	else
		body="(SELECT string_agg('w' || ((g * 7919 + i * 104729) % 60000), ' ') FROM generate_series(1, 700) i)"
	fi
	$P <<SQL || exit 1
DROP TABLE IF EXISTS lh;
CREATE TABLE lh (id int, d wdoc, v wvec(8)) WITH (autovacuum_enabled = off);
INSERT INTO lh SELECT g, to_wdoc('simple',
   CASE WHEN g % 10 < 3 THEN 'a ' ELSE '' END || CASE WHEN g % 10 >= 7 THEN 'b ' ELSE '' END ||
   CASE WHEN g % 10 = 5 THEN 'c ' ELSE '' END || CASE WHEN g % 500 = 7 THEN 'rare ' ELSE '' END || $body),
   CASE WHEN g % 997 = 0 THEN NULL
        ELSE ('[' || (g % 97) / 97.0 || ',' || (g % 89) / 89.0 || ',' || (g % 83) / 83.0 || ',' ||
              (g % 79) / 79.0 || ',' || (g % 73) / 73.0 || ',' || (g % 71) / 71.0 || ',' ||
              (g % 67) / 67.0 || ',' || (g % 61) / 61.0 || ']')::wvec END
  FROM generate_series(1, $2) g;
CREATE INDEX lh_w ON lh USING weave (d, v);
VACUUM ANALYZE lh;
SQL
	$P -c "SELECT '$1 $2', avg(pg_column_size(d))::int, pg_size_pretty(pg_relation_size(reltoastrelid))
	       FROM lh, pg_class WHERE relname = 'lh' GROUP BY reltoastrelid" | tee -a $OUT/corpus.txt
}
for spec in "short 200000" "short 1000000" "long 50000" "long 200000"; do
	set -- $spec; kind=$1; n=$2
	corpus "$kind" "$n"
	for run in 1 2; do
		for qn in lexlex lexvec; do
			if [ $qn = lexlex ]; then fx="fuse(d <=> 'c', d <=> 'rare')"; else fx="fuse(d <=> 'c', v <-> $QV)"; fi
			for lim in 10 400; do
				for reuse in off on; do
					fq="SELECT id FROM lh ORDER BY $fx LIMIT $lim"
					f=$OUT/d_${kind}_${n}_${qn}_${lim}_${run}_${reuse}
					$P > $f 2>&1 <<SQL
SET jit = off; SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
SET pg_weave.reuse_distance = $reuse;
SELECT count(*) FROM lh WHERE d @@@ 'zzz';
SELECT 'guc', setting FROM pg_settings WHERE name = 'pg_weave.reuse_distance';
CREATE TEMP TABLE pl(l text);
DO \$\$ DECLARE r record; BEGIN FOR r IN EXECUTE \$q\$EXPLAIN (VERBOSE, COSTS OFF) $fq\$q\$ LOOP INSERT INTO pl VALUES (r."QUERY PLAN"); END LOOP; END \$\$;
SELECT 'planted', count(*) FROM pl WHERE l LIKE '%weave_current_fused_distance%';
SELECT 'fused', count(*) FROM pl WHERE l LIKE '%<~>%';
SELECT 'md5', md5(string_agg(id::text, ',')) FROM ($fq) s;
SET track_functions = 'all'; SET stats_fetch_consistency = none;
SELECT pg_stat_force_next_flush();
CREATE TEMP TABLE c0 AS SELECT funcname, calls FROM pg_stat_user_functions;
SELECT 'rows', count(*) FROM ($fq) s;
SELECT pg_stat_force_next_flush();
SELECT 'calls', coalesce(string_agg(f.funcname || '=' || (f.calls - coalesce(c0.calls, 0)), ' ' ORDER BY f.funcname), 'none')
  FROM pg_stat_user_functions f LEFT JOIN c0 USING (funcname) WHERE f.calls - coalesce(c0.calls, 0) > 0;
RESET track_functions;
CREATE TEMP TABLE tm(ms float8);
DO \$\$ DECLARE i int; t0 timestamptz; BEGIN
  FOR i IN 1..30 LOOP
    t0 := clock_timestamp();
    PERFORM count(*) FROM ($fq) s;
    IF i > 5 THEN INSERT INTO tm VALUES (extract(epoch FROM clock_timestamp() - t0) * 1000); END IF;
  END LOOP; END \$\$;
SELECT 'median_ms', percentile_cont(0.5) WITHIN GROUP (ORDER BY ms)::numeric(10,3) FROM tm;
SQL
					echo "$kind $n $qn L$lim run$run reuse=$reuse $(tr '\n' ' ' < $f)" | tee -a $OUT/D.txt
				done
			done
		done
	done
done
fi

# ---- E
if [[ $STAGES == *E* ]]; then
{
cp $OUT/score_reuse.full.out expected/score_reuse.out
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q postgresql-18 postgresql-server-dev-18 \
	> $OUT/apt18.log 2>&1 || { echo "E: apt PG18 FAILED"; tail -5 $OUT/apt18.log; exit 1; }
pg_lsclusters | awk '$1 == 18' | grep -q . || sudo pg_createcluster 18 main > /dev/null
sudo pg_ctlcluster 18 main start 2>/dev/null || true
PORT=$(pg_lsclusters | awk '$1 == 18 { print $3 }' | head -1)
sudo -u postgres psql -p "$PORT" -c "CREATE ROLE $(whoami) SUPERUSER LOGIN" >/dev/null 2>&1 || true
v=$(psql -p "$PORT" -d postgres -XAtc "show server_version_num")
echo "E: PG18 port $PORT server_version_num $v"
[ "${v:0:2}" = 18 ] || { echo "E: port $PORT is not PG18"; exit 1; }
P18=/usr/lib/postgresql/18/bin/pg_config
make clean > /dev/null 2>&1
make -j"$(nproc)" PG_CONFIG=$P18 > $OUT/build18.log 2>&1 || { echo "E: PG18 build FAILED"; grep -m10 error: $OUT/build18.log; exit 1; }
sudo make install PG_CONFIG=$P18 with_llvm=no > $OUT/install18.log 2>&1 || { echo "E: PG18 install FAILED"; tail -5 $OUT/install18.log; exit 1; }
sudo find /usr/lib/postgresql/18/lib/bitcode -maxdepth 1 -name "pg_weave*" -exec find {} -depth -delete \; 2>/dev/null
PGPORT=$PORT make installcheck PG_CONFIG=$P18 TAP_TESTS= > $OUT/ic18.log 2>&1; rc=$?
cp regression.diffs $OUT/regression18.diffs 2>/dev/null
grep -E "^(ok|not ok) |^# (All|[0-9]+ of)" $OUT/ic18.log
echo "E: PG18 installcheck exit $rc"
} 2>&1 | tee $OUT/E.txt
fi
echo "G86F JOB DONE"
