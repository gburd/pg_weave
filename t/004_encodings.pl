# t/004_encodings.pl -- pg_weave correctness on non-UTF-8 SERVER encodings.
#
# A PostgreSQL database has ONE server encoding, chosen at initdb.  The main
# regression suite runs on a UTF-8 database (sql/pg_weave.sql covers the UTF-8
# multi-script + corner cases).  This TAP test covers what a single-encoding
# regression DB cannot: server encodings that are NOT UTF-8.
#
# Two things are gated here, both of which a real deployment hits and neither of
# which the UTF-8 suite can catch:
#   1. CREATE EXTENSION pg_weave must SUCCEED on a non-UTF-8 server.  A non-ASCII
#      byte anywhere in the install SQL (historically a UTF-8 ellipsis default
#      on weave_snippet) makes it fail with "invalid byte sequence for encoding".
#      `make check-ascii` guards the source; this proves the actual install.
#   2. pg_weave's @@@ match set must EQUAL PostgreSQL's native to_tsvector @@ on
#      the same non-UTF-8 database.  pg_weave's built-in fold_token has a
#      byte-wise (ASCII-only) fold fallback for non-UTF-8 encodings and the
#      regconfig path uses the encoding-aware parsetext(); this proves neither
#      corrupts high bytes (LATIN1 0x80-0xFF) or multibyte lead/trail bytes
#      (EUC_JP), including the trap where a multibyte char's trailing byte falls
#      in the ASCII token range.
#
# Ground truth is native to_tsvector('simple', body) @@ to_tsquery('simple', q):
# pg_weave's regconfig analyzer IS PostgreSQL's tokenizer, so any divergence is a
# pg_weave encoding bug, not an analyzer difference.  Content is given as Perl
# byte strings already in the target encoding, so this file itself stays ASCII
# (check-ascii clean) while inserting real high/multibyte bytes.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub qesc { my $s = shift; $s =~ s/'/''/g; return $s; }

# ($enc): initdb a server of that encoding, install pg_weave, load probe bodies
# (bytes already in that encoding), build a weave index, and assert pg_weave @@@
# count == native to_tsvector @@ count for each probe term.
sub run_encoding_case
{
	my ($enc, $probes) = @_;

	my $node = PostgreSQL::Test::Cluster->new("enc_$enc");
	$node->init(extra => [ '--encoding', $enc, '--locale', 'C' ]);
	$node->start;

	# (1) install must succeed on this server encoding (the ASCII-install gate)
	my ($rc, $out, $err) = $node->psql('postgres', 'CREATE EXTENSION pg_weave');
	is($rc, 0, "$enc: CREATE EXTENSION pg_weave succeeds")
	  or diag("$enc CREATE EXTENSION stderr: $err");

	if ($rc == 0)
	{
		$node->safe_psql('postgres', 'CREATE TABLE d (id int, body text)');

		my $i = 0;
		my @rows;
		for my $p (@$probes)
		{
			$i++;
			push @rows, "($i,'" . qesc($p->[0]) . "')";
		}
		$node->safe_psql('postgres',
			'INSERT INTO d VALUES ' . join(',', @rows));
		$node->safe_psql('postgres',
			"CREATE INDEX d_weave ON d USING weave (to_wdoc('simple', body))");

		# (2) per-probe: pg_weave @@@ (forced index scan) == native to_tsvector @@
		for my $p (@$probes)
		{
			my $term = qesc($p->[1]);

			my $fts = $node->safe_psql('postgres',
				    "SET enable_seqscan=off; SET enable_bitmapscan=off; "
				  . "SELECT count(*) FROM d "
				  . "WHERE to_wdoc('simple',body) \@\@\@ to_wquery('simple','$term')");
			my $native = $node->safe_psql('postgres',
				    "SELECT count(*) FROM d "
				  . "WHERE to_tsvector('simple',body) \@\@ to_tsquery('simple','$term')");

			is($fts, $native,
				"$enc: pg_weave count == native for a term in '$p->[1]' (got $fts)");
		}
	}

	$node->stop;
}

# --- LATIN1 (ISO-8859-1): single-byte high chars 0x80-0xFF ------------------
# 0xE9=e-acute 0xFC=u-umlaut 0xDF=sharp-s 0xE8=e-grave 0xE7=c-cedilla.
run_encoding_case(
	'LATIN1',
	[
		[ "caf\xe9 soci\xe9t\xe9", "caf\xe9"        ],   # cafe societe
		[ "z\xfcrich m\xfcnchen",  "z\xfcrich"      ],   # zurich muenchen
		[ "stra\xdfe fu\xdfball",  "stra\xdfe"      ],   # strasse (sharp-s 0xDF)
		[ "gar\xe7on \xe8re",      "gar\xe7on"      ],   # garcon ere
		[ "plain ascii only",      "ascii"          ],
		[ "caf\xe9 soci\xe9t\xe9", "zzzzz"          ],   # no-match probe
	]);

# --- EUC_JP: multibyte (JIS X 0208, lead 0xA1-0xFE) -------------------------
# EUC_JP JIS X 0208 kanji as \x byte pairs: tokyo, tosho-kan, hon, yomu
run_encoding_case(
	'EUC_JP',
	[
		[ "\xc5\xec\xb5\xfe postgresql",     "\xc5\xec\xb5\xfe"         ],  # kanji + ascii
		[ "\xbf\xde\xbd\xf1\xb4\xdb de hon", "\xbf\xde\xbd\xf1\xb4\xdb" ],  # tosho-kan (library)
		[ "\xcb\xdc wo \xc6\xc9 mu",         "\xcb\xdc"                 ],  # hon (book)
		[ "english only text here",          "english"                 ],
		[ "\xc5\xec\xb5\xfe postgresql",     "zzzzz"                    ],  # no-match
	]);

done_testing();
