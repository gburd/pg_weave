-- G88: approximate regular expressions, `atom{~k}`, over dictionary TOKENS.
--
-- THE RULE.  A `/re/` whose text contains "{~" is an approximate pattern and is
-- decided by TRE (POSIX ERE plus `atom{~k}`) on both sides: the index's
-- dictionary walk (weave_regex_terms, src/am/amscan.c) and the heap predicate
-- (weave_doc_has_regex, src/query/doc.c).  Every other pattern is core's ARE, as
-- before.  `{~k}` binds to the atom before it, like a quantifier: `colou?r{~1}`
-- allows one edit on the `r` alone; `(colour){~1}` on the whole word.  Each edit
-- (insert, delete, substitute) costs 1; outside the approximate atoms the match
-- is exact.  Unanchored, like `~`: the pattern may match inside a token.
--
-- THE GATE, per pattern: the index answer (weave_count(), which enters the scan
-- machinery with no executor recheck behind it), the planner's answer with
-- seqscan off, and the heap answer (to_wdoc() of the body, evaluated row by row)
-- must agree -- in an index with the trigram weft and one without.  The oracle
-- for an approximate pattern is TRE itself, so a fixed expected id list (verified
-- against TRE standalone when it was written) is part of the test; for an exact
-- pattern it is core's `~` over the same tokens.
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ra (id int PRIMARY KEY, body text, d wdoc);
INSERT INTO ra(id, body) VALUES
  (1, 'colour'), (2, 'color'), (3, 'colr'), (4, 'colouur'), (5, 'kolour'),
  (6, 'flavour'), (7, 'flavor'), (8, 'e1234'), (9, 'e1x34'), (10, 'e12345'),
  (11, 'hello'), (12, 'helo'), (13, 'hallo'), (14, 'heello'), (15, 'jello'),
  (16, 'world'), (17, 'wrld'), (18, 'worlds'), (19, 'abc'), (20, 'abd'),
  (21, 'abcd'), (22, 'xabc'), (23, 'foobaz'), (24, 'fobaz'), (25, 'barbaz'),
  (26, 'bxrbaz'), (27, 'baz'), (28, 'colur'), (29, 'hellp');
-- Filler so the weft has a vocabulary to narrow among.  No filler token matches
-- any pattern below (checked against TRE when the file was written).
INSERT INTO ra(id, body) SELECT g, 'filler token number ' || g
  FROM generate_series(100, 3099) g;
UPDATE ra SET d = to_wdoc('simple', body);
CREATE TABLE rb AS SELECT * FROM ra;
CREATE INDEX ra_on ON ra USING weave (d) WITH (trigrams = on);
CREATE INDEX rb_off ON rb USING weave (d);
ANALYZE ra;
ANALYZE rb;

CREATE FUNCTION ra_ids(tab regclass, p text) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
  SET LOCAL enable_seqscan = off;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM %s WHERE d @@@ $1::wquery', tab)
    INTO r USING '/' || p || '/';
  RETURN r;
END $$;

CREATE FUNCTION ra_heap(p text) RETURNS int[] LANGUAGE sql AS $$
  SELECT array_agg(id ORDER BY id) FROM ra
   WHERE to_wdoc('simple', body) @@@ ('/' || p || '/')::wquery $$;

-- ---- approximate patterns: expected ids from TRE, and three-way agreement ----
SELECT p,
       ra_heap(p) AS ids,
       ra_heap(p) IS NOT DISTINCT FROM expected AS heap_is_tre,
       weave_count('ra_on', ('/' || p || '/')::wquery)
         = coalesce(cardinality(ra_heap(p)), 0) AS count_on,
       weave_count('rb_off', ('/' || p || '/')::wquery)
         = coalesce(cardinality(ra_heap(p)), 0) AS count_off,
       ra_ids('ra', p) IS NOT DISTINCT FROM ra_heap(p) AS index_on,
       ra_ids('rb', p) IS NOT DISTINCT FROM ra_heap(p) AS index_off
  FROM (VALUES
    ('^col(our){~1}$',       '{1,2,4,28}'::int[]),   -- whole-token edit on "our"
    ('^col(our){~0}$',       '{1}'),                 -- {~0} is the exact pattern
    ('^(colour){~2}$',       '{1,2,3,4,5,28}'),
    ('^e1[0-9]{~1}34$',      '{8,9}'),               -- a class may be substituted
    ('^(hello){~1}$',        '{11,12,13,14,15,29}'),
    ('(hello){~1}',          '{11,12,13,14,15,29}'), -- unanchored
    ('^(foo|bar){~1}baz$',   '{23,24,25,26}'),       -- alternation inside the atom
    ('^w(or){~1}ld$',        '{16,17}'),
    ('^(abc){~1}$',          '{19,20,22}'),          -- see "TRE's insertion rule"
    ('^h(e){~1}llo$',        '{11,13,14}'),
    ('^fla(vou){~1}r$',      '{6,7}'),
    ('(z?){~1}hello',        '{11,12,13,14,15,29}'), -- nullable atom: see below
    ('^hel(lo){~1}$',        '{11,12,29}'),
    ('ello{~1}',             '{11,14,15,29}'),       -- binds to the last `o` only
    ('zzqq(xx){~1}',         NULL)                   -- nothing matches
  ) v(p, expected)
  ORDER BY p COLLATE "C";

-- {~0} BEHAVES LIKE THE EXACT PATTERN: same rows as core's `~` on the pattern
-- with the `{~0}` removed, which is an ARE pattern and takes the ARE route.
SELECT p,
       ra_heap(p) IS NOT DISTINCT FROM ra_heap(replace(p, '{~0}', '')) AS same_as_exact,
       ra_ids('ra', p) IS NOT DISTINCT FROM ra_ids('ra', replace(p, '{~0}', '')) AS index_same
  FROM (VALUES ('^col(our){~0}$'), ('(hello){~0}'), ('e1[0-9]{~0}34'),
               ('(foo|bar){~0}baz')) v(p)
  ORDER BY p COLLATE "C";

-- ---- EXACT PATTERNS ARE UNCHANGED: still ARE, still agree with core's `~` ----
SELECT p,
       ra_ids('ra', p) IS NOT DISTINCT FROM
         (SELECT array_agg(id ORDER BY id) FROM ra
           WHERE EXISTS (SELECT 1 FROM unnest(string_to_array(body, ' ')) t
                          WHERE t ~ p)) AS index_is_core,
       ra_heap(p) IS NOT DISTINCT FROM
         (SELECT array_agg(id ORDER BY id) FROM ra
           WHERE EXISTS (SELECT 1 FROM unnest(string_to_array(body, ' ')) t
                          WHERE t ~ p)) AS heap_is_core
  FROM (VALUES ('colou?r'), ('^e12[0-9]{2}$'), ('(foo|bar)baz'), ('h.llo'),
               ('\yhello\y'), ('wor+ld'), ('ab[cd]')) v(p)
  ORDER BY p COLLATE "C";

-- ---- THE NARROWING IS APPLIED where the pattern has a literal run outside its
-- approximate atoms, and refused where it would be unsound.  regex_trgm counts
-- calls in which the weft narrowed the candidates.
SELECT weave_channel_stats_reset();
SELECT weave_count('ra_on', '/^fla(vou){~1}r$/'::wquery) AS flavour;
SELECT regex_trgm > 0 AS narrowed_on_fla FROM weave_channel_stats();
SELECT weave_channel_stats_reset();
SELECT weave_count('ra_on', '/(z?){~1}hello/'::wquery) AS nullable_atom;
SELECT regex_trgm = 0 AS not_narrowed_nullable_atom_leaks_its_budget
  FROM weave_channel_stats();
SELECT weave_channel_stats_reset();
SELECT weave_count('ra_on', '/\.?hello{~1}/'::wquery)
       = coalesce(cardinality(ra_heap('\.?hello{~1}')), 0) AS escape_agrees;
SELECT regex_trgm = 0 AS not_narrowed_on_an_escape FROM weave_channel_stats();

-- ---- TRE'S INSERTION RULE, recorded because it surprises: an approximate atom
-- may not gain a character AFTER its last one when the pattern ends there.
-- (abc){~1} anchored rejects "abcd" but accepts "xabc".  Index and heap agree,
-- because both are TRE; doc/specs/FUZZY_CHANNEL.md states it.
SELECT ra_heap('^(abc){~1}$') @> '{21}' AS abcd_matches,
       ra_heap('^(abc){~1}$') @> '{22}' AS xabc_matches;

-- ---- errors -----------------------------------------------------------------
-- more than three approximate atoms: refused (G92: TRE would assert)
SELECT count(*) FROM ra WHERE d @@@ '/a{~1}b{~1}c{~1}d{~1}/'::wquery;
-- three is fine
SELECT count(*) FROM ra WHERE d @@@ '/a{~1}b{~1}c{~1}/'::wquery;
-- a malformed approximate bound is a TRE syntax error, as is TRE's own {~n,m}
-- form, which pg_weave does not accept
SELECT count(*) FROM ra WHERE d @@@ '/ab{~x}/'::wquery;
SELECT count(*) FROM ra WHERE d @@@ '/ab{~1,2}/'::wquery;

DROP FUNCTION ra_ids(regclass, text);
DROP FUNCTION ra_heap(text);
DROP TABLE ra;
DROP TABLE rb;
