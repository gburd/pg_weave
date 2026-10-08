--
-- doc/GAPS.md G94: `WHERE d @@@ q ORDER BY d <=> q` must return EVERY match of
-- q, in distance order, when q is not covered by its literal terms -- a NOT
-- (`!fox`, `fox | !dog`), a prefix, fuzzy or regex leaf.  The ranked pass
-- generates candidates from q's literal terms only, so a match holding none of
-- them is never ranked; it used to be lost, because the G56 padding phase was
-- skipped whenever the restriction was the ORDER BY query.
--
-- The order argument (src/am/amscan.c weave_pad_wanted): a match the ranked
-- pass misses holds no literal term, so its BM25 over q's literal terms is 0 and
-- its distance is exactly 1.0 -- the value the padding emits it at, and >= every
-- ranked distance.  A match through the NOT that holds a literal term from
-- another branch IS a candidate and is ranked.  Every check below is against
-- two oracles that do not use the ordering scan: the heap's `@@@` (the set) and
-- weave_search()'s scores (the distance, 1.0 for a row it does not rank), plus
-- a third that does not use the index at all: a row is ranked (< 1.0) exactly
-- when it holds one of q's literal terms (`lits`).
--
-- What each block guards, and the mutant that proves it can fail
-- (bench/aws/g94_mutants.sh):
--   1. every window (LIMIT below / above the ranked count, OFFSET across the
--      boundary, the tail, no LIMIT) is a correct ORDER BY answer: no row short,
--      none extra, none twice, each at its own distance, each at its place
--                    (mutants: no padding; padding before the ranked phase ends;
--                     padding at a distance below the ranked ones)
--   2. `!"quick brown"`: a NOT over a phrase is an over-generating gate, so the
--      padding's rows need the executor's recheck  (mutant: padding unrechecked)
--   3. a cursor, with MOVE and another ordering scan between fetches
--   4. a rescan per outer row (a correlated subplan)
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET jit = off;
SET max_parallel_workers_per_gather = 0;

-- deterministic documents: tf 1..3 of fox, zero-term documents (g % 4 = 0 with
-- no other term), the exact term `fo` (g % 101 = 0) so a prefix query has one
-- literal-term row, `quick brown` adjacent (g % 11) and `brown quick` (g % 13)
CREATE TABLE rn (id int, d wdoc, tag int DEFAULT 0)
  WITH (autovacuum_enabled = off, fillfactor = 60);
INSERT INTO rn (id, d) SELECT g, to_wdoc('simple', concat_ws(' ',
	CASE WHEN g % 5 = 0 THEN repeat('fox ', 1 + g % 3) END,
	CASE WHEN g % 7 = 0 THEN 'dog' END,
	CASE WHEN g % 11 = 0 THEN 'quick brown' END,
	CASE WHEN g % 13 = 0 THEN 'brown quick' END,
	CASE WHEN g % 17 = 0 THEN 'fog foggy' END,
	CASE WHEN g % 101 = 0 THEN 'fo' END,
	CASE WHEN g % 4 > 0 THEN repeat('x' || (g % 19) || ' ', g % 4) END))
  FROM generate_series(1, 2000) g;
INSERT INTO rn (id, d) SELECT g, NULL FROM generate_series(2001, 2010) g;
CREATE INDEX rn_w ON rn USING weave (d);
-- after the build: pending documents (some term-free), changed documents, deletes
INSERT INTO rn (id, d) SELECT g, to_wdoc('simple',
	CASE WHEN g % 2 = 0 THEN 'fox x3' WHEN g % 3 = 0 THEN '' ELSE 'x5 x5' END)
  FROM generate_series(2011, 2040) g;
UPDATE rn SET d = to_wdoc('simple', 'dog x7') WHERE id IN (3, 6, 9);
DELETE FROM rn WHERE id % 97 = 0;
-- an index TID is a HOT-chain root, which a later HOT update moves away from the
-- row's ctid, so weave_search()'s TIDs are mapped to ids through every TID seen
CREATE TEMP TABLE rootmap AS SELECT ctid AS tid, id FROM rn;
UPDATE rn SET tag = 1 WHERE id % 50 = 1;
INSERT INTO rootmap SELECT ctid, id FROM rn r
 WHERE NOT EXISTS (SELECT 1 FROM rootmap m WHERE m.tid = r.ctid);
SELECT count(*) AS rows, count(*) FILTER (WHERE d IS NULL) AS null_docs,
	   count(*) FILTER (WHERE d IS NOT NULL AND NOT d @@@ '!zzz') AS not_matching_bang_zzz
  FROM rn;

-- the queries, each with the OR of its literal terms (NULL: none is a term)
CREATE TEMP TABLE qs (qn int, q text, lits text);
INSERT INTO qs VALUES
	(1, 'fox', 'fox'),							-- covered: no padding
	(2, '!fox', 'fox'),
	(3, '!fox & !dog', 'fox | dog'),
	(4, '!(fox | dog)', 'fox | dog'),
	(5, 'fox | !dog', 'fox | dog'),
	(6, 'quick | !fox', 'quick | fox'),
	(7, '(fox & !dog) | !quick', 'fox | dog | quick'),
	(8, '!(fox & dog)', 'fox | dog'),
	(9, '!"quick brown"', 'quick | brown'),
	(10, 'fox & !"quick brown"', 'fox | quick | brown'),	-- covered
	(11, 'fo*', 'fo'),
	(12, 'fo* | dog', 'fo | dog'),
	(13, 'fox & fo*', 'fox | fo'),				-- covered
	(14, 'brwn~1', 'brwn'),
	(15, '/fo.*/', NULL),
	(16, 'dog | !/fo.*/', 'dog'),
	(17, '!zzz', 'zzz'),
	(18, 'zzz', 'zzz');							-- covered, empty

-- the oracle: the heap's match set (no index), each row at weave_search()'s
-- distance, 1.0 when it does not rank the row; and whether it holds a literal term
SET enable_indexscan = off; SET enable_bitmapscan = off;
CREATE TEMP TABLE ex (q text, id int, dist float8, hasterm bool);
DO $$
DECLARE r record;
BEGIN
	FOR r IN SELECT * FROM qs ORDER BY qn LOOP
		INSERT INTO ex
		SELECT r.q, h.id, coalesce(w.dist, 1.0),
			   coalesce(h.d @@@ r.lits::wquery, false)
		  FROM rn h
		  LEFT JOIN (SELECT m.id, 1.0 / (1.0 + s.score) AS dist
					   FROM weave_search('rn_w', r.q::wquery, 100000) s
					   JOIN rootmap m ON m.tid = s.ctid) w USING (id)
		 WHERE h.d @@@ r.q::wquery;
	END LOOP;
END $$;
RESET enable_indexscan; RESET enable_bitmapscan;
CREATE INDEX ON ex (q, id);
ANALYZE ex;

-- the ordering scan's settings live on the functions that run it, so the
-- checking queries plan normally
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
EXPLAIN (COSTS OFF)
SELECT id FROM rn WHERE d @@@ '!fox'::wquery ORDER BY d <=> '!fox'::wquery LIMIT 10;
RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;

-- one stream, in the scan's order, with the distance the scan published per row
CREATE TEMP TABLE st (ord int, id int, cur float8);
CREATE FUNCTION pg_temp.verify(q text, pos0 int, expect int, OUT n int, OUT short int,
							   OUT extra int, OUT dup int, OUT wrong_value int,
							   OUT misplaced int)
LANGUAGE sql SET enable_seqscan = on SET enable_bitmapscan = on SET enable_sort = on AS $$
	WITH o AS (SELECT row_number() OVER (ORDER BY dist) - 1 AS pos, dist
				 FROM ex WHERE ex.q = verify.q)
	SELECT (SELECT count(*)::int FROM st),
		   expect - (SELECT count(*)::int FROM st),
		   (SELECT count(*)::int FROM st LEFT JOIN ex e ON e.q = verify.q AND e.id = st.id
			 WHERE e.id IS NULL),
		   (SELECT (count(*) - count(DISTINCT id))::int FROM st),
		   (SELECT count(*)::int FROM st JOIN ex e ON e.q = verify.q AND e.id = st.id
			 WHERE st.cur IS NULL OR abs(st.cur - e.dist) > 1e-9 * e.dist),
		   (SELECT count(*)::int FROM st JOIN ex e ON e.q = verify.q AND e.id = st.id
			  JOIN o ON o.pos = verify.pos0 + st.ord WHERE e.dist <> o.dist)
$$;
CREATE FUNCTION pg_temp.win(q text, lim int, off int) RETURNS text LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off SET enable_sort = off AS $$
DECLARE
	r record;
	v record;
	i int := 0;
	m int := (SELECT count(*) FROM ex WHERE ex.q = win.q);
BEGIN
	DELETE FROM st;
	FOR r IN EXECUTE format('SELECT id, weave_current_distance(''rn_w'', ctid, %1$L::wquery) AS cur '
							'FROM rn WHERE d @@@ %1$L::wquery ORDER BY d <=> %1$L::wquery '
							'LIMIT %2$s OFFSET %3$s', q, coalesce(lim::text, 'ALL'), off)
	LOOP
		INSERT INTO st VALUES (i, r.id, r.cur);
		i := i + 1;
	END LOOP;
	SELECT * INTO v FROM pg_temp.verify(q, off, greatest(0, least(coalesce(lim, m), m - off)));
	IF v.short = 0 AND v.extra = 0 AND v.dup = 0 AND v.wrong_value = 0 AND v.misplaced = 0 THEN
		RETURN NULL;
	END IF;
	RETURN format('L%s/O%s: n=%s short=%s extra=%s dup=%s wrong_value=%s misplaced=%s',
				  coalesce(lim::text, 'ALL'), off, v.n, v.short, v.extra, v.dup,
				  v.wrong_value, v.misplaced);
END $$;

-- each query really is answered by the ordering scan on rn_w
CREATE FUNCTION pg_temp.is_ix(q text) RETURNS bool LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off SET enable_sort = off AS $$
DECLARE l text; ix bool := false; ob bool := false;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id FROM rn WHERE d @@@ %1$L::wquery '
							'ORDER BY d <=> %1$L::wquery LIMIT 10', q) LOOP
		ix := ix OR l ~ 'Index Scan using rn_w';
		ob := ob OR l LIKE '%Order By: (d <=>%';
	END LOOP;
	RETURN ix AND ob;
END $$;

-- ---- 1. windows ------------------------------------------------------------
-- nranked: rows below the floor distance.  terms_ok: a row is ranked exactly
-- when it holds a literal term, so the padding's rows are the floor rows.
SELECT qs.qn, qs.q, x.nmatch, x.nranked, x.terms_ok, pg_temp.is_ix(qs.q) AS ix,
	   coalesce((SELECT string_agg(f, '; ')
				   FROM (SELECT pg_temp.win(qs.q, w.lim, w.off) AS f
						   FROM (VALUES (5, 0), (greatest(x.nranked - 3, 1), 0),
										(x.nranked + 5, 0), (10, greatest(x.nranked - 5, 0)),
										(7, greatest(x.nmatch - 4, 0)), (NULL, 0)) w(lim, off)) y),
				'ok') AS windows
  FROM qs,
	   LATERAL (SELECT count(*)::int AS nmatch,
					   count(*) FILTER (WHERE dist < 1.0)::int AS nranked,
					   bool_and((dist < 1.0) = hasterm) AS terms_ok
				  FROM ex WHERE ex.q = qs.q) x
 ORDER BY qs.qn;

-- ---- 3. a cursor ----------------------------------------------------------
-- 3 rows, MOVE 20, then the rest one at a time, with a different ordering scan
-- on the same index between fetches: each row's published distance must be its own
CREATE FUNCTION pg_temp.cur(q text) RETURNS text LANGUAGE plpgsql
SET enable_seqscan = off SET enable_bitmapscan = off SET enable_sort = off AS $$
DECLARE
	c refcursor := 'rn_c';
	r record;
	i int := 0;
	m int := (SELECT count(*) FROM ex WHERE ex.q = cur.q);
	v record;
BEGIN
	DELETE FROM st;
	OPEN c FOR EXECUTE format('SELECT id, weave_current_distance(''rn_w'', ctid, %1$L::wquery) AS cur '
							  'FROM rn WHERE d @@@ %1$L::wquery ORDER BY d <=> %1$L::wquery', q);
	LOOP
		IF i = 3 THEN
			MOVE FORWARD 20 IN c;
			i := i + 20;
		END IF;
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		INSERT INTO st VALUES (i, r.id, r.cur);
		i := i + 1;
		IF i % 50 = 0 THEN
			PERFORM count(*) FROM (SELECT id FROM rn WHERE d @@@ 'dog'::wquery
									ORDER BY d <=> 'dog'::wquery LIMIT 3) z;
		END IF;
	END LOOP;
	CLOSE c;
	-- the 20 moved rows are positions 3..22; verify() checks the rest in place
	SELECT * INTO v FROM pg_temp.verify(q, 0, least(m, 3) + greatest(0, m - 23));
	RETURN format('n=%s short=%s extra=%s dup=%s wrong_value=%s misplaced=%s',
				  v.n, v.short, v.extra, v.dup, v.wrong_value, v.misplaced);
END $$;
SELECT q, pg_temp.cur(q) AS cursor FROM qs WHERE qn IN (2, 5, 9, 11) ORDER BY qn;

-- ---- 4. a rescan per outer row ------------------------------------------
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
SELECT v.q, l.lim,
	   (SELECT count(*) FROM (SELECT 1 FROM rn WHERE d @@@ v.q::wquery
							  ORDER BY d <=> v.q::wquery LIMIT l.lim) z) AS n,
	   least(l.lim, (SELECT count(*) FROM ex WHERE ex.q = v.q)) AS want
  FROM (VALUES ('!fox'), ('fox | !dog'), ('fo*')) v(q), (VALUES (3), (500), (5000)) l(lim)
 ORDER BY 1, 2;

RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;
DROP TABLE rn;
