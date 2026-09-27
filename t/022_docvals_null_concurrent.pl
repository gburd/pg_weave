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
DECLARE deadline timestamptz := clock_timestamp() + interval '10 seconds'; b int := 0;
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
DECLARE deadline timestamptz := clock_timestamp()+interval '10 seconds'; c bigint; bad int := 0; tot int := 0;
BEGIN
  WHILE clock_timestamp() < deadline LOOP
    SELECT count(*) INTO c FROM docs WHERE price < 100;
    tot := tot + 1;
    IF c <> $anchor_expected THEN bad := bad + 1; RAISE WARNING 'ANCHOR_MISS count=%', c; END IF;
  END LOOP;
  RAISE NOTICE 'READER_DONE reads=% wrong=%', tot, bad;
END \$\$;
};

sub psql_proc {
	my ($sql) = @_;
	my ($in, $out, $err) = ($sql, '', '');
	my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=0', '-d', $conn],
				  '<', \$in, '>', \$out, '2>', \$err);
	return ($h, \$out, \$err);
}

# start writer + two readers concurrently: all three loops are alive at the same
# time, which is the overlap hard rule 11 / G28 demands (t/005's shape).
my ($wh, $wout, $werr)    = psql_proc($writer_sql);
my ($r1h, $r1out, $r1err) = psql_proc($reader_sql);
my ($r2h, $r2out, $r2err) = psql_proc($reader_sql);

finish($r1h);
finish($r2h);
finish($wh);

my $all_err = "$$r1err\n$$r2err";
my $reads_line = join("\n", grep { /READER_DONE/ } split /\n/, $all_err);
diag("reader summary: $reads_line");
# Evidence the readers actually ran their loop many times (not zero overlap).
my ($total_reads) = ($all_err =~ /READER_DONE reads=(\d+)/);
cmp_ok($total_reads // 0, '>', 0,
	'a reader completed at least one gated read concurrently with the writer');

my $misses = () = ($all_err =~ /ANCHOR_MISS/g);
my $reader_errored = ($all_err =~ /\bERROR:/) ? 1 : 0;

is($misses, 0,
	'no concurrent anchor read returned a wrong count (page-recycle miss or leaked NULL)');
is($reader_errored, 0, 'no reader hit an ERROR during concurrent merge/insert churn');

my $final = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SET enable_bitmapscan=off;
	  SELECT count(*) FROM docs WHERE price < 100});
is($final, $anchor_expected, 'anchor gate still exact after the churn settles');

$node->stop;
done_testing();
