# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# 035_merge_freed_page.pl -- a merge whose input bolt has a damaged chain WARNS
# and SKIPS that input; it never publishes a smaller bolt (doc/GAPS.md G75, task
# L23 in doc/PHASES.md).
#
# Before L23 every chain reader the merge uses stopped quietly at a page flagged
# freed, or at a cut nextblk, so a live page freed by a bug (the G15/G62 class)
# was laundered by the next merge: a smaller, self-consistent bolt was written,
# the input freed, and the postings behind the damage were gone with no
# invariant left to say so.
#
# HOW.  One table per damage case, identical workload: eight rounds of
# INSERT + VACUUM, each VACUUM flushing the pending rows into one bolt.  Every
# bolt lands on the same merge level, so the eighth VACUUM is the one that
# merges.  Round 4's bolt is the smallest, hence always chosen.  After round 7 the
# server is stopped and ONE page of round 4's bolt is rewritten, then:
#
#   round 8 VACUUM: succeeds; the log WARNS naming the index, the bolt's
#     dictionary block and the damaged block; nothing merges (the level holds
#     seven mergeable runs without it), every round-4 page keeps its LSN;
#   weave_check(deep) reports bolt_chains_intact = false;
#   every term is still answered (no row is lost: the damage is to chain
#     LINKAGE, and a scan reaches each term through the dictionary index and
#     its firstposting directly, so no term is reachable only through it);
#   round 9 VACUUM: the other eight bolts merge (the damaged one is left out,
#     not allowed to block its level) -- two bolts remain;
#   round 10 (no VACUUM) + weave_vacuum(): it flushes a third bolt and compacts
#     the two healthy ones, the damaged one again left out (its compaction used
#     to stop there and compact nothing);
#   REINDEX: one bolt, every invariant holds, every term answered.
#
# The cases.  "Freed" is what weave_free_page_locked() writes: the flag, the XID
# stamp, nextblk ended.  "Cut" is nextblk ended with NO flag, which no per-page
# test can see; only a recorded length shows it.
#   dict_freed     the LAST dictionary page freed
#   dict_mid_freed a MIDDLE dictionary page freed: the merge's dictionary walk
#                  used to stop there and publish a bolt without the terms behind
#   dict_cut       the FIRST dictionary page cut: the term count
#                  (WeaveSegMeta.nterms) disagrees
#   post_freed     the LAST page of the shared posting chain freed
#   post_mid_freed a MIDDLE posting page freed
#   post_cut       a MIDDLE posting page cut: the chain no longer reaches the
#                  last term's first posting block
#   dictindex_freed, doclen_freed, surf_freed, doclist_freed: the last page of
#                  the dictionary index, doclen sidecar, SuRF trie and
#                  document list chains freed

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use constant {
	BLCKSZ        => 8192,
	PRUNE_XID_OFF => 20,      # PageHeaderData.pd_prune_xid
	OPAQUE_OFF    => 8184,    # BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData))
	NEXTBLK_OFF   => 8188,    # OPAQUE_OFF + flags(2) + kind(2)
	WEAVE_FREED   => (1 << 8),
	NTERMS_PER_DOC => 12,
};

# case => [chain kind (weave_page_info), which page, how]
my %spec = (
	dict_freed      => [ 'dictionary',     'last',  'free' ],
	dict_mid_freed  => [ 'dictionary',     'mid',   'free' ],
	dict_cut        => [ 'dictionary',     'first', 'cut' ],
	post_freed      => [ 'postings',       'last',  'free' ],
	post_mid_freed  => [ 'postings',       'mid',   'free' ],
	post_cut        => [ 'postings',       'mid',   'cut' ],
	dictindex_freed => [ 'dict_index',     'last',  'free' ],
	doclen_freed    => [ 'doclen_sidecar', 'last',  'free' ],
	surf_freed      => [ 'surf_trie',      'last',  'free' ],
	doclist_freed   => [ 'doclist',        'last',  'free' ],
);
my @cases = sort keys %spec;

my $node = PostgreSQL::Test::Cluster->new('primary');
# PG18 inits with data checksums on; this test rewrites page bytes offline
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf',
	"fsync = off\nautovacuum = off\nmax_parallel_maintenance_workers = 0\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Round r adds n documents of NTERMS_PER_DOC unique terms each, and the same
# terms to the case's truth table, then VACUUMs.
sub round
{
	my ($t, $r, $n, $novacuum) = @_;
	sql(qq{
		INSERT INTO $t (body)
		SELECT string_agg('r${r}d' || g || 't' || k, ' ')
		  FROM generate_series(1, $n) g, generate_series(1, ${\ NTERMS_PER_DOC}) k
		 GROUP BY g;
		INSERT INTO ${t}_terms
		SELECT 'r${r}d' || g || 't' || k
		  FROM generate_series(1, $n) g, generate_series(1, ${\ NTERMS_PER_DOC}) k;
	});
	sql("VACUUM $t") unless $novacuum;
}

# (answered, total): terms the index answers with exactly their one document --
# every term of round 4 (the damaged bolt) and one term per document of every
# other round.  weave_search() enters the scan machinery directly, with no
# executor recheck behind it (AGENTS.md: the suite running is not the SITE
# running).
sub answers
{
	my ($t) = @_;
	return sql(qq{
		SELECT count(*) FILTER (WHERE (SELECT count(*) FROM weave_search('${t}_w',
		                    to_wquery('simple', term), 5)) = 1)
		       || '/' || count(*)
		  FROM ${t}_terms WHERE term LIKE 'r4d%' OR term LIKE '%t1'});
}

sub pages_of
{
	my ($t, $lo, $hi) = @_;
	my %p;
	for my $row (split /\n/, sql(qq{
		SELECT blkno, kind, coalesce(nextblk, -1), lsn
		  FROM weave_page_info('${t}_w')
		 WHERE NOT freed AND NOT uninitialized AND blkno > 0
		   AND kind NOT LIKE 'pending%'
		   AND lsn > '$lo' AND lsn <= '$hi'}))
	{
		my ($b, $k, $nx, $lsn) = split /\|/, $row;
		$p{$b} = { kind => $k, next => $nx, lsn => $lsn };
	}
	return \%p;
}

# the chain of `kind` inside a page set, in order
sub chain
{
	my ($pages, $kind) = @_;
	my @b = grep { $pages->{$_}{kind} eq $kind } keys %$pages;
	return () unless @b;
	my %pointed = map { $pages->{$_}{next} => 1 } @b;
	my @head = grep { !$pointed{$_} } @b;
	die "one $kind head expected, got (@head) among: "
	  . join(' ', map { "$_=$pages->{$_}{kind}->$pages->{$_}{next}" } sort { $a <=> $b } keys %$pages)
	  unless @head == 1;
	my @c = ($head[0]);
	push @c, $pages->{ $c[-1] }{next} while $pages->{ $c[-1] }{next} != -1;
	die "$kind chain leaves the bolt" if grep { !exists $pages->{$_} } @c;
	return @c;
}

sub lsns
{
	my ($t, $blks) = @_;
	my $in = join(',', @$blks);
	return sql(qq{
		SELECT string_agg(blkno || ':' || lsn || ':' || freed, ' ' ORDER BY blkno)
		  FROM weave_page_info('${t}_w') WHERE blkno IN ($in)});
}

# ---- build every case's index up to round 7 --------------------------------
my %c;
my @live;
for my $t (@cases)
{
	sql(qq{
		CREATE TABLE $t (id serial, body text);
		CREATE TABLE ${t}_terms (term text);
		CREATE INDEX ${t}_w ON $t USING weave (to_wdoc('simple', body));
	});
	for my $r (1 .. 7)
	{
		# the INSERT pointer: VACUUM assigns no XID, so its commit flushes
		# nothing and the WRITE pointer can trail the pages it just wrote
		my $lo = sql('SELECT pg_current_wal_insert_lsn()');
		round($t, $r, $r == 4 ? 70 : 100);
		my $hi = sql('SELECT pg_current_wal_insert_lsn()');
		$c{$t}{pages} = pages_of($t, $lo, $hi) if $r == 4;
	}
	is(sql("SELECT weave_index_nsegments('${t}_w')"), '7',
		"$t: seven bolts, one per round, none merged yet");
	my ($kind, $which, $how) = @{ $spec{$t} };
	my @ch = chain($c{$t}{pages}, $kind);
	if (!@ch)
	{
		fail("$t: round 4's bolt has no $kind page to damage");
		next;
	}
	push @live, $t;
	$c{$t}{dictroot} = (chain($c{$t}{pages}, 'dictionary'))[0];
	cmp_ok(scalar @ch, '>=', $which eq 'last' ? 1 : 3,
		"$t: round 4's $kind chain spans " . scalar(@ch) . " page(s): @ch");
	$c{$t}{bad} = $which eq 'first' ? $ch[0]
	  : $which eq 'last' ? $ch[-1] : $ch[ int(@ch / 2) ];
	is(sql(qq{SELECT count(*) FROM weave_check('${t}_w', true) WHERE NOT ok}), '0',
		"$t: every invariant holds before the damage");
	$c{$t}{ans0} = answers($t);
	like($c{$t}{ans0}, qr{^(\d+)/\1$}, "$t: every term answered before the damage ($c{$t}{ans0})");
}

# ---- damage one page per case, offline -------------------------------------
my $xid = sql('SELECT txid_current()') % (2**32);
my %path = map { $_ => $node->data_dir . '/' . sql("SELECT pg_relation_filepath('${_}_w')") } @cases;
$node->stop;

sub rw
{
	my ($p, $blk, $off, $fmt, $fn) = @_;
	my $len = length(pack($fmt, 0));
	open(my $fh, '+<:raw', $p) or die "open $p: $!";
	sysseek($fh, $blk * BLCKSZ + $off, 0) or die "seek: $!";
	sysread($fh, my $buf, $len) == $len or die "short read";
	my $v = $fn->(unpack($fmt, $buf));
	sysseek($fh, $blk * BLCKSZ + $off, 0) or die "seek: $!";
	syswrite($fh, pack($fmt, $v)) == $len or die "write: $!";
	close($fh) or die "close: $!";
	return;
}

# what weave_free_page_locked() writes: the flag, the XID stamp, nextblk ended
sub mark_freed
{
	my ($p, $blk) = @_;
	rw($p, $blk, OPAQUE_OFF, 'v', sub { $_[0] | WEAVE_FREED });
	rw($p, $blk, PRUNE_XID_OFF, 'V', sub { $xid });
	rw($p, $blk, NEXTBLK_OFF, 'V', sub { 0xFFFFFFFF });
	return;
}

for my $t (@live)
{
	if ($spec{$t}[2] eq 'free')
	{
		mark_freed($path{$t}, $c{$t}{bad});
	}
	else
	{
		rw($path{$t}, $c{$t}{bad}, NEXTBLK_OFF, 'V', sub { 0xFFFFFFFF });
	}
}
$node->start;

# ---- the merge meets it ----------------------------------------------------
for my $t (@live)
{
	my $bad = $c{$t}{bad};
	my @r4 = sort { $a <=> $b } keys %{ $c{$t}{pages} };
	my $before = lsns($t, \@r4);

	is(answers($t), $c{$t}{ans0}, "$t: every term still answered after the damage");

	my $logpos = -s $node->logfile;
	round($t, 8, 100);    # dies if VACUUM fails
	pass("$t: round 8 VACUUM succeeded over the damaged bolt");
	my $log = slurp_file($node->logfile, $logpos);
	like($log,
		qr/WARNING:  index "${t}_w": bolt with dictionary at block $c{$t}{dictroot} has a damaged chain; skipping its merge/,
		"$t: the WARNING names the index and the bolt (dictionary block $c{$t}{dictroot})");
	like($log, qr/DETAIL:  [^\n]*block $bad\b/,
		"$t: its DETAIL names the damaged block $bad");
	is(sql("SELECT weave_index_nsegments('${t}_w')"), '8',
		"$t: nothing merged -- eight bolts");
	is(lsns($t, \@r4), $before, "$t: every page of the damaged bolt is untouched");

	my $chk = sql(qq{SELECT ok || ' ' || coalesce(detail, '')
		FROM weave_check('${t}_w', true) WHERE invariant = 'bolt_chains_intact'});
	like($chk, qr/^false .*block $bad\b/,
		"$t: weave_check(deep) reports the bolt, naming block $bad");
	note("$t: bolt_chains_intact: $chk");
	like(answers($t), qr{^(\d+)/\1$}, "$t: every term answered after round 8");

	$logpos = -s $node->logfile;
	round($t, 9, 100);
	is(sql("SELECT weave_index_nsegments('${t}_w')"), '2',
		"$t: round 9 merges the eight healthy bolts around the damaged one");
	like(slurp_file($node->logfile, $logpos), qr/skipping its merge/,
		"$t: round 9 met and skipped it again");
	is(lsns($t, \@r4), $before, "$t: the damaged bolt is still untouched after round 9");
	like(answers($t), qr{^(\d+)/\1$}, "$t: every term answered after round 9");

	# weave_vacuum()'s compaction (weave_compact_to_one) skips the damaged bolt
	# too.  Round 10's rows stay pending, so weave_vacuum() itself flushes them
	# into a third bolt and then compacts; before the skip, both of its loops
	# stopped at the damaged bolt and it compacted none (three bolts remained).
	round($t, 10, 100, 1);
	$logpos = -s $node->logfile;
	sql("SELECT weave_vacuum('${t}_w')");
	is(sql("SELECT weave_index_nsegments('${t}_w')"), '2',
		"$t: weave_vacuum() flushes round 10 and compacts the two healthy bolts around the damaged one");
	like(slurp_file($node->logfile, $logpos), qr/skipping its merge/,
		"$t: weave_vacuum() met and skipped it");
	is(lsns($t, \@r4), $before, "$t: the damaged bolt is still untouched after weave_vacuum()");
	like(answers($t), qr{^(\d+)/\1$}, "$t: every term answered after weave_vacuum()");

	sql("REINDEX INDEX ${t}_w");
	is(sql("SELECT weave_index_nsegments('${t}_w')"), '1', "$t: REINDEX rebuilds one bolt");
	is(sql(qq{SELECT count(*) FROM weave_check('${t}_w', true) WHERE NOT ok}), '0',
		"$t: every invariant holds after REINDEX");
	like(answers($t), qr{^(\d+)/\1$}, "$t: every term answered after REINDEX");
}

done_testing();
