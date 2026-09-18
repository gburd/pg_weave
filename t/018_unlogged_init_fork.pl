#!/usr/bin/perl
# 018_unlogged_init_fork.pl -- an UNLOGGED weave index is never left corrupt.
#
# WHY THIS FILE EXISTS.  ambuildempty() exists for exactly one purpose: to write
# the INIT fork of an unlogged index.  Crash recovery resets an unlogged
# relation's main fork FROM its init fork (ResetUnloggedRelations), so an init
# fork that does not contain a valid empty index leaves a main fork that is not
# empty but CORRUPT -- a weave index whose block 0 does not exist, which every
# reader starts at.  weave_buildempty() used to be a bare
# weave_init_metapage(index) call, which writes block 0 of the MAIN fork: it
# therefore initialized the wrong fork, left the init fork empty, AND scribbled
# on a main fork that ambuild had already filled (the Assert in
# weave_init_metapage() fires in an assert build).  contrib/bloom had the
# identical bug until 2016 (PostgreSQL abaffa9075, bug #14155).
#
# WHY IT IS A TAP TEST AND NOT sql/.  The failure is invisible without a crash.
# CREATE INDEX on an unlogged table, queries against it, DROP -- all of that
# succeeds in one session with an empty init fork, because nothing reads the init
# fork until recovery copies it over the main fork.  It takes its own cluster and
# an immediate stop.
#
# WHAT IS ASSERTED, and why it is phrased as an invariant rather than a policy.
# pg_weave writes 100% through GenericXLog (AGENTS.md hard rule 2), and
# GenericXLogStart() sets `isLogged = RelationNeedsWAL(relation)`, which is FALSE
# for an unlogged relation -- so GenericXLog emits no WAL for the init fork and
# cannot initialize it.  The current resolution is therefore to REFUSE unlogged
# relations at build time (see weave_reject_unlogged() in src/am/ambuild.c for
# the full reasoning, including why contrib/bloom's buffer-cache approach is not
# a template).  But the property that matters is not "we refuse": it is "there is
# never an unlogged weave index that a crash turns into a corrupt one".  So this
# test accepts EITHER outcome and checks the one that occurred -- a refusal must
# be a clean, documented ERROR, and a successful build must survive a crash and
# still answer.  If someone later makes unlogged work (the route that stays
# inside rule 2 is a lazily initialized metapage over a zero-block main fork),
# this file starts exercising the crash path instead of the refusal path without
# being rewritten, which is the difference between a test that pins a decision
# and a test that pins a requirement.
#
# Deliberately NO CHECKPOINT before the crash, for the same reason as t/014 and
# t/016: a checkpoint would put the pages on disk and leave the WAL path
# untested.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('unlogged_init');
$node->init;
# fsync off for the same reason t/014 and t/016 do it: an immediate stop kills the
# postmaster, so anything already written(2) survives in the OS.  What is under
# test is whether the records were written at all.
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

# Run a DDL statement and report its outcome as data rather than as a psql exit
# status, so both branches of the invariant can be checked in one place.  A
# RAISE NOTICE would be fatal here: safe_psql() warn()s on non-empty stderr and
# this file runs with `warnings FATAL => 'all'`.
$node->safe_psql('postgres', q{
	CREATE FUNCTION try_ddl(stmt text) RETURNS text LANGUAGE plpgsql AS $$
	BEGIN
		EXECUTE stmt;
		RETURN 'built';
	EXCEPTION WHEN OTHERS THEN
		RETURN 'refused ' || SQLSTATE || ' ' || SQLERRM;
	END $$;
});

# A permanent index over the same data and the same opclass.  It is the control:
# it proves the DDL below is well formed and that this cluster can build and
# recover a weave index at all, so a refusal further down is a decision and not a
# broken test.
$node->safe_psql('postgres', q{
	CREATE TABLE logged_doc (id serial, body text);
	INSERT INTO logged_doc(body)
	  SELECT 'unlogged probe common' || (g % 7) || ' tag' || g
	    FROM generate_series(1, 400) g;
	CREATE INDEX logged_weave ON logged_doc USING weave (to_wdoc('simple', body));
});
my $logged_hits = $node->safe_psql('postgres',
	q{SELECT count(*) FROM logged_doc WHERE to_wdoc('simple', body) @@@ 'common3'});
cmp_ok($logged_hits, '>', 0,
	"control: a permanent weave index answers ($logged_hits rows)");

# --- an unlogged heap with a weave index ----------------------------------
$node->safe_psql('postgres', q{
	CREATE UNLOGGED TABLE unlogged_doc (id serial, body text);
	INSERT INTO unlogged_doc(body)
	  SELECT 'unlogged probe common' || (g % 7) || ' tag' || g
	    FROM generate_series(1, 400) g;
});

my $outcome = $node->safe_psql('postgres', q{
	SELECT try_ddl($$CREATE INDEX unlogged_weave ON unlogged_doc
	                   USING weave (to_wdoc('simple', body))$$)});
note("CREATE INDEX on an unlogged table: $outcome");

my $refused = ($outcome ne 'built');

if ($refused)
{
	# The refusal has to be a clean, classifiable ERROR that names the reason.
	# A bare XX000 from an elog(), or a message that does not say "unlogged",
	# leaves a user with no way to tell a deliberate limitation from a bug.
	like($outcome, qr/^refused 0A000 /,
		'an unlogged weave index is refused with ERRCODE_FEATURE_NOT_SUPPORTED');
	like($outcome, qr/unlogged/,
		'the refusal message names unlogged relations as the reason');

	# And the refusal left nothing behind: no index, and a heap still usable.
	is($node->safe_psql('postgres',
			q{SELECT count(*) FROM pg_class WHERE relname = 'unlogged_weave'}),
		'0', 'the refused build left no index behind');
	is($node->safe_psql('postgres', q{SELECT count(*) FROM unlogged_doc}),
		'400', 'the unlogged heap is untouched by the refused build');

	# A TEMP index must still work.  RelationNeedsWAL() is false for temp
	# relations too, so a check written against that instead of against
	# RELPERSISTENCE_UNLOGGED would refuse temp indexes as well -- for a hazard
	# they do not have, since a temp relation is discarded on crash and never
	# has an init fork.  One psql call, because a temp table dies with its
	# session.
	is($node->safe_psql('postgres', q{
			CREATE TEMP TABLE temp_doc (id serial, body text);
			INSERT INTO temp_doc(body)
			  SELECT 'temp probe common' || (g % 7) FROM generate_series(1, 200) g;
			CREATE INDEX temp_weave ON temp_doc USING weave (to_wdoc('simple', body));
			SELECT count(*) FROM temp_doc
			 WHERE to_wdoc('simple', body) @@@ 'common3';
		}), '29', 'a TEMP weave index still builds and answers');

	# ALTER TABLE ... SET UNLOGGED rebuilds the indexes through the same
	# ambuild callback, so it must be refused by the same check -- otherwise an
	# existing permanent index becomes an unlogged one by the back door, which
	# is the same corruption with no CREATE INDEX anywhere near it.  On its own
	# table, so that a future version where the ALTER succeeds does not silently
	# turn the control index above into an unlogged one and break the
	# crash-recovery assertions that follow.
	$node->safe_psql('postgres', q{
		CREATE TABLE flip_doc (id serial, body text);
		INSERT INTO flip_doc(body)
		  SELECT 'flip probe common' || (g % 7) FROM generate_series(1, 200) g;
		CREATE INDEX flip_weave ON flip_doc USING weave (to_wdoc('simple', body));
	});
	my $alter = $node->safe_psql('postgres',
		q{SELECT try_ddl($$ALTER TABLE flip_doc SET UNLOGGED$$)});
	note("ALTER TABLE ... SET UNLOGGED: $alter");
	like($alter, qr/^refused /,
		'ALTER TABLE ... SET UNLOGGED is refused for a weave-indexed table');
	is($node->safe_psql('postgres',
			q{SELECT relpersistence FROM pg_class WHERE relname = 'flip_doc'}),
		'p', 'the table is still permanent after the refused ALTER');
}
else
{
	# Unlogged builds are supported.  Then the init fork must be real: crash,
	# recover, and the index has to be USABLE.  Recovery truncates the unlogged
	# heap, so the row count is 0 and the correct answer to every query is "no
	# rows" -- but an index that cannot be read at all raises instead, and an
	# index whose metapage is missing raises differently, so the assertions
	# below are that the reads succeed and that fresh inserts become findable.
	$node->stop('immediate');
	$node->start;

	is($node->safe_psql('postgres', q{SELECT count(*) FROM unlogged_doc}), '0',
		'recovery reset the unlogged heap, as it always does');
	is($node->safe_psql('postgres', q{
			SELECT count(*) FROM unlogged_doc
			 WHERE to_wdoc('simple', body) @@@ 'common3'}), '0',
		'the recovered unlogged index reads as an EMPTY index, not a broken one');
	my ($ok, $detail) = split /\|/, $node->safe_psql('postgres',
		q{SELECT ok, coalesce(detail, '') FROM weave_check('unlogged_weave', true)
		   WHERE NOT ok LIMIT 1}), 2;
	ok(!defined $ok || $ok eq '',
		'weave_check(deep) reports no failed invariant on the recovered init fork'
		. (defined $detail ? " ($detail)" : ''));

	# And it still accepts and answers new work, which is what "usable" means.
	$node->safe_psql('postgres', q{
		INSERT INTO unlogged_doc(body)
		  SELECT 'after recovery common3 tag' || g FROM generate_series(1, 50) g});
	is($node->safe_psql('postgres', q{
			SELECT count(*) FROM unlogged_doc
			 WHERE to_wdoc('simple', body) @@@ 'common3'}), '50',
		'the recovered unlogged index indexes and returns new rows');
}

# --- the crash, unconditionally -------------------------------------------
#
# On the refusal branch this is the half that proves the refusal is durable
# rather than a first-call accident, and that nothing the failed build did to
# the cluster survives a crash.  It also re-establishes that the control index
# -- built in the same session, through the same writer -- crosses recovery
# intact, so "no unlogged weave index exists to be corrupted" is a statement
# about a cluster that demonstrably recovers weave indexes.
$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres',
		q{SELECT count(*) FROM logged_doc
		   WHERE to_wdoc('simple', body) @@@ 'common3'}),
	$logged_hits,
	'the permanent control index answers identically after an immediate shutdown');

my ($cok, $cdetail) = split /\|/, $node->safe_psql('postgres',
	q{SELECT ok, coalesce(detail, '') FROM weave_check('logged_weave', true)
	   WHERE NOT ok LIMIT 1}), 2;
ok(!defined $cok || $cok eq '',
	'weave_check(deep) reports no failed invariant on the control index after recovery'
	. (defined $cdetail ? " ($cdetail)" : ''));

if ($refused)
{
	is($node->safe_psql('postgres',
			q{SELECT count(*) FROM pg_class WHERE relname = 'unlogged_weave'}),
		'0', 'no unlogged weave index came into existence across recovery');
	like($node->safe_psql('postgres', q{
			SELECT try_ddl($$CREATE INDEX unlogged_weave ON unlogged_doc
			                   USING weave (to_wdoc('simple', body))$$)}),
		qr/^refused 0A000 /,
		'the refusal still holds after crash recovery');
}

done_testing();
