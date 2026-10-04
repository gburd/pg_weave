# 029_flush_atomic.pl -- a pending flush adds its segment and clears the folded
# prefix of the pending list in ONE WAL record.
#
# doc/GAPS.md G65.  weave_flush_pending() used to commit the new segment
# (weave_add_segment_with_room) and then clear the pending list in a SECOND
# GenericXLog record.  A crash, or an ERROR from the clear's ReadBuffer, between
# the two left the folded documents in the new segment AND on the list; the next
# flush folded them again, so each got two docids and the corpus statistics
# counted it twice.
#
# A crash cannot be aimed between two records from a TAP test without an
# injection point, and pg_weave's test hooks are not in any shipped build.  So
# the test asserts the STRUCTURAL property that makes the window impossible: the
# flush writes exactly one record that touches the metapage, and that record
# also carries the cut page.  Measured on the pre-fix tree with the same probe:
# 2 metapage records per flush.  That is the positive control -- the assertion
# below fails on the old code.
#
# The second half is the behavioural claim at the boundary that IS reachable:
# after a flush and an immediate stop with no XID-acquiring statement in
# between, the index agrees with the heap on every count, whichever side of the
# flush recovery lands on.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\n");
$node->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE fa (id serial, d wdoc);
	INSERT INTO fa(d) SELECT to_wdoc('seed w' || g) FROM generate_series(1, 200) g;
	CREATE INDEX fa_w ON fa USING weave (d);
});

# --- one record ---------------------------------------------------------------
# Two pending pages' worth of rows, so the cut is a real page chain and not only
# the head.  The flush runs inside VACUUM; a trailing XID-acquiring statement
# flushes the WAL so pg_get_wal_block_info can read up to lsn1.
$node->safe_psql('postgres', q{
	INSERT INTO fa(d) SELECT to_wdoc('pending w' || g || ' ' || repeat('x', 200))
	  FROM generate_series(1, 120) g;
});
my $npending = $node->safe_psql('postgres',
	q{SELECT count(*) FROM fa WHERE d @@@ 'pending'});
is($npending, '120', 'precondition: the 120 pending rows are answerable');

my $nseg0 = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('fa_w')});
my $lsn0 = $node->safe_psql('postgres', q{SELECT pg_current_wal_insert_lsn()});
$node->safe_psql('postgres', 'VACUUM fa');
$node->safe_psql('postgres', 'CREATE TABLE fa_anchor AS SELECT 1');
my $lsn1 = $node->safe_psql('postgres', q{SELECT pg_current_wal_lsn()});
my $nseg1 = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('fa_w')});
cmp_ok($nseg1, '>', $nseg0,
	"precondition: the VACUUM flushed ($nseg0 -> $nseg1 segments)");

# every record in the window that touches the index's metapage (block 0), with
# the other main-fork index blocks it carries
my $recs = $node->safe_psql('postgres', qq{
	SELECT string_agg(blks::text, ' ' ORDER BY start_lsn)
	  FROM (SELECT start_lsn,
	               array_agg(DISTINCT relblocknumber ORDER BY relblocknumber) AS blks
	          FROM pg_get_wal_block_info('$lsn0', '$lsn1')
	         WHERE relfilenode = pg_relation_filenode('fa_w')
	           AND relforknumber = 0
	         GROUP BY start_lsn) r
	 WHERE 0 = ANY (blks)});
my @recs = split / /, $recs;
is(scalar(@recs), 1,
	"THE FLUSH TOUCHES THE METAPAGE IN ONE RECORD (got: $recs); two records is "
	  . 'the G65 window');

# --- crash right after a flush ------------------------------------------------
$node->safe_psql('postgres', q{
	INSERT INTO fa(d) SELECT to_wdoc('second w' || g) FROM generate_series(1, 300) g;
});
$node->safe_psql('postgres', 'VACUUM fa');
# NOTHING between the flush and the crash.
$node->stop('immediate');
$node->start;

my $heap = $node->safe_psql('postgres', q{SELECT count(*) FROM fa});
my $ndocs = $node->safe_psql('postgres',
	q{SELECT ndocs::bigint FROM weave_index_stats('fa_w')});
my $second = $node->safe_psql('postgres', q{
	SET enable_seqscan = off;
	SELECT count(*) FROM fa WHERE d @@@ 'second'});
is($second, '300', 'every row of the crashed flush is answerable exactly once');
is($ndocs, $heap, "ndocs equals the heap after recovery ($ndocs vs $heap): no double count");
my $bad = $node->safe_psql('postgres',
	q{SELECT count(*) FROM weave_check('fa_w', true) WHERE NOT ok});
# A count alone is undiagnosable after the fact (doc/GAPS.md G21's lesson): when
# this fails, print WHICH invariant and, for a leak, what the orphaned pages are --
# a leaked page's kind names the write path that left it.
if ($bad ne '0')
{
	diag("G75 weave_check violations:\n" . $node->safe_psql('postgres',
		q{SELECT string_agg(invariant || ': ' || coalesce(detail, ''), E'\n')
		    FROM weave_check('fa_w', true) WHERE NOT ok}));
	diag("G75 unreachable unflagged pages:\n" . $node->safe_psql('postgres',
		q{SELECT string_agg(blkno || ' kind=' || coalesce(kind, '(none)')
		                    || ' flags=0x' || coalesce(to_hex(flags), '?')
		                    || ' nextblk=' || coalesce(nextblk::text, 'none')
		                    || ' lsn=' || lsn, E'\n' ORDER BY blkno)
		    FROM weave_page_info('fa_w')
		   WHERE NOT reachable AND coalesce(freed, false) = false
		     AND NOT uninitialized}));
	diag("G75 relation pages: " . $node->safe_psql('postgres',
		q{SELECT pg_relation_size('fa_w') / current_setting('block_size')::int}));
}
is($bad, '0', 'weave_check(deep) is clean after recovery');

# and the NEXT flush, which is where a doubly-held document used to become two
$node->safe_psql('postgres', q{
	INSERT INTO fa(d) SELECT to_wdoc('third w' || g) FROM generate_series(1, 50) g;
});
$node->safe_psql('postgres', 'VACUUM fa');
$ndocs = $node->safe_psql('postgres',
	q{SELECT ndocs::bigint FROM weave_index_stats('fa_w')});
$heap = $node->safe_psql('postgres', q{SELECT count(*) FROM fa});
is($ndocs, $heap, "ndocs still equals the heap after the next flush ($ndocs vs $heap)");

$node->stop;
done_testing();
