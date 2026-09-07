# Competitive benchmark strategy

Written 2026-09-07. This supersedes the ad-hoc harnesses in `bench/`.

The design is derived from the recorded failures of three predecessor projects —
`pg_fts`, `pg_turbovec`, `pg_tre` — which between them produced **more than
twenty** benchmark bugs, retractions, and corrections. Every rule below exists
because one of those cost real time or published a wrong number. The failures are
cited inline so nobody removes a rule without knowing what it prevents.

## 0. What we are trying to prove, and what would disprove it

pg_weave's claim is that **one index** beats the **sum of specialised
extensions** — not that it beats each on its own turf by a margin. The
falsifiable form:

> On a single corpus, in a single index, pg_weave answers BM25 ranked retrieval,
> vector ANN, fuzzy/regex, and their fusion at latency and index size no worse
> than the best specialised extension for each, with equal or better correctness.

Any single axis where a specialist wins and pg_weave cannot close the gap is a
result to publish, not to hide. `pg_turbovec/docs/PARITY_GAPS.md` retracted its own
headline claim when a controlled re-run overturned it; that is the standard.

## 1. Competitor set

Engines conflict at the catalog level and **cannot be co-installed**: pg_search,
pg_textsearch, and VectorChord-bm25 all define an access method named `bm25`
(`pg_fts/bench/RESULTS_4WAY_2026-07-29.md:22-25`), and pg_tre and pg_trgm both
define the `%` operator. Therefore **one EC2 instance per engine**, always.

### Lexical / BM25

| engine | index | query form |
|---|---|---|
| **pg_weave** | `USING weave (d)` | `WHERE d @@@ q ORDER BY d <=> q LIMIT k` |
| pg_fts | `USING fts (d)` | `WHERE d @@@ q ORDER BY d <=> q LIMIT k` |
| Timescale pg_textsearch | `USING bm25 (content) WITH (text_config='english')` | `ORDER BY content <@> 'T'::bm25query LIMIT k` |
| ParadeDB pg_search | `USING bm25 (id, content) WITH (key_field='id')` | `WHERE content @@@ 'T' ORDER BY paradedb.score(id) DESC LIMIT k` |
| VectorChord-bm25 | `USING bm25 (tv bm25_ops)` | `SET "bm25.limit"=k;` then `ORDER BY tv <&> to_bm25query(to_tsvector('english','T'), 'idx')` |
| tsvector + GIN | `USING gin (tsv)` | `WHERE tsv @@ q ORDER BY ts_rank_cd(...) DESC LIMIT k` |

`to_bm25query` takes **(tsvector, regclass)** — the reverse of the obvious guess,
and getting it wrong produced a 0.083 ms "win" that was an erroring query
(`pg_fts/bench/data_5way/correctness.txt`).

### Vector ANN

| engine | index | sweep axis |
|---|---|---|
| **pg_weave** | `USING weave (v vec_cosine_ops)` | `weave.vec_oversample`, `vec_recall` |
| pgvector HNSW | `USING hnsw (v vector_cosine_ops)` | `hnsw.ef_search` |
| pgvector IVFFlat | `USING ivfflat (v vector_cosine_ops) WITH (lists=N)` | `ivfflat.probes` |
| VectorChord | `USING vchordrq (v vector_cosine_ops)` | `vchordrq.probes` × `epsilon` |
| pgvectorscale | `USING diskann (v vector_cosine_ops)` | `diskann.query_search_list_size` × `query_rescore` |

### Fuzzy / substring / regex

| engine | index | note |
|---|---|---|
| **pg_weave** | `USING weave (t gram_ops)` | opt-in corpus trigrams |
| pg_trgm GIN | `USING gin (t gin_trgm_ops)` | |
| pg_trgm GiST | `USING gist (t gist_trgm_ops)` | **never benchmarked by pg_tre** — a real gap |
| pg_bigm | `USING gin (t bigm_ops)` | never benchmarked before |
| pg_tre | `USING tre (t)` | |
| seq scan + `levenshtein()` | — | correctness oracle for fuzzy |

### Hybrid

| stack | fusion |
|---|---|
| **pg_weave** | fused-threshold top-k, one index |
| pg_search + pgvector | application-side or server-side RRF, two indexes |
| pg_textsearch + pgvector | RRF |

Excluded, with reasons: **ZomboDB** (archived 2025, requires external
Elasticsearch), **rum** (not BM25), **pg_tokenizer** (not an AM), **Qdrant /
Milvus / Weaviate / Pinecone** (not PostgreSQL — a different product category;
turbopuffer is addressed separately in `doc/COMPETITIVE.md` and is deliberately
not a target, see §0 there).

## 2. The corpus, and the rule that broke every previous run

**One generated `content` column, byte-identical on every host, verified by
checksum before any index is built.**

pg_fts's 5-way had to be retracted because pg_weave and VectorChord indexed
`title||body` while pg_search and pg_textsearch indexed `body` alone — a 48 %
difference in postings scanned for the common term, discovered only when the match
counts disagreed (`RESULTS_5WAY_158:93-128`, commit `81532f4`, recorded by the
author as "my error"). The correction's *explanation* was then also wrong and
needed a second correction.

So the harness:

1. Builds the corpus **once**, on a builder host, into a compressed TSV.
2. Records `sha256` of that artifact.
3. Every engine host downloads it and **asserts the checksum matches** before
   proceeding. Mismatch aborts the run.
4. Every engine indexes the **same single `content` column**, materialised
   identically, and the harness records `md5(string_agg(content))` per host and
   asserts equality across hosts in the analyzer.

Corpora:

| id | source | rows | for |
|---|---|---|---|
| `wiki-2m` | wikimedia/wikipedia 20231101.en, first 2,188,038 | 2.19 M | lexical; comparable to all prior pg_fts runs |
| `msmarco-1m` | MS MARCO passage v1 + qrels | 1 M | **relevance (nDCG@10)** — never once measured by any predecessor |
| `beir-subset` | BEIR: nfcorpus, scifact, fiqa | ~50 k each | relevance generalisation |
| `cohere-1m` | Cohere wiki-en embeddings, 1024-d | 1 M | vector; the corpus pg_turbovec's 490× loss was measured on |
| `gist-1m` | ann-benchmarks GIST-1M HDF5 + neighbors | 1 M | vector with published ground truth |
| `glove-100` | ann-benchmarks GloVe-100 | 1.18 M | vector worst case for the Beta codebook assumption |
| `logs-10m` | synthetic log lines, planted patterns | 10 M | fuzzy/regex at scale, and the temp-disk wall |

**Document length is a corpus dimension, not a detail.** `synth-2m` averages 11.6
words per document and measured a genuine 1.56× codec optimization at 1.06×,
because at that length almost every term frequency is 1, the tf column packs at one
bit, and the per-bit loop the optimization removed had one iteration. Wikipedia's
avgdl is 485. A short-document corpus systematically understates **every**
per-posting decode optimization, which is the entire class of work aimed at the
largest competitive gap — see `bench/RESULTS_PORT_1_5_10.md`. Use `synth-2m-long`
(~120 words) or `wiki-2m` for that class, and never `synth-2m`.

Query terms are **selected from the built corpus by measured document frequency**,
never hardcoded. pg_weave's own earlier harness silently benchmarked an empty term
because a `percent_rank` window selected nothing; and pg_tre's realistic generator
exists only because the first corpus produced "pathologically non-selective rows".

## 3. Correctness gates — before any timing

**No latency number is recorded until its correctness gate passes.** This is the
single most important rule in the document and it has been violated by two of the
three predecessors.

- pg_turbovec published "we win 2.3× on warm p50". A pre-AVX2 scalar-fallback bug
  was returning results **fast and wrong**. Corrected value: **490× slower**
  (`docs/PARITY_GAPS.md:12-31`).
- pg_turbovec also recorded a full latency frontier for a configuration whose
  **recall@10 was 0**, initially mis-diagnosed as data degeneracy.

Gates:

1. **Vector: recall must be > 0**, and recorded, before any latency point is kept.
   A configuration with recall 0 is not a fast configuration.
2. **Vector: recall@10 against a brute-force oracle** over ≥1000 held-out queries.
   Oracle computed by exact distance in-database *and* cross-checked with an
   offline BLAS computation; the two must agree.
3. **Lexical: top-k score parity against a seq-scan BM25 oracle**, per engine —
   not just row counts. pg_fts admits "pg_fts is the only engine whose ranked
   output was verified exact; the others were checked only for expected row
   counts" (`RESULTS_5WAY_158:158-161`). A fast-but-wrong engine must be flagged,
   including ours.
4. **Lexical: per-engine match counts reported in the same table as latency,
   always.** Analyzer parity with Tantivy is unachievable — it does not stem, so
   `year` matches 501,231 documents there and 733,960 under Snowball
   (`RESULTS_5WAY_159:22-62`). A common-term latency quoted without its match count
   is meaningless, and this is a permanent condition, not a bug to fix.
5. **Fuzzy: symmetric `EXCEPT` against a seq-scan reference**, both directions,
   must total zero. pg_tre did this and it is the reason its correctness claims
   hold; keep it.
6. **`EXPLAIN` must show the index is used**, asserted mechanically per query. Two
   predecessors published seq-scan numbers as index numbers: pg_fts's "24 s ranked
   latency" was a missing `WHERE` clause with `amoptionalkey=false`, and pg_weave's
   own first six benchmark runs measured a seq scan for exactly the same reason
   (fixed as task L7).
7. **One pass with default GUCs, always.** pg_turbovec carried a 450× latency tax
   for ~20 releases because every benchmark set tuning GUCs and no run ever
   exercised the defaults.

## 4. Statistical validity

The predecessors' rigor *regressed* over time: pg_fts's August runs reported
p50/p95/p99 + IQR + bootstrap CI over N=200, and the September runs reported bare
medians of 5. Restore and fix the floor.

Per measurement point:

- **N ≥ 200 timed samples**, after **≥ 10 discarded warmup samples**. Engines have
  different steady-state ramps; a fixed warmup count applied uniformly is the only
  defensible choice.
- Report **p50, p90, p95, p99, IQR, min, max, N**, and a **bootstrap 95 % CI on
  the median** (10,000 resamples). A point estimate with no dispersion is not a
  measurement.
- **No outlier rejection.** Report the distribution. p99 is a deliverable, not
  noise — a p50 win with a p99 loss is a regression for anyone with an SLO.
- **Do not compare medians across runs.** The disjoint-CI test compares engines
  measured on the same host in the same run; applying it to a before/after pair
  from two different runs is outside what it is designed for. A change's effect
  must be measured by running both arms in one run, or by accepting that the
  comparison carries un-quantified between-run variance and saying so.
- **Host-variance quantification.** Run the pg_weave arm on **two independent
  instances of the same type**, and report the between-host delta. If that delta
  exceeds the smallest cross-engine difference being claimed, the claim is not
  supported. No predecessor did this, and pg_tre saw a "+18 % regression" that was
  purely host noise (`RESULTS-v2.0-ab.md:73-75`).
- **Raw timing series are the deliverable.** The analyzer computes every statistic
  centrally from the raw series. Per-engine agents report samples, never medians:
  a pg_fts sub-agent reported 6.86 ms where hand measurement gave 2.13 ms, a 3.2×
  error that flattered pg_fts (`RESULTS_5WAY_159b:15-31`).
- **Two clock sources.** Single-stream latency from server-side
  `EXPLAIN (ANALYZE)` `Execution Time` (excludes network and client parsing);
  throughput and concurrent latency from the **driver host** over the network
  (includes them, which is what an application experiences). Report both and never
  mix them in one table.

## 5. Topology: parallel, with dedicated drivers

Predecessors ran the driver on the database host, so `pgbench` competed with the
server for the same 16 vCPUs — which silently caps measured throughput.

```
                    ┌──────────────┐
                    │ coordinator  │  workstation: launch, collect, analyze
                    └──────┬───────┘
        ┌──────────────────┼──────────────────┐
        │                  │                  │
   ┌────▼─────┐      ┌─────▼────┐       ┌─────▼────┐
   │ builder  │      │ engine   │  ×N   │ driver   │  ×M
   │ corpus   │─────▶│ hosts    │◀─────▶│ hosts    │
   │ + oracle │ s3   │ 1 per    │  net  │ pgbench  │
   └──────────┘      │ engine   │       │ + custom │
                     └──────────┘       └──────────┘
```

- **Builder** (`c7i.8xlarge`): builds each corpus once, computes the brute-force
  ANN ground truth and the BM25 oracle, uploads to S3 with checksums, terminates.
  Doing this once instead of per-engine removes the largest source of
  cross-host divergence.
- **Engine hosts**, one per engine, launched **in parallel**:
  `r6id.4xlarge` (16 vCPU, 128 GB, 884 GB local NVMe) for lexical and fuzzy —
  matching every prior pg_fts run so numbers stay comparable;
  `i4i.8xlarge` (32 vCPU, 256 GB, 2×3.75 TB NVMe) for vector — matching
  pg_turbovec's runs. PGDATA and temp tablespace on instance-store NVMe, never
  EBS.
- **Driver hosts**, `c7i.4xlarge`, same AZ and placement group as their engine
  host to keep network latency out of the comparison. One driver per engine for
  concurrency sweeps.
- **Cost is not a constraint** on the `bene` account, so parallelism is bounded by
  API limits and correctness, not spend. Every resource is tagged
  `Project=pg_weave,Run=<id>` and the orchestrator terminates on **every** exit
  path, then verifies termination rather than assuming the API call worked.

## 6. Measurement matrix

### Lexical, per engine

Bands chosen by measured df: rare (~10⁴), mid (~2.5×10⁴), common (~7×10⁵).

Ranked k=10 / k=100 per band · `count(*)` · AND · OR-2 · OR-3 · phrase · prefix ·
fuzzy · regex · build time · index size (as built, no manual maintenance) · peak
build RSS · per-band match count · top-k parity vs oracle.

### Vector, per engine

**Recall/latency frontier**, swept — not a single point. For each engine, sweep its
tuning axis over ≥6 values, compute recall@10 and latency at each, and compare
engines **at matched recall** (0.90, 0.95, 0.99, and each engine's maximum). A
single-point vector comparison is uninterpretable; pg_turbovec's frontier sweeps
are the one piece of its methodology that was unambiguously right.

Also: index size at matched recall, build time, and `recall=exact` cost.

### Fuzzy

Exact substring at 5 % / 1 % / 0.1 % selectivity · `LIKE '%…%'` · fuzzy k=1 ·
**fuzzy k=2** · character-class regex `E-[0-9]{4}` · anchored present/absent ·
**edit-distance KNN `<@>` ORDER BY** · build time · **build temp disk** ·
index size after churn.

k=2, the char-class regex, and the `<@>` KNN exist only in pg_tre's *legacy*
harness and were dropped from its modern matrix; they go back in.

### Hybrid — the axis that decides the project

pg_weave's fused top-k versus a two-index RRF stack, at matched nDCG@10:
latency p50/p99, `score()` call count where observable, and total index size for
the whole capability. This is the comparison no predecessor could run, because none
of them had both modalities in one index.

### Relevance

**nDCG@10, Recall@100, MRR** on MS MARCO and ≥2 BEIR datasets, with qrels.
`pg_fts/bench/ndcg.py` exists and works and was **never once run** — the corpus had
no judgments and the plan to add one was never executed. This is the axis that
decides whether a fusion method is actually better, as opposed to faster.

### Adverse conditions

Cold cache (`drop_caches`) · larger-than-RAM (`shared_buffers` deliberately below
index size — every prior competitive run was fully cached and the storage axis is
untested) · concurrent write + query soak · CIC under churn · SIGKILL mid-build ·
`maintenance_work_mem` starvation · post-churn bloat and what reclaims it.

## 7. Reporting

One `bench/compete/results/<run-id>/` per run containing: raw timing series per
engine per query, the correctness-gate transcripts, the recall frontiers, host
metadata (`lscpu`, `free`, mount layout, every non-default GUC), corpus checksums,
engine versions and commits, and `SUMMARY.md`.

`SUMMARY.md` states, in this order: the setup; the correctness gates and whether
each passed; the matched-recall / matched-nDCG comparison tables with dispersion;
**"where pg_weave loses"** as its own section; and "what this does not measure".

A result whose correctness gate failed is reported as a **failure**, not omitted.
