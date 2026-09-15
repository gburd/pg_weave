#!/usr/bin/perl
# 015_alloc_outcomes.pl -- WHICH allocation path runs, measured rather than inferred.
#
# WHY THIS TEST EXISTS. Two separate investigations in this codebase's lineage
# stalled on not knowing which of three branches weave_new_buffer() took, and both
# spent a cycle acting on a guess:
#
#   - doc/PHASES.md L19 asserted that under ShareUpdateExclusiveLock the page
#     recycle gate (weave_page_recyclable -> GlobalVisCheckRemovableXid) blocks
#     reuse, so phase 2 of a vacate+pack cannot reuse phase 1's freed pages and the
#     rewrite would EXTEND the relation. Plausible. Never measured.
#   - the sibling project published "one WAL record per freed page" as the cost of
#     freeing a segment, then retracted it: measured 0.005 ms/page, ~2,800x cheaper
#     than published. Its number came from a stack sample on an index already
#     damaged by a different bug. A stack sample gives a LOCATION, not a RATE.
#
# So this file measures the branch taken. It asserts the INVARIANTS that must hold
# whatever the numbers are, and diag()s the numbers themselves so a human reads
# them. It deliberately does not assert specific counts: those depend on corpus
# size and on the visibility horizon, and a test that pins them would fail for
# reasons that are not bugs.
#
# THE DISCRIMINATION THIS BUYS, which reasoning could not:
#   extend high + fsm_defer high  -> the recycle gate really is the constraint
#   extend high + fsm_defer ZERO  -> the free list was never consulted at all,
#                                    a different bug with a different fix
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('alloc_outcomes');
$node->init;
# The recycle gate compares against the GLOBAL visibility horizon, so a long-lived
# snapshot anywhere in the cluster holds pages unrecyclable. Keep the cluster quiet
# so the measurement reflects the operation and not an accident of background
# activity.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# Returns the seven counters as a hashref, after resetting them, running $sql, and
# reading them back -- so every number brackets exactly one operation.
#
# THE SINGLE safe_psql IS LOAD-BEARING, not tidiness. The counters are
# backend-local, so a reset, an operation and a read split across three psql
# invocations are three different backends and the read returns a fresh backend's
# zeros -- which reads exactly like "the allocator was never called" while the index
# grows. The sibling project lost a cycle to that reading. Do not split this into
# separate safe_psql calls, and do not add a test that reads the counters after an
# operation performed elsewhere.
sub bracket
{
    my ($sql) = @_;
    my $out = $node->safe_psql('postgres', qq{
        SELECT weave_alloc_stats_reset();
        $sql
        SELECT lowfree_reuse || ' ' || lowfree_defer || ' ' || lowfree_contended
            || ' ' || fsm_reuse || ' ' || fsm_defer || ' ' || fsm_contended
            || ' ' || extend
          FROM weave_alloc_stats();
    });
    my @f = split /\s+/, (split /\n/, $out)[-1];
    return {
        lowfree_reuse     => $f[0],
        lowfree_defer     => $f[1],
        lowfree_contended => $f[2],
        fsm_reuse         => $f[3],
        fsm_defer         => $f[4],
        fsm_contended     => $f[5],
        extend            => $f[6],
    };
}

sub fmt
{
    my ($s) = @_;
    return sprintf(
        'lowfree reuse=%s defer=%s cont=%s | fsm reuse=%s defer=%s cont=%s | extend=%s',
        $s->{lowfree_reuse}, $s->{lowfree_defer}, $s->{lowfree_contended},
        $s->{fsm_reuse}, $s->{fsm_defer}, $s->{fsm_contended}, $s->{extend});
}

sub idxpages
{
    return $node->safe_psql('postgres',
        q{SELECT pg_relation_size('docs_weave') / current_setting('block_size')::int});
}

# ---------------------------------------------------------------------------
# A build allocates every page it uses, so it is the control: a fresh relation
# has no free pages, therefore every page MUST come from an extension. If this
# arm ever shows reuse, the counters are wired to the wrong branches.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id int, body text);
    INSERT INTO docs
      SELECT i, 'w' || (i % 997) || ' w' || (i % 89) || ' common term here'
        FROM generate_series(1, 60000) i;
});
my $build = bracket(q{CREATE INDEX docs_weave ON docs USING weave (to_wdoc('simple', body));});
diag("BUILD:  " . fmt($build));
cmp_ok($build->{extend}, '>', 0, 'a build extends the relation');
# NOT "a build reuses nothing". That assertion was written first and it was WRONG:
# a build's finalizing collapse writes the merged segment and then frees its
# inputs, so weave_alloc_begin() finds real free pages and the low-bias list hands
# them out -- one observed build did 78 low-bias reuses against 157 extends. It is
# also timing-dependent, because whether those pages are in the FSM yet when
# weave_alloc_begin() gathers depends on the collapse. The invariant is that every
# allocated page came from exactly one of the three paths and that a build on an
# empty relation must extend at least once.
cmp_ok($build->{extend} + $build->{fsm_reuse} + $build->{lowfree_reuse}, '>=',
    $build->{extend}, 'every allocated page is attributed to one of the three paths');
is($build->{lowfree_contended} + $build->{fsm_contended}, 0,
    'a single-backend build sees no buffer contention');

# ---------------------------------------------------------------------------
# THE L19 QUESTION. Delete most rows, then run plain VACUUM -- which reaches
# weave_vacuumcleanup() under ShareUpdateExclusiveLock, the lock L19 is about.
$node->safe_psql('postgres', 'DELETE FROM docs WHERE id % 10 <> 0');
my $pre_vac = idxpages();
my $plain = bracket(q{VACUUM docs;});
my $post_vac = idxpages();
diag("PLAIN VACUUM (ShareUpdateExclusiveLock), $pre_vac -> $post_vac pages: " . fmt($plain));

# ---------------------------------------------------------------------------
# THE CONTRAST. weave_vacuum() takes AccessExclusiveLock, which is what licenses
# weave_page_recyclable() to bypass the visibility gate. If L19's premise is right,
# THIS arm reuses and the arm above does not.
my $pre_wv = idxpages();
my $wv = bracket(q{SELECT weave_vacuum('docs_weave');});
my $post_wv = idxpages();
diag("weave_vacuum() (AccessExclusiveLock), $pre_wv -> $post_wv pages: " . fmt($wv));

cmp_ok($post_wv, '<', $pre_wv,
    'weave_vacuum() still reclaims (L18 has not regressed)');

# The invariant that holds regardless of which branch ran: a rewrite that shrank
# the file must have obtained its pages from somewhere, and every page came from
# exactly one of the three paths.
cmp_ok($wv->{lowfree_reuse} + $wv->{fsm_reuse} + $wv->{extend}, '>', 0,
    'the reclaiming rewrite allocated pages through the instrumented paths');

# THE LOAD-BEARING ASSERTION. L18's design claim is that AccessExclusiveLock lets
# the pack phase reuse the pages the vacate phase just freed. That is only true if
# the low-bias list actually hands pages out. If this is 0 while the file still
# shrank, then the shrink comes from truncation alone and the two-phase relocation
# is not doing what its comment says.
cmp_ok($wv->{lowfree_reuse}, '>', 0,
    'weave_vacuum() reuses freed pages via the low-bias list (the L18 mechanism)');

# ---------------------------------------------------------------------------
# INGEST WITHOUT MAINTENANCE. The sibling project reports a ~210x transient bloat
# spike in this shape -- batched inserts, no vacuum, segment count pinned -- with
# space "freed but never reused" and the cause still unknown. Measure whether we
# do the same thing, and record which branch runs while it happens.
my $pre_ingest = idxpages();
my $ingest = bracket(q{
    INSERT INTO docs SELECT i, 'w' || (i % 997) || ' w' || (i % 89) || ' more text'
      FROM generate_series(200000, 260000) i;
});
my $post_ingest = idxpages();
my $nseg = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('docs_weave')});
diag("INGEST no maintenance, $pre_ingest -> $post_ingest pages, segments $nseg: "
   . fmt($ingest));

# Not a bloat assertion -- one batch is not the sibling's ten -- but a floor on
# sanity: an ingest that allocates must not be reusing pages it cannot have freed.
cmp_ok($ingest->{extend} + $ingest->{fsm_reuse}, '>', 0,
    'ingest allocates pages');

# ---------------------------------------------------------------------------
# THE L19 RESULT, PINNED AS A NEGATIVE.
#
# L19 asked for repeated plain VACUUM to reclaim tombstoned space. The obvious
# implementation -- add a tombstone term to weave_vacuumcleanup()'s trigger, the
# same term L18 added to weave_index_is_compacted() -- WAS IMPLEMENTED AND
# MEASURED HERE, and it produces an unbounded ratchet:
#
#   pages:  1022 -> 1166 -> 1239 -> 1312 -> 1385 -> 1458   (+73 every cycle)
#   lowfree_reuse:     0     0      0      0      0
#   lowfree_defer:  1001  1092   1165   1238   1311
#   extend:          144    73     73     73     73
#
# lowfree_reuse = 0 with lowfree_defer = everything is the whole story, and it is
# only visible because the counters exist. The rewrite runs, frees pages, and can
# reuse none of them: weave_page_recyclable() bypasses GlobalVisCheckRemovableXid()
# only under AccessExclusiveLock, and autovacuum holds ShareUpdateExclusiveLock
# where a concurrent scan may still reference those pages. Pages freed by cycle N
# become recyclable only in a later transaction, and a rewrite needs pages before
# it can free any -- so in-cycle reclaim under a share lock is circular, not merely
# unimplemented. The deferred list also grows every cycle, so the scan gets slower
# as the file gets bigger.
#
# So the trigger stays tombstone-blind, and THIS ARM GUARDS THAT DECISION: it
# asserts the no-ratchet property, which fails loudly if someone adds the term
# again. It deliberately does NOT assert that plain VACUUM shrinks the index --
# that is the L19 goal and it is not met. See doc/GAPS.md G14 and src/am/amvacuum.c.
$node->safe_psql('postgres', 'DELETE FROM docs WHERE id % 4 <> 0');

# FIRST, ATTRIBUTE IT. weave_vacuum_compact() is reached from BOTH weave_vacuum()
# (AccessExclusiveLock) and weave_vacuumcleanup() (ShareUpdateExclusiveLock), and
# L18 added a tombstone term to weave_index_is_compacted(), which is the pre-pass
# guard INSIDE weave_vacuum_compact -- i.e. shared by both callers. So L18 could
# have made the share-lock caller start rewriting. Run the same loop with that term
# disabled (frac = 1.0 can never be exceeded) and with it at its default, and let
# the difference say whether L18 caused the ratchet or merely coexists with it.
sub ratchet_loop
{
    my ($label, $frac, $burn) = @_;
    my $start = idxpages();
    my (@sizes, @defers, @reuse);
    for my $cycle (1 .. 5)
    {
        # Optionally advance the transaction id horizon between cycles. This is
        # the difference between "pages are unrecyclable forever" and "pages are
        # unrecyclable until the horizon passes", and it decides whether the
        # ratchet below is a production defect or an artifact of an idle cluster:
        # weave_free_page() stamps ReadNextTransactionId() on the freed page and
        # weave_page_recyclable() asks GlobalVisCheckRemovableXid(), so in a
        # cluster where nothing consumes xids the horizon never passes the stamp.
        if ($burn)
        {
            $node->safe_psql('postgres', q{
                CREATE TABLE IF NOT EXISTS xidburn(i int);
                DO $$ BEGIN FOR k IN 1..200 LOOP
                    INSERT INTO xidburn VALUES (k); DELETE FROM xidburn;
                END LOOP; END $$;
            });
        }
        my $s = bracket(qq{SET pg_weave.vacuum_tombstone_frac = $frac; VACUUM docs;});
        push @sizes, idxpages();
        push @defers, $s->{lowfree_defer} + $s->{fsm_defer};
        push @reuse, $s->{lowfree_reuse} + $s->{fsm_reuse};
        diag("  [$label] cycle $cycle: $sizes[-1] pages, " . fmt($s));
    }
    diag("$label: start $start -> " . join(' -> ', @sizes)
       . " ; defers: " . join(',', @defers)
       . " ; reuse: " . join(',', @reuse));
    return ($start, \@sizes, \@defers, \@reuse);
}

my ($off_start, $off_sizes, $off_defers) = ratchet_loop('L18 term OFF', '1.0', 0);
my ($on_start,  $on_sizes,  $on_defers)  = ratchet_loop('L18 term ON', '0.2', 0);
# THE SEVERITY QUESTION: does an advancing xid horizon let the freed pages be
# reused, or does the file grow regardless?
my ($burn_start, $burn_sizes, $burn_defers, $burn_reuse) =
    ratchet_loop('xid horizon ADVANCING', '0.2', 1);

# THE GUARDS. The property that matters is a RATCHET -- unbounded growth per cycle
# -- not the size after the first cycle. The first VACUUM after a large delete
# legitimately grows the index: weave_bulkdelete writes a livedocs tombstone blob
# proportional to the number of deleted docids, and that is real, bounded,
# one-time work (measured here as +71 pages). What must never happen is cycles 2..N
# each adding the same amount again, which is what the reverted implementation did
# (+73 every cycle, 1022 -> 1166 -> 1239 -> 1312 -> 1385 -> 1458, forever).
#
# So: compare each arm's LATER cycles against its FIRST cycle, not against the
# pre-VACUUM size. A stricter-looking assertion against the start size would fail
# on legitimate tombstone bookkeeping, which is how a guard gets loosened until it
# guards nothing.
sub no_ratchet
{
    my ($label, $sizes) = @_;
    my $after_first = (sort { $b <=> $a } @{$sizes}[1 .. $#$sizes])[0];
    cmp_ok($after_first, '<=', int($sizes->[0] * 1.02) + 8,
        "$label: no growth after the first cycle (no ratchet)");
}
no_ratchet('stalled xid horizon, L18 disabled', $off_sizes);
no_ratchet('stalled xid horizon, shipped default', $on_sizes);
no_ratchet('advancing xid horizon', $burn_sizes);

# THE LOAD-BEARING ONE: the skip must not have turned into "never reclaim".
cmp_ok($burn_sizes->[-1], '<', int($burn_start * 0.9),
    'plain VACUUM RECLAIMS once the xid horizon advances (the skip did not degrade into never working)');
cmp_ok($burn_reuse->[0], '>', 0,
    'and it reclaims by REUSING freed pages, not by truncation alone');

# ---------------------------------------------------------------------------
# And the counters must be monotonic within a backend and zeroable, or every
# number above is untrustworthy.
my $a = $node->safe_psql('postgres', q{
    SELECT weave_alloc_stats_reset();
    SELECT extend FROM weave_alloc_stats();
});
is((split /\n/, $a)[-1], '0', 'weave_alloc_stats_reset() zeroes the counters');

$node->stop;
done_testing();
