# Result: would a CLUSTER-ORDERED vector weft shrink the fused scan's candidate set?

**No. 0.00 % of blocks pruned and 100.00 % of lanes scored, on all three BEIR corpora
the nDCG row is measured on, in BOTH orderings — and 0.00 % against an ORACLE threshold
too, which is the ceiling over every possible block ordering.** The measurement was taken
*before* the format change it was meant to justify (hard rule 9), and it withdrew the
change.

Date: 2026-09-23. Local host (floki, 8 cores). Harness: `bench/code_scan.c`, unmodified —
it already carries `order=clustered|natural` with a real k-means (`kmeans_assign`,
`bycluster`). The only new code is `bench/tsv2fvecs.py`, which converts `prepdata.py`'s
TSV corpora into the `.fvecs` the harness reads.

**Why this run exists.** `doc/PHASES.md` V13 (warp ordering by cluster) and
`doc/specs/FUSED_TOPK.md` §8's remaining blocker — the size of the vector channel's
candidate set — met in a maintainer decision on 2026-09-23: order the weft by cluster and
reopen F8, whose `vecdocmap` contract requires strictly ascending docids
(`include/weave/vecdocmap.h:35,105,122`). That is a page-format version bump, a
docid→lane indirection at 4 bytes per document, a merge path that becomes a re-clustering
instead of a merge-sort, and a reopened task. This run is the hour spent asking what it
would buy.

## Method, and what makes the arms comparable

- Corpora: the same MiniLM (`all-MiniLM-L6-v2`, 384-d, L2-normalized) vectors
  `bench/normprod.sh` measures nDCG on — scifact (5,183), nfcorpus (3,633), fiqa (57,600).
  Queries are the datasets' own: 300 / 323 / 648.
- `bits=4 k=10 iters=8`, `lists = ceil(n/32)` — **one k-means cluster per 32-lane block**,
  which is the most favourable setting for a block bound (see the sweep below for why more
  clusters is *worse*, not better).
- `order=natural` keeps file order. That arm stands in for today's weft only because
  `tsv2fvecs.py` preserves row order and asserts the id column is ascending; a sorted
  converter would have made the control arm something else with no visible symptom.
- **Two reps per arm.** These counters are deterministic, so the two reps are
  byte-identical — which is the point: the A/A spread is exactly 0.00 %, so a between-arm
  delta of any size would be real. (It is the *timings* in these logs that are not usable:
  a 57,600×1,800 k-means was running on the same 8-core host. No latency is quoted here.)

## The table

| corpus | arm | θ (k-th best) | B2 | B3 = ⟨q,c⟩ + ‖q‖R | mean R | min of three | blocks pruned, running θ | blocks pruned, **oracle θ** | lanes scored |
|---|---|---|---|---|---|---|---|---|---|
| scifact | natural | 0.4462 | 1.0069 | 1.1477 | 1.0436 | 1.0068 | **0.00 %** | **0.00 %** | **100.00 %** |
| scifact | clustered | 0.4462 | 1.0068 | 1.1034 | 0.9992 | 0.9946 | **0.00 %** | **0.00 %** | **100.00 %** |
| nfcorpus | natural | 0.3697 | 1.0067 | 1.1096 | 1.0227 | 1.0010 | **0.00 %** | **0.00 %** | **100.00 %** |
| nfcorpus | clustered | 0.3697 | 1.0067 | 1.0485 | 0.9619 | 0.9791 | **0.00 %** | **0.00 %** | **100.00 %** |
| fiqa | natural | 0.5441 | 1.0069 | 1.1061 | 1.0307 | 1.0069 | **0.00 %** | **0.00 %** | **100.00 %** |
| fiqa | clustered | 0.5441 | 1.0069 | 1.0699 | 0.9946 | 0.9846 | **0.00 %** | **0.00 %** | **100.00 %** |

Both reps of every row are identical to the digit.

**Clustering worked; it just does not matter.** This is the check that separates "the
change buys nothing" from "the arm did not run" (AGENTS.md: a result needs evidence that
the specific thing you meant to run, ran). The clustered arm's mean block radius is
genuinely smaller — 1.0436 → 0.9992 on scifact, 1.0227 → 0.9619 on nfcorpus, 1.0307 →
0.9946 on fiqa — and B3 falls with it. The bound gets 4–6 % tighter and the pruning rate
does not move, because it needs a factor, not a percentage.

## The inversion: how tight the radius would have to be

A block is prunable when `⟨q,c⟩ + ‖q‖·R ≤ θ`. With ‖q‖ = 1 (normalized corpus, inner
product — pg_weave has no `metric='cosine'`, V16), the required radius is `θ − ⟨q,c⟩`:

| corpus | required R | best measured R | factor still needed |
|---|---|---|---|
| scifact | 0.3420 | 0.9992 | **2.92×** |
| nfcorpus | 0.2831 | 0.9619 | **3.40×** |
| fiqa | 0.4687 | 0.9946 | **2.12×** |

And 2–3.4× tighter is not a tuning target on 384-d normalized sentence embeddings: the
mean distance between two random unit vectors in high dimension is ≈ √2 ≈ 1.414, so a
measured block radius of ~1.0 is already well below random — clustering is doing real
work — while 0.28–0.47 would require blocks of near-duplicates. That is a property of the
data's intrinsic dimensionality, not of the storage order, and no ordering can change it.

**The oracle column makes the argument without the geometry.** `blocks pruned, oracle θ`
is measured with θ set to the true k-th best score from the start — the best any block
ordering could do, since ordering only affects how fast the running θ approaches the
oracle one. It is **0.00 %** in all six arms. Cluster ordering is an attempt to improve a
quantity whose ceiling is already zero.

## The counter-intuitive sweep: more clusters makes it worse

scifact, clustered, `lists` swept while everything else is held:

| lists | vectors per cluster | mean R | blocks pruned, oracle θ | lanes scored |
|---|---|---|---|---|
| 162 (= n/32) | 32 | **0.9992** | 0.00 % | 100.00 % |
| 648 | 8 | 1.0246 | 0.00 % | 100.00 % |
| 1,296 | 4 | 1.0328 | 0.00 % | 100.00 % |
| 2,591 | 2 | 1.0432 | 0.00 % | 100.00 % |

Smaller clusters do not give smaller blocks. A block is 32 lanes regardless, so at
`lists > n/32` each block is a *concatenation of several clusters* and its bounding radius
is the union's, which is larger — at `lists = n/2` the radius (1.0432) is back to the
natural-order value (1.0436). So `lists = n/32` is not a starting point to tune from, it is
the optimum of the whole knob, and the optimum prunes nothing.

## What this refutes, and what it does not

**Refuted:** that a cluster-ordered weft shrinks the vector channel's candidate set on
these corpora. Also refuted, by the same numbers: that `doc/PHASES.md` V13 can be
re-justified by a measurement on the product's own datasets. V13's re-justification has now
failed twice — on GIST-960d and GloVe-200d (`bench/RESULTS_CODE_SCAN.md`, 0.00 % and
0.01 %) and now on the three BEIR corpora that the nDCG row and §8's gate actually use.

**Not refuted, and worth keeping separate:**

- **That some corpus exists where it works.** `bench/RESULTS_CODE_SCAN.md`'s open question
  ("whether the bound prunes on a corpus that is genuinely well-clustered") is still open.
  What this adds is that *the corpora pg_weave measures itself on are not that corpus*, and
  the `lists` sweep says the deficit is not a clustering-quality problem.
- **That the candidate set is the blocker.** It still is. This run removes one of the three
  structural options for shrinking it and says nothing about the other two.
- **Anything about the lexical channel.** Untouched here; the fused scan's lexical work fell
  2.8× under the same normalizer that left the vector channel at 1.000×.
- **`bits=4`.** One width, the ratified one. A wider code tightens B1 (2.234, useless here),
  not B2 or B3.

## Consequence for the maintainer decision of 2026-09-23

The decision was "cluster-ordered weft, and reopen F8". **It is withdrawn before
implementation on this evidence.** The cost was a page-format version bump, a 4-byte/doc
docid→lane indirection, a merge path rewritten from merge-sort to re-clustering, and F8's
strictly-ascending-docid contract reopened — for a measured 0.00 %, with an oracle ceiling
of 0.00 %.

The remaining options are unchanged in kind and now one shorter:

1. **Vector-major second copy** — forfeits the storage gate, which is a stated claim.
   Untested; it attacks *bytes touched per lane* rather than *lanes touched*, so the
   0.00 % above does not bear on it.
2. **Narrow claim 3's scope** to the channels where selectivity demonstrably helps, and
   state the vector channel's 1.000× as a recorded loss.

Both need their own measurement before anyone writes code. That is the rule this file is an
instance of.

## Reproduce

```sh
python3 bench/tsv2fvecs.py --in <corpus.tsv> --out corpus.fvecs --dim 384
python3 bench/tsv2fvecs.py --in <queries.tsv> --out queries.fvecs --dim 384
gcc -O2 -Wall -Wextra -I include -o /tmp/cs bench/code_scan.c \
    src/vector/quantize.c src/vector/pack.c src/vector/kernels.c -lm
/tmp/cs selfcheck=64                      # the kernel gate, first
/tmp/cs corpus.fvecs <n> <nq> bits=4 k=10 order=natural   lists=$(((n+31)/32)) \
    iters=8 queries=queries.fvecs dim=384
/tmp/cs corpus.fvecs <n> <nq> bits=4 k=10 order=clustered lists=$(((n+31)/32)) \
    iters=8 queries=queries.fvecs dim=384
```

Logs: `/scratch/pg_weave/clustersim/{scifact,nfcorpus,fiqa}-{natural,clustered}-{1,2}.log`
and `sweep-scifact-*.log`. The TSV corpora are `/scratch/pg_weave/normstudy/*/corpus.tsv`
(built by `bench/prepdata.py --embed minilm`).
