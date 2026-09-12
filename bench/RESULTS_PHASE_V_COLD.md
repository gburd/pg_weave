# Result: the Phase V gate on EC2 — window at n=1M, the real HNSW baseline, and cold latency

Date: 2026-09-12. Instances: two `r7i.2xlarge` (8 vCPU, 64 GB, Xeon Platinum
8488C), us-east-2, gp3 root at 8000 IOPS / 500 MB/s, runs
`pgweave-20260912-204034` (pg_weave side) and `pgweave-20260912-211128` (pgvector
side). **One engine per host**, so neither side shares a buffer pool or a page
cache with the other. Harnesses: `bench/rerank_cold.sh`, `bench/hnsw_base.sh`,
`bench/ivf_recall.c`, corpus TEXMEX GIST-1M at 960-d, L2-normalized, 999,990 rows
after 10 zero-norm rows were dropped.

Reproduce: `bench/aws/run.sh r7i.2xlarge rerankcold` and
`bench/aws/run.sh r7i.2xlarge hnswbase`.

**Read the validity section before quoting any latency from this file.** Some arms
of this run are invalid and are published as invalid.

## 1. The window at n=1M: +25% per decade, at every width

`bench/RESULTS_BITWIDTH_SWEEP.md` drew the Phase V frontier from windows measured
at n = 100k–200k and flagged them as a lower bound for the 1M gate corpus, on the
argument that ten times the vectors put ten times more near-neighbours in range to
displace the true top-10. That is now measured. Full probe, k=10, self-check PASS.

| bits | w10 | w15 | w20 | w25 | w30 | w40 | w50 | w75 | w100 |
|---|---|---|---|---|---|---|---|---|---|
| 3 | 0.7240 | 0.8410 | 0.9160 | 0.9530 | 0.9730 | 0.9840 | **0.9930** | 0.9990 | 1.0000 |
| 4 | 0.8520 | 0.9540 | 0.9810 | **0.9920** | 0.9980 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
| 5 | 0.9100 | 0.9880 | **1.0000** | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |

Smallest window reaching `recall@10 >= 0.99`, against the same corpus at n=100k:

| bits | n=100k | n=1M | growth |
|---|---|---|---|
| 3 | 40 | 50 | +25% |
| 4 | 20 | 25 | +25% |
| 5 | 15 | 20 | +33% |

**The direction was right and the magnitude is modest: about +25% per decade of
n, uniform across widths.** Not the doubling that would have made the shape
unaffordable, and not the invariance that would have made the warning wrong.

Two controls make this attributable to *n* rather than to the machine:

- the same binary was run at n=100k on the same host and reproduced the published
  n=100k row exactly (0.7880 / 0.9100 / 0.9570 / 0.9780 / 0.9860 / 0.9950 /
  0.9980 / 1.0000);
- the n=1M 3-bit row was reproduced **bit-identically on a different machine**
  (the local workstation) with `lists=1` instead of `lists=1024`. Same eight
  figures to four decimals. That is a direct confirmation of the theoretical claim
  that the full-probe column is partition-independent — and it makes `lists=1
  probes=1` the cheap way to measure a ceiling, since it skips k-means entirely.

## 2. The HNSW denominator was an estimate, and it was 30% low

`doc/specs/VECTOR_CHANNEL.md` §2.1.1 has been dividing every "× HNSW" figure in
Phase V by "on the order of 5,700 B per vector at m = 16", explicitly flagged as
"unmeasured on this corpus — measure before quoting it", and then quoted many
times. Measured, m=16, ef_construction=64, `vector_cosine_ops`, 999,990 × 960-d:

| | |
|---|---|
| HNSW index | 7,683 MB |
| **bytes/vector** | **8,056** |
| table + toast | 11 GB |
| build time | 277 s (24 GB `maintenance_work_mem`, 7 parallel workers) |

**So the 0.15× budget is ~1,208 B/vector, not ~855.** Re-pricing the frontier at
1024-d, where index bytes are `bits × 128`:

| bits | index B/vector | × HNSW | window @ 0.99 (n=1M) | SIMD kernel |
|---|---:|---:|---|---|
| 3 | 384 | 0.048 | 50 | yes |
| **4** | **512** | **0.064** | **25** | **yes** |
| 5 | 640 | 0.079 | 20 | no |
| 6 | 768 | 0.095 | ~20 | no |
| 8 | 1024 | **0.127** | — | no |

**This overturns a stated conclusion.** `bench/RESULTS_BITWIDTH_SWEEP.md` reasoned
that "8 bits is 1,024 B/vector at 1024-d = 0.18× HNSW against a 0.15× budget, so
the width that clears 0.99 on the easier corpus already misses the storage claim".
Against the measured denominator 8 bits is **0.127×** and fits with room to spare.
The single-width conclusion still holds, but the reason has changed: a single code
width fails on **recall** (8 bits reaches only 0.9860 on GIST at full probe), not
on storage. Every "jointly unsatisfiable" argument that leaned on the storage half
was leaning on a guess.

The recommended shape is unchanged and now has more margin: **4 bits plus an exact
rerank of a top-25 window** — 0.9920 recall at n=1M, 512 B/vector = **0.064×
HNSW**, and the widest width that keeps the SIMD code-scan kernel.

## 3. pgvector HNSW never reaches recall@10 0.99 on this corpus

Recall@10 against an exact sequential scan over the same table, 25 held-out
queries, all returning 10 rows, plan asserted to use the index:

| ef | 10 | 40 | 100 | 200 | 400 | 800 |
|---|---|---|---|---|---|---|
| recall@10 | 0.4400 | 0.7200 | 0.8560 | 0.9160 | 0.9600 | **0.9760** |

**It tops out at 0.9760.** This independently reproduces the observation
`VECTOR_CHANNEL.md` §8a imported from pg_turbovec — that pgvector HNSW never
reached 0.99 at any `ef` on this corpus, `ef = 400` topping out at 0.983. The
figures differ (their 0.983 vs our 0.960 at ef=400) but the conclusion is the
same, from an independent implementation of the measurement.

**The consequence for the gate is structural, not numerical.** Phase V's
`p50 <= 2x pgvector HNSW` is conditioned on `recall@10 >= 0.99`, and the baseline
does not have an operating point at that recall. There is nothing to be 2× of. The
gate needs restating rather than passing or failing, which is what §8a said was
"worth resolving explicitly rather than discovering at the end".

**Caveat that limits this, and it is a real one.** `m = 16` and
`ef_construction = 64` are modest for 960 dimensions; pgvector's own guidance is
to raise `m` for high-dimensional data. So 0.9760 is a ceiling for *these build
parameters*, not for HNSW. A sweep over `m` and `ef_construction` is the obvious
next measurement and could move this number materially. Until it is run, the
honest statement is "not at m=16/ef_construction=64", not "not at all".

## 4. Cold latency: what is valid, and what is not

### Invalid: every warm arm after the first loop iteration

`pg_prewarm` lives in an extension neither harness created, so every call failed
and every failure was swallowed by `|| true`. The consequence, reported as a
result before it was caught:

- `rerank_cold` window=20 "warm" **read 230.9 pages per query** and reported 57.3 ms;
- `hnsw_base` ef=40 "warm" **read 2,136 pages per query** and reported 879.6 ms —
  250× the ef=10 warm figure for 4× the `ef`.

Only the **first** iteration of each loop was genuinely warm, and only because the
load had just finished and the OS page cache still held everything. Both were
verified warm by reading **zero** pages, which is what makes them usable:

| arm | p50 | reads |
|---|---|---|
| rerank, window 10, warm | **0.598 ms** | 0 |
| pgvector HNSW, ef 10, warm | **3.548 ms** | 0 |

The guard did not catch this because it checked that cold read *more* than warm — a
**relative** property, which held perfectly while both arms read heavily. The
failure was **absolute**: a warm arm that reads at all is not warm. Both harnesses
now assert absolutely in both directions and verify `pg_prewarm`'s returned page
count against `pg_relation_size` for every relation including the TOAST relation.

### Also invalid: the read counts

Buffer counts summed the planner's own catalog access into the execution figure,
while latency came from `Execution Time`, which excludes planning. On a
freshly-restarted server that is hundreds of cold catalog reads charged to the
query. So the "reads per candidate" this run appears to show (13–19) is **not
attributable** and is not quoted here. Counts are now split at the `Planning:`
line, and one plan per arm is kept — not having kept a plan is why the attribution
could not be recovered after the fact.

### Valid: the cold latencies

`Execution Time` is execution-only, and the cold protocol — `sync`,
`drop_caches`, restart PostgreSQL, one query — is sound. These stand:

| pg_weave heap rerank (cold) | p50 | | pgvector HNSW (cold) | p50 | recall@10 |
|---|---|---|---|---|---|
| window 10 | 55.6 ms | | ef 10 | 416 ms | 0.4400 |
| window 20 | 86.0 ms | | ef 100 | 2,100 ms | 0.8560 |
| window 40 | 148.1 ms | | ef 400 | 6,191 ms | 0.9600 |
| window 100 | 328.7 ms | | ef 800 | 11,471 ms | 0.9760 |

**Cold, the heap rerank is not the problem.** At the recommended shape's window of
25 it interpolates to roughly 100 ms, against 6.2 s for the closest thing pgvector
has to a 0.99 operating point. The mechanism is the one predicted in
`bench/RESULTS_RERANK_IO.md`: HNSW's traversal is *dependent* random I/O — it
cannot know its next node until the current one is scored — while a rerank window's
TIDs are all known before the first fetch. Cold, that difference is two orders of
magnitude, in our favour.

**And the local prediction was still wrong.** `bench/RESULTS_RERANK_IO.md`
predicted ~48 page reads for a 20-candidate window at 1024-d and, at ~0.25 ms per
random read, roughly 12 ms cold. Measured: **86 ms**, about 7× that. The page
counts in that file were called "device-independent" on the grounds that they are a
property of the storage layout. That claim is **falsified**: at 250k rows with a
32 MB pool the toast *index* stayed largely resident, so descents were nearly free,
while at 1M rows with a genuinely cold cache each descent pays its full depth. Page
counts are a function of cache state and scale, not of layout alone.

## 5. What this does not measure, and it is the deciding thing

**There is no pg_weave vector query here.** V7 (code pages) and V8 (the code-scan
shuttle) are not implemented, so what was measured is the **rerank component in
isolation** — 25 candidate vectors fetched from the heap and scored exactly. A
real query also scans 1M codes, which at 4 bits is 512 MB to read and score, and
that cost is **unmeasured**. Cold, a 512 MB sequential read on this volume is
~1 s at the provisioned 500 MB/s; warm it is a SIMD pass whose nearest reference
point is pg_turbovec's 41.4 ms flat scan at a different width and dimension.

So: the rerank half of the shape is affordable and the code-scan half is unknown.
Nothing here licenses a claim that pg_weave's vector channel beats pgvector at
anything. What it licenses is narrower and still useful — **the heap rerank, which
was the part the Phase V gate was reopened over, is not what will decide it.**

## 6. Harness failures, recorded because they are the transferable part

Nine bugs, listed in `bench/aws/run.sh`'s history and the memory note. The three
worth generalizing:

1. **A relative guard cannot detect an absolute failure.** Third instance this
   cycle: the `shared_buffers` readback that ran under an auth that always failed;
   the slice-vs-full detoast guard that could not distinguish arms sharing a page;
   and now cold-reads-more-than-warm while neither arm was what it claimed.
2. **Never edit a running bash script.** bash reads scripts incrementally, so
   editing `run.sh` mid-run shifted byte offsets, killed the driver with a syntax
   error 400 lines from anything relevant, and lost the bits=4/5 half of the n=1M
   sweep. It was re-run locally for free.
3. **A driver that reports success for a job that produced nothing is worse than
   one that crashes.** `run.sh` has no `set -e`; an unchecked pipeline printed
   "done — artifacts in ..." over a job that died on its first SQL statement.
