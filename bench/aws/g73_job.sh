#!/bin/bash
# doc/GAPS.md G73 EC2 job: bench/aws/g73_repro.pl, G73N clusters per C arm.
#   G73_CARMS  C arms (default "base"); each is a copy of the tree with ONE
#              exact-once substitution (apply() below), built and installed,
#              with a distinct .so md5 required
#   G73_ARMS   test-side arms cycled per round inside the reproducer
#   G73N       reproducer runs per C arm (default 4), G73_ROUNDS rounds each
# Logs: /tmp/out/<carm>-<n>/ (TAP log, regress log); summary /tmp/out/g73.log.
set -u
OUT=${G73_OUT:-/tmp/out}; mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
CARMS="${G73_CARMS:-base}"
N="${G73N:-4}"
log() { echo "$(date +%T) $*" | tee -a $OUT/g73.log; }

apply() {	# arm -> substitution in the current dir
	local f=src/am/am.c from to
	case $1 in
	base) return 0 ;;
	# the G73 fix removed (mutant): this VACUUM's own frees use up the window
	unfixed) from='		if (!ok && weave_page_freed_this_epoch(BufferGetPage(buf), nextxid))'
		to='		if (false && !ok && weave_page_freed_this_epoch(BufferGetPage(buf), nextxid))	/* ARM */' ;;
	# hypothesis (a) on the unfixed tree, and with the fix
	refresh) apply unfixed && apply refresh0; return $? ;;
	refreshfix) apply refresh0; return $? ;;
	ampfix) apply amp; return $? ;;
	# hypothesis (a): force the horizon recomputation nbtree's
	# _bt_pendingfsm_finalize() does, before the probe decides
	refresh0) from='	WeaveProbeDiag d = {0, 0, 0, InvalidTransactionId, InvalidTransactionId};
'
		to='	WeaveProbeDiag d = {0, 0, 0, InvalidTransactionId, InvalidTransactionId};
	(void) GetOldestNonRemovableTransactionId(NULL);	/* ARM */
' ;;
	# the probe-window hypothesis: no 256-candidate bound
	nobound) from='	for (blk = 1; blk < nblocks && probed < WEAVE_RECYCLE_PROBE_MAX; blk++)'
		to='	for (blk = 1; blk < nblocks; blk++)	/* ARM */' ;;
	# AMPLIFIER, not a fix: a 64-candidate window.  Run 1 measured the pass-0
	# probe passing at candidate 1..228 (typically 130-150), so 64 should make the
	# window-full shape the common case rather than a 1-in-15 one
	amp) from='#define WEAVE_RECYCLE_PROBE_MAX 256'; to='#define WEAVE_RECYCLE_PROBE_MAX 64	/* ARM */' ;;
	# L22 growth 2's reverted allocator fix (65bed24: skip a freed, not-yet-
	# recyclable FSM candidate and put it back), on top of this tree.  Its
	# revert (eb2dc03) was for t/028 3/10, whose failing shape is G73's.
	skip) git apply -R bench/aws/g73/skip.patch && echo "src/am/am.c: skip.patch applied (ARM)"; return $? ;;
	unfixedskip) apply skip && apply unfixed; return $? ;;
	ampunfixed) apply amp && apply unfixed; return $? ;;
	amprefresh) apply amp && apply refresh; return $? ;;
	ampnobound) apply amp && apply nobound; return $? ;;
	*)
		if [ -n "${ARM_FROM_FILE:-}" ] && [ "$1" = "${ARM_NAME:-}" ]; then
			f=${ARM_FILE:-src/am/am.c}; from=$(cat "$ARM_FROM_FILE"); to=$(cat "$ARM_TO_FILE")
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

install_tree() {
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}

ok=1
declare -A MD5
# Build every arm once and keep its .so; runs then swap the installed .so, so arms
# can be INTERLEAVED in blocks on one host (G73_BLOCKS) without a rebuild each.
# Only pg_weave.so differs between C arms; the install SQL is the tree's own.
BUILT=""
for arm in $CARMS; do
	D=/tmp/arm-$arm
	[ -d $D ] && find $D -depth -delete
	cp -a $SRC $D
	if ! (cd $D && apply $arm > $OUT/apply-$arm.log 2>&1); then
		log "$arm: DID NOT APPLY ($(cat $OUT/apply-$arm.log))"; ok=0; continue
	fi
	if ! install_tree $D $arm; then
		log "$arm: DID NOT BUILD ($(grep -m3 error $OUT/build-$arm.log))"; ok=0; continue
	fi
	cp $LIB/pg_weave.so /tmp/so-$arm.so
	md5=$(md5sum /tmp/so-$arm.so | cut -d' ' -f1)
	dup=0
	for a in "${!MD5[@]}"; do
		[ "${MD5[$a]}" = "$md5" ] && { log "$arm: .so identical to $a's -- arm not counted"; ok=0; dup=1; }
	done
	[ $dup = 1 ] && continue
	MD5[$arm]=$md5
	log "$arm: built, so=$md5 ($(cat $OUT/apply-$arm.log | tr '\n' ' '))"
	BUILT="$BUILT $arm"
done
TESTS="${G73_TESTS:-bench/aws/g73_repro.pl}"
SHAPES="${G73_SHAPES:-5:3000}"	# storm seconds:CREATE INDEX rows
for blk in $(seq 1 ${G73_BLOCKS:-1}); do
for arm in $BUILT; do
	D=/tmp/arm-$arm
	sudo install -m 755 /tmp/so-$arm.so $LIB/pg_weave.so
	md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	[ "$md5" = "${MD5[$arm]}" ] || { log "$arm: installed .so is not the arm's"; ok=0; continue; }
	for shape in $SHAPES; do
	secs=${shape%%:*}; rows=${shape##*:}
	for r in $(seq 1 $N); do
		tag=$arm-s$secs-r$rows-b$blk-$r
		[ -d $D/tmp_check ] && find $D/tmp_check -depth -delete
		(cd $D && G73_SECS=$secs G73_BASEROWS=$rows make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
			PROVE_TESTS="$TESTS" > $OUT/tap-$tag.log 2>&1)
		rc=$?
		mkdir -p $OUT/$tag
		cp $D/tmp_check/log/* $OUT/$tag/ 2>/dev/null
		if ! grep -q '^Result: ' $OUT/tap-$tag.log; then log "$tag: RAN_NOTHING rc=$rc"; ok=0; continue; fi
		nr=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep -c 'G73R ')
		nnok=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep '^not ok' | tr '\n' ' ')
		probe=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep -c 'G73 trail.*no free page recyclable yet')
		# evidence the thing meant to run, ran: reproducer rounds, or t/028's control line
		ctl=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep -c 'quiet plain VACUUM still truncates the index')
		case $TESTS in
		*g73_repro*) [ "$nr" -gt 0 ] || { log "$tag: RAN_NOTHING (no G73R rounds)"; ok=0; } ;;
		*028*) [ "$ctl" -gt 0 ] || { log "$tag: RAN_NOTHING (no t/028 control line)"; ok=0; } ;;
		esac
		log "$tag so=$md5 rc=$rc $(grep '^Result: ' $OUT/tap-$tag.log) rounds=$nr ctl=$ctl probe_decline_trail_lines=$probe not_ok=[$nnok] $(cat $OUT/$tag/regress_log_* 2>/dev/null | grep 'G73T ' | sed 's/.*# //' | tr '\n' ' ')"
	done
	done
done
done
log "TOTALS (reproducer rounds):"
for d in $OUT/*/; do t=$(basename $d); c=${t%%-s[0-9]*}; cat $d/regress_log_* 2>/dev/null | grep 'G73R ' |
	sed "s/.*arm=\([a-z]*\) secs=\([0-9]*\) rows=\([0-9]*\).*result=\([A-Z]*\).*/$c \2s \3r \1 \4/"; done | sort | uniq -c | tee -a $OUT/g73.log
log "TOTALS (TAP runs):"
for arm in $BUILT; do
	n=$(grep -c " $arm-s.* so=.*Result: FAIL" $OUT/g73.log); t=$(grep -c " $arm-s.* so=" $OUT/g73.log)
	log "TAP $arm: $n of $t failed"
done
log "DONE ok=$ok"
[ $ok = 1 ]
