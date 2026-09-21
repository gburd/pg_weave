# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 019_pending_v8_upgrade.pl -- read a v8-layout PENDING page with current code.
#
# WHY THIS EXISTS.  Task V7's second half (doc/GAPS.md G23) made a pending item
# carry the inserted row's vector, which GREW the item header from 12 bytes to 16
# and changed the item stride; Z8's second half (G35) grew it again to 20 for the
# raw gram_ops text.  The three layouts are told apart by page kind
# (WEAVE_PK_PENDING vs _V9 vs _V10), and weave_pending_iter_next() in
# include/weave/am.h has a branch for each.  An index upgraded with un-flushed
# pending documents reaches an OLD branch on its very next scan -- so it is not a
# theoretical path, and before this file nothing executed it.  This codebase has
# closed two gaps (G24, G26) whose entire content was "unreachable, therefore
# untested", and a compatibility branch that is claimed in a comment and run by
# nothing is the same bet.
#
# HOW.  The same manufacture-the-old-image method as t/010_format_v6_upgrade.pl,
# and legitimate for the same reason: the delta down to v8 is precisely known and
# small.  An item is
#
#     v10: tid[6] pad[2] doclen[4] veclen[4] gramlen[4]
#            | wdoc[doclen] ... | wvec[veclen] ... | gram[gramlen] ...
#          stride = MAXALIGN(20 + doclen) + MAXALIGN(veclen) + MAXALIGN(gramlen)
#     v9:  tid[6] pad[2] doclen[4] veclen[4] | wdoc[doclen] ... | wvec[veclen] ...
#          stride = MAXALIGN(16 + doclen) + MAXALIGN(veclen)
#     v8:  tid[6] pad[2] doclen[4]           | wdoc[doclen] ...
#          stride = MAXALIGN(12 + doclen)
#
# so the downgrade repacks each item eight bytes earlier, drops the vector and the
# gram text, and rewrites pd_lower and the page's kind bits.  A v8 writer produced
# exactly these bytes.
#
# WHAT IT ASSERTS, in order of what would go unnoticed without it:
#   1. The pending documents still answer lexically, byte-identically -- i.e. the
#      v8 stride is read correctly rather than producing "skipping malformed
#      pending document" (which is what a wrong stride actually does, and how the
#      first version of the G23 change was caught).
#   2. An INSERT after the downgrade does NOT append to the v8 tail page: mixing
#      two item layouts on one page makes it unparseable by either reader.  Both
#      kinds must then be present at once.
#   3. A flush over the MIXED chain folds every document from both layouts, and
#      the rows from the v8 page get DEAD lanes -- their vectors are genuinely
#      gone, and a dead lane is the honest representation of that.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use constant {
	BLCKSZ                => 8192,
	CONTENT_START         => 24,	# MAXALIGN(SizeOfPageHeaderData)
	PD_LOWER_OFF          => 12,	# PageHeaderData.pd_lower, uint16
	OPAQUE_SIZE           => 8,		# MAXALIGN(sizeof(WeavePageOpaqueData))
	WEAVE_PENDING_BIT     => 1 << 3,	# the legacy one-hot kind bit
	WEAVE_PAGE_KIND_EXT   => 1 << 15,
	WEAVE_PK_PENDING_V10  => 33,
	V10_HDR               => 20,
	V8_HDR                => 12,
};

sub maxalign { my ($n) = @_; return ($n + 7) & ~7; }

# Rewrite every current-layout pending page in `path` into the v8 layout, dropping
# the vectors and the gram text.  Server MUST be down.  Returns the number of pages
# rewritten and the number of items repacked.
#
# Blocks are SCANNED rather than walked from meta.pendinghead: the walk would
# duplicate the metapage arithmetic t/010 already owns, and a scan cannot miss a
# page that the chain has lost track of -- which, if it ever happened, is
# something this test should trip over rather than skip.
sub downgrade_pending_pages
{
	my ($path) = @_;
	open(my $fh, '+<:raw', $path) or die "open $path: $!";
	binmode $fh;
	my $size = (stat($fh))[7];
	my ($npages, $nitems) = (0, 0);

	for (my $blk = 0; $blk * BLCKSZ < $size; $blk++)
	{
		my $page;

		sysseek($fh, $blk * BLCKSZ, 0) or die "seek: $!";
		sysread($fh, $page, BLCKSZ) == BLCKSZ or last;

		my $opoff = BLCKSZ - OPAQUE_SIZE;
		my ($flags, $kind) = unpack('vv', substr($page, $opoff, 4));
		next unless ($flags & WEAVE_PAGE_KIND_EXT)
			&& $kind == WEAVE_PK_PENDING_V10;

		my $lower = unpack('v', substr($page, PD_LOWER_OFF, 2));
		die "pd_lower $lower out of range on block $blk"
		  if $lower < CONTENT_START || $lower > $opoff;

		# repack the items, eight bytes earlier each, vector and gram text dropped
		# 'vvv xx V' is the ItemPointerData + padding shape: bi_hi, bi_lo, ip_posid
		# are three uint16s, and doclen is a uint32 at offset 8, so the two padding
		# bytes have to be spelled out -- Perl's pack inserts no alignment of its
		# own, and omitting the xx reads doclen from offset 6.
		my $out = '';
		my $p = CONTENT_START;
		my $onpage = 0;
		while ($p + V10_HDR <= $lower)
		{
			my ($b_hi, $b_lo, $posid, $doclen, $veclen, $gramlen) =
			  unpack('vvvxxVVV', substr($page, $p, V10_HDR));
			my $stride = maxalign(V10_HDR + $doclen) + maxalign($veclen)
			  + maxalign($gramlen);
			last if $p + $stride > $lower;

			my $doc = substr($page, $p + V10_HDR, $doclen);
			my $item = pack('vvvxxV', $b_hi, $b_lo, $posid, $doclen) . $doc;
			$item .= "\0" x (maxalign(V8_HDR + $doclen) - length($item));
			$out .= $item;
			$onpage++;
			$p += $stride;
		}
		die "block $blk: repacked 0 items from a current-layout pending page"
		  unless $onpage;
		$nitems += $onpage;

		# The tail between the new pd_lower and the opaque must be zeroed: it was
		# never written by a v8 producer, and leaving v10 bytes there would let a
		# reader that mis-computes pd_lower appear to work.
		my $newlower = CONTENT_START + length($out);
		substr($page, CONTENT_START, $opoff - CONTENT_START,
			   $out . ("\0" x ($opoff - $newlower)));
		substr($page, PD_LOWER_OFF, 2, pack('v', $newlower));
		# ...and the kind LAST, so an interrupted surgery leaves a v10 page whose
		# items are v8 -- unparseable, which is louder than silently wrong.
		substr($page, $opoff, 4,
			   pack('vv', ($flags & ~WEAVE_PAGE_KIND_EXT) | WEAVE_PENDING_BIT, 0));

		sysseek($fh, $blk * BLCKSZ, 0) or die "seek: $!";
		syswrite($fh, $page) == BLCKSZ or die "write: $!";
		$npages++;
	}
	close($fh) or die "close: $!";
	return ($npages, $nitems);
}

my $node = PostgreSQL::Test::Cluster->new('pendv8');
# no_data_checksums is REQUIRED, not tidiness.  PostgreSQL 18 turns data checksums
# on by default (17 does not), and this test rewrites page bytes behind the
# server's back without recomputing pd_checksum -- so on 18 the very next read of
# the surgery's page failed with "invalid page in block 24" and the test died after
# five assertions while passing cleanly on 17.  Upstream added this option for
# exactly this class of test; PostgreSQL 17's init() ignores the unknown key, where
# checksums are off anyway.
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# A small built index, then three documents left in the pending buffer.
$node->safe_psql('postgres', q{
    CREATE TABLE docs (id serial, body text, d wdoc, v wvec(4));
    INSERT INTO docs(body)
      SELECT 'built shared term'||(g % 5)||' rare'||g FROM generate_series(1, 200) g;
    UPDATE docs SET d = to_wdoc('simple', body),
                    v = ('[' || (id % 7) || ',' || (id % 5) || ','
                              || (id % 3) || ',' || (id % 2) || ']')::wvec;
    CREATE INDEX docs_weave ON docs USING weave (d, v);
});
$node->safe_psql('postgres', q{
    INSERT INTO docs(body, d, v)
      SELECT 'pendingdoc shared term'||g,
             to_wdoc('simple', 'pendingdoc shared term'||g),
             ('[' || g || ',' || g || ',' || g || ',' || g || ']')::wvec
        FROM generate_series(901, 903) g;
});

sub pending_answers
{
	my ($label) = @_;
	my @out;
	for my $q ('pendingdoc', 'shared', 'rare7', 'pendingdoc AND term901')
	{
		push @out, $node->safe_psql('postgres',
			qq{SET enable_seqscan=off;
			   SELECT count(*) FROM docs WHERE d @@@ '$q'::wquery});
	}
	push @out, $node->safe_psql('postgres', q{
		SET enable_seqscan=off;
		SELECT string_agg(id::text, ',' ORDER BY id) FROM docs
		 WHERE d @@@ 'pendingdoc'::wquery});
	note("$label: " . join(' | ', @out));
	return \@out;
}

sub kind_pages
{
	my ($kind) = @_;
	return $node->safe_psql('postgres',
		"SELECT coalesce(sum(npages), 0) FROM weave_index_size_detail('docs_weave')
		  WHERE kind = '$kind'");
}

my $before = pending_answers('current-layout pending (as inserted)');
is($before->[0], '3', 'the three pending documents are searchable before surgery');
cmp_ok(kind_pages('pending'), '>', 0,
	'and they are on a current-layout pending page');
is(kind_pages('pending_v8'), '0', 'with no legacy pending page yet');

my $relpath = $node->safe_psql('postgres',
	"SELECT pg_relation_filepath('docs_weave')");
my $abspath = $node->data_dir . '/' . $relpath;

# CHECKPOINT first: the pending page lives in shared buffers until one, and the
# surgery would otherwise be overwritten by the eviction of a newer image.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop;
my ($npages, $nitems) = downgrade_pending_pages($abspath);
is($npages, 1, 'exactly one pending page was rewritten into the v8 layout');
is($nitems, 3, 'all three items were repacked');
$node->start;

# --- 1. THE COMPATIBILITY GATE --------------------------------------------
my $after = pending_answers('v8 pending (manufactured legacy image)');
is_deeply($after, $before,
	'a v8-layout pending page returns byte-identical answers under current code');
cmp_ok(kind_pages('pending_v8'), '>', 0, 'and it now reads as the legacy kind');
is(kind_pages('pending'), '0', 'with no current-layout page left');

# --- 2. a new INSERT must not append to the v8 tail ------------------------
$node->safe_psql('postgres', q{
    INSERT INTO docs(body, d, v)
      VALUES ('pendingdoc shared term904', to_wdoc('simple', 'pendingdoc shared term904'),
              '[904,904,904,904]');
});
cmp_ok(kind_pages('pending'), '>', 0,
	'the new item started a current-layout page instead of appending to the v8 one');
cmp_ok(kind_pages('pending_v8'), '>', 0, 'and the v8 page is still there');
is($node->safe_psql('postgres',
		q{SET enable_seqscan=off;
		  SELECT count(*) FROM docs WHERE d @@@ 'pendingdoc'::wquery}),
	'4', 'all four pending documents answer across the MIXED chain');

# --- 3. a flush over the mixed chain --------------------------------------
my $lanes_before = $node->safe_psql('postgres',
	"SELECT sum(nvec) FROM weave_vec_meta('docs_weave')");
$node->safe_psql('postgres', "SELECT weave_merge('docs_weave')");
my $lanes_after = $node->safe_psql('postgres',
	"SELECT sum(nvec) FROM weave_vec_meta('docs_weave')");
is($lanes_after, $lanes_before + 4,
	"the flush covered every document from both layouts ($lanes_before -> $lanes_after)");

# Three dead lanes, one live: the v8 page genuinely has no vectors to carry, and a
# dead lane says so.  A weft that skipped them instead would shift every later
# lane and mis-associate vectors with documents.
is($node->safe_psql('postgres',
		"SELECT count(*) FROM weave_vec_lanes('docs_weave') WHERE NOT live"),
	'3', 'the three v8-page rows get dead lanes, not skipped ones');
is($node->safe_psql('postgres',
		q{SET enable_seqscan=off;
		  SELECT count(*) FROM docs WHERE d @@@ 'pendingdoc'::wquery}),
	'4', 'and all four are still searchable after the flush');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM weave_check('docs_weave') WHERE NOT ok"),
	'0', 'every weave_check() invariant holds after the mixed-chain flush');

$node->stop;
done_testing();
