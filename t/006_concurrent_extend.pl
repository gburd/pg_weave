# 006_concurrent_extend.pl -- read + insert + merge the SAME index at once.
#
# Regression for the field-reported error:
#
#   ERROR: unexpected data beyond EOF in block N of relation "base/.../<node>"
#
# It happens when two unrelated, non-parallel backends extend the same index
# concurrently -- e.g. weave_merge() writing merged output while live ingestion
# (INSERT flushing a pending buffer into a new segment) extends the relation --
# and one backend reads/extends a block past its cached EOF.  pg_weave must take
# the relation extension lock around every P_NEW extension, not only during a
# parallel build, so concurrent extenders coordinate.
#
# 005 drives INSERT and weave_merge from ONE session (sequential), so the two
# never extend at the same instant and it cannot catch this.  Here a DEDICATED
# merge backend loops weave_merge() while SEVERAL other backends loop INSERTs into
# the same index, plus readers -- so extenders genuinely overlap.  Any writer
# ERROR (especially "beyond EOF") fails the test.
#
# RETRACTED 2026-09-29 (hard rule 13): "so extenders genuinely overlap" was
# false until that date.  The harness ran the six sessions ONE AFTER ANOTHER
# (see the FIX note at psql_proc below), so no two extenders ever overlapped
# and every result this file reported before 2026-09-29 was from SERIAL runs --
# exactly the "005 cannot catch this" shape the paragraph above rejects.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
# Small buffers + tiny mwm => flushes/segments happen constantly, so INSERTs
# and merges extend the relation often -> tight extension-race window.
$node->append_conf('postgresql.conf', "shared_buffers = 16MB\n");
$node->append_conf('postgresql.conf', "maintenance_work_mem = 1MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, kind text, body text);
    INSERT INTO docs(kind, body)
      SELECT 'anchor', 'anchorterm w'||(g % 50)||' filler doc'||g
      FROM generate_series(1, 2000) g;
    CREATE INDEX docs_bm25 ON docs USING weave (to_wdoc('simple', body));
});

my $conn = $node->connstr('postgres');

# --- Merge backend: loop weave_merge()/weave_vacuum() for ~12s ------------------
my $merge_sql = q{
DO $$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp() + interval '12 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    PERFORM weave_merge('docs_bm25');
    PERFORM weave_vacuum('docs_bm25');
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=merge start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), b;
END $$;
};

# --- Inserter backend: churn new documents (new segments) for ~12s ----------
# Distinct session id `s` keeps every batch's bodies unique so segments keep
# growing/flushing (constant relation extension) alongside the merge.
sub inserter_sql {
    my ($s) = @_;
    return qq{
DO \$\$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp() + interval '12 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    INSERT INTO docs(kind, body)
      SELECT 'churn', 'churnterm s$s r'||b||' w'||(g%50)||' doc'||g
      FROM generate_series(1, 1500) g;
    DELETE FROM docs WHERE kind='churn' AND body LIKE 'churnterm s$s %' AND (id % 3) = 0;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'SPAN kind=inserter$s start=% end=% iters=%',
    extract(epoch from t_start), extract(epoch from clock_timestamp()), b;
  RAISE NOTICE 'INSERTER_DONE s=$s batches=%', b;
END \$\$;
};
}

# --- Reader: count the anchor term repeatedly (must stay 2000, no error) -----
my $reader_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE t_start timestamptz := clock_timestamp(); deadline timestamptz := clock_timestamp()+interval '12 seconds'; c bigint; bad int := 0; tot int := 0;
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
# `finish($_) for (...)` therefore ran each 12 s loop alone, one session after
# another: no insert ever extended the relation while the merge did.
# t/007_segment_cap.pl had exactly this bug and fixed it under doc/GAPS.md G28
# (see its comment above the peak bound); the fix was never propagated here.
# Every pass this file reported before 2026-09-29 was a SERIAL run (hard rule
# 13).  Now: psql exits via \q, pump_all pumps every handle with a deadline,
# and each session reports a wall-clock SPAN so the overlap is asserted.
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

# merge + 3 inserters + 2 readers, all pumped at once on the same index
my ($mh, $mout, $merr)   = psql_proc($merge_sql);
my ($i1h, $i1out, $i1err) = psql_proc(inserter_sql(1));
my ($i2h, $i2out, $i2err) = psql_proc(inserter_sql(2));
my ($i3h, $i3out, $i3err) = psql_proc(inserter_sql(3));
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);
my $merger = { name => 'merge', h => $mh, err => $merr };
my @inserters = ({ name => 'inserter i1', h => $i1h, err => $i1err },
                 { name => 'inserter i2', h => $i2h, err => $i2err },
                 { name => 'inserter i3', h => $i3h, err => $i3err });
my @readers = ({ name => 'reader r1', h => $r1h, err => $r1err },
               { name => 'reader r2', h => $r2h, err => $r2err });

finish_or_report([ $merger, @inserters, @readers ], 150);

my $writer_err = "$$merr\n$$i1err\n$$i2err\n$$i3err";
my $reader_err = "$$r1err\n$$r2err";
my $all_err = "$writer_err\n$reader_err";

# POSITIVE CONTROL: the merge and every inserter did work, every inserter's loop
# overlapped the merge's (concurrent extenders -- the thing this file is for),
# and every reader overlapped the merge and at least one inserter.  Serial
# execution (the pre-2026-09-29 harness) gives ~0 s overlap.
my $ms = span_of($$merr);
diag("merge: no SPAN notice (aborted?)") unless $ms;
cmp_ok($ms ? $ms->{iters} : 0, '>', 0,
    'merge completed at least one weave_merge/weave_vacuum iteration (b > 0)');
my @ispans;
for my $i (@inserters) {
    my $is = span_of(${ $i->{err} });
    diag("$i->{name}: no SPAN notice (aborted before its end?)") unless $is;
    push @ispans, $is if $is;
    cmp_ok($is ? $is->{iters} : 0, '>', 0,
        "$i->{name} completed at least one insert batch (b > 0)");
    my $ov = ($is && $ms) ? overlap_secs($is, $ms) : -1;
    cmp_ok($ov, '>=', 5,
        sprintf('%s and merge ran concurrently (overlap %.1fs)', $i->{name}, $ov));
}
for my $r (@readers) {
    my $rs = span_of(${ $r->{err} });
    diag("$r->{name}: no SPAN notice (aborted before its end?)") unless $rs;
    my $ovm = ($rs && $ms) ? overlap_secs($rs, $ms) : -1;
    cmp_ok($ovm, '>=', 5,
        sprintf('%s and merge ran concurrently (overlap %.1fs)', $r->{name}, $ovm));
    my $ovi = -1;
    if ($rs) {
        for my $is (@ispans) {
            my $o = overlap_secs($rs, $is);
            $ovi = $o if $o > $ovi;
        }
    }
    cmp_ok($ovi, '>=', 5,
        sprintf('%s and an inserter ran concurrently (overlap %.1fs)', $r->{name}, $ovi));
}

# The specific reported failure.
my $beyond_eof = ($all_err =~ /beyond EOF/i) ? 1 : 0;
is($beyond_eof, 0, 'no "unexpected data beyond EOF" during concurrent merge + insert');

# Any ERROR at all in a writer (merge/insert) backend is a failure -- read,
# insert, and merge on the same index must all succeed concurrently.
my $writer_errored = ($writer_err =~ /\bERROR:/) ? 1 : 0;
if ($writer_errored) {
    my @errs = grep { /\bERROR:/ } split /\n/, $writer_err;
    diag("writer errors:\n" . join("\n", @errs));
}
is($writer_errored, 0, 'no ERROR in merge/insert backends during concurrent churn');

# Readers must not error and must never see a wrong anchor count.
my $reader_errored = ($reader_err =~ /\bERROR:/) ? 1 : 0;
if ($reader_errored) {
    my %seen;
    my @e = grep { /\bERROR:/ && !$seen{$_}++ } split /\n/, $reader_err;
    diag("distinct reader ERROR lines (first 5):\n"
        . join("\n", @e[0 .. ($#e < 4 ? $#e : 4)]));
}
my $misses = () = ($reader_err =~ /ANCHOR_MISS/g);
is($reader_errored, 0, 'no ERROR in reader backends during concurrent churn');
is($misses, 0, 'concurrent readers always saw the exact anchor count');

my $merge_done = ($$merr !~ /\bERROR:/) ? 1 : 0;
is($merge_done, 1, 'merge backend ran its weave_merge/weave_vacuum loop without aborting');

my $final = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery});
is($final, 2000, 'anchor count still exact after concurrent churn settles');

$node->stop;
done_testing();
