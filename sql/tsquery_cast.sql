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

-- core's row set, the cast's row set (heap evaluation), the two index counts,
-- or the cast's refusal
CREATE FUNCTION pg_temp.cmp(q text, OUT core int[], OUT weave int[],
							OUT ix_ok bool, OUT refused text)
LANGUAGE plpgsql AS $$
DECLARE
	wq wquery;
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
		 AND weave_count('tc_expr', wq) = cardinality(core);
END $$;

-- ---- hand-written ---------------------------------------------------------
SELECT q, cardinality(c.core) AS ncore,
	   CASE WHEN c.refused IS NOT NULL THEN 'refused'
			WHEN c.core = c.weave AND c.ix_ok THEN 'same' ELSE 'DIFFERENT' END AS verdict,
	   c.refused
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
		-- phrase shapes wquery cannot express exactly
		('quick <2> brown'), ('quick <0> brown'), ('quick <3> brown'),
		('quick <-> (brown <-> fox)'), ('quick <-> (brown | fox)'),
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
-- coverage: the converted set exercises each feature at least once
SELECT count(*) FILTER (WHERE q ~ ':[A-D]+') AS weighted,
	   count(*) FILTER (WHERE q ~ ':\*') AS prefixed,
	   count(*) FILTER (WHERE q ~ '<->') AS phrased,
	   count(*) FILTER (WHERE q ~ '!') AS negated
  FROM rres WHERE refused IS NULL;

-- the text form of each mapped shape
SELECT q, q::tsquery::wquery::text AS wquery
  FROM (VALUES ('fox:A'), ('fox:BD'), ('fox:ABCD'), ('fo:*'), ('!fox:C'),
			   ('quick <-> brown <-> fox'), ('fox:A & (dog | la:*)')) v(q);
