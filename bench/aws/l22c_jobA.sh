#!/bin/bash
# L22 round 2, growth 1 (doc/PHASES.md L22), job A: t/028's truncation control,
# 10 runs per arm -- base (the fix: a share-lock pass over several tombstone-free
# bolts runs only if the FSM predicts it shrinks the file) vs oldtrig (the pre-fix
# rule, built as an exact-once substitution with its own .so) -- then t/033 and
# t/015 twice on the fix.  Each t/028 run's G73 trail now carries the cleanup
# trigger's inputs and decision (DEBUG2) for the three post-DELETE VACUUMs.
set -u
cd $HOME/pg_weave
mkdir -p /tmp/out
ARMS="base oldtrig" L22N=${N028:-10} TESTS=t/028_vacuum_truncate_race.pl bash bench/aws/l22_job.sh
for a in base oldtrig; do
	n=$(grep -c " $a-[0-9]* .*Result: FAIL" /tmp/out/l22.log); t=$(grep -c " $a-[0-9]* so=" /tmp/out/l22.log)
	echo "T028 $a: $n of $t failed" | tee -a /tmp/out/l22.log
	for d in /tmp/out/$a-*; do
		echo "TRAIL $(basename $d): $(grep -h 'G73 trail: after VACUUM 1' $d/regress_log_* | sed 's/.*after VACUUM 1: //' | cut -c1-600)" >> /tmp/out/l22.log
	done
done
ARMS=base L22PFX=g L22N=2 TESTS="t/033_reclaim_crash_loop.pl t/015_alloc_outcomes.pl" bash bench/aws/l22_job.sh
grep -E "T028|g?base-[0-9]+ so=" /tmp/out/l22.log | tail -24
exit 0
