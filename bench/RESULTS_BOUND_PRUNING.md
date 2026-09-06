# Result: does the vector-channel block bound actually prune?

Date: 2026-09-06. Harness: `bench/bound_pruning.c`. Reproduce with

```
gcc -O2 -I include -o /tmp/bound_pruning bench/bound_pruning.c \
    src/vector/quantize.c src/vector/pack.c -lm
/tmp/bound_pruning 1     # warp ordered by cluster
/tmp/bound_pruning 0     # warp in random order
```

This measurement was taken **before** implementing the fused scorer, on purpose.
`doc/specs/FUSED_TOPK.md` §8 sets a gate of `score()` calls ≤ 0.2× the RRF
baseline, and notes that if the bounds are too loose the entire design is
pointless and no amount of SIMD recovers it. So the bound was measured first.
It is a good thing it was.

## Setup

- dim = 256, 4-bit codes, 256 blocks × 32 lanes = 8192 vectors, k = 10.
- 200 queries. Each query is a **perturbation of a real corpus vector**, not a
  random direction, so true near-neighbours exist and θ is meaningful. Measuring
  against random query directions gives a flattering-looking tightness ratio and
  a completely wrong pruning rate; that mistake is easy to make and worth
  naming.
- Metric: fraction of blocks whose bound ≤ θ, i.e. skippable without scoring any
  lane.
- Soundness is asserted on every block of every query: the harness exits
  non-zero if any lane's score exceeds the bound. It did not.

Two warp orderings:

- **coherent** — each 32-lane block is one tight cluster (32 perturbations of a
  shared direction, σ = 0.35). This is what ordering the warp by the Vamana
  build's k-means partition produces.
- **random** — vectors assigned to blocks in arbitrary order. This is what you
  get if codes are written in heap order.

## Numbers

| bound formulation | coherent warp | random warp |
|---|---:|---:|
| (B1) LUT / per-coordinate max: `smax · Σⱼ maxc(qⱼ·C[c])` | 0.0 % | 0.0 % |
| (B2) Cauchy-Schwarz: `maxrecnorm · ‖q‖₂` | 0.2 % | 0.0 % |
| (B3) centroid + radius: `⟨q,c⟩ + ‖q‖₂·R` | **99.6 %** | 0.0 % |
| min of all three | **99.6 %** | 0.0 % |

Resulting `score()` call ratio: **0.004** with a coherent warp, **1.000** with a
random one.

## Two conclusions, both of which changed the design

**1. The bound originally specified was worthless.** (B1) — the natural
per-coordinate maximum over the query lookup table, which is the direct analogue
of block-max WAND's `max_tf` and was what the first draft of
`doc/specs/FUSED_TOPK.md` §2 proposed — prunes **0.0 %** of blocks. It is
provably correct and completely useless.

The reason is structural, not a tuning failure. Because the codebook is
symmetric about zero, `Σⱼ maxc(qⱼ·C[c])` equals `absmax · ‖q‖₁`, and in
dimension *d* we have `‖q‖₁ ≈ √d · ‖q‖₂`. So (B1) is looser than plain
Cauchy-Schwarz by a factor of order √d before any data-dependent slack is
considered — 16× at d = 256, measured at 2.2×–4× after the max-over-lanes
partly closes the gap. An estimate that is 8–20× above the value it bounds never
falls below a threshold set by real neighbours.

(B2) Cauchy-Schwarz is genuinely tighter and still prunes essentially nothing:
it is tight only when a block member is parallel to the query, and in high
dimension nothing is parallel to anything.

(B3) works because it separates the block's *location* from its *extent*.
`⟨q,c⟩` carries all the query-dependent signal and `‖q‖₂·R` is a small
correction — provided R is small.

**2. Warp ordering is a correctness-adjacent requirement, not an optimization.**
(B3) with a random warp order prunes 0.0 %, identically to the useless bounds.
R is only small when block members are spatially near each other. An
implementation that writes codes in heap order will pass every unit test, satisfy
contract (C2) in `include/weave/channel.h`, produce correct answers — and quietly
degrade the fused scorer to a full scan.

This is now task **V13** in `doc/PHASES.md` with its own gate, and the
requirement is stated in `include/weave/quantize.h` next to the bound itself so
it cannot be missed by someone reading only the header. The Vamana build already
computes a k-means partition for its out-of-core pass, so the ordering is
available for free; it just has to actually be used when assigning warp
positions.

## Cost of (B3)

Per 32-lane block: the centroid stored as a quantized code plus a 4-byte radius.
At dim = 256 and 4 bits that is 128 + 4 bytes against 4096 bytes of codes —
**3.2 % storage overhead for a 250× improvement in pruning**. Computing
`⟨q,c⟩` is one extra lookup-table gather per block, which is 1/32 of the work of
scoring the block it may let us skip.

One subtlety, easy to get wrong and load-bearing: R must be measured against the
centroid **as reconstructed from its stored code**, not against the exact float
centroid. The reader only has the code. Using the exact centroid makes R too
small and the bound unsound. `test/hegel/test_quantize.c` does it the correct
way and comments on why.

## What this does not tell us

- d = 256 only, one synthetic distribution, one cluster tightness (σ = 0.35).
  Real embeddings are not isotropic Gaussian mixtures. The measurement must be
  repeated on Cohere-wiki 1024-d and GloVe 200-d before Phase V is called done —
  GloVe especially, since `turbovec`'s own benchmarks name d = 200 as the
  hardest regime for the asymptotic-Beta assumption the codebook rests on.
- Pruning rate is not latency. 99.6 % of blocks skipped is an upper bound on the
  win; the actual speedup depends on whether the surviving 0.4 % are contiguous
  and on how much the per-block bound computation costs relative to scoring.
- Nothing here involves the lexical channel or the fused loop. This measures one
  channel's bound in isolation, which is the right first question but not the
  last one. `bench/fuse.sql` and `bench/RESULTS_FUSE.md` are still owed.
