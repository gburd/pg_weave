# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Crash recovery of a VECTOR-BEARING index (task V7's stated gate: "the
# crash-recovery TAP test extended to a vector index").
#
# WHY A SEPARATE FILE FROM t/014_merge_durability.pl.  That test asserts that
# weave_merge() and weave_vacuum() flush their own WAL, which is a property of the
# maintenance functions.  This one asserts a property of the WRITER: the vector
# weft -- a WEAVE_VMETA page, a WEAVE_PK_VDIR directory chain and a WEAVE_PK_VCODES
# strip chain, hundreds of pages of it -- is written entirely through GenericXLog
# (AGENTS.md hard rule 2), so an immediate shutdown must leave a weft that is
# complete, reachable, and whose every directory record still recomputes from the
# codes stored beside it.
#
# WHAT WOULD BE MISSED WITHOUT IT.  A writer that used log_newpage, or that linked a
# chain in a different GenericXLog cycle from the page it links to, produces an index
# that is perfectly good until the first crash and then has a strip chain that stops
# halfway.  Nothing in the regression suite can see that: it builds, checks and drops
# in one session with no crash in between.  And the check that catches it is not
# "does the index still answer" -- the LEXICAL half answers fine with a truncated
# vector weft -- it is weave_check(deep), whose reachability walk follows the chains
# and whose vector_block_stats_match_codes reads every block back.
#
# Deliberately NO CHECKPOINT before the crash, for the same reason as t/014: a
# checkpoint would put the pages on disk and leave the WAL path untested.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Print every page the reachability walk did not reach.  A leak report can only
# carry a count and a first block; the page's KIND is what names the write path that
# left it, and for a weft that is the difference between "the strip chain" and "the
# directory chain".  Copied in spirit from t/014, which needed exactly this to
# diagnose doc/GAPS.md G21.
sub dump_orphans
{
	my ($node, $label) = @_;
	my $rows = $node->safe_psql('postgres', q{
		SELECT string_agg(blkno || ' kind=' || coalesce(kind, '(none)')
		                  || ' kind_id=' || coalesce(kind_id::text, '?')
		                  || ' nextblk=' || coalesce(nextblk::text, 'none')
		                  || ' lsn=' || lsn,
		                  E'\n' ORDER BY blkno)
		  FROM weave_page_info('vd_weave')
		 WHERE NOT reachable AND coalesce(freed, false) = false
		   AND NOT uninitialized});
	return if $rows eq '';
	diag("$label: orphaned page(s) in vd_weave\n$rows");
	return;
}

sub check_clean
{
	my ($node, $label) = @_;
	my ($ok, $detail) = split /\|/,
		$node->safe_psql('postgres',
			q{SELECT ok, coalesce(detail, '') FROM weave_check('vd_weave', true)
			   WHERE NOT ok LIMIT 1}), 2;
	ok(!defined $ok || $ok eq '',
		"weave_check(deep) reports no failed invariant $label"
		. (defined $detail ? " ($detail)" : ''));
	dump_orphans($node, $label);
	return;
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# fsync off for the same reason t/014 does it: an immediate stop kills the
# postmaster, so anything already written(2) survives in the OS.  What is under test
# is whether the records were written at all.
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# 96 dimensions at the default 4 bits: one lane strip plus one centroid strip per
# block, so both strip flavours and the directory chain are all on disk.  2,000 rows
# is 63 blocks -- more than one directory page (28 records per page), which is the
# thing that makes the directory a CHAIN rather than a single page and therefore the
# thing a crash can truncate.  Every 9th vector is NULL, so dead lanes are in the
# image too.
$node->safe_psql('postgres', q{
	CREATE TABLE vd (id serial, body text, emb wvec(96));
	INSERT INTO vd(body, emb)
	  SELECT 'vecdur common' || (g % 11) || ' tag' || g,
	         CASE WHEN g % 9 = 0 THEN NULL
	              ELSE (SELECT '[' || string_agg(((g * 3 + k * 7) % 97 - 48)::text, ',')
	                              || ']' FROM generate_series(1, 96) k)::wvec
	         END
	    FROM generate_series(1, 2000) g;
	CREATE INDEX vd_weave ON vd USING weave (to_wdoc('simple', body), emb);
});

# The weft exists before the crash.  Asserted rather than assumed: a test that
# crashes an index with no vector pages in it passes vacuously, which is the failure
# mode two earlier versions of t/014 had.
my ($nblocks_before, $npages_before) = split /\|/, $node->safe_psql('postgres', q{
	SELECT (SELECT count(*) FROM weave_vec_blocks('vd_weave')),
	       (SELECT sum(npages) FROM weave_index_size_detail('vd_weave')
	         WHERE kind IN ('vector_meta', 'vector_dir', 'vector_codes'))});
cmp_ok($nblocks_before, '>', 28,
	"the weft has $nblocks_before blocks, so its directory is a multi-page chain");
cmp_ok($npages_before, '>', 100,
	"the weft occupies $npages_before pages, so a truncated chain would be visible");

my $live_before = $node->safe_psql('postgres',
	q{SELECT sum(nlive) FROM weave_vec_blocks('vd_weave')});
my $stats_before = $node->safe_psql('postgres', q{
	SELECT md5(string_agg(blockno || ':' || livemask || ':' || smax || ':'
	                      || maxrecnorm || ':' || minnorm || ':' || censcale
	                      || ':' || cenrad, ',' ORDER BY segno, blockno))
	  FROM weave_vec_blocks('vd_weave')});
my $rows_before = $node->safe_psql('postgres', q{SELECT count(*) FROM vd});

check_clean($node, 'before the crash');

# NOTHING between the build and the crash.  No CHECKPOINT, and no XID-acquiring
# statement either: CREATE INDEX does acquire an XID (it writes catalog rows), so
# unlike weave_merge() its WAL is flushed on commit -- but the pages themselves are
# only in the WAL if every one of them went through GenericXLog.
$node->stop('immediate');
$node->start;

# --- the weft survived, in full -------------------------------------------
my ($nblocks_after, $npages_after) = split /\|/, $node->safe_psql('postgres', q{
	SELECT (SELECT count(*) FROM weave_vec_blocks('vd_weave')),
	       (SELECT sum(npages) FROM weave_index_size_detail('vd_weave')
	         WHERE kind IN ('vector_meta', 'vector_dir', 'vector_codes'))});
is($nblocks_after, $nblocks_before,
	"every one of the $nblocks_before vector blocks is still readable after recovery");
is($npages_after, $npages_before,
	'the weft occupies the same number of pages after recovery');

is($node->safe_psql('postgres',
		q{SELECT sum(nlive) FROM weave_vec_blocks('vd_weave')}),
	$live_before,
	"the live-lane count survived ($live_before lanes), so no dead lane was invented");

# The bound fields are the (C2) inputs, and a torn page that recovered to an EARLIER
# version of itself would still parse -- it would just carry someone else's bounds.
# Hashing them is how "recovered" is distinguished from "recovered to the same
# bytes".
is($node->safe_psql('postgres', q{
		SELECT md5(string_agg(blockno || ':' || livemask || ':' || smax || ':'
		                      || maxrecnorm || ':' || minnorm || ':' || censcale
		                      || ':' || cenrad, ',' ORDER BY segno, blockno))
		  FROM weave_vec_blocks('vd_weave')}),
	$stats_before,
	'every block bound recovered to the same value it had before the crash');

# And the check that actually reads the codes back: reachability (no leaked strip or
# directory page) plus vector_block_stats_match_codes (every record recomputes from
# the codes on the pages that recovered).
check_clean($node, 'after the crash');

is($node->safe_psql('postgres', q{SELECT count(*) FROM vd}), $rows_before,
	'the lexical half still answers over every row after recovery');

# --- a merge on a vector index, then a crash ------------------------------
# sect. 7.3's interim rule is that a merge SKIPS a group containing a vector-bearing
# bolt.  A skipped merge must leave the index EXACTLY as it found it -- including
# after a crash, which is the state where "it left the metapage half-updated" would
# show up.  weave_merge() is reachable from VACUUM's cleanup, so this is also the
# assertion that the skip is not an ereport.
$node->safe_psql('postgres', q{
	INSERT INTO vd(body, emb) SELECT 'late tag' || g, NULL
	  FROM generate_series(9001, 9050) g});
my $nseg_before_merge = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('vd_weave')});
my $merged = $node->safe_psql('postgres', q{SELECT weave_merge('vd_weave')});
my $nseg_after_merge = $node->safe_psql('postgres',
	q{SELECT weave_index_nsegments('vd_weave')});

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('vd_weave')}),
	$nseg_after_merge,
	"the segment count is unchanged by the crash "
	. "(before merge: $nseg_before_merge, after: $nseg_after_merge, merged: '$merged')");
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_vec_blocks('vd_weave')}),
	$nblocks_before,
	'the skipped merge did not drop, rewrite or truncate the weft');
check_clean($node, 'after the merge-and-crash');

# --- VACUUM, which walks the free path, then a crash ----------------------
# weave_vacuum() reclaims and truncates.  With a vector weft present, that exercises
# the one path that FREES weft pages (weave_free_segment -> weave_vec_free_weft) and
# is therefore where a free path that forgot the strip chain leaves a leak that
# survives a crash.
$node->safe_psql('postgres', q{
	DELETE FROM vd WHERE id % 10 = 0;
	VACUUM vd;
});
$node->safe_psql('postgres', q{SELECT weave_vacuum('vd_weave')});
my $size_vacuumed = $node->safe_psql('postgres',
	q{SELECT pg_relation_size('vd_weave')});

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT pg_relation_size('vd_weave')}),
	$size_vacuumed,
	'weave_vacuum() on a vector index survived an immediate shutdown');
check_clean($node, 'after the vacuum-and-crash');

# REINDEX writes a whole new weft and frees the old one, then a crash.  This is the
# largest free-path exercise available: if weave_vec_free_weft() misses a chain, the
# old weft's pages are unreachable and unflagged, and the count is in the hundreds.
$node->safe_psql('postgres', q{REINDEX INDEX vd_weave});
my $blocks_reindexed = $node->safe_psql('postgres',
	q{SELECT count(*) FROM weave_vec_blocks('vd_weave')});
cmp_ok($blocks_reindexed, '>', 0, 'REINDEX rewrote the weft');

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_vec_blocks('vd_weave')}),
	$blocks_reindexed,
	'the rebuilt weft survived an immediate shutdown');
check_clean($node, 'after the reindex-and-crash');

done_testing();
