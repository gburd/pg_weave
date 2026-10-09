#!/bin/bash
# L22 round 2, growth 1, job B: t/033's size bound is a hard assertion again.
# Smoke (run.sh) runs it once on the fix; here, twice more on the fix, and twice on
# mutant oldtrig (the pre-fix rule), which must FAIL the bound with its own .so.
set -u
cd $HOME/pg_weave
mkdir -p /tmp/out
ARMS="base oldtrig" L22N=2 TESTS=t/033_reclaim_crash_loop.pl bash bench/aws/l22_job.sh
for a in base oldtrig; do
	for d in /tmp/out/$a-*; do
		echo "T033 $(basename $d): $(grep -h '^.*not ok\|excess of c_w' $d/regress_log_* | cut -c1-300 | tr '\n' ' ')" | tee -a /tmp/out/l22.log
	done
done
exit 0
