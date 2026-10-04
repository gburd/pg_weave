#!/usr/bin/env bash
#
# bench/vmajor_bytes.sh -- what a SECOND, vector-major copy of the 4-bit codes
# would cost on disk, against a measured pgvector HNSW on the same vectors.
#
# Runs on an EC2 host via `SCRIPT=bench/vmajor_bytes.sh bench/aws/run.sh
# c7i.2xlarge script`, which has already built and installed pg_weave.  Results:
# bench/RESULTS_VECMAJOR.md.
#
# For dim in DIMS: N random unit vectors (gaussian, L2-normalized, fixed seed per
# dim), loaded once into a weave table (wvec + a constant trivial wdoc) and once
# into a pgvector table (same generator, same seed => same vectors), then
#   CREATE INDEX ... USING weave (d, v) WITH (bits = 4)
#   CREATE INDEX ... USING hnsw (v vector_cosine_ops) WITH (m = 16, ef_construction = 64)
# and every byte of the weave index split by weave_index_size_detail(), with the
# code weft additionally split into lane strips and centroid strips by
# weave_vec_strips().  The two splits are independent reports, so the script
# asserts they agree on the code-page count before it prints anything.
#
# Writes /tmp/out/vmajor_raw.tsv (every size_detail row) and
# /tmp/out/vmajor_bytes.tsv (one row per dim), and prints the latter.
#
set -euo pipefail

N=${N:-200000}
DIMS=${DIMS:-"384 768 960 1024"}
PSQL="psql -X -q -v ON_ERROR_STOP=1"
OUT=/tmp/out
mkdir -p "$OUT"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

# The right server, not merely a server (AGENTS.md: derive, never assume).
ver=$($PSQL -tAc "show server_version_num")
case "$ver" in 17*) ;; *) echo "expected PG17, got $ver" >&2; exit 1 ;; esac

say "installing pgvector"
sudo apt-get -qq install -y postgresql-17-pgvector >/dev/null
ls -l /usr/lib/postgresql/17/lib/vector.so
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave" -c "CREATE EXTENSION IF NOT EXISTS vector"
$PSQL -tA -F$'\t' -c "SELECT extname, extversion FROM pg_extension WHERE extname IN ('pg_weave','vector') ORDER BY 1" \
	| tee "$OUT/versions.tsv"
echo "nproc=$(nproc) mem_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo)" | tee -a "$OUT/versions.tsv"

say "building the generator"
cat > /tmp/gen.c <<'C'
/* gen <n> <dim> <seed>: n rows "id\t[v1,...]" of L2-normalized gaussian vectors.
 * Deterministic (xorshift64* + Box-Muller), so two runs with one seed emit the
 * same vectors -- which is how the weave and pgvector tables hold the same data. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t s;
static double u01(void) { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
	return ((s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0) + 1e-300; }
int main(int argc, char **argv) {
	long n = atol(argv[1]); int dim = atoi(argv[2]); s = strtoull(argv[3], 0, 10) | 1;
	double *v = malloc(sizeof(double) * dim);
	for (long i = 1; i <= n; i++) {
		double ss = 0;
		for (int j = 0; j < dim; j++) {
			v[j] = sqrt(-2 * log(u01())) * cos(2 * M_PI * u01());
			ss += v[j] * v[j];
		}
		double inv = 1 / sqrt(ss);
		printf("%ld\t[", i);
		/* rounded to float32 first, so the decimal names one float exactly and
		 * wvec (strtod) and pgvector (strtof) parse it to the same value */
		for (int j = 0; j < dim; j++) printf(j ? ",%.9g" : "%.9g", (double) (float) (v[j] * inv));
		fputs("]\n", stdout);
	}
	return fflush(stdout) != 0;
}
C
gcc -O2 -o /tmp/gen /tmp/gen.c -lm

printf 'dim\tkind\tnpages\tbytes\tfree_bytes\n' > "$OUT/vmajor_raw.tsv"
printf 'dim\tn\tsegs\tnblocks\tweave_B\tcodes_pages\tlane_pages\tcen_pages\tcodes_B\tdir_B\twarp_B\tvmeta_B\tlexical_B\thnsw_B\tweave_Bpv\tcodes_Bpv\tlane_Bpv\tcen_Bpv\tdir_Bpv\twarp_Bpv\thnsw_Bpv\n' \
	> "$OUT/vmajor_bytes.tsv"

for D in $DIMS; do
	say "dim $D: load $N vectors twice"
	$PSQL -c "DROP TABLE IF EXISTS tw, tp"
	$PSQL -c "CREATE TABLE tw (id int, d wdoc DEFAULT to_wdoc('v'), v wvec($D))"
	$PSQL -c "CREATE TABLE tp (id int, v vector($D))"
	/tmp/gen "$N" "$D" "$D" | $PSQL -c "COPY tw (id, v) FROM STDIN"
	/tmp/gen "$N" "$D" "$D" | $PSQL -c "COPY tp (id, v) FROM STDIN"
	# Same vectors in both tables: spot-check three rows coordinate by coordinate.
	$PSQL -tAc "SELECT count(*) FROM tw" | grep -qx "$N"
	$PSQL -tAc "SELECT count(*) FROM tp" | grep -qx "$N"
	bad=$($PSQL -tAc "SELECT count(*) FROM tw JOIN tp USING (id)
	                  WHERE id IN (1, $N / 2, $N) AND (tw.v::real[]) <> (tp.v::real[])")
	[ "$bad" = 0 ] || { echo "dim $D: weave and pgvector rows differ ($bad)" >&2; exit 1; }

	say "dim $D: weave build"
	t0=$SECONDS
	$PSQL -c "SET maintenance_work_mem = '2GB'" \
		      -c "CREATE INDEX tw_idx ON tw USING weave (d, v) WITH (bits = 4)"
	echo "weave_build_seconds dim=$D $((SECONDS - t0))" | tee -a "$OUT/build_seconds.txt"
	say "dim $D: hnsw build"
	t0=$SECONDS
	$PSQL -c "SET maintenance_work_mem = '4GB'" \
		      -c "SET max_parallel_maintenance_workers = 7" \
		      -c "CREATE INDEX tp_hnsw ON tp USING hnsw (v vector_cosine_ops) WITH (m = 16, ef_construction = 64)" \
		2>&1 | tee "$OUT/hnsw_build_$D.log"
	echo "hnsw_build_seconds dim=$D $((SECONDS - t0))" | tee -a "$OUT/build_seconds.txt"

	$PSQL -tA -F$'\t' -c "SELECT $D, kind, npages, bytes, free_bytes FROM weave_index_size_detail('tw_idx') ORDER BY kind" \
		>> "$OUT/vmajor_raw.tsv"

	# Two independent page counts for the code weft must agree, and the
	# size_detail buckets must sum to the relation, or nothing below means anything.
	chk=$($PSQL -tAc "
		SELECT (SELECT npages FROM weave_index_size_detail('tw_idx') WHERE kind = 'vector_codes')
		       = (SELECT count(*) FROM weave_vec_strips('tw_idx'))
		   AND (SELECT sum(bytes) FROM weave_index_size_detail('tw_idx')) = pg_relation_size('tw_idx')
		   AND (SELECT sum(nvec) FROM weave_vec_meta('tw_idx')) = $N
		   AND (SELECT bool_and(bits = 4 AND dim = $D) FROM weave_vec_meta('tw_idx'))")
	[ "$chk" = t ] || { echo "dim $D: consistency check failed" >&2; exit 1; }

	$PSQL -tA -F$'\t' >> "$OUT/vmajor_bytes.tsv" <<-SQL
		WITH sd AS (SELECT kind, npages, bytes FROM weave_index_size_detail('tw_idx')),
		     vm AS (SELECT count(*) AS segs, sum(nblocks) AS nblocks FROM weave_vec_meta('tw_idx')),
		     st AS (SELECT count(*) FILTER (WHERE NOT centroid) AS lane_pages,
		                   count(*) FILTER (WHERE centroid) AS cen_pages
		              FROM weave_vec_strips('tw_idx')),
		     r AS (
		SELECT $D AS dim, $N::numeric AS n, vm.segs, vm.nblocks,
		       pg_relation_size('tw_idx') AS weave_b,
		       (SELECT npages FROM sd WHERE kind = 'vector_codes') AS codes_pages,
		       st.lane_pages, st.cen_pages,
		       (SELECT bytes FROM sd WHERE kind = 'vector_codes') AS codes_b,
		       (SELECT bytes FROM sd WHERE kind = 'vector_dir') AS dir_b,
		       (SELECT bytes FROM sd WHERE kind = 'vector_warp') AS warp_b,
		       (SELECT bytes FROM sd WHERE kind = 'vector_meta') AS vmeta_b,
		       pg_relation_size('tw_idx')
		         - (SELECT sum(bytes) FROM sd WHERE kind LIKE 'vector\_%') AS lexical_b,
		       pg_relation_size('tp_hnsw') AS hnsw_b
		  FROM vm, st)
		SELECT dim, n, segs, nblocks, weave_b, codes_pages, lane_pages, cen_pages,
		       codes_b, dir_b, warp_b, vmeta_b, lexical_b, hnsw_b,
		       round(weave_b / n, 1), round(codes_b / n, 1),
		       round(lane_pages * 8192 / n, 1), round(cen_pages * 8192 / n, 1),
		       round(dir_b / n, 1), round(warp_b / n, 1), round(hnsw_b / n, 1)
		  FROM r
	SQL
	tail -1 "$OUT/vmajor_bytes.tsv"
	$PSQL -c "DROP TABLE tw, tp"
done

say "done"
cat "$OUT/vmajor_raw.tsv"
echo
cat "$OUT/vmajor_bytes.tsv"
