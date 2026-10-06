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
It needs no `shared_preload_libraries` entry.

TODO(with_llvm): result of the plain-install leg goes here.

## Example

TODO: paste from bench/aws/out/<run>/remote/readme_pg17.out.

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
