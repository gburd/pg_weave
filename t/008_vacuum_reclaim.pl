# 008_vacuum_reclaim.pl -- weave_vacuum() must SHRINK a bloated index, not grow it.
#
# Regression for a bug the deletion-XID recycle gate introduced: the gate made
# weave_new_buffer refuse to reuse just-freed pages until their free-XID horizon
# passed.  That is correct for the concurrent INSERT/merge path, but weave_vacuum
# (single-writer, exclusive lock) MUST be able to repack live data into the low
# pages it just freed -- with the gate blocking that, the vacate+pack phase
# extended the relation instead, so weave_vacuum() GREW the index every call
# (e.g. 182MB -> 340MB -> 498MB) and never truncated a tail.  The fix bypasses
# the recycle gate during compaction (weave_lowfree active).
#
# This builds an index, runs weave_vacuum() twice, and asserts: it does not grow,
# it reaches a stable floor, and a second call is idempotent (no further change).

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "shared_buffers = 256MB\n");
$node->append_conf('postgresql.conf', "maintenance_work_mem = 64MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
# a corpus large enough that the index spans many pages (so bloat/truncation is
# measurable), high enough vocabulary that merges actually move pages around.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    INSERT INTO docs(body)
      SELECT (SELECT string_agg('w'||((g*7+s)%4000), ' ') FROM generate_series(1,40) s)
             || ' uid'||g
      FROM generate_series(1, 120000) g;
    CREATE INDEX docs_weave ON docs USING weave (to_wdoc('simple', body));
});

sub idxpages {
    return $node->safe_psql('postgres',
        q{SELECT (pg_relation_size('docs_weave')/current_setting('block_size')::int)::int});
}

my $built = idxpages();
diag("index pages after build: $built");

# First weave_vacuum: must not grow, should compact to a floor.
$node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $v1 = idxpages();
diag("index pages after weave_vacuum #1: $v1");

# Second weave_vacuum: idempotent (already at floor).
$node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $v2 = idxpages();
diag("index pages after weave_vacuum #2: $v2");

# THE assertions: weave_vacuum never grows the index, and converges.
cmp_ok($v1, '<=', $built, 'weave_vacuum() does not grow the index (reclaims, not extends)');
cmp_ok($v2, '<=', $v1 + 1, 'a second weave_vacuum() is idempotent (stable floor, +/-1 page)');

# ---------------------------------------------------------------------------
# THE OTHER HALF OF THE REQUIREMENT: it must still RECLAIM.
#
# Everything above is satisfied by a weave_vacuum() that does NOTHING. There are
# no deletes before it, and after L8/L12 a fresh build is already compact and
# weave_vacuum() returns false -- so `v1 <= built` and `v2 <= v1 + 1` both hold
# trivially. The assertions catch a vacuum that GROWS the index, which is the bug
# this file was written for, and they cannot catch a vacuum that stops shrinking
# one.
#
# That is not hypothetical. pg_fts hit the same growth bug (its `2605d00`, 35 -> 52
# -> 69 MB across cleanups with no rows added) and fixed it by SKIPPING a
# compaction pass whose free space is not yet reusable -- and its own new test then
# caught the fix degrading into never reclaiming at all (18 -> 22 MB), because with
# a stale free-space map the live size is overstated and the pass is skipped
# forever. A skip-style fix for a growth bug degrades exactly this way, and a
# no-growth assertion cannot see it.
#
# So: delete most of the corpus, and require the index to end up strictly smaller
# than it was before the deletes.
#
# TWO CALLS, DELIBERATELY. Pages freed by a merge are not recyclable inside the
# transaction that freed them -- weave_page_recyclable() gates on
# GlobalVisCheckRemovableXid() (src/am/am.c:2252) -- so the first weave_vacuum()
# after a large delete may legitimately reclaim little. The second runs in a new
# transaction where those pages have become removable. Asserting on the first call
# alone would be a flaky test of a correct implementation.
$node->safe_psql('postgres', 'DELETE FROM docs WHERE id % 10 <> 0');
# Plain VACUUM is what drives weave_bulkdelete and marks the tombstones;
# weave_vacuum() is what compacts. Neither substitutes for the other -- plain
# VACUUM never reaches weave_merge_segments_streaming().
$node->safe_psql('postgres', 'VACUUM docs');
my $r1 = $node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $d1 = idxpages();
my $r2 = $node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $d2 = idxpages();
my $nseg = $node->safe_psql('postgres',
    q{SELECT weave_index_nsegments('docs_weave')});
diag("index pages after deleting 90%: weave_vacuum #1 $d1 (returned $r1), "
   . "#2 $d2 (returned $r2), pre-delete $v2, segments $nseg");

# G14/L18: FIXED 2026-09-14, and these assertions are what diagnosed it.
#
# Before the fix: 2289 -> 2296 pages (it GREW by 7, the tombstone bookkeeping
# weave_bulkdelete adds), weave_vacuum() returned FALSE both times, segments = 1.
# The steady state was the state that could not reclaim -- insert-time tiered
# merge drives every index toward exactly one segment -- so the only recovery
# from a delete-heavy workload was REINDEX.
#
# After the fix: 2289 -> 264 pages, TRUE on the first call and FALSE on the
# second. Both of those matter. 264/2289 = 11.5% of the file for 10% of the rows,
# i.e. the reclaim is very nearly proportional and essentially complete. And
# false-on-the-second-call is the convergence property, not a failure: the
# rewrite emits a segment with ndeleted = 0, so the tombstone term is satisfied
# and the second call correctly finds nothing to do.
#
# The cause was NOT the one doc/GAPS.md originally recorded ("a merge of one
# segment is a no-op"). The rewrite machinery always handled a single segment and
# always dropped tombstoned postings; weave_index_is_compacted() simply had no
# term for tombstones, so nothing ever asked it to run. See src/am/amvacuum.c.
#
# These were TODO assertions, and a TODO that starts passing is reported by prove
# as an unexpected success -- which is exactly how this fix was confirmed rather
# than assumed. Kept as ordinary assertions now, with the margin tightened from
# the measurement: it reclaimed to 11.5%, so 80% is far too weak a bar to notice
# a regression. 50% would still pass if the fix half-broke; require 25%.
ok($r1 eq 't',
    'weave_vacuum() REPORTS that it did work after a 90% delete');
cmp_ok($d2, '<', $v2,
    'weave_vacuum() RECLAIMS after a 90% delete (strictly smaller than pre-delete)');
cmp_ok($d2, '<=', int($v2 * 0.25),
    'weave_vacuum() reclaims most of the file, not one page (measured: 11.5%)');
is($nseg, 1, 'still exactly one segment after the reclaiming rewrite');

# And the index is still correct after compaction -- run AFTER the delete/reclaim
# cycle so it covers the tombstoned state too, which is where a merge that
# mishandles tombstones shows up as wrong answers rather than as a size.
my $c = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'w7'::wquery});
my $seq = $node->safe_psql('postgres',
    q{SET enable_indexscan=off; SET enable_bitmapscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'w7'::wquery});
is($c, $seq, 'index results still correct after weave_vacuum compaction');

# ---------------------------------------------------------------------------
# WHAT PLAIN VACUUM DOES ABOUT TOMBSTONES, pinned so the answer is a test result
# rather than an assumption.
#
# L18 fixed weave_index_is_compacted(), which is the floor guard weave_vacuum()
# consults. weave_vacuumcleanup() -- the autovacuum/plain-VACUUM path -- has a
# SECOND and independent trigger for the same rewrite, and it has the same blind
# spot: it fires only when free pages exceed 25% of the file, and a tombstone is
# not a free page. So plain VACUUM is expected NOT to reclaim tombstoned space.
#
# That asymmetry is deliberate, not an oversight, and the reason is the recycle
# gate. weave_vacuum() takes AccessExclusiveLock, which is what licenses
# weave_page_recyclable() (src/am/am.c) to bypass GlobalVisCheckRemovableXid and
# reuse pages inside the same call -- that is why the two-phase vacate+pack
# converges in ONE pass there. Under autovacuum's ShareUpdateExclusiveLock a
# concurrent scan can exist, so the gate must stand, phase 2 cannot pack into the
# pages phase 1 just freed, and the rewrite would EXTEND instead. Adding the
# tombstone term to this trigger without solving that first is how pg_fts got
# 35 -> 52 -> 69 MB across three no-op cleanups.
#
# So this arm asserts the CURRENT, LIMITED behaviour: plain VACUUM must not grow
# the index, and is not required to shrink it. If someone later teaches the
# autovacuum path to reclaim tombstones, this assertion starts failing on the
# shrink and that is the prompt to re-read the paragraph above.
my $pre_pv = idxpages();
$node->safe_psql('postgres', 'DELETE FROM docs WHERE id % 100 = 0');
$node->safe_psql('postgres', 'VACUUM docs');
my $pv1 = idxpages();
$node->safe_psql('postgres', 'VACUUM docs');
my $pv2 = idxpages();
diag("plain VACUUM after a further delete: pre $pre_pv, #1 $pv1, #2 $pv2 "
   . '(tombstone reclaim is weave_vacuum() only -- see the comment above)');
cmp_ok($pv2, '<=', $pre_pv + 8,
    'repeated plain VACUUM does not grow the index (the pg_fts 35->52->69 MB bug)');

$node->stop;
done_testing();
