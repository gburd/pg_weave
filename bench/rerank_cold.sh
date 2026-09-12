#!/usr/bin/env bash
#
# bench/rerank_cold.sh -- cold p50 of a heap rerank, with the real wvec type.
#
# Runs ON the benchmark instance (bench/aws/run.sh job `rerankcold`). Needs
# pg_weave installed, PostgreSQL 17 running, the GIST corpus present, and sudo
# for drop_caches.
#
# WHAT THIS DECIDES
#
# bench/RESULTS_RERANK_IO.md counted the PAGES a heap-rerank candidate costs
# (2.388 at 1024-d) and said explicitly that converting them to latency needs EBS
# plus drop_caches, because the Phase V gate is `p50 <= 2x pgvector HNSW` on a cold
# cache. This is that conversion. The pgvector half runs on a SEPARATE INSTANCE
# (bench/hnsw_base.sh): one engine per host, because a shared buffer cache and
# shared page cache make a two-engine comparison meaningless.
#
# It also removes a caveat rather than adding one. The local run modelled `wvec`
# with a `bytea` of the same length and storage class, which is sound but assumed.
# Here the column is a real `wvec(960)` and the layout is READ BACK from
# pg_class, so the toast geometry is measured on the shipping type.
#
# WHAT COLD MEANS HERE, PRECISELY
#
# Each cold sample is: `sync`, `drop_caches`, restart PostgreSQL, run ONE query.
# Dropping the page cache without restarting leaves shared_buffers populated and
# measures a warm pool over a cold device, which is neither of the two regimes
# that matter. Restarting without dropping caches measures the inverse. Both are
# needed, every sample.
#
# THE GUARD, BECAUSE A COLD SAMPLE THAT WASN'T COLD IS THE FAILURE MODE
#
# Latency alone cannot tell you a sample was cold. So every sample is taken with
# `EXPLAIN (ANALYZE, BUFFERS)` and the script keeps the `shared read` count
# beside the milliseconds. A cold sample must read on the order of the page count
# bench/RESULTS_RERANK_IO.md predicts; one that reads far fewer was served from a
# cache that was supposed to be empty, and the script reports the read count in
# every row so that a suspiciously fast number is visibly a suspiciously cheap
# one. `EXPLAIN ANALYZE` also keeps psql's ~7 ms process startup out of the
# measurement, which at a 17 ms target is not a rounding error.
#
# The warm arm exists for the same reason in reverse: cold/warm must differ by
# roughly the I/O the page counts imply. If they do not, one of the two arms is
# not what it claims.
#
set -euo pipefail

CORPUS=${CORPUS:-/scratch/corpus/gist/gist_base.fvecs}
QUERIES=${QUERIES:-/scratch/corpus/gist/gist_query.fvecs}
NROWS=${NROWS:-1000000}
WINDOWS=${WINDOWS:-"10 20 40 100"}
NSAMP=${NSAMP:-25}
DIM=${DIM:-960}
OUT=${OUT:-$HOME/out}
WORK=${WORK:-/scratch/rerankcold}
PSQL="psql -X -q -v ON_ERROR_STOP=1"

mkdir -p "$OUT" "$WORK"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- load
if [ "$($PSQL -tAc "SELECT count(*) FROM pg_class WHERE relname='vec'")" = 0 ]; then
	say "building fvecs_dump"
	gcc -O2 -o "$WORK/fvecs_dump" "$(dirname "$0")/fvecs_dump.c" -lm

	say "loading $NROWS x ${DIM}-d into wvec (normalized)"
	$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave"
	$PSQL -c "DROP TABLE IF EXISTS vec"
	$PSQL -c "CREATE TABLE vec (id int PRIMARY KEY, v wvec($DIM))"
	# Straight into COPY: a 1M x 960-d dump is ~9 GB of text and writing it to
	# disk first would cost more than the load.
	"$WORK/fvecs_dump" "$CORPUS" "$NROWS" 0 2>"$OUT/load.err" \
		| $PSQL -c "COPY vec FROM STDIN"
	$PSQL -c "VACUUM (ANALYZE, FREEZE) vec"
	cat "$OUT/load.err"
fi

# Load integrity BEFORE any timing (.agent/skills/weave-bench: verify
# correctness before recording a latency). A short or NULL-poisoned load would
# produce faster numbers, not an error.
say "load integrity"
$PSQL -tA -F$'\t' <<-SQL | tee "$OUT/integrity.tsv"
	SELECT count(*), count(v), min(wvec_dims(v)), max(wvec_dims(v)),
	       round(min(wvec_norm(v))::numeric, 6), round(max(wvec_norm(v))::numeric, 6)
	FROM vec;
SQL
read -r n nv dmin dmax nmin nmax < <(tr '\t' ' ' < "$OUT/integrity.tsv")
[ "$n" = "$nv" ] || { echo "FAIL: $((n - nv)) NULL vectors" >&2; exit 1; }
[ "$dmin" = "$DIM" ] && [ "$dmax" = "$DIM" ] || { echo "FAIL: dims $dmin..$dmax != $DIM" >&2; exit 1; }
# Normalized rows must have unit norm. This is the check that would catch a
# loader that emitted raw vectors, which would silently change every distance.
awk -v lo="$nmin" -v hi="$nmax" 'BEGIN{ if (lo < 0.999 || hi > 1.001) {
	printf "FAIL: norms %.6f..%.6f are not unit -- loader did not normalize\n", lo, hi; exit 1 } }'
say "integrity ok: $n rows, dim $DIM, norms $nmin..$nmax"

# ---------------------------------------------------------------- layout
# The real type's toast geometry, read back rather than modelled.
say "storage layout of wvec($DIM)"
$PSQL -tA -F$'\t' <<-SQL | tee "$OUT/layout.tsv"
	SELECT pg_size_pretty(pg_relation_size(oid))            AS heap,
	       pg_relation_size(oid) / 8192                     AS heap_pages,
	       pg_size_pretty(pg_relation_size(reltoastrelid))   AS toast,
	       pg_relation_size(reltoastrelid) / 8192            AS toast_pages,
	       round((pg_relation_size(reltoastrelid) / 8192.0) / $NROWS, 3) AS toast_pages_per_value,
	       pg_size_pretty(pg_total_relation_size(oid))       AS total
	FROM pg_class WHERE relname = 'vec';
SQL

# ---------------------------------------------------------------- probes
# One SQL file per (window, sample). The candidate ids and the query vector are
# baked in as literals so that the warm and cold arms do exactly the same work,
# and so that no probe table has to be read (which would itself be a toast fetch
# on a cold cache and would confound the very thing being measured).
say "generating probes"
gcc -O2 -o "$WORK/fvecs_dump" "$(dirname "$0")/fvecs_dump.c" -lm 2>/dev/null || true
"$WORK/fvecs_dump" "$QUERIES" "$NSAMP" 0 >"$WORK/q.tsv" 2>/dev/null
[ "$(wc -l <"$WORK/q.tsv")" -ge "$NSAMP" ] || { echo "FAIL: too few query vectors" >&2; exit 1; }

for w in $WINDOWS; do
	s=0
	while [ "$s" -lt "$NSAMP" ]; do
		s=$((s + 1))
		q=$(sed -n "${s}p" "$WORK/q.tsv" | cut -f2)
		ids=$(awk -v n="$w" -v hi="$NROWS" -v seed="$((s * 7919 + w))" \
			'BEGIN{srand(seed); printf "{"; for(i=0;i<n;i++) printf "%s%d", (i?",":""), int(rand()*hi)+1; print "}"}')
		printf "EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM vec WHERE id = ANY('%s'::int[]) ORDER BY v <-> '%s'::wvec LIMIT 10;\n" \
			"$ids" "$q" >"$WORK/p-$w-$s.sql"
	done
done

# One sample. Emits: window arm sample ms shared_read shared_hit
sample() {
	local w=$1 arm=$2 s=$3 f=$WORK/p-$1-$3.sql log=$WORK/last.txt

	if [ "$arm" = cold ]; then
		sync
		echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
		sudo pg_ctlcluster 17 main restart
		# The restart's own first connection would otherwise be part of the
		# sample; spend it on something that touches nothing under test.
		$PSQL -tAc 'SELECT 1' >/dev/null
	fi
	$PSQL -f "$f" >"$log" 2>&1 || { cat "$log"; return 1; }
	awk -v w="$w" -v arm="$arm" -v s="$s" '
		/Execution Time:/            { ms = $3 }
		/Buffers: shared/            { for (i = 1; i <= NF; i++) {
		                                 if ($i ~ /^hit=/)  { sub(/hit=/, "", $i);  hit += $i }
		                                 if ($i ~ /^read=/) { sub(/read=/, "", $i); rd  += $i } } }
		END { printf "%s\t%s\t%s\t%.3f\t%d\t%d\n", w, arm, s, ms, rd, hit }
	' "$log"
}

: >"$OUT/samples.tsv"
for w in $WINDOWS; do
	say "window $w: $NSAMP cold + $NSAMP warm"
	# Warm arm first, prewarmed deliberately, so the cold arm is not warmed by it.
	$PSQL -c "SELECT pg_prewarm('vec'), pg_prewarm('vec_pkey')" >/dev/null 2>&1 || true
	for s in $(seq 1 "$NSAMP"); do sample "$w" warm "$s" >>"$OUT/samples.tsv"; done
	for s in $(seq 1 "$NSAMP"); do sample "$w" cold "$s" >>"$OUT/samples.tsv"; done
done

# ---------------------------------------------------------------- report
say "results"
printf 'window\tarm\tn\tp50_ms\tp99_ms\tmean_reads\tmean_hits\n' | tee "$OUT/summary.tsv"
sort -t$'\t' -k1,1n -k2,2 -k4,4g "$OUT/samples.tsv" | awk -F'\t' '
	{ key = $1 "\t" $2; ms[key][++c[key]] = $4; rd[key] += $5; ht[key] += $6 }
	END {
		for (k in c) {
			n = c[k]
			p50 = ms[k][int((n + 1) / 2)]
			p99 = ms[k][n > 1 ? int(n * 0.99 + 0.5) : 1]
			printf "%s\t%d\t%.3f\t%.3f\t%.1f\t%.1f\n", k, n, p50, p99, rd[k] / n, ht[k] / n
		}
	}' | sort -t$'\t' -k1,1n -k2,2r | tee -a "$OUT/summary.tsv"

# GUARD: a cold sample must actually have read pages, and must have read more
# than the warm arm. Equal read counts mean the cache was not dropped and every
# cold number is a warm number with a restart in front of it.
awk -F'\t' 'NR > 1 {
		if ($2 == "cold") { cold[$1] = $6; cms[$1] = $4 }
		if ($2 == "warm") { warm[$1] = $6; wms[$1] = $4 }
	}
	END {
		bad = 0
		for (w in cold) {
			if (cold[w] < 2) {
				printf "GUARD FAILED window=%s: cold arm read %.1f pages -- cache was not dropped\n", w, cold[w]; bad = 1
			} else if (cold[w] <= warm[w]) {
				printf "GUARD FAILED window=%s: cold reads %.1f <= warm reads %.1f -- arms are not distinct\n", w, cold[w], warm[w]; bad = 1
			} else
				printf "window=%s: guard ok (cold %.1f reads / %.3f ms vs warm %.1f reads / %.3f ms)\n", w, cold[w], cms[w], warm[w], wms[w]
		}
		exit bad
	}' "$OUT/summary.tsv"

say "artifacts in $OUT"
