# 025_docvals_text_concurrent.pl -- text docvalues vs. concurrent INSERT + flush.
#
# docvals-text slice (doc/plans/2026-09-28-docvals-text-slice.md), the
# concurrency gate.  A text_docval_ops column stores DICTIONARY ORDINALS per
# bolt: the build writes one dictionary, every pending flush (VACUUM) writes a
# new bolt with its own dictionary, and a post-build INSERT carries the raw
# bytes in its pending item until then.  So a text gate scan compares the key
# against a different ordinal space in every bolt plus raw bytes in the pending
# list, while INSERTs append to that list and a VACUUM folds it into a new bolt
# and moves the directory generation.  sql/docvals.sql section 15 checks the
# same shapes serially; this file checks them while they move.
#
# The anchor predicate `cat < 'k'` is FIXED.  The workload is INSERT-only (no
# DELETE, no UPDATE), and each inserter batch is its own committed
# transaction, so the committed count of `cat < 'k'` can only grow.  The
# inserted values straddle the anchor on purpose: values below 'k', values at
# and above it, '' (which is < 'k') and NULL (which never matches).  Three
# things are asserted about the concurrent reads:
#
#   monotone   each READ COMMITTED read is its own statement after a COMMIT,
#              so each sees a later snapshot, and a later snapshot sees a
#              superset of committed inserts: the index count must never go
#              DOWN.  A bolt whose ordinal bound was computed against another
#              bolt's dictionary, or rows lost between a flush's walk and its
#              clear (G61's shape), shows up as a decrease;
#   exact      in the same statement (one snapshot) the index count is compared
#              with a heap count through a TID Range Scan, so a read that is
#              monotone but wrong is caught too;
#   ran        the reader did at least MIN_READS reads and saw at least two
#              distinct counts, the flusher's VACUUMs moved the segment count,
#              and every session's wall-clock SPAN overlapped the inserters'.
#              A reader that never ran, or ran after the writers finished
#              (doc/GAPS.md G28 / G63: "concurrent" sessions that were serial),
#              is a FAIL, not a pass.
#
# Harness, as t/027: every psql gets its SQL on the command line (-c) or from
# a file (-f), never from a scalar stdin (IPC::Run writes that only while the
# harness is pumped), every handle is started before any is waited on, and all
# are pumped together.  The reader and inserters are single DO blocks run via
# -c with their own COMMITs, so GUCs are set with set_config() (psql -c sends a
# multi-statement string as ONE implicit transaction, where COMMIT is illegal).
# VACUUM cannot run in a DO block, so the flusher is a psql script with a
# \gset/\if deadline loop.
#
# All-correct does NOT prove safety (the windows are narrow and
# timing-dependent); a decrease, a mismatch or an error proves a hazard.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);
use Time::HiRes qw(time);

use constant MIN_READS => 20;
use constant NINSERTERS => 4;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

# Initial rows, column shapes from sql/docvals.sql section 15: a build
# dictionary of 26 letters x 40 suffixes, some NULL, some ''.  Every body
# contains 'common' so the lexical channel can be used as a coverage check.
$node->safe_psql('postgres', q{
	CREATE EXTENSION pg_weave;
	CREATE TABLE t (id bigserial, body wdoc, cat text COLLATE "C")
	    WITH (autovacuum_enabled = off);
	INSERT INTO t(body, cat)
	  SELECT to_wdoc('common seed ' || (g % 50)),
	         CASE WHEN g % 17 = 0 THEN NULL
	              WHEN g % 19 = 0 THEN ''
	              ELSE chr(97 + g % 26) || lpad((g % 40)::text, 2, '0') END
	  FROM generate_series(1, 3000) g;
	CREATE INDEX w ON t USING weave (body wdoc_lex_ops, cat text_docval_ops);
	ANALYZE t;
});

is($node->safe_psql('postgres', q{SELECT weave_index_nsegments('w')}), '1',
	'baseline: the build wrote one bolt');

# Positive control: the reader's anchor read is a plain Index Scan on the weave
# index (enable_seqscan=off only penalises cost; a silent fallback would make
# the run vacuous).
my $plan = $node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE cat < 'k'});
like($plan, qr/Index Scan using w on t/,
	'reader anchor read is an Index Scan on the weave index');

my $baseline = $node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM t WHERE cat < 'k'});
my $baseline_heap = $node->safe_psql('postgres', q{
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM t WHERE cat < 'k'});
is($baseline, $baseline_heap, "baseline: anchor count index == heap ($baseline)");
cmp_ok($baseline, '>', 0, 'baseline: the anchor matches some build rows');

my $conn = $node->connstr('postgres');

sub session
{
	my ($name, @args) = @_;
	my ($in, $out, $err) = ('', '', '');
	my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-d', $conn, @args],
				  '<', \$in, '>', \$out, '2>', \$err);
	return { name => $name, h => $h, out => \$out, err => \$err };
}

# --- Inserters: small committed batches for ~8 s ------------------------------
# Values vary with the inserter, the batch and the row, so each flushed bolt
# gets a dictionary that differs from the build's and from the other bolts'.
# About 10/26 of the lettered values are < 'k'; '' is < 'k'; NULL never
# matches; some values equal build values ('c07'), some are new ('k' || ...).
sub inserter_sql
{
	my ($i) = @_;
	return sprintf(q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0;
BEGIN
  WHILE clock_timestamp() < t0 + interval '8 seconds' LOOP
    INSERT INTO t(body, cat)
      SELECT to_wdoc('common ins%d w' || (g %% 50)),
             CASE WHEN g %% 11 = 0 THEN NULL
                  WHEN g %% 13 = 0 THEN ''
                  WHEN g %% 7 = 0 THEN 'k'
                  ELSE chr(97 + (g * 7 + b + %d) %% 26) || lpad(((b + g) %% 60)::text, 2, '0')
             END
      FROM generate_series(1, 20) g;
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=inserter start=%% end=%% iters=%%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b;
END $$;
}, $i, $i);
}

# --- Reader: the anchor count, one statement per read, for ~8 s ---------------
# ix: the index answer (plain Index Scan, the GUCs set below).  hp: the heap
# answer in the SAME statement (same snapshot) through a TID Range Scan; the
# FILTER keeps cat out of the qual so the weave index cannot answer it, and the
# ctid qual avoids PG18's keyless-weave-plan ERROR ("a weave index scan
# requires a query") with enable_seqscan off.
my $reader_sql = q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0; dec int := 0; bad int := 0;
        incs int := 0; ix bigint; hp bigint; prev bigint := -1; first bigint := -1;
BEGIN
  PERFORM set_config('enable_seqscan', 'off', false);
  PERFORM set_config('enable_bitmapscan', 'off', false);
  PERFORM set_config('pg_weave.scan_race_retries', '1000', false);
  WHILE clock_timestamp() < t0 + interval '8 seconds' LOOP
    SELECT (SELECT count(*) FROM t WHERE cat < 'k'),
           (SELECT count(*) FILTER (WHERE cat < 'k') FROM t WHERE ctid >= '(0,0)'::tid)
      INTO ix, hp;
    IF ix <> hp THEN
      bad := bad + 1;
      RAISE WARNING 'READER_MISMATCH index=% heap=%', ix, hp;
    END IF;
    IF prev >= 0 AND ix < prev THEN
      dec := dec + 1;
      RAISE WARNING 'READER_DECREASE prev=% now=%', prev, ix;
    END IF;
    IF prev >= 0 AND ix > prev THEN incs := incs + 1; END IF;
    IF first < 0 THEN first := ix; END IF;
    prev := ix;
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=reader start=% end=% iters=% dec=% bad=% incs=% first=% last=%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b, dec, bad, incs, first, prev;
END $$;
};

# --- Flusher: VACUUM (the pending flush) in a deadline loop for ~8 s ---------
# psql has no loop, so the script is a fixed number of \if-guarded blocks, each
# of which does nothing once the deadline has passed.  It tracks the segment
# count after each VACUUM: nchg (how often it changed) and maxseg are the
# evidence that flushes really landed during the inserts.
my $flush_file = $node->basedir . '/flusher.sql';
{
	my $s = q{SELECT clock_timestamp() + interval '8 seconds' AS deadline,
       extract(epoch FROM clock_timestamp()) AS t0, 0 AS n, 0 AS nchg,
       weave_index_nsegments('w') AS prev, weave_index_nsegments('w') AS maxseg \gset
};
	my $blk = q{SELECT clock_timestamp() < :'deadline'::timestamptz AS go \gset
\if :go
VACUUM t;
SELECT weave_index_nsegments('w') AS cur \gset
SELECT :n + 1 AS n, greatest(:maxseg, :cur) AS maxseg,
       :nchg + (:cur <> :prev)::int AS nchg, :cur AS prev \gset
SELECT pg_sleep(0.1) AS slept \gset
\endif
};
	$s .= $blk x 400;
	$s .= q{SELECT extract(epoch FROM clock_timestamp()) AS t1 \gset
\echo SPAN kind=flush start=:t0 end=:t1 iters=:n nchg=:nchg maxseg=:maxseg
};
	append_to_file($flush_file, $s);
}

# Start EVERY session before waiting on any, then pump them all together.
my @s;
push @s, session("inserter $_", '-c', inserter_sql($_)) for 1 .. NINSERTERS;
push @s, session('reader', '-c', $reader_sql);
push @s, session('flusher', '-f', $flush_file);

my $deadline = time() + 180;
while (time() < $deadline)
{
	my $live = 0;
	for my $x (@s)
	{
		next unless $x->{h}->pumpable;
		$live++;
		$x->{h}->pump_nb;
	}
	last if $live == 0;
	select(undef, undef, undef, 0.05);
}
my $hung = grep { $_->{h}->pumpable } @s;
is($hung, 0, scalar(@s) . ' concurrent sessions all finished within 180 s');
if ($hung)
{
	diag($node->safe_psql('postgres',
		q{SELECT pid, wait_event_type, wait_event, left(query, 40) FROM pg_stat_activity
		   WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()}));
	$_->{h}->kill_kill for @s;
}
finish($_->{h}) for @s;

for my $x (@s)
{
	unlike(${ $x->{err} }, qr/\bERROR:/, "$x->{name}: no ERROR")
	  or diag(${ $x->{err} });
}

# Parse each session's SPAN (NOTICEs on stderr; the flusher's \echo on stdout).
my (@ins, $rd, $fl);
for my $x (@s)
{
	my $txt = ${ $x->{err} } . "\n" . ${ $x->{out} };
	if ($txt =~ /SPAN kind=inserter start=([\d.]+) end=([\d.]+) iters=(\d+)/)
	{
		push @ins, { start => $1, end => $2, iters => $3 };
	}
	if ($txt =~ /SPAN kind=reader start=([\d.]+) end=([\d.]+) iters=(\d+) dec=(\d+) bad=(\d+) incs=(\d+) first=(-?\d+) last=(-?\d+)/)
	{
		$rd = { start => $1, end => $2, iters => $3, dec => $4, bad => $5,
				incs => $6, first => $7, last => $8 };
	}
	if ($txt =~ /SPAN kind=flush start=([\d.]+) end=([\d.]+) iters=(\d+) nchg=(\d+) maxseg=(\d+)/)
	{
		$fl = { start => $1, end => $2, iters => $3, nchg => $4, maxseg => $5 };
	}
}

is(scalar @ins, NINSERTERS, 'every inserter reported its SPAN');
ok(defined $rd, 'the reader reported its SPAN');
ok(defined $fl, 'the flusher reported its SPAN');
$rd //= { start => 0, end => 0, iters => 0, dec => -1, bad => -1, incs => 0, first => -1, last => -1 };
$fl //= { start => 0, end => 0, iters => 0, nchg => 0, maxseg => 0 };

sub overlap
{
	my ($x, $y) = @_;
	my $lo = $x->{start} > $y->{start} ? $x->{start} : $y->{start};
	my $hi = $x->{end} < $y->{end} ? $x->{end} : $y->{end};
	return $hi - $lo;
}

# POSITIVE CONTROLS: the race was run, not assumed.
my $i = 0;
for my $sp (@ins)
{
	$i++;
	cmp_ok($sp->{iters}, '>', 1, "inserter span $i committed more than one batch ($sp->{iters})");
	cmp_ok(overlap($sp, $rd), '>=', 5,
		sprintf('inserter span %d overlapped the reader by %.1fs', $i, overlap($sp, $rd)));
	cmp_ok(overlap($sp, $fl), '>=', 5,
		sprintf('inserter span %d overlapped the flusher by %.1fs', $i, overlap($sp, $fl)));
}
cmp_ok($rd->{iters}, '>=', MIN_READS, "the reader did at least " . MIN_READS . " reads ($rd->{iters})");
cmp_ok($rd->{incs}, '>=', 1,
	"the reader saw the anchor count grow at least once ($rd->{incs} increases, $rd->{first} -> $rd->{last})");
cmp_ok($fl->{iters}, '>', 1, "the flusher ran more than one VACUUM ($fl->{iters})");
cmp_ok($fl->{nchg}, '>=', 1,
	"a concurrent VACUUM changed the segment count ($fl->{nchg} changes, max $fl->{maxseg})");
cmp_ok($fl->{maxseg}, '>', 1, "more than one bolt existed during the run (max $fl->{maxseg})");

# THE ASSERTIONS about the concurrent reads.
is($rd->{dec}, 0, "the anchor count never decreased across $rd->{iters} concurrent reads");
is($rd->{bad}, 0, "every concurrent index read equalled the same-snapshot heap count");
cmp_ok($rd->{first}, '>=', $baseline, "the reader's first count is not below the baseline ($rd->{first} >= $baseline)");

# --- After everything settles: index == heap for every operator --------------
$node->safe_psql('postgres', q{
CREATE FUNCTION dv_counts(pred text, OUT ni bigint, OUT nb bigint, OUT ns bigint)
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_indexscan', 'on', true);
    EXECUTE 'SELECT count(*) FROM t WHERE ' || pred INTO ni;
    PERFORM set_config('enable_indexscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    EXECUTE 'SELECT count(*) FROM t WHERE ' || pred INTO nb;
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_seqscan', 'on', true);
    EXECUTE 'SELECT count(*) FROM t WHERE ' || pred INTO ns;
END $$;
});

my $cases_sql = q{
SELECT c.pred, d.ni, d.nb, d.ns
  FROM (SELECT format('cat %s %L', o.op, k.k) AS pred, o.n, k.m
          FROM (VALUES ('<', 1), ('<=', 2), ('=', 3), ('>=', 4), ('>', 5)) o(op, n),
               (VALUES ('', 1), ('a', 2), ('c07', 3), ('k', 4), ('k05', 5),
                       ('zz', 6), ('~', 7)) k(k, m)) c,
       LATERAL dv_counts(c.pred) d
 ORDER BY c.n, c.m};

sub check_all
{
	my ($phase) = @_;
	my $out = $node->safe_psql('postgres', $cases_sql);
	my $n = 0;
	for my $line (split /\n/, $out)
	{
		my ($pred, $ni, $nb, $ns) = split /\|/, $line;
		$n++;
		is($ni, $ns, "$phase: $pred index scan == seqscan ($ni vs $ns)");
		is($nb, $ns, "$phase: $pred bitmap scan == seqscan ($nb vs $ns)");
	}
	is($n, 35, "$phase: all 35 operator/constant cases were checked");

	my ($ai, $ab, $as) = split /\|/, $node->safe_psql('postgres',
		q{SELECT ni, nb, ns FROM dv_counts($$cat < 'k'$$)});
	is($ai, $as, "$phase: anchor `cat < 'k'` index == seqscan ($ai)");
	cmp_ok($ai, '>=', $rd->{last}, "$phase: anchor count not below the reader's last ($ai >= $rd->{last})");

	my ($heap, $cov) = split /\|/, $node->safe_psql('postgres',
		q{SELECT count(*), weave_count('w', 'common'::wquery) FROM t});
	is($cov, $heap, "$phase: the lexical channel covers every heap row ($cov of $heap)");

	my $viol = $node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('w', true) WHERE NOT ok});
	is($viol, '0', "$phase: weave_check deep finds nothing wrong");
}

# Pending rows may remain (inserts after the flusher's last VACUUM): check with
# them pending, then after a final flush.
check_all('after concurrent run');
$node->safe_psql('postgres', 'VACUUM t');
check_all('after final VACUUM');

my $nseg = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('w')});
# Not asserted: the tiered auto-merge in VACUUM may legitimately collapse the
# bolts again by the end (observed: 1).  The in-run maxseg > 1 check above is
# the positive control that several bolts coexisted while readers ran.
note("final segment count: $nseg");

$node->stop;
done_testing();
