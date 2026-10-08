#!/bin/bash
# L22 gate, second fix (doc/PHASES.md L22).  On one host, in order:
#   1. t/028 ten times on the fix and ten on the nodrop arm (the allocator
#      change G75 retracted failed t/028's truncation control 3 in 10; this one
#      touches the same loop, so it gets the same A/B)
#   2. bench/aws/g75_job.sh: control twice (t/029 t/031 t/032 t/033 t/015), the
#      mutants (nofit -> t/033), the 1M-row scale run with its twin and the
#      no-growth assertion, and the scale run on the nodrop mutant (the positive
#      control for that assertion: expected to FAIL it)
set -u
cd $HOME/pg_weave
ARMS="base nodrop" L22N=${N028:-10} TESTS=t/028_vacuum_truncate_race.pl bash bench/aws/l22_job.sh
rc1=$?
MUTS="${MUTS:-noreclaim:t/031_doclist_atomic.pl nofence:t/032_reclaim_concurrent.pl nofit:t/033_reclaim_crash_loop.pl}" \
PHASES="${PHASES:-control mutants scale scalemut}" SCALEMUT=nodrop G75_CYCLES=${G75_CYCLES:-14} \
	bash bench/aws/g75_job.sh
rc2=$?
echo "L22 GATE2 t028_ab=$rc1 g75_job=$rc2" | tee -a /tmp/out/l22.log
exit $rc2
