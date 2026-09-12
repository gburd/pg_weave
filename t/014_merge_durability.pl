# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Durability of the user-callable maintenance functions: weave_merge() and
# weave_vacuum() must survive `pg_ctl stop -m immediate` ON THEIR OWN.
#
# WHY THIS TEST EXISTS.  Both functions write every page through GenericXLog, so
# the WAL records exist -- but neither touches a heap tuple or a catalog row, so
# their transaction never acquires a TransactionId.  PostgreSQL's
# RecordTransactionCommit() only calls XLogFlush() when the transaction committed
# an XID, or dropped relations, or forceSyncCommit was set.  A pure
# index-maintenance call satisfies none of those, so the records sat in the WAL
# buffers, written to no file, and an immediate shutdown lost them.  Recovery then
# rolled the whole merge back: the input segments returned, the merged segment
# vanished, and weave_check() reported CLEAN -- because the pre-merge state is a
# perfectly consistent state.  Silent loss of the work the user explicitly asked
# for.
#
# Fixed by calling ForceSyncCommit() in both functions (src/am/amvacuum.c).
#
# WHAT WOULD BE MISSED WITHOUT THIS TEST.  Nothing else covers it, and the reason
# is instructive: t/012_surf_crash_recovery.pl hit this gap while testing
# something else, and worked around it by following each merge with a trivial
# INSERT into an unrelated table -- a transaction that DOES acquire an XID, whose
# commit flushes the WAL stream up to its LSN, including every earlier record.
# That workaround was correct for t/012's purpose and it also meant the durability
# gap was never itself under test. A regression would have been invisible.
#
# So the shape of this test is: run the maintenance function, crash IMMEDIATELY,
# with NO XID-acquiring statement in between, and assert the work survived.
#
# Deliberately NOT a CHECKPOINT before the crash: that would put the pages on disk
# and leave the WAL path untested, which is the whole point.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# fsync off because durability against a MACHINE crash is not what is under test:
# an immediate stop kills the postmaster, so WAL already written(2) survives in
# the OS. What is under test is whether the records were written at all.
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# Several segments is a precondition, not a detail: if the index holds one segment
# then weave_merge() has nothing to merge and the durability assertion below passes
# vacuously.  Two earlier versions of this test did exactly that.
#
# The lever is OVERSIZED DOCUMENTS, borrowed from t/007_segment_cap.pl: a body of
# a few thousand distinct tokens makes each pending-list flush produce an oversized
# segment, and the size-tiered merge will not collapse those into one.  Neither a
# plain 20k-row corpus nor pg_weave.build_collapse_max_mb achieves it -- a serial
# build writes a single segment to begin with, so there is nothing for the collapse
# cap to decline to collapse.
$node->safe_psql('postgres', q{
	CREATE TABLE md (id serial, body text);
	CREATE INDEX md_weave ON md USING weave (to_wdoc('simple', body));
});

$node->safe_psql('postgres', q{
DO $$
DECLARE b int := 0;
BEGIN
  WHILE b < 24 LOOP
    INSERT INTO md(body)
      SELECT 'mdterm ' || string_agg('t' || s || 'r' || b, ' ')
        FROM generate_series(1, 4000) s;
    b := b + 1;
  END LOOP;
END $$;
});

my $nseg_before = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('md_weave')});
cmp_ok($nseg_before, '>', 1,
	"the index has $nseg_before segments, so weave_merge() has work to do");

# --- weave_merge(), then an immediate crash with no XID in between -----------
my $merged = $node->safe_psql('postgres', q{SELECT weave_merge('md_weave')});
is($merged, 't', 'weave_merge() reported that it did something');

my $nseg_merged = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('md_weave')});
is($nseg_merged, '1', 'weave_merge() compacted to a single segment');

my $rows_merged = $node->safe_psql('postgres', q{SELECT count(*) FROM md});

# NOTHING between the merge and the crash. No INSERT, no CHECKPOINT. If the
# maintenance function does not flush its own WAL, this is where the work is lost.
$node->stop('immediate');
$node->start;

my $nseg_after = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('md_weave')});
is($nseg_after, '1',
	'THE MERGE SURVIVED an immediate shutdown with no XID anchor '
	. "(segments before crash: $nseg_merged, after recovery: $nseg_after)");

my ($ok, $detail) = split /\|/,
	$node->safe_psql('postgres',
		q{SELECT ok, coalesce(detail, '') FROM weave_check('md_weave', true)
		   WHERE NOT ok LIMIT 1}), 2;
ok(!defined $ok || $ok eq '',
	'weave_check() finds no failed invariant after recovery'
	. (defined $detail ? " ($detail)" : ''));

is($node->safe_psql('postgres', q{SELECT count(*) FROM md}), $rows_merged,
	'the index still answers over every row after recovery');

# --- weave_vacuum(), same treatment -----------------------------------------
# weave_vacuum() additionally reclaims dead pages and truncates, so it has more
# to lose than weave_merge() does.
$node->safe_psql('postgres', q{
	DELETE FROM md WHERE id % 5 = 0;
	VACUUM md;
});
# The DELETE and VACUUM above acquire XIDs of their own, so the state entering
# weave_vacuum() is durable; what follows must be durable on its own account.
$node->safe_psql('postgres', q{SELECT weave_vacuum('md_weave')});

my $size_vacuumed = $node->safe_psql('postgres',
	q{SELECT pg_relation_size('md_weave')});
my $rows_vacuumed = $node->safe_psql('postgres', q{SELECT count(*) FROM md});

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT pg_relation_size('md_weave')}),
	$size_vacuumed,
	'weave_vacuum() SURVIVED an immediate shutdown, including its truncation');

is($node->safe_psql('postgres', q{SELECT count(*) FROM md}), $rows_vacuumed,
	'the index still answers over every live row after recovery');

my ($vok, $vdetail) = split /\|/,
	$node->safe_psql('postgres',
		q{SELECT ok, coalesce(detail, '') FROM weave_check('md_weave', true)
		   WHERE NOT ok LIMIT 1}), 2;
ok(!defined $vok || $vok eq '',
	'weave_check() finds no failed invariant after the second recovery'
	. (defined $vdetail ? " ($vdetail)" : ''));

done_testing();
