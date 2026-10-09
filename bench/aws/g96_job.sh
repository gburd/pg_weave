#!/bin/bash
# G96 (wquery_out text parses back) on EC2.  Run as
#   SMOKE_TOLERATE_RED=1 SCRIPT=bench/aws/g96_job.sh bench/aws/run.sh c7i.4xlarge script
# A  keep the full-run outputs; list every file that differs from expected
# B  solo baseline of tsquery_cast, run TWICE (must not differ from itself); full vs solo
# C  mutants: each must APPLY, BUILD, INSTALL a .so differing from the clean one, then
#    change the solo output; the clean tree is reinstalled and re-run at the end
# E  PG18 installcheck (regression + isolation) with the full-run outputs as expected
# STAGES selects (default ABCE).  Results under /tmp/out.
set -u
cd ~/pg_weave
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
OUT=/tmp/out
mkdir -p $OUT
STAGES=${STAGES:-ABCE}
TESTS="wquery_roundtrip tsquery_cast"
NOTICE='NOTICE:  extension "pg_weave" already exists, skipping'

# ---- A
cp /tmp/ic.log $OUT/ic.full.log 2>/dev/null
cp regression.diffs $OUT/regression.full.diffs 2>/dev/null
mkdir -p $OUT/results; cp results/*.out $OUT/results/ 2>/dev/null
for f in results/*.out; do b=$(basename $f); cmp -s $f expected/$b || echo "A: differs from expected: $b"; done | tee $OUT/A.txt
grep -E "^(ok|not ok)|^# |^Result|^t/" /tmp/ic.log | head -80 > $OUT/ic.full.summary

install_tree() {	# $1 tree, $2 label
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make -j"$(nproc)" PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}
solo() {	# $1 tree, $2 label -> path of NOTICE-stripped output, or RAN_NOTHING
	(cd "$1" && for t in $TESTS; do find results -name "$t.out" -delete 2>/dev/null; touch expected/$t.out; done
	 make installcheck PG_CONFIG=$PGC REGRESS="$TESTS" ISOLATION= TAP_TESTS= > $OUT/ic-$2.log 2>&1)
	: > $OUT/solo-$2.out
	for t in $TESTS; do
		[ -s "$1/results/$t.out" ] || { echo RAN_NOTHING; return; }
		grep -vF "$NOTICE" "$1/results/$t.out" >> $OUT/solo-$2.out
	done
	echo $OUT/solo-$2.out
}
# the installed .so must be this tree's
install_tree ~/pg_weave clean || { echo "clean install FAILED"; exit 1; }

# ---- B
if [[ $STAGES == *B* ]]; then
{
	c1=$(solo ~/pg_weave clean1); c2=$(solo ~/pg_weave clean2)
	echo "B: clean1=$c1 clean2=$c2"
	[ "$c1" != RAN_NOTHING ] && [ "$c2" != RAN_NOTHING ] || { echo "B: CONTROL ran nothing"; exit 1; }
	echo "B: clean vs clean diff lines: $(diff $c1 $c2 | wc -l) (must be 0)"
	for t in $TESTS; do grep -vF "$NOTICE" $OUT/results/$t.out; done > /tmp/f.out
	echo "B: full-run vs solo diff lines, NOTICE removed: $(diff /tmp/f.out $c1 | grep -c '^[<>]') (must be 0)"
	echo "B: DIFFERENT rows in clean: $(grep -cE '\| DIFFERENT +\|' $c1) (must be 0)"
	echo "B: ' f ' cells in clean (a same_bytes/same_text false): $(grep -cE '\| f( |$)' $c1)"
	grep -q 'round_trip' $c1 && echo "B: round-trip summaries present" || echo "B: round-trip summaries MISSING"
} 2>&1 | tee $OUT/B.txt
fi

# ---- C
CLEAN_MD5=$(md5sum < $LIB/pg_weave.so | cut -c1-12)
mutant() {	# NAME FILE FROM TO  (literal replace, must match exactly once)
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
	if [ "$n" -gt 0 ]; then echo "MUTANT $name: BUILT (so=$md5), KILLED ($n changed lines, $(grep -cE '\| DIFFERENT +\|' $o) DIFFERENT rows)"
	else echo "MUTANT $name: BUILT (so=$md5), SURVIVED"; fi
	find "$m" -depth -delete
}
if [[ $STAGES == *C* ]]; then
{
echo "C: clean .so md5 $CLEAN_MD5"
# M1 `<N>` parsed as at-most (only `<->` stays exact)
mutant M1_gap_at_most src/query/parse.c '					tok.exact = !atmost;' \
	"					tok.exact = !atmost && st->buf[st->pos - 2] == '-';"
# M2 the weight suffix dropped
mutant M2_weight_dropped src/query/parse.c '			tok->weightmask = mask;
			st->pos = p;' '			tok->weightmask = 0;
			st->pos = p;'
# M3 the prefix suffix dropped
mutant M3_prefix_dropped src/query/parse.c '		tok->prefix = true;
		st->pos++;' '		st->pos++;'
# M4 ~k printed off by one
mutant M4_fuzzy_k_off_by_one src/query/parse.c '					appendStringInfo(&e.s, "~%u", it->distance);' \
	'					appendStringInfo(&e.s, "~%u", it->distance + 1);'
# M5 ~k parsed off by one
mutant M5_fuzzy_parse_off_by_one src/query/parse.c '		tok->fuzzy_k = (e > p) ? (int) k : 2;' \
	'		tok->fuzzy_k = (e > p) ? (int) k + 1 : 2;'
# M6 stopword elision drops an operator's flags again (the qnode_flatten bug)
mutant M6_flatten_drops_flags src/query/parse.c '	out[*k].flags = n->flags;' '	out[*k].flags = 0;'
# M7 an exact gap printed without subtracting the right operand's width
mutant M7_out_no_width src/query/parse.c '						else if ((int64) it->distance - r->width == 1)' \
	'						else if ((int64) it->distance == 1)'
install_tree ~/pg_weave clean-restore || { echo "C: clean reinstall FAILED"; exit 1; }
echo "C: reinstalled .so md5 $(md5sum < $LIB/pg_weave.so | cut -c1-12) (must be $CLEAN_MD5)"
o=$(solo ~/pg_weave clean3); echo "C: clean after mutants vs clean1 diff lines: $(diff $OUT/solo-clean1.out $o | wc -l) (must be 0)"
} 2>&1 | tee $OUT/C.txt
fi

# ---- E
if [[ $STAGES == *E* ]]; then
{
for f in $OUT/results/*.out; do cp $f expected/$(basename $f); done
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
echo "G96 JOB DONE"
