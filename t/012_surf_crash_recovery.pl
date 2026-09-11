# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Crash-recovery / WAL-replay correctness for the v7 FUZZY weft: the SuRF trie
# over a bolt's vocabulary, on its own WEAVE_PK_SURF page chain.
#
# WHY THIS TEST EXISTS AT ALL.  doc/PRODUCTION_READINESS.md gate 7: a new weft
# that survives no crash test is a data-loss risk.  Every page of the chain goes
# through GenericXLog (AGENTS.md hard rule 2 -- no raw XLogInsert, no
# log_newpage, no smgrwrite), and the point of that rule is that crash safety is
# auditable rather than trusted.  "Auditable" still has to be audited: an
# immediate shutdown discards shared buffers, so everything the index knows after
# recovery came out of WAL records.
#
# WHAT WOULD BE MISSED WITHOUT IT.  The trie is a filter with false positives and
# no false negatives.  A chain that came back one page short would at least be
# refused, because weave_surftrie_open() requires the image length to equal what
# the header's counts imply -- but a chain that came back with a stale page in the
# middle can validate structurally and still be WRONG, and the only thing that
# notices is weave_check()'s surf_trie_matches_dictionary comparison against the
# dictionary bytes.  So this test asserts, after each crash: the shape and the
# exact byte count of every trie are unchanged, every invariant including that
# comparison still holds, and the index answers identically.
#
# ---------------------------------------------------------------------------
# THE WAL-DURABILITY GOTCHA THIS TEST HAD TO LEARN, AND WHICH IS NOT ABOUT THE
# FUZZY WEFT AT ALL
# ---------------------------------------------------------------------------
#
# `SELECT weave_merge('idx')` writes WAL through GenericXLog but touches no heap
# and no catalog, so its transaction never acquires a TransactionId.  PostgreSQL's
# RecordTransactionCommit() only calls XLogFlush() when the transaction committed
# an XID (or truncated a relation, or forced sync commit), so the merge's WAL
# records are left in the WAL buffers -- written to no file.  `pg_ctl stop
# -m immediate` then loses them, and recovery rolls the whole merge back: the
# input bolts return, the merged bolt vanishes, and weave_check() is CLEAN,
# because the pre-merge state is a perfectly consistent state.
#
# This was verified to reproduce identically on the pre-v7 code, so it is a
# pre-existing durability gap and not something the fuzzy weft introduced; it is
# recorded in doc/specs/SEGMENT_FORMAT.md sect. 10 next to the WAL policy it
# qualifies.  What matters here is the consequence for this test: a merge phase
# that crashed immediately after `SELECT weave_merge()` would be asserting the
# absence of that bug rather than testing the surf chain, and would fail for a
# reason that has nothing to do with Z3.
#
# The fix is not a CHECKPOINT -- that would put the pages on disk and leave WAL
# replay untested, which is the one thing this file is for.  Instead the merge is
# followed by a trivial INSERT into an unrelated table: that transaction DOES
# acquire an XID, and XLogFlush() flushes the WAL stream up to its commit LSN,
# which includes every earlier record.  So the surf pages are still reconstructed
# from WAL alone, and the merge is durable.  Phases 1 and 2 (the build and the
# pending-list flush) need no anchor: CREATE INDEX and INSERT write heap and
# catalog rows, so they acquire XIDs of their own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
# fsync off because durability against a MACHINE crash is not what is under test:
# an immediate stop kills the postmaster, so WAL already written(2) survives in
# the OS, and it is WAL replay -- not fsync -- that this exercises.
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# One column per bolt so a difference reads as a diff rather than as "some
# aggregate moved".  `bytes` is the exact image length, which for this format is a
# pure function of the header counts -- so an identical string here means the
# recovered image has the same shape down to the byte.
my $stats_q = q{SELECT coalesce(string_agg(
	bolt || ':' || nterms || ':' || nslots || ':' || nnodes || ':' ||
	nterminal || ':' || ntrunc || ':' || maxdepth || ':' || bytes || ':' || npages,
	' ' ORDER BY bolt), '-none-') FROM weave_surf_stats('sfc_weave')};
my $check_q = q{SELECT coalesce(string_agg(invariant || '=' || ok, ' '
	ORDER BY invariant), '-clean-')
	FROM weave_check('sfc_weave', true) WHERE NOT ok};
my $surfpages_q = q{SELECT npages FROM weave_index_size_detail('sfc_weave')
	WHERE kind = 'surf_trie'};
my $count_q = "SET enable_seqscan=off; "
  . "SELECT count(*) FROM sfc WHERE d \@\@\@ 'common3'::wquery";

# Crash, recover, and assert that nothing about the fuzzy weft moved.
sub crash_and_compare
{
	my ($label) = @_;
	my $stats = $node->safe_psql('postgres', $stats_q);
	my $pages = $node->safe_psql('postgres', $surfpages_q);
	my $count = $node->safe_psql('postgres', $count_q);

	isnt($stats, '-none-', "$label: the index carries a fuzzy weft ($stats)");
	is($node->safe_psql('postgres', $check_q), '-clean-',
		"$label: every invariant holds before the crash");

	$node->stop('immediate');	# discards shared buffers; WAL is all that is left
	$node->start;

	is($node->safe_psql('postgres', $stats_q), $stats,
		"$label: every trie's shape and exact byte count survive recovery");
	is($node->safe_psql('postgres', $surfpages_q), $pages,
		"$label: the WEAVE_PK_SURF page count survives recovery");
	is($node->safe_psql('postgres', $check_q), '-clean-',
		"$label: trie/dictionary membership still agrees after recovery");
	is($node->safe_psql('postgres', $count_q), $count,
		"$label: answers identical after recovery");
}

# --- phase 1: the build writer (weave_write_segment) ------------------------
$node->safe_psql(
	'postgres', q{
	CREATE TABLE wal_anchor (id int);
	CREATE TABLE sfc (id int primary key, d wdoc);
	INSERT INTO sfc SELECT g, to_wdoc('common common' || (g % 17)
	                                  || ' mid' || (g % 101) || ' rare' || g)
		FROM generate_series(1, 3000) g;
	CREATE INDEX sfc_weave ON sfc USING weave (d);
});
crash_and_compare('build');

# --- phase 2: the pending list on top of a recovered index -----------------
# The index must be WRITABLE after recovery, and a bolt minted from the pending
# list must get its trie built on top of the recovered ones.
$node->safe_psql(
	'postgres', q{
	INSERT INTO sfc SELECT g, to_wdoc('common delta' || (g % 5) || ' rare' || g)
		FROM generate_series(20000, 20800) g;
});
crash_and_compare('pending');

# --- phase 3: the merge writer (weave_merge_segments_streaming) -------------
# A different code path from phase 1, and the one where THE TRIE IS REBUILT
# rather than combined: a merged bolt's vocabulary is the union of its inputs'
# minus fully tombstoned terms, so its level-order slot numbering, its
# rank/select tables and every one of its term ordinals differ from both inputs.
# Two LOUDS-Sparse images cannot be concatenated.
#
# The INSERT afterwards is the WAL anchor -- see the header comment.  Without it,
# recovery rolls the merge back and this phase tests nothing.
$node->safe_psql('postgres', q{SELECT weave_merge('sfc_weave')});
$node->safe_psql('postgres', q{INSERT INTO wal_anchor VALUES (1)});
my $merged_terms = $node->safe_psql('postgres',
	q{SELECT max(nterms) FROM weave_surf_stats('sfc_weave')});
cmp_ok($merged_terms, '>', 3000,
	"the merge produced a bolt with a rebuilt, larger vocabulary ($merged_terms terms)");
crash_and_compare('merge');

# --- phase 4: tombstones, then compaction ----------------------------------
# weave_vacuum() rewrites every bolt, so every trie is rebuilt over a vocabulary
# that may have lost terms whose every posting was tombstoned.
$node->safe_psql('postgres', q{
	DELETE FROM sfc WHERE id % 5 = 0;
	VACUUM sfc;
	SELECT weave_vacuum('sfc_weave');
});
$node->safe_psql('postgres', q{INSERT INTO wal_anchor VALUES (2)});
crash_and_compare('vacuum');

$node->stop;
done_testing();
