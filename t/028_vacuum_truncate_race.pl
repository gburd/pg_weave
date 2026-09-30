# Copyright (c) 2026, pg_weave contributors
#
# t/028_vacuum_truncate_race.pl -- plain VACUUM's tail truncation must not race
# concurrent INSERTs (doc/GAPS.md G67).
#
# weave_vacuum_compact() runs from amvacuumcleanup under VACUUM's
# ShareUpdateExclusiveLock, which does not exclude INSERT, and it used to end
# with RelationTruncate() on a tail it had judged free from an unlocked read of
# the free-space map.  An inserter handed one of those blocks in between lost
# its committed rows with the truncation, and a backend reading a block between
# the buffer drop and the file truncate left a valid buffer past EOF, after
# which every extension failed with "unexpected data beyond EOF".  Found by
# t/025 on PG18 (1 run in 12).  The shape below -- four inserters committing
# 20-row batches while one session VACUUMs every 50 ms -- corrupted the index in
# 6 of 12 ten-second rounds on the unfixed build and 0 of 30 on the fixed one.
#
# Each round starts from a fresh table and asserts, after the storm: no session
# saw an ERROR, index == heap for the anchor predicate, and weave_check(deep)
# is clean.  The final section is the POSITIVE CONTROL that the fix did not
# simply switch truncation off: a quiet plain VACUUM after a large DELETE must
# still shrink the index file.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);
use Time::HiRes qw(time);

use constant NROUNDS => $ENV{T028_ROUNDS} // 8;
use constant SECS => 5;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
# replica, not the TAP default minimal: acquiring AccessExclusiveLock is logged for
# hot standby only then, and that logging is what assigns the xid the parallel-
# VACUUM section below depends on.
$node->append_conf('postgresql.conf', "wal_level = replica\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave');

my $conn = $node->connstr('postgres');

sub session
{
	my ($name, @args) = @_;
	my ($in, $out, $err) = ('', '', '');
	my $h = start(['psql', '-X', '-q', '-d', $conn, @args],
				  '<', \$in, '>', \$out, '2>', \$err);
	return { name => $name, h => $h, out => \$out, err => \$err };
}

sub fresh_table
{
	$node->safe_psql('postgres', q{
		DROP TABLE IF EXISTS t;
		CREATE TABLE t (id bigserial, body wdoc, cat text COLLATE "C")
		    WITH (autovacuum_enabled = off);
		INSERT INTO t(body, cat) SELECT to_wdoc('common w' || (g % 50)),
		  CASE WHEN g % 13 = 0 THEN NULL
		       ELSE chr(97 + g % 26) || lpad((g % 97)::text, 2, '0') END
		  FROM generate_series(1, 3000) g;
		CREATE INDEX w ON t USING weave (body wdoc_lex_ops, cat text_docval_ops);
	});
}

sub idx_count
{
	my ($pred) = @_;
	return $node->safe_psql('postgres', qq{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM t WHERE $pred});
}

sub heap_count
{
	my ($pred) = @_;
	return $node->safe_psql('postgres', qq{
		SET enable_indexscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM t WHERE $pred});
}

for my $round (1 .. NROUNDS)
{
	fresh_table();
	my $end = sprintf('%.3f', time() + SECS);
	my @s;
	for my $i (1 .. 4)
	{
		push @s, session("inserter $i", '-c', qq{
DO \$\$ BEGIN
  WHILE extract(epoch FROM clock_timestamp()) < $end LOOP
    INSERT INTO t(body, cat) SELECT to_wdoc('common ins$i w' || (g % 50)),
      CASE WHEN g % 11 = 0 THEN NULL
           ELSE chr(97 + (g * $i) % 26) || lpad((g % 97)::text, 2, '0') END
      FROM generate_series(1, 20) g;
    COMMIT;
  END LOOP;
END \$\$});
	}
	# The VACUUM loop opens a NEW backend for every VACUUM.  Measured: with all
	# VACUUMs in one session this shape caught the unfixed build in about half
	# the runs; a fresh backend per VACUUM (the stress shape that found the
	# mechanism) caught it in about half the ROUNDS.
	my ($vin, $vout, $verr) = ('', '', '');
	my $vh = start(['bash', '-c',
			qq{while [ \$(date +%s.%N | cut -c1-14) \\< $end ]; do }
		  . qq{psql -X -q -h '} . $node->host . q{' -p } . $node->port
		  . qq{ -d postgres -c 'VACUUM t' || exit 1; echo v; sleep 0.05; done}],
		'<', \$vin, '>', \$vout, '2>', \$verr);
	push @s, { name => 'vacuum', h => $vh, out => \$vout, err => \$verr };

	# A reader, index-forced.  It is part of the race, not only an observer: a
	# block it reads between RelationTruncate()'s buffer drop and the file
	# truncate is what leaves a valid buffer past EOF.  Index and heap counts
	# come from ONE statement, so one snapshot.
	push @s, session('reader', '-c', qq{
DO \$\$ DECLARE ix bigint; hp bigint; bad int := 0; BEGIN
  PERFORM set_config('enable_seqscan', 'off', false);
  PERFORM set_config('enable_bitmapscan', 'off', false);
  WHILE extract(epoch FROM clock_timestamp()) < $end LOOP
    SELECT a.c, b.c INTO ix, hp
      FROM (SELECT count(*) c FROM t WHERE cat < 'k') a,
           (SELECT count(*) FILTER (WHERE cat < 'k') c FROM t
             WHERE ctid >= '(0,0)'::tid) b;
    IF ix <> hp THEN
      bad := bad + 1;
      IF bad <= 5 THEN RAISE WARNING 'T028_DISAGREE index=% heap=%', ix, hp; END IF;
    END IF;
    COMMIT;
  END LOOP;
  IF bad > 0 THEN RAISE EXCEPTION 'reader saw % index/heap disagreements', bad; END IF;
END \$\$});

	my $deadline = time() + SECS + 120;
	while (time() < $deadline)
	{
		my $live = 0;
		for my $x (@s)
		{
			next unless $x->{h}->pumpable;
			$live++;
			$x->{h}->pump_nb;
		}
		last if $live == 0;
		select(undef, undef, undef, 0.05);
	}
	my $hung = grep { $_->{h}->pumpable } @s;
	is($hung, 0, "round $round: every session finished");
	$_->{h}->kill_kill for grep { $_->{h}->pumpable } @s;
	finish($_->{h}) for @s;

	for my $x (@s)
	{
		unlike(${ $x->{err} }, qr/\bERROR:/,
			"round $round: $x->{name} saw no ERROR")
		  or diag(${ $x->{err} });
	}

	# HARNESS POSITIVE CONTROL: the VACUUM loop really ran.  A quoting slip in
	# this loop once made it exit on its first test, and every round then
	# "passed" with no VACUUM beside the inserters at all.
	my $nvac = () = $vout =~ /^v$/mg;
	cmp_ok($nvac, '>=', 10, "round $round: the VACUUM loop ran ($nvac VACUUMs)");
	unlike($verr, qr/\S/, "round $round: the VACUUM loop wrote nothing to stderr")
	  or diag($verr);

	my $nrows = $node->safe_psql('postgres',
		q{SELECT count(*) FROM t WHERE ctid >= '(0,0)'::tid});
	cmp_ok($nrows, '>', 3000, "round $round: the inserters committed rows ($nrows)");

	for my $pred ("cat < 'k'", "cat >= 'k'", "cat = 'c07'")
	{
		is(idx_count($pred), heap_count($pred),
			"round $round: index == heap for $pred");
	}
	is($node->safe_psql('postgres',
			q{SELECT count(*) FROM weave_check('w', true) WHERE NOT ok}),
		'0', "round $round: weave_check deep is clean");
	note("t/028 round $round done: $nrows rows");
}

# POSITIVE CONTROL: with nobody else in the index, plain VACUUM (not
# weave_vacuum, not VACUUM FULL) still gets its AccessExclusiveLock
# conditionally and truncates the freed tail.
$node->safe_psql('postgres', q{DELETE FROM t WHERE id % 10 <> 0});
my $before = $node->safe_psql('postgres', q{SELECT pg_relation_size('w') / 8192});
$node->safe_psql('postgres', 'VACUUM t') for 1 .. 3;
my $after = $node->safe_psql('postgres', q{SELECT pg_relation_size('w') / 8192});
cmp_ok($after, '<', $before,
	"quiet plain VACUUM still truncates the index ($before -> $after blocks)");
for my $pred ("cat < 'k'", "cat >= 'k'")
{
	is(idx_count($pred), heap_count($pred), "after truncation: index == heap for $pred");
}
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('w', true) WHERE NOT ok}),
	'0', 'after truncation: weave_check deep is clean');

# PARALLEL VACUUM (review of G67).  Acquiring AccessExclusiveLock assigns an
# xid (for the hot-standby lock record) before it checks for conflicts, and
# assigning an xid in parallel mode is an ERROR.  weave is
# VACUUM_OPTION_NO_PARALLEL, so a parallel VACUUM runs weave's cleanup in the
# leader while still in parallel mode.  With two parallel-capable btrees above
# min_parallel_index_scan_size and a weave index with enough free space for the
# compaction to run, every such VACUUM used to abort.  POSITIVE CONTROL: VERBOSE
# must report launched parallel workers, or the section tested nothing.
$node->safe_psql('postgres', q{
	CREATE TABLE p (id int, body wdoc, k1 int, k2 int) WITH (autovacuum_enabled = off);
	INSERT INTO p SELECT g, to_wdoc('common w' || g), g, g FROM generate_series(1, 60000) g;
	CREATE INDEX p_k1 ON p (k1);
	CREATE INDEX p_k2 ON p (k2);
	CREATE INDEX p_w ON p USING weave (body wdoc_lex_ops);
	INSERT INTO p SELECT g, to_wdoc('common x' || g), g, g
	  FROM generate_series(100000, 160000) g;
});
# the flush of 60000 pending rows is itself a VACUUM that reaches compaction
# (measured: that one already aborted on the unfixed build), so it is asserted too
my $launched = 0;
for my $pass (1 .. 2)
{
	$node->safe_psql('postgres', q{DELETE FROM p WHERE id % 10 <> 0}) if $pass == 2;
	my ($rc, $out, $err) = $node->psql('postgres',
		'VACUUM (PARALLEL 2, VERBOSE) p', on_error_stop => 0);
	is($rc, 0, "parallel VACUUM pass $pass succeeds") or diag($err);
	unlike($err, qr/\bERROR:/, "parallel VACUUM pass $pass raised no ERROR");
	$launched++ if $err =~ /launched [1-9]\d* parallel vacuum worker/;
}
my $pfreed = $node->safe_psql('postgres',
	q{SELECT count(*) FILTER (WHERE freed) * 4 > count(*) FROM weave_page_info('p_w')});
is($pfreed, 't', 'the parallel VACUUM reached a weave index over a quarter free (the compaction trigger)');
cmp_ok($launched, '>=', 1,
	"the VACUUMs really ran in parallel mode ($launched of 2 launched workers)");
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM weave_check('p_w', true) WHERE NOT ok}),
	'0', 'after parallel VACUUMs: weave_check deep is clean');
is($node->safe_psql('postgres', q{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM p WHERE body @@@ 'common'::wquery}),
	$node->safe_psql('postgres', q{SELECT count(*) FROM p WHERE ctid >= '(0,0)'::tid}),
	'after parallel VACUUMs: index == heap');

$node->stop;
done_testing();
