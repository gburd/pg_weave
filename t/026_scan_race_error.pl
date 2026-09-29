# 026_scan_race_error.pl -- a scan that loses every race with a merge must ERROR.
#
# doc/GAPS.md G58.  Every generation-bracketed read path in src/am/amscan.c
# snapshots the metapage directory generation, reads segment pages under
# per-page SHARE locks, and redoes the read when a concurrent merge or vacuum
# moved the generation (pages it read may have been freed and recycled).  The
# retries are capped by pg_weave.scan_race_retries, and what happens AFTER the
# cap is the point: each path used to degrade silently -- keep the last,
# possibly stale, attempt, or return no candidates -- and now raises SQLSTATE
# 40001 (serialization_failure) instead.
#
# What moves the generation (the thing this test has to drive):
# weave_meta_add_segment() (a pending flush or an insert-time segment),
# weave_merge_segments()/weave_merge_all() committing a merged directory, and
# weave_vacuum()'s livedocs rewrite.  weave_merge() does the first two in one
# call -- it flushes the pending list and then collapses -- so the writer below
# calls it once per iteration.
#
# Design.  A fixed set of rows (every 10th) contains 'needle marker'; the writer
# only ever inserts rows that do not, so the TRUE count of `needle & marker` is
# a constant N for the whole run.  The query has two terms so the count fast
# path (weave_count_dictdf_fastpath, which answers a single term from the
# dictionary df and never reaches the collector) cannot answer it: weave_count()
# then enters weave_collect_matches() directly -- the one route AGENTS.md
# records as reliably reaching the collector, rather than a plan that might
# re-derive the answer some other way.
#
# The reader sets pg_weave.scan_race_retries = 0 (one attempt, no retry), so any
# overlap with a generation bump must surface as the new 40001.  Three
# assertions:
#   errors_seen >= 1  the POSITIVE CONTROL that the path fires at all.  If it
#                     never fires within the deadline this test FAILS rather
#                     than skips: a guard that has never fired is not evidence.
#   wrong == 0        every SUCCESSFUL count is exactly N.
#   ok > 0            the reader was not simply erroring every time.
#
# And the twelfth-member guard (AGENTS.md): the GUC must really exist.  An
# absent GUC is indistinguishable from one that is set, because SET creates a
# placeholder and SHOW echoes it; pg_settings shows a real short_desc only in a
# session where the library is LOADED, so the checks touch the index first, and
# an out-of-range SET must be REFUSED (a placeholder accepts anything).
#
# The writer and reader are independent psql processes (IPC::Run, the t/005
# idiom that runs in the nix sandbox) whose loops overlap in wall-clock time.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $nrows = 100000;
my $N = $nrows / 10;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
$node->safe_psql('postgres', qq{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    INSERT INTO docs(body)
      SELECT CASE WHEN g % 10 = 0
                  THEN 'needle marker w' || (g % 50) || ' doc' || g
                  ELSE 'hay marker w' || (g % 50) || ' doc' || g END
      FROM generate_series(1, $nrows) g;
    CREATE INDEX docs_w ON docs USING weave (to_wdoc('simple', body));
    CREATE TABLE race_ctl (stop boolean NOT NULL);
    INSERT INTO race_ctl VALUES (false);
    CREATE TABLE race_writer (iters int);
    CREATE TABLE race_result (ok int, errs int, wrong int, other int,
                              lastother text);
});

# --- The GUC exists, in a session where the library is loaded -------------
my $guc = $node->safe_psql('postgres', q{
    SELECT weave_count('docs_w', 'needle & marker'::wquery) >= 0;
    SELECT coalesce(short_desc, '<null>') || '|' || setting || '|' || boot_val
      FROM pg_settings WHERE name = 'pg_weave.scan_race_retries';
});
my @guc = split /\n/, $guc;
is($guc[-1],
   'Retries a pg_weave scan makes when a concurrent merge reorganizes the index under it, before it raises a serialization failure.|10|10',
   'pg_weave.scan_race_retries is a real GUC (short_desc present), default 10');

my $show = $node->safe_psql('postgres', q{
    SELECT weave_count('docs_w', 'needle & marker'::wquery) >= 0;
    SHOW pg_weave.scan_race_retries;
});
is((split /\n/, $show)[-1], '10', 'SHOW pg_weave.scan_race_retries = 10 after load');

my ($rc, $o, $e) = $node->psql('postgres', q{
    SELECT weave_count('docs_w', 'needle & marker'::wquery) >= 0;
    SET pg_weave.scan_race_retries = 1001;
});
isnt($rc, 0, 'out-of-range pg_weave.scan_race_retries is refused (not a placeholder)');
like($e, qr/1001 is outside the valid range/, 'refusal names the range');

# --- Baseline: the true count, by a heap scan and by the index -------------
my $heap_n = $node->safe_psql('postgres',
    q{SELECT count(*) FROM docs WHERE body LIKE 'needle marker %'});
is($heap_n, $N, "baseline: $N needle rows in the heap");
my $idx_n = $node->safe_psql('postgres',
    q{SELECT weave_count('docs_w', 'needle & marker'::wquery)});
is($idx_n, $N, "baseline: weave_count agrees ($N)");

my $conn = $node->connstr('postgres');

# --- Writer: move the directory generation as often as it can --------------
# Each iteration inserts rows WITHOUT 'needle' (so N never changes) and calls
# weave_merge(), which flushes the pending list (one generation bump) and
# collapses the segments (another).  COMMIT per iteration so race_ctl's stop
# flag is seen and no one transaction grows without bound.  90 s backstop.
my $writer_sql = q{
DO $$
DECLARE deadline timestamptz := clock_timestamp() + interval '90 seconds';
        b int := 0; s boolean;
BEGIN
  LOOP
    SELECT stop INTO s FROM race_ctl;
    EXIT WHEN s OR clock_timestamp() >= deadline;
    INSERT INTO docs(body)
      SELECT 'hay marker w' || (g % 50) || ' churn' || b || ' doc' || g
      FROM generate_series(1, 200) g;
    COMMIT;
    PERFORM weave_merge('docs_w');
    COMMIT;
    b := b + 1;
  END LOOP;
  INSERT INTO race_writer VALUES (b);
  COMMIT;
END $$;
};

my ($win, $wout, $werr) = ('', '', '');
# The SQL goes on the command line, not stdin: IPC::Run writes a scalar stdin
# only while THAT harness is pumped, and nothing pumps the writer until
# finish(), so a '<' \$sql writer does not start until the reader has already
# finished (the first run of this test: 0 writer iterations, 0 races).  One DO
# statement, so -c runs it outside any implicit transaction block and its
# COMMITs are legal.
my $wh = start(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-d', $conn,
                '-c', $writer_sql],
               '<', \$win, '>', \$wout, '2>', \$werr);

# Do not start the reader until the writer has visibly done work: a race test
# whose racer has not started measures nothing.
my $n0 = $node->safe_psql('postgres', 'SELECT count(*) FROM docs');
my $started = 0;
for my $i (1 .. 300)
{
    $wh->pump_nb;
    if ($node->safe_psql('postgres', 'SELECT count(*) FROM docs') > $n0)
    {
        $started = 1;
        last;
    }
    select(undef, undef, undef, 0.1);
}
ok($started, 'writer is running before the reader starts') or diag($werr);

# --- Reader: retries = 0; count until errors and successes are both seen ---
# Each count runs in its own EXCEPTION block (a subtransaction), so a 40001
# is counted and the loop goes on.  The 40001 must carry the new message with
# "1 attempts" -- which also proves the GUC, not the old hard-coded 10, set the
# cap.  Anything else is `other` and fails the test.
my $reader_sql = q{
SET pg_weave.scan_race_retries = 0;
DO $$
DECLARE deadline timestamptz := clock_timestamp() + interval '60 seconds';
        c bigint; ok int := 0; errs int := 0; wrong int := 0; other int := 0;
        lastother text := ''; st text; msg text;
BEGIN
  WHILE clock_timestamp() < deadline AND NOT (errs >= 3 AND ok >= 3) LOOP
    BEGIN
      c := weave_count('docs_w', 'needle & marker'::wquery);
      IF c = __N__ THEN
        ok := ok + 1;
      ELSE
        wrong := wrong + 1;
        RAISE WARNING 'WRONG_COUNT count=%', c;
      END IF;
    EXCEPTION WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
      IF st = '40001' AND msg = 'pg_weave: index "docs_w" was reorganized by a concurrent merge or vacuum during every one of 1 attempts to read it' THEN
        errs := errs + 1;
      ELSE
        other := other + 1;
        lastother := st || ': ' || msg;
      END IF;
    END;
  END LOOP;
  INSERT INTO race_result VALUES (ok, errs, wrong, other, lastother);
END $$;
};
$reader_sql =~ s/__N__/$N/;

my ($rrc, $rout, $rerr) = $node->psql('postgres', $reader_sql);
is($rrc, 0, 'reader session ran to completion') or diag($rerr);

$node->safe_psql('postgres', 'UPDATE race_ctl SET stop = true');
finish($wh);
unlike($werr, qr/\bERROR:/, 'writer hit no ERROR') or diag($werr);

my $iters = $node->safe_psql('postgres', 'SELECT coalesce(max(iters), 0) FROM race_writer');
cmp_ok($iters, '>', 0, 'writer completed at least one flush+merge iteration');

my $res = $node->safe_psql('postgres',
    q{SELECT ok || '|' || errs || '|' || wrong || '|' || other || '|' || lastother FROM race_result});
my ($ok, $errs, $wrong, $other, $lastother) = split /\|/, $res, 5;
$lastother //= '';
note("reader: ok=$ok errors_40001=$errs wrong=$wrong other=$other; writer iterations=$iters");

cmp_ok($errs, '>=', 1,
    'positive control: a lost race with retries = 0 raised 40001 at least once');
is($wrong, 0, "every successful count was exactly $N");
cmp_ok($ok, '>', 0, 'at least one count succeeded');
is($other, 0, 'no error other than the G58 serialization failure')
  or diag("last other error: $lastother");

my $final = $node->safe_psql('postgres',
    q{SELECT weave_count('docs_w', 'needle & marker'::wquery)});
is($final, $N, 'count still exact after the churn settles');

# --- Phase 2: the RETRY path, over segments that carry tombstones ------------
# Phase 1 runs with retries = 0, so it never executes the retry branch -- and
# review found that branch double-freed the loaded tombstone maps whenever any
# segment had one (weave_tombstones_free did not reset hasany, and the retry
# freed again what the success path had already freed).  Here the writer
# DELETEs hay rows and VACUUMs (tombstones in segments; cleanup flushes the
# pending list, moving the generation) with no weave_merge, so tombstones
# persist.  Two readers: retries = 0 first, as the positive control that races
# DO happen against this writer; then retries = 1000, which must retry through
# them and return exactly N every time.  VACUUM cannot run inside a DO block,
# so the writer is driven from here, one statement per call, pumping the reader.
sub phase2
{
    my ($retries) = @_;
    my $sql = $reader_sql;
    $sql =~ s/scan_race_retries = 0/scan_race_retries = $retries/;
    $sql =~ s/of 1 attempts/of @{[$retries + 1]} attempts/;
    $sql =~ s/interval '60 seconds'/interval '30 seconds'/;
    $sql =~ s/NOT \(errs >= 3 AND ok >= 3\)/NOT (errs >= 3 AND ok >= 3) AND ok < 400/;
    $node->safe_psql('postgres', 'TRUNCATE race_result');

    # Tombstones BEFORE the reader starts.  The reader's DO block is one
    # transaction, so its snapshot can hold back the horizon for its whole
    # run, and on a fast host (CI's pg18 leg, 2026-09-29) the retries = 0
    # reader finished inside the writer's first round: no VACUUM in the phase
    # removed anything and the in-phase maximum read 0.  Deleting and
    # vacuuming here, with nothing else running, makes the precondition
    # deterministic; the writer below keeps adding more.
    $node->safe_psql('postgres', qq{
        DELETE FROM docs WHERE body LIKE 'hay %' AND id % 13 = $retries % 13});
    $node->safe_psql('postgres', 'VACUUM docs');
    my $maxdel = $node->safe_psql('postgres',
        q{SELECT ndeleted FROM weave_index_stats('docs_w')});

    my ($in, $out, $err) = ('', '', '');
    my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-d', $conn, '-c', $sql],
                  '<', \$in, '>', \$out, '2>', \$err);
    my $b = 0;
    while ($h->pumpable)
    {
        $h->pump_nb;
        $node->safe_psql('postgres', qq{
            INSERT INTO docs(body)
              SELECT 'hay marker w' || (g % 50) || ' p2churn$b doc' || g
              FROM generate_series(1, 200) g});
        $node->safe_psql('postgres', qq{
            DELETE FROM docs WHERE body LIKE 'hay %' AND id % 7 = $b % 7});
        $node->safe_psql('postgres', 'VACUUM docs');
        # sampled HERE, while the reader runs: a later merge folds tombstones
        # away, so a reading taken after the phase can be 0 (seen once)
        my $nd = $node->safe_psql('postgres',
            q{SELECT ndeleted FROM weave_index_stats('docs_w')});
        $maxdel = $nd if $nd > $maxdel;
        $b++;
    }
    finish($h);
    unlike($err, qr/server closed the connection|terminated abnormally/,
           "phase 2 (retries = $retries): reader backend did not crash") or diag($err);
    my $r = $node->safe_psql('postgres',
        q{SELECT ok || '|' || errs || '|' || wrong || '|' || other || '|' || lastother FROM race_result});
    my @r = split /\|/, $r, 5;
    note("phase 2 retries=$retries: ok=$r[0] errors_40001=$r[1] wrong=$r[2] other=$r[3]; writer rounds=$b; max ndeleted=$maxdel");
    return (@r[0 .. 4], $maxdel);
}

my @p0 = phase2(0);
cmp_ok($p0[1] // 0, '>=', 1,
       'phase 2 positive control: races against the tombstone writer happen (40001 with retries = 0)');
cmp_ok($p0[5], '>', 0, "segments carried tombstones during phase 2 (max ndeleted = $p0[5])");
my @p1 = phase2(1000);
cmp_ok($p1[0] // 0, '>', 0, 'phase 2 (retries = 1000): counts succeeded');
cmp_ok($p1[5], '>', 0, "phase 2 (retries = 1000) ran over tombstones too (max ndeleted = $p1[5])");
is($p1[2] // -1, 0, "phase 2 (retries = 1000): every count was exactly $N");
is($p1[3] // -1, 0, 'phase 2 (retries = 1000): no error of any kind')
  or diag("last other error: " . ($p1[4] // ''));

$node->stop;
done_testing();
