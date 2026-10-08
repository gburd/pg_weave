#!/bin/bash
# L22: t/028's truncation control, N runs per arm, logs kept (the smoke keeps none).
#   base   both L22 fixes      noskip  the first fix only
#   nofit  the second fix only pre     neither (the pre-L22 allocator and trigger)
cd $HOME/pg_weave
ARMS="${ARMS:-base noskip nofit pre}" L22N=${N028:-10} TESTS=t/028_vacuum_truncate_race.pl \
	bash bench/aws/l22_job.sh
for a in ${ARMS:-base noskip nofit pre}; do
	n=$(grep -c "^.* $a-[0-9]* .*Result: FAIL" /tmp/out/l22.log); t=$(grep -c " $a-[0-9]* so=" /tmp/out/l22.log)
	echo "T028 $a: $n of $t failed" | tee -a /tmp/out/l22.log
done
exit 0
