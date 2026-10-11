# G95: redirects, a HOT update while padding is suspended, and index metadata.
# Mutants: omit redirects; return physical TIDs; bypass the partial predicate;
# read a table column instead of evaluating the index expression.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('padding');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nfsync = off\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sql(q{
 CREATE EXTENSION pg_weave;
 ALTER EXTENSION pg_weave UPDATE;
 CREATE EXTENSION pageinspect;
 CREATE TABLE hotpad (id int, d wdoc, tag int) WITH (fillfactor=20);
 INSERT INTO hotpad SELECT g, to_wdoc('simple', ''), 0
   FROM generate_series(1,6) g;
 CREATE INDEX hotpad_w ON hotpad USING weave(d);
 CREATE TABLE roots AS SELECT id, ctid AS root FROM hotpad;
 UPDATE hotpad SET tag=1;
});
sql('VACUUM hotpad');
is(sql(q{
 SELECT count(*) FROM roots r,
   LATERAL heap_page_items(get_raw_page('hotpad',
     split_part(trim(both '()' from r.root::text), ',', 1)::int)) h
 WHERE h.lp=split_part(trim(both '()' from r.root::text), ',', 2)::int
   AND h.lp_flags=2
}), '6', 'all six original roots are LP_REDIRECT before any padding query');
is(sql(q{
 SELECT count(*) || '|' || count(DISTINCT r.id)
 FROM weave_search('hotpad_w', '!absent'::wquery, 100) s
 JOIN roots r ON r.root=s.ctid
}), '6|6', 'SRF returns each redirected root, not the visible physical TID');

my $reader = $node->background_psql('postgres');
$reader->query_safe(q{
 BEGIN;
 SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_sort=off;
 DECLARE walk NO SCROLL CURSOR FOR
   SELECT id, tag FROM hotpad WHERE d @@@ '!absent'::wquery
   ORDER BY d <=> '!absent'::wquery;
});
like($reader->query_safe(q{
 EXPLAIN (COSTS OFF) SELECT id,tag FROM hotpad WHERE d @@@ '!absent'::wquery
 ORDER BY d <=> '!absent'::wquery
}), qr/Index Scan using hotpad_w.*Order By:/s, 'suspended reader uses ordering scan');
is($reader->query_safe('FETCH 1 FROM walk'), '1|1', 'first padding row suspends walk');
# Separate backend commits while the reader retains its cursor snapshot/root list.
sql(q{UPDATE hotpad SET tag=2 WHERE id=2;
      INSERT INTO hotpad VALUES (99, to_wdoc('simple',''), 2)});
sql('VACUUM hotpad');
is(sql(q{
 SELECT count(*) FROM heap_page_items(get_raw_page('hotpad',0))
 WHERE (t_infomask2 & 32768) <> 0 AND (t_infomask2 & 16384) <> 0
}), '1', 'concurrent update really extends a HOT chain (old heap-only version retained)');
is($reader->query_safe('FETCH ALL FROM walk'), "2|1\n3|1\n4|1\n5|1\n6|1",
   'resumed walk sees old HOT version once and excludes post-snapshot insert');
$reader->query_safe('COMMIT');
$reader->quit;
is(sql('SELECT tag FROM hotpad WHERE id=2'), '2', 'writer committed new version');

# Distinct partial and expression indexes. The SRF is deliberate: the executor
# must not hide a missing AM predicate check by applying the WHERE clause itself.
for my $case ('partial', 'expression')
{
 my $key = $case eq 'partial' ? 'd' : "to_wdoc('simple', body)";
 my $pred = $case eq 'partial' ? 'WHERE eligible' : '';
 sql(qq{
   CREATE TABLE padmeta (id int, d wdoc, body text, eligible bool);
   INSERT INTO padmeta SELECT g, to_wdoc('simple','wrong'),
     CASE WHEN g=7 THEN NULL WHEN g%3=0 THEN 'fox' ELSE '' END, g%2=0
     FROM generate_series(1,12) g;
   UPDATE padmeta SET d=to_wdoc('simple',body);
   CREATE INDEX padmeta_w ON padmeta USING weave ($key) $pred;
   CREATE TABLE metaroots AS SELECT id, ctid AS root FROM padmeta;
 });
 my $want = $case eq 'partial' ? '2,4,8,10' : '1,2,4,5,8,10,11';
 is(sql(q{
   SELECT string_agg(r.id::text, ',' ORDER BY r.id)
   FROM weave_search('padmeta_w','!fox'::wquery,100) s
   JOIN metaroots r ON r.root=s.ctid
 }), $want, "G95 $case padding membership");
 my $where = $case eq 'partial' ? 'eligible AND' : '';
 my $settings = 'SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_sort=off;';
 like(sql(qq{$settings EXPLAIN (COSTS OFF) SELECT id FROM padmeta
   WHERE $where $key \@\@\@ '!fox'::wquery ORDER BY $key <=> '!fox'::wquery}),
   qr/Index Scan using padmeta_w.*Order By:/s, "$case ordering site reached");
 is(sql(qq{$settings SELECT string_agg(id::text, ',' ORDER BY id) FROM
   (SELECT id FROM padmeta WHERE $where $key \@\@\@ '!fox'::wquery
    ORDER BY $key <=> '!fox'::wquery) s}), $want, "$case ordering padding membership");
 sql('DROP TABLE padmeta, metaroots');
}
# A release build must not expose a test GUC. LOAD in THIS session matters:
# pg_settings in an unloaded backend says nothing about the installed library.
is(sql(q{LOAD 'pg_weave'; SELECT count(*) FROM pg_settings
  WHERE name LIKE 'pg_weave.test_%'}), '0', 'release library has no test-hook GUC');
$node->stop;
done_testing();
