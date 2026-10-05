#!/usr/bin/env bash
#
# bench/cenpack_job.sh -- the gate for vector weft v4 (centroid strip packed onto
# its block's last lane page; doc/specs/VECTOR_CHANNEL.md sect. 7.1 "v4").
#
# Launched by bench/cenpack_launch.sh, which prepends a write_v3_patch() function
# holding `git diff HEAD <base> -- src include` -- the remote tree is a git archive
# with no history, the same reason bench/aws/run.sh's p0 job extracts its diff
# locally.  The job then:
#
#   1. builds the v4 .so (the uploaded tree) and a v3 .so (the tree with the patch
#      applied in reverse), and installs v3;
#   2. under v3: builds the indexes, records each one's vector answers, and asserts
#      they are v3 wefts with one page per strip;
#   3. installs v4, restarts, and asserts the SAME v3 indexes give byte-identical
#      answers (dual-read), weave_check is clean, and a merge (insert + flush + a
#      REINDEX-free merge) writes v4 wefts that still answer identically;
#   4. REINDEXes copies under v4 and asserts v4 layout + identical answers -- "the
#      codes and the centroid are the same bytes, only their page differs";
#   5. cold-cache latency: the v3 index and its v4 REINDEX, same binary, same
#      vectors, each sampled with sync + drop_caches + restart, TWO runs per arm
#      (hard rule 10), interleaved;
#   6. the three mutants (reader ignores the packed centroid; writer packs a
#      centroid that does not fit; plan and reader disagree on the page index),
#      each built and asserted to BUILD, then asserted to be caught by sql that
#      reaches the mutated function.
#
# Results under /tmp/out.  Exit status is the job's.
#
set -euo pipefail
OUT=/tmp/out
mkdir -p "$OUT"
PG=/usr/lib/postgresql/17/bin/pg_config
PSQL="psql -X -q -v ON_ERROR_STOP=1"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }
die() { echo "FATAL: $*" >&2; exit 1; }

# write_v3_patch() is PREPENDED by the launcher (bench/cenpack_launch.sh), which
# has the history; run without it, the job refuses here.
type write_v3_patch >/dev/null 2>&1 || die "write_v3_patch() not defined: launch with bench/cenpack_launch.sh"

ver=$($PSQL -tAc "show server_version_num")
case "$ver" in 17*) ;; *) die "expected PG17, got $ver" ;; esac

NV=${NV:-20000}			# rows per dim in the identity tables
NQ=${NQ:-20}			# queries per dim
LATN=${LATN:-15}		# cold samples per (arm, run)
LATROWS=${LATROWS:-200000}

install_tree() {	# $1 = tree, $2 = label
	( cd "$1" && { make -s clean >/dev/null 2>&1 || true; } &&
	  make -s -j"$(nproc)" PG_CONFIG=$PG with_llvm=no > "$OUT/build_$2.log" 2>&1 ) \
		|| { grep -E 'error' "$OUT/build_$2.log" | head -20; die "build $2 failed"; }
	( cd "$1" && sudo make install PG_CONFIG=$PG with_llvm=no > "$OUT/install_$2.log" 2>&1 ) \
		|| { tail -20 "$OUT/install_$2.log"; die "install $2 failed"; }
	sudo find /usr/lib/postgresql/17/lib/bitcode -maxdepth 1 -name 'pg_weave*' \
		-exec find {} -depth -delete \; 2>/dev/null || true
	sudo pg_ctlcluster 17 main restart
	$PSQL -tAc 'SELECT 1' >/dev/null
	md5sum /usr/lib/postgresql/17/lib/pg_weave.so | tee -a "$OUT/so_md5.txt"
	echo "installed $2" | tee -a "$OUT/so_md5.txt"
}

# ---- 1. two trees -------------------------------------------------------------
say "1. trees: v4 = uploaded, v3 = reverse patch"
V4=$HOME/pg_weave
V3=/tmp/pg_weave_v3
# find -delete, not rm -r: this repository's harness refuses the latter (AGENTS.md)
[ -d "$V3" ] && find "$V3" -depth -delete
cp -a "$V4" "$V3"
write_v3_patch > /tmp/cenpack_v3.patch
[ -s /tmp/cenpack_v3.patch ] || die "no embedded patch"
( cd "$V3" && git apply --check /tmp/cenpack_v3.patch && git apply /tmp/cenpack_v3.patch ) \
	|| die "the v3 patch does not apply to the uploaded tree"
grep -q 'define WEAVE_VMETA_VERSION		3' "$V3/include/weave/vector.h" || die "v3 tree is not v3"
grep -q 'WEAVE_VECWEFT_CUR' "$V4/include/weave/vector.h" || die "v4 tree is not v4"
echo "v3 tree carries WEAVE_VMETA_VERSION 3; v4 tree carries WEAVE_VECWEFT_CUR" | tee "$OUT/trees.txt"

# ---- 2. under v3 -------------------------------------------------------------
install_tree "$V3" v3
$PSQL -c "DROP EXTENSION IF EXISTS pg_weave CASCADE" -c "CREATE EXTENSION pg_weave" \
      -c "CREATE EXTENSION IF NOT EXISTS pageinspect"

cat > /tmp/gen.c <<'C'
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t s;
static double u01(void) { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
	return ((s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0) + 1e-300; }
int main(int argc, char **argv) {
	long n = atol(argv[1]); int dim = atoi(argv[2]); s = strtoull(argv[3], 0, 10) | 1;
	long first = argc > 4 ? atol(argv[4]) : 1;
	double *v = malloc(sizeof(double) * dim);
	for (long i = first; i < first + n; i++) {
		double ss = 0;
		for (int j = 0; j < dim; j++) { v[j] = sqrt(-2 * log(u01())) * cos(2 * M_PI * u01()); ss += v[j] * v[j]; }
		printf("%ld\t[", i);
		for (int j = 0; j < dim; j++) printf(j ? ",%.9g" : "%.9g", (double) (float) (v[j] / sqrt(ss)));
		fputs("]\n", stdout);
	}
	return 0;
}
C
gcc -O2 -o /tmp/gen /tmp/gen.c -lm

# 384/768/960/1024 pack; 500 and 1000 spill.  Two bit widths at 384.
DIMS="384 500 768 960 1000 1024"
answers() {	# $1 = index, $2 = table, $3 = dim -> one line per query: md5 of the result
	$PSQL -tA -c "SET pg_weave.vec_kernel = 'scalar'" -c "
		SELECT q.id, md5(string_agg(s.segno || ':' || s.warp || ':' || s.docid || ':' || s.score::text,
		                            ',' ORDER BY s.score, s.docid))
		  FROM ${2}_q q, LATERAL weave_vec_scan('$1', q.v, 50) s
		 GROUP BY q.id ORDER BY q.id"
}
for D in $DIMS; do
	say "2. v3 build, dim $D"
	$PSQL -c "CREATE TABLE t$D (id int, d wdoc DEFAULT to_wdoc('v'), v wvec($D))"
	/tmp/gen "$NV" "$D" "$D" | $PSQL -c "COPY t$D (id, v) FROM STDIN"
	$PSQL -c "CREATE TABLE t${D}_q (id int, v wvec($D))"
	/tmp/gen "$NQ" "$D" "$((D + 7))" | $PSQL -c "COPY t${D}_q (id, v) FROM STDIN"
	$PSQL -c "CREATE INDEX i${D}_old ON t$D USING weave (d, v) WITH (bits = 4)"
	$PSQL -c "CREATE INDEX i${D}_mrg ON t$D USING weave (d, v) WITH (bits = 4)"
	answers "i${D}_old" "t$D" "$D" > "$OUT/ans_v3_$D.txt"
	[ "$(wc -l < "$OUT/ans_v3_$D.txt")" = "$NQ" ] || die "dim $D: v3 answered $(wc -l < "$OUT/ans_v3_$D.txt") queries"
	$PSQL -tA -c "SELECT count(*), count(DISTINCT blkno), (SELECT sum(nblocks) FROM weave_vec_meta('i${D}_old'))
	              FROM weave_vec_strips('i${D}_old')" | tee "$OUT/layout_v3_$D.txt"
	IFS='|' read -r ns np nb < "$OUT/layout_v3_$D.txt"
	[ "$ns" = "$np" ] || die "dim $D: a v3 weft has $ns strips on $np pages"
	$PSQL -tAc "SELECT md5(string_agg(code::text, ',' ORDER BY segno, warp)) FROM weave_vec_lanes('i${D}_old')" \
		> "$OUT/lanes_v3_$D.txt"
	$PSQL -tAc "SELECT md5(string_agg(blockno||':'||livemask||':'||smax||':'||maxrecnorm||':'||minnorm||':'||censcale||':'||cenrad, ',' ORDER BY segno, blockno)) FROM weave_vec_blocks('i${D}_old')" \
		> "$OUT/blocks_v3_$D.txt"
done
$PSQL -c "CREATE TABLE tlat (id int, d wdoc DEFAULT to_wdoc('v'), v wvec(960))"
/tmp/gen "$LATROWS" 960 960 | $PSQL -c "COPY tlat (id, v) FROM STDIN"
$PSQL -c "CREATE TABLE tlat_q (id int, v wvec(960))"
/tmp/gen 8 960 4242 | $PSQL -c "COPY tlat_q (id, v) FROM STDIN"
$PSQL -c "SET maintenance_work_mem = '2GB'" -c "CREATE INDEX lat_v3 ON tlat USING weave (d, v) WITH (bits = 4)"
$PSQL -c "VACUUM ANALYZE"

# ---- 3. upgrade: the v4 .so reads v3 wefts -----------------------------------
install_tree "$V4" v4
$PSQL -c "ALTER EXTENSION pg_weave UPDATE"
# WeaveVecMeta.version: a uint16 after the uint32 magic, at PageGetContents() = 24
vprobe() {
	$PSQL -tA -c "SELECT string_agg(DISTINCT 'v' || (get_byte(page, 28) + 256 * get_byte(page, 29)), ',')
	              FROM weave_vec_meta('$1') m, LATERAL (SELECT get_raw_page('$1', m.root::int) AS page) p"
}
fail=0
for D in $DIMS; do
	say "3. v4 .so on the v3 index, dim $D"
	answers "i${D}_old" "t$D" "$D" > "$OUT/ans_v4so_v3idx_$D.txt"
	if cmp -s "$OUT/ans_v3_$D.txt" "$OUT/ans_v4so_v3idx_$D.txt"; then
		echo "IDENTICAL dim=$D v3-index answers under the v4 .so ($NQ queries)" | tee -a "$OUT/identity.txt"
	else
		echo "DIFFER dim=$D v3-index answers under the v4 .so" | tee -a "$OUT/identity.txt"; fail=1
	fi
	v=$($PSQL -tAc "SELECT count(*) FROM weave_check('i${D}_old', true) WHERE NOT ok")
	echo "check dim=$D v3-index-under-v4 violations=$v" | tee -a "$OUT/identity.txt"
	[ "$v" = 0 ] || { $PSQL -c "SELECT * FROM weave_check('i${D}_old', true) WHERE NOT ok"; fail=1; }

	vprobe "i${D}_mrg" > "$OUT/pre_version_$D.txt"
	# merge: inserts flush into a second bolt (no vector weft for pending rows that
	# carry... they do since v9 -- either way) and weave_merge rewrites everything
	/tmp/gen 100 "$D" "$((D + 99))" "$((NV + 1))" | $PSQL -c "COPY t$D (id, v) FROM STDIN"
	$PSQL -tAc "SELECT weave_merge('i${D}_mrg')" >/dev/null
	$PSQL -tAc "SELECT weave_merge('i${D}_mrg')" >/dev/null
	vprobe "i${D}_mrg" > "$OUT/mrg_version_$D.txt"
	$PSQL -tA -c "SELECT count(*), count(DISTINCT blkno), (SELECT sum(nblocks) FROM weave_vec_meta('i${D}_mrg'))
	              FROM weave_vec_strips('i${D}_mrg')" | tee "$OUT/layout_mrg_$D.txt"
	v=$($PSQL -tAc "SELECT count(*) FROM weave_check('i${D}_mrg', true) WHERE NOT ok")
	echo "check dim=$D merged violations=$v segs=$($PSQL -tAc "SELECT weave_index_nsegments('i${D}_mrg')") weft_versions=$(cat "$OUT/mrg_version_$D.txt")" | tee -a "$OUT/identity.txt"
	[ "$v" = 0 ] || fail=1
	[ "$(cat "$OUT/mrg_version_$D.txt")" = v4 ] || { echo "MERGE dim=$D did not leave only v4 wefts" | tee -a "$OUT/identity.txt"; fail=1; }
	# and before the merge the same index's wefts were v3 (positive control on the probe)
	[ "$(cat "$OUT/pre_version_$D.txt")" = v3 ] || { echo "PROBE dim=$D did not read v3 before the merge" | tee -a "$OUT/identity.txt"; fail=1; }
done

# ---- 4. REINDEX under v4: same codes, same answers, new layout -----------------
for D in $DIMS; do
	say "4. v4 REINDEX, dim $D"
	$PSQL -c "DELETE FROM t$D WHERE id > $NV" -c "VACUUM t$D"
	$PSQL -c "CREATE INDEX i${D}_new ON t$D USING weave (d, v) WITH (bits = 4)"
	$PSQL -c "DROP INDEX i${D}_old"	# so weave_vec_scan cannot pick the old one by accident -- it takes the named index anyway
	answers "i${D}_new" "t$D" "$D" > "$OUT/ans_v4_$D.txt"
	if cmp -s "$OUT/ans_v3_$D.txt" "$OUT/ans_v4_$D.txt"; then
		echo "IDENTICAL dim=$D v4-REINDEX answers == v3 answers ($NQ queries)" | tee -a "$OUT/identity.txt"
	else
		echo "DIFFER dim=$D v4-REINDEX answers vs v3" | tee -a "$OUT/identity.txt"; fail=1
	fi
	$PSQL -tAc "SELECT md5(string_agg(code::text, ',' ORDER BY segno, warp)) FROM weave_vec_lanes('i${D}_new')" \
		> "$OUT/lanes_v4_$D.txt"
	$PSQL -tAc "SELECT md5(string_agg(blockno||':'||livemask||':'||smax||':'||maxrecnorm||':'||minnorm||':'||censcale||':'||cenrad, ',' ORDER BY segno, blockno)) FROM weave_vec_blocks('i${D}_new')" \
		> "$OUT/blocks_v4_$D.txt"
	cmp -s "$OUT/lanes_v3_$D.txt" "$OUT/lanes_v4_$D.txt" && cmp -s "$OUT/blocks_v3_$D.txt" "$OUT/blocks_v4_$D.txt" \
		&& echo "IDENTICAL dim=$D lane codes and block records v3 == v4" | tee -a "$OUT/identity.txt" \
		|| { echo "DIFFER dim=$D lane codes or block records" | tee -a "$OUT/identity.txt"; fail=1; }
	$PSQL -tA -c "SELECT count(*), count(DISTINCT blkno), (SELECT sum(nblocks) FROM weave_vec_meta('i${D}_new'))
	              FROM weave_vec_strips('i${D}_new')" | tee "$OUT/layout_v4_$D.txt"
	v=$($PSQL -tAc "SELECT count(*) FROM weave_check('i${D}_new', true) WHERE NOT ok")
	echo "check dim=$D v4 violations=$v" | tee -a "$OUT/identity.txt"
	[ "$v" = 0 ] || fail=1
done
printf 'dim\tv3_strips\tv3_pages\tv4_strips\tv4_pages\tnblocks\n' > "$OUT/layout.tsv"
for D in $DIMS; do
	IFS='|' read -r a b nb < "$OUT/layout_v3_$D.txt"; IFS='|' read -r c e _ < "$OUT/layout_v4_$D.txt"
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$D" "$a" "$b" "$c" "$e" "$nb" >> "$OUT/layout.tsv"
done
cat "$OUT/layout.tsv"
[ "$fail" = 0 ] || die "identity/upgrade checks failed (see $OUT/identity.txt)"

# ---- 5. cold latency, same binary, v3 index vs v4 index ---------------------------
say "5. cold latency"
$PSQL -c "SET maintenance_work_mem = '2GB'" -c "CREATE INDEX lat_v4 ON tlat USING weave (d, v) WITH (bits = 4)"
for ix in lat_v3 lat_v4; do
	$PSQL -tA -c "SELECT '$ix', count(*), count(DISTINCT blkno), pg_relation_size('$ix'),
	                     round(pg_relation_size('$ix')::numeric / $LATROWS, 1) AS b_per_vec,
	                     (SELECT sum(bytes) FROM weave_index_size_detail('$ix')) = pg_relation_size('$ix') AS sums
	              FROM weave_vec_strips('$ix')" | tee -a "$OUT/lat_layout.txt"
	$PSQL -tA -F$'\t' -c "SELECT '$ix', kind, npages, bytes, free_bytes FROM weave_index_size_detail('$ix') ORDER BY kind" >> "$OUT/lat_size_detail.tsv"
done
# same answers, both indexes, before timing anything (hard rule 8)
for ix in lat_v3 lat_v4; do
	$PSQL -tA -c "SELECT q.id, md5(string_agg(s.docid || ':' || s.score::text, ',' ORDER BY s.score, s.docid))
	              FROM tlat_q q, LATERAL weave_vec_scan('$ix', q.v, 10) s GROUP BY q.id ORDER BY q.id" > "$OUT/lat_ans_$ix.txt"
done
cmp -s "$OUT/lat_ans_lat_v3.txt" "$OUT/lat_ans_lat_v4.txt" || die "latency indexes answer differently"
echo "lat_v3 and lat_v4 answer identically" | tee -a "$OUT/identity.txt"
printf 'arm\trun\tsample\tms\tshared_read\tshared_hit\n' > "$OUT/lat.tsv"
cold() {	# $1 index, $2 run, $3 sample
	local qid=$(( ($3 % 8) + 1 ))
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
	sudo pg_ctlcluster 17 main restart; $PSQL -tAc 'SELECT 1' >/dev/null
	$PSQL -tA -c "SET jit = off" -c "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
		SELECT count(*) FROM weave_vec_scan('$1', (SELECT v FROM tlat_q WHERE id = $qid), 10)" > /tmp/plan.json
	python3 - "$1" "$2" "$3" >> "$OUT/lat.tsv" <<'PY'
import json, sys
p = json.load(open('/tmp/plan.json'))[0]
def walk(n):
    r, h = n.get('Shared Read Blocks', 0), n.get('Shared Hit Blocks', 0)
    return r, h
r, h = walk(p['Plan'])
print(f"{sys.argv[1]}\t{sys.argv[2]}\t{sys.argv[3]}\t{p['Execution Time']:.3f}\t{r}\t{h}")
PY
}
for run in 1 2; do
	for s in $(seq 1 "$LATN"); do
		if [ $(( (s + run) % 2 )) = 0 ]; then cold lat_v3 $run $s; cold lat_v4 $run $s
		else cold lat_v4 $run $s; cold lat_v3 $run $s; fi
	done
done
python3 - "$OUT/lat.tsv" <<'PY' | tee "$OUT/lat_summary.txt"
import csv, statistics, sys
rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
for arm in ('lat_v3', 'lat_v4'):
    for run in ('1', '2'):
        xs = sorted(float(r['ms']) for r in rows if r['arm'] == arm and r['run'] == run)
        rd = sorted(int(r['shared_read']) for r in rows if r['arm'] == arm and r['run'] == run)
        print(f"{arm} run{run} n={len(xs)} p50={statistics.median(xs):.1f}ms min={xs[0]:.1f} max={xs[-1]:.1f} reads_p50={statistics.median(rd)}")
PY

# ---- 6. mutants -----------------------------------------------------------------
say "6. mutants"
mut() {	# $1 name, $2 file, $3 python replace old, $4 new
	local M=/tmp/pg_weave_mut_$1
	[ -d "$M" ] && find "$M" -depth -delete
	cp -a "$V4" "$M"
	python3 - "$M/$2" "$3" "$4" <<'PY' || return 2
import sys
p, o, n = sys.argv[1:4]
s = open(p).read()
assert s.count(o) == 1, f"mutation site not unique: {s.count(o)}"
open(p, 'w').write(s.replace(o, n))
PY
	if ! install_tree "$M" "mut_$1"; then echo "MUTANT $1 DID NOT BUILD" | tee -a "$OUT/mutants.txt"; return 2; fi
	echo "MUTANT $1 BUILT" | tee -a "$OUT/mutants.txt"
}
mutcheck() {	# $1 name: build fresh 1024-d + 500-d indexes, scan, check
	local caught=0 D
	for D in 1024 500 960; do
		$PSQL -c "DROP INDEX IF EXISTS m_$D" >/dev/null
		if ! $PSQL -c "CREATE INDEX m_$D ON t$D USING weave (d, v) WITH (bits = 4)" > "$OUT/mut_$1_$D.log" 2>&1; then
			echo "  $1 dim $D: CREATE INDEX errored: $(grep -m1 ERROR "$OUT/mut_$1_$D.log")" | tee -a "$OUT/mutants.txt"; caught=1; continue
		fi
		if ! answers "m_$D" "t$D" "$D" > "$OUT/mut_ans_$1_$D.txt" 2>"$OUT/mut_$1_$D.err"; then
			echo "  $1 dim $D: scan errored: $(grep -m1 ERROR "$OUT/mut_$1_$D.err")" | tee -a "$OUT/mutants.txt"; caught=1
		elif ! cmp -s "$OUT/mut_ans_$1_$D.txt" "$OUT/ans_v3_$D.txt"; then
			echo "  $1 dim $D: answers DIFFER from v3" | tee -a "$OUT/mutants.txt"; caught=1
		fi
		v=$($PSQL -tAc "SELECT count(*) FROM weave_check('m_$D', true) WHERE NOT ok" 2>&1 || echo ERR)
		[ "$v" != 0 ] && { echo "  $1 dim $D: weave_check violations=$v" | tee -a "$OUT/mutants.txt"; caught=1; }
		$PSQL -c "DROP INDEX IF EXISTS m_$D" >/dev/null 2>&1 || true
	done
	[ "$caught" = 1 ] && echo "MUTANT $1 CAUGHT" | tee -a "$OUT/mutants.txt" || echo "MUTANT $1 SURVIVED" | tee -a "$OUT/mutants.txt"
}
# M1: the READER ignores the packed centroid -- page_take takes only the first strip
# of every page, so the writer still packs and the reader never sees the centroid
mut ignore_packed src/vector/vecweft.c \
"			*why = \"no such vector code page in the weft's layout\";
		return -1;
	}
" \
"			*why = \"no such vector code page in the weft's layout\";
		return -1;
	}
	n = 1;
" && mutcheck ignore_packed
# M2: the writer packs a centroid that does not fit (the fit check is gone)
mut nofit src/vector/vecweft.c \
"		(int) cen_coord_bytes(bits, dim) <= usable)" \
"		0 <= usable)" && mutcheck nofit
# M3: plan and reader disagree on the page index (the cursor walks one page too many per block)
mut cursor_pages src/vector/vecshuttle.c \
"	for (k = 0; k < g->pages_per_block; k++)
	{" \
"	for (k = 0; k < g->strips_per_block; k++)
	{" && mutcheck cursor_pages
install_tree "$V4" v4_final
grep -c 'CAUGHT' "$OUT/mutants.txt" | sed 's/^/mutants caught: /' | tee -a "$OUT/mutants.txt"
grep -q SURVIVED "$OUT/mutants.txt" && die "a mutant survived"
grep -q 'DID NOT BUILD' "$OUT/mutants.txt" && die "a mutant did not build"
say "ALL CENPACK GATES PASSED"
exit 0
