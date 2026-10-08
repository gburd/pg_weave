#!/bin/bash
# L22 EC2 job: t/033 under ablation arms of the cleanup's compaction trigger
# (doc/PHASES.md L22).  Each arm is a copy of the tree with ONE exact-once
# substitution in src/am/amvacuum.c, built and installed (distinct .so md5
# required), then the TESTS run L22N times.  Per run the TAP log and the node's
# server log (the "L22 census" lines, when the tree has the diagnostic) are kept
# under /tmp/out/<arm>-<run>/.
# ARMS: base (no substitution), or any name in apply() below.
#
# The C arms patch the DIAGNOSTIC tree they were written against: trig_* need
# d48c485, fit needs 24a0b57 (both removed by the fix, 4a18fdf).  swap and xid
# patch t/033 only and run on any tree.  Results: doc/PHASES.md L22,
# bench/RESULTS_G75_RECLAIM.md.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
ARMS="${ARMS:-base swap}"
L22N="${L22N:-2}"
TESTS="${TESTS:-t/033_reclaim_crash_loop.pl t/015_alloc_outcomes.pl}"
log() { echo "$(date +%T) $*" | tee -a $OUT/l22.log; }

apply() {	# arm -> substitution in the current dir
	local f=src/am/amvacuum.c from to
	case $1 in
	base) return 0 ;;
	nodrop)	f=src/am/am.c; from='				if (ndropped < WEAVE_FSM_DROP_MAX &&'; to='				if (false && ndropped < WEAVE_FSM_DROP_MAX &&	/* ARM */' ;;
	nofit)	from='			 !weave_pack_fits_reusable(index)))'; to='			 !weave_any_free_page_recyclable(index)))	/* ARM */' ;;
	fit)	from='!weave_any_free_page_recyclable(index))'; to='!weave_l22_pack_fits(index))	/* ARM */' ;;
	swap)	f=t/033_reclaim_crash_loop.pl
			from="\$fill->('c', \$cyc) . '; ' . \$fill->('t', \$cyc));"
			to="\$fill->('t', \$cyc) . '; ' . \$fill->('c', \$cyc));	# ARM" ;;
	xid)	f=t/033_reclaim_crash_loop.pl
			from="safe_psql('postgres', \$fill->('c', \$cyc) . '; '"
			to="safe_psql('postgres', 'SELECT txid_current(); ' . \$fill->('c', \$cyc) . '; '	# ARM
				 . ''" ;;
	trig_pages)		from='if (nblocks > 16 && freeblks > nblocks / 4)'; to='if (nblocks > 16 && pgfree > nblocks / 4)	/* ARM */' ;;
	trig_reusable)	from='if (nblocks > 16 && freeblks > nblocks / 4)'; to='if (nblocks > 16 && reusable > nblocks / 4)	/* ARM */' ;;
	trig_off)		from='if (nblocks > 16 && freeblks > nblocks / 4)'; to='if (false && nblocks > 16 && freeblks > nblocks / 4)	/* ARM */' ;;
	*)
		if [ -n "${ARM_FROM_FILE:-}" ] && [ "$1" = "${ARM_NAME:-}" ]; then
			from=$(cat "$ARM_FROM_FILE"); to=$(cat "$ARM_TO_FILE")
		else
			echo "unknown arm $1"; return 2
		fi ;;
	esac
	FROM="$from" TO="$to" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "arm: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' $f || return 1
	echo "$f: $(grep -c 'ARM' $f) ARM line(s)"
}

install_tree() {	# dir tag
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}

ok=1
declare -A MD5
for arm in $ARMS; do
	D=/tmp/arm-$arm
	[ -d $D ] && find $D -depth -delete
	cp -a $SRC $D
	if ! (cd $D && apply $arm > $OUT/apply-$arm.log 2>&1); then
		log "$arm: DID NOT APPLY ($(cat $OUT/apply-$arm.log))"; ok=0; continue
	fi
	if ! install_tree $D $arm; then
		log "$arm: DID NOT BUILD ($(grep -m3 error $OUT/build-$arm.log))"; ok=0; continue
	fi
	md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	# a C arm must build a .so of its own; a test-only arm (swap, xid) must not
	case $(cat $OUT/apply-$arm.log) in *t/0*) testarm=1 ;; *) testarm=0 ;; esac
	if [ $testarm = 0 ]; then
		for a in "${!MD5[@]}"; do
			[ "${MD5[$a]}" = "$md5" ] && { log "$arm: .so identical to $a's -- arm not counted"; ok=0; }
		done
	fi
	MD5[$arm]=$md5
	for r in $(seq 1 $L22N); do
		tag=$arm-$r
		[ -d $D/tmp_check ] && find $D/tmp_check -depth -delete
		(cd $D && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
			PROVE_TESTS="$TESTS" > $OUT/tap-$tag.log 2>&1)
		rc=$?
		mkdir -p $OUT/$tag
		cp $D/tmp_check/log/* $OUT/$tag/ 2>/dev/null
		if ! grep -q '^Result: ' $OUT/tap-$tag.log; then log "$tag: RAN_NOTHING rc=$rc"; ok=0; continue; fi
		ex=$(grep -h 'excess of c_w over its twin' $OUT/$tag/regress_log_* 2>/dev/null | head -1)
		ncen=$(cat $OUT/$tag/*.log 2>/dev/null | grep -c 'L22 census')
		nnok=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep -c '^not ok')
		log "$tag so=$md5 rc=$rc $(grep '^Result: ' $OUT/tap-$tag.log) census_lines=$ncen not_ok=$nnok :: $ex"
	done
	find $D -depth -delete
done
log "DONE ok=$ok"
[ $ok = 1 ]
