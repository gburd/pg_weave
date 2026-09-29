# 022_docvals_null_concurrent.pl -- docvals NULL gate vs. concurrent churn.
#
# docvals-nulls slice, Task 6, the concurrency gate (PRODUCTION_READINESS gate 9
# specialised to the v2 null-bearing store).  t/005_concurrency.pl is the model:
# a fixed "anchor" set whose gated count is a known CONSTANT is read repeatedly
# by independent async readers, while a writer churns the index
# (INSERT + DELETE + weave_merge + weave_vacuum) to force continuous segment
# writes/frees/recycles concurrent with the reads.
#
# WHY docvals-specific, and why NULLs.  weave_merge()/weave_vacuum() hold a lock
# that does NOT conflict with a scan's AccessShareLock, so a docvals gate scan --
# which reads the metapage directory, releases the lock, then reads each bolt's
# docvalues store page-at-a-time -- runs concurrently with a merge that FREES the
# old bolt's store pages and an insert/flush that can RECYCLE one of those blocks
# and overwrite it before the scan reads it.  The anchor gate `price < 100` has a
# known constant count over the anchors; if a recycled/rewritten store page ever
# makes the scan miss, double-count, or misread a value, the count moves.  The
# ANCHOR SET INCLUDES NULL-price rows and the CHURN SET is all NULL-or->=1000
# price, so `price < 100` must return EXACTLY the non-null anchors at every
# instant: a NULL that leaked through the gate under concurrency (a torn v2
# bitmap read mid-recycle) shows up as a count above the constant.
#
# Reader and writer run as independent async psql processes via IPC::Run so they
# truly overlap (each loops in-session for ~10s; the writer's and readers' loops
# are alive at the same wall-clock time -- genuine overlap, not the serial
# "concurrency" of doc/GAPS.md G28 / hard rule 11).
#
# RETRACTED 2026-09-29 (hard rule 13): the paragraph above was false until that
# date.  The harness ran the three sessions ONE AFTER ANOTHER -- the very G28
# serial "concurrency" it disclaims (see the FIX note at psql_proc below) -- so
# every result this file reported before 2026-09-29 was from SERIAL runs.
#
# A wrong anchor count / error => the hazard is REAL (FAIL).  All-correct does
# NOT prove safety (the window is narrow and timing-dependent) but is the
# expected result if the null-bearing docvals gate is concurrency-safe.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
# small shared_buffers => freed pages recycle sooner (tighter race window)
$node->append_conf('postgresql.conf', "shared_buffers = 16MB\n");
$node->append_conf('postgresql.conf', "maintenance_work_mem = 1MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# ANCHORS (never deleted): 2000 rows whose price is a small value < 100, except
# every 9th which is NULL.  CHURN (deleted/reinserted repeatedly): price always
# NULL or >= 1000.  So `price < 100` selects EXACTLY the non-null anchors, for
# all time and regardless of the churn -- a NULL anchor, a NULL churn row and a
# non-null churn row are all excluded (the first two by the null bitmap, the
# third by the comparison).  Column shapes copied from sql/docvals.sql section 9.
$node->safe_psql('postgres', q{
	CREATE TABLE docs (id bigserial PRIMARY KEY, kind text, body wdoc, price bigint);
	INSERT INTO docs(kind, body, price)
	  SELECT 'anchor', to_wdoc('anchor common ' || (g % 50)),
	         CASE WHEN g % 9 = 0 THEN NULL ELSE (g % 50)::bigint END
	  FROM generate_series(1, 2000) g;
	INSERT INTO docs(kind, body, price)
	  SELECT 'churn', to_wdoc('churn common ' || (g % 50)),
	         CASE WHEN g % 3 = 0 THEN NULL ELSE (1000 + g % 500)::bigint END
	  FROM generate_series(1, 4000) g;
	CREATE INDEX docs_dv ON docs USING weave (body, price int8_docval_ops);
	ANALYZE docs;
});

# The known constant, measured through the index (not hard-coded): the count of
# non-null anchor rows.  Also assert it equals IS NOT NULL over the anchors AND
# is strictly below the anchor total, so we know NULLs are genuinely excluded
# (not merely that some constant is stable).
my $anchor_expected = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  SELECT count(*) FROM docs WHERE price < 100});
my $anchor_notnull = $node->safe_psql('postgres',
	q{SELECT count(*) FROM docs WHERE kind='anchor' AND price IS NOT NULL});
my $anchor_total = $node->safe_psql('postgres',
	q{SELECT count(*) FROM docs WHERE kind='anchor'});
# Positive control: the reader's gate is an Index Scan on the weave index, not a
# silent seqscan (enable_seqscan=off only penalises cost).  If it fell back, the
# concurrency window under test would not be exercised and the run would pass
# vacuously (AGENTS.md hard rule 11 / G28 territory).
my $anchor_plan = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE price < 100});
like($anchor_plan, qr/Index Scan using docs_dv/,
	'reader gate is an Index Scan on the weave index, not a seqscan fallback');
is($anchor_expected, $anchor_notnull,
	"baseline: `price < 100` gate == non-null anchors ($anchor_expected)");
cmp_ok($anchor_expected, '<', $anchor_total,
	"baseline: some anchors are NULL and excluded ($anchor_expected < $anchor_total)");

my $conn = $node->connstr('postgres');

# --- Writer: churn ~10s to force segment writes/frees/recycles --------------
# INSERT (mix of NULL and non-NULL price, all >= 1000 so never < 100) + DELETE
# the churn rows + weave_merge (flush pending, merge bolts) + weave_vacuum
# (tombstone-drop rewrite, free + recycle pages).
my $writer_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp() + interval '10 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    DELETE FROM docs WHERE kind='churn';
    INSERT INTO docs(kind, body, price)
      SELECT 'churn', to_wdoc('churn common ' || (g % 50)),
             CASE WHEN g % 3 = 0 THEN NULL ELSE (1000 + b + g % 500)::bigint END
      FROM generate_series(1, 4000) g;
    PERFORM weave_merge('docs_dv');
    PERFORM weave_vacuum('docs_dv');
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=writer start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), b;
END $$;
\echo WRITER_DONE
};

# --- Reader: count the anchor gate as fast as possible for ~10s -------------
# The constant is interpolated from $anchor_expected.  Any count other than the
# constant is an ANCHOR_MISS (a recycled/torn store page, or a NULL that leaked
# through the v2 bitmap gate).  enable_seqscan/bitmapscan off so the read goes
# through the docvalues store, not the heap.
my $reader_sql = qq{
SET enable_seqscan=off;
SET enable_bitmapscan=off;
DO \$\$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp()+interval '10 seconds'; c bigint; bad int := 0; tot int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    SELECT count(*) INTO c FROM docs WHERE price < 100;
    tot := tot + 1;
    IF c <> $anchor_expected THEN bad := bad + 1; RAISE WARNING 'ANCHOR_MISS count=%', c; END IF;
  END LOOP;
  RAISE NOTICE 'SPAN kind=reader start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), tot;
  RAISE NOTICE 'READER_DONE reads=% wrong=%', tot, bad;
END \$\$;
};

# FIX 2026-09-29 (found while writing t/026): THIS FILE WAS NOT CONCURRENT.
# IPC::Run's start(..., '<', \$scalar) writes nothing to the child's stdin until
# that harness is pumped, and finish($h) pumps only $h.  The old
# `finish($r1h); finish($r2h); finish($wh);` therefore ran reader 1's 10 s loop
# alone, then reader 2's, then the writer's: no read ever overlapped the churn.
# t/007_segment_cap.pl had exactly this bug and fixed it under doc/GAPS.md G28
# (see its comment above the peak bound); the fix was never propagated here or
# to t/005, which this file was modelled on.  Every pass this file reported
# before 2026-09-29 was a SERIAL run (hard rule 13).  Now: psql exits via \q,
# pump_all pumps every handle with a deadline, and each session reports a
# wall-clock SPAN so the overlap is asserted rather than assumed.
sub psql_proc {
	my ($sql) = @_;
	# \q so psql exits and pumpable() goes false (t/007).
	$sql .= "\n\\q\n" unless $sql =~ /\\q\s*$/;
	my ($in, $out, $err) = ($sql, '', '');
	my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=0', '-d', $conn],
				  '<', \$in, '>', \$out, '2>', \$err);
	return ($h, \$out, \$err);
}

# Pump EVERY handle each iteration, bounded by a deadline.  Returns 1 if hung.
sub pump_all {
	my ($handles, $secs) = @_;
	my $deadline = time() + $secs;

	while (time() < $deadline) {
		my $live = 0;
		for my $p (@$handles) {
			next unless $p->{h}->pumpable;
			$live++;
			$p->{h}->pump_nb;
		}
		return 0 if $live == 0;
		select(undef, undef, undef, 0.1);
	}
	return 1;
}

sub tail_of {
	my ($s, $n) = @_;
	my @l = split /\n/, $s;
	@l = @l[-$n .. -1] if @l > $n;
	return join("\n", @l);
}

# A hang is a failure with a diagnosis, not a CI timeout.
sub finish_or_report {
	my ($handles, $secs) = @_;
	my $hung = pump_all($handles, $secs);

	if ($hung) {
		diag("HUNG: sessions did not finish in ${secs}s");
		diag("$_->{name} stderr tail:\n" . tail_of(${ $_->{err} }, 8)) for @$handles;
		diag($node->safe_psql('postgres',
			q{SELECT pid, wait_event_type, wait_event, left(query, 40) FROM pg_stat_activity
			   WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()}));
		$_->{h}->kill_kill for @$handles;
	} else {
		finish($_->{h}) for @$handles;
	}
	ok(!$hung, scalar(@$handles) . " concurrent psql sessions all finished within ${secs}s");
}

# Parse a session's own "SPAN kind=... start=<epoch> end=<epoch> iters=<n>".
sub span_of {
	my ($err) = @_;
	return ($err =~ /SPAN kind=\S+ start=([\d.]+) end=([\d.]+) iters=(\d+)/)
		? { start => $1, end => $2, iters => $3 } : undef;
}

sub overlap_secs {
	my ($x, $y) = @_;
	my $lo = $x->{start} > $y->{start} ? $x->{start} : $y->{start};
	my $hi = $x->{end} < $y->{end} ? $x->{end} : $y->{end};
	return $hi - $lo;
}

# start writer + two readers, and pump them all at once
my ($wh, $wout, $werr)    = psql_proc($writer_sql);
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);
my @readers = ({ name => 'reader r1', h => $r1h, err => $r1err },
			   { name => 'reader r2', h => $r2h, err => $r2err });
my $writer = { name => 'writer', h => $wh, err => $werr };

finish_or_report([ $writer, @readers ], 120);

my $all_err = "$$r1err\n$$r2err";
my $reads_line = join("\n", grep { /READER_DONE/ } split /\n/, $all_err);
diag("reader summary: $reads_line");
# Evidence the readers actually ran their loop (kept; NOT proof of overlap --
# the SPAN assertions below are).
my ($total_reads) = ($all_err =~ /READER_DONE reads=(\d+)/);
cmp_ok($total_reads // 0, '>', 0,
	'a reader completed at least one gated read concurrently with the writer');

# POSITIVE CONTROL: the writer did work, and each reader's loop overlapped it.
# Serial execution (the pre-2026-09-29 harness) gives ~0 s overlap.
my $ws = span_of($$werr);
diag("writer: no SPAN notice (aborted?)") unless $ws;
cmp_ok($ws ? $ws->{iters} : 0, '>', 0,
	'writer completed at least one churn iteration (b > 0)');
for my $r (@readers) {
	my $rs = span_of(${ $r->{err} });
	diag("$r->{name}: no SPAN notice (aborted before its end?)") unless $rs;
	my $ov = ($rs && $ws) ? overlap_secs($rs, $ws) : -1;
	cmp_ok($ov, '>=', 5,
		sprintf('%s and writer ran concurrently (overlap %.1fs)', $r->{name}, $ov));
}

my $misses = () = ($all_err =~ /ANCHOR_MISS/g);
my $reader_errored = ($all_err =~ /\bERROR:/) ? 1 : 0;
if ($reader_errored) {
	my %seen;
	my @e = grep { /\bERROR:/ && !$seen{$_}++ } split /\n/, $all_err;
	diag("distinct reader ERROR lines (first 5):\n"
		. join("\n", @e[0 .. ($#e < 4 ? $#e : 4)]));
}

is($misses, 0,
	'no concurrent anchor read returned a wrong count (page-recycle miss or leaked NULL)');
is($reader_errored, 0, 'no reader hit an ERROR during concurrent merge/insert churn');

my $final = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  SELECT count(*) FROM docs WHERE price < 100});
is($final, $anchor_expected, 'anchor gate still exact after the churn settles');

$node->stop;
done_testing();
