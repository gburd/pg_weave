-- doc/GAPS.md G89 and G90: every wdoc that any SQL function returns must come
-- back unchanged from its own text form (pg_dump, text COPY, text-format logical
-- replication) and its own binary form (binary COPY, pg_dump -Fc of the data).
--
-- G89: to_wdoc(regconfig, text) used to store parsetext()'s positions, which
-- core clamps at 16,383, so a term recurring past that point had duplicate
-- positions and wdoc_in/wdoc_recv refused the value the server had built.
-- G90: the text form did not carry doclen and wdoc_recv discarded it, so a
-- stopword config's length (stopwords counted) came back as the sum of tf.
--
-- "Unchanged" is byte identity of wdoc_send(), which encodes version, nterms,
-- doclen, has_pos, every term, tf and position (wdoc has no '=' operator).
CREATE EXTENSION IF NOT EXISTS pg_weave;

-- text round trip of one value: 'same', 'DIFFERENT', or the error it raised
CREATE FUNCTION pg_temp.text_rt(d wdoc) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	RETURN CASE WHEN wdoc_send(d::text::wdoc) = wdoc_send(d)
				THEN 'same' ELSE 'DIFFERENT' END;
EXCEPTION WHEN others THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

-- binary round trip of one row, through COPY ... (FORMAT binary) and back;
-- per-backend file name: parallel suites on one host share /tmp (G60)
CREATE TEMP TABLE rtd (id int, what text, d wdoc);
CREATE TEMP TABLE rtb (d wdoc);
CREATE FUNCTION pg_temp.bin_rt(i int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	f	text := '/tmp/pg_weave_wdoc_rt_' || pg_backend_pid() || '.bin';
	ok	bool;
BEGIN
	EXECUTE format('COPY (SELECT d FROM rtd WHERE id = %s) TO %L WITH (FORMAT binary)', i, f);
	TRUNCATE rtb;
	EXECUTE format('COPY rtb FROM %L WITH (FORMAT binary)', f);
	SELECT wdoc_send(b.d) = wdoc_send(a.d) INTO ok FROM rtd a, rtb b WHERE a.id = i;
	RETURN CASE WHEN ok THEN 'same' ELSE 'DIFFERENT' END;
EXCEPTION WHEN others THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

-- An ispell dictionary emits several lexemes for one token, two of them equal
-- for 'footballklubber' ({footballklubber,foot,ball,klubber,football,klubber})
-- and more lexemes than tokens for 'booking' ({booking,book}).
CREATE TEXT SEARCH DICTIONARY wrt_ispell (
	Template = ispell, DictFile = ispell_sample, AffFile = ispell_sample);
CREATE TEXT SEARCH CONFIGURATION wrt_cfg (COPY = simple);
ALTER TEXT SEARCH CONFIGURATION wrt_cfg
	ALTER MAPPING FOR asciiword WITH wrt_ispell, simple;

INSERT INTO rtd VALUES
	(1,  'text short',            to_wdoc('The quick brown fox, the QUICK fox!')),
	(2,  'text empty',            to_wdoc('')),
	(3,  'text 20000',            to_wdoc(repeat('a b ', 10000))),
	(4,  'simple short',          to_wdoc('simple', 'the quick brown fox the fox')),
	(5,  'simple empty',          to_wdoc('simple', '')),
	(6,  'simple 16383',          to_wdoc('simple', repeat('a b ', 8191) || 'a')),
	(7,  'simple 16384',          to_wdoc('simple', repeat('a b ', 8192))),
	(8,  'simple 16385',          to_wdoc('simple', repeat('a b ', 8192) || 'a')),
	(9,  'simple 20000',          to_wdoc('simple', repeat('a b ', 10000))),
	(10, 'english stopwords',     to_wdoc('english', 'the cat sat on the mat')),
	(11, 'english all stopwords', to_wdoc('english', 'the of and the')),
	(12, 'english 20000',         to_wdoc('english', repeat('the cats sat on mats ', 4000))),
	(13, 'english weighted A',    to_wdoc('english', 'the cat sat on the mat', 'A')),
	(14, 'english 20000 C',       to_wdoc('english', repeat('the cats sat on mats ', 4000), 'C')),
	(15, 'concat english',        to_wdoc('english', 'vacuum lock', 'A')
	                              || to_wdoc('english', 'the vacuum runs', 'C')),
	(16, 'concat 20000+20000',    to_wdoc('simple', repeat('a b ', 10000))
	                              || to_wdoc('english', repeat('the cats sat on mats ', 4000), 'B')),
	(17, 'setwdocweight 20000',   setwdocweight(to_wdoc('simple', repeat('a b ', 10000)), 'B')),
	(18, 'tsvector',              to_wdoc(to_tsvector('english', 'the cat sat on the mat'))),
	(19, 'tsvector stripped',     to_wdoc(strip(to_tsvector('english', 'the cat sat on the mat')))),
	(20, 'tsvector weighted',     to_wdoc(setweight(to_tsvector('english', 'a fat cat'), 'B'))),
	(21, 'ispell klubber',        to_wdoc('wrt_cfg', 'footballklubber')),
	(22, 'ispell booking',        to_wdoc('wrt_cfg', 'booking booking'));

-- length and the round trips, per producer.  wdoc_length is the BM25 length:
-- every token position for a regconfig analyzer, stopwords included.
SELECT id, what, wdoc_length(d) AS len,
	   pg_temp.text_rt(d) AS text_rt, pg_temp.bin_rt(id) AS bin_rt
FROM rtd ORDER BY id;

-- a text-format COPY of the whole table, out and back in (what pg_dump does)
SELECT '/tmp/pg_weave_wdoc_rt_' || pg_backend_pid() || '.txt' AS rtfile \gset
COPY rtd TO :'rtfile';
CREATE TEMP TABLE rtd2 (LIKE rtd);
COPY rtd2 FROM :'rtfile';
SELECT count(*) AS rows_back,
	   count(*) FILTER (WHERE wdoc_send(a.d) = wdoc_send(b.d)) AS identical
FROM rtd a JOIN rtd2 b USING (id);

-- Positions past 16,383 are the true token ordinals: the last 'b' of a
-- 20,000-token document is at 20000, and 'a' has 10000 distinct positions.
SELECT substring(d::text from '([0-9]+)$') AS last_b_pos FROM rtd WHERE id = 9;
SELECT substring(d::text from '^''a'':([0-9]+)@') AS a_tf FROM rtd WHERE id = 9;

-- The canonical text form shows the length only when it differs from the sum
-- of tf, so documents whose two definitions agree render as before.
SELECT id, d::text FROM rtd WHERE id IN (1, 4, 10, 11, 13, 15, 18, 21, 22) ORDER BY id;

-- Phrase and NEAR on a document past 16,383 tokens.  Before G89 both 'alpha'
-- and 'beta' were stored at 16383, so the adjacent phrase did not match and a
-- NEAR across 101 tokens did.
CREATE TEMP TABLE longdoc (id int, d wdoc);
INSERT INTO longdoc VALUES
	(1, to_wdoc('simple', repeat('x ', 17000) || 'alpha beta ' || repeat('y ', 100) || 'gamma'));
SELECT d @@@ to_wquery('simple', '"alpha beta"')      AS adjacent_phrase,     -- t
	   d @@@ to_wquery('simple', '"beta alpha"')      AS reversed_phrase,     -- f
	   d @@@ to_wquery('simple', 'NEAR(beta gamma, 5)')   AS near_5,          -- f
	   d @@@ to_wquery('simple', 'NEAR(beta gamma, 101)') AS near_101         -- t
FROM longdoc;
-- the same answers from the index's positional postings
CREATE INDEX longdoc_w ON longdoc USING weave (d) WITH (positions = on);
SELECT weave_count('longdoc_w', to_wquery('simple', '"alpha beta"'))    AS adjacent_phrase,  -- 1
	   weave_count('longdoc_w', to_wquery('simple', 'NEAR(beta gamma, 5)')) AS near_5;       -- 0

-- parsetext() is called with its counter started far below zero so it never
-- clamps; the builder recovers each position from its low 16 bits.  That is
-- exact whenever the final unclamped count proves no run of 65,536 positions
-- without a lexeme was crossed.  65,534 stopwords between two words: exact.
SELECT wdoc_out(to_wdoc('english', 'cat ' || repeat('the ', 65534) || 'dog')) AS gap_65535;
-- 65,535 trailing stopwords: still provably exact
SELECT wdoc_length(to_wdoc('english', 'cat ' || repeat('the ', 65535))) AS tail_65535;
-- 65,535 stopwords between two words (a gap of 65,536) cannot be told from a
-- gap of 0, and is refused rather than stored at the wrong position.
SELECT wdoc_out(to_wdoc('english', 'cat ' || repeat('the ', 65535) || 'dog')) AS gap_65536;

-- || used to shift the right side by the left side's length, which is wrong
-- for a tsvector-derived left side whose positions exceed its length.
SELECT (to_wdoc(to_tsvector('english', 'the cat sat on the mat'))
		|| to_wdoc('english', 'mat'))::text AS concat_tsvector_left;
-- and it sized its position array from the two lengths, which an ispell
-- document ({booking,book} for one token) exceeds
SELECT (to_wdoc('wrt_cfg', 'booking') || to_wdoc('wrt_cfg', 'booking'))::text AS concat_ispell;

-- the length is validated at the trust boundary: it is at least every tf
SELECT $$'a':2@1,2 |1$$::wdoc;                      -- ERROR: length below a tf
SELECT ($$'a':2@1,5 |7$$::wdoc)::text AS explicit_len;
SELECT wdoc_length('|4'::wdoc) AS empty_with_len;

DROP TEXT SEARCH CONFIGURATION wrt_cfg;
DROP TEXT SEARCH DICTIONARY wrt_ispell;
