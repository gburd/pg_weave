# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 033_reclaim_crash_loop.pl -- a workload that crashes mid-flush over and over
# does not grow the index without bound (doc/GAPS.md G75).
#
# Before G75's fix every crash inside a flush's write-before-publish window left
# that flush's pages unreachable and unflagged, and nothing but REINDEX got them
# back, so a crash loop grew the relation by about one flush per crash, forever.
# Now one VACUUM reclaims them.
#
# HOW.  Two tables with identical indexes and identical workloads in one cluster:
# `c` is the one whose flush gets crashed, `t` is the twin that is only flushed
# after recovery.  Each cycle inserts the same pending rows into both, starts a
# VACUUM of `c` in the background, kills the server with an immediate stop as
# soon as `c`'s index has grown (so the stop lands INSIDE the flush, in its
# write-before-publish window), restarts, and VACUUMs both.  Per cycle:
#
#   - the crash is evidence only if it stranded something: count the leaked
#     pages after recovery, and require the total over all cycles to be > 0;
#   - after the VACUUM, `c` has no leaked page and weave_check(deep) is clean;
#   - answers equal the heap.
#
# At the end `c`'s index is no larger than its twin's plus a bound.  The bound is
# one flush's worth of pages per cycle that the reclaim had freed but the next
# flush could not yet reuse (a freed page waits out its XID), which is not
# cumulative: the size is asserted to stop GROWING over the second half of the
# cycles, which is the "bounded" claim exactly.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# 11 cycles, MAINTAINER DECISION 2026-10-06 (G75 decision 2).  18 cycles made the
# whole TAP run ~1,900 s instead of ~380 s, which is too much; 8 (the lead's interim
# cut) stopped at cycle 7, one cycle BEFORE the post-crash compaction growth starts,
# so the size bound below could never fail and prove reported it as "TODO passed".
# 11 reaches cycles 8-10, where the measured excess (2,626 / 5,299 / 7,972 pages in
# pgweave-20261006-010652-fb9f) clears the bound, so the TODO is a live signal again.
# What stays HARD at every cycle -- leaked == 0 after one VACUUM and a clean deep
# check -- needs only enough crashes that land inside a flush, and the
# total-stranded assertion below proves they did.  The 1M-row scale run
# (bench/aws/g75_job.sh, RESULTS_G75.md) is the long loop.  The growth itself is
# task L22 in doc/PHASES.md.
my $cycles = 11;
my $rows = 30000;

my $node = PostgreSQL::Test::Cluster->new('crashloop');
$node->init;
# The walwriter flushes every 1 ms, so the records of a flush in progress reach
# disk almost as they are written and an immediate stop strands what it shows.
$node->append_conf('postgresql.conf', qq{
autovacuum = off
log_min_messages = info
wal_writer_delay = 1ms
wal_writer_flush_after = 0
});
$node->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	CREATE TABLE c (id int, d wdoc, emb wvec(4), price int8);
	CREATE TABLE t (id int, d wdoc, emb wvec(4), price int8);
	CREATE INDEX c_w ON c USING weave (d, emb, price int8_docval_ops);
	CREATE INDEX t_w ON t USING weave (d, emb, price int8_docval_ops);
});

sub pages
{
	my ($idx) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT pg_relation_size('$idx') / current_setting('block_size')::int});
}

sub leaked
{
	my ($idx) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT count(*) FROM weave_page_info('$idx')
		 WHERE NOT reachable AND coalesce(freed, false) = false AND NOT uninitialized});
}

# unreachable and not free: leaked pages plus zero pages a crash left past the
# old end of the file (those are re-recorded in the FSM, not freed)
sub stranded
{
	my ($idx) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT count(*) FROM weave_page_info('$idx')
		 WHERE NOT reachable AND coalesce(freed, false) = false});
}

my $fill = sub {
	my ($tab, $cyc) = @_;
	my $lo = $cyc * $rows + 1;
	my $hi = ($cyc + 1) * $rows;
	return qq{INSERT INTO $tab SELECT g, to_wdoc('simple', 'cyc$cyc w' || g || ' common'),
	          ('[' || (g % 97) || ',' || (g % 13) || ',1,1]')::wvec, g
	          FROM generate_series($lo, $hi) g};
};

my $total_stranded = 0;
my @sizes;
my @twin;
my @strand;
for my $cyc (0 .. $cycles - 1)
{
	$node->safe_psql('postgres', $fill->('c', $cyc) . '; ' . $fill->('t', $cyc));
	# VACUUM c in the background; stop the server as soon as the flush has
	# written pages it has not yet published -- that IS the window
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_until(qr/started/, "\\echo started\nVACUUM c;\n");
	my $inwindow = 0;
	for (1 .. 2000)
	{
		if (leaked('c_w') > 0) { $inwindow = 1; last; }
		last if $node->safe_psql('postgres', q{
			SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'VACUUM c%' AND state = 'active'}) eq '0';
	}
	$node->stop('immediate');
	eval { $bg->quit };
	$node->start;
	# THE TWIN GETS THE SAME VACUUM SCHEDULE: one VACUUM where `c` had the one
	# the stop interrupted (or, once the window is missed, let finish), and one
	# after the restart.  Without this the control is not a control: `c` got two
	# VACUUMs a cycle and the twin one, and a second back-to-back VACUUM after a
	# large merge runs the share-lock compaction that cannot reuse its own frees
	# (the ratchet weave_vacuumcleanup()'s L19 note records) -- measured: from
	# cycle 8, each post-restart VACUUM of `c` rewrote the whole index and
	# extended ~2,080 pages while nothing was stranded, and the twin stayed flat.
	$node->safe_psql('postgres', 'VACUUM t');

	my $leak = leaked('c_w');
	my $strand = stranded('c_w');
	$total_stranded += $leak;
	push @strand, $strand;

	# The allocator's outcomes for the post-crash VACUUM, read in ITS session
	# (the counters are backend-local; am.c says why a zero elsewhere means
	# nothing).  When the excess grows, these name the branch that extended.
	my ($vrc, $vout, $verr) = $node->psql('postgres',
		"SELECT weave_alloc_stats_reset();\nVACUUM c;\nSELECT 'alloc ' || weave_alloc_stats()::text;");
	my ($alloc) = $vout =~ /(alloc .*)/;
	note("cycle $cyc: post-crash VACUUM c " . ($alloc // "no alloc stats: $verr"));
	$node->safe_psql('postgres', 'VACUUM t');
	is(leaked('c_w'), '0', "cycle $cyc: after VACUUM no page is leaked (the crash stranded $leak, window seen: $inwindow)");
	is($node->safe_psql('postgres', q{
		SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail, ''), '; '), '')
		  FROM weave_check('c_w', true) WHERE NOT ok}),
		'', "cycle $cyc: weave_check(deep) is clean");
	push @sizes, pages('c_w');
	push @twin, pages('t_w');
	note("cycle $cyc: leaked=$leak stranded=$strand c_w=$sizes[-1] t_w=$twin[-1]");
}

cmp_ok($total_stranded, '>', 0,
	"the immediate stops landed inside a flush and stranded pages ($total_stranded in $cycles cycles)");

my $c_idx = $node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM c WHERE d @@@ 'common'});
is($c_idx, $node->safe_psql('postgres', 'SELECT count(*) FROM c'),
	'every row of the crashed table answers through its index');
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
	SELECT string_agg(id::text, ',') FROM (SELECT id FROM c ORDER BY emb <-> '[0,0,0,0]'::wvec, id LIMIT 5) s}),
	$node->safe_psql('postgres', q{
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT string_agg(id::text, ',') FROM (SELECT id FROM c ORDER BY emb <-> '[0,0,0,0]'::wvec, id LIMIT 5) s}),
	'the crashed table\'s vector ORDER BY equals the heap');

# BOUNDED.  The crashed index's EXCESS over its never-crashed twin must not
# accumulate.  Unfixed, every crash strands its pages for good, so the excess is
# the running SUM of what the crashes stranded.  Fixed, a crash's pages are freed
# by the next VACUUM's reclaim and reused by the flush of the VACUUM after it (a
# freed page waits out its XID), so the excess at any cycle is about what ONE
# crash stranded, plus up to one flush of allocation-order noise.  Asserted at
# EVERY cycle: the final cycle alone is not evidence, because a compaction that
# happens to fire on one index and not the other swings the difference by
# thousands of pages (measured: +2848 -> -8653 in one cycle).
#
# The bound was measured to discriminate, not assumed to.  Two earlier versions
# of the fix reclaimed every page and still failed it: recording the freed pages
# in the FSM at once (the excess climbed 148, 255, 255, 1076, 1126, 1670, 2259,
# 2848) and running the pass after the flush (256, 2931, 4259, 5565, 7347,
# 7992).  Both leave the flush extending instead of reusing; see the comment at
# the pass in weave_vacuumcleanup().
my @excess = map { $sizes[$_] - $twin[$_] } 0 .. $#sizes;
my ($maxs, $sums) = (0, 0);
for (@strand) { $maxs = $_ if $_ > $maxs; $sums += $_; }
# Slack: 5 % of the twin, for merges and compactions that land in different
# cycles on the two indexes (measured: 844 pages at a cycle that stranded 4).
# One whole flush was tried first and is too loose to tell the two cases apart.
my $slack = 64 + int($twin[-1] / 20);
my $bound = $maxs + $slack;
my $worst = -1e9;
for (@excess) { $worst = $_ if $_ > $worst; }
note('excess of c_w over its twin per cycle: ' . join(' ', @excess)
	  . '; stranded per crash: ' . join(' ', @strand) . "; slack $slack pages");
# TODO, NOT PASSING, AND RECORDED AS A LOSS (doc/GAPS.md G75, "OPEN: the size
# bound").  At 18 cycles the crashed index's excess over its twin reaches ~22,000
# pages on runs where every cycle's stranded pages WERE reclaimed (leaked == 0 and
# a clean deep check, asserted hard above).  The allocator counters say where the
# growth comes from: from the first large merge on, every post-crash VACUUM of
# `c` runs the share-lock compaction (lowfree_reuse 10k-21k, extend ~2,080 --
# the L19 ratchet weave_vacuumcleanup() describes), while the twin, on the same
# VACUUM schedule, does not compact at all.  WHY IS NOW KNOWN (task L22,
# doc/PHASES.md): the trigger fires on both; the twin's pass is stopped by the
# recyclability probe, and the crashed index's pass starts with fewer reusable
# pages than live ones and extends the shortfall.  INSERT order, not the crash,
# picks the index.  A fix that declines that pass made this bound pass and was
# REVERTED, because t/028's truncation control needs the same pass.  So the bound
# stays a visible TODO rather than loosened.  `prove` reports it as
# "not ok # TODO" every run.
TODO:
{
	local $TODO = 'G75 / L22: crash-loop size bound -- crashed index compacts every cycle, twin does not (open, doc/PHASES.md L22)';
cmp_ok($worst, '<=', $bound,
	"at every cycle the crashed index's excess over its twin (worst $worst pages) is at most one crash's stranding ($maxs) + $slack");
# The discriminating case is accumulation: had nothing been reclaimed, the
# excess at the last cycle would be the running sum of the strandings.  So the
# evidence that this run could tell the two apart is that the sum exceeds the
# bound.  Measured on runs that passed the bound: sums of 2,846 to 3,446 pages
# against bounds of 2,200 to 3,000, which is not a wide margin -- so the cycle
# count was raised, rather than the bound loosened, when one run fell short.
cmp_ok($sums, '>', $bound,
	"and the crashes stranded enough in total ($sums pages) that accumulation would exceed that bound ($bound)");

}

$node->stop;
done_testing();
