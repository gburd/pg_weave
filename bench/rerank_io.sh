#!/usr/bin/env bash
#
# bench/rerank_io.sh -- what does one heap rerank candidate actually cost in pages?
#
# WHY THIS EXISTS
#
# bench/RESULTS_BITWIDTH_SWEEP.md left Phase V with exactly one surviving shape:
# 3-bit codes in the index (0.067x pgvector HNSW) plus an EXACT float32 rerank of
# a top-100 window, which measured recall@10 = 1.0000 on both corpora. A *stored*
# float32 sidecar is 4*dim bytes and is refuted by arithmetic, so the rerank must
# read full precision from the heap, where the row already keeps it.
#
# doc/specs/VECTOR_CHANNEL.md 2.1.1 prices that as "up to 100 heap fetches" and
# calls the cold-cache figure the one that decides the gate. That price is
# probably wrong, and in the expensive direction:
#
#   wvec is declared STORAGE = external (sql/pg_weave--0.1.0--0.2.0.sql:47).
#
# A 1024-d wvec is 4 * 1024 + header = ~4104 bytes, which is over
# TOAST_TUPLE_THRESHOLD, so the toaster pushes it OUT OF LINE. Fetching it back
# is then a btree descent of the toast index plus ceil(payload / 1996) chunk
# reads -- NOT one heap fetch. If that is ~5 pages per candidate instead of 1,
# the cold estimate for a 100-candidate window is off by ~5x, and it is off
# before any implementation exists to be blamed for it.
#
# The effect is also bimodal in dim, which no Phase V document currently says:
# a 200-d vector (800 bytes) leaves the tuple under the threshold and stays
# INLINE, so GloVe-200d reranks from the heap page itself and pays nothing extra,
# while the 1024-d gate corpus pays the toast path on every candidate. A
# conclusion drawn at 200-d does not transfer to 1024-d.
#
# WHAT THIS MEASURES, AND WHAT IT DOES NOT
#
# Authoritative here: PAGES TOUCHED per query, split heap / toast / toast-index,
# from pg_statio_all_tables. Page counts are a property of the storage layout and
# are device-independent, so a laptop and a c7i.8xlarge must agree on them.
#
# NOT authoritative here: latency. Local numbers run against a warm OS page
# cache, and the deciding figure for the Phase V gate is cold-cache p50 against
# pgvector HNSW. Converting pages to cold latency needs EBS plus drop_caches;
# that is the EC2 follow-up, and this script deliberately reports pages so that
# the follow-up has a prediction to be checked against rather than a fresh guess.
#
# The latency column is further inflated by md5() itself, which costs microseconds
# per 4KB where the real rerank is a dot product costing well under one. That is
# tolerable precisely because the deciding run is I/O-bound -- against ~300us of
# EBS random read per page, md5's few microseconds are noise -- but it does mean
# the WARM latency here must not be read as the rerank's CPU cost.
#
# THE GUARDS, WHICH ARE THE POINT
#
# Three broken guards in a row this cycle read exactly like working ones, so this
# script does not trust its own hot query. Two things can silently make the answer
# look cheap:
#
#  (1) the corpus fitting in shared_buffers, so reads are counted as absence of
#      work. Guarded by CONFIGURATION, before any query runs: the toast relation
#      must exceed 4x shared_buffers or the run aborts. The first smoke run of
#      this script failed exactly this way -- 20k rows fit in the pool, the full
#      arm read 0.139 toast pages per candidate, and it looked like a result.
#
#  (2) the hot query not actually materialising the vector. On external storage
#      length() and substring() fetch only the slices they need, so a query that
#      LOOKS like a detoast can read one chunk. Guarded by DISCRIMINATION at
#      GUARD_DIM: a slice arm must read strictly fewer pages than a full arm.
#
# Guard (2) only works where a value spans several PAGES, which is why it runs at
# GUARD_DIM=8192 and not at the dims under test. Learned the hard way: at 768-d
# the slice and full arms both measured 0.964 toast pages per candidate, and the
# first version of this script called that a failed guard. It was not. It is the
# packing described below -- both arms touch the same single page, so page counts
# cannot distinguish them no matter how many chunks each arm reads. A guard whose
# unit is wrong reports failures as confidently as successes.
#
# PAGES ARE NOT CHUNKS, WHICH IS THE FINDING THAT KILLED THE HYPOTHESIS ABOVE
#
# The "~5 pages per candidate" fear at the top of this file is WRONG, and measured
# to be wrong. A value's toast chunks are inserted consecutively and ~4 chunks of
# 2032 bytes pack into one 8KB toast page, so the chunks of one vector overwhelmingly
# share a page. Verified directly rather than reasoned: at 1536-d (4 chunks) the
# toast relation holds exactly 250,000 pages for 250,000 values -- 1.000 pages per
# value -- and the measured read count is 0.985 per candidate.
#
# So a rerank candidate costs about ONE random page read, out of line or not, and
# the toast path is ~1.5x the inline path rather than ~5x. That is a much better
# answer than the hypothesis, and it is recorded here at the same volume as the
# hypothesis was.
#
# MODELLING NOTE
#
# The table stores a bytea with STORAGE = external rather than a wvec. PostgreSQL's
# toaster is type-agnostic -- it sees a varlena length and a storage class -- so a
# 4104-byte external bytea and a 4104-byte external wvec toast identically. Using
# bytea keeps this measurement independent of pg_weave's own code, which is the
# right dependency direction for a number that is supposed to constrain a design
# decision.
#
# The row here is (id int, vec bytea) and nothing else, which is the CHEAPEST
# possible rerank: a real document row also carries the text body, which pushes
# more out of line and can only add pages. Every number below is a LOWER BOUND on
# the real cost. If the lower bound misses the gate, the gate is missed.
#
set -euo pipefail

DIMS=${DIMS:-"200 384 768 1024 1536"}
GUARD_DIM=${GUARD_DIM:-8192}   # only job: prove the full arm really detoasts
GUARD_ROWS=${GUARD_ROWS:-40000} # 32KB/value, so fewer rows still overflow the pool
NROWS=${NROWS:-250000}         # ~1GB of toast at 1024-d, ~30x shared_buffers
WINDOW=${WINDOW:-100}          # Phase V's rerank window
CLIENTS=${CLIENTS:-1}
TIME_S=${TIME_S:-20}
SB=${SB:-32MB}                 # small on purpose: force reads, do not cache the corpus
BASE=${BASE:-/scratch/pgw-rerank-io}
OUT=${OUT:-$BASE/out}

PGDATA=$BASE/pgdata
export PGHOST=$BASE/sock
export PGPORT=${PGPORT:-55433}
export PGDATABASE=bench
export PGUSER=${PGUSER:-$(id -un)}

mkdir -p "$BASE" "$OUT" "$PGHOST"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $PGDATABASE"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

start_cluster() {
	if [ ! -s "$PGDATA/PG_VERSION" ]; then
		say "initdb $PGDATA"
		initdb -D "$PGDATA" -U "$PGUSER" --no-locale --encoding=UTF8 >/dev/null
	fi
	pg_ctl -D "$PGDATA" -s -w -o "-k $PGHOST -c listen_addresses= \
		-c shared_buffers=$SB -c max_connections=32 \
		-c fsync=off -c full_page_writes=off -c synchronous_commit=off \
		-c track_io_timing=on -c max_wal_size=8GB" start
	psql -X -q -d postgres -c "SELECT 1" >/dev/null
	psql -X -q -d postgres -tAc \
		"SELECT 1 FROM pg_database WHERE datname='bench'" | grep -q 1 ||
		psql -X -q -d postgres -c "CREATE DATABASE bench" >/dev/null
}

stop_cluster() { pg_ctl -D "$PGDATA" -s -w -m fast stop || true; }
trap stop_cluster EXIT

# Restarting is how shared_buffers gets cleared between arms. The OS page cache
# survives it, so this buys "cold pool, warm OS" -- enough to force pg_statio to
# count reads, not enough to call the resulting latency a cold number.
recycle() { stop_cluster; start_cluster; }

CURROWS=$NROWS   # rows in the table measure() is about to query

build_table() {
	local dim=$1 bytes=$((4 * $1))
	CURROWS=${2:-$NROWS}
	say "load dim=$dim ($bytes B/vector, $CURROWS rows)"
	$PSQL <<-SQL
		DROP TABLE IF EXISTS v;
		CREATE TABLE v (id int PRIMARY KEY, vec bytea);
		ALTER TABLE v ALTER COLUMN vec SET STORAGE external;
		INSERT INTO v
		SELECT g, set_byte(decode(repeat('a5', $bytes), 'hex'), g % $bytes, g % 256)
		FROM generate_series(1, $CURROWS) g;
		VACUUM (ANALYZE, FREEZE) v;
	SQL

	# GUARD (1), before any timing: a corpus that fits in the pool measures
	# caching. Also record pages-per-value, which is what the read counts must be
	# compared against -- see the packing note in the header.
	$PSQL -tA -F$'\t' <<-SQL >>"$OUT/layout.tsv"
		SELECT $dim,
		       pg_relation_size(oid) / 8192,
		       pg_relation_size(reltoastrelid) / 8192,
		       CASE WHEN pg_relation_size(reltoastrelid) = 0 THEN 0
		            ELSE round((pg_relation_size(reltoastrelid) / 8192.0) / $CURROWS, 3) END,
		       pg_size_pretty(pg_relation_size(reltoastrelid))
		FROM pg_class WHERE relname = 'v';
	SQL
	local tb sb hb
	tb=$($PSQL -tAc "SELECT pg_relation_size(reltoastrelid) FROM pg_class WHERE relname='v'")
	hb=$($PSQL -tAc "SELECT pg_relation_size(oid) FROM pg_class WHERE relname='v'")
	sb=$($PSQL -tAc "SELECT setting::bigint * 8192 FROM pg_settings WHERE name='shared_buffers'")
	# Whichever relation holds the vectors is the one that must not fit: inline
	# configurations are bounded by the main heap, out-of-line ones by the toast.
	local hold=$tb
	[ "$tb" -eq 0 ] && hold=$hb
	if [ "$hold" -lt $((4 * sb)) ]; then
		echo "GUARD FAILED dim=$dim: the relation holding the vectors is $hold B," \
			"under 4x shared_buffers $sb B -- the corpus would be cached and every" \
			"read count below would understate. Raise NROWS or lower SB." >&2
		exit 1
	fi
}

# One (dim, arm) point. Emits a single TSV row.
measure() {
	local dim=$1 arm=$2 expr=$3 spread=$4 ids
	case $spread in
	scattered) ids="SELECT (random() * ($CURROWS - 1))::int + 1 FROM generate_series(1, $WINDOW)" ;;
	clustered) ids="SELECT (random() * ($CURROWS - $WINDOW))::int + g FROM generate_series(1, $WINDOW) g" ;;
	esac

	local script=$OUT/q.sql
	cat >"$script" <<-SQL
		SELECT count($expr) FROM v WHERE id IN ($ids);
	SQL

	recycle
	$PSQL -c "SELECT pg_stat_reset();" >/dev/null
	local raw=$OUT/pgbench-$dim-$arm-$spread.log
	pgbench -n -f "$script" -c "$CLIENTS" -j "$CLIENTS" -T "$TIME_S" \
		-P 10 --progress-timestamp >"$raw" 2>&1 || { cat "$raw"; exit 1; }

	# pg_statio_all_tables reports the parent's toast blocks in toast_blks_*, and
	# the toast index's in tidx_blks_*. Divide by transactions, then by WINDOW, to
	# get pages per rerank candidate -- the quantity the design decision needs.
	$PSQL -tA -F$'\t' <<-SQL >>"$OUT/pages.tsv"
		WITH io AS (
		  SELECT heap_blks_read AS h, idx_blks_read AS i,
		         coalesce(toast_blks_read,0) AS t, coalesce(tidx_blks_read,0) AS ti
		  FROM pg_statio_all_tables WHERE relname = 'v'
		), n AS (
		  SELECT $(grep -oP '(?<=^number of transactions actually processed: )\d+' "$raw" | head -1)::numeric AS q
		)
		SELECT $dim, '$arm', '$spread', n.q,
		       round(io.h / n.q / $WINDOW, 3),
		       round(io.i / n.q / $WINDOW, 3),
		       round(io.t / n.q / $WINDOW, 3),
		       round(io.ti / n.q / $WINDOW, 3),
		       round((io.h + io.i + io.t + io.ti) / n.q / $WINDOW, 3),
		       $(grep -oP '(?<=^latency average = )[0-9.]+' "$raw" | head -1)
		FROM io, n;
	SQL
}

say "postgres $(postgres --version | awk '{print $3}'), shared_buffers=$SB, window=$WINDOW, nrows=$NROWS"
: >"$OUT/pages.tsv"
: >"$OUT/layout.tsv"
start_cluster

for dim in $DIMS; do
	build_table "$dim"
	measure "$dim" full "md5(vec)" scattered
	measure "$dim" slice "substring(vec,1,4)" scattered
	measure "$dim" full "md5(vec)" clustered
done

# GUARD (2): a value spanning several pages, where slice and full CAN differ.
build_table "$GUARD_DIM" "$GUARD_ROWS"
measure "$GUARD_DIM" full "md5(vec)" scattered
measure "$GUARD_DIM" slice "substring(vec,1,4)" scattered

say "results -> $OUT/pages.tsv"
printf 'dim\theap_pages\ttoast_pages\tpages_per_value\ttoast_size\n'
cat "$OUT/layout.tsv"
printf '\ndim\tarm\tspread\tqueries\theap\tidx\ttoast\ttidx\ttotal\tlat_ms\n'
cat "$OUT/pages.tsv"

awk -F'\t' -v gd="$GUARD_DIM" '
  $1 == gd && $2 == "full"  { full  = $9 }
  $1 == gd && $2 == "slice" { slice = $9 }
  END {
    if (slice >= full) {
      printf "GUARD FAILED dim=%s: slice %.3f >= full %.3f total pages/candidate --", gd, slice, full
      print  " the full arm is not materialising the whole vector, so every number above is a floor of unknown depth."
      exit 1
    }
    printf "guard ok at dim=%s: full %.3f vs slice %.3f pages/candidate -- md5() does detoast\n", gd, full, slice
  }
' "$OUT/pages.tsv"
