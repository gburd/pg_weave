-- WEAVE_TEST_HOOKS + WEAVE_TEST_ORD_WIDTH_MAX=128 only.
-- Removing the cap+1 probe must fail BOTH overlapping-set controls.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET jit = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;
SET pg_weave.wand_initial_k = 32;
CREATE TABLE capoverlap (id int, d wdoc);
INSERT INTO capoverlap SELECT g, to_wdoc('simple', 'fox dog')
  FROM generate_series(1,128) g;
CREATE INDEX capoverlap_w ON capoverlap USING weave(d);
CREATE TEMP TABLE capfail (label text);
CREATE FUNCTION pg_temp.capcheck(q text, want int, label text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE p json; n int; unique_n int;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
  IF p->0->'Plan'->>'Node Type' <> 'Index Scan' THEN
    RAISE EXCEPTION 'G95 overlap ordering site not reached: %', p;
  END IF;
  BEGIN
    EXECUTE 'SELECT count(*), count(DISTINCT id) FROM (' || q || ') s'
      INTO n, unique_n;
  EXCEPTION WHEN program_limit_exceeded THEN
    IF want = -1 THEN
      RAISE NOTICE 'G95 overlap PASS %: 54000', label;
      RETURN;
    END IF;
    RAISE NOTICE 'G95 overlap FAIL %: unexpected 54000', label;
    INSERT INTO capfail VALUES (label);
    RETURN;
  END;
  IF n <> want OR unique_n <> want THEN
    RAISE NOTICE 'G95 overlap FAIL %: rows %, distinct %, wanted %', label, n, unique_n, want;
    INSERT INTO capfail VALUES (label);
  ELSE
    RAISE NOTICE 'G95 overlap PASS %: % distinct rows', label, n;
  END IF;
END $$;
-- Two channels, same 128 documents: summed maxhits is 256, not 128.
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap ORDER BY fuse(d <=> ''fox''::wquery, d <=> ''dog''::wquery)',
  128, 'fused exact cap');
-- The empty half makes min(df(fox)+df(dog), N)=256. Rebuild so both
-- counterexamples exercise flushed postings rather than only pending entries.
INSERT INTO capoverlap SELECT g, to_wdoc('simple', '') FROM generate_series(129,256) g;
REINDEX INDEX capoverlap_w;
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap WHERE d @@@ ''fox | dog''::wquery ORDER BY d <=> ''fox | dog''::wquery',
  128, 'lexical exact cap');
-- Also drain padding, not just the covered ranked set.
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap WHERE d @@@ ''fox | dog | !absent''::wquery ORDER BY d <=> ''fox | dog | !absent''::wquery',
  256, 'lexical exact cap plus padding');
INSERT INTO capoverlap VALUES (257, to_wdoc('simple', 'fox dog'));
REINDEX INDEX capoverlap_w;
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap WHERE d @@@ ''fox | dog''::wquery ORDER BY d <=> ''fox | dog''::wquery',
  -1, 'lexical cap plus one');
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap ORDER BY fuse(d <=> ''fox''::wquery, d <=> ''dog''::wquery)',
  -1, 'fused cap plus one');
-- Do not let the probe itself escape as a successful LIMIT cap+1 prefix.
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap WHERE d @@@ ''fox | dog''::wquery ORDER BY d <=> ''fox | dog''::wquery LIMIT 129',
  -1, 'lexical cap plus one LIMIT');
SELECT pg_temp.capcheck(
  'SELECT id FROM capoverlap ORDER BY fuse(d <=> ''fox''::wquery, d <=> ''dog''::wquery) LIMIT 129',
  -1, 'fused cap plus one LIMIT');
DO $$
DECLARE failures text;
BEGIN
  SELECT string_agg(label, ', ' ORDER BY label) INTO failures FROM capfail;
  IF failures IS NOT NULL THEN
    RAISE EXCEPTION 'G95 overlap controls failed: %', failures;
  END IF;
  RAISE NOTICE 'G95 overlap controls complete';
END $$;
DROP TABLE capoverlap;
