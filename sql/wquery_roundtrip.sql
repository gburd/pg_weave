--
-- G96: wquery_out's text parses back.  For every wquery, q::text::wquery has
-- the same bytes as q (compared through wquery_send) and its text is q's text.
-- Every item kind and flag: AND / OR / NOT, the at-most phrase ("..." and
-- NEAR), the exact gap (WEAVE_QF_PHRASE_EXACT), prefix, fuzzy, regex, weights,
-- nested groups, the empty query.  Then two generators -- random TEXT queries
-- through wquery_in, and random postfix item lists through wquery_recv (the
-- binary path, which reaches shapes no text spelling produces) -- and a text
-- COPY of a wquery column out and back.
--
CREATE EXTENSION IF NOT EXISTS pg_weave;
SELECT setseed(0.96);

-- q's verdict: the reparsed bytes, the reparsed text, the printed text
CREATE FUNCTION pg_temp.rt(q wquery, OUT txt text, OUT same_bytes bool,
						   OUT same_text bool)
LANGUAGE plpgsql AS $$
BEGIN
	txt := q::text;
	same_bytes := wquery_send(txt::wquery) = wquery_send(q);
	same_text := txt::wquery::text = txt;
END $$;

-- ---- hand-written, through wquery_in --------------------------------------
SELECT i, input, r.txt, r.same_bytes, r.same_text
  FROM unnest(ARRAY[
		-- boolean
		'quick', 'quick & brown', 'quick | brown', '!quick', 'quick brown',
		'quick AND brown OR NOT fox', 'a & (b | !c)', '!(a | b) & !!c',
		'((a | b) & (c | (d & !e)))', 'a - b', '-a',
		-- native at-most phrases
		'"quick brown"', '"quick brown fox"', '"quick bro*"', '"and or not near"',
		'NEAR(quick brown, 3)', 'NEAR(a b c, 2)', 'NEAR(a b)', 'NEAR(a b, 1)',
		'"a b" & NEAR(c d, 4)', '!"a b"', '"a b" | !NEAR(c d e, 7)',
		-- the operator spellings
		'a <-> b', 'a <2> b', 'a <0> b', 'a <=3> b', 'a <-> b <-> c',
		'(a <-> b) <2> c', 'a <-> (b <2> c)', '(a <0> b) <3> (c <-> d)',
		'a <-> b & c', 'a | b <-> c', '!a <-> b', '"a b" <-> c', 'a <-> "b c"',
		'NEAR(a b, 3) <2> c', 'a <=1> b', 'a <=2> (b <=2> c)', 'a <=1> b*',
		'a~1 <-> b', 'a:A <2> b:BC', '/x.y/ <-> b',
		-- suffixes, bare and quoted
		'fo*', 'fo~1', 'fo~', 'fo~0', 'fo~12', 'fox:A', 'fox:BD', 'fox:abcd',
		$$'fo'*$$, $$'fo'~1$$, $$'fox':A$$, $$'fox':DCBA$$,
		-- quoted literals: verbatim bytes, escapes, keywords and operators
		$$'Fox'$$, $$'it\'s'$$, $$'a\\b'$$, $$'and' & 'or' & 'not'$$, $$'near'$$,
		$$'a <-> b'$$, $$'pkg-config'$$, $$'-x'$$, $$'a b'$$, $$'"x"'$$,
		$$"'Big' city"$$, $$NEAR('a b' c, 2)$$,
		-- regex: the lexer does not look inside
		'/a<->b/', '/a<2>b/', $$/it's/$$, '/^qu/', '/a\.b/', '/a|b/ & c',
		-- the empty query
		'', '   '
	]) WITH ORDINALITY u(input, i),
	   LATERAL pg_temp.rt(input::wquery) r
 ORDER BY i;

-- every exact-gap shape the tsquery cast produces (G93's list) round-trips too
SELECT q, r.txt, r.same_bytes, r.same_text
  FROM unnest(ARRAY['fox:A', 'fox:ABCD', 'fo:*', 'quick <-> brown', 'quick <2> brown',
					'quick <0> brown', 'quick <-> (brown <-> fox)',
					'quick <2> (brown <3> fox)', '(quick <2> brown) <-> (fox <0> fox)',
					'(quick <0> quick) <2> fox', 'quick <2> brown | dog <0> dog',
					'!(quick <2> brown)', 'quick:A <2> brown:C & !fox:D']) q,
	   LATERAL pg_temp.rt(q::tsquery::wquery) r;

-- an exact gap means what the operator says: <N> of the cast and of the text
-- are the same value, and N is not "at most N"
SELECT q, wquery_send(q::wquery) = wquery_send(q::tsquery::wquery) AS same_as_cast,
	   to_wdoc('a x b') @@@ q::wquery AS on_gap_2
  FROM unnest(ARRAY['a <2> b', 'a <-> b', 'a <3> b', '(a <-> x) <-> b',
					'a <-> (x <-> b)']) q;
SELECT to_wdoc('a x b') @@@ 'a <=2> b'::wquery AS at_most_2,
	   to_wdoc('a x b') @@@ 'a <=1> b'::wquery AS at_most_1,
	   to_wdoc('a b') @@@ 'a <2> b'::wquery AS exact_2_on_gap_1,
	   to_wdoc('a b') @@@ 'a <0> b'::wquery AS exact_0;

-- the suffixes keep their value: a weight restricts, a prefix expands, ~k is k
SELECT $$'fox':A$$::wquery::text AS w, to_wdoc('simple', 'fox', 'B') @@@ $$'fox':A$$::wquery AS w_wrong_zone,
	   to_wdoc('simple', 'fox', 'A') @@@ $$'fox':A$$::wquery AS w_right_zone,
	   to_wdoc('foxes') @@@ $$'fox'*$$::wquery AS prefix_hit,
	   to_wdoc('fax') @@@ $$'fox'~1$$::wquery AS fuzzy_1,
	   to_wdoc('fax') @@@ $$'fxx'~1$$::wquery AS fuzzy_1_miss,
	   to_wdoc('fax') @@@ $$'fxx'~2$$::wquery AS fuzzy_2;

-- spellings that are NOT an operator or a literal keep their old meaning
SELECT input, input::wquery::text
  FROM unnest(ARRAY['a < b', 'a <b', 'a <x> b', 'a <=> b', 'a <- b', 'a > b',
					$$don't$$, $$'tis$$, $$x ' y$$, $$'tis and 'twas$$, $$''$$,
					'c++', 'foo/', 'pkg-config', 'a-(b)']) input;

-- errors, each where it used to be a different query silently
DO $$
DECLARE
	q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['a /foo', 'a / b', 'fo~99999999999', 'a <99999999999> b',
							 'NEAR(a b, 99999999999)', 'NEAR(a b, 0)', 'NEAR(a b, x)',
							 '"a /re/"', 'NEAR(a /re/, 2)', 'a <->', '<-> a', 'a <-> <-> b',
							 '''a''*:A', 'fox:A*', 'fo*:A'] LOOP
		BEGIN
			PERFORM q::wquery;
			RAISE NOTICE '%: accepted as %', q, q::wquery::text;
		EXCEPTION WHEN syntax_error THEN
			RAISE NOTICE '%: syntax error', q;
		END;
	END LOOP;
END $$;

-- stopword elision keeps an exact gap's flag (qnode_flatten dropped it, so an
-- exact gap became "at most" whenever a stopword was elided anywhere)
SELECT to_wquery('english', 'the & (quick <2> brown)')::text AS elided,
	   wquery_send(to_wquery('english', 'the & (quick <2> brown)'))
	   = wquery_send('quick <2> brown'::wquery) AS flag_kept;

-- ---- randomized, text in --------------------------------------------------
-- random query trees over every operand and operator kind, printed by the
-- generator in any accepted spelling, then parsed, printed and parsed again
CREATE FUNCTION pg_temp.tq(depth int) RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	r float8 := random();
	w text := (ARRAY['quick','brown','fox','Dog','and','a-b','it''s','x\y','near','f'])[1 + floor(random() * 10)::int];
	q text := '''' || replace(replace(w, '\', '\\'), '''', '\''') || '''';
	op text := (ARRAY['&','|','<->','<2>','<0>','<=1>','<=3>','AND','OR',''])[1 + floor(random() * 10)::int];
	d float8 := random();
BEGIN
	IF depth <= 0 OR r < 0.3 THEN
		IF d < 0.12 THEN
			RETURN q || '*';
		ELSIF d < 0.24 THEN
			RETURN q || '~' || floor(random() * 4)::int;
		ELSIF d < 0.36 THEN
			RETURN q || ':' || (ARRAY['A','B','CD','ABCD','db'])[1 + floor(random() * 5)::int];
		ELSIF d < 0.42 THEN
			RETURN '/' || (ARRAY['^qu','a<->b','fo.*','x|y'])[1 + floor(random() * 4)::int] || '/';
		ELSIF d < 0.52 THEN
			RETURN '"' || (ARRAY['quick brown','a b c','fo* x','and or'])[1 + floor(random() * 4)::int] || '"';
		ELSIF d < 0.6 THEN
			RETURN 'NEAR(' || (ARRAY['quick brown','a b c','x y* z'])[1 + floor(random() * 3)::int]
				|| ', ' || 1 + floor(random() * 4)::int || ')';
		ELSIF d < 0.8 THEN
			RETURN q;
		END IF;
		RETURN lower(replace(w, '''', ''));
	ELSIF r < 0.42 THEN
		RETURN '!(' || pg_temp.tq(depth - 1) || ')';
	END IF;
	RETURN '(' || pg_temp.tq(depth - 1) || ' ' || op || ' ' || pg_temp.tq(depth - 1) || ')';
END $$;
CREATE TEMP TABLE tqs AS
SELECT i, pg_temp.tq(4) AS input FROM generate_series(1, 1000) i;
CREATE TEMP TABLE tres AS
SELECT i, input, r.* FROM tqs, LATERAL pg_temp.rt(input::wquery) r;
SELECT count(*) AS queries, count(*) FILTER (WHERE same_bytes AND same_text) AS round_trip,
	   count(*) FILTER (WHERE txt LIKE '%"%') AS quoted_phrase,
	   count(*) FILTER (WHERE txt LIKE '%NEAR(%') AS near,
	   count(*) FILTER (WHERE txt ~ '<->|<[0-9]+>') AS exact_gap,
	   count(*) FILTER (WHERE txt LIKE '%<=%') AS at_most_op,
	   count(*) FILTER (WHERE txt ~ $$'\*$$) AS prefix,
	   count(*) FILTER (WHERE txt ~ $$'~[0-9]$$) AS fuzzy,
	   count(*) FILTER (WHERE txt ~ $$':[A-D]$$) AS weighted,
	   count(*) FILTER (WHERE txt LIKE '%/%') AS regex,
	   count(*) FILTER (WHERE txt LIKE '%!%') AS negated
  FROM tres;
SELECT i, input, txt FROM tres WHERE NOT (same_bytes AND same_text) ORDER BY i LIMIT 10;

-- ---- randomized, binary in ------------------------------------------------
-- random postfix item lists written as wquery_send's format, received through
-- COPY (FORMAT binary) into a wquery column: wquery_recv builds shapes no text
-- produces (an at-most phrase over a NOT, mixed NEAR gaps, a phrase of
-- phrases).  Each must print, parse back to the same bytes, and print the same.
CREATE FUNCTION pg_temp.bi(typ int, op int, flags int, dist bigint, term text)
RETURNS bytea LANGUAGE sql IMMUTABLE AS $$
	SELECT int2send(((typ << 8) | op)::int2) || int2send(flags::int2)
		|| int4send(dist::int4)
		|| CASE WHEN typ = 1 THEN int4send(octet_length(term)) || convert_to(term, 'UTF8')
				ELSE ''::bytea END
$$;
-- one random operand subtree as items, and its width (as wquery_out computes it)
CREATE FUNCTION pg_temp.bq(depth int, OUT b bytea, OUT n int, OUT w bigint)
LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	r float8 := random();
	t text := (ARRAY['quick','Brown','fox','it''s','a\b','and','x y','pkg-config','f','<->'])[1 + floor(random() * 10)::int];
	l record;
	rr record;
	k int;
	d bigint;
	fl int;
BEGIN
	IF depth <= 0 OR r < 0.3 THEN
		r := random();
		w := 0; n := 1;
		IF r < 0.15 THEN b := pg_temp.bi(1, 0, 1, 0, t);			-- prefix
		ELSIF r < 0.3 THEN b := pg_temp.bi(1, 0, 2, 1 + floor(random() * 3)::int, t);	-- fuzzy
		ELSIF r < 0.45 THEN b := pg_temp.bi(1, 0, 8, 1 + floor(random() * 15)::int, t);	-- weighted
		ELSIF r < 0.52 THEN b := pg_temp.bi(1, 0, 4, 0, (ARRAY['^qu','a<->b','x|y','it''s'])[1 + floor(random() * 4)::int]);
		ELSE b := pg_temp.bi(1, 0, 0, 0, t);
		END IF;
		RETURN;
	END IF;
	l := pg_temp.bq(depth - 1);
	IF r < 0.4 THEN
		b := l.b || pg_temp.bi(2, 1, 0, 0, NULL); n := l.n + 1; w := l.w;
		RETURN;
	END IF;
	rr := pg_temp.bq(depth - 1);
	k := 1 + floor(random() * 4)::int;		-- 1 AND, 2 OR, 3 at-most, 4 exact
	IF k <= 2 THEN
		b := l.b || rr.b || pg_temp.bi(2, k + 1, 0, 0, NULL); w := greatest(l.w, rr.w);
	ELSIF k = 3 THEN
		d := (ARRAY[1, 1, 1, 2, 3])[1 + floor(random() * 5)::int];
		b := l.b || rr.b || pg_temp.bi(2, 4, 0, d, NULL); w := d + l.w + rr.w;
	ELSE
		d := (ARRAY[0, 1, 1, 2, 5])[1 + floor(random() * 5)::int] + rr.w;
		b := l.b || rr.b || pg_temp.bi(2, 4, 16, d, NULL); w := d + l.w;
	END IF;
	n := l.n + rr.n + 1;
END $$;
-- wquery_recv, reached from SQL: the bytes as a one-column binary COPY file,
-- written by the server (lo_export), read into a wquery column.  Per-backend
-- file name (G60).
CREATE TEMP TABLE rcv (w wquery);
CREATE FUNCTION pg_temp.recv(b bytea) RETURNS wquery LANGUAGE plpgsql AS $$
DECLARE
	f text := '/tmp/pg_weave_g96_' || pg_backend_pid() || '.bin';
	o oid;
	r wquery;
BEGIN
	o := lo_from_bytea(0, '\x5047434f50590aff0d0a00'::bytea || int4send(0) || int4send(0)
					   || int2send(1::int2) || int4send(octet_length(b)) || b
					   || int2send((-1)::int2));
	PERFORM lo_export(o, f);
	PERFORM lo_unlink(o);
	DELETE FROM rcv;
	EXECUTE format('COPY rcv FROM %L WITH (FORMAT binary)', f);
	SELECT w INTO r FROM rcv;
	RETURN r;
END $$;
-- control: the path is wquery_recv, and it returns what wquery_send sent
SELECT wquery_send(pg_temp.recv(wquery_send('a & "b c"'::wquery)))
	   = wquery_send('a & "b c"'::wquery) AS recv_path_works;

CREATE TEMP TABLE braw AS
SELECT i, int2send(2::int2) || int4send(x.n) || x.b AS b
  FROM generate_series(1, 1000) i, LATERAL pg_temp.bq(4) x;
CREATE FUNCTION pg_temp.brt(b bytea, OUT q wquery, OUT err text) LANGUAGE plpgsql AS $$
BEGIN
	q := pg_temp.recv(b);
EXCEPTION WHEN invalid_binary_representation THEN
	err := SQLERRM;
END $$;
CREATE TEMP TABLE bres AS
SELECT i, b, x.q, x.err, r.* FROM braw, LATERAL pg_temp.brt(b) x,
	   LATERAL pg_temp.rt(x.q) r;
SELECT count(*) AS lists, count(*) FILTER (WHERE err IS NOT NULL) AS refused,
	   count(*) FILTER (WHERE wquery_send(q) = b) AS recv_exact,
	   count(*) FILTER (WHERE same_bytes AND same_text) AS round_trip,
	   count(*) FILTER (WHERE txt LIKE '%"%') AS quoted_phrase,
	   count(*) FILTER (WHERE txt LIKE '%NEAR(%') AS near,
	   count(*) FILTER (WHERE txt ~ '<->|<[0-9]+>') AS exact_gap,
	   count(*) FILTER (WHERE txt LIKE '%<=%') AS at_most_op,
	   count(*) FILTER (WHERE txt ~ $$'\*$$) AS prefix,
	   count(*) FILTER (WHERE txt ~ $$'~[0-9]$$) AS fuzzy,
	   count(*) FILTER (WHERE txt ~ $$':[A-D]$$) AS weighted,
	   count(*) FILTER (WHERE txt LIKE '%/%') AS regex
  FROM bres;
SELECT i, err, txt FROM bres
 WHERE err IS NOT NULL OR NOT (same_bytes AND same_text) ORDER BY i LIMIT 10;

-- what wquery_recv refuses: each of these used to be stored, and the first
-- two crashed wquery_out and the evaluators (stack[-1])
SELECT what, (pg_temp.brt(int2send(2::int2) || int4send(n) || b)).err
  FROM (VALUES
		('AND alone', 1, pg_temp.bi(2, 2, 0, 0, NULL)),
		('two terms, no operator', 2, pg_temp.bi(1, 0, 0, 0, 'a') || pg_temp.bi(1, 0, 0, 0, 'b')),
		('NOT alone', 1, pg_temp.bi(2, 1, 0, 0, NULL)),
		('unknown term flag', 1, pg_temp.bi(1, 0, 32, 0, 'a')),
		('prefix and weight', 1, pg_temp.bi(1, 0, 9, 1, 'a')),
		('fuzzy ~0', 1, pg_temp.bi(1, 0, 2, 0, 'a')),
		('weight mask 0', 1, pg_temp.bi(1, 0, 8, 0, 'a')),
		('weight mask 16', 1, pg_temp.bi(1, 0, 8, 16, 'a')),
		('gap on a plain term', 1, pg_temp.bi(1, 0, 0, 3, 'a')),
		('empty term', 1, pg_temp.bi(1, 0, 0, 0, '')),
		('slash in a regex', 1, pg_temp.bi(1, 0, 4, 0, 'a/b')),
		('gap on AND', 3, pg_temp.bi(1, 0, 0, 0, 'a') || pg_temp.bi(1, 0, 0, 0, 'b') || pg_temp.bi(2, 2, 0, 1, NULL)),
		('exact flag on OR', 3, pg_temp.bi(1, 0, 0, 0, 'a') || pg_temp.bi(1, 0, 0, 0, 'b') || pg_temp.bi(2, 3, 16, 0, NULL)),
		('exact gap under its right width', 5,
		 pg_temp.bi(1, 0, 0, 0, 'a') || pg_temp.bi(1, 0, 0, 0, 'b') || pg_temp.bi(1, 0, 0, 0, 'c')
		 || pg_temp.bi(2, 4, 16, 1, NULL) || pg_temp.bi(2, 4, 16, 0, NULL))
	   ) v(what, n, b);

-- ---- a text COPY of a wquery column, out and back -------------------------
CREATE TEMP TABLE wqa (id int, w wquery);
CREATE TEMP TABLE wqb (id int, w wquery);
INSERT INTO wqa SELECT i, input::wquery FROM tqs;
INSERT INTO wqa SELECT 10000 + i, q FROM bres WHERE q IS NOT NULL;
INSERT INTO wqa SELECT 20000 + i, q::tsquery::wquery
  FROM unnest(ARRAY['quick <2> brown', 'fox:A <0> dog', '(a <3> b) <-> (c <-> d)',
					'fo:* & !dog:C']) WITH ORDINALITY u(q, i);
SELECT '/tmp/pg_weave_g96_' || pg_backend_pid() || '.txt' AS tfile \gset
COPY wqa TO :'tfile';
COPY wqb FROM :'tfile';
SELECT count(*) AS copied, count(*) FILTER (WHERE wquery_send(a.w) = wquery_send(b.w)) AS identical
  FROM wqa a JOIN wqb b USING (id);
