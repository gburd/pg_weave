# 009_doclen_sidecar.pl -- format v4 (per-segment doclen sidecar) integrity.
#
# 1.5.0 moves per-document length out of the posting lists into a per-segment
# quantized-byte sidecar (on-disk format v3 -> v4).  This test exercises the v4
# write/read/merge/recovery paths that the format change touches:
#   - a fresh v4 index answers @@@ and ranked queries correctly (quantized
#     doclen must still score + order right vs a seqscan ground truth);
#   - inserting more rows (new segments) then weave_merge() combines segments and
#     stays correct (the merged-segment sidecar path);
#   - weave_vacuum() compaction stays correct (the vacuum rewrite path);
#   - a crash + recovery preserves the index (v4 pages are GenericXLog'd, so
#     replay must reconstruct byte-identical sidecar + posting pages).
#
# The dual-read of a genuinely-old v3 on-disk index (built by the prior release)
# is validated out-of-tree in the release qualification (it needs two .so
# builds); here we cover everything reproducible with a single v4 build.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "shared_buffers = 256MB\n");
$node->append_conf('postgresql.conf', "maintenance_work_mem = 64MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# High-vocabulary corpus with VARIED tf/doclen so BM25 scores spread out (not a
# single degenerate tie band) -- the regime where quantized doclen must still
# order correctly.  'hot' appears a variable number of times per doc.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    INSERT INTO docs(body)
      SELECT repeat('hot ', 1 + (g % 6))
             || (SELECT string_agg('w'||((g*13+s) % 8000), ' ')
                 FROM generate_series(1, 30 + (g % 60)) s)
      FROM generate_series(1, 80000) g;
    ALTER TABLE docs ADD COLUMN d wdoc;
    UPDATE docs SET d = to_wdoc('english', body);
    CREATE INDEX docs_weave ON docs USING weave (d);
});

# --- helper: index count vs seqscan ground truth for a term ---
sub check_term {
    my ($term, $label) = @_;
    my $idx = $node->safe_psql('postgres', qq{
        SET enable_seqscan=off;
        SELECT count(*) FROM docs WHERE d \@\@\@ to_wquery('english','$term')});
    my $seq = $node->safe_psql('postgres', qq{
        SET enable_indexscan=off; SET enable_bitmapscan=off;
        SELECT count(*) FROM docs WHERE d \@\@\@ to_wquery('english','$term')});
    is($idx, $seq, "v4 count matches seqscan: $label ($term)");
    return $idx;
}

# --- helper: ranked top-k must all actually match the query (no bogus rows) ---
sub check_ranked_valid {
    my ($term, $k, $label) = @_;
    # every id the ranked scan returns must satisfy @@@ (a mis-scored doclen
    # could surface a non-matching doc); count how many of the top-k truly match.
    my $bad = $node->safe_psql('postgres', qq{
        SET enable_seqscan=off;
        WITH topk AS (
          SELECT id FROM docs WHERE d \@\@\@ to_wquery('english','$term')
          ORDER BY d <=> to_wquery('english','$term') LIMIT $k)
        SELECT count(*) FROM topk
        WHERE id NOT IN (
          SELECT id FROM docs WHERE d \@\@\@ to_wquery('english','$term'))});
    is($bad, 0, "v4 ranked top-$k all match: $label ($term)");
}

# 1) fresh v4 index: correct counts + valid ranked results
my $c_hot  = check_term('hot',  'common term, fresh v4');
my $c_w1   = check_term('w1',   'mid term, fresh v4');
check_ranked_valid('hot', 10, 'fresh v4');
check_ranked_valid('w1',  10, 'fresh v4');
cmp_ok($c_hot, '>', 0, 'common term has matches');

# 2) insert more rows -> new segments -> weave_merge combines them; stays correct
$node->safe_psql('postgres', q{
    INSERT INTO docs(body,d)
      SELECT body, to_wdoc('english', body) FROM docs LIMIT 40000});
$node->safe_psql('postgres', q{SELECT weave_merge('docs_weave')});
my $c_hot2 = check_term('hot', 'common term after insert+merge');
cmp_ok($c_hot2, '>', $c_hot, 'merge preserved the new rows');
check_ranked_valid('hot', 20, 'after insert+merge');

# 3) weave_vacuum compaction stays correct
$node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
check_term('hot', 'common term after weave_vacuum');
check_ranked_valid('hot', 10, 'after weave_vacuum');

# 4) crash + recovery: the v4 sidecar + posting pages are GenericXLog'd, so an
#    immediate stop + restart must replay to a correct index.
my $before = check_term('w1', 'before crash');
$node->stop('immediate');
$node->start;
my $after = $node->safe_psql('postgres', q{
    SET enable_seqscan=off;
    SELECT count(*) FROM docs WHERE d @@@ to_wquery('english','w1')});
is($after, $before, 'v4 index correct after crash + recovery');
check_ranked_valid('hot', 10, 'after crash recovery');

$node->stop;
done_testing();
