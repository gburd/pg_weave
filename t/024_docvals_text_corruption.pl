# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# On-disk corruption of a v3 (text, dictionary-bearing) WEAVE_PK_DOCVALS store:
# the loader must ERROR cleanly (ERRCODE_INDEX_CORRUPTED), never crash and never
# answer wrongly.  t/021_docvals_null_corruption.pl is the model and covers the
# v2 header (null_off, version); this test covers what v3 added
# (include/weave/docvals.h): the dictionary region at dict_off -- uint32 ndict,
# uint32 offs[ndict + 1], then the blob -- and the rule that every non-NULL value
# slot is an ordinal in [0, ndict).  weave_docvals_validate() is reached through
# the real loader (src/pages/docvals_page.c weave_docvals_load, "corrupt
# docvalues store in index", validator reason as DETAIL):
#
#   (a) dict_off moved (+8, and misaligned +1): the validator recomputes the one
#       legal offset, MAXALIGN(end of null bitmap or docids), and refuses others.
#   (b) offs[] broken: offs[1] above offs[2] (decreasing), and offs[ndict]
#       claiming a blob past the end of the image.
#   (c) an on-disk ordinal of a NON-NULL doc set to ndict, and to -1.  The
#       ordinal evaluator compares ordinals without looking entries up, so an
#       out-of-range ordinal is a confident wrong answer, not a crash, unless the
#       validator refuses it -- the hazard no fixed-output regression can see
#       (AGENTS.md hard rule 1).
#   (d) ndict = 0xFFFFFFFF: offs[ndict + 1] must be sized in uint64 and bounded
#       by the image before any offset is read.
#
# Each case restores the PRISTINE page bytes captured before the first case, so
# the cases are independent and no REINDEX (and hence no relfilenode change) is
# needed between them.  After the last case the pristine page is restored once
# more and the gate must answer again with a clean weave_check -- proving the
# restoration, and so that each case's failure was caused by its own mutation.
#
# Every offset is computed from the header read off the page (values_off,
# docids_off, null_off, dict_off, ndocs), never hard-coded.  The store header is
# located exactly as t/021 does: find WEAVE_PK_DOCVALS pages by opaque kind,
# then search for the "WDV1" magic.  The store is kept small (one page) so the
# whole image, dictionary included, is contiguous on that one page after the
# page header (weave_docvals_write lays it from PageGetContents).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Page-layout constants shared with t/021, plus the WeaveDocvalsHeader field
# offsets relative to the store magic (include/weave/docvals.h): magic u32 @0,
# version u16 @4, typid_kind u16 @6, ndocs u32 @8, null_off u32 @12,
# zonemap_off u32 @16, values_off u32 @20, docids_off u32 @24, dict_off u32 @28.
use constant {
	BLCKSZ              => 8192,
	OPAQUE_FLAGS_OFF    => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_PAGE_KIND_EXT => (1 << 15),
	WEAVE_PK_DOCVALS    => 25,
	DV_OFF_VERSION      => 4,
	DV_OFF_KIND         => 6,
	DV_OFF_NDOCS        => 8,
	DV_OFF_NULL_OFF     => 12,
	DV_OFF_VALUES_OFF   => 20,
	DV_OFF_DOCIDS_OFF   => 24,
	DV_OFF_DICT_OFF     => 28,
	WEAVE_DV_KIND_TEXT  => 2,
};

use constant DV_MAGIC => pack('V', 0x57445631);    # "WDV1", little-endian

sub docvals_blocks
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
		  if ($flags & WEAVE_PAGE_KIND_EXT) && $kind == WEAVE_PK_DOCVALS;
	}
	close($fh) or die "close: $!";
	return @blks;
}

# (page base, absolute header offset) of the first WEAVE_PK_DOCVALS page that
# carries the magic, or (-1, -1).
sub locate_header
{
	my ($path) = @_;
	open(my $fh, '<:raw', $path) or die "open $path: $!";
	for my $base (docvals_blocks($path))
	{
		my $buf;
		sysseek($fh, $base, 0) or die "seek: $!";
		sysread($fh, $buf, BLCKSZ) == BLCKSZ or next;
		my $pos = index($buf, DV_MAGIC);
		next if $pos < 0;
		close($fh);
		return ($base, $base + $pos);
	}
	close($fh);
	return (-1, -1);
}

sub read_bytes
{
	my ($p, $o, $n) = @_;
	open(my $fh, '<:raw', $p) or die $!;
	sysseek($fh, $o, 0) or die $!;
	my $b;
	sysread($fh, $b, $n) == $n or die "short read";
	close($fh);
	return $b;
}
sub read_u8  { return unpack('C', read_bytes($_[0], $_[1], 1)); }
sub read_u16 { return unpack('v', read_bytes($_[0], $_[1], 2)); }
sub read_u32 { return unpack('V', read_bytes($_[0], $_[1], 4)); }

sub write_bytes
{
	my ($p, $o, $bytes) = @_;
	open(my $fh, '+<:raw', $p) or die $!;
	sysseek($fh, $o, 0) or die $!;
	syswrite($fh, $bytes) == length($bytes) or die "write: $!";
	close($fh) or die $!;
}

# An int64 as two little-endian uint32 halves, so no 64-bit-perl 'q' is needed.
sub pack_i64
{
	my ($v) = @_;
	return pack('VV', 0xFFFFFFFF, 0xFFFFFFFF) if $v == -1;
	return pack('VV', $v, 0);
}

my $node = PostgreSQL::Test::Cluster->new('primary');
# No page checksums: PostgreSQL's own checksum gate must not intercept the
# rewritten page before pg_weave's validator sees it.  PG18 enables checksums by
# default (AGENTS.md); PG17 ignores the key.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# 100 rows, 10 distinct non-empty "C"-collated strings m00..m09, NULL on every
# 7th row: a v3 store with a null bitmap and a 10-entry dictionary, small enough
# for one page.  Built by CREATE INDEX over pre-inserted rows, so the store is a
# flushed build-segment store on disk.  Shape from sql/docvals.sql section 10.
$node->safe_psql('postgres', q{
	CREATE TABLE dvt (id int, body wdoc, cat text COLLATE "C");
	INSERT INTO dvt
	SELECT g, to_wdoc('common doc ' || (g % 5)),
	       CASE WHEN g % 7 = 0 THEN NULL
	            ELSE 'm' || lpad((g % 10)::text, 2, '0') END
	  FROM generate_series(1, 100) g;
	CREATE INDEX dvt_w ON dvt USING weave (body wdoc_lex_ops, cat text_docval_ops);
	ANALYZE dvt;
});

# The gate that drives the loader.  Always carries the cat qual: under
# enable_seqscan=off a qual-less count(*) errors "a weave index scan requires a
# query" on PG18.
my $query = "SET enable_seqscan=off; SET enable_bitmapscan=off; "
  . "SELECT count(*) FROM dvt WHERE cat < 'm05'";
my $heap_count = $node->safe_psql('postgres',
	"SELECT count(*) FROM dvt WHERE cat < 'm05'");
my $before = $node->safe_psql('postgres', $query);
ok($before > 0, "pre-corruption: the text gate finds rows through the store ($before)");
is($before, $heap_count, 'pre-corruption: the index answer equals the heap answer');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('dvt_w', true) WHERE NOT ok}),
	'0', 'pre-corruption: weave_check deep reports every invariant holds');

# No REINDEX happens in this test, so the relfilenode and path are stable.
my $path = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('dvt_w')");
ok(-f $path, "located the index relfile: $path");

$node->stop;

my ($pagebase, $hdr) = locate_header($path);
cmp_ok($hdr, '>=', 0, 'located the WDV1 store header on a WEAVE_PK_DOCVALS page');
my $pristine = read_bytes($path, $pagebase, BLCKSZ);

# The header, read off the page.
my %H = (
	version    => read_u16($path, $hdr + DV_OFF_VERSION),
	kind       => read_u16($path, $hdr + DV_OFF_KIND),
	ndocs      => read_u32($path, $hdr + DV_OFF_NDOCS),
	null_off   => read_u32($path, $hdr + DV_OFF_NULL_OFF),
	values_off => read_u32($path, $hdr + DV_OFF_VALUES_OFF),
	docids_off => read_u32($path, $hdr + DV_OFF_DOCIDS_OFF),
	dict_off   => read_u32($path, $hdr + DV_OFF_DICT_OFF),
);
is($H{version}, 3, "the store is v3 (version=$H{version})");
is($H{kind}, WEAVE_DV_KIND_TEXT, "the store's value kind is text ($H{kind})");
is($H{ndocs}, 100, "the store covers every row (ndocs=$H{ndocs})");
cmp_ok($H{null_off}, '>', 0, "the null bitmap is present (null_off=$H{null_off})");
cmp_ok($H{dict_off}, '>', 0, "the dictionary is present (dict_off=$H{dict_off})");

my $ndict = read_u32($path, $hdr + $H{dict_off});
is($ndict, 10, "the dictionary holds the 10 distinct strings (ndict=$ndict)");
my $offs_base = $hdr + $H{dict_off} + 4;           # absolute offset of offs[0]
my @offs = map { read_u32($path, $offs_base + 4 * $_) } (0 .. $ndict);
my $blob_end = $offs_base + 4 * ($ndict + 1) + $offs[$ndict];
cmp_ok($blob_end, '<=', $pagebase + OPAQUE_FLAGS_OFF,
	'the whole store, dictionary blob included, lies on the one located page');
is($offs[0], 0, 'offs[0] is 0 as written');
cmp_ok($offs[2], '>', $offs[1], 'offs[] strictly increases over non-empty entries');

# A non-NULL dense docid: its null bit must be clear.
my $victim = -1;
for my $i (0 .. $H{ndocs} - 1)
{
	my $byte = read_u8($path, $hdr + $H{null_off} + ($i >> 3));
	if ((($byte >> ($i & 7)) & 1) == 0) { $victim = $i; last; }
}
cmp_ok($victim, '>=', 0, "found a non-NULL doc for the ordinal cases (dense id $victim)");
my $victim_slot = $hdr + $H{values_off} + 8 * $victim;
my ($ord_lo, $ord_hi) = unpack('VV', read_bytes($path, $victim_slot, 8));
ok($ord_hi == 0 && $ord_lo < $ndict,
	"the victim's on-disk ordinal is in range before corruption ($ord_lo)");

$node->start;

# One corruption pass: restore the pristine page, apply the mutation, restart,
# and assert a clean ERROR naming the corrupt store (with the validator's reason
# as DETAIL), no wrong count, and a healthy backend/postmaster afterwards.
sub corrupt_case
{
	my ($label, $reason, $mutate) = @_;

	$node->stop;
	write_bytes($path, $pagebase, $pristine);
	$mutate->();
	$node->start;

	my ($rc, $stdout, $stderr) = $node->psql('postgres', $query);
	isnt($rc, 0, "$label: the gate over the corrupt store fails the query");
	unlike($stderr,
		qr/server closed the connection unexpectedly|terminating connection/,
		"$label: no connection loss (clean ERROR, not a crash)");
	like($stderr, qr/corrupt docvalues store/,
		"$label: the error names the corrupt docvalues store")
	  or diag("stderr was: $stderr");
	like($stderr, $reason, "$label: the validator's reason is reported")
	  or diag("stderr was: $stderr");
	is($stdout, '', "$label: no (wrong) count was returned");

	$node->connect_ok('dbname=postgres', "$label: cluster still accepts connections");
	is($node->safe_psql('postgres', 'SELECT 1'), 1,
		"$label: backend healthy after the corrupt-store scan");
	return;
}

my $dict_off_abs = $hdr + DV_OFF_DICT_OFF;
my $bad_dict = qr/dict_off is not the aligned post-docids\/bitmap offset/;

# (a) dict_off moved by one aligned word, and misaligned by one byte.
corrupt_case('dict_off +8', $bad_dict, sub {
	write_bytes($path, $dict_off_abs, pack('V', $H{dict_off} + 8));
});
corrupt_case('dict_off +1 (misaligned)', $bad_dict, sub {
	write_bytes($path, $dict_off_abs, pack('V', $H{dict_off} + 1));
});

# (b) offs[1] raised above offs[2]: a decreasing pair, which would hand a reader
#     a wrapped (huge) entry length.
corrupt_case('offs[1] > offs[2] (decreasing)',
	qr/dictionary offsets are not non-decreasing/, sub {
	write_bytes($path, $offs_base + 4, pack('V', $offs[2] + 1));
});

# (b') offs[ndict] claims a blob far past the end of the image.  Still
#     non-decreasing, so only the blob-length bound can catch it.
corrupt_case('offs[ndict] past the blob end',
	qr/image too short for the dictionary blob/, sub {
	write_bytes($path, $offs_base + 4 * $ndict, pack('V', 0x7FFFFFFF));
});

# (c) the non-NULL victim's ordinal set to ndict (one past the last entry) ...
my $bad_ord = qr/dictionary ordinal out of range/;
corrupt_case('ordinal == ndict', $bad_ord, sub {
	write_bytes($path, $victim_slot, pack_i64($ndict));
});

# ... and to -1, which the ordinal evaluator would treat as below every boundary.
corrupt_case('ordinal == -1', $bad_ord, sub {
	write_bytes($path, $victim_slot, pack_i64(-1));
});

# (d) ndict = 0xFFFFFFFF: offs[ndict + 1] would span 16 GiB; must be bounded in
#     uint64 against the image before any offset is read.
corrupt_case('ndict = 0xFFFFFFFF',
	qr/image too short for the dictionary offsets/, sub {
	write_bytes($path, $hdr + $H{dict_off}, pack('V', 0xFFFFFFFF));
});

# Final restoration: the pristine page answers again, so every case above
# failed because of its own mutation and nothing leaked between them.
$node->stop;
write_bytes($path, $pagebase, $pristine);
is(read_bytes($path, $pagebase, BLCKSZ), $pristine,
	'restored the pristine docvalues page bytes');
$node->start;

is($node->safe_psql('postgres', $query), $before,
	'after restoration: the text gate answers the pre-corruption count again');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('dvt_w', true) WHERE NOT ok}),
	'0', 'after restoration: weave_check deep reports every invariant holds');

$node->stop;
done_testing();
