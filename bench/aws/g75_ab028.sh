#!/bin/bash
# A/B(/C) for t/028 test 113 ("quiet plain VACUUM still truncates the index"),
# doc/GAPS.md G73: does the G75 branch fail it more often than its base does, and
# which part of the branch does it?  Arms alternate run by run so host drift hits
# all of them equally.  Arm "branch" is the shipped tree; every other arm is the
# shipped tree with src/ and include/ replaced by a gzipped tar (base64) the
# launcher writes to /tmp/g75_arm_<name>.b64 -- the tests stay the branch's, so
# the only difference between arms is the C code.  AB_ARMS lists the arms.
# Each run: fresh install, t/028 alone, logs kept; a run counts only if the log
# shows test 113 was REACHED, so a run that died early is not a pass.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
N=${AB_N:-10}
ARMS=${AB_ARMS:-branch base}
TEST=${AB_TEST:-t/028_vacuum_truncate_race.pl}	# the file to run
MARK=${AB_MARK:-113}							# the test number that decides
TB=$(basename $TEST .pl)
log() { echo "$(date +%T) $*" | tee -a $OUT/ab028.log; }

declare -A dir fails runs
for arm in $ARMS; do
	fails[$arm]=0; runs[$arm]=0
	if [ $arm = branch ]; then dir[$arm]=$SRC; continue; fi
	d=/tmp/ab-$arm
	[ -d $d ] && find $d -depth -delete
	cp -a $SRC $d
	b64=/tmp/g75_arm_$arm.b64
	[ -s $b64 ] || { log "no $b64"; exit 1; }
	(cd $d && find src include -depth -delete && base64 -d < $b64 | tar xzf -) \
		|| { log "could not unpack arm $arm"; exit 1; }
	diff -rq $SRC/src $d/src > $OUT/ab-srcdiff-$arm.txt 2>&1
	log "arm $arm differs from branch in $(grep -c . $OUT/ab-srcdiff-$arm.txt) path(s) under src/"
	[ -s $OUT/ab-srcdiff-$arm.txt ] || { log "arm $arm is IDENTICAL to branch -- refusing to run"; exit 1; }
	dir[$arm]=$d
done

install() {
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/ab-build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > /dev/null 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	md5sum $LIB/pg_weave.so | cut -d' ' -f1
}

for r in $(seq 1 $N); do
	for arm in $ARMS; do
		d=${dir[$arm]}
		md5=$(install $d $arm) || { log "$arm: build failed"; exit 1; }
		(cd $d && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
			PROVE_TESTS=$TEST > $OUT/ab-$TB-$arm-$r.log 2>&1)
		rl=$d/tmp_check/log/regress_log_$TB
		line=$(grep -E "(not )?ok $MARK " $rl 2>/dev/null | head -1)
		if [ -z "$line" ]; then log "$TB $arm run $r (so=$md5): test $MARK NOT REACHED"; continue; fi
		runs[$arm]=$((runs[$arm]+1))
		case "$line" in *"not ok"*)
			fails[$arm]=$((fails[$arm]+1))
			cp $rl $OUT/ab-$TB-$arm-$r.regress_log
			cp $d/tmp_check/log/${TB}_*.log $OUT/ 2>/dev/null ;;
		esac
		log "$TB $arm run $r (so=$md5): $(echo "$line" | grep -oE "(not ok|ok) $MARK .*" | cut -c1-200)"
		grep -h 'G73 trail\|excess of' $rl | sed "s/^/    $arm run $r: /" >> $OUT/ab028.log
	done
done
summary=""
for arm in $ARMS; do summary="$summary $arm: ${fails[$arm]} of ${runs[$arm]} failed $MARK;"; done
log "RESULT $TB$summary"
install $SRC branch-restore > /dev/null
