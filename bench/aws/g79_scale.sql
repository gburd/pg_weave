-- bench/aws/g79_scale.sql -- doc/GAPS.md G79 at scale (hard rule 12: it touches
-- tombstones).  psql -X -v n=<rows> -f bench/aws/g79_scale.sql
--
-- The shape the bug needs, at n rows: DELETE ~10 %, VACUUM (tombstones), INSERT
-- ~10 % new rows that REUSE the freed ctids, then compare the vector ORDER BY
-- against the heap, twice: with the new rows in the pending list, and after a
-- second VACUUM has flushed them into a newer bolt.
--
-- THE NEW ROWS ARE FAR AWAY (every coordinate + 100), and that is what makes the
-- check exact despite the quantizer: no query below can have a new row among its
-- nearest neighbours, so ANY new row in an index top-k is a wrong answer -- the
-- G79 signature, a recycled ctid ranked at the dead row's distance.  Recall against
-- the heap is reported too, before the DELETE and after, but only `bad`, `dup` and
-- the drain's ordering are gated: recall is the quantizer's, not this fix's.
--
-- Self-checking: the last statement RAISEs if any gate failed, so the exit status
-- of psql (ON_ERROR_STOP) is the verdict.  Every number is also printed.
\set ON_ERROR_STOP on
SET client_min_messages = notice;
SET pg_weave.vec_kernel = 'scalar';
DROP TABLE IF EXISTS g79s, g79q, g79dead, g79res;
SELECT setseed(0.79);
CREATE TABLE g79s (id int, body wdoc, emb wvec(16)) WITH (autovacuum_enabled = off);
INSERT INTO g79s
  SELECT g, to_wdoc('simple', 'common t' || (g % 1000)),
         ('[' || array_to_string(ARRAY(SELECT round(random()::numeric, 4)
                                         FROM generate_series(1, 16) WHERE g > 0), ',')
          || ']')::wvec
    FROM generate_series(1, :n) g;
CREATE INDEX g79s_w ON g79s USING weave (body, emb);
ANALYZE g79s;
CREATE TABLE g79q AS
  SELECT q, ('[' || array_to_string(ARRAY(SELECT round(random()::numeric, 4)
                                            FROM generate_series(1, 16) WHERE q > 0), ',')
             || ']')::wvec AS v
    FROM generate_series(1, 50) q;
CREATE TABLE g79res (stage text, query text, lim int, nq int, bad bigint, dup bigint,
                     q_with_bad int, recall numeric);

CREATE FUNCTION pg_temp.wait_for_horizon() RETURNS void LANGUAGE plpgsql AS $$
DECLARE x xid := (txid_current() % 4294967296)::text::xid; i int;
BEGIN
  FOR i IN 1 .. 600 LOOP
    IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
                    WHERE datname = current_database() AND pid <> pg_backend_pid()
                      AND (age(backend_xmin) > age(x) OR age(backend_xid) > age(x))) THEN
      RETURN;
    END IF;
    PERFORM pg_sleep(0.1);
    PERFORM pg_stat_clear_snapshot();
  END LOOP;
  RAISE NOTICE 'wait_for_horizon: an older snapshot was still held after 60 s';
END $$;

-- The index arm must BE the index: assert the plan before measuring it.
CREATE FUNCTION pg_temp.g79_plan(sql text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP p := p || l || E'\n'; END LOOP;
  IF p !~ 'Index Scan using g79s_w' OR p ~ 'Sort' THEN
    RAISE EXCEPTION 'index arm is not the index ordering scan: %', p;
  END IF;
END $$;

-- kind: 'vec' = ORDER BY emb <-> q, 'fuse' = ORDER BY fuse(body <=> 'common', emb <-> q)
CREATE FUNCTION pg_temp.g79_measure(stage text, kind text, lim int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
  r record; a_idx int[]; a_heap int[]; inner_sql text; sql text;
  nbad bigint := 0; ndup bigint := 0; qbad int := 0; ov bigint := 0; tot bigint := 0; b int;
BEGIN
  FOR r IN SELECT q, v FROM g79q ORDER BY q LOOP
    inner_sql := format('SELECT id FROM g79s ORDER BY %s LIMIT %s',
                  CASE kind WHEN 'vec' THEN format('emb <-> %L::wvec', r.v)
                            ELSE format('fuse(body <=> %L::wquery, emb <-> %L::wvec)', 'common', r.v) END,
                  lim);
    sql := 'SELECT array_agg(id) FROM (' || inner_sql || ') s';
    IF r.q = 1 THEN PERFORM pg_temp.g79_plan(inner_sql); END IF;
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    EXECUTE sql INTO a_idx;
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    EXECUTE sql INTO a_heap;
    PERFORM set_config('enable_indexscan', 'on', true);
    b := (SELECT count(*) FROM unnest(a_idx) x WHERE x >= 1000000000);
    nbad := nbad + b;
    IF b > 0 THEN qbad := qbad + 1; END IF;
    ndup := ndup + cardinality(a_idx) - (SELECT count(DISTINCT x) FROM unnest(a_idx) x);
    ov := ov + (SELECT count(*) FROM (SELECT unnest(a_idx) INTERSECT SELECT unnest(a_heap)) z);
    tot := tot + cardinality(a_heap);
  END LOOP;
  INSERT INTO g79res VALUES (stage, kind, lim, 50, nbad, ndup, qbad, round(ov::numeric / tot, 4));
END $$;

-- One LIMIT-less drain: every row once, and every new row after every old one.
CREATE FUNCTION pg_temp.g79_drain(stage text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE v wvec; n bigint; nd bigint; lastold bigint; firstnew bigint; nrows bigint;
BEGIN
  SELECT q.v INTO v FROM g79q q WHERE q = 1;
  PERFORM set_config('enable_seqscan', 'on', true);
  SELECT count(*) INTO nrows FROM g79s;
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  EXECUTE format($q$
    SELECT count(*), count(DISTINCT id), max(rn) FILTER (WHERE id < 1000000000),
           min(rn) FILTER (WHERE id >= 1000000000)
      FROM (SELECT row_number() OVER () AS rn, id
              FROM (SELECT id FROM g79s ORDER BY emb <-> %L::wvec) s) t $q$, v)
    INTO n, nd, lastold, firstnew;
  PERFORM set_config('enable_seqscan', 'on', true);
  RAISE NOTICE 'drain % : rows=% emitted=% distinct=% last_old_rank=% first_new_rank=%',
    stage, nrows, n, nd, lastold, firstnew;
  INSERT INTO g79res VALUES (stage, 'drain', 0, 1,
    CASE WHEN firstnew IS NOT NULL AND firstnew < lastold THEN 1 ELSE 0 END
      + CASE WHEN n <> nrows THEN 1 ELSE 0 END,
    n - nd, 0, NULL);
END $$;

SELECT pg_temp.g79_measure('0 built', 'vec', 10);

CREATE TABLE g79dead AS SELECT ctid AS tid, id FROM g79s WHERE (hashint4(id) & 1023) < 102;
DELETE FROM g79s WHERE (hashint4(id) & 1023) < 102;
SELECT pg_temp.wait_for_horizon();
VACUUM g79s;
SELECT 'after VACUUM 1' AS stage, (SELECT count(*) FROM g79dead) AS deleted,
       ndeleted AS tombstones, weave_index_nsegments('g79s_w') AS bolts
  FROM weave_index_stats('g79s_w');

INSERT INTO g79s
  SELECT 1000000000 + g, to_wdoc('simple', 'common new' || g),
         ('[' || array_to_string(ARRAY(SELECT round((100 + random())::numeric, 4)
                                         FROM generate_series(1, 16) WHERE g > 0), ',')
          || ']')::wvec
    FROM generate_series(1, (SELECT count(*) FROM g79dead)) g;
SET enable_seqscan = on;
SELECT 'after INSERT' AS stage,
       (SELECT count(*) FROM g79s s JOIN g79dead d ON s.ctid = d.tid WHERE s.id >= 1000000000)
         AS new_rows_on_a_dead_ctid,
       (SELECT count(*) FROM g79s WHERE id >= 1000000000) AS new_rows;

SELECT pg_temp.g79_measure('1 pending', 'vec', 10);
SELECT pg_temp.g79_measure('1 pending', 'vec', 100);
SELECT pg_temp.g79_measure('1 pending', 'fuse', 10);
SELECT pg_temp.g79_drain('1 pending');

SELECT pg_temp.wait_for_horizon();
VACUUM g79s;
SELECT 'after VACUUM 2' AS stage, ndeleted AS tombstones, weave_index_nsegments('g79s_w') AS bolts
  FROM weave_index_stats('g79s_w');
SELECT pg_temp.g79_measure('2 flushed', 'vec', 10);
SELECT pg_temp.g79_measure('2 flushed', 'vec', 100);
SELECT pg_temp.g79_measure('2 flushed', 'fuse', 10);
SELECT pg_temp.g79_drain('2 flushed');

SELECT * FROM g79res ORDER BY stage, query, lim;
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM g79res WHERE bad > 0 OR dup > 0) THEN
    RAISE EXCEPTION 'G79 SCALE GATE FAILED';
  END IF;
  RAISE NOTICE 'G79 SCALE GATE PASSED';
END $$;
DROP TABLE g79s, g79q, g79dead, g79res;
