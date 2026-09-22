# Fused top-k vs RRF over-fetch — first real-corpus measurement

**Date:** 2026-09-22 · **Commit:** d30ee5c · **Extension:** 0.19.0 · **PostgreSQL:** 17
**Host:** EC2 `c7i.8xlarge` (32 vCPU, 64 GB), us-east-2, gp3 8000 IOPS / 500 MB/s
**Embeddings:** `all-MiniLM-L6-v2`, 384-d, CPU, computed on the instance
**Harness:** `bench/aws/run.sh c7i.8xlarge fuse` → `bench/prepdata.py` + `bench/fuse.sh`
**Tuning:** `shared_buffers` 25 GB, `work_mem` 256 MB, `jit = off` (recorded in `tuning.log`)

## Verdict: the Phase F gate is NOT met. Two of five rows fail, and one of them fails for a reason this project already measured and did not act on.

| `FUSED_TOPK.md` §8 row | Gate | scifact | nfcorpus | fiqa | |
|---|---|---|---|---|---|
| recall vs exhaustive fused scan | 1.000 | **1.000** | **1.000** | **1.000** | **PASS** |
| p99 latency, k=10 | ≤ 0.70× RRF | **0.609×** | **0.560×** | **0.633×** | **PASS** |
| p50 latency, k=10 | ≤ 0.50× RRF | 0.582× | 0.795× | 0.578× | **FAIL** (faster, not 2× faster) |
| nDCG@10 | ≥ RRF | 0.982× | 0.924× | 0.687× | **FAIL** |
| channel `score()` calls | ≤ 0.20× RRF | 0.648× | 0.903× | 0.541× | **FAIL** |

Both arms read **one** `weave` index over `(body, emb)`; nothing differs but the
scorer. The control does not pay the storage cost of a real two-index RRF stack, so
every margin here is a **lower bound** on the margin against a genuine deployment —
which makes the two failures worse, not better.

## The two failures

### 1. The linear-sum objective ranks worse than RRF, and worst where the vector channel matters most

| dataset | docs | nDCG@10 fused | nDCG@10 RRF | ratio | recall@100 fused | recall@100 RRF |
|---|---|---|---|---|---|---|
| scifact | 5,183 | 0.6720 | 0.6846 | 0.982× | 0.8892 | 0.9517 |
| nfcorpus | 3,633 | 0.3161 | 0.3422 | 0.924× | 0.2908 | 0.3251 |
| fiqa | 57,600 | 0.2393 | 0.3482 | **0.687×** | 0.5141 | 0.6932 |

**This is not a correctness defect.** The fused scan computes its objective exactly —
the recall row is 1.000 on every dataset, verified against an exhaustive per-channel
oracle. The objective itself is the problem.

`fuse(body <=> q, emb <#> v, weights => '{0.5,0.5}')` is a weighted sum of **raw,
unnormalized** channel scores. BM25 is unbounded and runs to ~10–20 on these corpora;
a quantized inner product lives in ~[−1, 1]. Measured on scifact: max BM25 ≈ 10.0,
max vector score ≈ 0.305 — a **33× scale mismatch**. At equal weights the vector term
cannot reorder the BM25 ranking by more than a rounding nudge, so the fused arm is
**effectively lexical-only**, and it loses to a control that is scale-free by
construction. RRF ranks on *reciprocal rank*, so a channel's units never enter.

The evidence that this is the mechanism, rather than a guess: **the deficit tracks how
much the dense channel should contribute.** fiqa is the dataset in this set where dense
retrieval carries the most signal, and it is where the fused arm loses by 31 %. On
scifact, where BM25 alone is already strong, the arms are within 2 %.

**So "better quality" cannot be claimed, and the gate as written cannot be met by
tuning the scorer.** What it needs is per-channel score normalization or calibration
before the sum — a design gap, tracked as `doc/GAPS.md` **G44**. Until then the honest
statement is: *the fused scan computes a weighted sum faster than RRF computes a rank
fusion, and the weighted sum is the worse ranking function.*

### 2. The `score()`-call mechanism fails, and the vector block bound is why

This is the row §8 singles out: *"If the `score()` call ratio is not dramatically lower,
stop and fix the bounds before optimizing anything else — a loose bound makes the entire
design pointless."*

Split by channel, the ratio says exactly where the problem is:

| dataset | lexical calls fused/RRF | vector calls fused/RRF | combined | vector share of fused calls |
|---|---|---|---|---|
| scifact | 0.353× | **0.991×** | 0.648× | 71 % |
| nfcorpus | 0.581× | **0.985×** | 0.903× | 87 % |
| fiqa | **0.149×** | **0.956×** | 0.541× | 86 % |

**The lexical side works.** On fiqa it clears the 0.2× gate on its own (0.149×) — the
fused threshold really does suppress lexical scoring work, and `blkskip` confirms it:
3,444,538 blocks skipped on fiqa against 7,081,750 pivots.

**The vector side does essentially nothing.** 0.956–0.991× of the control, and
`vec_blocks_bound_skipped = 0` on **all three datasets** — the vector block bound
pruned **zero** blocks, so the vector channel scores every live lane in both arms. And
because the vector channel is 71–87 % of all fused `score()` calls, it drags the
combined ratio to 0.54–0.90× however well the lexical side prunes.

**This was already known and is the sharpest process lesson in this run.**
`bench/RESULTS_BOUND_PRUNING.md` measured the vector block bound pruning **0.0 %** of
blocks, and `doc/GAPS.md` G43 recorded the generalization — *any bound this project
computes but does not act on is in the same position*. The fused scorer is the first
consumer that depends on that bound doing work, and it does not. The mechanism row was
therefore predictable from a measurement already on disk; nobody connected the two until
the gate failed. Hard rule 9 says measure the thing the design rests on before building
on it. The measurement existed. The *inference* did not.

## Latency: a real win, and the A/A leg says so

| dataset | p50 fused | p50 RRF | p99 fused | p99 RRF | p50 A/A repeat | noise floor | delta ÷ noise |
|---|---|---|---|---|---|---|---|
| scifact | 2.135 | 3.668 | 2.949 | 4.840 | 2.144 | 0.009 ms | **170×** |
| nfcorpus | 1.488 | 1.871 | 1.667 | 2.978 | 1.489 | 0.001 ms | **383×** |
| fiqa | 12.687 | 21.968 | 19.804 | 31.277 | 12.674 | 0.013 ms | **714×** |

All milliseconds; 50 queries × 7 reps, arms **alternated per query**, first rep dropped.

`fused_aa` is the **same arm measured a second time**, in the third slot of each query's
rotation — after the RRF run, so it carries whatever that did to the cache rather than
sharing a warm one. This is AGENTS.md hard rule 10, and it is the only reason the
latency numbers above are admissible: the within-arm spread is **0.001–0.013 ms**, while
the between-arm delta is **170–714× larger**. The p99 win is genuine and the p50 win is
genuine; the p50 win is simply not the 2× the gate asks for.

Both plans were asserted, not assumed — `fuse_plans` in each log shows an
`Index Scan using fd_weave` for the fused arm and one per channel for RRF. A flat
latency curve is usually the planner having ignored the index.

## Correctness: 299 of 299 comparable queries exact

| dataset | queries attempted | **compared** | mismatched vs oracle | skipped (ambiguous oracle) |
|---|---|---|---|---|
| scifact | 100 | 100 | **0** | 0 |
| nfcorpus | 100 | 99 | **0** | 1 |
| fiqa | 100 | 100 | **0** | 0 |

The oracle is exhaustive and per-channel — `weave_search()` at k = |D| plus
`weave_vec_scan()` at k = |D|, summed outside the index. It is deliberately **not** the
`fuse()` fallback, which has no corpus and scores with df = 1 (§7a (1)); the fallback
disagreed on 100/85/100 queries and that number is a diagnostic, not a failure.

This row is the reason the run happened at all: it is the same gate that caught
**G43** (a posting cursor reporting itself exhausted one block early) on first contact
with a real corpus, and it is why no EC2 time was spent while that was open.

**`compared` is the load-bearing column, and it exists because of this run.** The first
attempt reported "gate passed" on nfcorpus and fiqa having compared **0 of 100** queries:
the tie test demanded every score in the whole corpus be distinct, which is false on any
corpus of this size, so every query was skipped and the mismatch count stayed 0. Only a
tie *straddling rank 10* makes a top-10 set ambiguous, which is what is tested now, and
the gate now **dies** when it compares nothing. A gate that reports on something other
than the thing under test is this project's most recurrent failure mode; that makes three
this week (G42, the phantom GUC in G43, this).

## What was NOT measured, and why

- **MS MARCO passage.** §8 names it explicitly and it is **missing**. The upstream
  source `bench/prepdata.py` fetches — `msmarco.z22.web.core.windows.net/msmarcoranking/`
  — now returns **HTTP 404** for `queries.dev.small.tsv`; the hosting moved. Tracked as
  `doc/GAPS.md` **G45**. It would not change the verdict: the nDCG gate fails on three
  BEIR datasets already, and a fourth cannot turn three losses into a win.
- **A second embedding model.** Every number here is `all-MiniLM-L6-v2`. The scale
  mismatch behind failure 1 is a property of *BM25 vs cosine-scale vectors* generally,
  but the size of the nDCG deficit is model-specific and should not be quoted as if it
  were not.
- **Concurrency.** Single client throughout. Nothing here says anything about the fused
  scan under contention.

## Reproducing

```sh
AWS_PROFILE=hotdog AWS_REGION=us-east-2 \
  FUSE_DATASETS="scifact nfcorpus fiqa" FUSE_LIMIT=200000 \
  REPS=7 LATN=50 CHECKN=100 \
  bash bench/aws/run.sh c7i.8xlarge fuse
```

Datasets are downloaded and embedded on the instance; `manifest.json` per dataset
carries the sha256 of every emitted file. Hard rule 11: **these numbers are provisional
until they reproduce at a second scale.** The three corpora here span 3,633 → 57,600
documents, which is a 16× range and is why the per-dataset trend in failure 1 can be
read at all — but it is one run on one instance type with one model.
