#!/usr/bin/env bash
# Run on the dev host, in ~/pg_weave.  Builds, installs and runs installcheck for
# one major, taking every status directly.  Usage: devgate.sh <17|18> [PROVE_TESTS]
set -u
v=$1; shift
tests=${1:-}
export PG_CONFIG=/usr/lib/postgresql/$v/bin/pg_config
export PATH=/usr/lib/postgresql/$v/bin:$PATH
export PGPORT=$(pg_lsclusters -h | awk -v v="$v" '$1==v && $2=="main"{print $3}')
out=/tmp/out/gate-$v; mkdir -p "$out"
cd ~/pg_weave
fail() { echo "GATE FAIL pg$v: $1"; tail -40 "$2" 2>/dev/null; exit 1; }
make clean PG_CONFIG="$PG_CONFIG" >/dev/null 2>&1
make -s with_llvm=no PG_CONFIG="$PG_CONFIG" >"$out/build.log" 2>&1 || fail build "$out/build.log"
sudo make install with_llvm=no PG_CONFIG="$PG_CONFIG" >"$out/install.log" 2>&1 || fail install "$out/install.log"
# A bitcode tree left by any earlier install makes PG17's JIT kill backends (AGENTS.md, tenth member).
sudo find /usr/lib/postgresql/$v/lib/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
sudo pg_ctlcluster "$v" main restart || fail restart /dev/null
actual=$(psql -XAtc 'show server_version_num' postgres)
[ "$((actual / 10000))" = "$v" ] || fail "port $PGPORT is server $actual, not $v" /dev/null
sha256sum pg_weave.so > "$out/library.sha256"
if [ -n "$tests" ]; then
	make installcheck PG_CONFIG="$PG_CONFIG" REGRESS= ISOLATION= PROVE_TESTS="$tests" >"$out/installcheck.log" 2>&1
else
	make installcheck PG_CONFIG="$PG_CONFIG" >"$out/installcheck.log" 2>&1
fi
rc=$?
cp regression.diffs "$out/" 2>/dev/null || : > "$out/regression.diffs"
grep -E '^(# All|# [0-9]+ of|ok|not ok|All tests|Files=|Result:|t/)' "$out/installcheck.log" | tail -60 > "$out/summary.log"
[ "$rc" = 0 ] || fail "installcheck rc=$rc" "$out/installcheck.log"
echo "GATE PASS pg$v $(grep -E '^Files=' "$out/installcheck.log" | tail -1)"
