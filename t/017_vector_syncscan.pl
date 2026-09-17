# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# The vector weft's LANE ORDER does not depend on where the heap scan started
# (task V7).
#
# WHY THIS NEEDS A CLUSTER OF ITS OWN.  A bolt's warp is its dense docid space and
# a docid is derived from the tid, so lane `w` of the weft must be the `w`-th
# smallest ctid -- which is only the same thing as "the w-th tuple the build
# callback saw" when the scan starts at block 0.  It does not always: pg_weave's
# build passes allow_sync = true to table_index_build_scan()
# (src/am/ambuild.c:3760), so with synchronize_seqscans on, a relation larger than
# NBuffers/4 is scanned starting from wherever the last scan of it left off, and
# the callback sees the tail of the table before its head.  The writer therefore
# sorts its lanes by docid (vec_docid_order() in src/vector/vecwrite.c) before
# packing blocks, and a writer that did not would associate every vector after the
# start block with somebody else's document -- while every count(*) stayed right.
#
# WHAT THE REGRESSION SUITE CANNOT DO ABOUT IT.  Reaching that path needs a
# relation bigger than a quarter of shared_buffers, and shared_buffers is not
# settable at run time: in `make installcheck`'s cluster the threshold is thousands
# of pages.  So sql/vecindex.sql's dead-lane assertion, which is the same
# assertion, runs only against an in-order scan -- and a mutation deleting the sort
# passed it.  Here shared_buffers is 1MB, so NBuffers is 128 and 32 pages is enough.
#
# THE FIXTURE PROVES IT IS NOT VACUOUS BY OBSERVING THE START BLOCK, not by deriving
# it, and the difference cost an hour: a scan that runs to COMPLETION leaves the
# recorded position back at the block it started from, so warming with
# `SELECT count(*)` records 0 and every assertion below passes against an in-order
# scan.  Measured on a plain PostgreSQL 17 cluster: a full scan of a 3,158-page table
# at shared_buffers = 1MB leaves the next scan starting at block 0; a cursor closed
# half way leaves it starting at block 1,568.  So the warm scan here stops early, and
# the guard reads the start block back with `SELECT ctid ... LIMIT 1` and requires it
# to be nonzero before the index is built.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# shared_buffers is the only setting that matters; the others keep the mechanism
# under test single.  max_parallel_maintenance_workers = 0 because a parallel build
# has its own block-order story (each worker accumulates its own bolt) and mixing
# the two would leave a failure with two possible causes.  autovacuum off so no
# background scan of this relation moves the synchronized-scan position.
$node->append_conf('postgresql.conf', <<'EOF');
fsync = off
shared_buffers = 1MB
autovacuum = off
synchronize_seqscans = on
max_parallel_maintenance_workers = 0
EOF
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# 1,200 rows of a 96-dimension vector is ~55 pages at 1MB of shared buffers: over
# the NBuffers/4 sync-scan threshold (32) and over the 16-page report interval.
# Every 7th vector is NULL, because a dead lane is the only place lane order is
# observable: a livemask bit is what differs between "sorted by docid" and "in the
# order the callback happened to see them", and no row count differs at all.
$node->safe_psql('postgres', q{
	CREATE TABLE vs (id serial, d wdoc, v wvec(96));
	INSERT INTO vs(d, v)
	  SELECT to_wdoc('syncscan common' || (g % 5) || ' tag' || g),
	         CASE WHEN g % 7 = 0 THEN NULL
	              ELSE (SELECT '[' || string_agg(((g * 7 + k * 13) % 101 - 50)::text, ',')
	                              || ']' FROM generate_series(1, 96) k)::wvec
	         END
	    FROM generate_series(1, 1200) g});

# Precondition: the relation must be over the NBuffers/4 threshold at which
# heapam.c's initscan() turns synchronized scanning on at all.
my ($pages, $nbuffers) = split /\|/, $node->safe_psql('postgres', q{
	SELECT (pg_relation_size('vs') / 8192)::int,
	       (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')});
ok($pages > $nbuffers / 4,
	"the fixture qualifies for a synchronized scan ($pages pages > "
	. ($nbuffers / 4) . " = NBuffers/4)");

# Warm the shared position with a scan that STOPS EARLY.  ss_report_location()
# records multiples of 16 pages, and a scan that reaches the end reports the block it
# started from -- so a completed scan records 0 and is useless here.
$node->safe_psql('postgres',
	'SELECT count(*) FROM (SELECT ctid FROM vs LIMIT 700) x');

# The guard, and it is an OBSERVATION: the first tuple a fresh seqscan returns comes
# from the block that scan starts at, so a nonzero block here is direct evidence that
# the build below starts mid-relation.  Without this the whole file would pass while
# testing exactly the in-order scan sql/vecindex.sql already covers.
my $startblk = $node->safe_psql('postgres',
	q{SELECT split_part(substr(ctid::text, 2), ',', 1)::int FROM vs LIMIT 1});
cmp_ok($startblk, '>', 0,
	"the next sequential scan of vs starts mid-relation (block $startblk of $pages)");

$node->safe_psql('postgres',
	'CREATE INDEX vs_weave ON vs USING weave (d, v)');

# The control: the same table indexed with the position ignored, so this build
# starts at block 0 and the callback order IS ctid order.  Two indexes over one
# heap that must agree, which is a stronger statement than either alone -- it says
# the stored image does not depend on where the scan started.
$node->safe_psql('postgres', q{
	SET synchronize_seqscans = off;
	CREATE INDEX vs_ctl ON vs USING weave (d, v)});

# THE assertion: a dead lane is at the warp position of a NULL-vector row, where
# warp is the row's ctid RANK and not the order it was fed to the writer.  Stated
# as two EXCEPTs rather than as a count, because the counts match in both worlds:
# 171 of 1,200 rows have a NULL vector either way.
my $mismatch = $node->safe_psql('postgres', q{
	WITH dead AS (
	    SELECT (b.firstwarp + i)::bigint AS w
	      FROM weave_vec_blocks('vs_weave') b, generate_series(0, b.nlanes - 1) i
	     WHERE (b.livemask >> i) & 1 = 0),
	     nulls AS (
	    SELECT (row_number() OVER (ORDER BY ctid) - 1)::bigint AS w, v IS NULL AS isnull_v
	      FROM vs)
	SELECT (SELECT count(*) FROM (SELECT w FROM dead
	                              EXCEPT SELECT w FROM nulls WHERE isnull_v) x)
	     + (SELECT count(*) FROM (SELECT w FROM nulls WHERE isnull_v
	                              EXCEPT SELECT w FROM dead) y)});
is($mismatch, '0',
	'every dead lane is a NULL-vector row and every NULL-vector row is a dead lane, after a scan that started mid-relation');

# ... and there is something to be wrong about: a fixture whose vectors were all
# present would report 0 mismatches with no lanes compared.
my $dead = $node->safe_psql('postgres', q{
	SELECT count(*) FROM weave_vec_blocks('vs_weave') b,
	                      generate_series(0, b.nlanes - 1) i
	 WHERE (b.livemask >> i) & 1 = 0});
is($dead, '171', 'the NULL-vector rows left 171 dead lanes (1200/7)');

# The two indexes agree block for block.  This is the property in its general form:
# the weft's image is a function of the heap's contents, not of the scan's start.
my $same = $node->safe_psql('postgres', q{
	SELECT (SELECT array_agg(livemask ORDER BY segno, blockno)
	          FROM weave_vec_blocks('vs_weave'))
	     = (SELECT array_agg(livemask ORDER BY segno, blockno)
	          FROM weave_vec_blocks('vs_ctl')) AS same});
is($same, 't', 'the weft written from a mid-relation scan is identical to one written from block 0');

# And both are structurally sound, deep -- the run that says the sort did not buy
# the lane order at the cost of a leak or a stale bound.
for my $idx ('vs_weave', 'vs_ctl')
{
	my ($ok, $detail) = split /\|/, $node->safe_psql('postgres',
		"SELECT ok, coalesce(detail, '') FROM weave_check('$idx', true)
		  WHERE NOT ok LIMIT 1"), 2;
	ok(!defined $ok || $ok eq '',
		"weave_check(deep) reports no failed invariant for $idx"
		. (defined $detail ? " ($detail)" : ''));
}

$node->stop;
done_testing();
