# Experimental real-AM physical replay probe. Never production regression code.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish timeout);
use Time::HiRes qw(time sleep);
use Cwd qw(getcwd);

my $p = PostgreSQL::Test::Cluster->new('primary');
$p->init(allows_streaming => 1);
$p->append_conf('postgresql.conf', "autovacuum=off\njit=off\n");
$p->start;
my $helper = $ENV{PROBE_HELPER};
$p->safe_psql('postgres', qq{
 CREATE EXTENSION pg_weave;
 CREATE FUNCTION probe_root(regclass) RETURNS bigint AS '$helper','probe_root' LANGUAGE C STRICT;
 CREATE FUNCTION probe_page(regclass,bigint) RETURNS text AS '$helper','probe_page' LANGUAGE C STRICT;
 CREATE FUNCTION probe_fence(regclass) RETURNS bool AS '$helper','probe_fence' LANGUAGE C STRICT;
 CREATE FUNCTION probe_hold(regclass,int) RETURNS void AS '$helper','probe_hold' LANGUAGE C STRICT;
});
$p->backup('base');
my $s = PostgreSQL::Test::Cluster->new('standby');
$s->init_from_backup($p, 'base', has_streaming => 1);
$s->append_conf('postgresql.conf', "hot_standby_feedback=off\nmax_standby_streaming_delay=-1\njit=off\n");
$s->start;

for my $arm ('none', 'wait', 'cancel')
{
 # A finite deadline must be in force BEFORE startup enters the lock wait.
 # Reloading -1 -> finite during an existing wait does not arm its timer.
 if ($arm eq 'cancel')
 {
  $s->stop;
  $s->append_conf('postgresql.conf', "max_standby_streaming_delay='10s'\n");
  $s->start;
  is($s->safe_psql('postgres', 'SHOW max_standby_streaming_delay'), '10s',
   'CANCEL: finite delay configured before conflict');
 }
 my $tab = "docs_$arm";
 my $idx = "idx_$arm";
 $p->safe_psql('postgres', qq{
  CREATE TABLE $tab(id int, d wdoc) WITH (autovacuum_enabled=false);
  INSERT INTO $tab SELECT g,to_wdoc('alpha token ' || g) FROM generate_series(1,300) g;
  CREATE INDEX $idx ON $tab USING weave(d);
 });
 $p->wait_for_catchup($s);
 my $gate = getcwd() . "/pause-$arm";
 open(my $gf, '>', $gate) or die $!; close $gf;
 my ($out, $err) = ('', '');
 # Two terms defeat count's dictionary-df fast path. The marker proves this.
 my $h = start(['psql','-XAt','-v','ON_ERROR_STOP=1','-d',$s->connstr('postgres'),
  '-c', "SET pg_weave.probe_pause_file='$gate'; SELECT weave_count('$idx','alpha & token'::wquery);"],
  '>' => \$out, '2>' => \$err, timeout(120));
 my $deadline = time() + 30;
 while ($err !~ /PROBE REAL SCAN CAPTURE/ && time() < $deadline)
 { $h->pump_nb; sleep 0.01; }
 like($err, qr/PROBE REAL SCAN CAPTURE/, "$arm real scan capture site BUILT AND REACHED");
 my ($pid, $oid, $root) = $err =~ /CAPTURE pid=(\d+) index=(\d+) root=(\d+)/;
 die "no real capture witness: $err" unless defined $root;
 is($s->safe_psql('postgres', "SELECT EXISTS(SELECT FROM pg_locks WHERE pid=$pid AND relation=$oid AND mode='AccessShareLock' AND granted)"), 't', "$arm real scan holds index ASL");
 my $before = $s->safe_psql('postgres', "SELECT probe_page('$idx',$root)");
 $p->safe_psql('postgres', "INSERT INTO $tab VALUES(301,to_wdoc('beta')); SELECT weave_merge('$idx');");
 my $retired = $p->safe_psql('postgres', "SELECT probe_page('$idx',$root)");
 my @ret = split /:/, $retired;
 isnt($ret[1], '0', "$arm target really retired by merge");
 my $current = $p->safe_psql('postgres', "SELECT probe_root('$idx')");
 isnt($current, $root, "$arm ownership removed before fence");
 if ($arm ne 'none')
 {
  is($p->safe_psql('postgres', "SELECT probe_fence('$idx')"), 't', "$arm conditional ordinary index AEL acquired");
  ok($s->poll_query_until('postgres', "SELECT EXISTS(SELECT FROM pg_locks WHERE relation=$oid AND mode='AccessExclusiveLock' AND NOT granted)"), "$arm startup waits for real scan index ASL");
 }
 $p->safe_psql('postgres', 'SELECT txid_current()');
 $p->safe_psql('postgres', 'SELECT txid_current()');
 my $start = $p->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
 my ($rc, $unused, $reuseerr) = $p->psql('postgres', "SET pg_weave.probe_target='$root'; INSERT INTO $tab VALUES(302,to_wdoc('gamma'));");
 is($rc, 0, "$arm later INSERT passed real recycle gate");
 like($reuseerr, qr/PROBE REAL REUSE target=$root xid=\d+/, "$arm experimental allocator BUILT AND REACHED");
 my $end = $p->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
 my $after = $p->safe_psql('postgres', "SELECT probe_page('$idx',$root)");
 my @aft = split /:/, $after;
 is($aft[1], '0', "$arm reused target live");
 is($ret[0], '2', "$arm retired target DICT");
 is($aft[0], '36', "$arm replacement PENDING v10");
 isnt($aft[2], $ret[2], "$arm target WAL LSN changed");
 diag("WITNESS $arm pid=$pid target=$root before=$before retired=$retired after=$after insert_wal=$start..$end");
 my ($walout,$walerr);
 ok(IPC::Run::run(['pg_waldump','-p',$p->data_dir.'/pg_wal','-s',$start,'-e',$end], '>' => \$walout, '2>' => \$walerr), "$arm WAL dump readable");
 like($walout, qr/blk $root\b/, "$arm WAL contains exact reused block");
 open(my $wf,'>',"wal-$arm.txt") or die $!; print $wf $walout; close $wf;
 if ($arm eq 'none')
 {
  $p->wait_for_catchup($s);
  is($s->safe_psql('postgres', "SELECT probe_page('$idx',$root)"), $after, 'NO FENCE: actual replacement while real scan paused');
  is($s->safe_psql('postgres', "SELECT EXISTS(SELECT FROM pg_locks WHERE pid=$pid AND relation=$oid AND mode='AccessShareLock' AND granted)"), 't', 'NO FENCE: real scan still retains ASL after replacement');
  is($s->safe_psql('postgres', "SELECT pg_cancel_backend($pid)"), 't', 'NO FENCE: cancel before any stale payload parse');
 }
 else
 {
  ok($s->poll_query_until('postgres', "SELECT pg_last_wal_receive_lsn() >= '$end'::pg_lsn"), "$arm received reuse WAL while replay blocked");
  is($s->safe_psql('postgres', "SELECT pg_last_wal_replay_lsn() < '$start'::pg_lsn"), 't', "$arm reuse not replayed with scan paused");
  if ($arm eq 'wait') { unlink($gate) or die $!; }
 }
 my $scanok = finish($h);
 if ($arm eq 'wait')
 {
  ok($scanok, 'WAIT: released real scan completes');
  like($out, qr/^300$/m, 'WAIT: exact real count result');
  like($err, qr/PROBE REAL SCAN RELEASE/, 'WAIT: real pause released');
 }
 else
 {
  ok(!$scanok, "$arm paused real query canceled");
  like($err, $arm eq 'none' ? qr/canceling statement due to user request/ : qr/canceling statement due to conflict with recovery/, "$arm expected cancellation cause");
  unlike($err, qr/PROBE REAL SCAN RELEASE/, "$arm never resumed stale parser");
  unlink($gate) or die $!;
 }
 diag("SCAN $arm stdout=$out stderr=$err");
 $p->wait_for_catchup($s);
 if ($arm eq 'cancel')
 {
  is($s->safe_psql('postgres', "SELECT confl_lock > 0 FROM pg_stat_database_conflicts WHERE datname='postgres'"), 't', 'CANCEL: core recovery lock-conflict counter incremented');
 }
 is($s->safe_psql('postgres', "SELECT probe_root('$idx')"), $current, "$arm fresh reader follows new root");
 is($s->safe_psql('postgres', "SELECT probe_page('$idx',$root)"), $after, "$arm replacement replay completed");
 is($s->safe_psql('postgres', "SELECT weave_count('$idx','gamma'::wquery)"), '1', "$arm fresh real query sees later INSERT");
}

# Primary acquisition is conditional, not a progress guarantee.
for my $mode (1, 3) # AccessShareLock, RowExclusiveLock
{
 my $r = $p->background_psql('postgres');
 $r->query_safe("BEGIN; SELECT probe_hold('idx_wait',$mode)");
 is($p->safe_psql('postgres', "SELECT probe_fence('idx_wait')"), 'f', "PRIMARY mode=$mode denies conditional AEL");
 $r->query_safe('COMMIT'); $r->quit;
 is($p->safe_psql('postgres', "SELECT probe_fence('idx_wait')"), 't', "PRIMARY mode=$mode release permits conditional AEL");
}

# A real primary cursor is self-compatible with AEL; the horizon must remain.
my $plan = $p->safe_psql('postgres', q{SET enable_seqscan=off; SET enable_bitmapscan=off;
 EXPLAIN (COSTS OFF) SELECT id FROM docs_wait WHERE d @@@ 'alpha & token'::wquery;});
like($plan, qr/Index Scan using idx_wait/, 'SELF: cursor query uses real weave index scan');
my $r = $p->background_psql('postgres', on_error_stop => 0);
$r->query_safe(q{BEGIN; SET enable_seqscan=off; SET enable_bitmapscan=off;
 DECLARE c CURSOR FOR SELECT id FROM docs_wait WHERE d @@@ 'alpha & token'::wquery;
 FETCH 1 FROM c;});
is($r->query_safe(q{SELECT EXISTS(SELECT FROM pg_locks WHERE pid=pg_backend_pid()
 AND relation='idx_wait'::regclass AND mode='AccessShareLock' AND granted)}),
 't', 'SELF: cursor retains index ASL');
is($r->query_safe(q{SELECT backend_xmin IS NOT NULL FROM pg_stat_activity
 WHERE pid=pg_backend_pid()}), 't', 'SELF: cursor retains primary snapshot horizon');
is($r->query_safe("SELECT probe_fence('idx_wait')"), 't', 'SELF: own suspended cursor does not prevent conditional AEL');
my $root = $p->safe_psql('postgres', "SELECT probe_root('idx_wait')");
$p->safe_psql('postgres', "INSERT INTO docs_wait VALUES(303,to_wdoc('delta')); SELECT weave_merge('idx_wait');");
$p->safe_psql('postgres', 'SELECT txid_current()');
$p->safe_psql('postgres', 'SELECT txid_current()');
my ($rc, $out, $err) = $p->psql('postgres', "SET pg_weave.probe_target='$root'; INSERT INTO docs_wait VALUES(304,to_wdoc('epsilon'));");
isnt($rc, 0, 'SELF: suspended primary snapshot forbids real allocator reuse');
like($err, qr/PROBE refused target $root: real recycle gate/, 'SELF: refusal is unchanged recycle gate, not lock error');
$r->query_safe('CLOSE c; COMMIT'); $r->quit;
($rc, $out, $err) = $p->psql('postgres', "SET pg_weave.probe_target='$root'; INSERT INTO docs_wait VALUES(304,to_wdoc('epsilon'));");
is($rc, 0, 'SELF: after cursor transaction release real reuse succeeds');
like($err, qr/PROBE REAL REUSE target=$root/, 'SELF: released candidate site reached');
$s->stop;
$p->stop;
diag('PROBE COMPLETE: real scan none/wait/cancel and primary locks/self-cursor executed');
done_testing();
