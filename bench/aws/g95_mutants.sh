#!/usr/bin/env bash
# Run only on the owned dev host, after clean full gates. Every mutant must
# build, install a changed library, announce its actual site, then fail SQL.
set -euo pipefail
cd ~/pg_weave
export PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
export PATH=/usr/lib/postgresql/17/bin:$PATH
export PGPORT=$(pg_lsclusters -h | awk '$1==17 && $2=="main"{print $3}')
out=/tmp/out/g95-mutants
mkdir -p "$out"
cp src/am/amscan.c "$out/amscan.clean.c"
install() {
 make clean with_llvm=no > "$out/$1-clean.log" 2>&1
 make -j2 with_llvm=no > "$out/$1-build.log" 2>&1 || {
  tail -60 "$out/$1-build.log"; return 1;
 }
 sudo make install with_llvm=no > "$out/$1-install.log" 2>&1 || {
  tail -60 "$out/$1-install.log"; return 1;
 }
 echo "BUILT AND INSTALLED $1"
}
restore() {
 cp "$out/amscan.clean.c" src/am/amscan.c
 install restore
}
trap restore EXIT
run_sql() {
 dropdb --if-exists g95_mutation > "$out/$1-db.log" 2>&1
 createdb g95_mutation
 psql -X -v ON_ERROR_STOP=1 -d g95_mutation -f sql/ranked_not.sql > "$out/$1-sql.log" 2>&1
}
install clean
clean=$(sha256sum "$($PG_CONFIG --pkglibdir)/pg_weave.so" | cut -d' ' -f1)
run_sql clean
grep -q 'randomized_windows' "$out/clean-sql.log"
for mutant in recheck seen root; do
 cp "$out/amscan.clean.c" src/am/amscan.c
 MUTANT=$mutant python3 - <<'PY'
import os
p = 'src/am/amscan.c'
s = open(p).read()
m = os.environ['MUTANT']
a, b = {
 'recheck': ('keep = weave_doc_matches(doc, w->query);',
             'elog(NOTICE, "G95 MUTANT SITE recheck"); keep = true;'),
 'seen': ('if (w->nseen > 0 &&',
          'elog(NOTICE, "G95 MUTANT SITE seen");\n\t\tif (false && w->nseen > 0 &&'),
 'root': ('*tid = root;\n\t\t\treturn true;',
          'elog(NOTICE, "G95 MUTANT SITE root");\n\t\t\t*tid = w->slot->tts_tid;\n\t\t\treturn true;')
}[m]
assert s.count(a) == 1, (m, s.count(a))
open(p, 'w').write(s.replace(a, b))
PY
 install "$mutant"
 hash=$(sha256sum "$($PG_CONFIG --pkglibdir)/pg_weave.so" | cut -d' ' -f1)
 [ "$hash" != "$clean" ]
 if run_sql "$mutant"; then
  echo "SURVIVED $mutant"; exit 1
 fi
 grep -q "G95 MUTANT SITE $mutant" "$out/$mutant-sql.log"
 grep -q 'ERROR:.*G95 .*mismatch' "$out/$mutant-sql.log"
 echo "KILLED $mutant built=$hash site-reached SQL-oracle-failed"
done
restore
trap - EXIT
run_sql restored
cmp "$out/clean-sql.log" "$out/restored-sql.log"
dropdb g95_mutation
echo 'G95 MUTANTS COMPLETE'
