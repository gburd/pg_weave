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
DECLARE b int := 0; ns int; mx int := 0;
BEGIN
  WHILE b < 60 LOOP
    INSERT INTO docs(body)
      SELECT 'capterm ' || string_agg('t'||s||'x'||$sid||'r'||b||'w'||g, ' ')
      FROM generate_series(1,1) g, generate_series(1,4000) s;
    -- Sample the LIVE segment count, not just the count at the end.  The
    -- metapage directory is read off a physical page rather than through a
    -- snapshot, so this sees every backend's segment adds and merges as they
    -- happen -- which is the only way a transient spike shows up at all.  A
    -- reading taken after everything has quiesced is taken exactly when the
    -- compactor has had time to catch up.
    SELECT weave_index_nsegments('docs_bm25') INTO ns;
    IF ns > mx THEN mx := ns; END IF;
    b := b + 1;
  END LOOP;
  RAISE NOTICE 'INSERTER_DONE sid=$sid rows=% maxseg=%', b, mx;
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

# ...and comfortably under it, not merely inside it.
#
# The insert-time merge is deliberately NOT run on every insert: at 1,660 terms/doc
# every document mints a one-document segment and merging immediately rewrote a whole
# level-0 run per document (measured: 550,895 pages extended for 6,000 documents whose
# compacted form is 2,029 pages -- G20 in doc/GAPS.md).  It is now gated on there
# being WEAVE_MERGE_FANOUT smallest-level runs waiting.
#
# That gate is exactly what this bound protects, and the gate is NOT behaviour-neutral:
# weave_merge_segments() also compacts when nsegments exceeds WEAVE_MERGE_THRESHOLD
# with no level over capacity, so deferring lets the directory sit higher between
# merges.  Measured with the gate in place under the worst case for segment minting
# (one oversized row per transaction, 4,000 transactions): max 15 live segments, up
# from max 8 without it -- see bench/RESULTS_G20_MERGE_GATE.md.
#
# A `<= 128` assertion alone cannot protect it: 128 is the hard cap, and by the time
# the directory reaches it the index is already in the state a field deployment hit
# (8 -> 128 segments in ~1h, after which it could neither merge nor VACUUM).  A bound
# that only fails once recovery is impossible is not a guard.
cmp_ok($nseg, '<=', 64, 'segment count stays well under the cap, not just inside it');

# ...and the same bound on the PEAK, not just on the resting value.  Each inserter
# tracked the highest live segment count it observed between its own inserts.  Without
# that, every assertion above is taken after all four backends have quiesced and the
# compactor has had all the time it wants -- which is precisely the one moment at
# which a deferral bug cannot be seen.
my @maxes = ($ins_err =~ /INSERTER_DONE sid=\d+ rows=\d+ maxseg=(\d+)/g);
my $peak = 0;
foreach my $m (@maxes) { $peak = $m if $m > $peak; }
diag("peak live segments observed by inserters = $peak (from " . scalar(@maxes) . " backends)");
cmp_ok(scalar(@maxes), '==', 4, 'every inserter reported its peak segment count');
cmp_ok($peak, '<=', 64, 'peak live segment count stayed well under the cap');

$node->stop;
done_testing();
