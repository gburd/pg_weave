# Result: when does scoring the qualifying set beat the gated fused scan? (V17, measurement only)

**`doc/PHASES.md` V17 ("switch plan strategy on predicate selectivity"), measured 2026-10-04.
No production code changed. This is the evidence for or against building the switch.**

**Headline, scale 1 (scifact 5,183 and fiqa 57,600, real MiniLM 384-d):**

- **There is a crossover, and it sits at roughly 1–2 % selectivity, or about 100–800
  qualifying rows.** Above it the shipped gated fused scan (arm A) wins by 2.8–5.5× at 10 %
  and 25–33× with no predicate. Below it, an exact per-row rescore of the qualifying set
  (arm C) wins by 1.3–1.6× at 1 % and 3.7× at 0.1 %. It wins on **every one of 25 queries**
  at every point at or below 1 %, with one exception: the fiqa lexical gate at 1 %, which
  is a tie (0.97×, C faster on 7 of 25 queries).
- **Only C's shape crosses over.** Both materializing arms built from the index's
  corpus-wide SRFs have a floor that the predicate never touches. Arm B (`weave_search(k = N)`
  plus the heap vector) costs 22 ms on fiqa even when 6 rows qualify. Arm D
  (`weave_search` plus `weave_vec_scan(docids)`) costs 25 ms there. Neither ever beats A
  where A is working normally.
- **A cliff in the shipped scan, found by this run:** when the gate admits **fewer rows
  than the LIMIT**, A runs out of ranked rows. It then enters the G56/G71 padding phase,
  which walks the **entire heap** and hands every row to the executor for recheck. The
  same 25 queries jump from 2.14 ms at 58 qualifying rows to **261 ms at 6** on fiqa's
  lexical gate, and from 0.70 to **40 ms** on scifact. The work counters cannot see it:
  every `weave_fuse_stats()`/`weave_work_stats()` field is identical between the two
  cases. See "The padding cliff" below. It is tracked as **G76** on main, so it is not
  written up in `doc/GAPS.md` here.

**Scale 2 (1M synthetic rows): see "Scale 2" below.**

Harness: `bench/v17_crossover.sh` (new), driven on EC2 by `bench/aws/v17_job.sh` through
the `script` job. Raw data: `bench/results/v17/<run>/`.

## The question

zvec switches strategy on predicate selectivity. Below a ratio it materializes the matching
ids and scores them exactly. Above it, it pushes a bitmap into the index. pg_weave has one
strategy, the gated fused scan: the predicate becomes a REQUIRED gate shuttle, and the
bolt loop skips what the gate excludes (`so->plainTids` / `so->nplain`, materialized before
the bolt loop in `weave_fuse_pass()`, `src/am/amscan.c`). `bench/RESULTS_GATE_SWEEP.md`
showed that this one strategy already gets cheaper as the gate tightens: CPU work tracks
selectivity to three digits, and after G27 page traffic does too. V17 asks the next
question. **Is there a selectivity below which materializing beats the gated scan, and by
how much?**

## Method

**Four arms, one objective.** Every arm returns the top 10 rows satisfying the predicate
under `0.5 · bm25 + 0.5 · ip`, with `pg_weave.fuse_normalize = off` on every arm, so the
weights mean the same raw sum everywhere.

| arm | what it is | objective it computes |
|---|---|---|
| **A** | the gated fused scan as it ships: `SELECT id FROM vx WHERE pred ORDER BY fuse(body <=> q, emb <#> v, weights => '{0.5,0.5}') LIMIT 10`, `enable_seqscan = enable_bitmapscan = off` | quantized vector codes, sidecar doclen |
| **B** | the task's materialize-and-rescore: qualifying ids `LEFT JOIN weave_search(index, q, N)` on ctid, plus the exact heap inner product, top 10 | exact vector, sidecar doclen |
| **C** | the same exact objective, with the lexical half scored **per qualifying row** by `weave_bm25(body, q, ndocs, avgdl, dfs)` (statistics fetched once per query, before timing) | exact vector, heap doclen |
| **D** | qualifying ids, `weave_search(k = N)` plus `weave_vec_scan(index, v, n, docids)` restricted to the qualifying docids | quantized codes, sidecar doclen (A's objective) |

C exists because B's lexical cost is a corpus-wide posting walk whatever the predicate is.
B alone would therefore measure `weave_search(k = N)` rather than materialization. C's cost
is proportional to the qualifying set, which is the shape zvec's switch has. D is A's
correctness twin and the closest SQL model of a switch inside the AM, which would score
codes rather than heap vectors.

**Points.** No predicate (100 %), plus two gate kinds at targets of 10, 1, 0.1 and 0.01 %.
The docvals facet is `price < c`, where `price` is a hash **rank** 0..N−1 added to the
table. It admits exactly `c` rows and is uncorrelated with heap order: `corr(price, block)`
is −0.0004 on fiqa and 0.0217 on scifact. The lexical gate is `body @@@ term`, with the term
chosen log-nearest in document frequency, as in `bench/gatesweep.sh`. Selectivity is
measured with a seqscan `count(*)`; quote `rows`/`sel`, not `target`. At scale 1 the two
smallest targets admit fewer than 10 rows (5 and 1 on scifact, 58 and 6 on fiqa). That is
where the padding cliff shows, and why those rows are reported separately below.

**Plan check (fatal if it fails).** A's plan must carry `Order By` and, at gated points, an
`Index Cond`. B, C and D must reach the index for the predicate (`Index Cond` or
`Recheck Cond`) and may use a bitmap. With seqscan allowed, the gated points would measure
a heap scan.

**Correctness, before any latency is believed (hard rule 8).** For each of 225
(point, query) pairs per corpus:
- A must equal D as a top-10 set, unless D has a tie across ranks 10 and 11.
- B must equal C, under the same tie condition.
- |A ∩ B| / |A| is reported.
- **Positive control:** A with `fuse_normalize = on` computes a different objective and must
  disagree with D somewhere. A gate that cannot fail has not been shown to work.

**Latency.** `EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)` Execution Time. Each measurement
takes 6 reps of the statement, drops the first and keeps the median of the other 5, per
query. Over the 25 queries, the table reports p50 and p99 of those per-query medians. Each
(point, arm) is measured in **two slots**, interleaved by query, so drift cannot land on
one arm (hard rule 10). The tables average the two slots, and the "A/A" column is the
larger of A's and C's slot-to-slot p50 difference. Buffers come from one
`EXPLAIN (ANALYZE, BUFFERS)` per (point, arm), on the first query, at the top node.

**Host.** EC2 `c7i.4xlarge` (16 vCPU, 32 GB), us-east-2, Debian 13, PostgreSQL 17.11,
pg_weave 0.27.0. Commit `cfa537a` (harness) on main `89d7dcf`. `shared_buffers` 12.6 GB,
`work_mem` 256 MB, `jit = off`, `max_parallel_workers_per_gather = 0` set in-session.
Smoke (regression, isolation and TAP) green on the same instance before any number was
taken. Run **`pgweave-20261004-231619`**.

## Correctness: what the arms agree on

| corpus | pairs | A ≠ D (untied) | B ≠ C (untied) | mean \|A ∩ B\|/\|A\| | positive control fired |
|---|---|---|---|---|---|
| scifact | 225 | **0** | 15 | 0.9974 | 122 of 225 |
| fiqa | 225 | **0** | 48 | 0.9990 | 173 of 225 |

- **A = D on 450 of 450.** The gated fused scan returns exactly the top 10 of its own
  objective at every selectivity, so every latency below compares arms that answer the same
  question. The control fired, so that agreement carries information. It did not fire at
  the points where every qualifying row is returned, where it cannot.
- **B ≠ C is a finding about `weave_search()`, not a harness bug: it is not exact BM25.**
  It scores with the doclen sidecar's **one quantized byte per document**
  (`src/am/am.c`, "The doclen sidecar (v4) stores one quantized length byte per doc").
  `weave_bm25()` uses the heap document's exact length. Controlled locally: with document
  lengths 2–3 the two agree to 2e-16, and with lengths spread over 50–500 tokens they
  differ by up to **4.5 %** relative. So "exact BM25 via `weave_search`" in the V17 brief
  holds only up to doclen quantization. Arm C is the exact one.
- |A ∩ B| = 0.997–0.999 is the price of A's quantized objective (4-bit codes and quantized
  doclen) against the exact one. A materializing plan that scores heap vectors returns a
  slightly **different and more exact** answer. That is a semantic difference a switch
  would introduce, separate from its speed.

## Scale 1: the crossover

p50 and p99 in ms, averaged over the two slots, 25 queries each. **A/C > 1 means the
per-row rescore is faster.** Buffers are `shared hit + read` at the top node, for query 1.

**scifact (5,183 docs)**

| gate | sel | rows | A p50 | A p99 | C p50 | C p99 | B p50 | D p50 | **A/C** | A/A ms | A buf | C buf |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | 1.000 | 5183 | 2.010 | 2.791 | 45.982 | 50.155 | 8.569 | 18.720 | **0.04** | 0.354 | 507 | 16944 |
| facet | 0.100 | 518 | 1.724 | 1.894 | 4.825 | 5.258 | 3.123 | 4.363 | **0.36** | 0.039 | 524 | 2068 |
| facet | 0.010 | 52 | 0.758 | 0.877 | 0.516 | 0.560 | 2.336 | 2.838 | **1.47** | 0.016 | 310 | 271 |
| lexical | 0.100 | 517 | 1.784 | 1.947 | 5.281 | 5.639 | 2.885 | 4.357 | **0.34** | 0.040 | 511 | 2061 |
| lexical | 0.010 | 52 | 0.702 | 0.798 | 0.434 | 0.479 | 2.322 | 2.813 | **1.62** | 0.011 | 283 | 256 |

**fiqa (57,600 docs)**

| gate | sel | rows | A p50 | A p99 | C p50 | C p99 | B p50 | D p50 | **A/C** | A/A ms | A buf | C buf |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | 1.000 | 57600 | 11.941 | 15.808 | 370.062 | 395.062 | 171.257 | 975.714 | **0.03** | 0.275 | 4492 | 178947 |
| facet | 0.100 | 5760 | 9.319 | 11.183 | 38.852 | 42.952 | 39.541 | 68.179 | **0.24** | 0.696 | 3285 | 21648 |
| facet | 0.010 | 576 | 5.267 | 5.726 | 4.121 | 4.364 | 25.312 | 29.689 | **1.28** | 0.015 | 1928 | 2521 |
| facet | 0.001 | 58 | 2.298 | 2.519 | 0.619 | 0.658 | 23.329 | 26.447 | **3.72** | 0.026 | 1133 | 498 |
| lexical | 0.099 | 5711 | 9.949 | 11.678 | 54.334 | 57.081 | 35.683 | 57.431 | **0.18** | 0.616 | 3444 | 22071 |
| lexical | 0.010 | 576 | 5.207 | 5.769 | 5.380 | 5.684 | 24.136 | 29.031 | **0.97** | 0.032 | 1679 | 2380 |
| lexical | 0.001 | 58 | 2.141 | 2.308 | 0.579 | 0.650 | 22.802 | 25.482 | **3.70** | 0.007 | 909 | 278 |

**Per query** (both slots averaged; C faster than A, out of 25):

| point | scifact facet | scifact lexical | fiqa facet | fiqa lexical |
|---|---|---|---|---|
| 10 % | 0 | 0 | 0 | 0 |
| 1 % | 25 | 25 | 25 | **7** |
| 0.1 % | (5 rows, padding) | (5 rows, padding) | 25 | 25 |

**Where it crosses.** Log-linear interpolation of A/C between the 10 % and 1 % points,
which bracket it on all four series:

| corpus | gate | crossover sel | ≈ qualifying rows |
|---|---|---|---|
| scifact | facet | 1.9 % | ~97 |
| scifact | lexical | 2.0 % | ~106 |
| fiqa | facet | 1.4 % | ~810 |
| fiqa | lexical | 0.95 % | ~550 |

**The margins clear the noise floor by a wide margin (hard rule 10).** The largest
slot-to-slot spread at any 1 % or 0.1 % point is 0.032 ms. The smallest A−C difference at
those points is 0.17 ms, at fiqa's lexical 1 % point. There A is faster by 3 %, about 5×
the spread, but only 18 of 25 queries agree, so the point is recorded as a tie, not a win. At 10 % the spread is 0.04–0.70 ms against a 3–44 ms A−C gap. p99 moves with
p50 at every point, so neither arm buys its median with its tail.

**What the shape says.**
- **C costs about 7–10 µs per qualifying row**: 4.8 ms for 518 rows on scifact, 38.9 ms for
  5,760 on fiqa. That is detoasting one `wdoc` and one 384-d vector, one BM25 and one inner
  product, per row. C's buffers track the qualifying count: 72–88 at 1–5 rows, 2,068 at
  518, 21,648 at 5,760.
- **A's cost falls with selectivity but has a floor.** The gate is materialized and the
  bolt loop still walks block directories and warp maps whatever the gate holds
  (`bench/RESULTS_GATE_SWEEP.md`, "CORRECTION"). On fiqa, A is 2.1–2.3 ms at 58
  qualifying rows. That floor is what C undercuts.
- **The crossover is neither a constant selectivity nor a constant row count** across two
  corpora 11× apart in size (~100 rows / ~2 % against ~550–800 rows / ~1 %). That is
  expected if A's floor grows with the corpus while C's cost per row does not. Scale 2
  tests it.

## The padding cliff (tracked as G76 on main; recorded here as found)

When the gate admits **fewer rows than LIMIT**, A gets slower by one to two orders of
magnitude:

| corpus | gate | rows | A p50 ms | A buffers | A p50 one point up (rows) | C p50 ms |
|---|---|---|---|---|---|---|
| scifact | facet | 5 | 1.517 | 3,346 | 0.758 (52) | 0.069 |
| scifact | facet | 1 | 1.397 | 3,311 | | 0.033 |
| scifact | lexical | 5 | **40.081** | **17,109** | 0.702 (52) | 0.051 |
| scifact | lexical | 1 | **40.123** | **17,086** | | 0.021 |
| fiqa | facet | 6 | **14.268** | **28,271** | 2.298 (58) | 0.254 |
| fiqa | lexical | 6 | **261.142** | **80,756** | 2.141 (58) | 0.056 |

**Mechanism.** In `weave_gettuple`'s ordered path (`src/am/amscan.c`, at the
`weave_pad_begin()` call), once `weave_fuse_grow()` reports the candidates exhausted,
`weave_pad_wanted()` returns true for every fused scan (G71). `weave_pad_begin()` then
starts a `table_beginscan()` over the **whole heap**. `weave_pad_gettuple()` calls
`FormIndexDatum()` on each row and emits each one with `xs_recheck`, and the executor
rejects every row that fails the WHERE clause. The answer is correct. The cost is
O(heap), paid exactly when the predicate is most selective, which is the case claim 3 is
about. A lexical gate costs more than a facet because the executor's `@@@` recheck and
`FormIndexDatum` detoast every `wdoc`. On local scifact, the 17,009 buffers split into
3,112 heap, 4,749 TOAST and 107 index.

**The work counters cannot see it.** On a local 20k-row table, LIMIT 2 against LIMIT 10
over the same 2 qualifying rows gives identical `pivots`, `scores`, `vec_scores`,
`gate_scores`, `vec_lanes`, `vec_blocks` and `lex_pages_load`. `pg_statio_user_tables`
heap blocks go from **4 to 2,497**, and index blocks are 238 both times. Every
counter-based sweep in `bench/RESULTS_GATE_SWEEP.md` used targets well above k rows, so it
could not have met this case.

**Local reproducer**: `bench/v17_padding_cliff.sql`, verbatim below. It runs against scifact
loaded by `bench/fuse.sh`. The 2026-10-04 local run used the workstation's cached
hash-embedded scifact, `PGDATABASE=v17sci FUSE_LOAD_ONLY=1 FUSE_ALLOW_HASH=1 bash
bench/fuse.sh /scratch/pg_weave/benchdata scifact`. The vectors do not matter to the
cliff, only the 5-row gate and the LIMIT.

```sql
SELECT wq, qv::text AS qv FROM fq ORDER BY qid LIMIT 1 \gset
SET jit = off; SET max_parallel_workers_per_gather = 0; SET pg_weave.fuse_normalize = off;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS abolish_rows FROM fd WHERE body @@@ 'abolish'::wquery;   -- 5
-- (1) the cliff: 5 qualifying rows, LIMIT 10
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF)
SELECT id FROM fd WHERE body @@@ 'abolish'::wquery
 ORDER BY fuse(body <=> :'wq'::wquery, emb <#> :'qv'::wvec, weights => '{0.5,0.5}') LIMIT 10;
-- (2) control: LIMIT 5 = the qualifying count, so no padding
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF)
SELECT id FROM fd WHERE body @@@ 'abolish'::wquery
 ORDER BY fuse(body <=> :'wq'::wquery, emb <#> :'qv'::wvec, weights => '{0.5,0.5}') LIMIT 5;
```

Local output, workstation, so the times are only indicative:

```
(1) Limit (actual rows=5 loops=1)
      Buffers: shared hit=17009
      ->  Index Scan using fd_weave on fd (actual rows=5 loops=1)
            Index Cond: (body @@@ '''abolish'''::wquery)
            Rows Removed by Index Recheck: 5178
    Execution Time: 38.962 ms
(2) Limit (actual rows=5 loops=1)
      Buffers: shared hit=128
      ->  Index Scan using fd_weave on fd (actual rows=5 loops=1)
            Index Cond: (body @@@ '''abolish'''::wquery)
    Execution Time: 0.329 ms
```

The EC2 run shows the same signature on the MiniLM corpus, for the same term and query 1
(`v17-plans-scifact.txt`, `=== BUFFERS lex 0.001 A`): `Buffers: shared hit=17109`,
`Rows Removed by Index Recheck: 5178`. **5178 = 5183 − 5.** Every row in the table that
fails the predicate passed through the executor.

**Why this matters to V17 itself.** The cliff is the largest effect in this file
(4,663× on fiqa's 6-row lexical point), but it is **not** the crossover. A switch to
materializing would hide it, and so would restricting the padding walk to the gate's
tidset. The crossover numbers above are all taken at points where A does not pad.

## Scale 2

*(pending: synthetic 1M rows × 384-d, same run, in progress when this was written)*

## What this does NOT tell us

- **C is SQL, not a product mechanism.** Its per-row cost includes executor overhead and a
  heap-vector detoast. A switch inside the AM would score codes from the weft and doclen
  from the sidecar, so its constant could be lower than C's (no detoast) or higher (random
  code-page access instead of sequential heap order). That constant is **unmeasured**. D was
  meant to model it and does not, because `weave_search(k = N)` and the corpus-wide
  `weave_vec_scan` dominate it with a floor the predicate never touches.
- **C changes the answer.** Its objective is exact while A's is quantized; they overlap
  99.7–99.9 %. A switch that scores heap vectors would make results depend on selectivity
  by an amount this file measures but does not decide on. A code-scoring switch would not.
- **One k (10), one weight vector (0.5/0.5), one embedding model, one instance type.** The
  crossover in rows should scale with the cost of A's floor, which depends on segment count
  (one segment per index here), corpus size and k. A larger k raises the number of
  qualifying rows at which A starts padding.
- **Warm and resident only.** Every buffer is a hit. Under memory pressure, C's random heap
  reads and A's sequential directory walks would trade places in an unknown way.
- **Correlated predicates are untested.** Both gates here are uncorrelated with the
  ranking and with heap order. A facet correlated with the vector neighbourhood would help
  A's bolt bounds and leave C unchanged.
- **The local cliff split and the counter identity come from the workstation** and are
  counts, not times. The cliff's latency comes from EC2.

## Reproduce

```sh
cd <worktree> && echo 'bash bench/aws/v17_job.sh' > /scratch/pg_weave/v17/job.sh
SCRIPT=/scratch/pg_weave/v17/job.sh bench/aws/run.sh c7i.4xlarge script
# outputs: bench/aws/out/<run>/remote/v17-{lat,raw,buf,check,points,plans,summary}-<db>.*
# table:   per-point A/B/C/D p50 averaged over the two slots, from v17-lat-<db>.tsv
```

On an already-loaded database: `PREP=none DB=<db> bash bench/v17_crossover.sh`.
