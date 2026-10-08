#!/bin/bash
# G75 EC2 job: control (the clean tree's G75 TAP files, twice), mutants (each
# must BUILD and install a different .so before its failure counts), then the
# 1M-row scale run.  Results under /tmp/out.  Exit 0 iff every control passed
# and every mutant was caught.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
PHASES="${PHASES:-control mutants scale}"
MUTS="${MUTS:-noreclaim:t/031_doclist_atomic.pl noguard:t/032_reclaim_concurrent.pl nobarrier:t/032_reclaim_concurrent.pl nofence:t/032_reclaim_concurrent.pl nofit:t/033_reclaim_crash_loop.pl}"
log() { echo "$(date +%T) $*" | tee -a $OUT/g75.log; }

cat > $OUT/apply.sh <<'MUTEOF'
#!/bin/bash
set -e
m=$1
f=src/am/amvacuum.c
sub() {	# file, from (literal), to (literal): must match exactly once
	FROM="$2" TO="$3" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "mutant: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' "$1"
}
case $m in
noreclaim)
	sub $f '		weave_free_page_locked(index, buf, false);' '		UnlockReleaseBuffer(buf);	/* MUTANT noreclaim */
		continue;' ;;
nobarrier)
	sub $f '	weave_segwrite_barrier(index);
	return fence;' '	/* MUTANT nobarrier */
	return fence;' ;;
nofence)
	sub $f '		if (PageGetLSN(page) > fence)' '		if (false && PageGetLSN(page) > fence)	/* MUTANT nofence */' ;;
noguard)
	"$0" nobarrier; "$0" nofence ;;
noskip)	# L22: the FSM loop re-queues a freed, not-yet-recyclable page and stops again
	sub src/am/am.c '				if (nskipped < WEAVE_FSM_SKIP_MAX &&' '				if (false && nskipped < WEAVE_FSM_SKIP_MAX &&	/* MUTANT noskip */' ;;
nofit)	# L22: the share-lock pass is gated by the recyclability probe alone again
	sub $f '			 !weave_pack_fits_reusable(index)))' '			 !weave_any_free_page_recyclable(index)))	/* MUTANT nofit */' ;;
*) echo "unknown mutant $m"; exit 2 ;;
esac
grep -c MUTANT $f src/am/am.c
MUTEOF
chmod +x $OUT/apply.sh

install_tree() {	# dir tag -> 0 on success
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}

# run_tap dir tag tests... -> prints "files=N fail=M" from prove's own summary;
# "RAN_NOTHING" if prove never printed its Result line
run_tap() {
	local d=$1 tag=$2; shift 2
	(cd "$d" && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
		PROVE_TESTS="$*" > $OUT/tap-$tag.log 2>&1)
	local rc=$?
	mkdir -p $OUT/taplog-$tag
	cp "$d"/tmp_check/log/regress_log_* $OUT/taplog-$tag/ 2>/dev/null
	if ! grep -q '^Result: ' $OUT/tap-$tag.log; then echo RAN_NOTHING; return; fi
	echo "rc=$rc $(grep -E '^Result: ' $OUT/tap-$tag.log) $(grep -cE '^not ok' $OUT/taplog-$tag/* 2>/dev/null | awk -F: '{s+=$2} END {print "not_ok=" s}')"
}

ok=1
CLEAN_MD5=

# the smoke that ran before this script left its TAP logs in the tree: keep them
if [[ " $PHASES " == *" smokelogs "* ]]; then
	mkdir -p $OUT/smoke-taplog
	cp $SRC/tmp_check/log/* $OUT/smoke-taplog/ 2>/dev/null
	log "SMOKELOGS: copied $(ls $OUT/smoke-taplog | wc -l) files"
fi

# TAPS (a list) run TAPN times each on the clean install, logs kept per run
if [[ " $PHASES " == *" tap "* ]]; then
	install_tree "$SRC" clean || { log "TAP build/install FAILED"; exit 1; }
	for t in ${TAPS:-}; do
		for r in $(seq 1 ${TAPN:-1}); do
			res=$(run_tap "$SRC" "$(basename $t .pl)-$r" $t)
			log "TAP $t run $r: $res"
			case "$res" in *"Result: PASS"*) ;; *) ok=0 ;; esac
		done
	done
fi

if [[ " $PHASES " == *" control "* ]]; then
	install_tree "$SRC" clean || { log "CONTROL build/install FAILED"; exit 1; }
	CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	for r in 1 2; do
		res=$(run_tap "$SRC" control$r t/029_flush_atomic.pl t/031_doclist_atomic.pl t/032_reclaim_concurrent.pl t/033_reclaim_crash_loop.pl t/015_alloc_outcomes.pl)
		log "CONTROL run $r (so=$CLEAN_MD5): $res"
		case "$res" in *"Result: PASS"*) ;; *) ok=0 ;; esac
	done
fi

if [[ " $PHASES " == *" mutants "* ]]; then
	[ -n "$CLEAN_MD5" ] || { install_tree "$SRC" clean; CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1); }
	# which files each mutant must fail: noreclaim -> t/031 (the brief's gate);
	# the guard mutants -> t/032 (the concurrency test)
	for spec in $MUTS; do
		m=${spec%%:*}; tests=${spec#*:}
		D=/tmp/mut-$m
		rm -rf $D; cp -a $SRC $D
		if ! (cd $D && bash $OUT/apply.sh $m > $OUT/apply-$m.log 2>&1); then
			log "$m: DID NOT APPLY ($(cat $OUT/apply-$m.log)) -- not counted"; ok=0; continue
		fi
		if ! install_tree $D $m; then
			log "$m: DID NOT BUILD/INSTALL -- not counted ($(grep -m3 error $OUT/build-$m.log))"; ok=0; continue
		fi
		md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
		if [ "$md5" = "$CLEAN_MD5" ]; then log "$m: installed .so is the CLEAN one -- not counted"; ok=0; continue; fi
		res=$(run_tap $D mut-$m $tests)
		case "$res" in
		RAN_NOTHING) log "$m: the TAP run printed no Result -- not counted"; ok=0 ;;
		*"Result: FAIL"*) log "$m: CAUGHT by $tests (so=$md5): $res" ;;
		*) log "$m: SURVIVED $tests (so=$md5): $res"; ok=0 ;;
		esac
		rm -rf $D
	done
	install_tree "$SRC" clean-restore > /dev/null 2>&1
	[ "$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)" = "$CLEAN_MD5" ] || log "WARNING: clean .so not restored"
fi

if [[ " $PHASES " == *" scale "* ]]; then
	if [ -f $SRC/bench/aws/g75_scale.sh ]; then
		bash $SRC/bench/aws/g75_scale.sh > $OUT/scale.log 2>&1
		src=$?
		log "SCALE exit=$src: $(grep -hE '(SCALE twin|SCALE end|RESULT)' $OUT/scale_progress.log | tr '\n' ' ')"
		[ $src = 0 ] || ok=0
	else
		log "SCALE: no bench/aws/g75_scale.sh in this tree"; ok=0
	fi
fi

# the scale run on a MUTANT (SCALEMUT=<name>): the positive control for the
# scale run's twin bound; expected to FAIL it
if [[ " $PHASES " == *" scalemut "* ]]; then
	m=${SCALEMUT:-nofit}; D=/tmp/mut-$m
	[ -d $D ] && find $D -depth -delete
	cp -a $SRC $D
	if (cd $D && bash $OUT/apply.sh $m > $OUT/apply-scale-$m.log 2>&1) && install_tree $D scale-$m; then
		md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
		[ -n "$CLEAN_MD5" ] && [ "$md5" = "$CLEAN_MD5" ] && { log "SCALEMUT $m: .so is the CLEAN one"; ok=0; }
		mkdir -p $OUT/scalemut; OUT=$OUT/scalemut G75_REF=0 bash $SRC/bench/aws/g75_scale.sh > $OUT/scalemut/scale.log 2>&1
		src=$?
		# expected to FAIL; a pass means the scale run cannot see this mutant
		[ $src = 0 ] && log "SCALEMUT $m: SURVIVED the scale run"
		log "SCALEMUT $m (so=$md5) exit=$src: $(grep -hE '(SCALE twin|RESULT)' $OUT/scalemut/scale_progress.log | tr '\n' ' ')"
	else
		log "SCALEMUT $m: did not apply/build -- not counted"; ok=0
	fi
	find $D -depth -delete
	install_tree "$SRC" clean-restore2 > /dev/null 2>&1
fi

log "DONE ok=$ok"
[ $ok = 1 ]
