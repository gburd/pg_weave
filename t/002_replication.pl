# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Physical (streaming) replication of the pg_weave weave index.
#
# The weave index is fully WAL-logged via GenericXLog, so a streaming standby
# must reconstruct it from the primary's WAL and answer @@@ / <=> / weave_count
# identically.  This test builds and mutates the index on the primary, waits
# for the standby to catch up, and compares query answers on both nodes --
# including the incremental-insert (pending) and delete/VACUUM (tombstone)
# paths, which must all replicate.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Primary
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
$primary->safe_psql(
	'postgres', q{
	CREATE TABLE docs (id int primary key, d wdoc);
	INSERT INTO docs SELECT g, to_wdoc('alpha doc ' || g)
		FROM generate_series(1, 1500) g;
	CREATE INDEX docs_bm25 ON docs USING weave (d);
});

# Standby from a base backup
my $backup = 'bkp';
$primary->backup($backup);
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup, has_streaming => 1);
$standby->start;

# Mutate the primary AFTER the standby is streaming: pending-list inserts,
# a delete, and a VACUUM (flush + merge + tombstone) -- all must replicate.
$primary->safe_psql(
	'postgres', q{
	INSERT INTO docs SELECT g, to_wdoc('beta doc ' || g)
		FROM generate_series(5000, 5200) g;
	DELETE FROM docs WHERE id <= 100;
	VACUUM docs;
});

# Wait for the standby to replay up to the primary's current WAL position.
$primary->wait_for_catchup($standby);

my $q = "SET enable_seqscan=off;";
for my $case (
	[ 'alpha count', "SELECT count(*) FROM docs WHERE d \@\@\@ 'alpha'::wquery" ],
	[ 'beta count',  "SELECT count(*) FROM docs WHERE d \@\@\@ 'beta'::wquery" ],
	[
		'ranked top-10',
		"SELECT string_agg(id::text, ',') FROM (SELECT id FROM docs WHERE d \@\@\@ 'alpha'::wquery ORDER BY d <=> 'alpha'::wquery LIMIT 10) x"
	],
	[
		'weave_count',
		"SELECT weave_count('docs_bm25', 'alpha'::wquery)"
	])
{
	my ($label, $sql) = @$case;
	my $on_primary = $primary->safe_psql('postgres', "$q $sql");
	my $on_standby = $standby->safe_psql('postgres', "$q $sql");
	is($on_standby, $on_primary, "standby matches primary: $label");
}

# The replicated index must also be MVCC/tombstone-correct on the standby:
# the 100 deleted alpha docs must not appear.
my $alpha_standby = $standby->safe_psql('postgres',
	"$q SELECT count(*) FROM docs WHERE d \@\@\@ 'alpha'::wquery");
is($alpha_standby, 1400, 'standby reflects deletes (1500 - 100 tombstoned)');

# Maintenance functions must refuse to run during recovery: on a hot standby
# weave_merge()/weave_vacuum() would otherwise start work and fail hard at their
# first WAL write.  Each must ERROR cleanly (read-only transaction), not crash.
my ($rc, $stdout, $stderr);
($rc, $stdout, $stderr) =
  $standby->psql('postgres', "SELECT weave_merge('docs_bm25')");
isnt($rc, 0, 'weave_merge() errors on a standby (does not run during recovery)');
like($stderr, qr/cannot run during recovery/,
	'weave_merge() gives the recovery error');
($rc, $stdout, $stderr) =
  $standby->psql('postgres', "SELECT weave_vacuum('docs_bm25')");
isnt($rc, 0, 'weave_vacuum() errors on a standby (does not run during recovery)');
like($stderr, qr/cannot run during recovery/,
	'weave_vacuum() gives the recovery error');
# The standby is still queryable after the rejected maintenance calls.
is( $standby->safe_psql('postgres',
		"$q SELECT count(*) FROM docs WHERE d \@\@\@ 'alpha'::wquery"),
	1400, 'standby still answers queries after rejected maintenance calls');

# Failover: promote the standby to a primary.  The replicated weave index must
# answer identically on the promoted node, and maintenance functions -- rejected
# while it was in recovery -- must now SUCCEED (recovery has ended).
$primary->stop;                  # simulate primary loss
$standby->promote;
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "standby did not leave recovery after promote";

# same answers on the promoted node (ranked + boolean + count)
is( $standby->safe_psql('postgres',
		"$q SELECT count(*) FROM docs WHERE d \@\@\@ 'alpha'::wquery"),
	1400, 'promoted node: alpha count correct (index survived failover)');
is( $standby->safe_psql('postgres',
		"$q SELECT count(*) FROM docs WHERE d \@\@\@ 'beta'::wquery"),
	201, 'promoted node: beta count correct');
is( $standby->safe_psql('postgres', "SELECT weave_count('docs_bm25', 'alpha'::wquery)"),
	1400, 'promoted node: weave_count correct');

# maintenance now works on the promoted primary, and writes replicate/persist
is( $standby->safe_psql('postgres', "SELECT weave_vacuum('docs_bm25') IS NOT NULL"),
	't', 'promoted node: weave_vacuum() now runs (no longer in recovery)');
$standby->safe_psql('postgres',
	"INSERT INTO docs SELECT g, to_wdoc('gamma doc ' || g) FROM generate_series(9000, 9099) g");
is( $standby->safe_psql('postgres',
		"$q SELECT count(*) FROM docs WHERE d \@\@\@ 'gamma'::wquery"),
	100, 'promoted node accepts writes to the index after failover');

$standby->stop;
done_testing();
