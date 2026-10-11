#!/usr/bin/env bash
# Recorded G95 A/A then alternating A/B, both scales, from a committed tree.
# MAIN_PATCH is a committed reverse diff of production changes against f0b6ed7;
# its applied amscan.c hash must match that revision before A is built.
# AWS_PROFILE=kiwi SCRIPT=bench/aws/g95_job.sh bench/aws/run.sh c7i.2xlarge script
set -euo pipefail
cd ~/pg_weave
out=/tmp/out/g95
mkdir -p "$out"
export PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
export PATH=/usr/lib/postgresql/17/bin:$PATH
export PGPORT=$(pg_lsclusters -h | awk '$1==17 && $2=="main"{print $3}')
[ "$(psql -XAt -d postgres -c 'show server_version_num' | cut -c1-2)" = 17 ]
base=/scratch/pg_weave/g95-main
mkdir -p "$base"
cp -a . "$base/"
patch -d "$base" -p1 < bench/aws/g95-main.patch
# Main's production file, not a mutant that merely disables the new route.
(cd "$base" && sha256sum -c bench/aws/g95-main.sha256)
build() {
 local tree=$1 arm=$2
 (cd "$tree"; make clean with_llvm=no >"$out/$arm-clean.log" 2>&1
  make -j2 with_llvm=no >"$out/$arm-build.log" 2>&1) || {
   tail -60 "$out/$arm-build.log"; exit 1;
 }
}
install() {
 local tree=$1 arm=$2
 (cd "$tree"; sudo make install with_llvm=no >"$out/$arm-install.log" 2>&1) || {
   tail -60 "$out/$arm-install.log"; exit 1;
 }
 sha256sum "$($PG_CONFIG --pkglibdir)/pg_weave.so" >> "$out/libraries.txt"
}
build "$base" main
build "$HOME/pg_weave" stream
psql -X -v ON_ERROR_STOP=1 -d postgres -c 'CREATE EXTENSION IF NOT EXISTS pg_weave'
for n in 200000 1000000; do
 install "$HOME/pg_weave" stream
 psql -X -v ON_ERROR_STOP=1 -d postgres > "$out/load-$n.log" <<SQL
SET jit = off;
DROP TABLE IF EXISTS g95_bench;
CREATE TABLE g95_bench (id int, d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO g95_bench SELECT g, to_wdoc('simple', concat_ws(' ',
  CASE WHEN g % 10 = 0 THEN 'w1' END, CASE WHEN g % 2 = 0 THEN 'w2' END,
  CASE WHEN g % 500 = 3 THEN 'rare' END,
  (SELECT string_agg('f' || ((g::int8 * 7919 + i * 104729) % 50000), ' ')
     FROM generate_series(1, 8) i))) FROM generate_series(1, $n) g;
CREATE INDEX g95_bench_w ON g95_bench USING weave(d);
VACUUM ANALYZE g95_bench;
SQL
 # A/A before comparing arms; then A/B twice, all on the same host/corpus.
 for leg in aa1:main aa2:main a1:main b1:stream a2:main b2:stream; do
  label=${leg%:*}; arm=${leg#*:}
  if [ "$arm" = main ]; then install "$base" "$arm"; else install "$HOME/pg_weave" "$arm"; fi
  psql -X -v ON_ERROR_STOP=1 -v n="$n" -v arm="$arm" -v leg="$label" \
   -d postgres -f bench/aws/g95_measure.sql > "$out/$n-$label.log" 2>&1
  echo "G95 MEASURED n=$n leg=$label arm=$arm"
 done
done
psql -X -v ON_ERROR_STOP=1 -d postgres -c 'DROP TABLE g95_bench'
echo 'G95 BENCHMARK COMPLETE'
