# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# On-disk corruption of a v6 WEAVE_CHANDESC page must produce a clean ERROR,
# not a WARNING-and-skip like t/003_corruption.pl's posting/pending cases.
#
# This is the difference doc/specs/SEGMENT_FORMAT.md sect. 8 item 2 draws on
# purpose: a corrupt posting/pending page is a bounded miss because dropping a
# few postings is a wrong-but-safe answer the reader can recover from; a
# corrupt channel descriptor is a structural claim about WHERE a bolt's data
# lives (nweft indexes an array, each weft's root becomes a ReadBuffer()
# argument), so a best-effort read would either misplace a weft's root or leak
# a page.  doc/CONVENTIONS.md decision 2 (on-disk bytes are not trusted, a
# corrupt page produces a clean ERROR) says refuse instead.
#
# The real production path exercised here is weave_read_meta() ->
# weave_chandesc_required() (include/weave/am.h) -> weave_read_chandesc()
# (src/am/am.c) -> weave_chandesc_check() (include/weave/chandesc.h). Before
# this fix weave_chandesc_required() had zero callers, so this ERROR path was
# reachable only via a direct C call nothing in the tree made -- see the
# review item fixed alongside this test. weave_read_meta() now validates every
# live bolt's descriptor page on every read of the segment directory, which
# is every scan, build, merge and vacuum, so an ordinary SELECT is enough to
# reach it.
#
# fuzz_chandesc.c already proves weave_chandesc_check() itself never
# crashes/overflows on any input and that its planted-bug variant
# (FUZZ_NO_ARRAY_GUARD) is caught under ASan; this test is the end-to-end
# proof that a corrupt page reaching that same validator through the real
# call chain turns into a clean backend-surviving ERROR, not a crash and not
# a silently wrong answer.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# On-disk page-layout constants shared with t/003_corruption.pl, plus the v6
# extended-kind escape (pagekind.h) and the WeaveChanDescPageData header
# (include/weave/am.h) this test corrupts.
use constant {
	BLCKSZ            => 8192,
	CONTENT_START     => 24,      # MAXALIGN(SizeOfPageHeaderData)
	PD_LOWER_OFF      => 12,      # offset of pd_lower (uint16) in PageHeaderData
	OPAQUE_FLAGS_OFF  => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_PAGE_KIND_EXT => (1 << 15),
	WEAVE_PK_CHANDESC   => 16,
	CD_NWEFT_OFF        => 6,     # offsetof(WeaveChanDescPageData, nweft)
};

# Find every WEAVE_CHANDESC page (extended-kind escape bit set, opaque.kind ==
# WEAVE_PK_CHANDESC) and overwrite its nweft field with a value above
# WEAVE_MAX_WEFTS (32) -- the exact planted bug fuzz_chandesc.c's
# FUZZ_NO_ARRAY_GUARD variant models, here reaching the validator through the
# real backend call chain instead of a direct call. Returns the number of
# pages hit.
sub corrupt_chandesc_pages
{
	my ($path) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	my $size = -s $fh;
	my $hits = 0;

	for (my $base = 0; $base + BLCKSZ <= $size; $base += BLCKSZ)
	{
		my $buf;
		sysseek($fh, $base + OPAQUE_FLAGS_OFF, 0) or die "seek: $!";
		sysread($fh, $buf, 4) == 4 or last;
		my ($flags, $kind) = unpack('vv', $buf);

		next unless ($flags & WEAVE_PAGE_KIND_EXT) && $kind == WEAVE_PK_CHANDESC;

		sysseek($fh, $base + PD_LOWER_OFF, 0) or die "seek: $!";
		sysread($fh, $buf, 2) == 2 or last;
		my $pd_lower = unpack('v', $buf);
		next if $pd_lower <= CONTENT_START + CD_NWEFT_OFF + 1;

		sysseek($fh, $base + CONTENT_START + CD_NWEFT_OFF, 0) or die "seek: $!";
		syswrite($fh, pack('v', 0xFFFF)) == 2 or die "write: $!";
		$hits++;
	}
	close($fh) or die "close $path: $!";
	return $hits;
}

my $node = PostgreSQL::Test::Cluster->new('primary');
# Same rationale as t/003_corruption.pl: disable checksums so PG's own
# page-checksum gate does not intercept the torn page before pg_weave's
# decoder sees it, and fsync off because durability is not what is under test.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

$node->safe_psql(
	'postgres', q{
	CREATE TABLE cc (id int, body text);
	INSERT INTO cc SELECT g, 'alpha beta common' || (g % 50)
		FROM generate_series(1, 500) g;
	CREATE INDEX cc_weave ON cc USING weave (to_wdoc('simple', body));
});

my $q = "SET enable_seqscan=off;";
my $before = $node->safe_psql('postgres',
	"$q SELECT count(*) FROM cc WHERE to_wdoc('simple', body) \@\@\@ 'common1'::wquery");
ok($before > 0, "pre-corruption: query finds rows ($before)");

my $relpath = $node->safe_psql('postgres',
	"SELECT pg_relation_filepath('cc_weave')");
ok(defined $relpath && length $relpath, "located index relfile: $relpath");

$node->stop;
my $abs = $node->data_dir . '/' . $relpath;
ok(-f $abs, "index file exists on disk: $abs");
my $hits = corrupt_chandesc_pages($abs);
ok($hits > 0, "corrupted at least one WEAVE_CHANDESC page; hit $hits");
$node->start;

# The crux: a scan must ERROR cleanly (ERRCODE_INDEX_CORRUPTED via
# weave_chandesc_required()), not crash the backend and not return a
# silently-wrong answer.
my ($rc, $stdout, $stderr) = $node->psql('postgres',
	"$q SELECT count(*) FROM cc WHERE to_wdoc('simple', body) \@\@\@ 'common1'::wquery");
isnt($rc, 0, 'scan over a corrupted channel-descriptor page fails the query');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss on corrupted-chandesc scan');
like($stderr, qr/corrupt channel-descriptor page/,
	'error names the corrupt channel-descriptor page');
like($stderr, qr/weft count is zero or above the maximum|REINDEX the index to rebuild it/,
	'error carries the specific structural-rule detail, not a generic message')
	or diag("stderr was: $stderr");

# The cluster is still fully alive afterwards.
$node->connect_ok('dbname=postgres', 'cluster still accepts connections after corruption');
my $alive = $node->safe_psql('postgres', 'SELECT 1');
is($alive, 1, 'backend healthy after the corrupted-chandesc scan');

# REINDEX rebuilds a clean descriptor page from the heap and restores
# correct answers, proving the corruption is recoverable, not terminal --
# the same contract weave_check_meta()'s errhint promises.
$node->safe_psql('postgres', 'REINDEX INDEX cc_weave');
my $after = $node->safe_psql('postgres',
	"$q SELECT count(*) FROM cc WHERE to_wdoc('simple', body) \@\@\@ 'common1'::wquery");
is($after, $before, 'REINDEX restores correct answers after chandesc corruption');

$node->stop;
done_testing();
