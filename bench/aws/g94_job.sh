#!/bin/bash
# G94 (ranked scan with NOT / prefix) on EC2.  Run as
#   SMOKE_TOLERATE_RED=1 SCRIPT=bench/aws/g94_job.sh bench/aws/run.sh c7i.4xlarge script
# A  keep the full-run outputs of ranked_not and tsquery_cast; list every other differing file
# B  solo baseline of the two files, run TWICE (must not differ from itself)
# C  mutants: each must APPLY, BUILD, INSTALL a .so differing from the clean one, then
#    change the solo output; the clean tree is reinstalled and re-run at the end
# D  latency: NOT queries at 200k and 1M rows, before (padding decision as on main) and
#    after, two runs per arm
# E  PG18 installcheck (regression + isolation) with the full-run outputs as expected
# STAGES selects (default ABCDE).  Results under /tmp/out.
set -u
cd ~/pg_weave
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
OUT=/tmp/out
mkdir -p $OUT
STAGES=${STAGES:-ABCDE}
TESTS="tsquery_cast ranked_not"
NOTICE='NOTICE:  extension "pg_weave" already exists, skipping'

# ---- A
cp /tmp/ic.log $OUT/ic.full.log 2>/dev/null
cp regression.diffs $OUT/regression.full.diffs 2>/dev/null
for t in $TESTS; do cp results/$t.out $OUT/$t.full.out 2>/dev/null; done
for f in results/*.out; do b=$(basename $f); cmp -s $f expected/$b || echo "A: differs from expected: $b"; done | tee $OUT/A.txt
grep -E "^(ok|not ok)|^# |^Result|^t/" /tmp/ic.log | head -80 > $OUT/ic.full.summary

install_tree() {	# $1 tree, $2 label
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make -j"$(nproc)" PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}
# run the two tests solo in tree $1; concatenated output (NOTICE removed) -> $OUT/solo-$2.out
solo() {
	(cd "$1" && for t in $TESTS; do find results -name "$t.out" -delete 2>/dev/null; touch expected/$t.out; done
	 make installcheck PG_CONFIG=$PGC REGRESS="$TESTS" ISOLATION= TAP_TESTS= > $OUT/ic-$2.log 2>&1)
	: > $OUT/solo-$2.out
	for t in $TESTS; do
		[ -s "$1/results/$t.out" ] || { echo RAN_NOTHING; return; }
		grep -vF "$NOTICE" "$1/results/$t.out" >> $OUT/solo-$2.out
	done
	echo $OUT/solo-$2.out
}

# ---- B
if [[ $STAGES == *B* ]]; then
	c1=$(solo ~/pg_weave clean1); c2=$(solo ~/pg_weave clean2)
	echo "B: clean1=$c1 clean2=$c2" | tee $OUT/B.txt
	[ "$c1" != RAN_NOTHING ] && [ "$c2" != RAN_NOTHING ] || { echo "B: CONTROL ran nothing" | tee -a $OUT/B.txt; exit 1; }
	echo "B: clean vs clean diff lines: $(diff $c1 $c2 | wc -l) (must be 0)" | tee -a $OUT/B.txt
	for t in $TESTS; do
		grep -vF "$NOTICE" $OUT/$t.full.out > /tmp/f.out; grep -vF "$NOTICE" ~/pg_weave/results/$t.out > /tmp/s.out
		echo "B: $t full-run vs solo diff lines, NOTICE removed: $(diff /tmp/f.out /tmp/s.out | grep -c '^[<>]') (must be 0)" | tee -a $OUT/B.txt
	done
fi

# ---- C
# mutant NAME FILE FROM TO  (python literal replace, must match exactly once)
CLEAN_MD5=$(md5sum < $LIB/pg_weave.so | cut -c1-12)
mutant() {
	local name=$1 m=~/mut-$1
	find "$m" -depth -delete 2>/dev/null
	cp -a ~/pg_weave "$m"
	( cd "$m" && FROM="$3" TO="$4" python3 -c "
import os; p='$2'; s=open(p).read(); a=os.environ['FROM']; n=s.count(a)
assert n == 1, 'pattern matched %d times' % n
open(p,'w').write(s.replace(a, os.environ['TO']))" ) || { echo "MUTANT $name: DID NOT APPLY"; return; }
	if ! install_tree "$m" "$name"; then
		echo "MUTANT $name: DID NOT BUILD/INSTALL -- not counted"; grep -m5 'error' $OUT/build-$name.log; return
	fi
	local md5=$(md5sum < $LIB/pg_weave.so | cut -c1-12)
	[ "$md5" != "$CLEAN_MD5" ] || { echo "MUTANT $name: installed .so is the CLEAN one -- not counted"; return; }
	local o=$(solo "$m" "$name")
	[ "$o" != RAN_NOTHING ] || { echo "MUTANT $name: no results -- not counted"; return; }
	diff $OUT/solo-clean1.out $o > $OUT/mut-$name.diff
	local n=$(grep -c '^[<>]' $OUT/mut-$name.diff)
	if [ "$n" -gt 0 ]; then echo "MUTANT $name: BUILT (so=$md5), KILLED ($n changed lines)"
	else echo "MUTANT $name: BUILT (so=$md5), SURVIVED"; fi
	find "$m" -depth -delete
}
A=src/am/amscan.c
if [[ $STAGES == *C* ]]; then
{
echo "C: clean .so md5 $CLEAN_MD5"
# P1 padding disabled again (main's decision: never pad when the restriction is the ORDER BY query)
mutant P1_nopad $A '	return !weave_query_lit_covered(so->query);
}' '	return false && !weave_query_lit_covered(so->query);
}'
# P2 padding rows not rechecked when the restriction is the ORDER BY query
mutant P2_norecheck $A '	scan->xs_recheck = (scan->numberOfKeys > 0);
	weave_set_itup(scan, so);' '	scan->xs_recheck = (scan->numberOfKeys > 0) && !so->ordQueryRestricts;
	weave_set_itup(scan, so);'
# P3 padding starts before the ranked phase is exhausted (after the first pass)
mutant P3_pad_early $A '			  : weave_ord_grow(scan->indexRelation, so)))' \
	'			  : (!weave_pad_wanted(scan, so) && weave_ord_grow(scan->indexRelation, so))))'
# P4 padding rows published below the ranked distances
mutant P4_pad_below $A '		: 1.0;
	dist[0].isnull = nulldist;' '		: (so->ordQueryRestricts ? 0.5 : 1.0);
	dist[0].isnull = nulldist;'
# P5 a prefix leaf counted as a literal term (prefix-only queries not padded)
mutant P5_prefix_covered $A '	return weave_query_leaves_cover(q, WEAVE_QF_PREFIX | WEAVE_QF_FUZZY |' \
	'	return weave_query_leaves_cover(q, WEAVE_QF_FUZZY |'
# P6 an OR covered by one covered arm (fox | !dog not padded)
mutant P6_or_one_arm $A '			st[top - 1] = (it->op == WEAVE_OP_OR) ? (st[top - 1] && st[top])' \
	'			st[top - 1] = (it->op == WEAVE_OP_OR) ? (want ? (st[top - 1] && st[top]) : (st[top - 1] || st[top]))'
install_tree ~/pg_weave clean-restore || { echo "C: clean reinstall FAILED"; exit 1; }
echo "C: reinstalled .so md5 $(md5sum < $LIB/pg_weave.so | cut -c1-12) (must be $CLEAN_MD5)"
o=$(solo ~/pg_weave clean3); echo "C: clean after mutants vs clean1 diff lines: $(diff $OUT/solo-clean1.out $o | wc -l) (must be 0)"
} 2>&1 | tee $OUT/C.txt
fi

# ---- D
if [[ $STAGES == *D* ]]; then
{
P="psql -X -q -At -d postgres -v ON_ERROR_STOP=1"
# before = the padding decision as on main (P1), after = this tree
B=~/before; find $B -depth -delete 2>/dev/null; cp -a ~/pg_weave $B
( cd $B && FROM='	return !weave_query_lit_covered(so->query);
}' TO='	return false && !weave_query_lit_covered(so->query);
}' python3 -c "
import os; p='src/am/amscan.c'; s=open(p).read(); a=os.environ['FROM']; assert s.count(a)==1
open(p,'w').write(s.replace(a, os.environ['TO']))" ) || { echo "D: before tree did not apply"; exit 1; }
install_only() {	# $1 an already-built tree
	(cd "$1" && sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-only.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	echo "D: installed $1 so=$(md5sum < $LIB/pg_weave.so | cut -c1-12)"
}
install_tree $B before || { echo "D: before tree did not build"; exit 1; }
install_tree ~/pg_weave after || exit 1
for n in 200000 1000000; do
	install_only ~/pg_weave || exit 1
	$P -c "CREATE EXTENSION IF NOT EXISTS pg_weave" || exit 1
	$P <<SQL || exit 1
DROP TABLE IF EXISTS lt;
CREATE TABLE lt (id int, d wdoc) WITH (autovacuum_enabled = off);
-- w1 in 10 % of documents, w2 in 50 %, rare in 0.2 %, 8 filler terms each
INSERT INTO lt SELECT g, to_wdoc('simple', concat_ws(' ',
   CASE WHEN g % 10 = 0 THEN 'w1' END, CASE WHEN g % 2 = 0 THEN 'w2' END,
   CASE WHEN g % 500 = 3 THEN 'rare' END,
   (SELECT string_agg('f' || ((g * 7919 + i * 104729) % 50000), ' ') FROM generate_series(1, 8) i)))
  FROM generate_series(1, $n) g;
CREATE INDEX lt_w ON lt USING weave (d);
VACUUM ANALYZE lt;
SQL
	for run in 1 2; do
		for arm in after before; do
			if [ $arm = after ]; then install_only ~/pg_weave || exit 1
			else install_only $B || exit 1; fi
			for spec in "!w1@10" "!w1@1000" "rare | !w1@10" "rare | !w1@5000" "w2 & !w1@10" "w1@10"; do
				q=${spec%@*}; lim=${spec#*@}
				fq="SELECT id FROM lt WHERE d @@@ '$q'::wquery ORDER BY d <=> '$q'::wquery LIMIT $lim"
				r=$($P <<SQL 2>&1 | tr '\n' ' '
SET jit = off; SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
SELECT 'rows', count(*) FROM ($fq) s;
CREATE TEMP TABLE tm(ms float8);
DO \$\$ DECLARE i int; t0 timestamptz; BEGIN
  FOR i IN 1..15 LOOP
    t0 := clock_timestamp();
    PERFORM count(*) FROM ($fq) s;
    IF i > 3 THEN INSERT INTO tm VALUES (extract(epoch FROM clock_timestamp() - t0) * 1000); END IF;
  END LOOP; END \$\$;
SELECT 'median_ms', percentile_cont(0.5) WITHIN GROUP (ORDER BY ms)::numeric(10,3),
       'min_ms', min(ms)::numeric(10,3), 'max_ms', max(ms)::numeric(10,3) FROM tm;
SQL
)
				echo "D n=$n run=$run arm=$arm q='$q' limit=$lim $r"
			done
			# the plan, once per arm: it must be the ordering index scan
			$P -c "SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
			       EXPLAIN (COSTS OFF) SELECT id FROM lt WHERE d @@@ '!w1'::wquery ORDER BY d <=> '!w1'::wquery LIMIT 10" \
				| tr '\n' ' ' | sed "s/^/D plan n=$n run=$run arm=$arm: /"; echo
		done
	done
	$P -c "SELECT 'D truth n=$n', count(*) FILTER (WHERE id % 10 <> 0), count(*) FILTER (WHERE id % 10 <> 0 OR id % 500 = 3) FROM lt"
done
install_only ~/pg_weave || exit 1
} 2>&1 | tee $OUT/D.txt
fi

# ---- E
if [[ $STAGES == *E* ]]; then
{
for t in $TESTS; do cp $OUT/$t.full.out expected/$t.out; done
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
echo "G94 JOB DONE"
