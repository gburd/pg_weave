# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Crash recovery of a NULL-BEARING docvals weft (docvals-nulls slice, Task 6,
# the crash-recovery gate; PRODUCTION_READINESS gate 7 specialised to the v2
# null bitmap).
#
# WHY A SEPARATE FILE FROM t/016_vector_durability.pl.  That test asserts the
# VECTOR writer is 100% GenericXLog.  This one asserts the same of the docvals
# writer AND, specifically, of the NEW v2 region it grew: the null bitmap.  A
# NULL-bearing store is a v2 store whose header carries a nonzero null_off and
# whose tail is a `ceil(ndocs/8)`-byte bitmap (include/weave/docvals.h); the gate
# excludes a docid whose null bit is set BEFORE the comparison.  If the writer put
# that bitmap on disk outside GenericXLog -- or wrote it in a different XLog cycle
# from the value/docid arrays it sits behind -- an immediate crash leaves a store
# that is perfect until the first crash and then has a null bitmap that does not
# match its values, i.e. a docid that IS null answering as if it held some int64.
#
# WHAT WOULD BE MISSED WITHOUT IT.  The lexical half of the index still answers
# over every row with a torn docvals store, so a row count cannot see it.  The
# check that can is: (1) weave_check(deep), whose reachability walk follows the
# WEAVE_PK_DOCVALS chain (src/am/amcheck.c) and would report a truncated chain,
# and (2) the gate answer itself -- index == heap for every btree strategy AND
# the NULLs still excluded -- recomputed after recovery and compared to the
# pre-crash answer.  A store that recovered to an earlier version of itself, or
# lost its bitmap, changes one of those numbers and nothing else.
#
# Deliberately NO CHECKPOINT before the crash, for the same reason as t/016 and
# t/014: a checkpoint would put the pages on disk and leave the WAL path
# untested.  No page-byte rewriting here, so no need for no_data_checksums (that
# is the torn-write test t/021's concern; recovery does not rewrite pages).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# weave_check(deep) must report no failed invariant.  Copied in spirit from
# t/016's check_clean: the deep pass is the only thing that walks the
# WEAVE_PK_DOCVALS chain and would notice a store the crash truncated.
sub check_clean
{
	my ($node, $label) = @_;
	my ($ok, $detail) = split /\|/,
		$node->safe_psql('postgres',
			q{SELECT ok, coalesce(detail, '') FROM weave_check('dvr_w', true)
			   WHERE NOT ok LIMIT 1}), 2;
	ok(!defined $ok || $ok eq '',
		"weave_check(deep) reports no failed invariant $label"
		. (defined $detail ? " ($detail)" : ''));
	return;
}

# The docvals gate, forced through the index (enable_seqscan/bitmapscan off, as
# sql/docvals.sql section 9 does): this reads the docvalues store and excludes
# any NULL docid before the comparison.
sub idx_count
{
	my ($node, $pred) = @_;
	return $node->safe_psql('postgres',
		"SET enable_seqscan=off; SET enable_bitmapscan=off; "
		. "SELECT count(*) FROM dvr WHERE $pred");
}

# The same predicate over a natural sequential scan of the heap: the oracle the
# gate must equal exactly (the docvals gate is EXACT).  enable_indexscan off so
# the qual cannot be pushed to the index.
sub heap_count
{
	my ($node, $pred) = @_;
	return $node->safe_psql('postgres',
		"SET enable_indexscan=off; SET enable_bitmapscan=off; SET enable_seqscan=on; "
		. "SELECT count(*) FROM dvr WHERE $pred");
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# fsync off for the same reason t/016 does it: an immediate stop kills the
# postmaster, so anything already written(2) survives in the OS.  What is under
# test is whether the records were written at all (100% GenericXLog).
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# A wdoc body column and a NULLABLE bigint facet, NULL every 7th row: ~100 NULLs
# of 700, so the null bitmap is nonempty (null_off != 0) and 50 still occurs at
# g in {50,150,250,350,450,550,650} with g%7<>0, so `= 50` is non-empty.  Built
# by CREATE INDEX over pre-inserted rows, so the store is a FLUSHED segment store
# (not a pending item), which is the thing the crash must recover intact.  Column
# shapes copied from sql/docvals.sql section 9 (dvn).
$node->safe_psql('postgres', q{
	CREATE TABLE dvr (id int, body wdoc, price bigint);
	INSERT INTO dvr
	SELECT g, to_wdoc('common doc ' || (g % 5)),
	       CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 100)::bigint END
	  FROM generate_series(1, 700) g;
	CREATE INDEX dvr_w ON dvr USING weave (body, price int8_docval_ops);
	ANALYZE dvr;
});

# The store exists AND is used before the crash: the index-forced gate agrees
# with the heap and is non-empty.  Asserted rather than assumed -- a test that
# crashes an index whose gate silently fell back to a seq scan would pass
# vacuously (the failure mode t/014 and t/016 both had early on).
my @preds = ('price < 50', 'price <= 50', 'price = 50', 'price >= 50', 'price > 50');

# Positive control: the comparison is answered by the weave index, not a silent
# seqscan fallback -- enable_seqscan=off is a cost penalty, not a prohibition, so
# count==heap alone could pass without ever loading the store (AGENTS.md: reaching
# a C function is not the same as returning the right answer).  Assert the plan.
my $plan_bf = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  EXPLAIN (COSTS OFF) SELECT id FROM dvr WHERE price < 50});
like($plan_bf, qr/Index Scan using dvr_w/,
	'the gate is an Index Scan on the weave index, not a seqscan fallback');

my %before;
for my $p (@preds)
{
	my ($i, $h) = (idx_count($node, $p), heap_count($node, $p));
	is($i, $h, "before the crash: gate '$p' == heap ($i)");
	$before{$p} = $i;
}
cmp_ok($before{'price < 50'}, '>', 0,
	'the gate is actually driven by the store (non-empty answer)');

# The null-exclusion witnesses, captured before the crash:
#   - IS NULL over the heap: how many NULLs there are (~100).
#   - `price < 100` through the gate: every NON-null row (values are 0..99), so
#     it must equal IS NOT NULL over the heap.  A NULL is neither < 100 nor
#     anything else, so if the bitmap were lost these two would diverge.
my $isnull_before   = $node->safe_psql('postgres',
	'SELECT count(*) FROM dvr WHERE price IS NULL');
my $isnotnull_heap  = $node->safe_psql('postgres',
	'SELECT count(*) FROM dvr WHERE price IS NOT NULL');
my $nonnull_gate_bf = idx_count($node, 'price < 100');
cmp_ok($isnull_before, '>', 0, "there are NULLs to exclude ($isnull_before of 700)");
is($nonnull_gate_bf, $isnotnull_heap,
	'before the crash: NULLs are excluded from a comparison (< 100 gate == IS NOT NULL)');

check_clean($node, 'before the crash');

# NOTHING between the build and the crash.  No CHECKPOINT; CREATE INDEX acquires
# an XID so its WAL is flushed on commit, but the pages themselves are only in
# the WAL if every one of them -- values, docids AND the null bitmap -- went
# through GenericXLog.
$node->stop('immediate');
$node->start;

# --- the null-bearing store survived, in full -----------------------------
check_clean($node, 'after the crash');

for my $p (@preds)
{
	my ($i, $h) = (idx_count($node, $p), heap_count($node, $p));
	is($i, $before{$p}, "after the crash: gate '$p' == pre-crash answer ($i)");
	is($i, $h,          "after the crash: gate '$p' == heap ($h)");
}

is($node->safe_psql('postgres', 'SELECT count(*) FROM dvr WHERE price IS NULL'),
	$isnull_before,
	"the NULL count survived recovery ($isnull_before), so the bitmap is intact");
is(idx_count($node, 'price < 100'),
	$node->safe_psql('postgres', 'SELECT count(*) FROM dvr WHERE price IS NOT NULL'),
	'after the crash: NULLs are STILL excluded from comparisons (v2 bitmap recovered)');

done_testing();
