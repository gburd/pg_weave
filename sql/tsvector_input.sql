-- doc/PHASES.md M7 step 2: a tsvector column as a weave index input
-- (tsvector_lex_ops), and the one doclen convention on every input path.
--
-- Maintainer decisions 2026-10-07: a stripped or mixed tsvector is ACCEPTED,
-- with one WARNING at CREATE INDEX / REINDEX carrying the count; a mixed
-- tsvector keeps the positions it has; doclen = the number of tokens that
-- produced at least one lexeme (stopwords do not count, a token yielding two
-- lexemes counts once) on every input path.
CREATE EXTENSION IF NOT EXISTS pg_weave;

-- ---- doclen: one convention on every input path ---------------------------
-- 'the cat sat on the mat': six tokens, two of them stopwords under english.
-- Every english path must say 4; simple has no stopwords and says 6.
SELECT wdoc_length(to_wdoc('english', 'the cat sat on the mat')) AS regconfig,
	   wdoc_length(to_wdoc(to_tsvector('english', 'the cat sat on the mat'))) AS tsvector,
	   wdoc_length(to_wdoc('english', 'the cat sat on the mat', 'B')) AS weighted,
	   wdoc_length(to_wdoc('english', 'the cat sat on the mat')::text::wdoc) AS text_io,
	   wdoc_length(to_wdoc('english', 'the cat') || to_wdoc('english', 'sat on the mat')) AS concat,
	   wdoc_length(to_wdoc('simple', 'the cat sat on the mat')) AS simple_regconfig,
	   wdoc_length(to_wdoc(to_tsvector('simple', 'the cat sat on the mat'))) AS simple_tsvector,
	   wdoc_length(to_wdoc('the cat sat on the mat')) AS builtin;
-- the text form of a convention-length document carries no |len suffix, and a
-- canonical literal without one gets the convention (distinct positions)
SELECT to_wdoc('english', 'the cat sat on the mat')::text AS txt;
SELECT wdoc_length($$'a':2@1,3 'b':1@3$$::wdoc) AS shared_position,
	   wdoc_length($$'a':2 'b':1$$::wdoc) AS no_positions;
-- an explicit length that differs from the convention still round-trips (G90)
SELECT $$'a':1@1 'b':1@5 |9$$::wdoc::text AS explicit_len;

-- ispell: 'booking' yields {booking, book} for one token, so it counts once,
-- and 'footballklubber' yields six lexemes (two equal) for one token
CREATE TEXT SEARCH DICTIONARY tsi_ispell (
	Template = ispell, DictFile = ispell_sample, AffFile = ispell_sample);
CREATE TEXT SEARCH CONFIGURATION tsi_cfg (COPY = english);
ALTER TEXT SEARCH CONFIGURATION tsi_cfg
	ALTER MAPPING FOR asciiword WITH tsi_ispell, english_stem;
SELECT txt,
	   wdoc_length(to_wdoc('tsi_cfg', txt)) AS regconfig,
	   wdoc_length(to_wdoc(to_tsvector('tsi_cfg', txt))) AS tsvector,
	   to_tsvector('tsi_cfg', txt)::text AS tsv
  FROM (VALUES ('booking'), ('the booking of footballklubber'),
			   ('booking booking the end')) v(txt);

-- ---- a mixed tsvector keeps its positions ---------------------------------
-- tsv || 'tag'::tsvector: 'tag' has no position, the rest do.  Phrase answers
-- must be core's @@ answers on the same tsvector and tsquery: a phrase over
-- the positioned lexemes matches, a phrase through the positionless one does
-- not (core: TS_MAYBE at the phrase, false at the top), plain AND does.
CREATE TEMP TABLE mix AS
SELECT to_tsvector('simple', 'quick brown fox') || 'tag'::tsvector AS tsv;
SELECT tsv::text, to_wdoc(tsv)::text AS wdoc, wdoc_length(to_wdoc(tsv)) AS len FROM mix;
SELECT q,
	   tsv @@ q::tsquery AS core,
	   to_wdoc(tsv) @@@ q::tsquery::wquery AS wdoc_expr,
	   tsv @@@ q::tsquery::wquery AS tsv_op
  FROM mix, (VALUES ('quick <-> brown'), ('brown <-> fox'), ('quick <-> fox'),
					('quick <2> fox'), ('tag <-> quick'), ('fox <-> tag'),
					('tag & fox'), ('tag'), ('!tag'), ('tag <-> tag')) v(q);
-- the positionless entry survives || on either side without being rebased,
-- and is still adjacent to nothing
SELECT (to_wdoc(tsv) || to_wdoc(tsv))::text AS twice FROM mix;
SELECT to_wdoc(tsv) || to_wdoc(tsv) @@@ 'tag <-> quick'::tsquery::wquery AS tag_quick,
	   to_wdoc(tsv) || to_wdoc(tsv) @@@ 'fox <-> quick'::tsquery::wquery AS fox_quick
  FROM mix;
-- a weight zone never contains an unknown position (a positionless document
-- matches no zone either; core matches any weight there -- a recorded divergence)
SELECT to_wdoc(setweight(tsv, 'A')) @@@ 'tag:A'::tsquery::wquery AS tag_a,
	   to_wdoc(setweight(tsv, 'A')) @@@ 'fox:A'::tsquery::wquery AS fox_a
  FROM mix;
-- a wholly stripped tsvector has no positions at all, as before
SELECT to_wdoc(strip(tsv))::text AS stripped FROM mix;

-- ---- the operator class ---------------------------------------------------
CREATE TABLE tsi (id int PRIMARY KEY, body text, tsv tsvector);
INSERT INTO tsi
SELECT g, b, to_tsvector('english', b)
  FROM (SELECT g, 'doc ' || g || ' the quick brown fox ' ||
				  (ARRAY['jumps', 'sleeps', 'runs', 'hides'])[1 + g % 4] ||
				  ' over the lazy dog number ' || (g % 7) AS b
		  FROM generate_series(1, 200) g) s;
-- one stripped, one mixed, one capped (a lexeme repeated 300 times: 255 kept)
UPDATE tsi SET tsv = strip(tsv) WHERE id = 3;
UPDATE tsi SET tsv = tsv || 'tag'::tsvector WHERE id = 4;
UPDATE tsi SET tsv = to_tsvector('english', repeat('spam ', 300) || 'quick fox') WHERE id = 5;
SELECT id, length(tsv) AS nlex, (SELECT max(array_length(positions, 1))
								   FROM unnest(tsv)) AS maxpos
  FROM tsi WHERE id IN (3, 4, 5) ORDER BY id;

-- the WARNING names the count of documents with a positionless lexeme: 2
CREATE INDEX tsi_ix ON tsi USING weave (tsv tsvector_lex_ops);
-- the reference: the same documents through an expression index on to_wdoc()
CREATE INDEX tsi_ex ON tsi USING weave (to_wdoc(tsv));
SELECT * FROM weave_index_tsvector_stats('tsi_ix');
-- not a tsvector index
SELECT * FROM weave_index_tsvector_stats('tsi_ex');
-- REINDEX warns again, with the same count
REINDEX INDEX tsi_ix;

-- same rows, same order as to_wdoc(tsv) through the expression index.  Each
-- arm is forced onto its index and the plan is shown, so the comparison is
-- index against index, not two sequential scans.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE arms (q text, arm text, ids int[]);
CREATE FUNCTION pg_temp.arm_ids(qq text, expr bool) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	IF expr THEN
		SELECT array_agg(id ORDER BY d, id) INTO r FROM (
			SELECT id, to_wdoc(tsv) <=> qq::wquery AS d FROM tsi
			 WHERE to_wdoc(tsv) @@@ qq::wquery
			 ORDER BY to_wdoc(tsv) <=> qq::wquery LIMIT 1000) s;
	ELSE
		SELECT array_agg(id ORDER BY d, id) INTO r FROM (
			SELECT id, tsv <=> qq::wquery AS d FROM tsi
			 WHERE tsv @@@ qq::wquery
			 ORDER BY tsv <=> qq::wquery LIMIT 1000) s;
	END IF;
	RETURN r;
END $$;
EXPLAIN (COSTS OFF) SELECT id FROM tsi WHERE tsv @@@ 'jump'::wquery
	ORDER BY tsv <=> 'jump'::wquery LIMIT 10;
EXPLAIN (COSTS OFF) SELECT id FROM tsi WHERE to_wdoc(tsv) @@@ 'jump'::wquery
	ORDER BY to_wdoc(tsv) <=> 'jump'::wquery LIMIT 10;
SELECT q, cardinality(pg_temp.arm_ids(q, false)) AS n,
	   pg_temp.arm_ids(q, false) = pg_temp.arm_ids(q, true) AS same
  FROM (VALUES ('jump'), ('quick'), ('fox | sleep'), ('quick & !hide'),
			   ('"quick brown"'), ('"brown quick"'), ('spam'), ('tag'),
			   ('numb & doc')) v(q);
-- the plain restriction and the phrase on the mixed row, through the index:
-- id 4 ('tag' positionless) keeps its phrase answers
SELECT id FROM tsi WHERE tsv @@@ '"quick brown"'::wquery AND id < 6 ORDER BY id;
SELECT id FROM tsi WHERE tsv @@@ 'tag'::wquery ORDER BY id;
-- count path and the bitmap path (heap recheck on a tsvector heap value)
SELECT weave_count('tsi_ix', '"quick brown" & fox'::wquery) AS cnt_ix,
	   weave_count('tsi_ex', '"quick brown" & fox'::wquery) AS cnt_ex;
RESET enable_bitmapscan;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM tsi WHERE tsv @@@ '"quick brown"'::wquery;
SELECT count(*) FROM tsi WHERE tsv @@@ '"quick brown"'::wquery;
RESET enable_indexscan;
RESET enable_seqscan;

-- inserts go through the pending list; still the same rows as the expression
-- index, and the statistic sees the new stripped row at once
INSERT INTO tsi VALUES (201, 'late', strip(to_tsvector('english', 'quick late fox'))),
					   (202, 'late', to_tsvector('english', 'quick brown late fox'));
SELECT * FROM weave_index_tsvector_stats('tsi_ix');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT q, pg_temp.arm_ids(q, false) = pg_temp.arm_ids(q, true) AS same
  FROM (VALUES ('quick'), ('late'), ('"quick brown"')) v(q);
RESET enable_bitmapscan;
RESET enable_seqscan;

-- the statistic before and after DELETE + VACUUM: the stripped row 3, the
-- capped row 5 and the late stripped row 201 leave it
DELETE FROM tsi WHERE id IN (3, 5, 201);
VACUUM tsi;
SELECT * FROM weave_index_tsvector_stats('tsi_ix');
SELECT ndocs FROM weave_index_stats('tsi_ix');
SELECT weave_merge('tsi_ix') IS NOT NULL AS merged;
SELECT * FROM weave_index_tsvector_stats('tsi_ix');

-- a partial index counts only the rows its predicate admits
CREATE INDEX tsi_part ON tsi USING weave (tsv tsvector_lex_ops) WHERE id > 100;
SELECT * FROM weave_index_tsvector_stats('tsi_part');

-- a tsvector column with no positionless document builds with no WARNING
CREATE TABLE tsi_clean AS SELECT id, tsv FROM tsi WHERE id NOT IN (4);
CREATE INDEX tsi_clean_ix ON tsi_clean USING weave (tsv tsvector_lex_ops);
SELECT * FROM weave_index_tsvector_stats('tsi_clean_ix');

-- an untyped literal is ambiguous against core's deprecated @@@(tsvector, tsquery);
-- a typed one is not
SELECT count(*) FROM tsi WHERE tsv @@@ 'quick';
SELECT count(*) FROM tsi WHERE tsv @@@ 'quick'::wquery;

DROP TABLE tsi_clean;
DROP TABLE tsi;
DROP TEXT SEARCH CONFIGURATION tsi_cfg;
DROP TEXT SEARCH DICTIONARY tsi_ispell;
