
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

# Short english docs with stopwords, long docs past 16,383 lexeme-bearing
# tokens with recurring terms (the G89 shape), weighted concatenations.  Every
# document has a distinct length (doclen counts tokens that produced a lexeme,
# doc/PHASES.md M7: g + g % 5 is injective), so no two BM25 scores tie and the
# top-k order is fully determined.
$node->safe_psql('src', q{
	CREATE TABLE docs (id int PRIMARY KEY, d wdoc);
	INSERT INTO docs
	  SELECT g, to_wdoc('english',
	                    'the vacuum of the ' || repeat('the bolt ', g) || repeat('lock ', g % 5)
	                    || 'is a common' || (g % 7) || ' tag' || g)
	    FROM generate_series(1, 300) g;
	INSERT INTO docs
	  SELECT 1000 + g, to_wdoc('english',
	                    repeat('the vacuum runs on a lock ', 6000 + 400 * g))
	    FROM generate_series(1, 4) g;
	INSERT INTO docs
	  SELECT 2000 + g, to_wdoc('english', 'vacuum lock ' || g, 'A')
	                   || to_wdoc('english', repeat('the vacuum ', 17000 + g), 'C')
	    FROM generate_series(1, 3) g;
	CREATE INDEX docs_w ON docs USING weave (d) WITH (positions = on);
});

# doc/GAPS.md G96: a stored wquery column survives the same text COPY.  One
# row per item kind and flag, the exact gaps through the tsquery cast (its
# only producer besides binary input).  Before G96 the phrases, prefix,
# fuzzy and weighted rows restored as different queries.
$node->safe_psql('src', q{
	CREATE TABLE wq (id int PRIMARY KEY, q wquery);
	INSERT INTO wq VALUES
	  (1, 'quick & (brown | !fox)'), (2, '"quick brown fox"'),
	  (3, 'NEAR(quick brown fox, 3)'), (4, 'fo* & qu*'),
	  (5, 'brwn~1 | brown~3'), (6, 'fox:A & dog:BD'), (7, '/^fo+x$/ | dog'),
	  (8, ''), (9, $$'it\'s' & 'Fox' & 'and'$$),
	  (10, 'quick <2> (brown <-> fox)'), (11, '(quick <0> brown) <3> fox'),
	  (12, '!(quick <=2> brown) & NEAR(a b, 1)');
	INSERT INTO wq SELECT 100 + i, q::tsquery::wquery
	  FROM unnest(ARRAY['quick <-> brown', 'quick <2> brown', 'fox:A <0> dog',
	                    '(a <3> b) <-> (c <-> d)', 'fo:* & !dog:C']) WITH ORDINALITY u(q, i);
});
my $wq_fp = q{
	SELECT string_agg(id || ':' || md5(wquery_send(q)::text), ',' ORDER BY id)
	  FROM wq};
my $src_wq = $node->safe_psql('src', $wq_fp);
is($node->safe_psql('src', q{SELECT count(*) FROM wq
	WHERE wquery_send(q::text::wquery) = wquery_send(q)}), '17',
	'control: every source wquery round-trips through its text in place');

my $nlong = $node->safe_psql('src',
	q{SELECT count(*) FROM docs WHERE wdoc_length(d) > 16383});
is($nlong, '7', 'control: seven source documents are longer than 16,383 tokens');
my $nstop = $node->safe_psql('src',
	q{SELECT count(*) FROM docs WHERE id <= 300
	   AND wdoc_length(d) <> wdoc_length(d::text::wdoc)});
is($nstop, '0', 'the text form keeps the length (G90)');

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
	is($node->safe_psql($db, $wq_fp), $src_wq,
		"-F$fmt: every restored wquery is byte-identical to its source (G96)");
}

$node->stop;
done_testing();
