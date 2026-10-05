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
# code weft additionally split into lane pages and centroid-only pages by
# weave_vec_strips() (one row per strip; since weft v4 a lane page may also carry
# its block's centroid strip, so pages are counted as DISTINCT blkno).  The two
# splits are independent reports, so the script asserts they agree on the
# code-page count before it prints anything.
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
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave" -c "CREATE EXTENSION IF NOT EXISTS vector" -c "CREATE EXTENSION IF NOT EXISTS pageinspect"
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

# Elements per HNSW page, from the page headers.  pgvector writes each element
# as two items (the element tuple and its neighbour tuple, on the same page when
# they fit), so elements = items / 2 over all non-meta pages.  Reported so the
# denominator is explained, not just quoted: pages/vector is a step function of
# how many (element + neighbours) pairs fit in 8 KB.
hnsw_census() {
	local idx=$1 tag=$2 rows=$3
	$PSQL -tA -F$'\t' -c "
		WITH p AS (SELECT (lower - 24) / 4 AS items
		             FROM generate_series(1, pg_relation_size('$idx') / 8192 - 1) b,
		                  LATERAL page_header(get_raw_page('$idx', b::int)))
		SELECT '$tag', $rows, pg_relation_size('$idx') / 8192 AS pages,
		       sum(items) / 2 AS elements, min(items), max(items),
		       round(avg(items), 3), pg_relation_size('$idx')
		  FROM p" | tee -a "$OUT/hnsw_census.tsv"
}
printf 'tag\trows\tpages\telements\tmin_items\tmax_items\tavg_items\tbytes\n' > "$OUT/hnsw_census.tsv"

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
		       = (SELECT count(DISTINCT (segno, blkno)) FROM weave_vec_strips('tw_idx'))
		   AND (SELECT sum(bytes) FROM weave_index_size_detail('tw_idx')) = pg_relation_size('tw_idx')
		   AND (SELECT sum(nvec) FROM weave_vec_meta('tw_idx')) = $N
		   AND (SELECT bool_and(bits = 4 AND dim = $D) FROM weave_vec_meta('tw_idx'))")
	[ "$chk" = t ] || { echo "dim $D: consistency check failed" >&2; exit 1; }

	$PSQL -tA -F$'\t' >> "$OUT/vmajor_bytes.tsv" <<-SQL
		WITH sd AS (SELECT kind, npages, bytes FROM weave_index_size_detail('tw_idx')),
		     vm AS (SELECT count(*) AS segs, sum(nblocks) AS nblocks FROM weave_vec_meta('tw_idx')),
		     -- per PAGE, since a v4 page may carry a lane strip and its block's
		     -- centroid strip: a lane page is any page with a lane strip on it, a
		     -- centroid page one that carries ONLY a centroid strip (v3, or a v4
		     -- dim whose centroid spills)
		     pg AS (SELECT segno, blkno, bool_or(NOT centroid) AS has_lane
		              FROM weave_vec_strips('tw_idx') GROUP BY segno, blkno),
		     st AS (SELECT count(*) FILTER (WHERE has_lane) AS lane_pages,
		                   count(*) FILTER (WHERE NOT has_lane) AS cen_pages
		              FROM pg),
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
	hnsw_census tp_hnsw "$D" "$N"
	$PSQL -c "DROP TABLE tw, tp"
done

# Duplicate control for the HNSW denominator.  pgvector's build stores a vector
# equal to one already in the graph as an extra heap TID on the existing element
# rather than as a new element, so an HNSW over a corpus with duplicates is
# smaller per ROW than one over unique vectors.  If that is why the recorded
# GIST-1M figure (8,056 B/row) is below 8,192, this arm shows it directly:
# DUPN unique vectors plus DUPX exact copies of some of them must build
# DUPN elements, not DUPN + DUPX.
DUPN=${DUPN:-20000}
DUPX=${DUPX:-2000}
say "duplicate control: $DUPN unique + $DUPX copies, 960-d"
$PSQL -c "DROP TABLE IF EXISTS tp" -c "CREATE TABLE tp (id int, v vector(960))"
/tmp/gen "$DUPN" 960 4242 | $PSQL -c "COPY tp (id, v) FROM STDIN"
$PSQL -c "INSERT INTO tp SELECT id + $DUPN, v FROM tp WHERE id <= $DUPX"
$PSQL -c "SET maintenance_work_mem = '4GB'" -c "SET max_parallel_maintenance_workers = 7" \
      -c "CREATE INDEX tp_hnsw ON tp USING hnsw (v vector_cosine_ops) WITH (m = 16, ef_construction = 64)"
hnsw_census tp_hnsw dup960 $((DUPN + DUPX))
$PSQL -c "DROP TABLE tp"

# Optional: count exact duplicates in GIST-1M itself, which is what would make
# the duplicate arm's mechanism the explanation of the recorded figure rather
# than merely a candidate.  Raw records are hashed, so no 11 GB text dump.
if [ "${GIST_DUPS:-0}" = 1 ]; then
	say "GIST-1M duplicate count"
	mkdir -p /scratch/corpus && cd /scratch/corpus
	if [ ! -f gist/gist_base.fvecs ]; then
		timeout 1500 curl -sS --connect-timeout 30 -O ftp://ftp.irisa.fr/local/texmex/corpus/gist.tar.gz \
			&& tar xzf gist.tar.gz && rm -f gist.tar.gz || echo "GIST fetch failed; skipping"
	fi
	if [ -f gist/gist_base.fvecs ]; then
		python3 - gist/gist_base.fvecs <<-'PY' | tee "$OUT/gist_dups.txt"
			import hashlib, struct, sys
			f = open(sys.argv[1], 'rb')
			dim = struct.unpack('<i', f.read(4))[0]; f.seek(0)
			rec = 4 + 4 * dim; seen = set(); n = zero = 0
			z = bytes(4 * dim)
			while True:
			    b = f.read(rec)
			    if len(b) < rec:
			        break
			    n += 1
			    if b[4:] == z:
			        zero += 1
			        continue
			    seen.add(hashlib.blake2b(b[4:], digest_size=16).digest())
			print(f"gist rows={n} zero={zero} nonzero={n - zero} distinct={len(seen)} dup_rows={n - zero - len(seen)}")
		PY
	fi
	cd - >/dev/null
fi

say "done"
cat "$OUT/vmajor_raw.tsv"
echo
cat "$OUT/vmajor_bytes.tsv"
echo
cat "$OUT/hnsw_census.tsv"
[ -f "$OUT/gist_dups.txt" ] && cat "$OUT/gist_dups.txt"
ls -la "$OUT"
