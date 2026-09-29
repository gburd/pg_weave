# 027_flush_insert_race.pl -- a pending flush must not lose rows a concurrent
# INSERT appends while it runs.
#
# doc/GAPS.md G61.  weave_flush_pending() walks the pending list, folds what it
# sees into a new segment, and then removes the list.  Nothing excludes a
# concurrent INSERT from that window: weave_insert() appends under the
# metapage buffer lock only, and neither autovacuum's cleanup (which holds the
# TABLE's ShareUpdateExclusiveLock) nor weave_merge()/weave_vacuum() (the
# INDEX's) conflicts with an inserter's RowExclusiveLock.  The removal used to
# drop the WHOLE list and free every page on it, so every item appended
# between the walk and the removal vanished from the index while its heap row
# stayed -- a silent wrong answer on ordinary INSERT + autovacuum, which
# weave_check() does not detect (it checks structure, not coverage).
#
# Measured before the fix (one 80,000-row INSERT racing weave_merge()):
# 74,298 / 75,207 / 76,294 of 81,000 rows indexed, weave_check clean.  Found by
# bench/aws/regress_loop.sh's autovacuum_naptime = 1s arm (G60), where
# sql/weave.sql's vconv block counted 61,260 of 64,000.
#
# The two shapes the fix has to keep, each proven to have FIRED by its own
# DEBUG1 line in the server log (a guard that never fired is not evidence):
#   single-row inserters keep appending to the page the flush last read (the
#     "cut page") -> "kept N bytes appended to the cut page";
#   a bulk inserter links whole new pages after it -> "kept pending pages
#     linked after the cut".
#
# Concurrency is REAL here, which it was not in t/005, t/006 and t/022 before
# 2026-09-29: every psql gets its SQL on the command line (IPC::Run writes a
# scalar stdin only while that harness is pumped), all handles are pumped
# together, and each session reports its wall-clock SPAN so the overlap is
# asserted rather than assumed.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);
use Time::HiRes qw(time);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->append_conf('postgresql.conf', "log_min_messages = debug1\n");
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_weave;
    CREATE TABLE race (id bigserial, d wdoc);
    INSERT INTO race(d) SELECT to_wdoc('shared seed w' || g) FROM generate_series(1, 1000) g;
    CREATE INDEX race_w ON race USING weave (d);
    CREATE TABLE race_ctl (stop boolean NOT NULL);
    INSERT INTO race_ctl VALUES (false);
});

my $conn = $node->connstr('postgres');

# Each session is one DO statement with its own COMMITs, run via -c so psql
# does not wrap it in a transaction block.
sub session {
    my ($sql) = @_;
    my ($in, $out, $err) = ('', '', '');
    my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-d', $conn, '-c', $sql],
                  '<', \$in, '>', \$out, '2>', \$err);
    return { h => $h, out => \$out, err => \$err };
}

# One row per statement, so the tail page grows item by item while a flush
# holds its cut on it.
my $single_sql = q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0;
BEGIN
  WHILE clock_timestamp() < t0 + interval '8 seconds' LOOP
    INSERT INTO race(d) VALUES (to_wdoc('shared single w' || b));
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=single start=% end=% iters=%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b;
END $$;
};

# Big batches: whole pages get linked after whatever page a flush stopped on.
my $bulk_sql = q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0;
BEGIN
  WHILE clock_timestamp() < t0 + interval '8 seconds' LOOP
    INSERT INTO race(d)
      SELECT to_wdoc('shared bulk r' || b || ' w' || g) FROM generate_series(1, 5000) g;
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=bulk start=% end=% iters=%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b;
END $$;
};

# The flusher: weave_merge() flushes the pending list and then merges, under
# the same maintenance lock autovacuum's cleanup takes.
my $flush_sql = q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0; s boolean;
BEGIN
  LOOP
    SELECT stop INTO s FROM race_ctl;
    EXIT WHEN s OR clock_timestamp() >= t0 + interval '60 seconds';
    PERFORM weave_merge('race_w');
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=flush start=% end=% iters=%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b;
END $$;
};

# A reader that compares, inside ONE statement (so one snapshot), the index's
# answer with the heap's.  Every row contains 'shared', so the two counts must
# be equal whatever the inserters are doing.  This is the only session here
# that can see a CONCURRENT miss: a scan that read the metapage between the
# flush's segment add and its clear (new generation, old head) and then met a
# freed page would drop the rows the clear kept -- which the per-round
# coverage check, run after everything settles, cannot see.  Found in review;
# the clear now bumps the generation itself.
#
# WHAT THIS READER DOES NOT PROVE: the mutant with that bump removed SURVIVED
# two runs (~270 checks per round, 7 rounds, 2026-09-29).  The window is the
# few microseconds between weave_add_segment_with_room() and the clear taking
# the metapage lock, and no hook exists to widen it.  The fix rests on the
# argument in include/weave/am.h (weave_page_is_live_pending), not on this
# test; what the reader does catch is a concurrent miss of any wider shape.
my $reader_sql = q{
DO $$
DECLARE t0 timestamptz := clock_timestamp(); b int := 0; bad int := 0; ix bigint; hp bigint;
BEGIN
  -- set here, not as separate statements: psql -c sends a multi-statement
  -- string as ONE implicit transaction, where the COMMITs below are illegal
  PERFORM set_config('enable_seqscan', 'off', false);
  PERFORM set_config('pg_weave.scan_race_retries', '1000', false);
  WHILE clock_timestamp() < t0 + interval '8 seconds' LOOP
    SELECT (SELECT count(*) FROM race WHERE d @@@ 'shared'::wquery),
           (SELECT count(*) FROM race) INTO ix, hp;
    IF ix <> hp THEN
      bad := bad + 1;
      RAISE WARNING 'READER_MISMATCH index=% heap=%', ix, hp;
    END IF;
    COMMIT;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=reader start=% end=% iters=% bad=%',
    extract(epoch FROM t0), extract(epoch FROM clock_timestamp()), b, bad;
END $$;
};

# One race round: a flusher, two single-row inserters, a bulk inserter and
# the reader, all pumped together.  Returns the two keep-shape firing counts from the server
# log written during the round.
sub race_round
{
    my ($round) = @_;

    # A fresh table each round after the first: weave_merge() compacts the
    # WHOLE index to one segment every call, so on a table grown by earlier
    # rounds one call can outlast the 8 s inserter window and the round is no
    # longer a race.  Coverage of the rounds that are discarded here is checked
    # at the end of each round, below.
    if ($round > 1)
    {
        $node->safe_psql('postgres', q{
            TRUNCATE race;
            INSERT INTO race(d) SELECT to_wdoc('shared seed w' || g) FROM generate_series(1, 1000) g;
            SELECT weave_merge('race_w');
        });
    }
    $node->safe_psql('postgres', 'UPDATE race_ctl SET stop = false');
    my $logstart = -s $node->logfile;

    my @s = (session($flush_sql), session($single_sql), session($single_sql),
             session($bulk_sql), session($reader_sql));
    my @inserters = @s[1 .. 4];	# the reader too: the flusher outlives it

    # Pump every handle; stop the flusher once the inserters are done.
    my $deadline = time() + 120;
    my $stopped = 0;
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
        if (!$stopped && !grep { $_->{h}->pumpable } @inserters)
        {
            $node->safe_psql('postgres', 'UPDATE race_ctl SET stop = true');
            $stopped = 1;
        }
        select(undef, undef, undef, 0.05);
    }
    my $hung = grep { $_->{h}->pumpable } @s;
    is($hung, 0, "round $round: all five sessions finished within 120 s");
    if ($hung) { $_->{h}->kill_kill for @s; }
    finish($_->{h}) for @s;

    my %span;
    for my $x (@s)
    {
        my $e = ${ $x->{err} };
        unlike($e, qr/\bERROR:/, "round $round: session hit no ERROR") or diag($e);
        while ($e =~ /SPAN kind=(\w+) start=([\d.]+) end=([\d.]+) iters=(\d+)(?: bad=(\d+))?/g)
        {
            push @{ $span{$1} }, [ $2, $3, $4, $5 ];
        }
    }

    # Evidence the race was actually run, not assumed.
    my $flush = $span{flush}[0];
    ok($flush && $flush->[2] > 1,
       "round $round: flusher ran more than one flush (" . ($flush ? $flush->[2] : 0) . ')');
    for my $kind (qw(single bulk reader))
    {
        for my $sp (@{ $span{$kind} || [] })
        {
            my $ov = ($sp->[1] < $flush->[1] ? $sp->[1] : $flush->[1])
                   - ($sp->[0] > $flush->[0] ? $sp->[0] : $flush->[0]);
            cmp_ok($ov, '>=', 5,
                   sprintf('round %d: %s inserter overlapped the flusher by %.1fs', $round, $kind, $ov));
            cmp_ok($sp->[2], '>', 0, "round $round: $kind inserter inserted ($sp->[2] statements)");
        }
    }
    is(scalar @{ $span{single} || [] }, 2, "round $round: both single-row inserters reported");
    is(scalar @{ $span{bulk} || [] }, 1, "round $round: the bulk inserter reported");
    my $rd = $span{reader}[0];
    is(scalar @{ $span{reader} || [] }, 1, "round $round: the reader reported");
    is($rd ? $rd->[3] : -1, 0,
       "round $round: the concurrent reader's index count always equalled the heap's ("
       . ($rd ? $rd->[2] : 0) . ' checks)');

    my $log = slurp_file($node->logfile, $logstart);
    my $kc = () = $log =~ /kept \d+ bytes appended to the cut page/g;
    my $kp = () = $log =~ /kept pending pages linked after the cut/g;
    note("round $round keep-shape firings: cut-page=$kc linked-pages=$kp");
    # Coverage of THIS round, so a later round's TRUNCATE cannot hide a loss.
    # The final round is left unflushed for the checks after the loop.
    my ($h, $c) = split /\|/, $node->safe_psql('postgres',
        q{SELECT count(*), weave_count('race_w', 'shared'::wquery) FROM race});
    is($c, $h, "round $round: index covers every heap row ($c of $h)");
    return ($kc, $kp);
}

# Which keep-shape a flush meets is timing-dependent (the linked-pages shape
# needs a bulk INSERT to link a page after the cut inside one flush's window),
# so rounds repeat, up to five, until BOTH have fired.  Every round is a full
# race with its own overlap assertions; coverage is asserted over all of them.
my ($kept_cut, $kept_pages) = (0, 0);
for my $round (1 .. 5)
{
    my ($kc, $kp) = race_round($round);
    $kept_cut += $kc;
    $kept_pages += $kp;
    last if $kept_cut >= 1 && $kept_pages >= 1;
}

# Both keep-shapes of the fix fired (DEBUG1 lines in weave_flush_pending()).
note("keep-shape firings, all rounds: cut-page=$kept_cut linked-pages=$kept_pages");
cmp_ok($kept_cut, '>=', 1, 'a flush kept items appended onto its cut page');
cmp_ok($kept_pages, '>=', 1, 'a flush kept pages linked after its cut');

# THE ASSERTION: every heap row is in the index, pending or folded.  Checked
# before and after a final flush, through the collector (weave_count) and
# through a forced plain index scan.
my $heap = $node->safe_psql('postgres', q{
    INSERT INTO race(d) SELECT to_wdoc('shared tail w' || g) FROM generate_series(1, 50000) g;
    SELECT count(*) FROM race});
my $pendpages = $node->safe_psql('postgres',
    q{SELECT count(*) FROM weave_page_info('race_w') WHERE kind LIKE 'pending%' AND NOT freed});
note("pending pages left before the final flush: $pendpages");

# The pending walk's memory must be linear in the list (doc/GAPS.md G64): it
# used to grow the match set with one tidset_or() per match, O(n^2) bytes, and
# a run of this test was OOM-killed at 17.6 GB inside the weave_count below.
# The 50,000 unflushed rows just inserted make that at least ~7 GB unfixed
# (measured: 7.3 GB peak RSS for 51,000), so the 1 GB bound discriminates.
# Measured in the SAME backend through /proc (Linux only).
my $viacount;
{
    my $bg = $node->background_psql('postgres');
    my $pid = $bg->query_safe('SELECT pg_backend_pid()');
    $viacount = $bg->query_safe(q{SELECT weave_count('race_w', 'shared'::wquery)});
    my $st = -r "/proc/$pid/status" ? slurp_file("/proc/$pid/status") : '';
    $bg->quit;
    SKIP:
    {
        skip 'no /proc/<pid>/status', 1 unless $st =~ /VmHWM:\s+(\d+) kB/;
        my $hwm = $1;
        cmp_ok($hwm, '<', 1024 * 1024,
               "collecting over $pendpages pending pages stayed under 1 GB (peak ${hwm} kB)");
    }
}
is($viacount, $heap, "index covers every heap row before the final flush ($viacount of $heap)");
$node->safe_psql('postgres', q{SELECT weave_merge('race_w')});
$viacount = $node->safe_psql('postgres', q{SELECT weave_count('race_w', 'shared'::wquery)});
is($viacount, $heap, "index covers every heap row after the final flush ($viacount of $heap)");
my $viascan = $node->safe_psql('postgres', q{
    SET enable_seqscan = off; SET enable_bitmapscan = off;
    SELECT count(*) FROM race WHERE d @@@ 'shared'::wquery});
is($viascan, $heap, 'a forced index scan returns every row');
my $viol = $node->safe_psql('postgres',
    q{SELECT count(*) FROM weave_check('race_w', true) WHERE NOT ok});
is($viol, 0, 'weave_check deep finds nothing wrong');

$node->stop;
done_testing();
