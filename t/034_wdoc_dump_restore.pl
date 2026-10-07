
# Copyright (c) 2026, PostgreSQL Global Development Group

# doc/GAPS.md G89 and G90: a stored wdoc column survives pg_dump and restore.
#
# pg_dump's table data is text COPY in every archive format, so a wdoc column
# is written by wdoc_out and restored by wdoc_in; both -Fp and -Fc are run.
# The binary form (wdoc_send / wdoc_recv) is covered by
# sql/wdoc_roundtrip.sql's COPY (FORMAT binary).
#
# Before the fix, the restore of a table holding one to_wdoc(regconfig, text)
# value of more than 16,383 tokens FAILED on that table's COPY (G89), and a
# stopword config's document length came back as the sum of tf, so BM25
# re-ranked the restored rows (G90).  This asserts the restored values are
# byte-identical, the restored index answers, and ORDER BY d <=> q returns the
# same rows with the same scores as the source.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('wdoc_dump');
$node->init;
$node->append_conf('postgresql.conf', "fsync = off\n");
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('src', 'CREATE EXTENSION pg_weave');

# Short english docs (stopwords: length != sum of tf), long docs past 16,383
# tokens with recurring terms (the G89 shape), weighted concatenations.  Every
# document has a distinct length, so no two BM25 scores tie and the top-k order
# is fully determined.
$node->safe_psql('src', q{
	CREATE TABLE docs (id int PRIMARY KEY, d wdoc);
	INSERT INTO docs
	  SELECT g, to_wdoc('english',
	                    'the vacuum of the ' || repeat('the ', g) || repeat('lock ', g % 5)
	                    || 'is a common' || (g % 7) || ' tag' || g)
	    FROM generate_series(1, 300) g;
	INSERT INTO docs
	  SELECT 1000 + g, to_wdoc('english',
	                    repeat('the vacuum runs on a lock ', 3000 + 400 * g))
	    FROM generate_series(1, 4) g;
	INSERT INTO docs
	  SELECT 2000 + g, to_wdoc('english', 'vacuum lock ' || g, 'A')
	                   || to_wdoc('english', repeat('the vacuum ', 9000 + g), 'C')
	    FROM generate_series(1, 3) g;
	CREATE INDEX docs_w ON docs USING weave (d) WITH (positions = on);
});

my $nlong = $node->safe_psql('src',
	q{SELECT count(*) FROM docs WHERE wdoc_length(d) > 16383});
is($nlong, '7', 'control: seven source documents are longer than 16,383 tokens');
my $nstop = $node->safe_psql('src',
	q{SELECT count(*) FROM docs WHERE id <= 300
	   AND wdoc_length(d) <> wdoc_length(d::text::wdoc)});
is($nstop, '0', 'the text form keeps the stopword-inclusive length (G90)');

# What the source answers, before anything moves.
my $fingerprint = q{
	SELECT string_agg(id || ':' || md5(wdoc_send(d)::text), ',' ORDER BY id)
	  FROM docs};
my $topk = q{
	SET enable_seqscan = off;
	SELECT id || ':' || round((d <=> 'vacuum lock'::wquery)::numeric, 9)
	  FROM docs ORDER BY d <=> 'vacuum lock'::wquery LIMIT 20};
my $plan = q{
	SET enable_seqscan = off;
	EXPLAIN (COSTS OFF)
	SELECT id FROM docs ORDER BY d <=> 'vacuum lock'::wquery LIMIT 20};
my $phrase = q{SELECT weave_count('docs_w', '"vacuum run"'::wquery)};

my $src_fp = $node->safe_psql('src', $fingerprint);
my $src_topk = $node->safe_psql('src', $topk);
my $src_phrase = $node->safe_psql('src', $phrase);
is(scalar(split /\n/, $src_topk), 20, 'control: the source top-k has 20 rows');
like($node->safe_psql('src', $plan), qr/Index Scan using docs_w/,
	'control: the source ORDER BY uses the weave index');
is($src_phrase, '4', 'control: the phrase "vacuum run" is in the four long documents');

my $tmp = PostgreSQL::Test::Utils::tempdir;
foreach my $fmt ('p', 'c')
{
	my $db = "dst_$fmt";
	my $file = "$tmp/dump.$fmt";

	$node->command_ok(
		[ 'pg_dump', '-F', $fmt, '-f', $file, '-d', $node->connstr('src') ],
		"pg_dump -F$fmt");
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	if ($fmt eq 'p')
	{
		$node->command_ok(
			[ 'psql', '-X', '-v', 'ON_ERROR_STOP=1', '-q', '-f', $file,
			  '-d', $node->connstr($db) ],
			"restore -F$fmt (psql, ON_ERROR_STOP)");
	}
	else
	{
		$node->command_ok(
			[ 'pg_restore', '--exit-on-error', '-d', $node->connstr($db), $file ],
			"restore -F$fmt (pg_restore --exit-on-error)");
	}

	is($node->safe_psql($db, 'SELECT count(*) FROM docs'), '307',
		"-F$fmt: every row restored");
	is($node->safe_psql($db, $fingerprint), $src_fp,
		"-F$fmt: every restored wdoc is byte-identical to its source");
	like($node->safe_psql($db, $plan), qr/Index Scan using docs_w/,
		"-F$fmt: the restored ORDER BY uses the restored weave index");
	is($node->safe_psql($db, $topk), $src_topk,
		"-F$fmt: ORDER BY d <=> q returns the same rows and scores after restore");
	is($node->safe_psql($db, $phrase), $src_phrase,
		"-F$fmt: phrase count from the restored index matches the source");
}

$node->stop;
done_testing();
