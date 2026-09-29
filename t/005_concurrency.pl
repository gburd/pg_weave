# 005_concurrency.pl -- scan vs. concurrent merge/insert page-recycle hazard.
#
# weave_merge()/weave_vacuum()/autovacuum hold ShareUpdateExclusiveLock, which does
# NOT conflict with a scan's AccessShareLock, so scans run concurrently with
# merges. A scan reads the metapage segment directory, releases the lock, then
# walks segment pages -- pinning each page only while reading it. A concurrent
# merge frees the old segment's pages (RecordFreeIndexPage -- no deletion-xid
# recycle gate, unlike nbtree/GIN), and a concurrent insert/flush's
# weave_new_buffer can recycle one of those blocks and overwrite it before the
# scan reads it -> the scan can miss or mis-count matches (bounded wrong result;
# decode hardening prevents a crash).
#
# This hammers that window: a fixed "anchor" set (never deleted) whose match
# count is a known CONSTANT, read repeatedly by concurrent readers, while a
# writer churns the index (INSERT + DELETE + weave_merge + weave_vacuum) to force
# continuous segment writes/frees/recycles.
#
# Reader and writer run as independent async psql processes via IPC::Run so they
# truly overlap. Each does its own loop in-session (fast: no per-op process
# spawn). At the end we inspect their output for any wrong anchor count or error.
#
# RETRACTED 2026-09-29 (hard rule 13): "so they truly overlap" was false until
# that date.  The harness ran the three sessions ONE AFTER ANOTHER (see the FIX
# note at psql_proc below), so every result this file reported before 2026-09-29
# was from SERIAL reads and writes and is not evidence of concurrency safety.
#
# A wrong count / error => the hazard is REAL (FAIL). All-correct does NOT prove
# safety (the window is narrow/timing-dependent) but is the expected result if
# pg_weave is concurrency-safe.

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
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, kind text, body text);
    INSERT INTO docs(kind, body)
      SELECT 'anchor', 'anchorterm w'||(g % 50)||' filler doc'||g
      FROM generate_series(1, 2000) g;
    INSERT INTO docs(kind, body)
      SELECT 'churn', 'churnterm w'||(g % 50)||' filler doc'||g
      FROM generate_series(1, 4000) g;
    CREATE INDEX docs_bm25 ON docs USING weave (to_wdoc('simple', body));
});

my $anchor_expected = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery});
is($anchor_expected, 2000, 'baseline: anchor term matches all 2000 anchor rows');

my $conn = $node->connstr('postgres');

# --- Writer: churn ~10s to force segment writes/frees/recycles --------------
my $writer_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp() + interval '10 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    DELETE FROM docs WHERE kind='churn';
    INSERT INTO docs(kind, body)
      SELECT 'churn','churnterm w'||(g%50)||' r'||b||' doc'||g FROM generate_series(1,4000) g;
    PERFORM weave_merge('docs_bm25');
    PERFORM weave_vacuum('docs_bm25');
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=writer start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), b;
END $$;
\echo WRITER_DONE
};

# --- Reader: count the anchor term as fast as possible for ~10s -------------
# Emits one line per read: '2000' when correct. \gset + \echo lets us print the
# value; we scan the output for any line that is a number other than 2000.
my $reader_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp()+interval '10 seconds'; c bigint; bad int := 0; tot int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    SELECT count(*) INTO c FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery;
    tot := tot + 1;
    IF c <> 2000 THEN bad := bad + 1; RAISE WARNING 'ANCHOR_MISS count=%', c; END IF;
  END LOOP;
  RAISE NOTICE 'SPAN kind=reader start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), tot;
  RAISE NOTICE 'READER_DONE reads=% wrong=%', tot, bad;
END $$;
};

# FIX 2026-09-29 (found while writing t/026): THIS FILE WAS NOT CONCURRENT.
# IPC::Run's start(..., '<', \$scalar) writes nothing to the child's stdin until
# that harness is pumped, and finish($h) pumps only $h.  The old
# `finish($r1h); finish($r2h); finish($wh);` therefore ran reader 1's 10 s loop
# alone, then reader 2's, then the writer's: the "concurrent churn" never
# overlapped a single read.  t/007_segment_cap.pl had exactly this bug and fixed
# it under doc/GAPS.md G28 (see its comment above the peak bound); the fix was
# never propagated here.  Every pass this file reported before 2026-09-29 was a
# SERIAL run (hard rule 13).  Now: psql exits via \q, pump_all pumps every handle
# with a deadline, and each session reports a wall-clock SPAN so the overlap is
# asserted rather than assumed.
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
my ($wh, $wout, $werr) = psql_proc($writer_sql);
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);
my @readers = ({ name => 'reader r1', h => $r1h, err => $r1err },
               { name => 'reader r2', h => $r2h, err => $r2err });
my $writer = { name => 'writer', h => $wh, err => $werr };

finish_or_report([ $writer, @readers ], 120);

my $all_err = "$$r1err\n$$r2err";
my $reads_line = join("\n", grep { /READER_DONE/ } split /\n/, $all_err);
diag("reader summary: $reads_line");

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

is($misses, 0, 'no concurrent anchor read returned a wrong count (page-recycle miss)');
is($reader_errored, 0, 'no reader hit an ERROR during concurrent merge/insert churn');

my $final = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery});
is($final, 2000, 'anchor count still exact after the churn settles');

$node->stop;
done_testing();
