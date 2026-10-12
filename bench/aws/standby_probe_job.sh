#!/usr/bin/env bash
# Disposable EC2 ONLY. No production install or ledger implementation.
set -euo pipefail
OUT=/tmp/out/standby-probe
mkdir -p "$OUT"
sudo apt-get -qq update >"$OUT/apt.log" 2>&1
sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y postgresql-18 postgresql-server-dev-18 >>"$OUT/apt.log" 2>&1
cp -a . /tmp/standby-probe-src
cd /tmp/standby-probe-src
python3 bench/aws/standby_probe_patch.py
cp src/am/am.c "$OUT/experimental-am.c"
cp src/am/amscan.c "$OUT/experimental-amscan.c"
for major in 17 18; do
  PGC=/usr/lib/postgresql/$major/bin/pg_config
  make clean PG_CONFIG="$PGC" with_llvm=no >"$OUT/clean-$major.log" 2>&1
  if ! make -j4 PG_CONFIG="$PGC" with_llvm=no >"$OUT/build-$major.log" 2>&1; then
    tail -80 "$OUT/build-$major.log"; exit 1
  fi
  sudo make install PG_CONFIG="$PGC" with_llvm=no >"$OUT/install-$major.log" 2>&1
  gcc -shared -fPIC -Iinclude -I"$("$PGC" --includedir-server)" -o "$OUT/standby_probe-$major.so" bench/aws/standby_probe.c >"$OUT/helper-$major.log" 2>&1 || { cat "$OUT/helper-$major.log"; exit 1; }
  cp pg_weave.so "$OUT/pg_weave-experimental-$major.so"
  sha256sum "$OUT/"*"-$major.so" >"$OUT/hashes-$major.txt"
  export PATH=/usr/lib/postgresql/$major/bin:$PATH
  export PG_REGRESS=/usr/lib/postgresql/$major/lib/pgxs/src/test/regress/pg_regress
  export PROBE_HELPER="$OUT/standby_probe-$major.so"
  mkdir -p "$OUT/pg$major"
  cd "$OUT/pg$major"
  export PERL5LIB=/usr/lib/postgresql/$major/lib/pgxs/src/test/perl
  prove -v /tmp/standby-probe-src/bench/aws/standby_probe.pl >"$OUT/prove-$major.log" 2>&1 || { cat "$OUT/prove-$major.log"; exit 1; }
  grep 'PROBE COMPLETE' "$OUT/prove-$major.log"
  cd /tmp/standby-probe-src
 done
