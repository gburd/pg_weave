#!/bin/bash
# M7 step 2 gate: re-run the step-1 harness on the final commit, scifact + one
# Wikipedia part (part 6 whole, first 20,000 articles = step-1 run 1's sample), TWICE
# (hard rule 10).  Each run's /tmp/out is moved to /tmp/out/runN before the next.
set -u
cd ~/pg_weave
mkdir -p /tmp/out
rc=0
for r in 1 2; do
	BEIR=scifact WIKIS="wiki6|6.xml-p958046p1483661|20000|0" bash bench/tsvcaps_job.sh \
		> /tmp/out/tsvcaps-run$r.log 2>&1 || rc=$?
	mkdir -p /tmp/out/run$r
	find /tmp/out -maxdepth 1 -type f ! -name 'tsvcaps-run*.log' -exec mv {} /tmp/out/run$r/ \;
	[ -d /tmp/out/runs ] && mv /tmp/out/runs /tmp/out/run$r/runs
	dropdb tsvcaps 2>/dev/null
	echo "run $r exit $rc"
done
exit $rc
