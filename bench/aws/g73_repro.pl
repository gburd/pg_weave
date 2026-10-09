# Copyright (c) 2026, pg_weave contributors
#
# bench/aws/g73_repro.pl -- doc/GAPS.md G73 reproducer: t/028's truncation
# control ("quiet plain VACUUM still truncates the index"), repeated.
#
# Each round is t/028's last round plus its control: a fresh table, one 5 s storm
# (four inserters committing 20-row batches beside a VACUUM loop, a new backend
# per VACUUM), then DELETE 90 % and three plain VACUUMs, each in its own backend
# at client_min_messages = debug2 so the recyclability probe's G73 line (am.c,
# weave_any_free_page_recyclable) and the cleanup trigger's line come back on
# stderr.  A round FAILS exactly as t/028 test 113 does: the file is not smaller
# after the three VACUUMs than before them.
#
# ARMS (env G73_ARMS, cycled round by round, default "none burn"):
#   none  the control as t/028 runs it
#   burn  a separate transaction spends an xid before EACH of the three VACUUMs
#
# Output: one "G73R" line per round, and every probe / trigger line, as notes.
# The job script counts G73R lines, so a round that did not run is visible.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);
use Time::HiRes qw(time);

my $nrounds = $ENV{G73_ROUNDS} // 12;
my @arms = split ' ', ($ENV{G73_ARMS} // 'none burn');
my $secs = $ENV{G73_SECS} // 5;	# t/028 uses 5; longer storms leave bigger bolts
my $baserows = $ENV{G73_BASEROWS} // 3000;	# t/028 uses 3000: the CREATE INDEX segment's size

my $node = PostgreSQL::Test::Cluster->new('g73');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\nautovacuum = off\nwal_level = replica\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_weave; CREATE EXTENSION pg_freespacemap');
my $conn = $node->connstr('postgres');

sub session
{
	my ($name, @args) = @_;
	my ($in, $out, $err) = ('', '', '');
	my $h = start(['psql', '-X', '-q', '-d', $conn, @args],
				  '<', \$in, '>', \$out, '2>', \$err);
	return { name => $name, h => $h, out => \$out, err => \$err };
}

my %tally;
for my $round (1 .. $nrounds)
{
	my $arm = $arms[($round - 1) % @arms];
	$node->safe_psql('postgres', qq{
		DROP TABLE IF EXISTS t;
		CREATE TABLE t (id bigserial, body wdoc, cat text COLLATE "C")
		    WITH (autovacuum_enabled = off);
		INSERT INTO t(body, cat) SELECT to_wdoc('common w' || (g % 50)),
		  CASE WHEN g % 13 = 0 THEN NULL
		       ELSE chr(97 + g % 26) || lpad((g % 97)::text, 2, '0') END
		  FROM generate_series(1, $baserows) g;
		CREATE INDEX w ON t USING weave (body wdoc_lex_ops, cat text_docval_ops);
	});
	my $end = sprintf('%.3f', time() + $secs);
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
	my ($vin, $vout, $verr) = ('', '', '');
	my $vh = start(['bash', '-c',
			qq{while [ \$(date +%s.%N | cut -c1-14) \\< $end ]; do }
		  . qq{psql -X -q -h '} . $node->host . q{' -p } . $node->port
		  . qq{ -d postgres -c 'VACUUM t' || exit 1; echo v; sleep 0.05; done}],
		'<', \$vin, '>', \$vout, '2>', \$verr);
	push @s, { name => 'vacuum', h => $vh, out => \$vout, err => \$verr };
	my $deadline = time() + $secs + 120;
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
	$_->{h}->kill_kill for grep { $_->{h}->pumpable } @s;
	finish($_->{h}) for @s;
	my $nvac = () = $vout =~ /^v$/mg;
	my $errs = join('', map { ${ $_->{err} } } @s);

	$node->safe_psql('postgres', q{DELETE FROM t WHERE id % 10 <> 0});
	my $before = $node->safe_psql('postgres', q{SELECT pg_relation_size('w') / 8192});
	my @sizes;
	for my $v (1 .. 3)
	{
		$node->safe_psql('postgres', 'SELECT txid_current()') if $arm eq 'burn';
		my ($vrc, $o, $e) = $node->psql('postgres',
			"SET client_min_messages = debug2;\nVACUUM t;");
		push @sizes, $node->safe_psql('postgres', q{SELECT pg_relation_size('w') / 8192});
		for my $l (split /\n/, $e)
		{
			note("G73L round=$round arm=$arm vac=$v $1")
			  if $l =~ /(G73 recycle probe .*|cleanup trigger: .*|no free page recyclable yet.*|bolts, no tombstones.*)/;
		}
		note("G73L round=$round arm=$arm vac=$v ERROR $e") if $e =~ /ERROR/;
	}
	my $res = $sizes[-1] < $before ? 'PASS' : 'FAIL';
	$tally{"$arm $res"}++;
	note("G73R round=$round arm=$arm secs=$secs rows=$baserows nvac=$nvac before=$before after=" . join(',', @sizes)
	  . " result=$res" . ($errs =~ /ERROR/ ? ' storm_error' : ''));
	cmp_ok($nvac, '>=', 10, "round $round: the storm's VACUUM loop ran ($nvac)");
}
note("G73T $_ $tally{$_}") for sort keys %tally;
$node->stop;
done_testing();
