# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 032_reclaim_concurrent.pl -- VACUUM's reclaim of stranded pages never frees a
# page a concurrent writer is still writing (doc/GAPS.md G75;
# doc/specs/SEGMENT_FORMAT.md sect. 10, "Pages a crash strands between write and
# link").
#
# A reclaim that frees a LIVE page is a corruption, not a leak, so this is the
# test that matters for G75 -- t/031 only shows the reclaim finds the pages a
# crash left.  The two writers that can run beside the reclaim are the oversized
# INSERT (writes a whole bolt, then publishes it, holding neither the
# maintenance mutex nor any lock VACUUM conflicts with) and the pending append
# (links each page in the record that writes it).  Two mechanisms keep the
# reclaim off their pages, and each phase below exercises one and PROVES it was
# exercised before it is allowed to count:
#
#   A. THE BARRIER.  An oversized INSERT is mid-write when the VACUUM starts.
#      The reclaim must wait for it.  Evidence: log_lock_waits reports the
#      VACUUM waiting for ExclusiveLock on page 4294967295 (the segment-write
#      lock, WEAVE_SEGWRITE_LOCKBLK) while the inserter holds it.
#   B. THE FENCE.  An oversized INSERT starts AFTER the barrier, while the
#      reclaim scans (slowed by vacuum_cost_delay), and writes into free pages
#      below the length the scan read.  Evidence: the reclaim's own VERBOSE line
#      counts those pages as "newer than the fence".
#
# Both phases end with weave_check(deep) clean and every query equal to the
# heap.  A reclaim that freed the in-flight bolt's pages would show up as the
# published bolt reaching freed pages (chains_*), a leak count, or a wrong
# answer -- and the mutants that remove each mechanism are expected to fail
# exactly here (doc/GAPS.md G75, mutation table).
#
# Phase 0 first: on a healthy index carrying every channel, the reclaim frees
# NOTHING.  A walk that missed a page kind would free that kind's live pages on
# every VACUUM; this is the guard that it does not, on every weft the format
# has.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
log_lock_waits = on
deadlock_timeout = 10ms
});
$node->start;

$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	-- n distinct terms, deterministic: the index expression builds the big
	-- document, so the heap row stays tiny and the oversized path is the only
	-- slow part of an INSERT.
	CREATE FUNCTION bigdoc(n int, k int) RETURNS wdoc IMMUTABLE LANGUAGE sql
	  AS $$ SELECT public.to_wdoc('simple'::regconfig, 'big' || ' ' ||
	          (SELECT string_agg('t' || k || 'x' || g, ' ') FROM generate_series(1, n) g)) $$;
});

sub leaked
{
	my ($idx) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT count(*) FROM weave_page_info('$idx')
		 WHERE NOT reachable AND coalesce(freed, false) = false AND NOT uninitialized});
}

sub deep_bad
{
	my ($idx) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT coalesce(string_agg(invariant || ': ' || coalesce(detail, ''), '; '), '')
		  FROM weave_check('$idx', true) WHERE NOT ok});
}

sub vacuum_verbose
{
	my ($sql) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', $sql);
	is($rc, 0, "$sql succeeds") or diag($err);
	my ($freed, $rec, $newer, $busy) =
	  $err =~ /reclaimed (\d+) stranded page\(s\), re-recorded (\d+) free page\(s\); (\d+) unreachable page\(s\) newer than the fence, (\d+) busy/;
	ok(defined $freed, "$sql ran the stranded-page reclaim") or diag($err);
	return ($freed // -1, $rec // -1, $newer // -1, $busy // -1);
}

# ---- phase 0: a healthy index with every weft reclaims nothing --------------
$node->safe_psql('postgres', q{
	CREATE TABLE allch (id int, d wdoc, body text, emb wvec(4), price int8);
	INSERT INTO allch
	  SELECT g, to_wdoc('simple', 'alpha w' || g || ' beta' || (g % 13)),
	         'gram text number ' || g,
	         ('[' || g || ',' || (g % 7) || ',1,2]')::wvec, g
	    FROM generate_series(1, 3000) g;
	CREATE INDEX allch_w ON allch USING weave (d, body gram_ops, emb, price int8_docval_ops)
	  WITH (positions = on, trigrams = on);
	INSERT INTO allch
	  SELECT g, to_wdoc('simple', 'gamma w' || g), 'more gram ' || g,
	         ('[' || g || ',1,1,1]')::wvec, g FROM generate_series(3001, 3400) g;
	DELETE FROM allch WHERE id % 17 = 0;
});
my $kinds = $node->safe_psql('postgres', q{
	SELECT string_agg(DISTINCT kind, ',' ORDER BY kind) FROM weave_page_info('allch_w')
	 WHERE reachable});
note("phase 0 live page kinds: $kinds");
like($kinds, qr/surf_trie/, 'phase 0 precondition: the index has a fuzzy weft');
like($kinds, qr/vector_codes/, 'phase 0 precondition: the index has a vector weft');
like($kinds, qr/cgram_postings/, 'phase 0 precondition: the index has a cgram weft');
like($kinds, qr/docvalues/, 'phase 0 precondition: the index has a docvalues weft');
like($kinds, qr/trigram_data/, 'phase 0 precondition: the index has trigram blobs');
like($kinds, qr/doclist/, 'phase 0 precondition: the index has a document list');
like($kinds, qr/pending/, 'phase 0 precondition: the index has pending pages');
for my $pass (1 .. 2)
{
	my ($freed) = vacuum_verbose('SET client_min_messages = debug2; VACUUM allch');
	is($freed, 0, "phase 0 pass $pass: a healthy all-channel index has nothing to reclaim");
	is(deep_bad('allch_w'), '', "phase 0 pass $pass: weave_check(deep) is clean");
}
$node->safe_psql('postgres', q{SELECT weave_vacuum('allch_w')});
is(deep_bad('allch_w'), '', 'phase 0: clean after weave_vacuum too');
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off; SELECT count(*) FROM allch WHERE d @@@ 'alpha'}),
	$node->safe_psql('postgres', q{
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM allch WHERE d @@@ 'alpha'}),
	'phase 0: the index still answers as the heap');

# ---- the table for phases A and B --------------------------------------------
$node->safe_psql('postgres', q{
	CREATE TABLE big (id int, n int);
	CREATE INDEX big_w ON big USING weave (bigdoc(n, id));
	INSERT INTO big VALUES (1, 10), (2, 10);
});

# ---- phase A: the barrier ----------------------------------------------------
# Size the INSERT so it is reliably still writing when the VACUUM arrives:
# grow n until one INSERT takes at least ~3 s.
my $n = 20000;
for (1 .. 6)
{
	my $t0 = [Time::HiRes::gettimeofday()];
	$node->safe_psql('postgres', "INSERT INTO big VALUES (100, $n)");
	my $dt = Time::HiRes::tv_interval($t0);
	note("calibration: n=$n took ${dt}s");
	last if $dt >= 3;
	$n = int($n * (3 / ($dt > 0.05 ? $dt : 0.05)) * 1.3);
	$n = 2_000_000 if $n > 2_000_000;
}
$node->safe_psql('postgres', 'DELETE FROM big WHERE id = 100; VACUUM big');

my $hitA = 0;
for my $try (1 .. 8)
{
	my $logpos = -s $node->logfile;
	my $w = $node->background_psql('postgres', on_error_stop => 0);
	my $wpid = $w->query_safe('SELECT pg_backend_pid()');
	$w->query_until(qr/started/, "\\echo started\nINSERT INTO big VALUES (1000 + $try, $n);\n");

	# Wait until the inserter HOLDS the segment-write lock AND has pages of its
	# unpublished bolt ON DISK: unreachable, unflagged, initialized pages, which
	# is exactly what a reclaim without the barrier would free.  Holding the
	# lock alone is not enough -- the writer sorts its terms under it before it
	# writes a page, and a VACUUM started then has every page the writer will
	# write AFTER its fence, so the fence alone protects them and this phase
	# could not tell a missing barrier from a present one (measured: the
	# barrier-removed mutant passed every corruption assertion of the first
	# version of this phase, caught only by the waiting assertion).  A bounded
	# loop rather than poll_query_until: that one would wait out its whole
	# timeout on a try whose window closed before the first poll.
	my $state = 'wait';
	my $inflight = 0;
	for (1 .. 3000)
	{
		$state = $node->safe_psql('postgres', qq{
			SELECT CASE
			  WHEN NOT EXISTS (SELECT FROM pg_locks WHERE pid = $wpid AND locktype = 'page'
			                     AND page = -1 AND mode = 'ShareLock' AND granted)
			    THEN CASE WHEN (SELECT state FROM pg_stat_activity WHERE pid = $wpid) <> 'active'
			              THEN 'done' ELSE 'wait' END
			  WHEN (SELECT count(*) FROM weave_page_info('big_w')
			         WHERE NOT reachable AND coalesce(freed, false) = false
			           AND NOT uninitialized) >= 8 THEN 'held'
			  ELSE 'wait' END});
		last if $state ne 'wait';
		usleep(2_000);
	}
	if ($state eq 'held')
	{
		$inflight = leaked('big_w');
		note("phase A try $try: the in-flight bolt has $inflight page(s) on disk");
	}
	if ($state ne 'held')
	{
		note("phase A try $try: the inserter's window closed before it was seen ($state)");
		$w->quit;
		next;
	}

	my $v = $node->background_psql('postgres', on_error_stop => 0);
	$v->query_until(qr/started/, "SET client_min_messages = debug2;\n\\echo started\nVACUUM big;\n");
	# the VACUUM must be seen WAITING on that lock while the inserter holds it
	my $waited = 0;
	for (1 .. 3000)
	{
		my $st = $node->safe_psql('postgres', qq{
			SELECT CASE
			  WHEN EXISTS (SELECT FROM pg_locks WHERE locktype = 'page' AND page = -1
			                 AND mode = 'ExclusiveLock' AND NOT granted) THEN 'waiting'
			  WHEN NOT EXISTS (SELECT FROM pg_locks WHERE pid = $wpid AND locktype = 'page'
			                     AND page = -1 AND granted) THEN 'released'
			  ELSE 'wait' END});
		if ($st eq 'waiting') { $waited = 1; last; }
		last if $st eq 'released';
		usleep(5_000);
	}
	$w->quit;
	$v->quit;
	# What each side said: the VACUUM's reclaim line (DEBUG2, to its client) and
	# any error the INSERT raised.  Without these a phase that is not hit cannot
	# say whether the reclaim ran before the writer's publish or after it.
	my ($vline) = ($v->{stderr} // '') =~ /(reclaimed \d+ stranded.*?ms)/;
	my ($werr) = ($w->{stderr} // '') =~ /(ERROR:.*)/;
	note("phase A try $try: VACUUM said: " . ($vline // '(no reclaim line)')
		  . '; INSERT ' . (defined $werr ? "raised $werr" : 'raised nothing'));
	my $log = substr(slurp_file($node->logfile), $logpos);
	my $sawlog = $log =~ /still waiting for ExclusiveLock on page 4294967295 of relation/;
	# every lock wait in this try, whatever it was on: a VACUUM that never
	# reaches its reclaim while the INSERT is in flight is waiting on SOMETHING
	note("phase A try $try: lock wait: $_") for ($log =~ /(process \d+ still waiting for [^\n]*)/g);
	note("phase A try $try: waited=$waited log=" . ($sawlog ? 1 : 0) . " inflight=$inflight");
	if ($waited && $sawlog && $inflight > 0)
	{
		$hitA = 1;
		last;
	}
}
ok($hitA, 'phase A: a VACUUM was observed waiting on the segment-write lock of an oversized INSERT whose unpublished pages were already on disk');
is(leaked('big_w'), '0', 'phase A: no page is leaked after the concurrent INSERT and VACUUM');
is(deep_bad('big_w'), '', 'phase A: weave_check(deep) is clean');
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM big WHERE bigdoc(n, id) @@@ 'big'}),
	$node->safe_psql('postgres', 'SELECT count(*) FROM big'),
	'phase A: every row, including the concurrent bolt, answers through the index');

# ---- phase B: the fence ------------------------------------------------------
# A pool of FREE pages for the second writer to reuse: several oversized bolts,
# merged into one, free their inputs.  Reuse waits out the freeing XID, which
# has committed by the time phase B starts.
# The reclaim's scan sleeps (vacuum_delay_point) only on UNREACHABLE blocks, so
# the pool is also what makes the scan slow enough for the writer to overtake it:
# the writer takes pool pages from the FSM faster than the scan walks them.
for my $k (1 .. 6)
{
	$node->safe_psql('postgres', "INSERT INTO big VALUES (2000 + $k, " . int($n / 3) . ')');
}
$node->safe_psql('postgres', q{SELECT weave_merge('big_w')});
# The freed pages are stamped with the next XID at free time and are reusable
# only once an XID at least that new has COMPLETED (weave_page_recyclable): two
# committed XIDs, so the writer below -- whose own XID is then newer -- can take
# them.
$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 2;
my $pool = $node->safe_psql('postgres', q{
	SELECT count(*) FROM weave_page_info('big_w') WHERE freed});
note("phase B pool of free pages: $pool");
cmp_ok($pool, '>', 100, 'phase B precondition: a pool of free pages exists for the writer to reuse');

my $delay = sprintf('%.3fms', 15000 / $pool < 0.05 ? 0.05 : (15000 / $pool > 20 ? 20 : 15000 / $pool));
note("phase B reclaim delay per unreachable block: $delay");
my $hitB = 0;
my $newerB = 0;
for my $try (1 .. 5)
{
	# a SLOW reclaim scan: vacuum_delay_point() per block
	my $logpos = -s $node->logfile;
	my $v = $node->background_psql('postgres', on_error_stop => 0);
	# about 15 s of scan over the pool, whatever its size (measured: a fixed 10 ms
	# against a 40,698-page pool would have scanned for ~400 s and timed out)
	$v->query_safe("SET vacuum_cost_delay = '$delay'; SET vacuum_cost_limit = 1");
	# the reclaim reports at DEBUG2 (PG17's lazy vacuum passes an index AM that
	# message level whatever VERBOSE says), so send this session's to the log
	$v->query_until(qr/started/, "SET log_min_messages = debug2;\n\\echo started\nVACUUM big;\n");
	# the VACUUM is past its barrier once it holds the maintenance mutex (page 0)
	$node->poll_query_until('postgres', q{
		SELECT count(*) > 0 FROM pg_locks
		 WHERE locktype = 'page' AND page = 0 AND mode = 'ExclusiveLock' AND granted});
	$node->safe_psql('postgres', "INSERT INTO big VALUES (3000 + $try, " . int($n / 3) . ')');
	$v->quit;			# waits for the VACUUM to finish
	# the reclaim's line went to the VACUUM session's stderr and to the log
	my $log = substr(slurp_file($node->logfile), $logpos);
	my @lines = $log =~ /(reclaimed \d+ stranded page\(s\).*newer than the fence, \d+ busy)/g;
	my ($newer) = ($lines[-1] // '') =~ /(\d+) unreachable page\(s\) newer than the fence/;
	note("phase B try $try: " . ($lines[-1] // 'no reclaim line'));
	if (defined $newer && $newer > 0)
	{
		$hitB = 1;
		$newerB = $newer;
		last;
	}
}
ok($hitB, "phase B: the reclaim met pages a writer wrote after its fence ($newerB) and left them");
is(leaked('big_w'), '0', 'phase B: no page is leaked');
is(deep_bad('big_w'), '', 'phase B: weave_check(deep) is clean');
is($node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM big WHERE bigdoc(n, id) @@@ 'big'}),
	$node->safe_psql('postgres', 'SELECT count(*) FROM big'),
	'phase B: every row answers through the index');
# and the pages the concurrent writer published survive the NEXT reclaim too
my ($freedB2) = vacuum_verbose('SET client_min_messages = debug2; VACUUM big');
is($freedB2, 0, 'phase B: a quiet VACUUM afterwards has nothing to reclaim');
is(deep_bad('big_w'), '', 'phase B: still clean after that VACUUM');

$node->stop;
done_testing();
