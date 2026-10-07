-- doc/readme_examples.sql -- every SQL statement shown in README.md, in order.
--
-- README.md quotes only output produced by running this file.  To check the README
-- again, run it the way its quoted output was produced, on EC2 Debian against
-- PostgreSQL 17 and 18:
--
--     SCRIPT=bench/aws/readme_job.sh bench/aws/run.sh c7i.2xlarge script
--
-- or on any server with the extension installed:
--
--     psql -X -a -v ON_ERROR_STOP=1 -f doc/readme_examples.sql
--
-- It creates a table named `docs` and drops it at the end.

CREATE EXTENSION IF NOT EXISTS pg_weave;

-- ---------------------------------------------------------------- the table
CREATE TABLE docs (
    id     int PRIMARY KEY,
    title  text NOT NULL,
    body   wdoc GENERATED ALWAYS AS (to_wdoc(title)) STORED,
    emb    wvec(4),
    price  int8
);

INSERT INTO docs (id, title, emb, price) VALUES
  (1, 'PostgreSQL streaming replication setup',  '[0.9,0.1,0.0,0.0]', 40),
  (2, 'Logical replication with publications',   '[0.8,0.2,0.1,0.0]', 25),
  (3, 'Tuning autovacuum for large tables',       '[0.1,0.9,0.0,0.1]', 60),
  (4, 'VACUUM FULL versus table rewrites',        '[0.2,0.8,0.1,0.0]', 15),
  (5, 'Vector similarity search in PostgreSQL',  '[0.0,0.1,0.9,0.2]', 80),
  (6, 'Full text search ranking with BM25',       '[0.1,0.0,0.8,0.3]', 35),
  (7, 'WAL archiving and point in time recovery', '[0.7,0.0,0.0,0.6]', 55),
  (8, 'Monitoring replica lag in PostgreSQL',    '[0.8,0.3,0.0,0.2]', 20);

-- 20,000 filler rows, so the planner has a reason to use an index.
INSERT INTO docs (id, title, emb, price)
SELECT g, 'archived note ' || g,
       ARRAY[-1, -1, -(g % 10) / 10.0, -(g % 7) / 7.0]::real[],
       1000 + g
  FROM generate_series(100, 20099) g;

-- ---------------------------------------------------------------- one index
-- One column per channel: lexical (wdoc), vector (wvec), substring (gram_ops on
-- the raw text), and one scalar facet (int8_docval_ops).
CREATE INDEX docs_weave ON docs USING weave
    (body, emb, title gram_ops, price int8_docval_ops);
ANALYZE docs;

-- ---------------------------------------------------------------- BM25
SELECT id, title FROM docs
 WHERE body @@@ 'replication'
 ORDER BY body <=> 'replication'
 LIMIT 3;

-- The pgvector idiom, ORDER BY with no WHERE, is also an index scan.
EXPLAIN (COSTS OFF)
SELECT id FROM docs ORDER BY body <=> 'replication' LIMIT 3;

-- Boolean operators and phrases.
SELECT id, title FROM docs WHERE body @@@ 'postgresql & !vector' ORDER BY id;
SELECT id, title FROM docs WHERE body @@@ '"streaming replication"';

-- count(*) answered from the index.
SELECT count(*) FROM docs WHERE body @@@ 'postgresql';

-- ---------------------------------------------------------------- vector kNN
SELECT id, title FROM docs ORDER BY emb <-> '[1,0,0,0]' LIMIT 3;

-- ---------------------------------------------------------------- fuzzy, prefix, regex
SELECT id, title FROM docs WHERE body @@@ 'replicaton~1' ORDER BY id;   -- 1 edit
SELECT id, title FROM docs WHERE body @@@ 'vacu*' ORDER BY id;          -- prefix
SELECT id, title FROM docs WHERE body @@@ '/^repl.*n$/' ORDER BY id;    -- regex, per token

-- Rank by spelling distance.
SELECT id, title FROM docs ORDER BY body <@> 'replicaton' LIMIT 3;

-- Substring across token boundaries (LIKE / ILIKE semantics), from the gram_ops column.
SELECT id, title FROM docs WHERE title @~ '%al repl%';
SELECT id, title FROM docs WHERE title @~* '%postgresql stream%';

-- ---------------------------------------------------------------- scalar facet
SELECT id, title, price FROM docs
 WHERE body @@@ 'replication' AND price < 30
 ORDER BY id;

-- ---------------------------------------------------------------- hybrid
SELECT id, title FROM docs
 WHERE price < 100
 ORDER BY fuse(body <=> 'postgresql replication',
               emb  <-> '[1,0,0,0]',
               weights => '{0.5,0.5}')
 LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT id, title FROM docs
 WHERE price < 100
 ORDER BY fuse(body <=> 'postgresql replication',
               emb  <-> '[1,0,0,0]',
               weights => '{0.5,0.5}')
 LIMIT 5;

-- A fuzzy term as the filter of a fused ranking.
SELECT id, title FROM docs
 WHERE body @@@ 'replicaton~1'
 ORDER BY fuse(body <=> 'postgresql', emb <-> '[1,0,0,0]')
 LIMIT 5;

-- The fused score itself (superuser by default; see README).
SELECT d.id, d.title, round(s.score::numeric, 4) AS score
  FROM weave_fuse_search('docs_weave', ARRAY['postgresql replication'::wquery],
                         ARRAY['[1,0,0,0]'::wvec], '{0.5,0.5}', 3) s
  JOIN docs d ON d.ctid = s.ctid
 ORDER BY s.score DESC;

-- ---------------------------------------------------------------- operations
SELECT bool_and(ok) AS all_invariants_hold FROM weave_check('docs_weave');

SELECT 'readme_examples: every statement above ran' AS marker;

-- ---------------------------------------------------------------- refused on purpose
\set ON_ERROR_STOP off
CREATE INDEX docs_cos ON docs USING weave (body, emb) WITH (metric = 'cosine');
\set ON_ERROR_STOP on

DROP TABLE docs;
