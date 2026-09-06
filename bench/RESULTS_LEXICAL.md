# Result: lexical channel vs tsvector + GIN

Date: 2026-09-06. Harness: `bench/lexical.sh` via `bench/aws/run.sh r6id.4xlarge lexical`.
Reproduce: `NDOCS=1000000 VOCAB=200000 bench/aws/run.sh r6id.4xlarge lexical`.

tsvector + GIN is the baseline every PostgreSQL user already has. Beating it is
the minimum bar for anyone to install anything, and it is the only comparison
that is always reproducible without third-party packages.

## Setup

| | |
|---|---|
| instance | `r6id.4xlarge`, 16 vCPU, 123 GiB, local NVMe |
| CPU | Intel Xeon Platinum 8375C @ 2.90 GHz |
| PostgreSQL | 17 from PGDG, `shared_buffers` 40 % of RAM, `maintenance_work_mem` 2 GB, `jit=off` |
| corpus | 1,000,000 docs, 200,000-term Zipf-ish vocabulary, avg 126 bytes, heap 1076 MB |
| df bands | rare = 25 docs, mid = 2,503 docs, common = 197,552 docs |
| method | warm = all reps in one session, first dropped, p50/p99. cold = first scan in a fresh backend, median of 5. |
| correctness | every match count verified equal across pg_weave, GIN, and a seq-scan reference **before** any timing |

Both engines index a **stored, pre-analyzed column** (`wdoc` and `tsvector`), not
an expression. Profiling in pg_fts showed an expression index puts 40–88 % of
measured latency into re-analysis inside the `ORDER BY`, which measures the
analyzer rather than the index.

## Latency, milliseconds

`par4` = `max_parallel_workers_per_gather = 4` (the default shape on any modern
install). `par0` = serial.

| query | weave p50 | weave p99 | GIN p50 par4 | GIN p50 par0 | verdict |
|---|---:|---:|---:|---:|---|
| ranked rare k=10 (df 25) | 0.05 | 0.05 | 0.03 | 0.03 | **behind 1.7×** |
| ranked rare k=100 | 0.05 | 0.07 | 0.03 | 0.03 | **behind 1.7×** |
| ranked mid k=10 (df 2.5k) | 3.54 | 3.56 | 2.06 | 2.09 | **behind 1.7×** |
| ranked mid k=100 | 3.56 | 3.57 | 2.08 | 2.06 | **behind 1.7×** |
| ranked common k=10 (df 198k) | **15.01** | 15.09 | 123.15 | 284.60 | ahead 8.2× / 19× |
| ranked common k=100 | **15.01** | 15.13 | 123.68 | 282.49 | ahead 8.2× / 19× |
| `count(*)` common | **0.43** | 0.43 | 255.65 | 254.53 | **ahead 595×** |
| `count(*)` AND (rare ∧ mid) | 0.04 | 0.04 | 0.02 | 0.02 | behind 2× (both trivial) |
| `count(*)` prefix | **40.7** | 40.9 | 156.28 | 313.88 | ahead 3.8× / 7.7× |
| **bare `ORDER BY` rare** | 83.1 | 83.4 | 80.4 | 352.6 | **see §Footgun** |

pg_weave's index-scan latencies are identical at par4 and par0 — the scan is
serial either way. GIN's common-term ranked latency **doubles** when parallelism
is removed (123 → 285 ms) because it loses parallel heap access, which is why the
pg_weave win widens from 8× to 19×.

## Size and build

| engine | build | index size |
|---|---:|---:|
| pg_weave | 10.3–11.7 s | 115–156 MB |
| tsvector + GIN | 8.3–10.1 s | 68–81 MB |

**Behind on both.** Index size is 1.7–1.9× GIN's. The range within pg_weave is
itself a finding: the harness calls `weave_merge()` and `weave_vacuum()` with
`|| true`, so a run where they did not complete leaves the index uncompacted at
156 MB versus 115 MB compacted. A 35 % size swing depending on whether an
optional maintenance step ran is not acceptable for a published number, and the
harness must stop tolerating it.

## The footgun

`ORDER BY d <=> query LIMIT k` **with no `WHERE` clause does not use the index.**
The planner emits a Seq Scan plus a top-N Sort that evaluates `<=>` on every row:
83 ms with 4 parallel workers, 362 ms serial, and **flat across every selectivity
band** because 25 matching documents and 197,552 matching documents cost exactly
the same when neither consults the index.

The supported form is:

```sql
SELECT id FROM docs
 WHERE d @@@ 'term'::wquery          -- required, or no index path is generated
 ORDER BY d <=> 'term'::wquery
 LIMIT 10;
```

which is **0.05 ms** — a 7,000× difference from the bare form on the same data
for the same intent.

This is the worst failure mode a database feature can have: correct results, a
four-orders-of-magnitude cliff, and no diagnostic. It is also the form every user
will write first, because `ORDER BY embedding <=> $1 LIMIT 10` is what pgvector
taught them. It is documented in pg_weave's own regression test
(`sql/weave.sql:1443`) as a known constraint rather than treated as a bug, which
is the wrong call. It is now task **L7** in `doc/PHASES.md` and the highest
priority in `doc/GAPS.md`.

## Features GIN does not have

Not a latency claim, but the reason to consider pg_weave at all: true BM25/BM25F
with index-maintained corpus statistics, `count(*)` pushdown via a custom scan,
positional phrase and `NEAR`, prefix, fuzzy `~k`, and regex — all over one `@@@`
/ `<=>` surface. `ts_rank` is not BM25 and needs the `tsvector` re-read from the
heap for every match, which is what the 255 ms `count(*)` and 123–285 ms ranked
common-term numbers are made of.

## Four bugs this benchmark found in itself

Recorded because each one produced a plausible, wrong number, and because the
sequence is a decent argument for why a benchmark needs to capture plans:

1. **One psql invocation per repetition**, making every measurement a first scan
   in a fresh backend. Produced a flat ~87 ms across all bands.
2. **`word_00042` tokens.** pg_weave's analyzer splits on non-word bytes, so the
   token became two terms and every "single rare term" query was silently a
   two-term boolean AND. Visible only in the plan's Sort Key.
3. **`percent_rank` band selection**, which on a heavy-tailed vocabulary can
   select nothing — leaving the rare band empty so the query matched 0 rows and
   the competitor "won" at 0.01 ms.
4. **The bare `ORDER BY` form**, which measured a seq scan for six consecutive
   runs while looking like an index benchmark.

None of these were visible from the timings alone. All four were found by making
the harness assert correctness first and print `EXPLAIN` plans.

## What this does not tell us

- **One corpus shape.** A synthetic Zipf-ish vocabulary with 126-byte documents.
  Real prose has different term-length and co-occurrence structure; the
  2.19M-article Wikipedia run (task P3) is still owed and is the number that
  should be published.
- **No comparison against pg_search, pg_textsearch, or VectorChord.** GIN is the
  floor, not the competition. pg_search measured 2.27 ms on common-term ranked
  where pg_fts took 74.7 ms on a 2.19M corpus; our 15.01 ms here is on a
  different corpus and is not comparable to that figure.
- **Single-stream latency only.** No concurrency, no mixed read/write, no
  throughput-under-load.
- **The 1.7× losses on rare and mid are small in absolute terms** (0.02 ms and
  1.5 ms) but they are losses, and `doc/GAPS.md` treats them as such.
