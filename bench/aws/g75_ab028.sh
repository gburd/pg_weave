#!/bin/bash
# A/B for t/028 test 113 ("quiet plain VACUUM still truncates the index"),
# doc/GAPS.md G73: does the G75 branch fail it more often than its base does?
# Arms alternate run by run so host drift hits both equally.  Arm "base" is this
# tree with src/ and include/ replaced by the merge base's (the tests stay the
# branch's, so the only difference is the C code).  The shipped tree has no
# history, so the base's C code travels as a gzipped tar of
# `git archive <base> src include`, base64-encoded
# (the launcher writes it to AB_B64, default /tmp/g75_ab028.base.b64).  Each run: fresh install, t/028 alone, the regress log
# kept; a run counts only if the log shows test 113 was REACHED, so a run that
# died early is not counted as a pass.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
N=${AB_N:-10}
log() { echo "$(date +%T) $*" | tee -a $OUT/ab028.log; }

BASEARM=/tmp/ab-base
[ -d $BASEARM ] && find $BASEARM -depth -delete
cp -a $SRC $BASEARM
B64=${AB_B64:-/tmp/g75_ab028.base.b64}
[ -s $B64 ] || { log "no $B64"; exit 1; }
(cd $BASEARM && find src include -depth -delete && base64 -d < $B64 | tar xzf -) \
	|| { log "could not unpack the base tree"; exit 1; }
diff -rq $SRC/src $BASEARM/src > $OUT/ab-srcdiff.txt 2>&1
log "base arm differs from branch in $(grep -c . $OUT/ab-srcdiff.txt) path(s) under src/"
[ -s $OUT/ab-srcdiff.txt ] || { log "the two arms are IDENTICAL -- refusing to run"; exit 1; }

install() {
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/ab-build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > /dev/null 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	md5sum $LIB/pg_weave.so | cut -d' ' -f1
}

declare -A fails runs
for arm in branch base; do fails[$arm]=0; runs[$arm]=0; done
for r in $(seq 1 $N); do
	for arm in branch base; do
		d=$SRC; [ $arm = base ] && d=$BASEARM
		md5=$(install $d $arm) || { log "$arm: build failed"; exit 1; }
		(cd $d && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
			PROVE_TESTS=t/028_vacuum_truncate_race.pl > $OUT/ab-$arm-$r.log 2>&1)
		rl=$d/tmp_check/log/regress_log_028_vacuum_truncate_race
		line=$(grep -E '(not )?ok 113 ' $rl 2>/dev/null | head -1)
		if [ -z "$line" ]; then log "$arm run $r (so=$md5): test 113 NOT REACHED"; continue; fi
		runs[$arm]=$((runs[$arm]+1))
		case "$line" in *"not ok"*)
			fails[$arm]=$((fails[$arm]+1))
			cp $rl $OUT/ab-$arm-$r.regress_log
			cp $d/tmp_check/log/028_vacuum_truncate_race_primary.log $OUT/ab-$arm-$r.server_log 2>/dev/null ;;
		esac
		log "$arm run $r (so=$md5): $(echo "$line" | grep -oE '(not ok|ok) 113 .*')"
	done
done
log "RESULT branch: ${fails[branch]} of ${runs[branch]} failed 113; base: ${fails[base]} of ${runs[base]}"
install $SRC branch-restore > /dev/null
