# Result: lexical channel vs tsvector + GIN

Date: 2026-09-07, re-measured after tasks L7, L8, L10. Harness: `bench/lexical.sh` via `bench/aws/run.sh r6id.4xlarge lexical`.
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

Compaction does not change ranked latency (rare 0.05, mid 3.54, common 15.08 both
before and after), which **falsifies the G3/G4 hypothesis** that the 1.7× loss on
rare and mid terms is per-segment setup cost: the build already produces a single
segment, so there was never per-segment overhead to remove. The fixed cost is
somewhere else and task L9 still owes a profile.
| ranked common k=10 (df 198k) | **15.01** | 15.09 | 123.15 | 284.60 | ahead 8.2× / 19× |
| ranked common k=100 | **15.01** | 15.13 | 123.68 | 282.49 | ahead 8.2× / 19× |
| `count(*)` common | **0.43** | 0.43 | 255.65 | 254.53 | **ahead 595×** |
| `count(*)` AND (rare ∧ mid) | 0.04 | 0.04 | 0.02 | 0.02 | behind 2× (both trivial) |
| `count(*)` prefix | **40.7** | 40.9 | 156.28 | 313.88 | ahead 3.8× / 7.7× |
| bare `ORDER BY` rare (no `WHERE`) | **0.05** | 0.06 | 80.75 | 354.00 | ahead **1,615× / 7,080×** |
| bare `ORDER BY` common (no `WHERE`) | **15.04** | 15.07 | 78.04 | 343.35 | ahead **5.2× / 22.8×** |

pg_weave's index-scan latencies are identical at par4 and par0 — the scan is
serial either way. GIN's common-term ranked latency **doubles** when parallelism
is removed (123 → 285 ms) because it loses parallel heap access, which is why the
pg_weave win widens from 8× to 19×.

## Size and build

| engine | build | index size as built |
|---|---:|---:|
| pg_weave | 29.2 s | **46 MB** |
| tsvector + GIN | 11.7 s | 81 MB |

**Ahead on size by 1.76×, behind on build by 2.5×.** Both numbers changed
direction in this run and both are consequences of task L8.

### Index size: a reported loss that was a measurement artifact

Earlier runs reported pg_weave at 115–156 MB against GIN's 68–81 MB and recorded
it as a 1.7–1.9× loss. `weave_index_size_detail()` (task L10) showed what those
bytes were, on a freshly built index before any maintenance:

| kind | pages | size | % of index | free |
|---|---:|---:|---:|---:|
| **freed** | 14,104 | **110 MB** | **70.7 %** | 0.4 % |
| postings | 4,769 | 37 MB | 23.9 % | 1.2 % |
| dictionary | 785 | 6.3 MB | 3.9 % | 0.1 % |
| doclen_sidecar | 290 | 2.3 MB | 1.5 % | 2.2 % |
| dict_index | 3 | 24 kB | 0.0 % | 22.9 % |
| meta | 1 | 8 kB | 0.0 % | 11.5 % |

70.7 % of the file was freed pages awaiting truncation. Live content was 46 MB
all along — **1.76× smaller than GIN**, not 1.9× larger. The cause was that the
end-of-build merge writes output before freeing inputs, leaving live data at the
high end of the file and free space at the low end, so the tail truncation
`ambuild` ran reclaimed nothing. L8 runs the full vacate+pack+truncate at the end
of the build instead. After it: **as-built 46 MB, compacted 46 MB, swing 0.0 %**,
and both `weave_merge()` and `weave_vacuum()` return false because there is
nothing left to do.

The live breakdown is unchanged by compaction, which confirms vacuum only
truncates and does not reorganize.

### Build time: L8's price, and it is a real regression

Build went **12.6 s → 29.2 s** against GIN's 11.7 s, so the build gap widened from
1.2× to **2.5×**. The vacate phase rewrites the whole index extend-only before
truncating, roughly doubling build write I/O.

This is a genuine trade, recorded as such: a search index is built once and
queried many times, and 46 MB with no required follow-up step beats 156 MB with
one. But it is a loss on a dimension that is supposed to be a win, and the fix is
known rather than hypothetical — bias the *end-of-build* merge's page allocation
toward the low free region so the output is front-packed and a plain tail
truncation suffices, removing the relocation pass entirely. At end of build the
freed inputs are ~70 % of the file and the live output ~30 %, so the low free
region is larger than the live segment and a single low-biased rewrite is
sufficient; `weave_vacuum_compact`'s two-phase dance exists for the harder general
case where it is not. New task **L12**.

## The footgun — FIXED by task L7

**Status: fixed.** `amoptionalkey` was `false`, which made PostgreSQL refuse to
generate any index path without a restriction clause on the first column. Setting
it true was the whole fix; everything else the AM needed was already in place.
Before and after, same corpus and host:

| | before L7 | after L7 | change |
|---|---:|---:|---|
| bare `ORDER BY` rare, par4 | 83.09 ms | **0.05 ms** | **1,662× faster** |
| bare `ORDER BY` rare, serial | 362.38 ms | **0.05 ms** | **7,248× faster** |
| bare `ORDER BY` common, par4 | 79.18 ms | **15.04 ms** | 5.3× faster |
| bare `ORDER BY` common, serial | 347.06 ms | **15.06 ms** | 23× faster |

No other measurement moved: rare 0.05, mid 3.42, common 15.00, `count(*)` 0.42 are
all unchanged, so the flag bought the bare form without costing the qualified one.
`[NO-INDEX]` no longer appears in any plan shape.

GIN cannot use its index for the bare form either — `ts_rank` with no `WHERE`
clause is a seq scan for the same reason — so pg_weave now beats it there by
**1,615× parallel and 7,080× serial**, which turns the project's worst liability
into its largest single margin.

The rest of this section is retained as the description of what was wrong, because
the diagnosis is more useful than the fix.

`ORDER BY d <=> query LIMIT k` **with no `WHERE` clause did not use the index.**
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
taught them. It was documented in pg_weave's own regression test as a known constraint rather
than treated as a bug, which was the wrong call. `sql/orderby.sql` now asserts the
plan for both forms, because a test that only checks rows passes just as happily
on the seq-scan path — which is exactly how the suite missed this.

One hazard the fix introduces, and worth knowing about: an unqualified `count(*)`
needs no columns, so `check_index_only()` succeeds regardless of `amcanreturn` and
the planner will consider an Index Only Scan over the weave index. That plan is
*wrong*, not slow — the index skips NULL values, so a full scan undercounts. It is
priced at 1e12 by `weave_costestimate` and rejected at runtime by
`weave_gettuple`/`weave_getbitmap`, the latter because PostgreSQL 18 compares
disabled-node counts before costs and so a prohibitive cost alone would not stop a
GUC-forced plan.

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
   runs while looking like an index benchmark — and which turned out to be a real
   product bug, now fixed as task L7.

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
  1.3 ms) but they are losses, and `doc/GAPS.md` treats them as such (G3, G4).
- **Index size measured 156 MB in this run**, the uncompacted figure, because
  `weave_merge`/`weave_vacuum` are still invoked with `|| true`. Gap G6 is open and
  the harness still tolerates what it should fail on.
