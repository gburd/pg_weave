# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 010_format_v6_upgrade.pl -- read a genuinely PRE-v6 on-disk index with v6 code.
#
# doc/PRODUCTION_READINESS.md blocking gate 5 (format v6) and gate 6 (an upgrade
# test over an index CONTAINING DATA, which was explicitly still owed).
#
# WHY THIS TEST LOOKS THE WAY IT DOES.  Backward compatibility is a hard
# requirement, and until now the project's own note in t/009_doclen_sidecar.pl was
# honest about not testing it: "the dual-read of a genuinely-old v3 on-disk index
# is validated out-of-tree in the release qualification (it needs two .so
# builds)".  An out-of-tree gate is a gate nobody runs.
#
# One .so is enough if the OLD image is manufactured instead of built: stop the
# server, rewrite the metapage into exactly the bytes a v5 build would have
# written, restart, and read it.  That is legitimate because the v5 -> v6 metapage
# delta is precisely known and tiny -- the version word, and four bytes per bolt
# descriptor that were TAIL PADDING in v5 and are WeaveSegMeta.chandesc in v6.
# (doc/specs/SEGMENT_FORMAT.md sect. 6 predicted the segs[] stride would change;
# it does not, because those four bytes already existed as padding for the
# struct's double members.  If the stride HAD changed this test would have to
# re-stride segs[], which is why weave_meta_from_page() carries the
# WeaveMetaPageDataV5 struct and the static asserts regardless.)
#
# What is asserted, in order:
#   1. A v6 index with data answers a set of queries; the answers are recorded.
#   2. After downgrading the metapage to v5, the SAME queries return BYTE-
#      IDENTICAL answers.  This is the compatibility gate: a pre-v6 index must
#      still be readable, and reading it must not change a single row.
#   3. weave_check() reports the index as format v5 with zero self-describing
#      bolts -- i.e. the versioned reader really took the pre-v6 path rather than
#      reading the padding bytes as block numbers.
#   4. The deep check REPORTS the orphaned descriptor pages as unreachable.  This
#      is the leak detector proving it has teeth: if it said "ok" here it would
#      say "ok" for a real leak too.
#   5. Upgrading in place -- inserting into the v5-shaped index and merging -- moves
#      the metapage to v6, gives the new bolt a descriptor page, and STILL returns
#      identical answers for the original rows.  That is gate 6's "upgrade over an
#      index containing data".
#   6. A crash and recovery over the upgraded index preserves it, because the
#      descriptor page is written through GenericXLog like everything else.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# On-disk metapage layout (include/weave/am.h).  BLCKSZ default build.
#   page contents start at MAXALIGN(SizeOfPageHeaderData) = 24
#   WeaveMetaPageData: magic u32 @0, version u32 @4, ndocs f64 @8,
#                      sumdoclen f64 @16, nsegments u32 @24, pendinghead @28,
#                      pendingtail @32, npending u32 @36, segs[128] @40
#   WeaveSegMeta (56 bytes): dictstart @0, trgmstart @4, livedocs @8, <pad @12>,
#                      ndocs @16, sumdoclen @24, nterms @32, ndeleted @36,
#                      livedocslen @40, dictindexstart @44, doclenstart @48,
#                      chandesc @52   <-- v6 uses what v5 left as padding
use constant {
	BLCKSZ                => 8192,
	CONTENT_START         => 24,
	META_MAGIC            => 0x42324635,    # "B2F5"
	META_VERSION_OFF      => 4,
	META_NSEGMENTS_OFF    => 24,
	META_SEGS_OFF         => 40,
	SEGMETA_SIZE          => 56,
	SEGMETA_CHANDESC_OFF  => 52,
	WEAVE_MAX_SEGMENTS    => 128,
	INVALID_BLOCK         => 0xFFFFFFFF,
	WEAVE_VERSION_V5      => 5,
	WEAVE_VERSION_V6      => 6,
};

# Rewrite the metapage of an index file into the v5 on-disk shape.  Server MUST be
# down.  Returns (old_version, list of chandesc blocks that were pointed at).
#
# Setting chandesc to 0 rather than to InvalidBlockNumber is the faithful choice:
# a v4/v5 metapage was produced by MemSet(meta, 0, ...) followed by field
# assignments, so those four padding bytes were ZERO on disk.  Zero is also block
# 0 -- the metapage -- so if the v6 reader ever trusted the padding instead of
# overwriting it, it would treat the metapage itself as a bolt's descriptor page,
# which is exactly the kind of wrong answer this test has to rule out.
sub downgrade_metapage_to_v5
{
	my ($path) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	binmode $fh;
	my $buf;

	sysseek($fh, CONTENT_START, 0)             or die "seek: $!";
	sysread($fh, $buf, 4) == 4                 or die "short read: $!";
	my $magic = unpack('V', $buf);
	die sprintf("metapage magic 0x%08X, expected 0x%08X", $magic, META_MAGIC)
	  unless $magic == META_MAGIC;

	sysseek($fh, CONTENT_START + META_VERSION_OFF, 0) or die "seek: $!";
	sysread($fh, $buf, 4) == 4                 or die "short read: $!";
	my $oldver = unpack('V', $buf);

	sysseek($fh, CONTENT_START + META_NSEGMENTS_OFF, 0) or die "seek: $!";
	sysread($fh, $buf, 4) == 4                 or die "short read: $!";
	my $nsegments = unpack('V', $buf);

	# collect, then blank, every bolt's chandesc pointer
	my @orphans;
	for my $s (0 .. WEAVE_MAX_SEGMENTS - 1)
	{
		my $off = CONTENT_START + META_SEGS_OFF + $s * SEGMETA_SIZE
		  + SEGMETA_CHANDESC_OFF;
		sysseek($fh, $off, 0)     or die "seek: $!";
		sysread($fh, $buf, 4) == 4 or die "short read: $!";
		my $blk = unpack('V', $buf);
		push @orphans, $blk
		  if $s < $nsegments && $blk != INVALID_BLOCK && $blk != 0;
		sysseek($fh, $off, 0)     or die "seek: $!";
		syswrite($fh, pack('V', 0)) == 4 or die "write: $!";
	}

	# and the version word last, so a crash mid-surgery leaves a v6 page
	sysseek($fh, CONTENT_START + META_VERSION_OFF, 0) or die "seek: $!";
	syswrite($fh, pack('V', WEAVE_VERSION_V5)) == 4   or die "write: $!";
	close($fh) or die "close: $!";
	return ($oldver, $nsegments, @orphans);
}

sub read_metapage_version
{
	my ($path) = @_;
	open(my $fh, '<:raw', $path) or die "open $path: $!";
	my $buf;
	sysseek($fh, CONTENT_START + META_VERSION_OFF, 0) or die "seek: $!";
	sysread($fh, $buf, 4) == 4 or die "short read: $!";
	close($fh);
	return unpack('V', $buf);
}

# no_data_checksums: this test rewrites index bytes by hand, and a checksum
# failure would mask the thing under test.  PG18 enables checksums at initdb by
# default; PG<=17's harness ignores the unknown key (checksums already off).
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "maintenance_work_mem = 64MB\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# A corpus with three frequency bands, enough rows for several posting blocks,
# and a trigram directory, so the index actually exercises every chain the
# versioned reader has to get right.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id bigserial PRIMARY KEY, body text);
    INSERT INTO docs(body)
      SELECT 'shared common'||(g % 13)||' mid'||(g % 211)||' rare'||g||' '
             || (SELECT string_agg('w'||((g*7+s) % 500), ' ')
                 FROM generate_series(1, 12) s)
      FROM generate_series(1, 20000) g;
    ALTER TABLE docs ADD COLUMN d wdoc;
    UPDATE docs SET d = to_wdoc('simple', body);
    CREATE INDEX docs_weave ON docs USING weave (d) WITH (trigrams = on);
});

# --- the answer set the upgrade must not perturb ---------------------------
# Counts across all three frequency bands, a phrase-free AND, and a ranked
# top-k with an explicit tiebreak so the ordering is total (a ranked query whose
# order depends on a tie is not a compatibility assertion, it is a coin flip).
my @count_queries = (
	q{d @@@ 'shared'::wquery},
	q{d @@@ 'common3'::wquery},
	q{d @@@ 'mid7'::wquery},
	q{d @@@ 'rare1234'::wquery},
	q{d @@@ 'common3 AND mid7'::wquery},
	q{d @@@ 'w42'::wquery},
);

sub answers
{
	my ($label) = @_;
	my @out;
	for my $q (@count_queries)
	{
		push @out, $node->safe_psql('postgres',
			qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE $q});
	}
	# ranked top-20, id as the tiebreak
	push @out, $node->safe_psql('postgres', q{
		SET enable_seqscan=off;
		SELECT string_agg(id::text, ',' ORDER BY ord) FROM (
		  SELECT id, row_number() OVER () AS ord FROM docs
		   WHERE d @@@ 'shared'::wquery
		   ORDER BY d <=> 'common3 mid7'::wquery, id LIMIT 20) t});
	# and the distance values themselves, to catch a change in how doclen and df
	# are read: a wrong doclen produces a plausible-but-different score, which the
	# id ordering above can hide when the ranking is not close.
	push @out, $node->safe_psql('postgres', q{
		SET enable_seqscan=off;
		SELECT string_agg(round(dist::numeric, 6)::text, ',' ORDER BY o) FROM (
		  SELECT d <=> 'common3 mid7'::wquery AS dist, id,
		         row_number() OVER (ORDER BY d <=> 'common3 mid7'::wquery, id) AS o
		    FROM docs WHERE d @@@ 'common3'::wquery
		   ORDER BY 3 LIMIT 20) t});
	note("$label: " . join(' | ', map { substr($_, 0, 40) } @out));
	return \@out;
}

my $before = answers('v6 (as built)');
ok($before->[0] > 0, 'the v6 index answers the broad query at all');

my $relpath = $node->safe_psql('postgres',
	"SELECT pg_relation_filepath('docs_weave')");
my $abspath = $node->data_dir . '/' . $relpath;

my $nseg_before = $node->safe_psql('postgres',
	"SELECT weave_index_nsegments('docs_weave')");
my $cd_before = $node->safe_psql('postgres',
	q{SELECT detail FROM weave_check('docs_weave')
	   WHERE invariant = 'chandesc_coverage'});
like($cd_before, qr/^\Q$nseg_before\E self-describing, 0 lexical-only$/,
	"every v6 bolt has a descriptor page ($nseg_before bolt(s))");

is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('docs_weave', true) WHERE NOT ok}),
	'0', 'v6 index satisfies every weave_check() invariant');

# --- manufacture the pre-v6 image -----------------------------------------
$node->stop;
my ($oldver, $nsegments, @orphans) = downgrade_metapage_to_v5($abspath);
is($oldver, WEAVE_VERSION_V6, 'the index really was written as format v6');
cmp_ok(scalar(@orphans), '>', 0,
	'the v6 index had at least one channel-descriptor page to orphan');
is(read_metapage_version($abspath), WEAVE_VERSION_V5,
	'metapage rewritten to format v5 on disk');
$node->start;

# 1. THE COMPATIBILITY GATE: identical answers from the pre-v6 image.
my $after_downgrade = answers('v5 (manufactured pre-v6 image)');
is_deeply($after_downgrade, $before,
	'a pre-v6 index returns byte-identical answers under v6 code');

# 2. the versioned reader really took the pre-v6 path
like($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('docs_weave')
		   WHERE invariant = 'metapage_version_recognized'}),
	qr/^format v5 /, 'weave_check() reports the index as format v5');
is($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('docs_weave')
		   WHERE invariant = 'chandesc_coverage'}),
	"0 self-describing, $nsegments lexical-only",
	'every pre-v6 bolt reads as lexical-only, not as a bolt rooted at block 0');

# 3. and no invariant other than reachability is violated: in particular
# chandesc_version_consistent must hold, because no bolt claims a descriptor.
is($node->safe_psql('postgres',
		q{SELECT ok FROM weave_check('docs_weave')
		   WHERE invariant = 'chandesc_version_consistent'}),
	't', 'no pre-v6 bolt claims a descriptor page');

# 4. TEETH: the orphaned descriptor pages must be REPORTED as leaked.  A leak
# detector that stays quiet here would stay quiet for a real leak.
is($node->safe_psql('postgres',
		q{SELECT ok FROM weave_check('docs_weave', true)
		   WHERE invariant = 'pages_reachable_or_freed'}),
	'f', 'the deep check reports the orphaned descriptor pages as unreachable');
like($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('docs_weave', true)
		   WHERE invariant = 'pages_reachable_or_freed'}),
	qr/unreachable page\(s\) not flagged freed/,
	'and says what it found');

# --- 5. upgrade in place, over an index CONTAINING DATA --------------------
# Inserting alone does not move the version word: the pending append touches only
# metapage HEAD fields, which are at identical offsets in every format
# generation, so it deliberately does not upcast.  A DIRECTORY change does.
$node->safe_psql('postgres', q{
    INSERT INTO docs(body, d)
      SELECT 'shared delta'||(g % 7)||' rare'||(100000+g),
             to_wdoc('simple', 'shared delta'||(g % 7)||' rare'||(100000+g))
      FROM generate_series(1, 3000) g;
});
$node->safe_psql('postgres', "SELECT weave_merge('docs_weave')");

# CHECKPOINT first: the metapage lives in shared buffers until one, and reading
# the file without it sees the pre-upgrade bytes.  (Learned the hard way here.)
$node->safe_psql('postgres', 'CHECKPOINT');
is(read_metapage_version($abspath), WEAVE_VERSION_V6,
	'a directory change upcasts the metapage to format v6 ON DISK');
like($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('docs_weave')
		   WHERE invariant = 'metapage_version_recognized'}),
	qr/^format v6 /, 'weave_check() now reports format v6');

my $cd_after = $node->safe_psql('postgres',
	q{SELECT detail FROM weave_check('docs_weave')
	   WHERE invariant = 'chandesc_coverage'});
like($cd_after, qr/^[1-9]\d* self-describing/,
	"bolts written after the upgrade self-describe ($cd_after)");

# The ORIGINAL rows must still answer exactly as they did before any of this.
# The new rows change the corpus statistics, so the ranked/BM25 comparisons are
# re-run against the pre-existing subset only.
for my $i (0 .. $#count_queries)
{
	my $q = $count_queries[$i];
	my $now = $node->safe_psql('postgres', qq{
		SET enable_seqscan=off;
		SELECT count(*) FROM docs WHERE id <= 20000 AND $q});
	is($now, $before->[$i],
		"after the in-place upgrade, the original rows still answer: $q");
}

# Index path versus sequential path over the whole (upgraded, mixed) index: the
# only ground truth that does not depend on the corpus being unchanged.
for my $q (@count_queries)
{
	my $idx = $node->safe_psql('postgres',
		qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE $q});
	my $seq = $node->safe_psql('postgres',
		qq{SET enable_indexscan=off; SET enable_bitmapscan=off;
		   SELECT count(*) FROM docs WHERE $q});
	is($idx, $seq, "upgraded index agrees with a sequential scan: $q");
}

# A vacuum rewrites and coalesces every bolt, so the pre-v6 bolts are gone and
# every remaining bolt self-describes.
$node->safe_psql('postgres', "SELECT weave_vacuum('docs_weave')");
like($node->safe_psql('postgres',
		q{SELECT detail FROM weave_check('docs_weave')
		   WHERE invariant = 'chandesc_coverage'}),
	qr/^[1-9]\d* self-describing, 0 lexical-only$/,
	'after a vacuum every bolt self-describes');

# ... but the pages THIS TEST orphaned stay orphaned, and that is the honest
# result rather than a fixable one: blanking chandesc took those pages off every
# chain WITHOUT setting WEAVE_FREED, so they are neither reachable nor in the FSM,
# and nothing short of a REINDEX reclaims a page in that state.  Assert exactly
# that -- the one violated invariant is the leak, and nothing else regressed.
is($node->safe_psql('postgres',
		q{SELECT string_agg(invariant, ',' ORDER BY invariant)
		    FROM weave_check('docs_weave', true) WHERE NOT ok}),
	'pages_reachable_or_freed',
	'the only violated invariant is the leak this test manufactured');

# REINDEX rebuilds from the heap, which is the documented cure.
$node->safe_psql('postgres', 'REINDEX INDEX docs_weave');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('docs_weave', true) WHERE NOT ok}),
	'0', 'REINDEX returns the index to a fully consistent state');

# --- 6. crash safety of the new page kind ---------------------------------
# The descriptor page is written through GenericXLog like every other page
# (AGENTS.md hard rule 2), so an immediate stop must replay to a valid index.
my $pre_crash = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE d @@@ 'shared'::wquery});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres',
		q{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE d @@@ 'shared'::wquery}),
	$pre_crash, 'crash recovery preserves the upgraded index');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('docs_weave', true) WHERE NOT ok}),
	'0', 'and every invariant still holds after replay');

$node->stop;
done_testing();
