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

Confirmed on the uncontended `c7i.4xlarge` at a larger size: **n = 200k, one
k-means cluster per block (`lists = 6250`, 6 iterations), 0.00% pruned at oracle
θ.** θ = 0.8881, `⟨q,c⟩` = 0.7255, mean R = 0.7160, so B3 = 1.4415 — the same
mechanism at ten times the corpus and with a better-converged clustering.

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

## Throughput: 291 ms per query at n = 1M, and the width does not matter

Date: 2026-09-13. Instance: `c7i.4xlarge` (16 vCPU, Xeon Platinum 8488C),
us-east-2, run `pgweave-20260913-*`, **doing nothing else**. GIST-960d,
L2-normalized, real held-out queries, 10 queries per point, `order=natural`
(nothing prunes, so warp order cannot change the time). Reproduce with
`bench/aws/run.sh c7i.4xlarge codescan`.

Nanoseconds per vector scored, which is the figure that extrapolates:

| kernel | n = 50k | n = 200k | n = 1M, 4 bits | n = 1M, 3 bits |
|---|---:|---:|---:|---:|
| `lut-wide` | 292.6 | 291.6 | **291.2** | **292.1** |
| `lut-avx2` | 426.6 | 425.4 | 425.2 | 419.0 |
| `scalar` | 10,945.3 | 10,953.4 | 10,943.2 | 7,998.4 |

Per query at n = 1M: **291 ms** (`lut-wide`), 425 ms (`lut-avx2`), **10.9 s**
(`scalar`).

### It is compute-bound, and the evidence is that n does not matter

Per-vector cost is flat to within 0.5% from n = 50k to n = 1M. At 50k the codes
are 23 MB and fit in L3; at 1M they are 458 MB and cannot. **If this were
bandwidth-bound those two points would differ, and they do not.** The cost is one
LUT gather per coordinate per vector, so it is `O(dim × nvec)` and indifferent to
where the bytes live.

### Therefore 3 bits buys nothing, and revision trigger 1 is resolved for 4 bits

`doc/PHASES.md`'s committed shape lists five named experiments that could replace
it. The first was: "if scanning 4-bit codes dominates, 3 bits scans 25% fewer
bytes and needs window 50 — trading code-scan bytes for rerank page reads."

Measured, **3 bits scans 25% fewer bytes in the same time**: 292.1 ns against
291.2 ns, a 0.3% difference in the wrong direction and inside the noise. The width
sets how many bits come out of each lookup, not how many lookups happen. So:

| | 4 bits | 3 bits |
|---|---|---|
| scan, n = 1M | 291.2 ns/vec | 292.1 ns/vec |
| index | 512 B/vec, 0.064× HNSW | 384 B/vec, 0.048× HNSW |
| rerank window @ 0.99, n = 1M | 25 | 50 |
| rerank cold p50 | ~100 ms | ~180 ms |

Three bits trades 25% of a storage budget that is already met by 2–3× for a
doubled rerank window, which is real cold I/O. That is a slack constraint bought
with a binding one. **4 bits stays, now on measured grounds rather than on the
SIMD-path argument alone.**

### The shipped kernel default is the slower path

`lut-wide` beats `lut-avx2` by **1.44×** at every size and both widths, and
`weave_score_kernel_best()` selects `lut-avx2`. So the default resolution picks the
slower kernel. This independently reproduces what `doc/specs/VECTOR_CHANNEL.md` §9
records from pg_turbovec — they "measured AVX2 and declined it", because a
gather-per-coordinate has no reuse to amortize the gather latency against. Ours is
the same shape and the same outcome.

Not yet established: whether `lut-wide` also wins at small `dim`, where the AVX2
setup cost is amortized over fewer coordinates. Every point measured here is
960-d. The fix is a measurement across `dim`, not a one-line change to the
selection order.

### Widths 5–8 are not merely unattractive, they are unusable

`scalar` is **37× `lut-wide`** — 10.9 s per query at n = 1M. That is what widths
5–8 run, because the wide and AVX2 paths pack 8 lanes into a 32-bit word and are
structurally limited to 4 bits (`KERNEL_GROUP_BITS_MAX`). The ratified shape's
"widest width that keeps the SIMD kernel" argument was made qualitatively; this is
the quantity.

## What this means for the gate, plainly

The restated Phase V gate's latency term is `p50 ≤ 2× pgvector HNSW` at matched
recall, warm and cold. Warm, pgvector HNSW measured **3.548 ms** at ef = 10
(`bench/RESULTS_PHASE_V_COLD.md`), so the bar is ~7.1 ms. A flat scan is **291 ms**
before the rerank is added.

**The vector channel misses the latency term by roughly 40×, and no choice of code
width changes that.** This is the same order as the 490× loss pg_turbovec measured
for their flat scan against pgvector HNSW, reached independently.

Because the cost is `O(dim × nvec)` with no bandwidth component, only two things
can move it:

1. **Scan fewer vectors.** Candidate reduction: IVF (task V9, demoted 2026-09-12)
   or a proximity graph (withdrawn earlier). The block bound was the third option
   and it does not prune. **V9's demotion rested partly on the flat scan being
   cheap once the bound pruned it; that premise is now measured false, so the
   demotion should be reconsidered.**
2. **Score fewer coordinates.** Dimensionality reduction before quantization, or a
   two-stage scan over a prefix of coordinates. Neither is in the ledger.

What will *not* help: narrower codes, a different SIMD kernel (the best available
is already 291 ms, and the portable one at that), or better warp ordering.

## What is still unmeasured

- Whether `lut-wide`'s win over `lut-avx2` holds at lower `dim`.
- Whether the bound prunes on a corpus that is genuinely well-clustered. Both
  corpora here fail it, for opposite reasons, which is what makes the failure look
  structural — but "no real corpus we tried" is not "no corpus".
- Any end-to-end pg_weave vector query, since V7 and V8 still do not exist. The
  291 ms is the scan in isolation, measured through the shipping kernels on
  shipping-format packed codes, which is the closest proxy available without them.
