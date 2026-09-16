#!/usr/bin/env bash
#
# bench/ingest_amplification.sh -- reproduce the G20 bulk-ingest measurement, and
# A/B the insert-time-merge gate against it.
#
# G20 (doc/GAPS.md) measured that a body index over term-rich documents extends
# ~100 index pages per document against ~2 pages of real postings, because every
# oversized document mints a one-document segment and the insert path merged after
# every single one.  The freed pages cannot be reused inside the inserting
# transaction -- weave_page_recyclable()'s xid gate correctly refuses them -- so the
# only lever is to rewrite less often.  This script measures the lever.
#
# It reports RAW numbers only: pages, MB, per-arm allocator counters, and segment
# counts.  No ratios, no percentages: bench/RESULTS_INGEST_AMPLIFICATION.md and
# doc/GAPS.md own the arithmetic, and a script that pre-chews its numbers hides the
# arm that was actually run.
#
# Everything is backend-local, so ARM A RUNS IN ONE psql SESSION THROUGHOUT: the
# allocator counters are per-backend and a read from a second backend reports that
# backend's zeros.  Batches are TOP-LEVEL INSERTs, one transaction each, not a loop
# inside a DO block -- a DO block is one transaction, the xid horizon cannot advance
# inside it, and every free page is then trivially unrecyclable.  That artifact has
# now been hit four times in this lineage; see G20's "my own artifact" note.
#
# Prerequisites: a RELOCATED, WRITABLE copy of the PostgreSQL tree with this build
# of pg_weave installed into it (the nix store is read-only):
#
#   PGSTORE=$(nix develop --command pg_config --bindir); PGSTORE=${PGSTORE%/bin}
#   mkdir -p /scratch/pg17 && cp -rL "$PGSTORE"/. /scratch/pg17/ && chmod -R u+w /scratch/pg17
#   nix develop --command bash -c 'make install DESTDIR=/scratch/stage'
#   cp -f /scratch/stage"$PGSTORE"/lib/pg_weave.so /scratch/pg17/lib/
#   cp -f /scratch/stage"$PGSTORE"/share/postgresql/extension/* /scratch/pg17/share/postgresql/extension/
#
# PostgreSQL finds its own share/ and lib/ relative to the postgres executable, so a
# copied tree is self-consistent as long as bin/postgres is a real file (not a
# symlink back into the store, which find_my_exec() would resolve).
#
# Usage:
#   bench/ingest_amplification.sh batches   # arm A: 6 x 1,000 docs, one txn per batch
#   bench/ingest_amplification.sh percommit # arm B: N single-row transactions, max nseg
#
set -euo pipefail

PGROOT=${PGROOT:-/scratch/pg17}
RUNDIR=${RUNDIR:-/scratch/g20data}
PGPORT=${PGPORT:-55440}
NBATCH=${NBATCH:-6}
BATCHSZ=${BATCHSZ:-1000}
NPERCOMMIT=${NPERCOMMIT:-4000}
TERMS=${TERMS:-1660}		# terms per document: already over one pending page
VOCAB=${VOCAB:-50000}		# shared vocabulary, not one private term set per doc

export PATH="$PGROOT/bin:$PATH"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -h $RUNDIR -p $PGPORT -U postgres -d postgres"

say() { printf '\n=== %s\n' "$*"; }

# The document body: TERMS deterministic draws from a VOCAB-term vocabulary.  The
# mixing is arithmetic rather than random() so the corpus is reproducible, and the
# subquery is a genuine LATERAL with an outer reference so it is evaluated per row
# (an uncorrelated sublink becomes an InitPlan and every document comes out
# identical -- which silently turns 1,000 documents into 1,000 copies of one).
body_select() { # $1 = number of docs, $2 = batch seed
	cat <<SQL
SELECT s.body
  FROM generate_series(1,$1) AS d(g),
       LATERAL (SELECT string_agg('v' ||
                  (1 + ((d.g::bigint * 7919 + t.g::bigint * 104729
                         + $2::bigint * 1000003) % $VOCAB))::text, ' ') AS body
                  FROM generate_series(1,$TERMS) AS t(g)) s
SQL
}

setup_sql() {
	cat <<'SQL'
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
CREATE INDEX docs_weave ON docs USING weave (to_wdoc('simple', body));
SELECT weave_alloc_stats_reset();
SQL
}

report_sql() { # $1 = label
	cat <<SQL
SELECT '$1' AS arm,
       pg_relation_size('docs_weave') / 8192 AS pages,
       pg_relation_size('docs_weave') / (1024*1024) AS mb,
       weave_index_nsegments('docs_weave') AS nseg,
       (SELECT count(*) FROM docs) AS rows,
       a.fsm_reuse, a.fsm_defer, a.lowfree_reuse, a.lowfree_defer, a.extend
  FROM weave_alloc_stats() a \gx
SQL
}

case "${1:-batches}" in
batches)
	say "arm A: $NBATCH batches of $BATCHSZ docs, $TERMS terms/doc, one transaction per batch"
	{
		setup_sql
		for b in $(seq 1 "$NBATCH"); do
			echo "INSERT INTO docs(body) $(body_select "$BATCHSZ" "$b");"
			report_sql "batch$b"
		done
		echo "SELECT weave_vacuum('docs_weave');"
		report_sql "after weave_vacuum"
	} >"$RUNDIR/armA.sql"
	time $PSQL -f "$RUNDIR/armA.sql"
	;;
percommit)
	# Worst case for segment MINTING: one oversized document per transaction, so a
	# new one-document segment per commit and nothing amortised across rows.  This
	# is the arm the segment-count safety property lives or dies on, so nsegments
	# is sampled after EVERY commit and the maximum reported -- a final reading
	# would miss a spike, and the failure being guarded against (a directory that
	# creeps to the hard cap and can then neither merge nor VACUUM) is a spike.
	say "arm B: $NPERCOMMIT single-row transactions, $TERMS terms/doc, nseg sampled per commit"
	{
		setup_sql
		echo "DROP TABLE IF EXISTS obs; CREATE TABLE obs (i serial, n int);"
		for i in $(seq 1 "$NPERCOMMIT"); do
			echo "INSERT INTO docs(body) $(body_select 1 "$i");"
			echo "INSERT INTO obs(n) SELECT weave_index_nsegments('docs_weave');"
		done
		# in the SAME session: the allocator counters are backend-local, and a
		# summary run from a second psql invocation reports that backend's zeros
		echo "SELECT count(*) AS commits, max(n) AS max_nseg, min(n) AS min_nseg,
		             (SELECT n FROM obs ORDER BY i DESC LIMIT 1) AS final_nseg
		        FROM obs \gx"
		report_sql "arm B end"
	} >"$RUNDIR/armB.sql"
	time $PSQL -f "$RUNDIR/armB.sql"
	;;
*)
	echo "usage: $0 {batches|percommit}" >&2
	exit 2
	;;
esac
