# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Crash recovery of a TEXT docvals weft (store v3: a per-segment DICTIONARY plus
# one ordinal per docid), across all three places a text value can live when the
# server dies: the build bolt, a bolt a VACUUM flushed from the pending buffer,
# and the pending buffer itself.  PRODUCTION_READINESS gate 7 specialised to the
# text slice (doc/plans/2026-09-28-docvals-text-slice.md).
#
# WHY A SEPARATE FILE FROM t/020_docvals_null_recovery.pl.  t/020 crashes an
# int8 store built by CREATE INDEX and nothing else.  A text store differs in
# three ways that each have their own way of coming back wrong:
#
#   1. The dictionary is a variable-length region in front of the ordinals.  A
#      writer that put the dictionary pages on disk outside GenericXLog, or in a
#      different XLog cycle from the ordinals that index into it, recovers to a
#      store whose ordinals point at the wrong strings -- every row still has
#      "a value", so a row count cannot see it; only a comparison against the
#      heap can.
#   2. A pending item carries its value inline as `0x01 || bytes` (so '' is a
#      real one-byte value and NULL is dvlen 0).  A crash leaves those items to
#      be replayed from WAL and then compared under the column collation, with
#      no dictionary at all.  A replayed item that lost its trailer, or confused
#      '' with NULL, changes exactly one boundary case.
#   3. The FLUSHED bolt has a dictionary built from the pending items' bytes, not
#      from the heap, so it is a separate writer path from the build's.
#
# WHAT WOULD BE MISSED WITHOUT IT.  The lexical half of the index answers over
# every row whatever the docvals store holds.  The checks that can see a torn
# text store are (a) weave_check(deep), which walks the docvalues chain and
# checks dictionary order under the column collation, and (b) the gate answer
# as a SET of ids -- index == heap for every btree strategy and for constants
# below, between, equal to a build value, equal to a pending value and above
# everything, on BOTH the plain Index Scan and the bitmap route.  sql/docvals.sql
# sections 10c and 15 prove that agreement without a crash; this file proves it
# survives one.
#
# Deliberately NO CHECKPOINT after the post-build writes, for the reason t/016
# and t/020 give: a checkpoint would put the pages on disk and leave the WAL path
# untested.  (There is one CHECKPOINT right after the build, so the build bolt is
# on disk and the replay under test is of the post-build pending writes and the
# flush, which is where the text-specific writers are.)
#
# THE WAL-FLUSH GOTCHA OF t/012 DOES NOT BITE HERE, but only because of the
# order of operations: a VACUUM that flushes the pending buffer need not acquire
# an XID, so its GenericXLog records might sit in the WAL buffers at commit.  The
# batch INSERTed after it DOES acquire an XID, and its commit XLogFlush()es the
# WAL stream up to its own LSN, which covers the flush's records.  The segment
# count is compared before and after the crash so that, if that anchoring ever
# stopped holding, the test would say "the flushed bolt was lost" rather than
# something vaguer.
#
# Also here: the non-deterministic collation refusal (ambuild.c
# weave_build_dvcollation).  A collation that equates byte-distinct strings makes
# "the distinct values of a segment" ill-defined and `=` on ordinals silently
# drops rows, so CREATE INDEX must refuse it.  That needs ICU; when the build has
# none, the skip is asserted to be BECAUSE of ICU, so an unrelated failure of
# CREATE COLLATION is not quietly counted as a skip.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

note('t/023_docvals_text_recovery: START');

# weave_check(deep) must report no failed invariant.  Same shape as t/020's
# check_clean, plus a count so a failure says how many invariants broke.
sub check_clean
{
	my ($node, $label) = @_;
	my $nbad = $node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('dvtr_w', true) WHERE NOT ok});
	my $detail = $node->safe_psql('postgres',
		q{SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail, ''),
		                             '; ' ORDER BY invariant), '')
		    FROM weave_check('dvtr_w', true) WHERE NOT ok});
	is($nbad, '0',
		"weave_check(deep) reports no failed invariant $label"
		. ($detail ne '' ? " ($detail)" : ''));
	return;
}

# The id SET a predicate selects, as one comparable string, under one of three
# planner regimes.  Each safe_psql is its own session, so the SETs cannot leak
# between arms.  The ORDER BY inside string_agg makes the string a canonical
# form of the set, so plan-dependent output order cannot cause a false mismatch.
#
#   index  -- plain Index Scan: enable_seqscan/bitmapscan off (sql/docvals.sql 10c)
#   bitmap -- Bitmap Index Scan: enable_indexscan/seqscan off
#   heap   -- the oracle: a natural seqscan, the qual cannot reach the index
#
# Every call carries a WHERE clause, so none of them plans the keyless weave
# scan that ERRORs with "a weave index scan requires a query".
my %regime = (
	index  => 'SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_indexscan=on;',
	bitmap => 'SET enable_seqscan=off; SET enable_indexscan=off; SET enable_bitmapscan=on;',
	heap   => 'SET enable_indexscan=off; SET enable_bitmapscan=off; SET enable_seqscan=on;',
);

sub id_set
{
	my ($node, $mode, $pred) = @_;
	return $node->safe_psql('postgres',
		"$regime{$mode} "
		. "SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '<empty>') "
		. "FROM dvtr WHERE $pred");
}

sub id_count
{
	my ($node, $mode, $pred) = @_;
	return $node->safe_psql('postgres',
		"$regime{$mode} SELECT count(*) FROM dvtr WHERE $pred");
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# fsync off for the same reason t/016 and t/020 do it: an immediate stop kills
# the postmaster, so anything already written(2) survives in the OS.  What is
# under test is whether the records were written at all (100% GenericXLog).
# autovacuum off so no background VACUUM flushes the pending buffer behind the
# test's back: the pending-at-crash batch must really be pending at the crash.
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# --- 1. the build bolt ------------------------------------------------------
#
# 30 distinct 'mNN' values, NULL every 17th row, and exactly one '' (a real
# dictionary entry, the minimum of text -- g = 137 is not a multiple of 17).
# Shapes copied from sql/docvals.sql section 15 (dvx).
$node->safe_psql('postgres', q{
	CREATE TABLE dvtr (id int, body wdoc, cat text COLLATE "C")
	    WITH (autovacuum_enabled = off);
	INSERT INTO dvtr
	SELECT g, to_wdoc('common doc ' || (g % 5)),
	       CASE WHEN g % 17 = 0 THEN NULL
	            WHEN g = 137 THEN ''
	            ELSE 'm' || lpad((g % 30)::text, 2, '0') END
	  FROM generate_series(1, 400) g;
	CREATE INDEX dvtr_w ON dvtr USING weave (body wdoc_lex_ops, cat text_docval_ops);
	ANALYZE dvtr;
});
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('dvtr_w')}),
	'1', 'the build wrote one bolt');
$node->safe_psql('postgres', 'CHECKPOINT');

# --- 2. post-build writes: a flushed bolt, then a pending batch -------------
#
# Batch 1 + batch 2 go to the pending buffer and a VACUUM flushes them into a
# second bolt with its own dictionary.  Values are new to the build dictionary
# on purpose: below every build value ('a'), between two ('m05x'), equal to one
# ('m10'), '' and NULL, a 3000-byte value (still fits a pending page; the
# 20000-byte oversized path is sql/docvals.sql 15's concern, not this file's).
my $seg0 = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('dvtr_w')});
$node->safe_psql('postgres', q{
	INSERT INTO dvtr VALUES
	  (10001, to_wdoc('zebra special report'), 'a'),
	  (10002, to_wdoc('zebra special report'), 'm05x'),
	  (10003, to_wdoc('zebra special report'), ''),
	  (10004, to_wdoc('zebra special report'), NULL),
	  (10005, to_wdoc('zebra special report'), 'm10'),
	  (10006, to_wdoc('zebra special report'), repeat('z', 3000));
});
$node->safe_psql('postgres', q{
	INSERT INTO dvtr
	SELECT 20000 + g, to_wdoc('zebra second batch'),
	       CASE WHEN g % 10 = 0 THEN NULL
	            WHEN g % 10 = 1 THEN ''
	            WHEN g % 10 = 2 THEN 'm10'
	            ELSE 'b' || lpad(g::text, 2, '0') END
	  FROM generate_series(1, 40) g;
});
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('dvtr_w')}),
	$seg0, 'batches 1-2 are pending (no new bolt before the VACUUM)');
$node->safe_psql('postgres', 'VACUUM dvtr');
my $seg_flushed = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('dvtr_w')});
cmp_ok($seg_flushed, '>', $seg0,
	"VACUUM flushed the pending text rows into a new bolt ($seg0 -> $seg_flushed)");

# Batch 3: left PENDING at the crash.  'q77' occurs ONLY here, so an answer for
# `cat = 'q77'` can only come from replayed pending items -- the positive
# control that the pending buffer survived rather than being silently empty.
# Also '', NULL, a build value, a between value and a second 3000-byte value
# ('y'-filled, so it sorts below the 'z' one and both are distinct).  This
# INSERT's commit is also the XLogFlush anchor for the VACUUM above.
$node->safe_psql('postgres', q{
	INSERT INTO dvtr VALUES
	  (30001, to_wdoc('zebra pending report'), 'q77'),
	  (30002, to_wdoc('zebra pending report'), 'q77'),
	  (30003, to_wdoc('zebra pending report'), ''),
	  (30004, to_wdoc('zebra pending report'), NULL),
	  (30005, to_wdoc('zebra pending report'), 'm10'),
	  (30006, to_wdoc('zebra pending report'), 'm05x'),
	  (30007, to_wdoc('zebra pending report'), repeat('y', 3000)),
	  (30008, to_wdoc('zebra pending report'), 'A');
});
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('dvtr_w')}),
	$seg_flushed, 'batch 3 is pending at the crash (no new bolt)');

# Pre-crash witnesses, to compare the recovered answers against as well as the
# heap.  Whole-heap count with default planner settings (no SET) so it cannot
# plan a keyless weave scan.
my $nrows_before = $node->safe_psql('postgres', 'SELECT count(*) FROM dvtr');
my $q77_before = id_set($node, 'heap', q{cat = 'q77'});
is($q77_before, '30001,30002', 'the pending-only value q77 is in the heap');

check_clean($node, 'before the crash');

# --- 3. crash ---------------------------------------------------------------
$node->stop('immediate');
$node->start;
note('t/023_docvals_text_recovery: recovered from immediate stop');

is($node->safe_psql('postgres', 'SELECT count(*) FROM dvtr'),
	$nrows_before, "the heap survived the crash ($nrows_before rows)");
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('dvtr_w')}),
	$seg_flushed, 'the flushed bolt survived the crash (segment count unchanged)');

# --- 5 (first, so a vacuous agreement cannot pass). POSITIVE CONTROL --------
#
# The index arm must really run the text gate.  enable_seqscan=off is a cost
# penalty, not a prohibition: an unindexable clause would be a (disabled) Seq
# Scan in the index arm too and agree with the heap vacuously.
my $plan_idx = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  EXPLAIN (COSTS OFF) SELECT id FROM dvtr WHERE cat < 'm10'});
like($plan_idx, qr/Index Scan using dvtr_w/,
	'after the crash: the text gate is an Index Scan on the weave index');
like($plan_idx, qr/Index Cond:.*\bcat\b/,
	'after the crash: the text comparison is an Index Cond, not a Filter');
my $plan_bm = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_indexscan=off; SET enable_bitmapscan=on;
	  EXPLAIN (COSTS OFF) SELECT id FROM dvtr WHERE cat < 'm10'});
like($plan_bm, qr/Bitmap Index Scan on dvtr_w/,
	'after the crash: the bitmap route runs a Bitmap Index Scan on the weave index');

# The recovered PENDING rows are in the index answer, by value.
my $q77_idx = id_set($node, 'index', q{cat = 'q77'});
is($q77_idx, $q77_before,
	'after the crash: the replayed pending rows are found by value (q77)');
cmp_ok(id_count($node, 'index', q{cat = 'q77'}), '>', 0,
	'after the crash: the pending-only answer is non-empty');
is(id_set($node, 'index', q{cat = '' AND id > 30000}), '30003',
	"after the crash: a pending '' is a real value, not NULL");
is(id_set($node, 'index', q{cat = repeat('y', 3000)}), '30007',
	'after the crash: the 3000-byte pending value is found by value');

# --- 4. index == heap for every strategy and boundary constant --------------
#
# '' is the minimum of text (so `< ''` is empty, `>= ''` is every non-NULL),
# 'A' is below every non-empty value and itself pending, 'm055' is absent and
# between build values, 'm05x' is a flushed and pending value, 'm10' is in all
# three places, 'q77' is pending only, the 'y'/'z' 3000-byte values straddle
# 'yz', and '~' is above everything (0x7e > 'z' under "C").
my @ops = ('<', '<=', '=', '>=', '>');
my @consts = (q{''}, q{'A'}, q{'m055'}, q{'m05x'}, q{'m10'}, q{'q77'},
	q{'yz'}, q{repeat('z', 3000)}, q{'~'});
my @conj = (
	q{body @@@ 'zebra'::wquery AND cat < 'm10'},
	q{body @@@ 'zebra'::wquery AND cat >= ''},
	q{body @@@ 'zebra'::wquery AND cat = 'q77'},
);

sub agree_all
{
	my ($node, $phase) = @_;
	my @preds;
	for my $op (@ops)
	{
		push @preds, "cat $op $_" for @consts;
	}
	push @preds, @conj;
	for my $p (@preds)
	{
		my $h = id_set($node, 'heap', $p);
		is(id_set($node, 'index', $p), $h, "$phase: index scan == heap for '$p'");
		is(id_set($node, 'bitmap', $p), $h, "$phase: bitmap scan == heap for '$p'");
	}
	# '' as the minimum, stated as counts too so a failure reads directly
	is(id_count($node, 'index', q{cat < ''}), '0', "$phase: nothing is < ''");
	is(id_count($node, 'index', q{cat >= ''}),
		$node->safe_psql('postgres', 'SELECT count(*) FROM dvtr WHERE cat IS NOT NULL'),
		"$phase: >= '' is every non-NULL row (NULLs excluded, '' included)");
	return;
}

agree_all($node, 'after the crash');

# --- 6. deep check, then flush the recovered pending rows and re-check -------
check_clean($node, 'after the crash');

$node->safe_psql('postgres', 'VACUUM dvtr');
my $seg_final = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('dvtr_w')});
cmp_ok($seg_final, '>', $seg_flushed,
	"VACUUM after recovery flushed the replayed pending rows ($seg_flushed -> $seg_final)");

agree_all($node, 'after the post-recovery flush');
is(id_set($node, 'index', q{cat = 'q77'}), $q77_before,
	'after the post-recovery flush: q77 is found in its new bolt');
check_clean($node, 'after the post-recovery flush');

# --- 7. a non-deterministic collation is refused ----------------------------
#
# In a UTF8 database of its own: ICU refuses a SQL_ASCII database, and the
# cluster's default encoding follows the test environment's locale, so without
# this the "ICU unavailable" branch could be taken for an encoding reason.
$node->safe_psql('postgres',
	q{CREATE DATABASE ndcoll TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C'});
$node->safe_psql('ndcoll', 'CREATE EXTENSION pg_weave');
my ($crc, $cout, $cerr) = $node->psql('ndcoll',
	q{CREATE COLLATION nd (provider = icu, locale = 'und-u-ks-level2',
	                       deterministic = false)});
if ($crc == 0)
{
	note('t/023_docvals_text_recovery: ICU available, testing refusal');
	$node->safe_psql('ndcoll', q{
		CREATE TABLE ndt (id int, body wdoc, c text COLLATE nd)
		    WITH (autovacuum_enabled = off);
		INSERT INTO ndt VALUES (1, to_wdoc('alpha'), 'abc'),
		                       (2, to_wdoc('beta'), 'ABC');
	});
	my ($irc, $iout, $ierr) = $node->psql('ndcoll',
		q{CREATE INDEX ndt_w ON ndt USING weave (body wdoc_lex_ops, c text_docval_ops)});
	isnt($irc, 0, 'CREATE INDEX on a non-deterministic text docvalues column fails');
	like($ierr, qr/text docvalues require a deterministic collation/,
		'... with the deterministic-collation error');
}
else
{
	note('t/023_docvals_text_recovery: SKIP non-deterministic collation check, '
		. "CREATE COLLATION failed: $cerr");
	like($cerr, qr/ICU|icu/,
		'CREATE COLLATION failed because ICU is unavailable, not for another reason');
}

note('t/023_docvals_text_recovery: END');
done_testing();
