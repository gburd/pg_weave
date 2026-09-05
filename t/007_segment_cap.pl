# 007_segment_cap.pl -- writes must never fail because the segment directory
# filled up.
#
# Regression for the field outage: "index hit 128-segment cap under load", which
# forced disabling the index.  pg_weave stores its segment directory as a fixed
# array of WEAVE_MAX_SEGMENTS (128) descriptors in the metapage.  Each flush of a
# pending buffer / each oversized document becomes a segment; if merging falls
# behind the write rate, the directory used to fill and the NEXT insert threw
#   ERROR: weave index ... reached the maximum of 128 segments
# turning a write-heavy workload into a hard outage.  An index access method
# must never refuse a write because internal compaction is behind: adding a
# segment now merges to free a slot and retries instead of erroring.
#
# This drives enough segment-creating writes (oversized docs -> one segment
# each) to blow WAY past 128 segments, from several concurrent inserters plus a
# concurrent reader, and asserts: no insert ever hit the cap error, and every
# inserted row is searchable afterward.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "shared_buffers = 32MB\n");
# tiny mwm so eager merges pick small batches; the point is the cap, not memory
$node->append_conf('postgresql.conf', "maintenance_work_mem = 1MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
# An "oversized" document (its analyzed wdoc exceeds one pending page) is
# indexed as its own segment immediately -- the fastest way to mint segments.
# Build a big body: many distinct terms so the wdoc is large.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    CREATE INDEX docs_bm25 ON docs USING weave (to_wdoc('simple', body));
});

# helper: a big unique body ~ tens of KB of distinct tokens -> oversized segment
my $bodyexpr = q{'capterm ' || string_agg('t'||s||'x'||$SID||'r'||b||'w'||g, ' ')};

sub inserter_sql {
    my ($sid) = @_;
    # each backend inserts N oversized docs; across all backends this far
    # exceeds 128 segments unless merging keeps making room.
    my $expr = $bodyexpr;
    $expr =~ s/\$SID/$sid/g;
    return qq{
DO \$\$
DECLARE b int := 0;
BEGIN
  WHILE b < 60 LOOP
    INSERT INTO docs(body)
      SELECT 'capterm ' || string_agg('t'||s||'x'||$sid||'r'||b||'w'||g, ' ')
      FROM generate_series(1,1) g, generate_series(1,4000) s;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'INSERTER_DONE sid=$sid rows=%', b;
END \$\$;
};
}

# a concurrent reader, to also exercise read+write+merge together
my $reader_sql = q{
DO $$
DECLARE dl timestamptz := clock_timestamp() + interval '15 seconds'; c bigint;
BEGIN
  WHILE clock_timestamp() < dl LOOP
    SELECT count(*) INTO c FROM docs WHERE to_wdoc('simple', body) @@@ 'capterm'::wquery;
  END LOOP;
END $$;
};

sub psql_proc {
    my ($sql) = @_;
    my ($in, $out, $err) = ($sql, '', '');
    my $h = start(['psql', '-X', '-v', 'ON_ERROR_STOP=0', '-d', $node->connstr('postgres')],
                  '<', \$in, '>', \$out, '2>', \$err);
    return ($h, \$out, \$err);
}

# 4 concurrent inserters (240 oversized docs total -> ~240 segments if unmerged,
# well past the 128 cap) + 1 reader
my @ins;
push @ins, [ psql_proc(inserter_sql($_)) ] for (1 .. 4);
my ($rh, $rout, $rerr) = psql_proc($reader_sql);

finish($_->[0]) for @ins;
finish($rh);

my $ins_err = join("\n", map { ${ $_->[2] } } @ins);
my $all_err = "$ins_err\n$$rerr";

# THE assertion: no write ever failed because the directory filled.
my $cap_hit = ($all_err =~ /maximum of \d+ segments|reached the maximum|could not free a segment-directory slot/i) ? 1 : 0;
is($cap_hit, 0, 'no insert failed with a segment-cap error under concurrent write load');

my $any_err = ($ins_err =~ /\bERROR:/) ? 1 : 0;
if ($any_err) {
    my @e = grep { /\bERROR:/ } split /\n/, $ins_err;
    diag("inserter errors:\n" . join("\n", @e[0 .. ($#e < 8 ? $#e : 8)]));
}
is($any_err, 0, 'no inserter backend errored at all');

# All inserted rows must be searchable (data intact + merges preserved postings).
my $total = $node->safe_psql('postgres', 'SELECT count(*) FROM docs');
my $match = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'capterm'::wquery});
is($match, $total, "every inserted doc ($total) is searchable after the cap churn");

# The directory is bounded (merges kept it under the hard cap).
my $nseg = $node->safe_psql('postgres', q{SELECT weave_index_nsegments('docs_bm25')});
diag("final segments = $nseg (hard cap 128), rows = $total");
cmp_ok($nseg, '<=', 128, 'live segment count stayed within the hard cap');

$node->stop;
done_testing();
