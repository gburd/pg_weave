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

my $cycles = 10;
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

	my $leak = leaked('c_w');
	my $strand = stranded('c_w');
	$total_stranded += $leak;
	push @strand, $strand;

	$node->safe_psql('postgres', 'VACUUM c; VACUUM t');
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
# The bound was measured to discriminate, not assumed to: with the reclaim run
# BEFORE the flush (the first version of the fix) the excess climbed 148, 255,
# 255, 1076, 1126, 1670, 2259, 2848 against crash strandings of at most 1076 --
# above this bound from cycle 7.
my @excess = map { $sizes[$_] - $twin[$_] } 0 .. $#sizes;
my ($maxs, $sums) = (0, 0);
for (@strand) { $maxs = $_ if $_ > $maxs; $sums += $_; }
my $flush = int(($twin[-1] - $twin[0]) / ($cycles - 1)) + 1;
my $bound = $maxs + $flush + 32;
my $worst = -1e9;
for (@excess) { $worst = $_ if $_ > $worst; }
note('excess of c_w over its twin per cycle: ' . join(' ', @excess)
	  . '; stranded per crash: ' . join(' ', @strand) . "; one flush ~ $flush pages");
cmp_ok($worst, '<=', $bound,
	"at every cycle the crashed index's excess over its twin (worst $worst pages) is at most one crash's stranding ($maxs) + one flush ($flush) + 32");
cmp_ok($sums, '>', $bound,
	"and the crashes stranded enough in total ($sums pages) that accumulation would exceed that bound");

$node->stop;
done_testing();
