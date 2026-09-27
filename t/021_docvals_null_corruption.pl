# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# On-disk corruption of a v2 (null-bearing) WEAVE_PK_DOCVALS store: the loader
# must ERROR cleanly (ERRCODE_INDEX_CORRUPTED), never crash and never answer
# wrongly (docvals-nulls slice, Task 6, the torn-write gate; PRODUCTION_READINESS
# gate 8 specialised to the v2 null bitmap).
#
# t/013_surf_corruption.pl and t/011_chandesc_corruption.pl are the models: find
# the on-disk page of a given kind, rewrite header bytes behind the server's
# back, and assert the real backend call chain turns the corruption into a clean
# ERROR.  What is specific here is the NULL bitmap header the v2 store grew
# (include/weave/docvals.h): a v2 store carries a nonzero null_off and a version
# of 2, and the pure validator weave_docvals_validate() has three teeth this test
# reaches through the loader (src/pages/docvals_page.c, "corrupt docvalues store
# in index"):
#
#   (a) null_off PAST THE STORE END -- the bitmap would be read out of bounds.
#       The validator computes null_off + ceil(ndocs/8) in uint64 and refuses.
#   (b) an UNKNOWN version (3) -- doc/CONVENTIONS.md decision 3: a best-effort
#       read of a format we do not understand is worse than refusing.
#   (c) version forced to 1 while null_off is nonzero -- the "v1 store with a
#       bitmap" contradiction.  A v1 store predates the bitmap and MUST have
#       null_off == 0; a nonzero null_off on a v1 store is exactly the torn/mixed
#       image the Review Focus calls out, and the validator rejects it rather
#       than silently reading a bitmap a v1 reader would never look for.
#
# WHAT WOULD BE MISSED WITHOUT IT.  A validator that trusted null_off would read
# the bitmap off the end of the image (case a); one that accepted any version
# would misread a future format (b); one that did not cross-check version against
# null_off would read a bitmap that the store does not really have (c) -- and
# because a NULL that is wrongly treated as non-null (or vice versa) is a
# wrong-but-plausible answer, no fixed-expected-output regression test can catch
# any of the three (AGENTS.md hard rule 1).
#
# APPROACH FOR LOCATING THE HEADER.  The store header sits at the payload start
# of the WEAVE_PK_DOCVALS page, after the page header and any weave page-entry
# framing.  Rather than hard-code that in-page offset, this test uses the ROBUST
# alternative the plan blesses: it locates the WEAVE_PK_DOCVALS page by its
# opaque kind (exactly as t/013 locates a WEAVE_PK_SURF page), then SEARCHES that
# page for the store magic ("WDV1", stored little-endian as the 4 bytes of
# 0x57445631) and rewrites version (offset +4, uint16) and null_off (offset +12,
# uint32) at their fixed offsets from the magic.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# On-disk page-layout constants shared with t/003/t/011/t/013 (the extended-kind
# escape, pagekind.h), plus the WeaveDocvalsHeader field offsets relative to the
# store magic (include/weave/docvals.h): magic uint32 @0, version uint16 @4,
# ndocs uint32 @8, null_off uint32 @12.
use constant {
	BLCKSZ              => 8192,
	OPAQUE_FLAGS_OFF    => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_PAGE_KIND_EXT => (1 << 15),
	WEAVE_PK_DOCVALS    => 25,
	DV_OFF_VERSION      => 4,       # offsetof(WeaveDocvalsHeader, version)
	DV_OFF_NULL_OFF     => 12,      # offsetof(WeaveDocvalsHeader, null_off)
};

# The store magic "WDV1" as it appears on disk: the uint32 0x57445631 in
# little-endian byte order (0x31,0x56,0x44,0x57).  Searched for verbatim.
use constant DV_MAGIC => pack('V', 0x57445631);

# Every WEAVE_PK_DOCVALS page in the file, in block order (same scan as t/013's
# surf_blocks).  A build segment's docvalues store is one nextblk-linked chain;
# its ROOT page carries the header, so the header magic is on one of these pages.
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

# Absolute file offset of the store header on the first WEAVE_PK_DOCVALS page
# that carries the magic, or -1.  Reads the whole page and searches it, so the
# exact in-page framing offset never has to be hard-coded.
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
		return $base + $pos;
	}
	close($fh);
	return -1;
}

sub read_u16 { my ($p, $o) = @_;
	open(my $fh, '<:raw', $p) or die $!;
	sysseek($fh, $o, 0) or die $!;
	my $b; sysread($fh, $b, 2) == 2 or die "short read";
	close($fh); return unpack('v', $b); }

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
# Same rationale as t/011/t/013: no page checksums, so PostgreSQL's own
# page-checksum gate does not intercept the torn page before pg_weave's decoder
# sees it.  PG18 turns checksums on by default (AGENTS.md); PG17 ignores the key.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# A nullable-facet docvals index that HAS at least one NULL, so the store is v2
# (version 2, null_off != 0).  Built by CREATE INDEX over pre-inserted rows, so
# the store is a FLUSHED build-segment store, not a pending item -- the thing on
# disk this test corrupts.  Column shapes from sql/docvals.sql section 9.
$node->safe_psql('postgres', q{
	CREATE TABLE dvc (id int, body wdoc, price bigint);
	INSERT INTO dvc
	SELECT g, to_wdoc('common doc ' || (g % 5)),
	       CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 100)::bigint END
	  FROM generate_series(1, 500) g;
	CREATE INDEX dvc_w ON dvc USING weave (body, price int8_docval_ops);
	ANALYZE dvc;
});

# The gate that drives the loader: forced through the index (enable_seqscan and
# enable_bitmapscan off, as sql/docvals.sql section 9), a comparison reads the
# docvalues store -- so a corrupt store is discovered here, on an ordinary query.
my $query = "SET enable_seqscan=off; SET enable_bitmapscan=off; "
  . "SELECT count(*) FROM dvc WHERE price < 50";
my $before = $node->safe_psql('postgres', $query);
ok($before > 0, "pre-corruption: the gate finds rows through the store ($before)");
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('dvc_w', true) WHERE NOT ok}),
	'0', 'pre-corruption: every invariant holds');

# Read the path while the server is UP, then stop it -- pg_relation_filepath
# needs a connection, and REINDEX gives a NEW relfilenode, so the path is
# re-read after every corruption pass (the t/013 lesson).
sub stop_and_locate
{
	my $p = $node->data_dir . '/'
	  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('dvc_w')");
	$node->stop;
	return $p;
}

my $abs = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('dvc_w')");
ok(-f $abs, "located the index relfile: $abs");
{
	$node->stop;
	my $hdr = locate_header($abs);
	cmp_ok($hdr, '>=', 0, 'located the WDV1 store header on a WEAVE_PK_DOCVALS page');
	my $ver = read_u16($abs, $hdr + DV_OFF_VERSION);
	my $noff = read_u32($abs, $hdr + DV_OFF_NULL_OFF);
	is($ver, 2, "the store is v2 (version=$ver) because it carries NULLs");
	cmp_ok($noff, '>', 0, "the null bitmap is present (null_off=$noff != 0)");
	$node->start;
}

# A corruption pass: rewrite the header, restart, assert the gate ERRORs cleanly
# with ERRCODE_INDEX_CORRUPTED, the postmaster survives a follow-up query, and no
# wrong count is returned; then REINDEX back to a clean store.
sub corrupt_case
{
	my ($label, $mutate) = @_;

	my $path = stop_and_locate();
	my $hdr = locate_header($path);
	cmp_ok($hdr, '>=', 0, "$label: found the store header again");
	$mutate->($path, $hdr);
	$node->start;

	my ($rc, $stdout, $stderr) = $node->psql('postgres', $query);
	isnt($rc, 0, "$label: the gate over the corrupt store fails the query");
	unlike($stderr,
		qr/server closed the connection unexpectedly|terminating connection/,
		"$label: no connection loss (clean ERROR, not a crash)");
	like($stderr, qr/corrupt docvalues store/,
		"$label: the error names the corrupt docvalues store")
	  or diag("stderr was: $stderr");
	is($stdout, '', "$label: no (wrong) count was returned");

	# The cluster is fully alive afterwards.
	$node->connect_ok('dbname=postgres', "$label: cluster still accepts connections");
	is($node->safe_psql('postgres', 'SELECT 1'), 1,
		"$label: backend healthy after the corrupt-store scan");

	# REINDEX rebuilds a clean v2 store from the heap and restores answers.
	$node->safe_psql('postgres', 'REINDEX INDEX dvc_w');
	is($node->safe_psql('postgres',
			q{SELECT count(*) FROM weave_check('dvc_w', true) WHERE NOT ok}),
		'0', "$label: REINDEX rebuilds a valid store");
	is($node->safe_psql('postgres', $query), $before,
		"$label: REINDEX restores the correct gate answer");
	return;
}

# (a) null_off past the store end: 0xFFFFFFFF is not the one legal aligned
#     post-docids offset, so the validator refuses at the offset-equality check
#     (before the length bound it would also fail).
corrupt_case('null_off past end', sub {
	my ($path, $hdr) = @_;
	write_bytes($path, $hdr + DV_OFF_NULL_OFF, pack('V', 0xFFFFFFFF));
});

# (b) an unknown store version (3): the validator accepts 1 and 2 only.
corrupt_case('unknown version 3', sub {
	my ($path, $hdr) = @_;
	write_bytes($path, $hdr + DV_OFF_VERSION, pack('v', 3));
});

# (c) version forced to 1 while null_off is nonzero: the "v1 store with a bitmap"
#     contradiction.  null_off is still the real v2 value written at build time;
#     only the version byte moves, so this is precisely the mixed image the
#     validator's "version 1 requires null_off == 0" rule exists to reject.
corrupt_case('v1 version with a bitmap', sub {
	my ($path, $hdr) = @_;
	write_bytes($path, $hdr + DV_OFF_VERSION, pack('v', 1));
});

$node->stop;
done_testing();
