# Result: lexical channel vs tsvector + GIN vs pg_fts

Date: **2026-09-21**, two scales, `r6id.4xlarge`, pg_weave `8c8d0e3`, pg_fts **v1.8.3**
(`166b0b0`). Harness: `bench/lexical.sh` via `bench/aws/run.sh r6id.4xlarge lexical`.

Reproduce:

```sh
AWS_PROFILE=hotdog AWS_DEFAULT_REGION=... \
  NDOCS=1000000 VOCAB=200000 bench/aws/run.sh r6id.4xlarge lexical
AWS_PROFILE=hotdog AWS_DEFAULT_REGION=... \
  NDOCS=4000000 VOCAB=400000 bench/aws/run.sh r6id.4xlarge lexical
```

The corpus is seeded as of this run (`bench/corpus.sql`, `setseed(0.42)`), so it is now
a function of `(ndocs, vocab)` alone. It was **not** seeded for the 2026-09-07 run, which
is one of two reasons the pg_weave column below cannot be compared cell-by-cell against
that run; see "What changed since 2026-09-07".

## Why there is a third arm now

**pg_weave is a fork of pg_fts, and until this run the two had never been measured
against each other.** Every lexical number this project has published was against
tsvector + GIN, which measures *the fork plus its inheritance* against a PostgreSQL
built-in. It cannot separate an inherited win from an earned one, and Phase L's tasks
(L7, L8, L12, L13, L14, L15, L17, L18) are all claims about the difference.

The two forks present the same surface — `to_ftsdoc(text)` / `ftsdoc @@@ ftsquery` /
`ftsdoc <=> ftsquery` against `to_wdoc(text)` / `wdoc @@@ wquery` / `wdoc <=> wquery` —
and different AM and type names, so both extensions are installed in one database over
**one table**, and the arms differ in nothing but the index. Both analyzed columns are
built through the one-argument entry point, so both forks resolve
`default_text_search_config` identically.

## Setup

| | 1M scale | 4M scale |
|---|---|---|
| instance | `r6id.4xlarge`, 16 vCPU, 123 GiB, local NVMe | same |
| CPU | Intel Xeon Platinum 8375C @ 2.90 GHz, `avx512_vpopcntdq` | same |
| PostgreSQL | 17.11 PGDG, `shared_buffers` 40 % of RAM, `maintenance_work_mem` 2 GB, `jit=off` | same |
| corpus | 1,000,000 docs, 200,000-term Zipf-ish vocabulary | 4,000,000 docs, 400,000-term |
| table | 2,357 MB (id, body, `wdoc`, `tsvector`, `ftsdoc`, PK) | 9,238 MB |
| df bands | rare 25, mid 2,499, common 179,772 | rare 25, mid 2,498, common 580,077 |
| method | warm = 7 reps in one session, first dropped, p50/p99; cold = first scan in a fresh backend, median of 5 | same |
| correctness | **gated before any timing**: every arm's match count equals a seq-scan reference for rare, mid, common and `zzqrare`, at both scales — 4/4 and 4/4 | same |

All three arms index a **stored, pre-analyzed column**, not an expression. Profiling in
pg_fts showed an expression index puts 40–88 % of measured latency into re-analysis
inside the `ORDER BY`, which measures the analyzer rather than the index.

## Latency, warm p50 / p99 in milliseconds, `max_parallel_workers_per_gather = 4`

### 1M docs

| query | weave p50 | weave p99 | GIN p50 | fts p50 | vs GIN | vs fts |
|---|---:|---:|---:|---:|---|---|
| ranked rare k=10 (df 25) | 0.04 | 0.04 | 0.03 | 0.05 | **behind 1.33×** | ahead 1.25× |
| ranked rare k=100 | 0.05 | 0.07 | 0.03 | 0.06 | **behind 1.67×** | ahead 1.20× |
| ranked mid k=10 (df 2.5k) | **0.50** | 0.52 | 2.27 | 3.49 | ahead 4.5× | **ahead 7.0×** |
| ranked mid k=100 | **0.58** | 0.60 | 2.20 | 3.54 | ahead 3.8× | ahead 6.1× |
| ranked common k=10 (df 180k) | **7.52** | 7.55 | 154.53 | 14.02 | ahead 20.5× | ahead 1.86× |
| ranked common k=100 | **7.58** | 7.59 | 154.98 | 14.06 | ahead 20.4× | ahead 1.85× |
| bare `ORDER BY` rare (no `WHERE`) | **0.04** | 0.04 | 135.03 | 148.19 | ahead 3,376× | **ahead 3,705×** |
| bare `ORDER BY` common | **7.49** | 7.53 | 131.62 | 143.88 | ahead 17.6× | ahead 19.2× |
| `count(*)` common | 0.95 | 0.97 | 125.95 | 0.94 | ahead 133× | **tie** |
| `count(*)` AND (rare ∧ mid) | 0.04 | 0.04 | 0.02 | 0.04 | behind 2× (both trivial) | **tie** |
| `count(*)` prefix | 40.73 | 40.79 | 198.30 | 41.12 | ahead 4.9× | **tie** |

### 4M docs

| query | weave p50 | weave p99 | GIN p50 | fts p50 | vs GIN | vs fts |
|---|---:|---:|---:|---:|---|---|
| ranked rare k=10 (df 25) | 0.05 | 0.06 | 0.03 | 0.06 | **behind 1.67×** | ahead 1.20× |
| ranked rare k=100 | 0.06 | 0.08 | 0.03 | 0.06 | **behind 2.0×** | tie |
| ranked mid k=10 (df 2.5k) | **1.02** | 1.14 | 3.96 | 5.07 | ahead 3.9× | **ahead 5.0×** |
| ranked mid k=100 | **1.17** | 1.46 | 4.32 | 5.26 | ahead 3.7× | ahead 4.5× |
| ranked common k=10 (df 580k) | **19.65** | 19.65 | 578.09 | 31.59 | ahead 29.4× | ahead 1.61× |
| ranked common k=100 | **19.90** | 19.94 | 576.03 | 31.66 | ahead 29.0× | ahead 1.59× |
| bare `ORDER BY` rare | **0.05** | 0.06 | 551.52 | 592.64 | ahead 11,030× | **ahead 11,853×** |
| bare `ORDER BY` common | **19.89** | 19.93 | 540.91 | 585.67 | ahead 27.2× | ahead 29.4× |
| `count(*)` common | 3.81 | 3.82 | 547.24 | 3.81 | ahead 144× | **tie** |
| `count(*)` AND | 0.04 | 0.05 | 0.03 | 0.04 | behind 1.33× (trivial) | **tie** |
| `count(*)` prefix | 134.98 | 135.10 | 955.43 | 136.73 | ahead 7.1× | **tie** |

Serial (`par0`) numbers are in the logs. pg_weave and pg_fts are unchanged by
parallelism — both AMs set `amcanparallel = false`, so a ranked index scan is serial
either way. GIN loses parallel heap access: common-term ranked goes 154.53 → 207.24 ms
at 1M and 578.09 → 809.33 ms at 4M, and the bare `ORDER BY` form goes to **2,343 ms** at
4M, so every pg_weave margin against GIN widens serially.

## Size and build

| | 1M | | | 4M | | |
|---|---:|---:|---:|---:|---:|---:|
| | weave | GIN | fts | weave | GIN | fts |
| index size **as built** | **48 MB** | 83 MB | 169 MB | **173 MB** | 309 MB | 525 MB |
| build time | **10.9 s** | 11.2 s | 10.9 s | 56.8 s | 82.8 s | **52.7 s** |
| segments as built | 1 | — | 1 | 1 | — | 1 |
| G6 swing (as-built vs compacted) | 0.0 % | — | — | 0.0 % | — | — |

- **1.73–1.79× smaller than GIN** and **3.03–3.52× smaller than pg_fts.**
- **1.03–1.46× faster to build than GIN.**
- **Build against pg_fts is a LOSS that only appears at scale**: a tie at 1M (10.9 s
  each) and **1.08× slower at 4M** (56.8 s against 52.7 s). Recorded as a loss per hard
  rule 8. The mechanism is not established here; L8's vacate+pack pass is the obvious
  suspect and L12 removed most of its cost, so this is 4 s on 4M documents rather than
  the 2.5× it once was, but 4 s is not zero and the direction is against us.

## What is earned and what is inherited — the point of the pg_fts arm

| capability | verdict | evidence |
|---|---|---|
| bare `ORDER BY` with no `WHERE` | **EARNED** (task L7) | pg_fts's plan is `Limit[NO-INDEX]` at both scales: it seq-scans and evaluates `<=>` per row. 3,705× at 1M, **11,853× at 4M** |
| mid-band ranked latency | **EARNED** (L17, L14) | 7.0× at 1M, 5.0× at 4M, same corpus, same host, same analyzer |
| common-band ranked latency | **EARNED** | 1.86× at 1M, 1.61× at 4M |
| index size | **EARNED** (L8, L12, L17, L18) | 3.5× / 3.0× smaller than the fork it came from |
| `count(*)` pushdown | **INHERITED** (the mechanism) / **EARNED** (its cost, as of the G38 fix, same day) | 0.95 vs 0.94 at 1M and 3.81 vs 3.81 at 4M — identical, because both forks had the same O(heap-pages) visibility gate (`pg_fts_am_scan.c:4402`). The 133–144× win over GIN is pg_fts's `FtsCount` custom scan renamed. The **fix** to that gate exists only here and is unmeasured against pg_fts on a shared table |
| prefix `count(*)` | **INHERITED** | 40.73 vs 41.12 and 134.98 vs 136.73 — tie at both scales |
| rare-band ranked latency | **NEITHER** | 0.04 vs 0.05 and 0.05 vs 0.06 — one or two ticks of the 0.01 ms reporting resolution |
| build time | **a LOSS at 4M** | 56.8 s against 52.7 s |

This table is the reason the arm was added. Four of the seven capability rows this
project has cited as lexical strengths are **not attributable to pg_weave**, and two of
them — `count(*)` and prefix — are exact ties with upstream at both scales. Any README
or claim that presents the 133× `count(*)` margin as pg_weave's achievement is
misattributing it, and `doc/ARCHITECTURE.md` §9's four claims deliberately do not
include it.

## Phase L gate: NOT MET, and the remaining loss is 10–30 µs

The gate (`doc/PHASES.md`) is *"`bench/lexical.sh` re-run and recorded with **zero
measured losses** against tsvector + GIN on latency, p99, and index size"*.

Losses remaining against GIN, at both scales:

| query | weave | GIN | absolute gap |
|---|---:|---:|---:|
| ranked rare k=10 | 0.04 / 0.05 | 0.03 / 0.03 | 10 µs / 20 µs |
| ranked rare k=100 | 0.05 / 0.06 | 0.03 / 0.03 | 20 µs / 30 µs |
| `count(*)` AND | 0.04 / 0.04 | 0.02 / 0.03 | 20 µs / 10 µs |

Everything else is a win, including the two dimensions this gate used to fail on
outright — **index size is now 1.73–1.79× ahead** where it was recorded as 1.7–1.9×
behind, and **build is 1.03–1.46× ahead** where it was 1.2× behind and then 2.5× behind
after L8.

So the gate is **not met**, and the honest reading of *why* cuts both ways. The
remaining differences are real and reproduce at both scales in the same direction, so
they are not noise. They are also 10–30 µs on queries that complete in under 0.1 ms,
where p50 equals p99 for both arms and the reported precision is 0.01 ms — so nothing
here supports a claim that the loss matters, either. **A gate written as "zero measured
losses" cannot be closed by a measurement this size; it can only be closed by restating
the gate in absolute terms, and that is a maintainer decision, not a benchmark result.**
It is left open rather than quietly reinterpreted.

`doc/GAPS.md` G3 and G4 are the rare-band rows. The mid-band half of those gaps is
**closed by measurement**: the 1.7× loss they describe is now a 3.7–4.5× win.

## Methodology, against the rules this project sets itself

- **Hard rule 8 (correctness before latency).** Match counts for rare, mid, common and
  `zzqrare` were compared against a seq-scan reference for all three arms before any
  timing, at both scales, and every one agreed. The pg_fts arm agreeing on all four is
  also the operational proof that the two forks' analyzers are identical on this
  corpus, which is why the `default_text_search_config` question does not need arguing.
- **Hard rule 10 (re-run an arm against itself).** The `par4` and `par0` passes are two
  independent measurements of the same plan for every index-scan query, since neither
  AM is parallel-capable. They agree to ≤ 0.05 ms: weave mid 1.02/1.07, fts mid
  5.07/5.04, weave common 19.65/19.69, fts common 31.59/31.61, weave `count(*)`
  3.81/3.80 at 4M. Within-arm spread is smaller than every between-arm difference
  claimed above except the rare band, which is exactly why the rare band is written up
  as unresolved rather than as a win or a loss.
- **Hard rule 11 (a second scale).** Every direction in every table reproduces at 4M.
  Magnitudes move as expected: margins against GIN widen with corpus size (20.5× → 29.4×
  on common-term ranked) and margins against pg_fts narrow (7.0× → 5.0× on mid), which
  is a fact worth watching rather than explaining away — the mid-band advantage is a
  constant-factor saving in the doclen cursor (L17), so it dilutes as posting-scan work
  grows.

## What changed since 2026-09-07, and why the old column is not directly comparable

Two confounds, both introduced deliberately and both working *against* the new numbers:

1. **The corpus was unseeded.** The 2026-09-07 run drew different data at the same
   `NDOCS`/`VOCAB`: mid df 2,503 and common df 197,552 against 2,499 and 179,772 here.
   Fixed going forward by `setseed(0.42)`.
2. **The table is 2.2× wider.** It now carries an `ftsdoc` column alongside `wdoc` and
   `tsvector`, so the heap went 1,076 MB → 2,357 MB at the same document count. Every
   arm pays it equally, so between-arm comparisons in this file are unaffected, but a
   ranked scan fetching heap tuples pays more per row than it did in the old run.

Because both confounds make the new measurements *worse*, the improvements below are
lower bounds:

| | 2026-09-07 | 2026-09-21 (1M) | |
|---|---:|---:|---|
| ranked mid k=10 | 3.54 ms | **0.50 ms** | ≥ 7× better (L14, L17) |
| ranked common k=10 | 15.01 ms | **7.52 ms** | ≥ 2× better |
| build | 29.2 s | **10.9 s** | ≥ 2.7× better (L12) |
| index size as built | 46 MB | 48 MB | flat, on a differently-drawn corpus |

One figure moved the **wrong** way, and chasing it the same day found a real defect.
**`count(*)` common was 0.43 ms and measured 0.95 ms here**, on a corpus with *fewer*
matching documents — and pg_fts measured 0.94 ms on the same table, so the two forks
agreed to 0.01 ms and nothing in pg_weave's divergence could explain it.

**Answered, and it was not a regression: `count(*)`'s fast path is O(heap pages), not
O(matching documents)** (`doc/GAPS.md` G38). It proved whole-heap visibility with one
`VM_ALL_VISIBLE()` call per heap block, so all three published figures are three heap
sizes on one line — 0.43 ms at 1,076 MB, 0.95 at 2,357, 3.81 at 9,238 — and a term
matching *nothing* cost the same as one matching 196,785 documents. Below roughly
df 9,000 the fast path was up to **40× slower than the ordinary path it exists to
avoid**.

**The two forks agreeing to 0.01 ms is now explained rather than merely observed:**
pg_fts has the identical gate, `bm25_count_dictdf_fastpath()` with the same per-block
loop at `pg_fts_am_scan.c:4402`. So the *defect* is inherited, and **the fix is not** —
it exists only here, and it is owed to upstream as a bug report.

**Fixed 2026-09-21** (zero-df early-out plus `visibilitymap_count()`), measured at
**0.003–0.005 ms flat across every df** on an 87,486-page heap, a 69–93× improvement on
that heap, with the old per-block scan retained as a `USE_ASSERT_CHECKING` cross-check.

> **THE `count(*)` ROWS IN THE TABLES ABOVE ARE PRE-FIX.** 0.95 ms at 1M and 3.81 ms at
> 4M were measured before G38 was found. They are left as measured rather than edited,
> because a benchmark file records what was run — and the *comparisons* they support are
> unaffected, since pg_fts has the same gate and both arms were measured on the same
> table. **No post-fix ratio against GIN or pg_fts is stated here, because the fix was
> measured on a different corpus and heap** (`/scratch/pg_weave/g38.sh`, 683 MB heap), and
> dividing a number from one run by a number from another is what hard rule 11 forbids.
> The next lexical run produces it.

## What this does not tell us

- **One corpus shape.** A synthetic Zipf-ish vocabulary with short documents. Real prose
  has different term-length and co-occurrence structure; the Wikipedia run is still
  owed and is the number that should be published.
- **Still no comparison against pg_search, pg_textsearch, or VectorChord.** GIN is the
  floor and pg_fts is the provenance; neither is the competition. pg_search measured
  2.27 ms on common-term ranked where pg_fts took 74.7 ms on a 2.19M corpus, and
  nothing in this file is comparable to that figure.
- **Single-stream latency only.** No concurrency, no mixed read/write, no
  throughput-under-load. G28 (back-pressure) and the concurrency harness in `t/007` are
  the unmet parts of that.
- **No nDCG or recall.** This measures latency and size on queries whose answers are
  gated for equality, not ranking quality. `FUSED_TOPK.md` §8's nDCG rows are owed by
  Phase F and are not in scope here.

---

# SUPERSEDED: the 2026-09-07 run

Retained per hard rule 13: a claim this project published and has since improved on or
overturned keeps a marked note at the point of the original claim. The numeric table
below is **superseded** by the run above. The diagnoses are not, and they are the more
useful half.

**Superseded claims, and what replaced them:** "behind 1.7× on rare and mid ranked" —
the mid half is now a 3.7–4.5× win and the rare half is a 10–30 µs difference;
"ahead on size by 1.76×, behind on build by 2.5×" — build is now 1.03–1.46× *ahead* of
GIN after L12; "ahead 595× on `count(*)`" — measured at 133–144× on this corpus, and
now known to be **inherited from pg_fts** rather than earned.

## Index size: a reported loss that was a measurement artifact

Earlier runs reported pg_weave at 115–156 MB against GIN's 68–81 MB and recorded it as a
1.7–1.9× loss. `weave_index_size_detail()` (task L10) showed what those bytes were, on a
freshly built index before any maintenance:

| kind | pages | size | % of index | free |
|---|---:|---:|---:|---:|
| **freed** | 14,104 | **110 MB** | **70.7 %** | 0.4 % |
| postings | 4,769 | 37 MB | 23.9 % | 1.2 % |
| dictionary | 785 | 6.3 MB | 3.9 % | 0.1 % |
| doclen_sidecar | 290 | 2.3 MB | 1.5 % | 2.2 % |
| dict_index | 3 | 24 kB | 0.0 % | 22.9 % |
| meta | 1 | 8 kB | 0.0 % | 11.5 % |

70.7 % of the file was freed pages awaiting truncation. Live content was 46 MB all
along. The cause was that the end-of-build merge writes output before freeing inputs,
leaving live data at the high end of the file and free space at the low end, so the tail
truncation `ambuild` ran reclaimed nothing. L8 runs the full vacate+pack+truncate at the
end of the build instead; L12 then made that pass cheap by allocating from the low free
region. The 0.0 % as-built-vs-compacted swing in both runs above is what those two tasks
bought, and it is why `weave_merge()`/`weave_vacuum()` return false on a fresh build.

## The footgun — FIXED by task L7, and still present upstream

`ORDER BY d <=> query LIMIT k` **with no `WHERE` clause did not use the index.** The
planner emitted a Seq Scan plus a top-N Sort that evaluates `<=>` on every row: 83 ms
with 4 parallel workers, 362 ms serial, and **flat across every selectivity band**
because 25 matching documents and 197,552 matching documents cost exactly the same when
neither consults the index.

This is the worst failure mode a database feature can have: correct results, a
four-orders-of-magnitude cliff, and no diagnostic. It is also the form every user writes
first, because `ORDER BY embedding <=> $1 LIMIT 10` is what pgvector taught them. It was
documented in pg_weave's own regression test as a known constraint rather than treated
as a bug, which was the wrong call. `sql/orderby.sql` now asserts the plan for both
forms, because a test that only checks rows passes just as happily on the seq-scan path
— which is exactly how the suite missed this.

**The 2026-09-21 run adds a fact the original write-up could not have:
`pg_fts v1.8.3` still has it.** Its plan shape is `Limit[NO-INDEX]` at both scales, and
the resulting margin — 3,705× at 1M and 11,853× at 4M — is the single largest earned
number in this file.

One hazard the fix introduces: an unqualified `count(*)` needs no columns, so
`check_index_only()` succeeds regardless of `amcanreturn` and the planner will consider
an Index Only Scan over the weave index. That plan is *wrong*, not slow — the index
skips NULL values, so a full scan undercounts. It is priced at 1e12 by
`weave_costestimate` and rejected at runtime by `weave_gettuple`/`weave_getbitmap`, the
latter because PostgreSQL 18 compares disabled-node counts before costs and so a
prohibitive cost alone would not stop a GUC-forced plan.

## Four bugs this benchmark found in itself

Recorded because each one produced a plausible, wrong number, and because the sequence
is a decent argument for why a benchmark needs to capture plans:

1. **One psql invocation per repetition**, making every measurement a first scan in a
   fresh backend. Produced a flat ~87 ms across all bands.
2. **`word_00042` tokens.** pg_weave's analyzer splits on non-word bytes, so the token
   became two terms and every "single rare term" query was silently a two-term boolean
   AND. Visible only in the plan's Sort Key.
3. **`percent_rank` band selection**, which on a heavy-tailed vocabulary can select
   nothing — leaving the rare band empty so the query matched 0 rows and the competitor
   "won" at 0.01 ms.
4. **The bare `ORDER BY` form**, which measured a seq scan for six consecutive runs
   while looking like an index benchmark — and which turned out to be a real product
   bug, now fixed as task L7.

A fifth belongs to 2026-09-21: **the corpus was never seeded**, so "Reproduce:
`NDOCS=... bench/aws/run.sh ...`" was a reproducibility claim the harness could not
keep, and comparing pg_weave against its own earlier number was confounded for four
benchmark runs. None of the five were visible from the timings alone.
