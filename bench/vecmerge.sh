#!/usr/bin/env bash
#
# vecmerge.sh -- hard rule 12's missing gate: the vector weft's MERGE and VACUUM
# paths, at scale, with weave_check() as the assertion.
#
# WHY THIS SCRIPT EXISTS.  AGENTS.md hard rule 12 says that a release touching
# tombstones, merge or vacuum needs a run at scale before the work counts as done,
# because the sibling project shipped the same P0 through a green local gate twice.
# An audit on 2026-09-23 found that `bench/aws/run.sh` had **no job that builds a
# vector-carrying weave index at scale and no job that calls weave_check() at all**:
# `run_fuse` is the only vector index in the harness and its corpora are a few
# thousand documents, `run_p0merge` is lexical-only, and every other vector job is a
# standalone C binary with no index in it.  So every vector merge/vacuum change this
# project has ever shipped was gated by `installcheck` at 300 rows.  G27
# (`firstpage` in `WeaveVecDirRec`, commit 33dcc1b) is the change that made that
# unacceptable, because a wrong `firstpage` is not an error -- it is a scan reading
# another block's codes and computing a distance in the right units from the wrong
# documents.
#
# WHAT IS UNDER TEST, STATED AS THE FAILURE IT WOULD BE.
#   * A merge re-groups every surviving lane into new blocks and writes a new
#     directory.  If the writer's `firstpage` and the page the strips actually
#     landed on disagree -- which is what `vecwrite.c`'s reordering (strips emitted
#     BEFORE the directory record) exists to prevent -- the index answers wrongly.
#   * A VACUUM rewrite relocates pages through the FSM free list, whose structure
#     depends on vacuum/merge HISTORY and is not predictable from a formula (that is
#     why `firstpage` is stored rather than computed: two vacuum rewrites of the same
#     table were measured producing opposite page structures).  A scale run is the
#     only place where the free list is long enough for that to matter.
#   * `weave_check(deep)` recomputes every directory record from the codes stored
#     beside it AND (since G27) opens the page each record names and asserts its
#     header claims that block.  That is contract (C2) plus the location, on disk,
#     for every block rather than for the ones a query happens to visit.
#
# THE POSITIVE CONTROL IS NOT OPTIONAL, AND IT IS WHY THIS SCRIPT IS LONG.
# AGENTS.md's eleventh and twelfth members: a gate that reports on something other
# than the thing under test makes every green it ever printed uninformative.  Two
# controls run here:
#   1. Every check asserts the invariant `vector_block_stats_match_codes` is PRESENT
#      in weave_check()'s output.  `count(*) WHERE NOT ok = 0` is satisfied just as
#      well by an invariant that did not run -- on a lexical-only index it returns 0
#      forever.
#   2. `VECMERGE_MUTATE=1` rebuilds the extension with the code-page pointer
#      deliberately off by one page and asserts weave_check() NAMES the violation.
#      Until that has fired on this host, with this compiler, silence proves nothing.
#
# WHY THE XID BURN IS IN HERE.  `weave_free_page` stamps `ReadNextTransactionId()`
# and `weave_page_recyclable` asks `GlobalVisCheckRemovableXid()`, so on an IDLE
# cluster NOTHING is recyclable anywhere and any space-reclamation behaviour measured
# there is an artifact (project memory, L18/L19).  The vacuum legs below advance the
# horizon explicitly between cycles.
#
# NOT A LATENCY SCRIPT.  Every number it records is a count -- pages, segments,
# blocks, lanes, violations -- and counts are deterministic and host-independent.  It
# runs on EC2 only because the corpus and the build do not fit comfortably anywhere
# else, not because the machine has to be quiet.
#
# usage: bench/vecmerge.sh
#   NROWS=1000000  rows to load from the corpus
#   DIM=960        corpus dimension (GIST-1M)
#   BATCH=50000    rows per post-build insert batch
#   NBATCH=4       number of insert+merge cycles
#   DELFRAC=10     delete every Nth row before the vacuum legs
#   CORPUS=/scratch/corpus/gist/gist_base.fvecs
#   OUT=$HOME/out
#
# Copyright (c) 2025-2026, Gregory Burd

set -euo pipefail

NROWS=${NROWS:-1000000}
DIM=${DIM:-960}
BATCH=${BATCH:-50000}
NBATCH=${NBATCH:-4}
DELFRAC=${DELFRAC:-10}
# FOUR vacuum cycles, not three, and the reason is the first 1M run.  It failed a
# cycle1-vs-cycle3 ratchet at 190,091 -> 283,924 pages -- while the same script at
# 20k rows produced 2,577 -> 4,039 -> 2,577, an OSCILLATION whose peak simply landed
# on a different cycle.  A three-cycle ratchet cannot tell those apart, because its
# verdict depends on the parity of the cycle the relocation pass happens to fire on.
# Comparing cycles of the SAME parity (3 vs 1, 4 vs 2) separates a trend from a
# swing, and the peak-to-trough ratio is reported either way because a transient 1.5x
# is a real cost to a user even when it is bounded.
VACCYC=${VACCYC:-4}
CORPUS=${CORPUS:-/scratch/corpus/gist/gist_base.fvecs}
OUT=${OUT:-$HOME/out}
WORK=${WORK:-/scratch/vecmerge}
PSQL="psql -X -q -v ON_ERROR_STOP=1"

mkdir -p "$OUT" "$WORK"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }
fail() { printf '\033[31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

REPORT=$OUT/vecmerge.tsv
: >"$REPORT"
# One row per (stage, metric).  A flat long table rather than a wide one, because
# the stages are added to over time and a wide table silently shifts columns.
emit() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$REPORT"; }

# DEFERRED FAILURE, and the first run of this script at 1M rows is why it exists.  The
# page ratchet at the end of the vacuum legs failed, `fail()` exited, and everything
# after it never ran -- including the ANSWER check.  Worse, the mutation control in
# bench/aws/run.sh sits after this script and also never ran, so the eight clean
# weave_check() results the run DID produce were left uninformative: nothing had
# demonstrated that the invariant could fire on that host at all (AGENTS.md, twelfth
# member -- until a check has fired once, its silence is not evidence).
#
# An assertion that is fatal WHERE IT SITS decides what else gets measured, which
# turns the ordering of a script into a policy nobody wrote down.  `defer` records the
# failure, keeps going, and the script exits non-zero at the end.  `fail` is kept for
# the cases where continuing really is meaningless -- a corpus that did not load, a
# weave_check() that errored rather than reported.
DEFERRED=0
defer() {
	printf '\033[31mFAIL (deferred): %s\033[0m\n' "$*" >&2
	printf 'deferred_failure\t%s\t1\n' \
		"$(printf '%s' "$1" | tr -d '\t\n' | cut -c1-70)" >>"$REPORT"
	DEFERRED=$((DEFERRED + 1))
}

# ---------------------------------------------------------------- 1. load
#
# The corpus is loaded ONCE into a staging table and the measured table is derived
# from it, so that the several `CREATE INDEX` legs below never pay the fvecs parse
# again -- and so that the text column is generated from the id rather than from the
# vector, which keeps the lexical channel's content independent of the vector
# channel's.  The text is deliberately low-entropy with a handful of shared terms:
# the merge path under test is the WEFT's, and a huge vocabulary would just make the
# lexical half of every merge dominate the wall clock without adding exposure.
if [ "$($PSQL -tAc "SELECT count(*) FROM pg_class WHERE relname='vmsrc'")" = 0 ]; then
	say "building fvecs_dump"
	gcc -O2 -o "$WORK/fvecs_dump" "$(dirname "$0")/fvecs_dump.c" -lm \
		|| fail "fvecs_dump build failed"

	say "loading $NROWS x ${DIM}-d"
	$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave"
	$PSQL -c "CREATE TABLE vmsrc (id int PRIMARY KEY, v wvec($DIM))"
	"$WORK/fvecs_dump" "$CORPUS" "$NROWS" 0 2>"$OUT/load.err" \
		| $PSQL -c "COPY vmsrc FROM STDIN" || fail "COPY failed"
	$PSQL -c "VACUUM (ANALYZE) vmsrc"
	cat "$OUT/load.err"
fi

# Load integrity BEFORE anything else.  A short or NULL-poisoned load makes every
# count below smaller and nothing complains (weave-bench skill: verify correctness
# before recording a number).
read -r nsrc nvec dmin dmax < <($PSQL -tA -F' ' -c \
	"SELECT count(*), count(v), min(wvec_dims(v)), max(wvec_dims(v)) FROM vmsrc")
[ "$nsrc" = "$nvec" ] || fail "$((nsrc - nvec)) NULL vectors in the staging table"
[ "$dmin" = "$DIM" ] && [ "$dmax" = "$DIM" ] || fail "dims $dmin..$dmax != $DIM"
say "staging table ok: $nsrc rows, dim $DIM"
emit load rows "$nsrc"

# ---------------------------------------------------------------- helpers
#
# An ORDER-INDEPENDENT digest of the live lanes, in constant memory.  The obvious
# implementation -- two temp tables and EXCEPT in both directions, as
# sql/vecindex.sql does at 300 rows -- materializes ~480 MB of codes per snapshot
# here and sorts it twice.  A commutative sum over a per-lane hash costs one
# sequential pass and still catches all three failures, because `docid` is INSIDE
# the hash: a lost lane changes the count, an invented one changes the count, and a
# lane moved to another document changes the sum.  What it cannot catch is a
# hash collision, which is the trade being made deliberately.
lane_digest() {
	$PSQL -tA -F' ' -c "
		SELECT count(*),
		       coalesce(sum(('x' || substr(md5(docid::text || ':' || code::text
		              || ':' || scale::text || ':' || norm::text), 1, 8))::bit(32)::bigint), 0)
		  FROM weave_vec_lanes('$1') WHERE live"
}

# weave_check(deep) plus the control that it examined the vector weft at all.
#
# Returns nothing and dies on violation; prints the violating invariants, because a
# gate that reports FAIL and nothing else costs a round trip and this one runs on a
# remote host (AGENTS.md).
check_clean() {
	local idx=$1 stage=$2 v present
	# `-F$'\t'` rather than concatenating a '\t' inside the SQL string, which under
	# standard_conforming_strings is a literal backslash-t and would make every awk
	# field split below silently wrong.
	$PSQL -tA -F$'\t' -c "SELECT invariant, ok, coalesce(detail, '')
	                        FROM weave_check('$idx', true) ORDER BY invariant" \
		>"$WORK/check.$stage.tsv" || fail "$stage: weave_check() itself errored"

	present=$(grep -c '^vector_block_stats_match_codes' "$WORK/check.$stage.tsv" || true)
	[ "$present" -ge 1 ] || fail \
		"$stage: weave_check() did not report vector_block_stats_match_codes -- the
		 invariant under test did not run, so 'no violations' means nothing here"

	v=$(awk -F'\t' '$2 == "f"' "$WORK/check.$stage.tsv" | wc -l)
	emit "$stage" violations "$v"
	if [ "$v" != 0 ]; then
		awk -F'\t' '$2 == "f"' "$WORK/check.$stage.tsv" >&2
		fail "$stage: $v weave_check violations"
	fi
	say "$stage: weave_check(deep) clean, invariant under test present"
}

# Segments, blocks, and where the bytes are.  Recorded at every stage rather than
# asserted, except for the index-size ratchet in the vacuum legs: the point of the
# table is that a later reader can see whether a merge actually merged.
snapshot() {
	local idx=$1 stage=$2
	emit "$stage" segments "$($PSQL -tAc "SELECT weave_index_nsegments('$idx')")"
	emit "$stage" relpages "$($PSQL -tAc "SELECT pg_relation_size('$idx') / 8192")"
	$PSQL -tA -F' ' -c "SELECT kind, npages FROM weave_index_size_detail('$idx')
	                     WHERE kind LIKE 'vector%' ORDER BY kind" \
		| while read -r k n; do emit "$stage" "$k" "$n"; done
	read -r lanes dig < <(lane_digest "$idx")
	emit "$stage" live_lanes "$lanes"
	emit "$stage" lane_digest "$dig"
	# THE VECTOR / NON-VECTOR SPLIT, EMITTED RATHER THAN LEFT TO BE DERIVED.  The first
	# run of this script failed its page ratchet, and the one question that decided
	# whether G27 was implicated -- is the growth inside the weft or outside it? -- had
	# to be computed by hand from the buckets afterwards.  It is the discriminating
	# number, so it gets a row.  (It was outside: every vector bucket was byte-identical
	# across all three vacuum cycles while the total grew 1.49x.)
	vecp=$($PSQL -tAc "SELECT coalesce(sum(npages), 0) FROM weave_index_size_detail('$idx')
	                    WHERE kind LIKE 'vector%'")
	relp=$($PSQL -tAc "SELECT pg_relation_size('$idx') / 8192")
	emit "$stage" vector_pages "$vecp"
	emit "$stage" nonvector_pages "$((relp - vecp))"
	# Blocks and TOTAL lanes (live + dead), because the merge assertion below needs
	to=$($PSQL -tA -F' ' -c "SELECT count(*), count(DISTINCT blockno)
	                           FROM weave_vec_lanes('$idx')")
	emit "$stage" total_lanes "${to%% *}"
	emit "$stage" blocks "${to##* }"
}

# The horizon has to move or nothing is recyclable and the vacuum legs measure an
# artifact (project memory: three wrong mechanisms came from this).
# Separate psql invocations on purpose: `SELECT txid_current() FROM
# generate_series(1,200)` assigns ONE xid, because it is one transaction.  What has
# to advance is the number of COMMITTED transactions ahead of the freeing xid.
burn_xids() {
	local i
	for i in $(seq 1 25); do
		$PSQL -c "SELECT txid_current()" >/dev/null
	done
}

# ---------------------------------------------------------------- 2. build
say "build: $((NROWS - NBATCH * BATCH)) rows, then $NBATCH x $BATCH inserted, then $VACCYC vacuum cycles"
NBUILD=$((nsrc - NBATCH * BATCH))
[ "$NBUILD" -gt 0 ] || fail "NROWS too small for $NBATCH x $BATCH"

$PSQL -c "DROP TABLE IF EXISTS vm"
$PSQL -c "CREATE TABLE vm (id int PRIMARY KEY, d wdoc, v wvec($DIM))"
$PSQL -c "INSERT INTO vm SELECT id,
            to_wdoc('block' || (id % 997) || ' group' || (id % 31) || ' doc' || id),
            v FROM vmsrc WHERE id <= $NBUILD"
$PSQL -c "VACUUM (ANALYZE) vm"

say "CREATE INDEX (the only producer of a weft today -- see below)"
/usr/bin/time -f 'build %e s' $PSQL -c \
	"CREATE INDEX vm_weave ON vm USING weave (d, v) WITH (metric = 'ip')" \
	2>&1 | tee "$OUT/build_index.log"

check_clean vm_weave build
snapshot vm_weave build
blocks_prev=$(awk -F'\t' '$1 == "build" && $2 == "blocks" {print $3}' "$REPORT")
say "build: $blocks_prev blocks"

# ---------------------------------------------------------------- 3. insert + merge
#
# WHY THE INSERTED ROWS CARRY NULL VECTORS.  doc/GAPS.md G23: an inserted row's
# wvec is not available at pending-flush time, so a bolt produced by INSERT has no
# weft regardless of what the column holds.  Inserting real vectors here would
# therefore load a corpus, show it in `count(v)`, and silently produce DEAD LANES --
# a test that looks like it covers something it does not.  NULL makes the shape
# honest and is what sql/vecindex.sql does for the same reason.
#
# So every merge below is the MIXED case: one input with a weft, N without.  That is
# the only weft-producing merge the code can currently reach, and it is the one that
# rewrites the directory -- which is what G27 changed.
for i in $(seq 1 "$NBATCH"); do
	lo=$((NBUILD + (i - 1) * BATCH + 1))
	hi=$((NBUILD + i * BATCH))
	say "cycle $i: inserting rows $lo..$hi"
	$PSQL -c "INSERT INTO vm SELECT id,
	            to_wdoc('block' || (id % 997) || ' later' || (id % 31) || ' doc' || id),
	            NULL FROM vmsrc WHERE id BETWEEN $lo AND $hi"
	emit "insert$i" segments "$($PSQL -tAc "SELECT weave_index_nsegments('vm_weave')")"

	# Capture the pre-merge live set, then merge, then require the live lanes to be
	# byte-identical.  THE NO-RE-ENCODE PROPERTY: a merge moves codes, it must never
	# re-quantize them, and it must not move a code onto another document.
	read -r lanes_before dig_before < <(lane_digest vm_weave)
	say "cycle $i: weave_merge"
	/usr/bin/time -f "merge$i %e s" $PSQL -c "SELECT weave_merge('vm_weave')" \
		2>&1 | tee -a "$OUT/merge.log"
	read -r lanes_after dig_after < <(lane_digest vm_weave)

	emit "merge$i" live_lanes_before "$lanes_before"
	emit "merge$i" live_lanes_after "$lanes_after"
	[ "$lanes_before" = "$lanes_after" ] || fail \
		"merge$i: live lanes $lanes_before -> $lanes_after; a merge must not add or drop a live lane"
	[ "$dig_before" = "$dig_after" ] || fail \
		"merge$i: live-lane digest changed ($dig_before -> $dig_after) -- a code was
		 re-encoded or moved onto a different document"

	check_clean vm_weave "merge$i"
	snapshot vm_weave "merge$i"

	# THE MERGE HAS TO HAVE DONE SOMETHING, and `weave_index_nsegments()` does not
	# say so: it reads 1 both before and after here, because the inserted rows sit in
	# the pending area rather than in a second bolt until the merge flushes them.  A
	# reader given only that column would reasonably conclude this leg merges nothing.
	# What proves the DIRECTORY was rewritten -- which is the structure G27 changed --
	# is the block count rising by the batch: the merge emitted new blocks, and
	# check_clean then opened the page every one of their records names.
	blocks_now=$(awk -F'\t' -v s="merge$i" '$1 == s && $2 == "blocks" {print $3}' "$REPORT")
	awk -v a="$blocks_prev" -v b="$blocks_now" 'BEGIN { exit (b <= a) }' \
		|| defer "merge$i left the block count at $blocks_now; the directory was not rewritten, so this cycle tested nothing"
	say "cycle $i: blocks $blocks_prev -> $blocks_now, directory rewritten"
	blocks_prev=$blocks_now
done

# ---------------------------------------------------------------- 4. vacuum
#
# The path that killed the three arithmetic addressing designs: a VACUUM rewrite
# relocates pages through the FSM free list, and the free list's structure is a
# function of vacuum/merge history rather than of anything the writer can compute.
# Two cycles, with the xid horizon advanced between them, because the first VACUUM
# after a big delete legitimately GROWS the index by the livedocs tombstone blob and
# only later cycles can be held to a ratchet (project memory, L19).
say "deleting every ${DELFRAC}th row"
$PSQL -c "DELETE FROM vm WHERE id % $DELFRAC = 0"
emit delete rows_deleted "$($PSQL -tAc "SELECT count(*) FROM vmsrc WHERE id % $DELFRAC = 0")"

for cyc in $(seq 1 "$VACCYC"); do
	burn_xids
	say "vacuum cycle $cyc"
	/usr/bin/time -f "vacuum$cyc %e s" $PSQL -c "VACUUM vm" \
		2>&1 | tee -a "$OUT/vacuum.log"
	check_clean vm_weave "vacuum$cyc"
	snapshot vm_weave "vacuum$cyc"
done

# THE RATCHET, ON MATCHING PARITY.  Later cycles are compared to the FIRST vacuum,
# never to the pre-vacuum size, for the tombstone-blob reason above -- and only to
# cycles of the same parity, for the oscillation reason at the top of this file.
pg() { awk -F'\t' -v s="vacuum$1" -v k="$2" '$1 == s && $2 == k {print $3}' "$REPORT"; }
SERIES=""
for cyc in $(seq 1 "$VACCYC"); do SERIES="$SERIES $(pg "$cyc" relpages)"; done
VSERIES=""
for cyc in $(seq 1 "$VACCYC"); do VSERIES="$VSERIES $(pg "$cyc" vector_pages)"; done
say "relpages across $VACCYC vacuum cycles:$SERIES"
say "vector pages across the same cycles:$VSERIES"
emit vacuum relpages_series "$(printf '%s' "$SERIES" | tr -s ' ' ',' | sed 's/^,//')"
emit vacuum vector_pages_series "$(printf '%s' "$VSERIES" | tr -s ' ' ',' | sed 's/^,//')"

# The transient, reported rather than asserted: peak / trough over the series.
read -r pk tr <<<"$(printf '%s\n' $SERIES | awk 'NR==1{mn=mx=$1} {if($1>mx)mx=$1; if($1<mn)mn=$1} END{print mx, mn}')"
emit vacuum peak_to_trough "$(awk -v a="$pk" -v b="$tr" 'BEGIN{printf "%.3f", a/b}')"
say "peak/trough over the series: $pk / $tr = $(awk -v a="$pk" -v b="$tr" 'BEGIN{printf "%.2fx", a/b}')"

for cyc in $(seq 3 "$VACCYC"); do
	earlier=$((cyc - 2))
	a=$(pg "$earlier" relpages); b=$(pg "$cyc" relpages)
	av=$(pg "$earlier" vector_pages); bv=$(pg "$cyc" vector_pages)
	awk -v a="$a" -v b="$b" 'BEGIN { exit (b > a * 1.05) }' \
		|| defer "vacuum$cyc is $b pages against vacuum$earlier's $a -- same parity, so this is a TREND and not the oscillation (doc/GAPS.md G18 shape); vector pages $av -> $bv is what says whether the weft is implicated"
done

# ---------------------------------------------------------------- 5. the answer
#
# Everything above is structural.  This asks the scan the question a wrong
# `firstpage` would answer incorrectly WITHOUT erroring: does the index's own top-k
# agree with an exact sequential scan over the same table?  The recheck is on a
# sample of queries because the exact arm is a full pass per query.
# RECALL, NOT EQUALITY, AND THE DIFFERENCE MATTERS.  The index answers from 4-bit
# codes, so its top-10 is not required to equal the exact top-10 and an equality
# assertion here would fail for a reason that has nothing to do with this gate.  What
# a wrong `firstpage` produces is not a near miss: the scan's code cursor refuses a
# page whose header does not claim the block block-major order calls for, so the
# query ERRORS -- and if it somehow did not, it would be scoring other documents'
# codes and recall would collapse toward zero.  The floor below is therefore set
# where only a broken scan can miss it, and the MEASURED value is what gets recorded.
say "answer check: index recall@10 against an exact sequential scan"
NQ=${NQ:-10}
$PSQL -tAc "SELECT v::text FROM vmsrc WHERE id % (($nsrc / $NQ)) = 1 LIMIT $NQ" \
	>"$WORK/queries.txt"
[ "$(wc -l <"$WORK/queries.txt")" -ge 3 ] || fail "could not sample query vectors"

: >"$OUT/answers.tsv"
while IFS= read -r qv; do
	lit=${qv//\'/\'\'}
	# The exact arm forbids every index path, so it is a sequential scan computing
	# float distances -- the reference.  The index arm forbids only bitmapscan, and
	# the plan is asserted to be an index scan, because a plan that fell back to a
	# seq scan would agree with the reference perfectly while testing nothing.
	plan=$($PSQL -tAc "SET enable_bitmapscan = off;
	                   EXPLAIN (COSTS OFF) SELECT id FROM vm
	                     ORDER BY v <#> '$lit'::wvec LIMIT 10" | tr -d ' ')
	case $plan in
		*IndexScan*|*CustomScan*|*Weave*) ;;
		*) printf '%s\n' "$plan" >&2
		   fail "the index arm is not using the index -- see the plan above" ;;
	esac
	idxids=$($PSQL -tAc "SET enable_bitmapscan = off;
	                     SELECT string_agg(id::text, ',' ORDER BY id) FROM (
	                       SELECT id FROM vm ORDER BY v <#> '$lit'::wvec LIMIT 10) t")
	exids=$($PSQL -tAc "SET enable_indexscan = off; SET enable_bitmapscan = off;
	                    SELECT string_agg(id::text, ',' ORDER BY id) FROM (
	                      SELECT id FROM vm WHERE v IS NOT NULL
	                        ORDER BY v <#> '$lit'::wvec LIMIT 10) t")
	hit=$($PSQL -tAc "SELECT count(*) FROM
	                    unnest(string_to_array('$idxids', ',')) a
	                    JOIN unnest(string_to_array('$exids', ',')) b ON a = b")
	printf '%s\n' "$hit" >>"$OUT/answers.tsv"
done <"$WORK/queries.txt"

read -r nq recall < <(awk '{ n++; s += $1 } END { printf "%d %.4f\n", n, s / (n * 10) }' \
	"$OUT/answers.tsv")
emit answer queries "$nq"
emit answer recall_at_10 "$recall"
say "answer check: recall@10 = $recall over $nq queries"
awk -v r="$recall" 'BEGIN { exit (r < 0.90) }' \
	|| defer "recall@10 $recall is below the floor a working scan cannot miss -- the scan is reading the wrong codes"

if [ "$DEFERRED" != 0 ]; then
	say "vecmerge: $DEFERRED DEFERRED FAILURE(S) -- report at $REPORT"
	column -t -s$'\t' "$REPORT" || cat "$REPORT"
	exit 1
fi
say "vecmerge: all stages clean; report at $REPORT"
column -t -s$'\t' "$REPORT" || cat "$REPORT"
