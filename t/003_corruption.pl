# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# On-disk corruption / torn-page crash-safety for the pg_weave weave index.
#
# A corrupt index block is a BOUNDED MISS, not a crash.  The readers cast raw
# on-disk page bytes to their C structs and then trust the fields inside: the
# segment posting decoder (weave_decode_term) reads a posting block header's
# `count` and byte lengths, and the pending-list scan/flush cast a pending
# page to an WeaveDoc and walk its term offsets.  A torn page, a stale-format
# image, or a producing bug could put a `count` > WEAVE_BLOCK_SIZE (a stack
# overflow of the fixed 128-element decode arrays) or a bytelen/posbytelen that
# runs the FOR columns past the page (an out-of-bounds read) or a malformed
# pending document (a wild memcpy in add_posting / an OOB read in
# weave_doc_matches).  The 0.3.3/0.3.4 fixes clamp/validate these at the trust
# boundary: a corrupt block or pending doc is skipped with a WARNING and the
# backend SURVIVES.
#
# This test reproduces the reporter's crash directly -- it writes bad bytes
# into the index file with the server down, restarts, and asserts that scans
# and VACUUM over the corrupted index do NOT crash the backend (the connection
# stays alive, the cluster stays up, queries still answer).  It is the pg_weave
# analogue of libxtc's deterministic torn-write fault injection: a corrupt
# input must be a WARNING and a bounded miss, never a segfault.
#
# We inject two kinds of corruption, so the test keeps its teeth even if the
# exact on-disk offset of a header drifts:
#   * TARGETED: overwrite a posting block header's first uint32 (`count`) with
#     0xFFFF (>> WEAVE_BLOCK_SIZE) -- exercises the 0.3.4 count clamp precisely.
#   * COARSE: 0xFF-smash a run of bytes in a posting page's used content area
#     (after the page header, before pd_lower) -- guaranteed to hit real FOR
#     column bytes / block-header byte-length fields and exercise the
#     past-the-page rejection and general decode robustness.
# Plus a smashed PENDING page (exercises weave_doc_is_valid on the scan + flush
# paths).  The PRIMARY assertion is always "no crash": every query after
# corruption must return through a live connection, and VACUUM must complete.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# On-disk page-layout constants for the weave AM (pg_weave_am.h + PG bufpage).
#   BLCKSZ default build (8 KB); MAXALIGN(SizeOfPageHeaderData) = 24 is where a
#   page's contents (and thus the first posting block header / pending item)
#   begin; the page-special WeavePageOpaqueData (flags:uint16) sits at
#   BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData)) = 8192 - 8 = 8184; pd_lower
#   (uint16) at page-header offset 12 marks the end of the used content area.
use constant {
	BLCKSZ            => 8192,
	CONTENT_START     => 24,      # MAXALIGN(SizeOfPageHeaderData)
	PD_LOWER_OFF      => 12,      # offset of pd_lower (uint16) in PageHeaderData
	OPAQUE_FLAGS_OFF  => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_POSTING      => (1 << 2),
	WEAVE_PENDING      => (1 << 3),
	WEAVE_BLOCK_SIZE   => 128,
};

# Corrupt the on-disk index file (server must be DOWN).  Walks every 8 KB page,
# identifies posting/pending pages by their opaque flags, and injects the
# targeted count-overwrite + coarse content smash described above.  Returns a
# count of (posting pages hit, pending pages hit) so the caller can assert we
# actually found something to corrupt.
sub corrupt_index_file
{
	my ($path) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	my $size = -s $fh;
	my ($posting_hits, $pending_hits) = (0, 0);

	for (my $base = 0; $base + BLCKSZ <= $size; $base += BLCKSZ)
	{
		# read the opaque flags and pd_lower for this page
		my $buf;
		sysseek($fh, $base + OPAQUE_FLAGS_OFF, 0) or die "seek: $!";
		sysread($fh, $buf, 2) == 2 or last;
		my $flags = unpack('v', $buf);

		sysseek($fh, $base + PD_LOWER_OFF, 0) or die "seek: $!";
		sysread($fh, $buf, 2) == 2 or last;
		my $pd_lower = unpack('v', $buf);

		# only touch used content; nothing to smash on an empty page
		next if $pd_lower <= CONTENT_START;
		my $content_len = $pd_lower - CONTENT_START;

		if ($flags & WEAVE_POSTING)
		{
			# TARGETED: first uint32 at content start is the first block's
			# `count`; set it far above WEAVE_BLOCK_SIZE to drive the clamp.
			sysseek($fh, $base + CONTENT_START, 0) or die "seek: $!";
			syswrite($fh, pack('V', 0xFFFF)) == 4 or die "write: $!";

			# COARSE: 0xFF a run in the middle of the used content -- guaranteed
			# to hit FOR-column bytes and later block headers' byte lengths.
			my $smash_off = CONTENT_START + int($content_len / 4);
			my $smash_len = int($content_len / 2);
			$smash_len = 64 if $smash_len > 64;    # keep it a torn *region*, not the whole page
			if ($smash_len > 0)
			{
				sysseek($fh, $base + $smash_off, 0) or die "seek: $!";
				syswrite($fh, ("\xFF" x $smash_len)) == $smash_len
				  or die "write: $!";
			}
			$posting_hits++;
		}
		elsif ($flags & WEAVE_PENDING)
		{
			# Smash the pending item's wdoc bytes (after the item header) so
			# weave_doc_is_valid rejects it on both the scan and the flush path.
			my $smash_off = CONTENT_START + 8;     # past tid+doclen of first item
			my $smash_len = $content_len - 8;
			$smash_len = 96 if $smash_len > 96;
			if ($smash_len > 0)
			{
				sysseek($fh, $base + $smash_off, 0) or die "seek: $!";
				syswrite($fh, ("\xFF" x $smash_len)) == $smash_len
				  or die "write: $!";
			}
			$pending_hits++;
		}
	}
	close($fh) or die "close $path: $!";
	return ($posting_hits, $pending_hits);
}

my $node = PostgreSQL::Test::Cluster->new('primary');
# no_data_checksums: this test injects torn bytes to exercise pg_weave's OWN
# decode-path hardening (weave_decode_term / weave_doc_is_valid cast raw on-disk
# bytes to C structs and must survive).  PostgreSQL 18's initdb turns data
# checksums ON by default (PG<=17 defaulted them OFF); with checksums on, the
# smashed page fails PG's page-checksum verification in ReadBuffer and the read
# ERRORs ("invalid page in block N") BEFORE pg_weave's decoder ever sees the
# bytes -- so the query aborts (rc!=0) instead of returning a bounded-miss
# count, hiding the very code path this test targets.  Disabling checksums lets
# the torn bytes reach pg_weave's decoder exactly as they do on PG17, which is
# what these assertions were written to verify.  (no_data_checksums is a PG18+
# init param; PG<=17's harness ignores the unknown key -- checksums are already
# off there.)  PG's page-checksum gate is a separate defense layer; pg_weave's
# in-decoder hardening is what must hold when a torn page does reach it.
$node->init(no_data_checksums => 1);
# fsync off: the test never relies on durability, only on the page bytes we
# write while the server is down being what the server reads on restart.
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# Build an index whose posting lists span MANY 128-doc blocks: 'common' is in
# every one of 4000 rows (df=4000 -> ~32 blocks) and 'alpha beta' is a phrase
# so positions are decoded across block boundaries.  positions=on so the
# posbytelen path is exercised too.  VACUUM + weave_merge flush the pending
# write buffer into on-disk segment postings, which is what weave_decode_term
# reads.
$node->safe_psql(
	'postgres', q{
	CREATE TABLE docs (id int, body text);
	CREATE INDEX docs_weave ON docs USING weave (to_wdoc('simple', body))
		WITH (positions = on);
	INSERT INTO docs
		SELECT g, 'alpha beta common tok' || (g % 200)
		FROM generate_series(1, 4000) g;
	VACUUM docs;
	SELECT weave_merge('docs_weave');
});

# Add rows AFTER the merge so they land in the pending write buffer (a pending
# page on disk), to be corrupted and exercise weave_doc_is_valid.
$node->safe_psql(
	'postgres', q{
	INSERT INTO docs
		SELECT g, 'gamma delta pending doc ' || g
		FROM generate_series(9000, 9200) g;
});

my $q = "SET enable_seqscan=off;";

# Pre-corruption sanity: the index answers, and a whole-corpus term exists.
my $common_before = $node->safe_psql('postgres',
	"$q SELECT count(*) FROM docs WHERE to_wdoc('simple', body) \@\@\@ 'common'::wquery");
is($common_before, 4000, 'pre-corruption: common found in all 4000 built rows');

my $relpath = $node->safe_psql('postgres',
	"SELECT pg_relation_filepath('docs_weave')");
ok(defined $relpath && length $relpath, "located index relfile: $relpath");

# Corrupt with the server DOWN, then bring it back up: recovery replays no
# WAL for our hand-written bytes, so the server reads exactly the torn image.
$node->stop;
my $abs = $node->data_dir . '/' . $relpath;
ok(-f $abs, "index file exists on disk: $abs");
my ($ph, $pn) = corrupt_index_file($abs);
ok($ph > 0, "corrupted at least one posting page (targeted + coarse); hit $ph");
# pending pages may or may not be present depending on flush timing; report it.
diag("corrupted $ph posting page(s), $pn pending page(s)");
$node->start;

# ---- The crux: the backend must SURVIVE scans/VACUUM over the torn index. ----

# A scan over the corrupted posting lists must return through a LIVE
# connection -- not "server closed the connection unexpectedly".  We do not
# assert the count value (a corrupt block is a bounded miss and may drop
# postings); we assert the query completes without crashing the backend.
my ($rc, $stdout, $stderr) = $node->psql('postgres',
	"$q SELECT count(*) FROM docs WHERE to_wdoc('simple', body) \@\@\@ 'common'::wquery");
is($rc, 0, 'scan over corrupted posting blocks did not crash the backend');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss on corrupted-posting scan');
like($stdout, qr/^\d+$/, 'corrupted-posting scan returned a sane integer count');

# A phrase scan decodes positions across block boundaries -- the posbytelen
# path -- over the same corrupted blocks.  Must also survive.
($rc, $stdout, $stderr) = $node->psql('postgres',
	"$q SELECT count(*) FROM docs WHERE to_wdoc('simple', body) \@\@\@ '\"alpha beta\"'::wquery");
is($rc, 0, 'phrase scan (positions decode) over corrupted blocks did not crash');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss on corrupted-posting phrase scan');

# A scan that must read the corrupted PENDING page (gamma/delta live only in
# the pending buffer) must skip the malformed doc, not segfault.
($rc, $stdout, $stderr) = $node->psql('postgres',
	"$q SELECT count(*) FROM docs WHERE to_wdoc('simple', body) \@\@\@ 'gamma'::wquery");
is($rc, 0, 'scan over corrupted pending page did not crash the backend');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss on corrupted-pending scan');

# VACUUM triggers a pending-list FLUSH (weave_flush_pending) and touches the
# posting decoder -- the reporter's other crash path.  Must complete.
($rc, $stdout, $stderr) = $node->psql('postgres', 'VACUUM docs');
is($rc, 0, 'VACUUM (pending flush + decode) over corrupted index did not crash');
unlike($stderr, qr/server closed the connection unexpectedly|terminating connection/,
	'no connection loss during VACUUM of corrupted index');

# The cluster is still fully alive and can serve an ordinary connection and a
# normal (non-corrupt) query afterwards.
$node->connect_ok('dbname=postgres', 'cluster still accepts connections after corruption');
my $alive = $node->safe_psql('postgres', 'SELECT 1');
is($alive, 1, 'backend healthy after corruption + scans + VACUUM');

# Corruption should have been REPORTED (not silently), matching the fix's
# WARNING contract.  We accept either the segment-decode or the pending-doc
# warning (which one fires depends on which corrupted page a query reaches).
# This is a soft check: the hard guarantee is "no crash" above.
my $log = slurp_file($node->logfile);
my $warned = ($log =~ /truncated posting block/)
	|| ($log =~ /skipping malformed pending document/);
ok($warned, 'server logged a corruption WARNING (truncated posting block / malformed pending document)');

# REINDEX must rebuild a clean index from the heap and restore correct answers,
# proving corruption is recoverable, not terminal.
$node->safe_psql('postgres', 'REINDEX INDEX docs_weave');
my $common_after = $node->safe_psql('postgres',
	"$q SELECT count(*) FROM docs WHERE to_wdoc('simple', body) \@\@\@ 'common'::wquery");
is($common_after, 4000, 'REINDEX from heap restores correct answers after corruption');

$node->stop;
done_testing();
