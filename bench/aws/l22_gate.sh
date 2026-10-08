#!/bin/bash
# L22 gate on EC2 (doc/PHASES.md L22): bench/aws/g75_job.sh's control (t/029,
# t/031, t/032, t/033, t/015, twice), its mutants (nofit must fail t/033), the
# 1M-row scale run with a never-crashed twin, and the same scale run on the
# nofit mutant (expected to FAIL the twin bound: the positive control); then
# t/033 with the insert order swapped (bench/aws/l22_job.sh arm swap).
set -u
cd $HOME/pg_weave
PHASES="${PHASES:-control mutants scale scalemut}" G75_CYCLES=${G75_CYCLES:-14} \
	bash bench/aws/g75_job.sh
rc=$?
ARMS=swap L22N=1 TESTS=t/033_reclaim_crash_loop.pl bash bench/aws/l22_job.sh
rc2=$?
echo "L22 GATE g75_job=$rc swap=$rc2" | tee -a /tmp/out/l22.log
[ $rc = 0 ] && [ $rc2 = 0 ]
