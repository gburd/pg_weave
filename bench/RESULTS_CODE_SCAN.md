# Result: the vector block bound does not prune on real corpora

Date: 2026-09-13. Harness: `bench/code_scan.c`. Local workstation, 8 cores.
Corpora: TEXMEX GIST-1M at 960-d and GloVe-6B at 200-d, both L2-normalized,
real held-out query vectors, 4-bit codes, k=10, `lut-avx2` kernel.

```
gcc -O2 -march=native -std=gnu99 -I include -o /tmp/code_scan bench/code_scan.c \
    src/vector/quantize.c src/vector/pack.c src/vector/kernels.c -lm
/tmp/code_scan gist/gist_base.fvecs 20000 20 bits=4 order=clustered lists=625 \
    queries=gist/gist_query.fvecs
/tmp/code_scan glove200k.fvecs 100000 20 bits=4 order=clustered lists=3125
```

**Timings are deliberately omitted from this file.** The host was running at load
average 26 on 8 cores while these ran, so wall-clock numbers swung 2.2× on
identical work. The pruning rates and bound components below are deterministic
and contention-independent, which is why they are the only things reported. The
throughput measurement needs a dedicated machine and is not yet taken.

## The question

`doc/PHASES.md`'s restated Phase V gate is met on recall and storage and
unmeasured on latency, and the unmeasured half is the code scan: a query at the
ratified 4 bits must score every vector in the index. `doc/specs/FUSED_TOPK.md` §2
says that is affordable because a code block admits a cheap upper bound —
`⟨q,rₛ⟩ ≤ ⟨q,c⟩ + ‖q‖₂·R` for the block's centroid `c` and radius `R` — so most
blocks are skipped without scoring a lane. `bench/RESULTS_BOUND_PRUNING.md`
measured that bound pruning **99.6%** of blocks.

That measurement was taken at dim=256 over **8,192 synthetic vectors**, where a
"coherent" block is 32 perturbations of a shared random direction with σ = 0.35.
The pruning rate is exactly the quantity such a construction would flatter. This
re-asks it with real vectors, a real k-means clustering, and real queries.

## The answer: it prunes nothing

Fraction of blocks skipped without scoring a lane, one k-means cluster per block
(`lists = nblocks`, the most favourable realistic warp ordering):

| corpus | running θ | oracle θ |
|---|---|---|
| GIST-960d, n = 20k | **0.00%** | **0.00%** |
| GloVe-200d, n = 100k | **0.00%** | **0.01%** |

`oracle θ` compares each block's bound against the *final* k-th best score — the
best any block ordering could achieve, and the quantity the earlier harness
reported. `running θ` is what a scan in stored order gets, starting at −∞. Here
they agree, because the bound never gets under the bar at all.

Natural (file-order) warp ordering gives the same 0.00%, so this is not a
clustering-quality problem.

**Soundness passed on every query of every run**: no bound was ever below a lane
it covers, and the pruned arm's top-10 was identical to the flat arm's. The bound
is correct. It is simply never small enough to be useful.

## Why, in numbers rather than adjectives

Mean over blocks and queries. A bound prunes only if it lands **below** θ:

| | GIST-960d | GloVe-200d |
|---|---:|---:|
| best score (top-1) | 0.8828 | 0.5258 |
| **θ (k-th best) — the bar** | **0.8657** | **0.3905** |
| B1, LUT / per-coordinate max | 2.2098 | 2.2413 |
| B2, Cauchy–Schwarz `max‖rec‖·‖q‖` | 1.0058 | 1.0077 |
| B3, centroid+radius `⟨q,c⟩ + ‖q‖·R` | 1.3169 | 1.0395 |
| — of which `⟨q,c⟩` | 0.7317 | 0.0217 |
| — of which mean `R` | 0.5851 | 1.0177 |
| min of three | 1.0058 | 0.9820 |

**The two corpora fail for opposite reasons, which is what makes this structural
rather than bad luck.**

- **GIST** has genuinely tight blocks — mean radius 0.585 on a unit sphere — but
  every centroid resembles the query (`⟨q,c⟩` = 0.73), because GIST descriptors are
  globally correlated. So B3 = 1.32.
- **GloVe** has unrelated centroids (`⟨q,c⟩` = 0.02) but no tight blocks: mean
  radius 1.018, essentially the full spread of the sphere. So B3 = 1.04.

And underneath both, **B2 is ≈ 1.0 by construction.** For L2-normalized data
`max‖rec‖ ≈ ‖q‖ ≈ 1`, so Cauchy–Schwarz says "no score here exceeds 1.0", which is
true and useless — the maximum possible inner product between unit vectors is 1.0.
Since θ < 1.0 always, the min-of-three bound sits at ~1.0 and can only prune if B3
gets under θ, which requires `⟨q,c⟩ + R < θ`.

**The deeper reason is that the bound assumes worst-case alignment.** B3 comes from
Cauchy–Schwarz on the residual: `⟨q, rₛ − c⟩ ≤ ‖q‖·‖rₛ − c‖`, which is tight only
when the residual points along `q`. In *d* dimensions two arbitrary vectors are
nearly orthogonal, so the true term is smaller than the bound by a factor that
grows with `√d`. The bound is sound, and it is loose by an amount that increases
with exactly the dimensionality the channel exists to serve. That is a property of
radius bounds, not of this implementation, and it is the reason production ANN
systems reduce candidates with a graph or an inverted list rather than by bounding
a flat scan.

## What this costs the design

1. **`bench/RESULTS_BOUND_PRUNING.md`'s 99.6% is a synthetic-only result** and must
   not be quoted as a property of the channel. Its own setup section says the
   coherent case is "32 perturbations of a shared direction, σ = 0.35" and calls
   that "what ordering the warp by the Vamana build's k-means partition produces".
   Measured, k-means on real data does not produce that. The file has been marked.

2. **`doc/specs/FUSED_TOPK.md` §2's vector bound does not deliver its half of the
   fused-threshold mechanism.** The *lexical* half — block-max WAND over
   `max_tf`/`min_doclen` — is a different bound over a different quantity and is
   separately validated; nothing here touches it.

3. **Claim 2 of `doc/ARCHITECTURE.md` §9 (fused-threshold top-k) is now supported
   on its lexical half and unsupported on its vector half.** That needs the
   maintainer's attention before any external claim is made, because a claim that
   is half-true in a way we knew about is worse than one we withdrew.

4. **V13 (warp ordering by cluster) loses its remaining justification.** It was
   already reduced to "the block bound needs a coherent warp" when V9 was demoted;
   if the bound cannot prune with a coherent warp on real data, ordering the warp
   buys nothing measurable. It should be withdrawn or re-justified on some other
   ground.

5. **V9's demotion should be revisited, in the direction of re-promotion.** IVF was
   demoted because pg_turbovec measured flat beating IVF at 2 and 4 bits, and the
   flat scan was expected to be cheap *because the bound pruned it*. Without the
   bound, the flat scan is a full scan of every code on every query, and IVF is the
   only candidate-reduction mechanism this design has left that has not been
   withdrawn. This is not an argument that IVF works — it is an argument that the
   reason for demoting it no longer holds.

6. **What is NOT affected: the allowlist mask, and therefore Claim 3.** Skipping
   lanes that a selective predicate has already excluded is a different mechanism
   from skipping blocks that cannot win. `weave_score_block()` skips masked lanes
   without touching a code byte, and that is what makes a more selective query
   faster. Nothing here weakens it. The distinction matters: *masking* is
   predicate-driven and works; *bounding* is score-driven and does not.

## What is still unmeasured

**Throughput.** How long a flat scan of 1M codes actually takes at 4 bits and
960–1024-d, per kernel (`scalar`, `lut-wide`, `lut-avx2`), on an uncontended
machine. That number decides how bad the absence of pruning is, and it is the
next measurement. The preliminary local figures were taken under 3× CPU
oversubscription and are not reported here for that reason.

A rough expectation to be checked rather than trusted: at 4 bits a 960-d lane is
480 B, so 1M vectors is 480 MB per query, and the LUT path does one gather per
coordinate per vector — 960M gathers per query. If that is the shape of the cost,
the flat scan is compute-bound rather than bandwidth-bound, and pg_turbovec's
independently measured 490× loss to pgvector HNSW for a flat scan is the reference
point to compare against.
