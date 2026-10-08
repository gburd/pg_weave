#!/bin/bash
# L22 round 2, job 2: (1) the weave_compact_to_one() damaged-bolt skip's gate:
# t/035 on the tip, then mutant `noskip35` (compact_to_one never records a damaged
# bolt, the pre-fix stop) which must FAIL t/035, with a distinct .so;
# (2) step-1 arm b (autovacuum on) to 30 cycles, two runs: does it saw back?
set -u
cd $HOME/pg_weave
mkdir -p /tmp/out
cat > /tmp/noskip.from <<'X'
	sk.n = 0;
	PG_TRY();
X
cat > /tmp/noskip.to <<'X'
	memset(&sk, 0, sizeof(sk)); sk.n = WEAVE_MAX_SEGMENTS;	/* ARM: list full, so a damaged bolt stops both loops (pre-fix) */
	PG_TRY();
X
ARMS="base noskip35" L22N=1 TESTS=t/035_merge_freed_page.pl ARM_NAME=noskip35 \
	ARM_FROM_FILE=/tmp/noskip.from ARM_TO_FILE=/tmp/noskip.to bash bench/aws/l22_job.sh
for a in base noskip35; do
	echo "T035 $a: $(grep " $a-1 so=" /tmp/out/l22.log)" | tee -a /tmp/out/l22.log
	grep -h "^not ok" /tmp/out/$a-1/regress_log_* 2>/dev/null | head -5 | tee -a /tmp/out/l22.log
done
# reinstall the tip for arm b
make -s clean >/dev/null 2>&1; make -s with_llvm=no PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1 &&
	sudo make install with_llvm=no PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1 || { echo "reinstall failed"; exit 1; }
sudo find /usr/lib/postgresql/17/lib/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
mkdir -p /tmp/out/b30
OUT=/tmp/out/b30 RUNS="b1 b2" REFKINDS=ab CYC=${CYC:-30} bash bench/aws/l22_ordinary.sh
exit 0
