# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# On-disk corruption of a v7 WEAVE_PK_SURF page: weave_check() must REPORT it and
# the trie loader must ERROR cleanly, never crash and never answer wrongly.
#
# t/011_chandesc_corruption.pl is the model.  What is different here, and it is
# the whole reason this file exists separately, is that the surf trie has a class
# of corruption NO structural validator can see:
#
#   1. A COUNT NO SECTION LENGTH DEPENDS ON (nterms) is invisible to both layers
#      of the image validator, because nothing in the image contradicts it.  Only
#      the dictionary does.
#   2. SEMANTIC corruption -- a field that IS cross-checked against the trie's
#      shape, here maxdepth -- is caught by weave_surftrie_validate(), the deep
#      pass, and the loader refuses with a clean ERRCODE_INDEX_CORRUPTED naming
#      the rule.  This is the case that proves the loader's refusal path runs.
#   3. MEMBERSHIP corruption is caught by NEITHER.  Change the label byte of the
#      last slot to 0xFF and the image is still structurally perfect: every
#      popcount identity holds, the rank and select tables still match a
#      recomputation, labels are still strictly ascending inside every node (0xFF
#      is the largest byte and that slot is the last of its node), the DFS still
#      reaches every slot, and the ordinals are still 0 < 1 < ... < nterms.  The
#      trie simply now contains a term the dictionary does not, and is missing one
#      the dictionary has.  THAT SECOND HALF IS A DROPPED ROW -- the trie is a
#      filter with false positives and no false negatives, and per AGENTS.md hard
#      rule 1 no fixed-expected-output regression test can catch a dropped row.
#
# Case 3 is the one that gives weave_check()'s surf_trie_matches_dictionary its
# teeth: it is the only thing in the tree that compares the trie against the
# dictionary's actual bytes, and if it did not fail here it would not fail for a
# real writer bug either.  The test also asserts what case 3 does NOT do: the
# loader accepts the image (correctly -- it is valid), and ordinary queries still
# answer identically, because Z3 deliberately routes no query through the trie.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# On-disk page-layout constants shared with t/003 and t/011, plus the v6
# extended-kind escape (include/weave/pagekind.h) and the surf trie image header
# (include/weave/surftrie.h, doc/specs/FUZZY_CHANNEL.md section 3.3).
use constant {
	BLCKSZ              => 8192,
	CONTENT_START       => 24,      # MAXALIGN(SizeOfPageHeaderData)
	OPAQUE_FLAGS_OFF    => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_PAGE_KIND_EXT => (1 << 15),
	WEAVE_PK_SURF       => 21,
	ST_OFF_NTERMS       => 8,       # WEAVE_ST_OFF_NTERMS
	ST_OFF_NSLOTS       => 12,      # WEAVE_ST_OFF_NSLOTS
	ST_OFF_MAXDEPTH     => 28,      # WEAVE_ST_OFF_MAXDEPTH
	ST_HDRSIZE          => 32,      # WEAVE_ST_HDRSIZE; labels[] starts here
};

# Every WEAVE_PK_SURF page in the file, in block order.  The FIRST one carries
# the 32-byte image header and the start of labels[], which is all three
# corruptions need.
sub surf_blocks
{
	my ($path) = @_;
	open(my $fh, '<:raw', $path) or die "open $path: $!";
	my $size = -s $fh;
	my @blks;

	for (my $base = 0; $base + BLCKSZ <= $size; $base += BLCKSZ)
	{
		my $buf;
		sysseek($fh, $base + OPAQUE_FLAGS_OFF, 0) or die "seek: $!";
		sysread($fh, $buf, 4) == 4 or last;
		my ($flags, $kind) = unpack('vv', $buf);
		push @blks, $base
		  if ($flags & WEAVE_PAGE_KIND_EXT) && $kind == WEAVE_PK_SURF;
	}
	close($fh) or die "close: $!";
	return @blks;
}

sub read_u32 { my ($p, $o) = @_;
	open(my $fh, '<:raw', $p) or die $!;
	sysseek($fh, $o, 0) or die $!;
	my $b; sysread($fh, $b, 4) == 4 or die "short read";
	close($fh); return unpack('V', $b); }

sub write_bytes { my ($p, $o, $bytes) = @_;
	open(my $fh, '+<:raw', $p) or die $!;
	sysseek($fh, $o, 0) or die $!;
	syswrite($fh, $bytes) == length($bytes) or die "write: $!";
	close($fh) or die $!; }

my $node = PostgreSQL::Test::Cluster->new('primary');
# Same rationale as t/003 and t/011: no page checksums, so PostgreSQL's own gate
# does not intercept the torn page before pg_weave's decoder sees it.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');
$node->safe_psql(
	'postgres', q{
	CREATE TABLE sc (id int, body text);
	INSERT INTO sc SELECT g, 'alpha beta common' || (g % 50) || ' rare' || g
		FROM generate_series(1, 800) g;
	CREATE INDEX sc_weave ON sc USING weave (to_wdoc('simple', body));
});

my $query = "SET enable_seqscan=off; "
  . "SELECT count(*) FROM sc WHERE to_wdoc('simple', body) \@\@\@ 'common1'::wquery";
my $before = $node->safe_psql('postgres', $query);
ok($before > 0, "pre-corruption: the query finds rows ($before)");
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('sc_weave', true) WHERE NOT ok}),
	'0', 'pre-corruption: every invariant holds');
cmp_ok($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_surf_stats('sc_weave')}),
	'>', 0, 'pre-corruption: the loader reads at least one trie');

# REINDEX gives the index a NEW relfilenode, so the path has to be re-read after
# every one of them.  Caching it once cost a confusing "No such file or
# directory" from the second corruption pass.
# Read the path while the server is UP, then stop it: pg_relation_filepath needs a
# connection, and asking after the shutdown is a silent "no such file" two
# corruption passes later.
sub stop_and_locate
{
	my $p = $node->data_dir . '/'
	  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('sc_weave')");
	$node->stop;
	return $p;
}
my $abs = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('sc_weave')");
ok(-f $abs, "located the index relfile: $abs");

# ---------------------------------------------------------------------------
# 1. A COUNT THAT NO SECTION LENGTH DEPENDS ON: nterms + 1.
#
#    The image length is a pure function of nslots, nnodes and nterminal -- NOT of
#    nterms -- so bumping nterms leaves the image exactly as long as its own
#    header says it should be, every popcount identity intact and every
#    accelerator table still equal to a recomputation.  open() has nothing to
#    object to and validate()'s ordinal-range test (ord < nterms) only got looser.
#
#    So this is the FIRST of the three cases that only a comparison against the
#    dictionary can see, and it is the cheapest demonstration that the two-layer
#    image validator is NOT sufficient on its own -- which is the argument for
#    weave_check() owing a cross-structure invariant at all.
# ---------------------------------------------------------------------------
$abs = stop_and_locate();
my @blks = surf_blocks($abs);
cmp_ok(scalar(@blks), '>', 0, 'located at least one WEAVE_PK_SURF page on disk');
my $hdr = $blks[0] + CONTENT_START;
my $nterms = read_u32($abs, $hdr + ST_OFF_NTERMS);
my $nslots = read_u32($abs, $hdr + ST_OFF_NSLOTS);
cmp_ok($nterms, '>', 0, "trie header is where the format says: nterms=$nterms nslots=$nslots");
write_bytes($abs, $hdr + ST_OFF_NTERMS, pack('V', $nterms + 1));
$node->start;

my ($ok, $detail) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT ok || '|' || coalesce(detail, '') FROM weave_check('sc_weave')
	   WHERE invariant = 'surf_trie_matches_dictionary'});
is($ok, 'false', 'weave_check() reports a term count no image check can see');
like($detail,
	qr/disagrees with the header's counts|mutually inconsistent|declares \d+ terms but covers/,
	'and names what it found rather than "index is corrupted"')
  or diag("detail was: $detail");

# The loader ACCEPTS this one, and recording that is the point rather than a gap:
# nterms appears in no section length (only nslots, nnodes and nterminal do), so
# the image is still exactly as long as its counts imply and open() has nothing to
# object to.  Only a comparison against the dictionary can see it -- which is
# precisely why weave_check() owes that comparison.
my ($rc, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT * FROM weave_surf_stats('sc_weave')});
is($rc, 0, 'a count that changes no section length is invisible to the loader');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss');
$node->connect_ok('dbname=postgres', 'cluster still accepts connections');

# A corrupt trie must not change an answer, because Z3 routes no query through
# the trie -- and saying so here is what would catch a future change that quietly
# started consulting it without a recheck.
is($node->safe_psql('postgres', $query), $before,
	'ordinary answers are unaffected: no query is routed through the trie yet');

$node->safe_psql('postgres', 'REINDEX INDEX sc_weave');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('sc_weave', true) WHERE NOT ok}),
	'0', 'REINDEX rebuilds a valid trie');

# ---------------------------------------------------------------------------
# 2. SEMANTIC: maxdepth := 1.  Self-consistent (1 is inside 1..255) and it changes
#    no section length, so open() cannot see it.  weave_surftrie_validate()'s DFS
#    observes the real depth and rejects.  This is the layer whose absence would
#    turn a corrupt accelerator into a wrong answer rather than an error.
# ---------------------------------------------------------------------------
$abs = stop_and_locate();
@blks = surf_blocks($abs);
cmp_ok(scalar(@blks), '>', 0, 'the rebuilt index has surf pages again');
$hdr = $blks[0] + CONTENT_START;
write_bytes($abs, $hdr + ST_OFF_MAXDEPTH, pack('v', 1));
$node->start;

($ok, $detail) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT ok || '|' || coalesce(detail, '') FROM weave_check('sc_weave')
	   WHERE invariant = 'surf_trie_matches_dictionary'});
is($ok, 'false', 'weave_check() reports the semantic corruption too');
like($detail, qr/maximum depth is invalid/, 'and names the depth rule')
  or diag("detail was: $detail");
($rc, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT * FROM weave_surf_stats('sc_weave')});
isnt($rc, 0, 'the loader refuses an image the deep validator rejects');
unlike($stderr, qr/server closed the connection unexpectedly/,
	'still a clean ERROR');

$node->safe_psql('postgres', 'REINDEX INDEX sc_weave');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('sc_weave', true) WHERE NOT ok}),
	'0', 'REINDEX recovers again');

# ---------------------------------------------------------------------------
# 3. MEMBERSHIP: the last slot's label byte := 0xFF.  Structurally PERFECT -- see
#    the header comment -- and semantically wrong: the trie now claims a term the
#    dictionary does not have, and no longer claims one it does.  The second half
#    is a dropped row.
#
#    This is the case that gives surf_trie_matches_dictionary its reason to exist.
#    If this assertion passed, the invariant would be decoration.
# ---------------------------------------------------------------------------
$abs = stop_and_locate();
@blks = surf_blocks($abs);
cmp_ok(scalar(@blks), '>', 0, 'and again after the second REINDEX');
$hdr = $blks[0] + CONTENT_START;
$nslots = read_u32($abs, $hdr + ST_OFF_NSLOTS);
cmp_ok($nslots, '>', 1, "rebuilt trie has $nslots slots");
# labels[] is nslots bytes starting at the end of the 32-byte header, and it is
# the FIRST section, so labels[nslots-1] is on the first page for any vocabulary
# whose labels fit one page -- which this one's do.  Assert that rather than
# assume it.
my $label_off = ST_HDRSIZE + $nslots - 1;
cmp_ok($label_off, '<', BLCKSZ - CONTENT_START - 8,
	'the last label byte is on the first surf page');
write_bytes($abs, $hdr + $label_off, pack('C', 0xFF));
$node->start;

($ok, $detail) = split /\|/,
  $node->safe_psql('postgres',
	q{SELECT ok || '|' || coalesce(detail, '') FROM weave_check('sc_weave')
	   WHERE invariant = 'surf_trie_matches_dictionary'});
is($ok, 'false',
	'weave_check() catches a membership divergence no structural check can see');
like($detail, qr/differ|the dictionary carries term|the trie contains a term/,
	'and says the trie and the dictionary disagree about a term')
  or diag("detail was: $detail");

# The loader ACCEPTS this image, and that is correct rather than a gap: the bytes
# are a valid trie, just not this bolt's trie.  Recording it here is the honest
# statement of what the two layers can and cannot do.
is($node->safe_psql('postgres',
		q{SELECT count(*) > 0 FROM weave_surf_stats('sc_weave')}),
	't', 'the loader accepts a structurally valid but wrong image, as designed');
is($node->safe_psql('postgres', $query), $before,
	'and answers are still correct, because no query consults the trie yet');

$node->safe_psql('postgres', 'REINDEX INDEX sc_weave');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('sc_weave', true) WHERE NOT ok}),
	'0', 'REINDEX restores the trie/dictionary agreement');
is($node->safe_psql('postgres', $query), $before,
	'and answers are unchanged throughout');

$node->stop;
done_testing();
