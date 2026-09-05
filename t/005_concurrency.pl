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
DECLARE deadline timestamptz := clock_timestamp() + interval '10 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    DELETE FROM docs WHERE kind='churn';
    INSERT INTO docs(kind, body)
      SELECT 'churn','churnterm w'||(g%50)||' r'||b||' doc'||g FROM generate_series(1,4000) g;
    PERFORM weave_merge('docs_bm25');
    PERFORM weave_vacuum('docs_bm25');
    b := b + 1;
  END LOOP;
END $$;
\echo WRITER_DONE
};

# --- Reader: count the anchor term as fast as possible for ~10s -------------
# Emits one line per read: '2000' when correct. \gset + \echo lets us print the
# value; we scan the output for any line that is a number other than 2000.
my $reader_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE deadline timestamptz := clock_timestamp()+interval '10 seconds'; c bigint; bad int := 0; tot int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    SELECT count(*) INTO c FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery;
    tot := tot + 1;
    IF c <> 2000 THEN bad := bad + 1; RAISE WARNING 'ANCHOR_MISS count=%', c; END IF;
  END LOOP;
  RAISE NOTICE 'READER_DONE reads=% wrong=%', tot, bad;
END $$;
};

sub psql_proc {
    my ($sql) = @_;
    my ($in, $out, $err) = ($sql, '', '');
    my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=0', '-d', $conn],
                  '<', \$in, '>', \$out, '2>', \$err);
    return ($h, \$out, \$err);
}

# start writer + two readers concurrently
my ($wh, $wout, $werr) = psql_proc($writer_sql);
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);

finish($r1h);
finish($r2h);
finish($wh);

my $all_err = "$$r1err\n$$r2err";
my $reads_line = join("\n", grep { /READER_DONE/ } split /\n/, $all_err);
diag("reader summary: $reads_line");

my $misses = () = ($all_err =~ /ANCHOR_MISS/g);
my $reader_errored = ($all_err =~ /\bERROR:/) ? 1 : 0;

is($misses, 0, 'no concurrent anchor read returned a wrong count (page-recycle miss)');
is($reader_errored, 0, 'no reader hit an ERROR during concurrent merge/insert churn');

my $final = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'anchorterm'::wquery});
is($final, 2000, 'anchor count still exact after the churn settles');

$node->stop;
done_testing();
