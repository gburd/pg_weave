#!/usr/bin/env bash
#
# bench/hnsw_base.sh -- the pgvector HNSW baseline, measured instead of assumed.
#
# Runs ON its own benchmark instance (bench/aws/run.sh job `hnswbase`). One engine
# per host: pg_weave's side of this comparison runs on a different instance via
# bench/rerank_cold.sh, because a shared buffer cache and shared thermals make a
# two-engine comparison meaningless.
#
# THREE THINGS PHASE V CURRENTLY ASSUMES AND THIS MEASURES
#
# 1. THE DENOMINATOR. `size <= 0.15x pgvector HNSW` has been evaluated against an
#    ESTIMATE -- doc/specs/VECTOR_CHANNEL.md sect. 2.1.1 says "on the order of 5,700 B
#    per vector at m = 16 (unmeasured on this corpus -- measure before quoting
#    it)". Every x-HNSW figure in every Phase V document divides by that guess.
#    Here it is `pg_relation_size` on a real index.
#
# 2. WHETHER THE BASELINE CAN EVEN CLEAR THE RECALL BAR. VECTOR_CHANNEL.md sect. 8a
#    records, from pg_turbovec, that pgvector HNSW never reached R@10 = 0.99 at
#    any `ef` on its corpus (ef = 400 topped out at 0.983). If that reproduces
#    here, then `p50 <= 2x pgvector HNSW at recall@10 >= 0.99` compares against an
#    operating point the baseline does not have, and the gate needs restating
#    rather than passing or failing. That is worth discovering here rather than at
#    the end.
#
# 3. THE LATENCY, COLD. The gate is a cold-cache comparison, so every arm is
#    sampled the same way bench/rerank_cold.sh samples: sync, drop_caches, restart
#    PostgreSQL, one query, with `EXPLAIN (ANALYZE, BUFFERS)` so that the shared
#    read count sits beside the milliseconds and a sample that was not cold is
#    visible as one that did not read.
#
# GROUND TRUTH IS RECOMPUTED, NOT READ FROM THE CORPUS
#
# GIST ships gist_groundtruth.ivecs: exact L2 neighbours of the RAW vectors. Rows
# here are L2-normalized to match bench/ivf_recall.c's geometry (inner product on
# unit vectors = cosine), and ranking under L2 agrees with ranking under cosine
# only on unit-norm data -- so the shipped file describes a different problem.
# Ground truth is therefore an exact sequential scan over the same table, which is
# a stronger reference anyway: it cannot disagree with the data it came from.
#
set -euo pipefail

CORPUS=${CORPUS:-/scratch/corpus/gist/gist_base.fvecs}
QUERIES=${QUERIES:-/scratch/corpus/gist/gist_query.fvecs}
NROWS=${NROWS:-1000000}
NSAMP=${NSAMP:-25}
DIM=${DIM:-960}
EFS=${EFS:-"10 40 100 200 400 800"}
M=${M:-16}
EFC=${EFC:-64}
MWM=${MWM:-24GB}
PARW=${PARW:-7}
OUT=${OUT:-$HOME/out}
WORK=${WORK:-/scratch/hnswbase}
PSQL="psql -X -q -v ON_ERROR_STOP=1"

mkdir -p "$OUT" "$WORK"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- load
if [ "$($PSQL -tAc "SELECT count(*) FROM pg_class WHERE relname='vec'")" = 0 ]; then
	say "building fvecs_dump"
	gcc -O2 -o "$WORK/fvecs_dump" "$(dirname "$0")/fvecs_dump.c" -lm
	say "loading $NROWS x ${DIM}-d into pgvector (normalized)"
	$PSQL -c "CREATE EXTENSION IF NOT EXISTS vector"
	$PSQL -c "CREATE TABLE vec (id int PRIMARY KEY, v vector($DIM))"
	"$WORK/fvecs_dump" "$CORPUS" "$NROWS" 0 2>"$OUT/load.err" \
		| $PSQL -c "COPY vec FROM STDIN"
	$PSQL -c "VACUUM (ANALYZE, FREEZE) vec"
	cat "$OUT/load.err"
fi

say "load integrity"
# The norm is computed as sqrt(-(v <#> v)) rather than with l2_norm(): pgvector
# defines l2_norm for halfvec and sparsevec, so calling it on a `vector` is
# reachable only through implicit casts and errors with "function l2_norm(vector)
# is not unique". `<#>` is negative inner product, so -(v <#> v) is v.v.
$PSQL -tA -F$'\t' -c \
	"SELECT count(*), count(v), min(vector_dims(v)), max(vector_dims(v)),
	        round(min(sqrt(-(v <#> v)))::numeric,6),
	        round(max(sqrt(-(v <#> v)))::numeric,6) FROM vec" \
	| tee "$OUT/integrity.tsv"
read -r n nv dmin dmax nmin nmax < <(tr '\t' ' ' < "$OUT/integrity.tsv")
[ "$n" = "$nv" ] || { echo "FAIL: NULL vectors" >&2; exit 1; }
[ "$dmin" = "$DIM" ] && [ "$dmax" = "$DIM" ] || { echo "FAIL: dims" >&2; exit 1; }
awk -v lo="$nmin" -v hi="$nmax" 'BEGIN{ if (lo < 0.999 || hi > 1.001) {
	printf "FAIL: norms %.6f..%.6f not unit\n", lo, hi; exit 1 } }'
say "integrity ok: $n rows, dim $DIM"

# ---------------------------------------------------------------- queries + GT
gcc -O2 -o "$WORK/fvecs_dump" "$(dirname "$0")/fvecs_dump.c" -lm 2>/dev/null || true
"$WORK/fvecs_dump" "$QUERIES" "$NSAMP" 0 >"$WORK/q.tsv" 2>/dev/null
[ "$(wc -l <"$WORK/q.tsv")" -ge "$NSAMP" ] || { echo "FAIL: too few queries" >&2; exit 1; }

if [ "$($PSQL -tAc "SELECT count(*) FROM pg_class WHERE relname='gt'")" = 0 ]; then
	say "exact ground truth by sequential scan ($NSAMP queries)"
	$PSQL -c "CREATE TABLE gt (s int, rank int, id int)"
	for s in $(seq 1 "$NSAMP"); do
		q=$(sed -n "${s}p" "$WORK/q.tsv" | cut -f2)
		# No index exists yet, so this is exact by construction rather than by
		# a planner setting that could be overridden.
		$PSQL -c "INSERT INTO gt
			SELECT $s, row_number() OVER (), id FROM (
			  SELECT id FROM vec ORDER BY v <=> '$q'::vector LIMIT 10) t"
	done
	$PSQL -c "VACUUM ANALYZE gt"
fi

# ---------------------------------------------------------------- build
if [ "$($PSQL -tAc "SELECT count(*) FROM pg_class WHERE relname='vec_hnsw'")" = 0 ]; then
	# maintenance_work_mem must hold the whole graph or pgvector falls back to an
	# on-disk build that is far slower and, more importantly, is not the
	# configuration anyone deploys.  A 1M x 960-d graph is ~5 GB, so the harness
	# default of 2 GB would have measured the fallback path and called it HNSW.
	# pgvector emits a NOTICE when it switches; it is kept in the log deliberately
	# so that a build which did fall back is visible rather than inferred.
	say "building HNSW (m=$M, ef_construction=$EFC) -- this is the slow step"
	/usr/bin/time -f 'hnsw_build_seconds %e' \
		$PSQL -c "SET maintenance_work_mem = '$MWM';
		          SET max_parallel_maintenance_workers = $PARW;
		          CREATE INDEX vec_hnsw ON vec USING hnsw (v vector_cosine_ops)
		          WITH (m = $M, ef_construction = $EFC)" 2>&1 | tee "$OUT/build.log"
	if grep -qi 'hnsw graph no longer fits\|building index in-memory\|external' "$OUT/build.log"; then
		grep -i 'notice\|warning' "$OUT/build.log" | head -5
	fi
fi

say "index size -- THE DENOMINATOR every x-HNSW figure divides by"
$PSQL -tA -F$'\t' <<-SQL | tee "$OUT/size.tsv"
	SELECT pg_size_pretty(pg_relation_size('vec_hnsw')),
	       pg_relation_size('vec_hnsw'),
	       round(pg_relation_size('vec_hnsw')::numeric / $NROWS, 1) AS bytes_per_vector,
	       pg_size_pretty(pg_total_relation_size('vec')) AS table_total
SQL
HNSW_BPV=$(cut -f3 "$OUT/size.tsv")
say "pgvector HNSW = $HNSW_BPV bytes/vector at ${DIM}-d"

# ---------------------------------------------------------------- recall + latency
# Probe SQL is generated per (ef, sample) with the vector as a literal, so no
# probe table has to be read on a cold cache.
for ef in $EFS; do
	for s in $(seq 1 "$NSAMP"); do
		q=$(sed -n "${s}p" "$WORK/q.tsv" | cut -f2)
		printf "SET hnsw.ef_search = %s;\nEXPLAIN (ANALYZE, BUFFERS) SELECT id FROM vec ORDER BY v <=> '%s'::vector LIMIT 10;\n" \
			"$ef" "$q" >"$WORK/p-$ef-$s.sql"
		printf "SET hnsw.ef_search = %s;\nINSERT INTO res SELECT %s, %s, id FROM (SELECT id FROM vec ORDER BY v <=> '%s'::vector LIMIT 10) t;\n" \
			"$ef" "$ef" "$s" "$q" >"$WORK/i-$ef-$s.sql"
	done
done

# GUARD, before any recall number is computed: the index must actually be used.
# A sequential scan answers these queries exactly, so a planner that declines the
# index reports recall@10 = 1.0000 at every ef -- indistinguishable from a perfect
# index by inspection, and wrong. This is the same failure shape as the local
# guard that could not see what it was asserting about, so it is checked rather
# than assumed.
say "plan guard: the HNSW index must be used"
$PSQL -f "$WORK/p-$(echo $EFS | awk '{print $1}')-1.sql" >"$WORK/plan.txt" 2>&1
if ! grep -q 'vec_hnsw' "$WORK/plan.txt"; then
	echo "GUARD FAILED: the plan does not use vec_hnsw -- every recall figure would be 1.0000 by seq scan" >&2
	head -20 "$WORK/plan.txt" >&2
	exit 1
fi
say "plan guard ok: $(grep -m1 'Index Scan\|Scan using' "$WORK/plan.txt" | sed 's/^ *//')"

say "recall@10 by ef (warm, against the exact scan)"
# Computed in SQL, not with comm(1): comm requires collation order while the ids
# are numeric, so `sort -n | comm` fails outright ("file 1 is not in sorted
# order") and `sort | comm` would silently compare 10 against 100 as strings.
$PSQL -c "DROP TABLE IF EXISTS res; CREATE TABLE res (ef int, s int, id int)"
for ef in $EFS; do
	for s in $(seq 1 "$NSAMP"); do $PSQL -f "$WORK/i-$ef-$s.sql" >/dev/null; done
done
$PSQL -tA -F$'\t' <<-SQL | tee "$OUT/recall.tsv"
	SELECT r.ef,
	       round(count(g.id)::numeric / (count(DISTINCT r.s) * 10), 4) AS recall_at_10,
	       count(*) / count(DISTINCT r.s) AS rows_returned_per_query
	FROM res r LEFT JOIN gt g ON g.s = r.s AND g.id = r.id
	GROUP BY r.ef ORDER BY r.ef;
SQL

sample() {
	local ef=$1 arm=$2 s=$3 f=$WORK/p-$1-$3.sql log=$WORK/last.txt
	if [ "$arm" = cold ]; then
		sync
		echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
		sudo pg_ctlcluster 17 main restart
		$PSQL -tAc 'SELECT 1' >/dev/null
	fi
	$PSQL -f "$f" >"$log" 2>&1 || { cat "$log"; return 1; }
	awk -v ef="$ef" -v arm="$arm" -v s="$s" '
		/Execution Time:/ { ms = $3 }
		/Buffers: shared/ { for (i = 1; i <= NF; i++) {
		                      if ($i ~ /^hit=/)  { sub(/hit=/, "", $i);  hit += $i }
		                      if ($i ~ /^read=/) { sub(/read=/, "", $i); rd  += $i } } }
		END { printf "%s\t%s\t%s\t%.3f\t%d\t%d\n", ef, arm, s, ms, rd, hit }
	' "$log"
}

: >"$OUT/samples.tsv"
for ef in $EFS; do
	say "ef=$ef: $NSAMP warm + $NSAMP cold"
	$PSQL -c "SELECT pg_prewarm('vec_hnsw')" >/dev/null 2>&1 || true
	for s in $(seq 1 "$NSAMP"); do sample "$ef" warm "$s" >>"$OUT/samples.tsv"; done
	for s in $(seq 1 "$NSAMP"); do sample "$ef" cold "$s" >>"$OUT/samples.tsv"; done
done

say "results"
printf 'ef\tarm\tn\tp50_ms\tp99_ms\tmean_reads\tmean_hits\n' | tee "$OUT/summary.tsv"
sort -t$'\t' -k1,1n -k2,2 -k4,4g "$OUT/samples.tsv" | awk -F'\t' '
	{ key = $1 "\t" $2; ms[key][++c[key]] = $4; rd[key] += $5; ht[key] += $6 }
	END { for (k in c) { n = c[k]
		printf "%s\t%d\t%.3f\t%.3f\t%.1f\t%.1f\n", k, n, ms[k][int((n+1)/2)],
		       ms[k][n > 1 ? int(n * 0.99 + 0.5) : 1], rd[k] / n, ht[k] / n } }' \
	| sort -t$'\t' -k1,1n -k2,2r | tee -a "$OUT/summary.tsv"

say "recall by ef"
cat "$OUT/recall.tsv"
awk -F'\t' '
	$3 != 10 { printf "WARNING ef=%d returned %s rows per query, not 10\n", $1, $3 }
	$2 >= 0.99 && !found { found = 1; printf "pgvector HNSW reaches recall@10 %.4f at ef=%d\n", $2, $1 }
	END { if (!found) print "pgvector HNSW NEVER reaches recall@10 0.99 at any ef measured -- so the gate p50 <= 2x HNSW at recall >= 0.99 compares against an operating point the baseline does not have" }' \
	"$OUT/recall.tsv" | tee "$OUT/verdict.txt"

say "artifacts in $OUT"
