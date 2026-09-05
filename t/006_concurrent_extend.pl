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
DECLARE deadline timestamptz := clock_timestamp() + interval '12 seconds';
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    PERFORM weave_merge('docs_bm25');
    PERFORM weave_vacuum('docs_bm25');
  END LOOP;
END $$;
};

# --- Inserter backend: churn new documents (new segments) for ~12s ----------
# Distinct session id `s` keeps every batch's bodies unique so segments keep
# growing/flushing (constant relation extension) alongside the merge.
sub inserter_sql {
    my ($s) = @_;
    return qq{
DO \$\$
DECLARE deadline timestamptz := clock_timestamp() + interval '12 seconds'; b int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    INSERT INTO docs(kind, body)
      SELECT 'churn', 'churnterm s$s r'||b||' w'||(g%50)||' doc'||g
      FROM generate_series(1, 1500) g;
    DELETE FROM docs WHERE kind='churn' AND body LIKE 'churnterm s$s %' AND (id % 3) = 0;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'INSERTER_DONE s=$s batches=%', b;
END \$\$;
};
}

# --- Reader: count the anchor term repeatedly (must stay 2000, no error) -----
my $reader_sql = q{
SET enable_seqscan=off;
DO $$
DECLARE deadline timestamptz := clock_timestamp()+interval '12 seconds'; c bigint; bad int := 0; tot int := 0;
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

# merge + 3 inserters + 2 readers, all overlapping on the same index
my ($mh, $mout, $merr)   = psql_proc($merge_sql);
my ($i1h, $i1out, $i1err) = psql_proc(inserter_sql(1));
my ($i2h, $i2out, $i2err) = psql_proc(inserter_sql(2));
my ($i3h, $i3out, $i3err) = psql_proc(inserter_sql(3));
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);

finish($_) for ($i1h, $i2h, $i3h, $r1h, $r2h, $mh);

my $writer_err = "$$merr\n$$i1err\n$$i2err\n$$i3err";
my $reader_err = "$$r1err\n$$r2err";
my $all_err = "$writer_err\n$reader_err";

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
