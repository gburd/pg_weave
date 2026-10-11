-- Run with WEAVE_TEST_HOOKS and WEAVE_TEST_ORD_WIDTH_MAX=128.
-- The baseline completes with fabricated zero scores, not a resource error.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
SET jit = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SET pg_weave.wand_initial_k = 32;
CREATE TABLE capdocs (id int, d wdoc, v wvec(4));
INSERT INTO capdocs SELECT g, to_wdoc('simple', CASE WHEN g <= 300 THEN
  repeat('fox ', 1 + g % 5) ELSE '' END),
  ('[' || g || ',1,2,3]')::wvec FROM generate_series(1,400) g;
CREATE INDEX capdocs_w ON capdocs USING weave(d, v);
ANALYZE capdocs;
-- Control: below the cap remains usable.
SELECT count(*) = 10 AS small_limit_ok FROM
 (SELECT id FROM capdocs WHERE d @@@ 'fox | !absent'::wquery
  ORDER BY d <=> 'fox | !absent'::wquery LIMIT 10) s;
CREATE TEMP TABLE capgot (id int, dist float8);
DO $$
DECLARE p json; bad int; msg text; hint text;
BEGIN
  EXPLAIN (FORMAT JSON) SELECT id FROM capdocs
    WHERE d @@@ 'fox | !absent'::wquery
    ORDER BY d <=> 'fox | !absent'::wquery INTO p;
  IF p->0->'Plan'->>'Node Type' <> 'Index Scan' THEN
    RAISE EXCEPTION 'G95 cap ordering site not reached: %', p;
  END IF;
  BEGIN
    INSERT INTO capgot SELECT id,
      weave_current_distance('capdocs_w', ctid, 'fox | !absent'::wquery)
      FROM capdocs WHERE d @@@ 'fox | !absent'::wquery
      ORDER BY d <=> 'fox | !absent'::wquery;
  EXCEPTION WHEN program_limit_exceeded THEN
    GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, hint = PG_EXCEPTION_HINT;
    IF msg NOT LIKE '%128%' OR hint = '' THEN
      RAISE EXCEPTION 'G95 cap missing limit/hint: % / %', msg, hint;
    END IF;
    RAISE NOTICE 'G95 lexical cap refused: % [%]', msg, hint;
    RETURN;
  END;
  SELECT count(*) INTO bad FROM capgot WHERE id <= 300 AND dist = 1;
  RAISE EXCEPTION 'G95 cap not refused: % positive rows fabricated at distance 1', bad;
END $$;
-- A low-scoring pending row must not pass the capped fused frontier, even
-- before grow() is asked for another batch. Previously LIMIT 129 could return
-- that row in place of a better flushed row without ever calling grow().
INSERT INTO capdocs VALUES (401, to_wdoc('simple', 'fox ' || repeat('filler ', 100)), '[401,1,2,3]');
DO $$
BEGIN
  BEGIN
    PERFORM * FROM
      (SELECT id FROM capdocs
       ORDER BY fuse(d <=> 'fox'::wquery, d <=> 'absent'::wquery)
       LIMIT 129) s;
  EXCEPTION WHEN program_limit_exceeded THEN
    RAISE NOTICE 'G95 pending frontier refused';
    RETURN;
  END;
  RAISE EXCEPTION 'G95 pending frontier released before exhaustion';
END $$;
-- The sibling ladders must refuse too, not silently pad/truncate.
DO $$
DECLARE q text; p json;
BEGIN
  FOREACH q IN ARRAY ARRAY[
    'SELECT id FROM capdocs ORDER BY v <-> ''[1,1,2,3]''::wvec',
    'SELECT id FROM capdocs ORDER BY fuse(d <=> ''fox''::wquery, d <=> ''absent''::wquery)'
  ] LOOP
    EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
    IF p->0->'Plan'->>'Node Type' <> 'Index Scan' THEN
      RAISE EXCEPTION 'G95 sibling cap ordering site not reached: %', p;
    END IF;
    BEGIN
      EXECUTE 'CREATE TEMP TABLE sibling AS ' || q;
      RAISE EXCEPTION 'G95 sibling cap not refused: %', q;
    EXCEPTION WHEN program_limit_exceeded THEN
      RAISE NOTICE 'G95 sibling cap refused: %', q;
    END;
  END LOOP;
END $$;
DROP TABLE capdocs;
-- A full heap at the cap is NOT an error when maxhits proves exhaustion.
CREATE TABLE capexact (id int, d wdoc);
INSERT INTO capexact SELECT g, to_wdoc('simple', 'fox') FROM generate_series(1,128) g;
CREATE INDEX capexact_w ON capexact USING weave(d);
DO $$
DECLARE n int;
BEGIN
  SELECT count(*) INTO n FROM
    (SELECT id FROM capexact WHERE d @@@ 'fox'::wquery ORDER BY d <=> 'fox'::wquery) s;
  IF n <> 128 THEN RAISE EXCEPTION 'G95 proven exhaustion short: %', n; END IF;
  RAISE NOTICE 'G95 proven exhaustion at cap returns 128';
END $$;
DROP TABLE capexact;
