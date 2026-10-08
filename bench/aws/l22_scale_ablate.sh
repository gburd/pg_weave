#!/bin/bash
# L22 second growth at scale: bench/aws/g75_scale.sh in four arms AT ONCE on one
# host (same .so), each on its own cluster, port and output directory.
#   base   as the gate ran it
#   swap   the twin is inserted first (G75_ORDER=us)
#   ckpt   CHECKPOINT after each cycle's VACUUMs (G75_CKPT=1)
#   burn   one xid spent before the INSERT into s (G75_BURN=1)
#   drop   the allocator's FSM loop drops a freed, not-yet-recyclable candidate and
#          continues instead of re-recording it and extending the rest (the
#          temporary GUC pg_weave.l22_drop_deferred; its LOG line is the evidence
#          that it fired, grep scale_server.log)
# checkpoint_timeout is 1h in every arm, so no arm gets a timed checkpoint the
# others do not (parallel load stretches the cycles).
set -u
cd $HOME/pg_weave
CYC=${CYC:-12}
ARMS="${ARMS:-base swap ckpt burn drop}"
port=5500
for a in $ARMS; do
	port=$((port + 1))
	env=""
	case $a in
	swap) env="G75_ORDER=us" ;;
	ckpt) env="G75_CKPT=1" ;;
	burn) env="G75_BURN=1" ;;
	drop) env="G75_EXTRA_CONF=pg_weave.l22_drop_deferred=on" ;;
	esac
	mkdir -p /tmp/out/$a
	env $env OUT=/tmp/out/$a G75_TAG=$a G75_PORT=$port G75_CYCLES=$CYC G75_CKPT_TIMEOUT=1h \
		bash bench/aws/g75_scale.sh > /tmp/out/$a/scale.log 2>&1 &
	sleep 5
done
wait
for a in $ARMS; do
	echo "== $a: $(grep -E '^.*(SCALE twin|RESULT)' /tmp/out/$a/scale_progress.log | tr '\n' ' ') drop_fired=$(grep -c 'L22 drop_deferred fired' /tmp/out/$a/scale_server.log)"
done | tee /tmp/out/ablate_summary.log
exit 0
