# 008_vacuum_reclaim.pl -- weave_vacuum() must SHRINK a bloated index, not grow it.
#
# Regression for a bug the deletion-XID recycle gate introduced: the gate made
# weave_new_buffer refuse to reuse just-freed pages until their free-XID horizon
# passed.  That is correct for the concurrent INSERT/merge path, but weave_vacuum
# (single-writer, exclusive lock) MUST be able to repack live data into the low
# pages it just freed -- with the gate blocking that, the vacate+pack phase
# extended the relation instead, so weave_vacuum() GREW the index every call
# (e.g. 182MB -> 340MB -> 498MB) and never truncated a tail.  The fix bypasses
# the recycle gate during compaction (weave_lowfree active).
#
# This builds an index, runs weave_vacuum() twice, and asserts: it does not grow,
# it reaches a stable floor, and a second call is idempotent (no further change).

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "shared_buffers = 256MB\n");
$node->append_conf('postgresql.conf', "maintenance_work_mem = 64MB\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
# a corpus large enough that the index spans many pages (so bloat/truncation is
# measurable), high enough vocabulary that merges actually move pages around.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    INSERT INTO docs(body)
      SELECT (SELECT string_agg('w'||((g*7+s)%4000), ' ') FROM generate_series(1,40) s)
             || ' uid'||g
      FROM generate_series(1, 120000) g;
    CREATE INDEX docs_weave ON docs USING weave (to_wdoc('simple', body));
});

sub idxpages {
    return $node->safe_psql('postgres',
        q{SELECT (pg_relation_size('docs_weave')/current_setting('block_size')::int)::int});
}

my $built = idxpages();
diag("index pages after build: $built");

# First weave_vacuum: must not grow, should compact to a floor.
$node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $v1 = idxpages();
diag("index pages after weave_vacuum #1: $v1");

# Second weave_vacuum: idempotent (already at floor).
$node->safe_psql('postgres', q{SELECT weave_vacuum('docs_weave')});
my $v2 = idxpages();
diag("index pages after weave_vacuum #2: $v2");

# THE assertions: weave_vacuum never grows the index, and converges.
cmp_ok($v1, '<=', $built, 'weave_vacuum() does not grow the index (reclaims, not extends)');
cmp_ok($v2, '<=', $v1 + 1, 'a second weave_vacuum() is idempotent (stable floor, +/-1 page)');

# And the index is still correct after compaction.
my $c = $node->safe_psql('postgres',
    q{SET enable_seqscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'w7'::wquery});
my $seq = $node->safe_psql('postgres',
    q{SET enable_indexscan=off; SET enable_bitmapscan=off;
      SELECT count(*) FROM docs WHERE to_wdoc('simple', body) @@@ 'w7'::wquery});
is($c, $seq, 'index results still correct after weave_vacuum compaction');

$node->stop;
done_testing();
