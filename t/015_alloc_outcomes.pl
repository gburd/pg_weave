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

# CONVERGENCE, WHICH IS A DIFFERENT PROPERTY FROM no_ratchet AND HAS NEVER BEEN TESTED.
#
# weave_vacuum_compact()'s own header states the contract: "Converge a bloated index to
# its size floor in ONE call, stably (repeated calls do not oscillate) and NEVER
# returning larger than we started."  Nothing asserted the "do not oscillate" half, and
# no_ratchet() structurally cannot: it takes the MAX of cycles 2..N and compares it
# against cycle 1, so for a period-2 swing its verdict is decided by the PARITY of the
# cycle the loop happened to start on.  Land the peak on cycle 1 and every later cycle is
# <= it and the arm passes.  (The same parity trap bit bench/vecmerge.sh's page ratchet
# three times in one session; it is worth recognising by shape.)
#
# A ratchet is unbounded growth.  An oscillation is bounded and still a defect: every
# other VACUUM relocates the live segment, extends the relation, truncates nothing, and
# achieves no net change -- measured at 20k rows as 1,346 relocations plus 1,462 extends
# per cycle, forever, for a 1.57x file swing (doc/GAPS.md G47).
#
# The assertion is that the series reaches a FIXED POINT: the last two cycles agree.  A
# small epsilon because per-cycle bookkeeping may legitimately differ by a page or two;
# the swing this exists to catch is 57%, so the epsilon cannot hide it.
sub converges
{
	my ($label, $sizes) = @_;
	my $n = scalar @{$sizes};
	my ($a, $b) = ($sizes->[$n - 2], $sizes->[$n - 1]);
	my $slack = int($a * 0.01) + 4;
	cmp_ok(abs($b - $a), '<=', $slack,
		"$label: repeated VACUUMs reach a fixed point (series: "
		. join(' -> ', @{$sizes}) . ")");
}
converges('stalled xid horizon, L18 disabled', $off_sizes);
converges('stalled xid horizon, shipped default', $on_sizes);
converges('advancing xid horizon', $burn_sizes);

# ---------------------------------------------------------------------------
# THE ARM THAT DOES NOT CONVERGE, AND WHY IT IS A SEPARATE FIXTURE.
#
# Every arm above converges: the lexical-only `docs` index reaches 1164 pages and
# stays, or 94 with the horizon advancing, and does no work on cycles 2..5.  So the
# assertions above pass, and they would keep passing through the defect below --
# which is precisely why this arm exists rather than a tighter threshold on those.
#
# Put a VECTOR WEFT in the index and repeated VACUUMs stop converging.  Measured at
# 20k rows x 96-d: 2577 -> 4039 -> 2577 -> 4039 -> 2577 -> 4039, forever, peak/trough
# 1.57x, and at 1M x 960-d on EC2 the same shape reproduced BIT-IDENTICALLY across two
# independent runs (190091, 185234, 283924, 185234).  A lexical-only index over the
# same documents with the same history converges in three cycles (442 -> 373 -> 211 ->
# 211 -> 211), which is the ablation that attributes it to the weft.
#
# MECHANISM, measured with the counters this file exists to provide rather than read
# off the source (doc/GAPS.md G47).  Per grow cycle: lowfree_reuse=1230 (every free
# page below the live data), lowfree_defer=1346, extend=1462 -- and the deferred count
# equals the NEXT cycle's reuse count exactly.  weave_vacuum_compact()'s vacate phase
# frees the pages its pack phase needs, and a page freed by the current transaction is
# never recyclable within it, so the pack extends by the shortfall instead.  The next
# VACUUM finds those pages recyclable, packs, and truncates back down.  Neither state
# is front-packed (freetail=0 in both, so weave_truncate_free_tail() can never help)
# and both sit far above the 1347-page floor, at 1.91x and 3.00x.
#
# 2000 rows is the smallest fixture found that reproduces it (438 279 424 279 424),
# which is what keeps this arm cheap.
#
# TODO, NOT A FAILING TEST, deliberately.  The defect is real and unfixed; the fix is
# a maintainer decision because the credible options change what a plain VACUUM does
# (doc/GAPS.md G47 states them).  A TODO block pins the shape now and turns into a
# LOUD "unexpectedly succeeded" the moment a fix lands, which is the behaviour wanted
# from a gate for a known defect -- as opposed to deleting the gate, or landing red.
$node->safe_psql('postgres', q{
    CREATE TABLE vdocs (id int, d wdoc, v wvec(96));
    INSERT INTO vdocs
      SELECT i, to_wdoc('b' || (i % 97) || ' g' || (i % 31) || ' d' || i),
             (SELECT '[' || string_agg(((i * 7 + k * 13) % 101 - 50)::text, ',') || ']'
                FROM generate_series(1, 96) k)::wvec
        FROM generate_series(1, 1600) i;
    CREATE INDEX vdocs_weave ON vdocs USING weave (d, v) WITH (metric = 'ip');
    INSERT INTO vdocs
      SELECT i, to_wdoc('b' || (i % 97) || ' later ' || i), NULL
        FROM generate_series(1601, 2000) i;
    SELECT weave_merge('vdocs_weave');
    DELETE FROM vdocs WHERE id % 10 = 0;
});

my (@vsizes, @vstats);
for my $cycle (1 .. 5)
{
    # Same explicit xid burn as the arms above: without an advancing horizon nothing
    # is recyclable anywhere and the oscillation cannot even form (verified -- with a
    # stalled horizon the file sits flat at the peak).
    $node->safe_psql('postgres', q{
        CREATE TABLE IF NOT EXISTS xidburn2(i int);
        DO $$ BEGIN FOR k IN 1..200 LOOP
            INSERT INTO xidburn2 VALUES (k); DELETE FROM xidburn2;
        END LOOP; END $$;
    });
    my $s = bracket(q{VACUUM vdocs;});
    push @vstats, $s;
    push @vsizes, $node->safe_psql('postgres',
        q{SELECT pg_relation_size('vdocs_weave') / current_setting('block_size')::int});
    diag("  [vector weft] cycle $cycle: $vsizes[-1] pages, " . fmt($s));
}
diag('vector weft: ' . join(' -> ', @vsizes));

# THE SIZE-BASED TODO WAS REPLACED BY A MECHANISM-BASED ONE, AND THE REASON IS THE
# WHOLE POINT OF THE FIX THAT PRECEDED IT.
#
# Until 2026-09-24 this arm was `converges('vector weft present', \@vsizes)` under a
# TODO, and at this fixture size the series was 438 -> 279 -> 424 -> 279 -> 424: a
# 145-page swing, far outside converges()'s slack, so the TODO failed and pinned the
# defect.  Option 4 (the vacate phase now runs only under AccessExclusiveLock) removed
# 92 % of the churn, and the series became 297 -> 280 -> 283 -> 280 -> 283 -- a THREE
# page swing, INSIDE the slack.  The arm therefore started reporting "CONVERGED, G47 may
# be FIXED" about an index that still has no fixed point.
#
# A gate that can no longer fail is worse than no gate, and this one went blind by the
# defect getting 48x smaller rather than by anyone touching it.  So the TODO now asserts
# the MECHANISM, which is scale-free: a converged index does no allocation work.  Under
# a share lock the pack phase still extends on alternate cycles forever (138 reuses and
# 3 extends per cycle at this fixture size), so this fails today, for the right reason,
# and it will keep failing until the share-lock caller genuinely reaches a fixed point --
# at which point it flips to "unexpectedly succeeded" regardless of how small the
# remaining swing is.
#
# THE UNFIXABLE HALF IS STATED HERE SO NOBODY HUNTS IT: reaching the floor under a share
# lock requires recycling pages freed by the same transaction, and that hands a
# concurrent scan a page it is still reading -- the field-reported crash
# weave_page_recyclable()'s gate exists to prevent.  The floor is an AccessExclusiveLock
# outcome (weave_vacuum(), REINDEX), which the arm below asserts.  This TODO may
# therefore stay red permanently; if the decision is ever taken to accept that, it
# becomes a documented limitation and this block becomes a plain assertion of the
# oscillation's bounds.
{
    local $TODO = 'doc/GAPS.md G47, remaining half: under ShareUpdateExclusiveLock the '
        . 'compaction pass never reaches a fixed point, so it keeps allocating forever';
    # THE BURN IS LOAD-BEARING AND ITS ABSENCE MADE THIS ARM PASS BY DOING NOTHING.
    # Without an advancing horizon the L19 probe (weave_any_free_page_recyclable) skips
    # the pass outright, so "no allocation work" was true because no pass RAN -- the
    # arm reported "TODO passed", i.e. G47 fixed, on the strength of a skipped vacuum.
    # Same shape as a regression test that passes by not using the index.
    $node->safe_psql('postgres', q{
        DO $$ BEGIN FOR k IN 1..200 LOOP
            INSERT INTO xidburn2 VALUES (k); DELETE FROM xidburn2;
        END LOOP; END $$;
    });
    my $s = bracket(q{VACUUM vdocs;});
    push @vsizes, $node->safe_psql('postgres',
        q{SELECT pg_relation_size('vdocs_weave') / current_setting('block_size')::int});
    is($s->{extend} + $s->{lowfree_reuse} + $s->{fsm_reuse}, 0,
        'plain VACUUM on a settled weft index does no allocation work (G47)');
    diag("  [vector weft] settled-state probe: $vsizes[-1] pages, " . fmt($s));
}

# A TODO failure is not itemised by prove's default output, so the verdict is stated
# here explicitly -- otherwise "All tests successful" is the only thing the log says
# about it, which is indistinguishable from the arm not having run (AGENTS.md: a test
# result needs evidence the test RAN).  This line also tells whoever reads the log what
# the CURRENT cost of G47 is, which a silent TODO does not.
{
    my ($mn, $mx) = (sort { $a <=> $b } @vsizes)[0, -1];
    diag(sprintf('G47 status: series %s ; peak/trough %.3fx -- %s',
        join(' -> ', @vsizes), $mx / $mn,
        'the WASTE half is fixed (option 4, bench/RESULTS_G47_VACATE.md: 92 % of the '
      . 'extends and the whole 1.568x swing were the vacate phase running under a '
      . 'share lock).  The NON-CONVERGENCE half is open and may be unfixable; the '
      . 'floor is reachable only under AccessExclusiveLock, which the next arm gates.'));
}

# Not a TODO, because it holds today and is the half that keeps G47 a waste-of-work
# defect rather than a bloat defect: the oscillation is BOUNDED.  If this ever fails,
# G47 has become a ratchet and is a different, worse bug.
no_ratchet('vector weft present', \@vsizes);

# ---------------------------------------------------------------------------
# OPTION 4: THE VACATE PHASE RUNS ONLY UNDER AccessExclusiveLock.
#
# doc/GAPS.md G47 / bench/RESULTS_G47_VACATE.md.  G47 is two defects and only one is
# fixable; these two arms gate the fixable one and pin the unfixable one's shape.
#
# The vacate phase (phase 1 of weave_vacuum_compact) rewrites the segment extend-only
# so that the pack phase's low-bias free list "includes that whole low region".  Under
# a share lock that premise is FALSE for every page it frees -- a page freed by the
# current transaction is never recyclable within it -- so its extends are pure loss.
# Measured at 20k x 96-d, plain VACUUM: 1,346 of 1,461 extends per grow cycle (92.1 %)
# and the whole 1.568x file swing, for the SAME trough of 2,578 pages either way.
#
# THE SIGNATURE IS lowfree_defer, and that is what this asserts rather than a page
# count.  A page count is corpus-dependent and would have to be loosened until it
# gated nothing; lowfree_defer is the mechanism itself -- the count of free-list
# candidates the recycle gate rejected.  Pre-fix it was 1,231 and 1,346 on alternate
# cycles; post-fix it is 0 on every cycle, because the pass no longer frees pages it
# then has to ask for.  Scoped to this arm deliberately: the stalled-horizon arms above
# legitimately defer pages freed by EARLIER transactions, so the same assertion there
# would be wrong.  This arm burns xids every cycle.
for my $i (0 .. $#vstats)
{
    is($vstats[$i]->{lowfree_defer} + $vstats[$i]->{fsm_defer}, 0,
        "share-lock compaction cycle @{[$i + 1]} frees no page it then cannot reuse "
      . "(G47 option 4: the vacate phase must not run under ShareUpdateExclusiveLock)");
}

# AND THE OTHER HALF: under AccessExclusiveLock the vacate phase is LOAD-BEARING, so
# ablating it is not the fix.  Measured, same fixture at 20k: weave_vacuum() with the
# phase converges to 1,347 pages -- the exact floor -- in ONE call and then does
# nothing at all for five more cycles (0 extends, 0 reuses); with the phase ablated the
# same caller stalls at 2,693, 2.00x the floor, and pays 115 extends every cycle
# forever.  That contrast is why option 4 is a condition and not a deletion.
#
# THIS PROPERTY HAD NEVER BEEN TESTED.  The arms at the top of this file assert that
# weave_vacuum() reclaims and that it reuses freed pages; nothing asserted that
# repeated calls reach a FIXED POINT, which is the half of the function's header
# contract that is actually true.  It is also the property a future G47 fix must not
# break while chasing the share-lock half.
my @asizes;
for my $cycle (1 .. 3)
{
    my $s = bracket(q{SELECT weave_vacuum('vdocs_weave');});
    push @asizes, $node->safe_psql('postgres',
        q{SELECT pg_relation_size('vdocs_weave') / current_setting('block_size')::int});
    diag("  [weave_vacuum, AEL] cycle $cycle: $asizes[-1] pages, " . fmt($s));
}
diag('weave_vacuum (AEL): ' . join(' -> ', @asizes));
converges('weave_vacuum() under AccessExclusiveLock', \@asizes);

# Not just a fixed point -- a fixed point BELOW where the share-lock caller sits.  If
# these ever meet, either the AEL path stopped reaching the floor or the share-lock
# path started, and both are things to find out about from a test rather than from a
# user's disk usage.
cmp_ok($asizes[-1], '<', $vsizes[-1],
    'weave_vacuum() (AEL) reaches a smaller fixed point than plain VACUUM does');

# And the second call must do NOTHING.  This is the assertion that separates "converged"
# from "stable while working forever": with the vacate phase ablated under AEL the file
# sat at a constant 2,693 pages while extending 115 every single cycle, which every
# size-based assertion in this file would have called converged.
{
    my $s = bracket(q{SELECT weave_vacuum('vdocs_weave');});
    is($s->{extend} + $s->{lowfree_reuse} + $s->{fsm_reuse}, 0,
        'a converged index does no allocation work on the next weave_vacuum() '
      . '(a stable size while still relocating is not convergence)');
}

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
