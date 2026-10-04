# 030_bulkdelete_atomic.pl -- every WAL record VACUUM's bulkdelete writes to the
# metapage leaves corpus ndocs consistent with the segment directory.
#
# doc/GAPS.md G72.  weave_bulkdelete() swapped each segment's tombstone set
# (segs[s].ndeleted) in one GenericXLog record per segment and refreshed the
# corpus N (m->ndocs) in a SEPARATE record after the last one.  A crash or ERROR
# between them left ndeleted committed and ndocs still counting the deleted
# rows; once every dead row was carried in a tombstone set, no later VACUUM had
# tuples_removed > 0 to trigger the refresh, so BM25's N stayed wrong.
#
# t/029 could not aim a crash between two records.  This test can: it archives
# the WAL, takes a base backup before the VACUUM, and for EACH record in the
# VACUUM that touches the index's metapage it restores the backup with
# recovery_target_lsn = that record (inclusive) -- the state a crash immediately
# after that record would recover to.  At every such point it asserts
#
#     ndocs + ndeleted == C      (C = documents in the directory, constant here)
#
# which is ndocs == sum(segs.ndocs - segs.ndeleted) + npending with npending = 0
# and no merge in the window (both asserted as preconditions).
#
# POSITIVE CONTROL, RUN 2026-10-04 on the pre-fix amvacuum.c: 5 of 16 failed --
# 4 metapage records, ndocs + ndeleted = 2800/2830/2860 at the swap points, and
# ndocs stuck at 2600 against a heap of 2340 after the re-VACUUM.  doc/GAPS.md G72
# has the run ids.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\n");
$node->start;

# Three segments: the build's collapsed one, then two flushed ones.  Levels 3,
# 2, 2 and nsegments 3 are inside every merge threshold, so the cleanup does not
# merge and the only metapage records in the window are the swaps.
$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE ba (id int, d wdoc);
	INSERT INTO ba SELECT g, to_wdoc('seed w' || g) FROM generate_series(1, 2000) g;
	CREATE INDEX ba_w ON ba USING weave (d);
	INSERT INTO ba SELECT g, to_wdoc('second w' || g) FROM generate_series(2001, 2300) g;
	VACUUM ba;
	INSERT INTO ba SELECT g, to_wdoc('third w' || g) FROM generate_series(2301, 2600) g;
	VACUUM ba;
});
my $nseg0 = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('ba_w')});
is($nseg0, '3', 'precondition: three segments');
my ($c, $del0) = split /\|/, $node->safe_psql('postgres',
	q{SELECT ndocs::bigint, ndeleted::bigint FROM weave_index_stats('ba_w')});
is("$c|$del0", '2600|0', 'precondition: 2600 documents, no tombstones');

# Rows in every segment.
$node->safe_psql('postgres', 'DELETE FROM ba WHERE id % 10 = 0');
$node->backup('b');

my $lsn0 = $node->safe_psql('postgres', q{SELECT pg_current_wal_insert_lsn()});
$node->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON) ba');
$node->safe_psql('postgres', 'CREATE TABLE ba_anchor AS SELECT 1');
my $lsn1 = $node->safe_psql('postgres', q{SELECT pg_current_wal_lsn()});
my $walfile = $node->safe_psql('postgres', q{SELECT pg_walfile_name(pg_current_wal_lsn())});
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	qq{SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver})
  or die 'timed out waiting for the VACUUM\'s WAL to be archived';

my $heap = $node->safe_psql('postgres', q{SELECT count(*) FROM ba});
is($heap, '2340', 'precondition: 260 rows deleted');
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('ba_w')}), $nseg0,
	'precondition: the VACUUM did not merge');
my ($ndocs_end, $del_end) = split /\|/, $node->safe_psql('postgres',
	q{SELECT ndocs::bigint, ndeleted::bigint FROM weave_index_stats('ba_w')});
is("$ndocs_end|$del_end", "$heap|260", 'after the VACUUM: ndocs = heap, 260 tombstones');

# Every record in the window that touches the index's metapage (main fork, block 0).
my @lsns = split /\n/, $node->safe_psql('postgres', qq{
	SELECT DISTINCT start_lsn
	  FROM pg_get_wal_block_info('$lsn0', '$lsn1')
	 WHERE relfilenode = pg_relation_filenode('ba_w')
	   AND relforknumber = 0 AND relblocknumber = 0
	 ORDER BY start_lsn});
is(scalar(@lsns), $nseg0,
	'ONE METAPAGE RECORD PER SEGMENT (got ' . scalar(@lsns) . '); one more is the '
	  . 'separate G72 refresh');

# Recover to just after each record, as a crash there would.
my $i = 0;
my $sticky_done = 0;
foreach my $lsn (@lsns)
{
	$i++;
	my $pitr = PostgreSQL::Test::Cluster->new("pitr$i");
	$pitr->init_from_backup($node, 'b', has_restoring => 1, standby => 0);
	$pitr->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_action = 'promote'
archive_mode = off
});
	$pitr->start;
	$pitr->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die "pitr$i did not promote";
	my ($nd, $dl) = split /\|/, $pitr->safe_psql('postgres',
		q{SELECT ndocs::bigint, ndeleted::bigint FROM weave_index_stats('ba_w')});
	note("G72 point $i at $lsn: ndocs=$nd ndeleted=$dl");
	cmp_ok($dl, '>', 0, "point $i: recovery stopped inside the VACUUM (ndeleted=$dl)");
	is($nd + $dl, $c,
		"G72 POINT $i: ndocs + ndeleted = $c after a crash here (ndocs=$nd ndeleted=$dl)");

	# The first point with every tombstone committed is the one the old code
	# could not recover from: the next VACUUM finds every dead row carried.
	if (!$sticky_done && $dl == $del_end)
	{
		$sticky_done = 1;
		$pitr->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON) ba');
		my $h = $pitr->safe_psql('postgres', q{SELECT count(*) FROM ba});
		my $n = $pitr->safe_psql('postgres',
			q{SELECT ndocs::bigint FROM weave_index_stats('ba_w')});
		is($n, $h, "G72 STICKY: after the next VACUUM ndocs equals the heap ($n vs $h)");
	}
	$pitr->teardown_node;
}
ok($sticky_done, 'a recovery point with every tombstone committed was examined');

$node->stop;
done_testing();
