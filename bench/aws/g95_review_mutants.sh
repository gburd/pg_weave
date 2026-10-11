#!/usr/bin/env bash
# Owned Debian dev host only. Cap and partial-predicate positive controls.
set -euo pipefail
cd ~/pg_weave
export PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
export PATH=/usr/lib/postgresql/17/bin:$PATH
export PGPORT=$(pg_lsclusters -h | awk '$1==17 && $2=="main"{print $3}')
out=/tmp/out/g95-review-mutants
mkdir -p "$out"
cp src/am/amscan.c "$out/amscan.clean.c"
build() {
 local label=$1 flags=$2
 make clean with_llvm=no >"$out/$label-clean.log" 2>&1
 make -j4 with_llvm=no PG_CPPFLAGS="-I./include $flags" >"$out/$label-build.log" 2>&1 || {
  tail -60 "$out/$label-build.log"; exit 1;
 }
 sudo make install PG_CONFIG="$PG_CONFIG" with_llvm=no >"$out/$label-install.log" 2>&1 || {
  tail -60 "$out/$label-install.log"; exit 1;
 }
 cmp pg_weave.so "$($PG_CONFIG --pkglibdir)/pg_weave.so"
 sha256sum pg_weave.so >"$out/$label.sha256"
 echo "BUILT INSTALLED HASH-VERIFIED $label $(cat "$out/$label.sha256")"
}
restore() {
 cp "$out/amscan.clean.c" src/am/amscan.c
 build restored ''
}
trap restore EXIT
capflags='-DWEAVE_TEST_HOOKS -DWEAVE_TEST_ORD_WIDTH_MAX=128'
cap_sql() {
 dropdb --if-exists g95cap >"$out/$1-db.log" 2>&1
 createdb g95cap
 psql -X -v ON_ERROR_STOP=1 -d g95cap -f bench/aws/g95_cap.sql >"$out/$1-sql.log" 2>&1
}
build cap-clean "$capflags"
cap_sql cap-clean
grep -q 'G95 lexical cap refused' "$out/cap-clean-sql.log"
[ "$(grep -c 'G95 sibling cap refused' "$out/cap-clean-sql.log")" = 2 ]
for route in lexical vector fused; do
 cp "$out/amscan.clean.c" src/am/amscan.c
 ROUTE=$route python3 - <<'PY'
import os,re
p='src/am/amscan.c'; s=open(p).read(); route=os.environ['ROUTE']
pattern=r'ereport\(ERROR,\s*\(errcode\(ERRCODE_PROGRAM_LIMIT_EXCEEDED\),\s*errmsg\("weave '+route+r' ordering candidate limit.*?errhint\("Use a smaller LIMIT, or disable index scans to sort the full result\."\)\)\);'
s,n=re.subn(pattern, '{ elog(NOTICE, "G95 CAP MUTANT SITE '+route+'"); return false; }', s, flags=re.S)
assert n==1,n
open(p,'w').write(s)
PY
 build "cap-$route" "$capflags"
 ! cmp -s "$out/cap-clean.sha256" "$out/cap-$route.sha256"
 if cap_sql "cap-$route"; then echo "SURVIVED $route"; exit 1; fi
 grep -q "G95 CAP MUTANT SITE $route" "$out/cap-$route-sql.log"
 grep -q 'ERROR:.*G95 .*cap not refused' "$out/cap-$route-sql.log"
 echo "KILLED $route: built installed site reached oracle failed"
done
cp "$out/amscan.clean.c" src/am/amscan.c
# Macro alone must have NO effect in release: >128 positives retain real scores.
build release-override '-DWEAVE_TEST_ORD_WIDTH_MAX=128'
dropdb --if-exists g95cap
createdb g95cap
sed '/CREATE TEMP TABLE capgot/,$d' bench/aws/g95_cap.sql >"$out/release.sql"
cat >>"$out/release.sql" <<'SQL'
DO $$
DECLARE n int; ranked int;
BEGIN
 SELECT count(*), count(*) FILTER (WHERE id <= 300 AND dist < 1)
 INTO n, ranked FROM
 (SELECT id, weave_current_distance('capdocs_w',ctid,'fox | !absent'::wquery) dist
  FROM capdocs WHERE d @@@ 'fox | !absent'::wquery
  ORDER BY d <=> 'fox | !absent'::wquery) s;
 IF n <> 400 OR ranked <> 300 THEN
   RAISE EXCEPTION 'release cap override leaked: % / %', n, ranked;
 END IF;
 IF EXISTS (SELECT FROM pg_settings WHERE name LIKE 'pg_weave.test_%') THEN
   RAISE EXCEPTION 'release test GUC leaked';
 END IF;
 RAISE NOTICE 'G95 release override ignored; 400 rows, 300 positive; no test GUC';
END $$;
SQL
psql -X -v ON_ERROR_STOP=1 -d g95cap -f "$out/release.sql" >"$out/release-sql.log" 2>&1
grep -q 'G95 release override ignored' "$out/release-sql.log"
! strings pg_weave.so | grep -q 'pg_weave.test_pause_advisory_key'
echo 'RELEASE OVERRIDE AND GUC ABSENCE PROVEN'
# Predicate mutant exercises the shared walker through an SRF, so executor
# quals cannot accidentally repair its result.
python3 - <<'PY'
p='src/am/amscan.c';s=open(p).read()
a='if (w->predicate == NULL || ExecQual(w->predicate, econtext))'
b='if (w->predicate != NULL) elog(NOTICE, "G95 PREDICATE MUTANT SITE");\n\t\tif (true)'
assert s.count(a)==1
open(p,'w').write(s.replace(a,b))
PY
build predicate ''
! cmp -s "$out/release-override.sha256" "$out/predicate.sha256"
if make installcheck REGRESS= ISOLATION= PROVE_TESTS=t/036_padding_walk.pl >"$out/predicate-tap.log" 2>&1; then
 echo 'SURVIVED predicate'; exit 1
fi
grep -q 'Failed test.*G95 partial padding membership' "$out/predicate-tap.log"
grep -R 'G95 PREDICATE MUTANT SITE' tmp_check >"$out/predicate-site.log"
echo 'KILLED predicate: built installed site reached TAP membership failed'
restore
trap - EXIT
make installcheck REGRESS= ISOLATION= PROVE_TESTS=t/036_padding_walk.pl >"$out/restored-tap.log" 2>&1
grep -q 'Result: PASS' "$out/restored-tap.log"
dropdb g95cap
echo 'G95 REVIEW MUTANTS COMPLETE'
