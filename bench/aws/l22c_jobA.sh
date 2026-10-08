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
grep -q "T028 base: 0 of 10 failed" /tmp/out/l22.log || { echo "STOP: the fix fails t/028" | tee -a /tmp/out/l22.log; exit 1; }
ARMS=base L22PFX=g L22N=2 TESTS="t/033_reclaim_crash_loop.pl t/015_alloc_outcomes.pl" bash bench/aws/l22_job.sh
# the step-1 workloads on the fix (l22_job.sh's last arm, base, is what is installed)
echo "INSTALLED so=$(md5sum /usr/lib/postgresql/17/lib/pg_weave.so | cut -d' ' -f1)" | tee -a /tmp/out/l22.log
mkdir -p /tmp/out/ord /tmp/out/scale
# the 1M-row crash-loop scale run with its never-crashed twin, concurrently (own port)
(OUT=/tmp/out/scale G75_CYCLES=14 bash bench/aws/g75_scale.sh > /tmp/out/scale/scale.log 2>&1
 echo "SCALE fix exit=$? $(grep -hE '(SCALE twin|RESULT)' /tmp/out/scale/scale_progress.log | tr '\n' ' ')" | tee -a /tmp/out/l22.log) &
OUT=/tmp/out/ord bash bench/aws/l22_ordinary.sh > /tmp/out/ord/run.log 2>&1
echo "ORD fix exit=$? $(grep ASSERT /tmp/out/ord/summary.log | tr '\n' ' ')" | tee -a /tmp/out/l22.log
wait
# the positive control: mutant oldtrig on arm c must FAIL the assertion
cat > /tmp/mutc.sh <<'X'
mkdir -p /tmp/out/ordmut
OUT=/tmp/out/ordmut RUNS=c1 REFKINDS=c CYC=${MUTCYC:-10} bash $HOME/pg_weave/bench/aws/l22_ordinary.sh > /tmp/out/ordmut/run.log 2>&1
echo "ORD oldtrig exit=$? so=$(md5sum /usr/lib/postgresql/17/lib/pg_weave.so | cut -d' ' -f1) $(grep ASSERT /tmp/out/ordmut/summary.log | tr '\n' ' ')" | tee -a /tmp/out/l22.log
X
ARMS=oldtrig L22PFX=c L22N=1 TESTS=t/015_alloc_outcomes.pl bash bench/aws/l22_job.sh   # builds+installs the mutant
bash /tmp/mutc.sh
grep -E "T028|base-[0-9]+ so=|ORD|STOP|INSTALLED" /tmp/out/l22.log | tail -30
exit 0
