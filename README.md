# pg_weave

pg_weave is a PostgreSQL index access method, `weave`. One `CREATE INDEX` gives you six
kinds of retrieval over the same rows: BM25 ranked text search, vector nearest-neighbour
search, fuzzy (edit-distance) terms, regular expressions over tokens, prefix terms, and
substring (`LIKE '%...%'`) search through a character n-gram channel. A scalar column can
be indexed as a facet too. Every channel in a segment shares one document-id space. That
lets a selective filter, a lexical term or a facet such as `price < 100`, skip vector work
inside the scan instead of being applied afterwards.

**Status: 0.28.0, pre-1.0, not production-ready.** All six retrieval kinds ship. The
fuzzy/n-gram (Z) and vector (V) phase gates are not met, the fused-ranking gate passes two
of its five rows, and several measured results are losses, listed below. `doc/PRODUCTION_READINESS.md` is the gate list and `doc/GAPS.md` lists the known
defects.

## Install

PostgreSQL 17 or 18, built with PGXS:

```sh
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
```

```sql
CREATE EXTENSION pg_weave;
```

The extension is `trusted`, so a database owner can create it without superuser.
It needs no `shared_preload_libraries` entry. `make installcheck` runs the regression,
isolation and TAP suites against the installed server.

If your `clang` is a newer major than the LLVM your PostgreSQL was built against, install
with `with_llvm=no`. Otherwise `make install` can fail partway through the bitcode step
and leave bitcode behind that the JIT cannot read. That crashes backends later, and the
crash need not be in a query that touches pg_weave. To repair it, remove
`$(pg_config --pkglibdir)/bitcode/pg_weave*`. On Debian 13 with PGDG packages (clang 19,
LLVM 19) the default install works.

## Example

This is `doc/readme_examples.sql`. Its output below comes from running it on PostgreSQL
17.11 and 18.6 (EC2 run `pgweave-20261006-212542-c6e5`). The two majors produced the same
output line for line.

```sql
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

-- One column per channel: lexical (wdoc), vector (wvec), substring (gram_ops on
-- the raw text), and one scalar facet (int8_docval_ops).
CREATE INDEX docs_weave ON docs USING weave
    (body, emb, title gram_ops, price int8_docval_ops);
ANALYZE docs;
```

BM25 ranking, boolean and phrase queries, and index-answered `count(*)`:

```sql
SELECT id, title FROM docs
 WHERE body @@@ 'replication'
 ORDER BY body <=> 'replication'
 LIMIT 3;
--  1 | PostgreSQL streaming replication setup
--  2 | Logical replication with publications

EXPLAIN (COSTS OFF)
SELECT id FROM docs ORDER BY body <=> 'replication' LIMIT 3;
--  Limit
--    ->  Index Scan using docs_weave on docs
--          Order By: (body <=> '''replication'''::wquery)

SELECT id, title FROM docs WHERE body @@@ 'postgresql & !vector' ORDER BY id;   -- 1, 8
SELECT id, title FROM docs WHERE body @@@ '"streaming replication"';           -- 1
SELECT count(*) FROM docs WHERE body @@@ 'postgresql';                         -- 3
```

Vector nearest neighbours (`<->` is L2; `<#>` is negative inner product, for an index
built `WITH (metric = 'ip')`):

```sql
SELECT id, title FROM docs ORDER BY emb <-> '[1,0,0,0]' LIMIT 3;
--  1 | PostgreSQL streaming replication setup
--  2 | Logical replication with publications
--  8 | Monitoring replica lag in PostgreSQL
```

Fuzzy, prefix, regex, spelling-distance ranking, and substring search:

```sql
SELECT id, title FROM docs WHERE body @@@ 'replicaton~1' ORDER BY id;   -- 1, 2  (one edit)
SELECT id, title FROM docs WHERE body @@@ 'vacu*' ORDER BY id;          -- 4
SELECT id, title FROM docs WHERE body @@@ '/^repl.*n$/' ORDER BY id;    -- 1, 2  (per token)
SELECT id, title FROM docs ORDER BY body <@> 'replicaton' LIMIT 3;      -- 1, 2, 8
SELECT id, title FROM docs WHERE title @~ '%al repl%';                  -- 2  (LIKE)
SELECT id, title FROM docs WHERE title @~* '%postgresql stream%';       -- 1  (ILIKE)
```

A facet and a lexical term in one index condition:

```sql
SELECT id, title, price FROM docs
 WHERE body @@@ 'replication' AND price < 30
 ORDER BY id;
--  2 | Logical replication with publications |    25
```

Hybrid ranking. `fuse()` takes one distance per channel and optional weights. With a
facet filter it is still one index scan with no Sort:

```sql
SELECT id, title FROM docs
 WHERE price < 100
 ORDER BY fuse(body <=> 'postgresql replication',
               emb  <-> '[1,0,0,0]',
               weights => '{0.5,0.5}')
 LIMIT 5;
--  1 | PostgreSQL streaming replication setup
--  2 | Logical replication with publications
--  8 | Monitoring replica lag in PostgreSQL
--  7 | WAL archiving and point in time recovery
--  4 | VACUUM FULL versus table rewrites

-- EXPLAIN (COSTS OFF) of the same query:
--  Limit
--    ->  Index Scan using docs_weave on docs
--          Index Cond: (price < 100)
--          Order By: ((body <=> '(''postgresql'' & ''replication'')'::wquery) AND
--                     (emb <-> '[1,0,0,0]'::wvec) AND (body <~> '{0.5,0.5}'::real[]))

SELECT id, title FROM docs
 WHERE body @@@ 'replicaton~1'
 ORDER BY fuse(body <=> 'postgresql', emb <-> '[1,0,0,0]')
 LIMIT 5;
-- 1, 2
```

To get the fused score, use `weave_fuse_search()`. A `fuse(...)` in the select list is
recomputed per row from the heap value and is not the score the scan ranked by.

```sql
SELECT d.id, d.title, round(s.score::numeric, 4) AS score
  FROM weave_fuse_search('docs_weave', ARRAY['postgresql replication'::wquery],
                         ARRAY['[1,0,0,0]'::wvec], '{0.5,0.5}', 3) s
  JOIN docs d ON d.ctid = s.ctid
 ORDER BY s.score DESC;
--  1 | PostgreSQL streaming replication setup | 0.2582
--  2 | Logical replication with publications  | 0.1100
--  8 | Monitoring replica lag in PostgreSQL   | 0.0736

SELECT bool_and(ok) AS all_invariants_hold FROM weave_check('docs_weave');   -- t
```

Things the example does not show:

- **Vector order is approximate.** The scan ranks by 4-bit quantized codes and does not
  rerank against the stored floats (the planned rerank is `doc/PHASES.md` V10). For an
  exact top-k, take a wider index top-k and re-sort it by `emb <-> q` in an outer query.
- **Cosine is refused.** `WITH (metric = 'cosine')` fails with a hint to normalize the
  vectors and use `metric = 'ip'`. L1 is refused as well.
- **An index needs a `wdoc` column**, and holds at most one vector, one `gram_ops` and one
  docvalues column. Docvalues operator classes exist for `int2`, `int4`, `int8`, `float8`,
  `date`, `bool` and `text`.
- **`to_wdoc(text)` only lowercases and splits.** For stemming and stopwords use
  `to_wdoc('english', text)`, or `to_wdoc(tsvector)`. Fuzzy, prefix and regex terms are
  matched literally against whatever tokens the index holds.
- **`weave_fuse_search()` and `weave_search()` are superuser-only by default**, because they
  open the index without a privilege check. `GRANT EXECUTE` them to roles that may read
  the table.
- Index options: `positions`, `trigrams` (speeds up regex and long fuzzy terms), `bits`
  (code width, 2–8, default 4), `metric` (`l2` or `ip`).

## What it is fast at, and where it loses

Every figure below is quoted from the file named next to it. Losses sit next to wins.
Each file states its corpus, its host and the scale it was measured at, and most are
one corpus at one or two scales.

**Lexical, against tsvector + GIN** (`bench/RESULTS_LEXICAL.md`, synthetic corpus,
1M and 4M documents):

- Wins: common-term ranked top-10 is 20.5× (1M) and 29.4× (4M) faster, mid-frequency
  4.5× and 3.9×, index-answered `count(*)` 133× and 144×, prefix `count(*)` 4.9× and 7.1×.
  The index is 1.73–1.79× smaller and builds 1.03–1.46× faster.
- Losses: rare-term ranked top-10 is 1.33× (1M) and 1.67× (4M) slower, 0.04 ms against
  0.03 ms. Against pg_fts, the extension this one was forked from, the build is 1.08×
  slower at 4M.

**Fused hybrid ranking, against RRF over-fetch on the same index**
(`bench/RESULTS_FUSE.md`, BEIR scifact / nfcorpus / fiqa, 384-d MiniLM embeddings):

- Win: nDCG@10 is 1.053× / 1.010× / 1.114× RRF's, and recall against an exhaustive fused
  scan is 1.000.
- Losses: p50 latency is 0.710× / 0.827× / **1.172×** RRF's, so on fiqa the fused query is
  slower than the RRF query it is meant to replace. p99 misses its gate on two of three
  corpora. The fused scan reads every vector code block (1.000×), so it does not do less
  vector work than RRF. On nfcorpus, recall@100 and MRR@10 are slightly below RRF.

**Filters inside the fused scan** (`bench/RESULTS_GATE_SWEEP.md`, the same three corpora):

- Win: with a lexical filter matching 0.1 % of rows, the fused query is 5.7× to 13.8×
  faster than with no filter, at p50 and at p99. Against the same index answering in vector
  order and rechecking the filter afterwards, it reads 97–158× fewer buffers and discards
  no candidates.
- Loss: with no filter the fused path is about 30 % more expensive (0.7–0.8×). The
  crossover is between 10 % and 1 % selectivity.
- A scalar facet (`price < x`) cuts vector work the same way: 517× (fiqa) and 648×
  (scifact) fewer code blocks at 0.001 selectivity than an executor filter
  (`bench/RESULTS_DOCVALS_PRIZE.md`), and the fall reproduces at 1M rows
  (`bench/RESULTS_DOCVALS_SCALE.md`). These are work counters, not latencies.

**Vector storage** (`bench/RESULTS_VECMAJOR.md`, built index, 4-bit codes): 278 / 533 /
533 / 789 bytes per vector at 384 / 768 / 960 / 1,024 dimensions. That is 0.136× / 0.130× /
0.065× / 0.096× the size of a pgvector HNSW index built in the same run.

**Vector recall and latency, not yet a win:**

- The index orders by quantized codes and does not rerank against the stored floats. The
  exact rerank that reaches recall@10 0.9920 at 1M × 960-d was measured as a component
  (`bench/RESULTS_PHASE_V_COLD.md`) but is not built into the scan (`doc/PHASES.md` V10).
  The codes alone top out at recall@10 0.9225 (GloVe-200d) and 0.8680 (GIST-960d)
  (`bench/RESULTS_BITWIDTH_SWEEP.md`).
- No end-to-end vector query has been timed against pgvector. A standalone scan harness
  measured 1.51× pgvector HNSW's warm p50 at matched recall (`bench/RESULTS_CODE_SCAN.md`);
  that is not a SQL query, so it is not a comparison you should rely on.
- The per-block score bound prunes 0.00 % of blocks on real corpora
  (`bench/RESULTS_CODE_SCAN.md`): every query scores every code.

**Fuzzy, regex, substring** (`bench/RESULTS_FUZZY_REGEX.md`, `bench/RESULTS_CGRAM.md`,
synthetic 1M rows, one scale):

- `term~1` is 93 ms p50, inside its 200 ms gate. `term~2` is 293 ms and misses it.
- A character-class regex is 50 ms with the default index and 1.1 ms with
  `WITH (trigrams = on)`. A regex matching every row is 317 ms.
- Substring search with `gram_ops` makes the index 1.67× the size of `pg_trgm`'s GIN index
  on the same column, and it is 2.0–2.4× slower than `pg_trgm` on five of six patterns
  (faster on one). Two unselective patterns are slower than a sequential scan. Without
  `gram_ops` the index is 0.54× `pg_trgm`'s size but cannot answer `LIKE '%...%'`.

**Permanent trade-offs** (`doc/ARCHITECTURE.md` §8): no parallel ranked scan; positions
cost storage roughly in proportion to token count; exact recall, low latency and small
storage cannot all be had at once; and one extension carrying seven channel kinds is harder
to operate and to trust than pgvector.

## Read next

| file | what |
|---|---|
| `doc/ARCHITECTURE.md` | the design, the vocabulary, the four claims (§9) and the losses (§8) |
| `doc/PHASES.md` | every task, its spec and its gate |
| `doc/GAPS.md` | known defects and measured shortfalls |
| `doc/PRODUCTION_READINESS.md` | what has to be true before you should use it |
| `doc/specs/` | `FUSED_TOPK.md`, `VECTOR_CHANNEL.md`, `FUZZY_CHANNEL.md`, `SEGMENT_FORMAT.md` |
| `doc/CONVENTIONS.md`, `doc/TESTING.md` | how the code is written and tested |
| `AGENTS.md` | orientation for contributors, including the hard rules |

## License

PostgreSQL License. See `LICENSE`. Provenance per file is in `doc/LICENSING.md`.
