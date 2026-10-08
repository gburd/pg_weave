#!/bin/bash
# L22 gate, second fix (doc/PHASES.md L22).  On one host, in order (the result
# that matters most first):
#   1. the 1M-row scale run with its twin and the no-growth assertion
#   2. t/028 ten times on the fix and ten on the noskip arm (the allocator
#      change G75 retracted failed t/028's truncation control 3 in 10; this one
#      touches the same loop, so it gets the same A/B)
#   3. bench/aws/g75_job.sh control twice (t/029 t/031 t/032 t/033 t/015) and
#      the mutants (nofit -> t/033)
#   4. the scale run on the noskip mutant: the positive control for the
#      no-growth assertion, expected to FAIL it
set -u
cd $HOME/pg_weave
PHASES=scale G75_REF=${G75_REF:-0} G75_CYCLES=${G75_CYCLES:-14} bash bench/aws/g75_job.sh
rc0=$?
mkdir -p /tmp/out/scale1 && mv /tmp/out/scale*.log /tmp/out/scale_post* /tmp/out/scale_vac* /tmp/out/scale_quiet* /tmp/out/scale1/ 2>/dev/null
ARMS="base noskip" L22N=${N028:-10} TESTS=t/028_vacuum_truncate_race.pl bash bench/aws/l22_job.sh
rc1=$?
MUTS="${MUTS:-noreclaim:t/031_doclist_atomic.pl nofence:t/032_reclaim_concurrent.pl nofit:t/033_reclaim_crash_loop.pl}" \
PHASES="${PHASES:-control mutants scalemut}" SCALEMUT=noskip G75_CYCLES=${G75_CYCLES:-14} \
	bash bench/aws/g75_job.sh
rc2=$?
echo "L22 GATE2 scale=$rc0 t028_ab=$rc1 g75_job=$rc2" | tee -a /tmp/out/l22.log
[ $rc0 = 0 ] && [ $rc2 = 0 ]
