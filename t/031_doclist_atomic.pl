# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 031_doclist_atomic.pl -- a crash anywhere inside a flush cannot leave a bolt
# and its DOCUMENT LIST disagreeing (v12; doc/specs/SEGMENT_FORMAT.md sect. 6
# "The document list"; doc/GAPS.md G77/G78/G80/G81).
#
# The flush writes the list's pages in their own GenericXLog records, then the
# descriptor page that names the list's root, then ONE metapage record that adds
# the bolt and cuts the pending list (G65, t/029).  So the claim is structural:
# at every record boundary, either the bolt is not published (its pages are
# leaked, which the existing flush already accepts for a crash mid-write) or it
# is published WITH a list that validates and covers every posting, docvalues and
# warp docid.  A recovery that stops BETWEEN the list write and the publish must
# still answer every query exactly as the heap does -- from the pending list.
#
# HOW: the t/030 method.  Take a base backup with archiving on, run the flush,
# list every WAL record that touches the index during it (pg_walinspect), and
# for EACH record restore the backup and recover to just after it.  At every
# point assert (a) weave_check()'s doclist_valid and doclist_covers_postings,
# (b) a docvalues restriction, a NOT query and a vector ORDER BY equal the heap.
# The pending set mixes the three populations the list exists for: NULL
# documents (with and without a vector), zero-term documents, ordinary ones.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\n");
$node->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE da (id int, d wdoc, emb wvec(4), price int8);
	INSERT INTO da SELECT g, to_wdoc('common w' || g),
	       ('[' || g || ',' || g || ',' || g || ',' || g || ']')::wvec, 10 + g
	  FROM generate_series(1, 300) g;
	CREATE INDEX da_w ON da USING weave (d, emb, price int8_docval_ops);
	INSERT INTO da VALUES
	  (9001, NULL, '[0.5,0.5,0.5,0.5]', 1),
	  (9002, to_wdoc(''), '[0.6,0.6,0.6,0.6]', 2),
	  (9003, NULL, NULL, 3),
	  (9004, to_wdoc('rare'), '[0.7,0.7,0.7,0.7]', 4),
	  (9005, to_wdoc(''), NULL, 5);
});
is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('da_w')}), '1',
	'precondition: one bolt, five pending rows');

my %q = (
	price => q{SELECT string_agg(id::text, ',' ORDER BY id) FROM da WHERE price < 6},
	notq  => q{SELECT string_agg(id::text, ',' ORDER BY id) FROM da WHERE d @@@ '!common'},
	vec   => q{SELECT string_agg(id::text, ',') FROM
	           (SELECT id FROM da ORDER BY emb <-> '[0,0,0,0]'::wvec LIMIT 4) s},
);

sub answers
{
	my ($n, $path) = @_;
	my $set = $path eq 'heap'
	  ? 'SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off;'
	  : 'SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;';
	my %a;
	$a{$_} = $n->safe_psql('postgres', "$set $q{$_}") for sort keys %q;
	return \%a;
}

my $heap = answers($node, 'heap');
is($heap->{price}, '9001,9002,9003,9004,9005', 'heap: all five pending rows satisfy price < 6');
is($heap->{notq}, '9002,9004,9005', 'heap: the empty documents and "rare" match !common, NULL ones do not');

$node->backup('b');
my $lsn0 = $node->safe_psql('postgres', q{SELECT pg_current_wal_insert_lsn()});
$node->safe_psql('postgres', 'VACUUM da');	# the flush
$node->safe_psql('postgres', 'CREATE TABLE da_anchor AS SELECT 1');
my $lsn1 = $node->safe_psql('postgres', q{SELECT pg_current_wal_lsn()});
my $walfile = $node->safe_psql('postgres', q{SELECT pg_walfile_name(pg_current_wal_lsn())});
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	qq{SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver})
  or die 'timed out waiting for the flush\'s WAL to be archived';

is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('da_w')}), '2',
	'the VACUUM flushed the pending rows into a second bolt');
like($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('da_w') WHERE invariant = 'doclist_coverage'}),
	qr/^2 of 2 bolt\(s\) carry a document list, 2 COMPLETE$/,
	'both bolts carry a COMPLETE list');
is_deeply(answers($node, 'index'), $heap, 'after the flush the index answers as the heap');

my @lsns = split /\n/, $node->safe_psql('postgres', qq{
	SELECT DISTINCT start_lsn
	  FROM pg_get_wal_block_info('$lsn0', '$lsn1')
	 WHERE relfilenode = pg_relation_filenode('da_w') AND relforknumber = 0
	 ORDER BY start_lsn});
cmp_ok(scalar(@lsns), '>=', 5, 'the flush wrote ' . scalar(@lsns) . ' index WAL records');

my $i = 0;
my $max_leaked = 0;
my $seen_list_unpublished = 0;
my $seen_published = 0;
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

	my $nseg = $pitr->safe_psql('postgres', q{SELECT weave_index_nsegments('da_w')});
	my $dlpages = $pitr->safe_psql('postgres',
		q{SELECT count(*) FROM weave_page_info('da_w') WHERE kind = 'doclist'});
	$seen_list_unpublished = 1 if $nseg eq '1' && $dlpages > 1;
	$seen_published = 1 if $nseg eq '2';
	# Pages the flush had already logged when recovery stopped short of its
	# publish record are unreachable and not freed: the G72 leak class,
	# doc/GAPS.md G75.  Counted at every point, which is what turned G75's "once
	# in a while after an immediate stop" into a deterministic measurement; and
	# since G75's fix, ONE VACUUM must reclaim every one of them (below).
	my $leaked = $pitr->safe_psql('postgres', q{
		SELECT count(*) FROM weave_page_info('da_w')
		 WHERE NOT reachable AND coalesce(freed, false) = false AND NOT uninitialized});
	my $leakkinds = $pitr->safe_psql('postgres', q{
		SELECT coalesce(string_agg(kind || ':' || n, ',' ORDER BY kind), '')
		  FROM (SELECT kind, count(*) AS n FROM weave_page_info('da_w')
		         WHERE NOT reachable AND coalesce(freed, false) = false
		           AND NOT uninitialized GROUP BY kind) k});
	note("point $i at $lsn: bolts=$nseg doclist_pages=$dlpages leaked_pages=$leaked [$leakkinds]");
	$max_leaked = $leaked if $leaked > $max_leaked;

	is($pitr->safe_psql('postgres', q{
		SELECT string_agg(invariant || '=' || ok, ',' ORDER BY invariant)
		  FROM weave_check('da_w', true)
		 WHERE invariant IN ('doclist_valid', 'doclist_covers_postings')}),
		'doclist_covers_postings=true,doclist_valid=true',
		"point $i: every published bolt's list validates and covers its postings");
	is_deeply(answers($pitr, 'index'), $heap,
		"point $i (bolts=$nseg): the index answers price<6, !common and the vector ORDER BY as the heap");

	# G75: one VACUUM reclaims what the crash stranded.  VERBOSE so the reclaim's
	# own line is in the log as evidence that the pass ran and what it freed.
	my ($vrc, $vout, $verr) = $pitr->psql('postgres', 'VACUUM (VERBOSE) da');
	is($vrc, 0, "point $i: VACUUM succeeds after recovery") or diag($verr);
	my ($reclaimed) = $verr =~ /reclaimed (\d+) stranded page/;
	ok(defined $reclaimed, "point $i: the VACUUM ran the stranded-page reclaim")
	  or diag($verr);
	$reclaimed //= -1;
	cmp_ok($reclaimed, '>=', $leaked,
		"point $i: the reclaim freed every stranded page ($reclaimed of $leaked)");
	is($pitr->safe_psql('postgres', q{
		SELECT count(*) FROM weave_page_info('da_w')
		 WHERE NOT reachable AND coalesce(freed, false) = false AND NOT uninitialized}),
		'0', "point $i: after one VACUUM no page is leaked (was $leaked)");
	my $bad = $pitr->safe_psql('postgres', q{
		SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail, ''), '; '), '')
		  FROM weave_check('da_w', true) WHERE NOT ok});
	is($bad, '', "point $i: after one VACUUM weave_check(deep) is clean");
	is_deeply(answers($pitr, 'index'), $heap,
		"point $i: after the reclaim the index still answers as the heap");
	$pitr->stop('immediate');
	$pitr->clean_node;
}

# EVIDENCE THE WINDOW WAS HIT, not just that the loop ran: some point must have
# the new list's page on disk while the bolt is still unpublished, and some point
# must have the bolt published.  Without the first the test proves nothing about
# the window between the list write and the publish.
ok($seen_list_unpublished,
	'some recovery point has the new list written but its bolt not yet published');
ok($seen_published, 'and some recovery point has the bolt published');
# And for G75: the reclaim assertions above are vacuous unless some point leaked.
cmp_ok($max_leaked, '>=', 5,
	"some recovery point stranded several pages for the reclaim to find (max $max_leaked)");

done_testing();
