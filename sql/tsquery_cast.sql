--
-- G93: the tsquery -> wquery cast must answer exactly what core's @@ answers,
-- or refuse.  For every query below, `tsv @@ q` (core) and
-- `to_wdoc(tsv) @@@ q::wquery` must return the SAME row set over one table; a
-- tsquery shape wquery cannot express exactly is refused with
-- feature_not_supported, never converted into something that answers
-- differently.
--
CREATE EXTENSION IF NOT EXISTS pg_weave;
SELECT setseed(0.93);

-- every document fully positioned; zones A/B/C/D from setweight() segments, so
-- a lexeme can occur in several zones and at several distances from another
CREATE FUNCTION pg_temp.words(n int) RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT coalesce(string_agg((ARRAY['quick','brown','fox','dog','lazy',
									  'jump','over','fog'])[1 + floor(random() * 8)::int],
							   ' '), '')
	  FROM generate_series(1, n)
$$;
CREATE TEMP TABLE tc (id int PRIMARY KEY, tsv tsvector);
DO $$
BEGIN
	FOR g IN 1..200 LOOP
		INSERT INTO tc VALUES (g,
			setweight(to_tsvector('simple', pg_temp.words(floor(random() * 4)::int)), 'A') ||
			setweight(to_tsvector('simple', pg_temp.words(floor(random() * 4)::int)), 'B') ||
			setweight(to_tsvector('simple', pg_temp.words(floor(random() * 4)::int)), 'C') ||
			to_tsvector('simple', pg_temp.words(floor(random() * 5)::int)));
	END LOOP;
END $$;
SELECT count(*) AS docs, count(*) FILTER (WHERE tsv = strip(tsv) AND length(tsv) > 0) AS stripped
  FROM tc;

-- the index arms: weave_count() enters the scan machinery directly, so it
-- reaches the scan-side recheck rather than an executor recheck (AGENTS.md)
CREATE INDEX tc_tsv ON tc USING weave (tsv tsvector_lex_ops);
CREATE INDEX tc_expr ON tc USING weave (to_wdoc(tsv));
-- positions = on: a pure phrase chain is answered from the postings' positions
-- (weave_phrase_eval_seg), the other evaluator of an exact gap
CREATE INDEX tc_pos ON tc USING weave (to_wdoc(tsv)) WITH (positions = on);

-- core's row set, the cast's row set (heap evaluation), the two index counts
-- (ix_ok), the ranked index scan's relation to core (ranked), or the refusal
CREATE FUNCTION pg_temp.cmp(q text, OUT core int[], OUT weave int[],
							OUT ix_ok bool, OUT ranked text, OUT refused text)
LANGUAGE plpgsql AS $$
DECLARE
	wq wquery;
	r int[];
	rr record;
	ids int[] := '{}';
	tids tid[] := '{}';
	curs float8[] := '{}';
	bad int;
BEGIN
	SELECT coalesce(array_agg(id ORDER BY id), '{}') INTO core
	  FROM tc WHERE tsv @@ q::tsquery;
	BEGIN
		wq := q::tsquery::wquery;
	EXCEPTION WHEN feature_not_supported THEN
		refused := SQLERRM;
		RETURN;
	END;
	SELECT coalesce(array_agg(id ORDER BY id), '{}') INTO weave
	  FROM tc WHERE to_wdoc(tsv) @@@ wq;
	ix_ok := weave_count('tc_tsv', wq) = cardinality(core)
		 AND weave_count('tc_expr', wq) = cardinality(core)
		 AND weave_count('tc_pos', wq) = cardinality(core);
	-- the ranked scan (WHERE @@@ q ORDER BY <=> q).  Its candidates come from
	-- q's literal terms, so a prefix expansion or a row matching only through
	-- NOT is reached by the padding phase (doc/GAPS.md G94; it was a SUBSET).
	-- 'same' needs the set AND the order: each row at the distance the scan
	-- published for it, that distance weave_search()'s (which returns every
	-- match too, a padded one at score 0, distance 1.0), and the stream
	-- non-decreasing.  A row weave_search() does not return is 'MISORDERED'.
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	FOR rr IN SELECT id, ctid AS t, weave_current_distance('tc_tsv', ctid, wq) AS cur
				FROM tc WHERE tsv @@@ wq ORDER BY tsv <=> wq LIMIT 1000 LOOP
		ids := ids || rr.id;
		tids := tids || rr.t;
		curs := curs || rr.cur;
	END LOOP;
	SET LOCAL enable_seqscan = on;
	SET LOCAL enable_bitmapscan = on;
	SELECT coalesce(array_agg(i ORDER BY i), '{}') INTO r FROM unnest(ids) i;
	SELECT count(*) INTO bad
	  FROM (SELECT u.cur, lag(u.cur) OVER (ORDER BY u.o) AS prev, s.score
			  FROM unnest(tids, curs) WITH ORDINALITY u(t, cur, o)
			  LEFT JOIN weave_search('tc_tsv', wq, 1000) s ON s.ctid = u.t) z
	 WHERE cur IS NULL OR cur < prev OR score IS NULL
		OR abs(cur - 1.0 / (1.0 + score)) > 1e-9;
	ranked := CASE WHEN r = core AND bad = 0 THEN 'same'
				   WHEN r = core THEN 'MISORDERED'
				   WHEN r <@ core THEN 'subset'
				   ELSE 'SUPERSET' END;
END $$;

-- ---- hand-written ---------------------------------------------------------
SELECT q, cardinality(c.core) AS ncore,
	   CASE WHEN c.refused IS NOT NULL THEN 'refused'
			WHEN c.core = c.weave AND c.ix_ok THEN 'same' ELSE 'DIFFERENT' END AS verdict,
	   c.ranked, c.refused
  FROM (VALUES
		-- weights, single and combined
		('fox:A'), ('fox:B'), ('fox:C'), ('fox:D'), ('fox:AB'), ('fox:CD'),
		('fox:AD'), ('fox:BCD'), ('fox:ABCD'), ('fox:A & dog:B'), ('fox:A | dog:D'),
		-- prefixes
		('fo:*'), ('qu:*'), ('f:*'), ('fo:* & !fox'), ('fo:* | la:*'),
		-- prefix with a weight: wquery's prefix match has no zones
		('fo:*A'), ('fo:*AB'),
		-- phrases
		('quick <-> brown'), ('quick:A <-> brown'), ('quick <-> brown:B'),
		('(quick <-> brown) <-> fox'), ('(quick <-> brown) & fox'),
		('!(quick <-> brown)'), ('fox <-> fox'),
		-- exact gaps (WEAVE_QF_PHRASE_EXACT): <0> is the same position, <N>
		-- exactly N, a right operand's width adds to the gap
		('quick <2> brown'), ('quick <0> brown'), ('quick <3> brown'),
		('fox <0> fox'), ('fox <2> fox'), ('quick <1> brown'), ('quick:A <2> brown:C'),
		('quick <-> (brown <-> fox)'), ('quick <2> (brown <3> fox)'),
		('(quick <2> brown) <-> (fox <0> fox)'), ('(quick <0> quick) <2> fox'),
		('quick <-> (brown <-> (fox <-> dog))'), ('!(quick <2> brown)'),
		('quick <2> brown | dog <0> dog'), ('quick <2> brown & !fox'),
		-- phrase shapes wquery cannot express exactly: still refused
		('quick <-> (brown | fox)'),
		('quick <-> (brown & fox)'), ('quick <-> !brown'), ('fo:* <-> dog'),
		('quick <-> fo:*'),
		-- negation
		('!fox'), ('!fox & !dog'), ('!(fox | dog)'), ('!!fox'), ('fox & !dog:A'),
		-- a term no document has; the empty query
		('zzz'), ('!zzz'), ('')) v(q),
	   LATERAL pg_temp.cmp(q) c;

-- ---- randomized -----------------------------------------------------------
CREATE FUNCTION pg_temp.rq(depth int) RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	r float8 := random();
	w text := (ARRAY['quick','brown','fox','dog','lazy','jump','over','fog'])[1 + floor(random() * 8)::int];
	lbl text := (ARRAY['A','B','C','D','AB','CD','AD','BC','BCD','ABCD'])[1 + floor(random() * 10)::int];
	op text := (ARRAY['&','|','<->','<->','<->','<2>','<0>'])[1 + floor(random() * 7)::int];
	d float8 := random();
	lhs text;
BEGIN
	IF depth <= 0 OR r < 0.3 THEN
		IF d < 0.15 THEN
			RETURN left(w, 2) || ':*';
		ELSIF d < 0.45 THEN
			RETURN w || ':' || lbl;
		ELSIF d < 0.5 THEN
			RETURN left(w, 2) || ':*' || lbl;
		END IF;
		RETURN w;
	ELSIF r < 0.42 THEN
		RETURN '!(' || pg_temp.rq(depth - 1) || ')';
	END IF;
	lhs := pg_temp.rq(depth - 1);
	RETURN '(' || lhs || ' ' || op || ' ' || pg_temp.rq(depth - 1) || ')';
END $$;
CREATE TEMP TABLE rqs AS
SELECT i, pg_temp.rq(3) AS q FROM generate_series(1, 400) i;
CREATE TEMP TABLE rres AS
SELECT i, q, c.* FROM rqs, LATERAL pg_temp.cmp(q) c;
-- every converted query agrees with core; the rest were refused, by reason
SELECT count(*) AS queries,
	   count(*) FILTER (WHERE refused IS NULL AND core = weave AND ix_ok) AS same,
	   count(*) FILTER (WHERE refused IS NULL AND (core IS DISTINCT FROM weave
												  OR ix_ok IS NOT TRUE)) AS different,
	   count(*) FILTER (WHERE refused IS NOT NULL) AS refused,
	   count(*) FILTER (WHERE refused IS NULL AND cardinality(core) > 0) AS same_nonempty
  FROM rres;
SELECT refused, count(*) FROM rres WHERE refused IS NOT NULL GROUP BY 1 ORDER BY 1;
-- the disagreements, if any (none)
SELECT i, q, core, weave, ix_ok FROM rres
 WHERE refused IS NULL AND (core IS DISTINCT FROM weave OR ix_ok IS NOT TRUE)
 ORDER BY i LIMIT 20;
-- the ranked scan: the same set as core, in order, for every converted query
-- (G94: it was a subset for 35, each with a prefix or a NOT)
SELECT ranked, count(*),
	   count(*) FILTER (WHERE q !~ ':\*' AND q !~ '!') AS without_prefix_or_not
  FROM rres WHERE refused IS NULL GROUP BY 1 ORDER BY 1;
-- coverage: the converted set exercises each feature at least once
SELECT count(*) FILTER (WHERE q ~ ':[A-D]+') AS weighted,
	   count(*) FILTER (WHERE q ~ ':\*') AS prefixed,
	   count(*) FILTER (WHERE q ~ '<->') AS phrased,
	   count(*) FILTER (WHERE q ~ '<[0-9]+>') AS exact_gap,
	   count(*) FILTER (WHERE q ~ '!') AS negated
  FROM rres WHERE refused IS NULL;

-- the text form of each mapped shape; an exact gap prints as core does
SELECT q, q::tsquery::wquery::text AS wquery
  FROM (VALUES ('fox:A'), ('fox:BD'), ('fox:ABCD'), ('fo:*'), ('!fox:C'),
			   ('quick <-> brown <-> fox'), ('fox:A & (dog | la:*)'),
			   ('quick <0> brown'), ('quick <3> brown'), ('quick <-> (brown <2> fox)'),
			   ('(quick <2> brown) <0> (fox <-> dog)')) v(q);

-- phraseto_tsquery: a stopword leaves a gap, which core spells <N>
SELECT p, doc, phraseto_tsquery('english', p)::text AS tsquery,
	   phraseto_tsquery('english', p)::wquery::text AS wquery,
	   to_tsvector('english', doc) @@ phraseto_tsquery('english', p) AS core,
	   to_wdoc(to_tsvector('english', doc)) @@@ phraseto_tsquery('english', p)::wquery AS weave
  FROM (VALUES ('cat in the hat'), ('the cat in the hat'), ('cat hat'), ('cat in hat')) v(p),
	   (VALUES ('the cat in the hat sat'), ('a cat hat'), ('cat in a hat'),
			   ('cat in the big hat')) t(doc)
 ORDER BY 1, 2;

-- <0> where it can match: two lexemes at one position (what an ispell or
-- thesaurus dictionary produces; a 'simple' tsvector never does).  Core, the
-- heap, and the three index arms (positions = on answers a pure chain from the
-- postings) must agree.
CREATE TEMP TABLE tz (id int, tsv tsvector);
INSERT INTO tz VALUES (1, 'quick:1 brown:1 fox:2'), (2, 'quick:1 brown:2 fox:3'),
	(3, 'quick:1,3 brown:3 fox:4 dog:4'), (4, 'brown:1 quick:2 fox:2'),
	(5, 'quick:5 brown:5 fox:7 dog:9'), (6, 'fox:1 dog:1 quick:3 brown:3');
CREATE INDEX tz_tsv ON tz USING weave (tsv tsvector_lex_ops);
CREATE INDEX tz_expr ON tz USING weave (to_wdoc(tsv));
CREATE INDEX tz_pos ON tz USING weave (to_wdoc(tsv)) WITH (positions = on);
SELECT q, (SELECT array_agg(id ORDER BY id) FROM tz WHERE tsv @@ q::tsquery) AS core,
	   (SELECT array_agg(id ORDER BY id) FROM tz WHERE to_wdoc(tsv) @@@ q::tsquery::wquery) AS heap,
	   weave_count('tz_tsv', q::tsquery::wquery) AS ix_tsv,
	   weave_count('tz_expr', q::tsquery::wquery) AS ix_expr,
	   weave_count('tz_pos', q::tsquery::wquery) AS ix_pos
  FROM (VALUES ('quick <0> brown'), ('brown <0> quick'), ('fox <0> dog'),
			   ('(quick <0> brown) <-> fox'), ('quick <-> (fox <0> dog)'),
			   ('(fox <0> dog) <2> (quick <0> brown)'), ('quick <0> brown <2> dog'),
			   ('quick <2> fox'), ('quick <-> fox')) v(q);

-- the binary form keeps the exact-gap flag: through COPY (FORMAT binary) and
-- back (wquery_send -> wquery_recv) the value has the same bytes and the
-- received one answers like core.  Per-backend file name (G60).
CREATE TEMP TABLE wqa (id int, q text, w wquery);
CREATE TEMP TABLE wqb (id int, w wquery);
INSERT INTO wqa SELECT i, q, q::tsquery::wquery
  FROM unnest(ARRAY['quick <0> brown', 'quick <2> brown', 'quick <-> (brown <2> fox)',
					'(quick <2> brown) <-> fox', 'fox <0> fox']) WITH ORDINALITY u(q, i);
SELECT '/tmp/pg_weave_wq_rt_' || pg_backend_pid() || '.bin' AS wqfile \gset
COPY (SELECT id, w FROM wqa) TO :'wqfile' WITH (FORMAT binary);
COPY wqb FROM :'wqfile' WITH (FORMAT binary);
SELECT a.q, wquery_send(b.w) = wquery_send(a.w) AS binary_same, b.w::text AS received,
	   (SELECT count(*) FROM tc WHERE to_wdoc(tsv) @@@ b.w) AS recv_rows,
	   (SELECT count(*) FROM tc WHERE tsv @@ a.q::tsquery) AS core_rows
  FROM wqa a JOIN wqb b USING (id) ORDER BY id;

-- PRE-EXISTING, recorded not fixed: wquery_out's text does not parse back.
-- wquery_in reads '<' and '>' as separators and the '-' of '<->' as NOT, and
-- drops a suffix after a quoted term, so each rendering below re-parses to a
-- different query.  (Binary send/recv above is exact.)
SELECT w::text AS rendered, w::text::wquery::text AS reparsed,
	   w::text::wquery::text = w::text AS round_trips
  FROM (VALUES ('quick <-> brown'::tsquery::wquery), ('quick <2> brown'::tsquery::wquery),
			   ('fo:*'::tsquery::wquery), ('fox:A'::tsquery::wquery),
			   ('"quick brown"'::wquery)) v(w);

-- the ranked arm really is an index scan with the ranking pass
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM tc WHERE tsv @@@ 'fox:A'::wquery ORDER BY tsv <=> 'fox:A'::wquery LIMIT 1000;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- ---- the index's other inexact leaves (native wquery, no cast) ------------
-- With any fuzzy or regex leaf the collector's candidate set was the union of
-- those leaves' matches, whatever the rest of the query said, so an OR with a
-- plain term or any NOT returned only the leaves' rows.  Seqscan vs both indexes.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT q, (SELECT count(*) FROM tc WHERE to_wdoc(tsv) @@@ q::wquery) AS heap,
	   weave_count('tc_expr', q::wquery) AS expr_ix,
	   weave_count('tc_tsv', q::wquery) AS tsv_ix
  FROM (VALUES ('brwn~1'), ('quick | brwn~1'), ('!brwn~1'), ('quick & !brwn~1'),
			   ('!(quick & brwn~1)'), ('/fo.*/'), ('dog | /fo.*/'), ('!/fo.*/'),
			   ('dog & !/fo.*/'), ('brwn~1 & /fo.*/'), ('brwn~1 | /fo.*/'),
			   ('!(brwn~1 | /fo.*/)'), ('"quick brwn~1"'), ('quick & (dog | brwn~1)'),
			   ('quick | (dog & brwn~1)')) v(q);
RESET enable_indexscan;
RESET enable_bitmapscan;
