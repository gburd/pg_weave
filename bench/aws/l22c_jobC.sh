#!/bin/bash
# L22 round 2, growth 1, job C: t/028's truncation control at 30 runs per arm,
# fix (base) vs the pre-fix rule (oldtrig), interleaved in blocks of 5 so host drift
# hits both arms alike, with every run's post-DELETE trail kept; then t/033's hard
# bound twice on the fix and twice on oldtrig (must FAIL).
set -u
cd $HOME/pg_weave
mkdir -p /tmp/out
for blk in 1 2 3 4 5 6; do
	ARMS="base oldtrig" L22PFX=b$blk L22N=5 TESTS=t/028_vacuum_truncate_race.pl bash bench/aws/l22_job.sh
done
for a in base oldtrig; do
	n=$(grep -cE " b[0-9]$a-[0-9]+ .*Result: FAIL" /tmp/out/l22.log); t=$(grep -cE " b[0-9]$a-[0-9]+ so=" /tmp/out/l22.log)
	echo "T028 $a: $n of $t failed" | tee -a /tmp/out/l22.log
	for d in /tmp/out/b[0-9]$a-*; do
		f=$(grep -l "not ok" $d/regress_log_* 2>/dev/null | head -1)
		echo "TRAIL $(basename $d) $([ -n "$f" ] && echo FAILED || echo ok): $(grep -h 'G73 trail: after VACUUM 1' $d/regress_log_* | head -1 | sed 's/.*after VACUUM 1: //' | cut -c1-700)" >> /tmp/out/l22.log
	done
done
grep -h "TRAIL.*FAILED" /tmp/out/l22.log | head
for d in /tmp/out/b[0-9]*-*; do grep -q "not ok" $d/regress_log_* 2>/dev/null && grep -h "G73 trail" $d/regress_log_* | sed "s|^|$(basename $d) |" >> /tmp/out/failed_trails.log; done
ARMS="base oldtrig" L22PFX=t L22N=2 TESTS=t/033_reclaim_crash_loop.pl bash bench/aws/l22_job.sh
for d in /tmp/out/t*-*; do
	echo "T033 $(basename $d): $(grep -h 'not ok\|excess of c_w' $d/regress_log_* | cut -c1-300 | tr '\n' ' ')" | tee -a /tmp/out/l22.log
done
# the 1M-row crash-loop scale run with the never-crashed twin, on oldtrig then on the
# fix, sequentially on this host (one .so installed at a time; L22N=0 = build+install
# only).  Its stops-growing assertion is EXPECTED to fail on growth 2 in both arms;
# the gate is the fix's per-cycle excess against oldtrig's.
for a in oldtrig base; do
	ARMS=$a L22N=0 bash bench/aws/l22_job.sh > /dev/null 2>&1
	so=$(md5sum /usr/lib/postgresql/17/lib/pg_weave.so | cut -d' ' -f1)
	mkdir -p /tmp/out/scale-$a
	OUT=/tmp/out/scale-$a G75_TAG=$a G75_CYCLES=14 bash bench/aws/g75_scale.sh > /tmp/out/scale-$a/scale.log 2>&1
	echo "SCALE $a so=$so exit=$? $(grep -hE '(SCALE twin|RESULT)' /tmp/out/scale-$a/scale_progress.log | tr '\n' ' ')" | tee -a /tmp/out/l22.log
done
grep -E "^T028|^T033|^SCALE|DID NOT|identical" /tmp/out/l22.log
exit 0
