# Specification: the vector channel

Status: **codec implemented and property-tested; storage, kernels, and graph
unimplemented.** Tasks **V1**–**V14** in `doc/PHASES.md`.

Headers: `include/weave/quantize.h` (backend-independent codec),
`include/weave/vector.h` (type, pages, shuttle), `include/weave/graph.h`
(IVF coarse quantizer; see §8a for why not a graph). Implementation:
`src/vector/`.

Read `include/weave/channel.h` first. The bound contract (C2) is the thing this
channel exists to satisfy, and §6 below is the part of this document that
actually decided the design.

## 1. Scope, and what this is not

This is a **C reimplementation** of TurboQuant, the quantizer in
`~/src/turbovec` (Rust, MIT, same author). It is not a mechanical port: the Rust
code is read as a specification and as an oracle for test fixtures, and the C is
written to PostgreSQL conventions against `include/weave/quantize.h`.

`~/src/zvec` (Alibaba, Apache-2.0) contributed three **ideas** and **no code**:
filter pushed into graph traversal rather than applied after it, systematic
runtime ISA dispatch, and treating vector/lexical/scalar recall as
interchangeable plan operators. Apache-2.0's patent grant and NOTICE
requirements are incompatible with a clean PostgreSQL-licensed release; see
`doc/LICENSING.md`. Do not open a zvec source file with the intent of
transcribing it.

Non-goals for 1.0: GPU kernels, distributed/sharded search, learned
quantization, 1-bit codes (see §11).

## 2. The pipeline

Per vector *v* of dimension *d*, in `src/vector/quantize.c`:

| step | operation | why |
|---|---|---|
| 1 | `norm = ‖v‖`, `u = v/norm` | separate magnitude from direction |
| 2 | `x = R(u)` | deterministic rotation, §3 |
| 3 | `x' = (x − shift) ⊙ cscale` | optional TQ+ calibration, §5 |
| 4 | `code[j] = argminc \|x'[j] − C[c]\|` | Lloyd–Max scalar quantize, §4 |
| 5 | bit-pack `code[]` at `bits` bits/coord | §7 |
| 6 | `scale = norm / ⟨x, dequant(code)⟩` | renormalization |

**Step 6 is the trick.** Scalar quantization systematically shrinks a vector
toward the origin, so `⟨q, dequant(code)⟩` underestimates `⟨q, u⟩`. Dividing by
the projection `⟨x, x̂⟩` forces the reconstruction's component along the true
direction to equal `norm` exactly. The result is an *unbiased* compressed-domain
inner-product estimator, which is what removes the need for a float32 rerank pass
at moderate *k*.

This is asserted, not assumed: `test/hegel/test_quantize.c` property P6 checks
both that `⟨v, rec⟩ ≈ ‖v‖²` per vector and that the mean signed error over random
query directions is small relative to the mean `|⟨q,v⟩|`. If P6 ever fails, this
paragraph is wrong and the rerank sidecar (§11) stops being optional.

Borrowed from RaBitQ's length-renormalization step, adapted to a Lloyd–Max
codebook rather than a sign code.

## 3. The rotation

One round, in order:

1. Global Fisher–Yates permutation of all *d* coordinates.
2. Per-coordinate sign flip.
3. Normalized Walsh–Hadamard transform (×1/√B) on each contiguous block of
   `B = ` largest power of two dividing *d*, clamped to [8, 1024].

`WEAVE_ROT_ROUNDS = 2`. Both the permutation and the signs come from a
ChaCha8 stream seeded by 32 frozen bytes and the pair `(round, d)`, so the whole
transform is a pure function of *d*.

**The permutation must come before the Hadamard.** Real embeddings are often
energy-ordered — Matryoshka/MRL models put the highest-variance coordinates first
by construction — so a block-local transform over contiguous coordinates would
mix correlated high-energy dimensions with each other and leave the tail nearly
untouched. `test_quantize.c` asserts this directly: it feeds a geometrically
energy-decaying vector (ratio e⁸ ≈ 2981 between the first and last eighth) and
requires the post-rotation ratio to be within 10×. Get the order backwards and
that test fails while everything else passes.

**Determinism is part of the wire format.** Encoding the same vector must yield
byte-identical codes on x86-64 and aarch64, under any thread count, forever —
otherwise a row inserted on one machine and queried on another gives different
answers. This is why the transform uses only `+`, `−`, and a single trailing
multiply by a precomputed reciprocal, in a fixed reduction order, with **no FMA
and no BLAS**. turbovec reached this design by abandoning a QR/Householder
rotation whose output depended on `RAYON_NUM_THREADS` and the host libm; the
change also deleted a 42 MB OpenBLAS dependency. Every SIMD variant in
`src/vector/kernels.c` must reproduce the scalar path bit-for-bit, and task V2's
gate is a committed fixture hash that must match on both architectures in CI.

Two bugs found while implementing this, both worth knowing about:

- The Fisher–Yates rejection threshold `2³² − (2³² mod bound)` must be computed
  and compared in 64 bits. In `uint32` it truncates to 0 whenever `bound`
  divides 2³², turning the rejection loop into an infinite loop — which every
  power-of-two dimension triggers on the first step.
- Adaptive Simpson with a depth-40 cap is exponential, not adaptive. See §4.

## 4. The codebook

A coordinate of a uniformly random unit vector in ℝᵈ has density proportional to
`(1 − x²)^((d−3)/2)` on [−1, 1] — a symmetric Beta with both shapes `(d−1)/2`.
Because §3 makes the coordinates of *any* input look like that, the optimal
scalar quantizer is a pure function of `(bits, d)` and needs **no training
data**. There is no k-means, no codebook page, and no build-time sample: two
indexes over unrelated corpora with the same `(bits, d)` have bit-identical
codebooks. That is what "data-oblivious" buys.

Solved by Lloyd–Max — alternate (centroid = conditional mean of its cell,
boundary = midpoint of neighbouring centroids) — with conditional means from
adaptive Simpson quadrature against the Beta density, ≤ 200 iterations, memoized
per `(bits, d)` in an 8-slot process-local table. The memo needs no locking: the
result is a pure function of the key, so a racing writer can only store the same
bytes.

Two implementation notes that are the difference between a correct codebook and a
plausible one:

- **Clip the integration domain.** The density concentrates in `|x| ≲ 1/√d`; at
  d = 1024 the exponent is 510.5 and `(1−x²)^510.5` is zero beyond |x| > 0.4.
  Adaptive Simpson seeded on [−1, 1] with three points sees `f(−1)=f(1)=0`,
  `f(0)=1`, and a smooth-looking estimate — it misses the spike. So clip to where
  the log-density exceeds −80 and treat the rest as exactly zero. Without this,
  `absmax` comes out near 0.5 for every dimension, which looks fine in isolation;
  `test_quantize.c` catches it by requiring `absmax < 6/√d`.
- **Cap the recursion depth at 14, not 40.** With a tolerance tight enough to
  resolve the spike, depth 40 is 2⁴⁰ evaluations and the solve never returns.

Bit widths 2–4. Fixed-rate scalar quantization of a smooth source is within
roughly 2.7× of the Shannon rate–distortion bound; that factor is **cited from
the literature, not measured here.**

## 5. TQ+ calibration

At finite *d* the empirical coordinate distribution drifts from the asymptotic
Beta, worst at low *d* (turbovec names GloVe d = 200 as the hardest regime).
TQ+ fits a per-coordinate affine `(shift, cscale)` carrying the empirical high
quantile onto the outermost centroid. The anchor probability is read off the
codebook (`1 − 0.5/nlevels`, ≈ 0.94 at 2 bits, ≈ 0.97 at 4) rather than
hardcoded, because one hardcoded value mis-calibrates the other widths.

`cscale` is clamped to [0.05, 20] and validated on load as well as on fit — the
on-disk bytes are not trusted. An uncalibrated index is far better than one whose
dequantization overflows.

Opt-in via the `calibrate` reloption. Off by default.

## 6. The block bound — the section that decided the design

The fused scorer needs a per-block upper bound satisfying contracts (C2) and
(C3) of `include/weave/channel.h`. Three formulations are provably correct. Only
one prunes anything. This was **measured before implementing the scorer**, and it
changed the design twice.

Full harness and numbers: `bench/bound_pruning.c` and
`bench/RESULTS_BOUND_PRUNING.md`. At d = 256, 4 bits, k = 10, 8192 vectors, 200
realistic queries, fraction of blocks skipped without scoring any lane:

| bound | coherent warp | random warp |
|---|---:|---:|
| (B1) per-coordinate LUT max, `smax·Σⱼ maxc(qⱼ·C[c])` | 0.0 % | 0.0 % |
| (B2) Cauchy–Schwarz, `maxrecnorm·‖q‖₂` | 0.2 % | 0.0 % |
| (B3) centroid + radius, `⟨q,c⟩ + ‖q‖₂·R` | **99.6 %** | 0.0 % |

**(B1) was the original spec and it is worthless.** It is the direct analogue of
block-max WAND's `max_tf`, which is why it was the obvious choice. But codebook
symmetry makes it equal `absmax·‖q‖₁`, and `‖q‖₁ ≈ √d·‖q‖₂`, so it is looser than
plain Cauchy–Schwarz by a factor of order √d — 16× at d = 256 — before any
data-dependent slack. An estimate 8–20× above the value it bounds never falls
below a threshold set by real neighbours.

**(B3) is the bound.** With centroid *c* and radius `R = maxₛ‖rₛ − c‖`,

```
⟨q, rₛ⟩ = ⟨q, c⟩ + ⟨q, rₛ − c⟩ ≤ ⟨q, c⟩ + ‖q‖₂·R
```

`⟨q,c⟩` carries the query-dependent signal; `‖q‖₂·R` is a correction that is
small when the block is tight. For L2, with `minnorm = minₛ‖v‖`:

```
−‖q − v‖² ≤ −‖q‖² + 2(⟨q,c⟩ + ‖q‖₂·R) − minnorm²
```

Cost: the centroid stored as a quantized code plus a 4-byte radius — 132 bytes
against 4096 bytes of codes at d = 256 / 4 bits, **3.2 % overhead for a 250×
pruning improvement**. `⟨q,c⟩` is one lookup-table gather per block, 1/32 of the
work of scoring the block it may skip. `weave_block_bound_ip()` takes the min of
all three since (B1) and (B2) cost one comparison each and can win on a
degenerate block (one live lane ⇒ R = 0 ⇒ (B3) exact; a badly-quantized centroid
⇒ (B2) tighter).

**Subtlety that is load-bearing:** R must be measured against the centroid **as
reconstructed from its stored code**, not against the exact float centroid. The
reader only has the code. Using the exact centroid makes R too small and the
bound unsound. `test/hegel/test_quantize.c` does it correctly and says why.

**And the consequence that is easy to miss:** (B3) with a random warp order
prunes 0.0 %, identically to the useless bounds. R is small only when block
members are spatially near each other. An implementation that writes codes in
heap order passes every correctness test, satisfies (C2), returns right answers —
and silently degrades the fused scorer to a full scan. Hence task **V13**: assign
warp positions in the order the IVF build's k-means clustering produces. The
partition is computed anyway for the out-of-core pass; it just has to be used.

## 7. Storage

Page kinds, from the allocation table in `doc/specs/SEGMENT_FORMAT.md` (bits
0–9 belong to the lexical channel; 14–17 to fuzzy; 18–19 to docvalues/cgram):

| bit | kind | contents |
|---|---|---|
| 10 | `WEAVE_VMETA` | `WeaveVecMeta`: dim, bits, metric, pack layout, block directory root, calibration pointer, calibration sample size and date |
| 11 | `WEAVE_VCODES` | 32-lane code blocks, each preceded by `WeaveVecBlockHdr` |
| 12 | `WEAVE_VGRAPH` | IVF centroids + cluster directory; optionally a centroid graph (`include/weave/graph.h`, §8a) |
| 13 | `WEAVE_VRERANK` | optional full-precision sidecar for `recall=exact` |

Two pack layouts, recorded in `WeaveVecMeta` because a reader that guesses wrong
returns wrong distances rather than an error: `WEAVE_PACK_LANE`
(coordinate-major, what byte-LUT kernels want) and `WEAVE_PACK_VECMAJOR`
(vector-major, what int8-dot kernels want).

Adding a vector weft bumps the segment format to **v5**, because a bolt must
self-describe which wefts it carries — an index built without a vector channel
must cost literally zero vector bytes, not empty structures. `WeaveSegMeta`
gains a channel-descriptor pointer, which changes `segs[]` stride, so v4
metapages are read through a versioned reader. The codebase already does this
for v3 (`WeaveMetaPageDataV3`); follow that pattern.

## 8. SIMD kernels

Dispatch resolved once at `_PG_init` into a function-pointer table, the same
shape PostgreSQL itself uses for `pg_popcount` and CRC32C (`src/port/`).
Following core's pattern rather than inventing one keeps "which path ran?"
answerable from `weave_vec_kernel_name()` in a bug report.

| ISA | strategy | notes |
|---|---|---|
| scalar | reference | the oracle; every other path must match it |
| SSE2 | byte LUT | baseline x86-64 |
| AVX2 | byte LUT, `perm0`-interleaved lanes | one shuffle crosses the 128-bit lane boundary |
| AVX-512BW | byte LUT | wider accumulate |
| AVX-512 VNNI | int8 dot | vector-major layout |
| NEON | byte LUT | baseline aarch64 |
| NEON SDOT / I8MM SMMLA | int8 dot | vector-major; credited with turbovec's 3.4–3.7× ARM speedup |

Nibble-split byte-LUT wins at low bit widths and small *d*; int8 dot wins when
the hardware has a dot-product instruction and *d* is large. Do not guess —
`bench/kernels.c` should A/B them per host, and `weave_vec_kernel` exists to
force a path when reproducing a bug report from a different machine.

**Two data points from pg_turbovec v2.7.0, so we do not repeat the work.** They
made a wide-word Hamming kernel **~4.4× faster** at embedding dimensions
(100k×768-d: 6.13 → 1.28 ms; 1M×768-d: 59.0 → 13.7 ms, independently reproduced at
4.5–5.4× on a second machine) — and then **measured AVX2 and declined it**, because
the scalar wide-word form already extracts the available instruction-level
parallelism. If a Hamming path is ever added here, start wide-word scalar and do
not assume a vector ISA helps.

**Rotation kernels are held to a stricter standard than scoring kernels:** a
scoring kernel that is one ULP off changes a score slightly, but a rotation
kernel that is one ULP off changes a *code*, and therefore what the index
contains. Bit-identical or rejected.

## 8a. Coarse quantization (IVF), not a proximity graph

**A corrected plan.** Earlier drafts of this document, `include/weave/graph.h`, and
task V9 all specified a **Vamana graph over the quantized codes** as the route past
the flat scan's measured 490x loss to pgvector HNSW. That is withdrawn, on the
strength of the source project's own matched-recall data rather than on reasoning.

pg_turbovec added exactly that structure in v1.23.0 for exactly that reason, and
**deprecated it in v2.5.0**. GIST-10M, 960-d, at **R@10 >= 0.98**:

| kind | p50 | qps@8 | reaches R@10 0.98? |
|---|---:|---:|---|
| **IVF** | **28.4 ms** | **161** | yes |
| flat | 34.2 ms | 31 | yes |
| graph | — | — | **no — ceiling 0.873 at 181 ms** |

The graph did not merely lose; it **never reached the recall target at any
latency**. Its apparent sublinearity held only at *iso-beam*: p50 improved 1.11x
for a 10x larger corpus while recall fell 0.605 to 0.472. At iso-recall the curves
diverge rather than cross. It was additionally 57-90x slower to build, larger on
disk, and had no out-of-core path. Their conclusion: *"It is IVF, not the graph,
that beats flat's O(n) wall."*

So the sublinear path here is an **IVF coarse quantizer**: k-means over a sample,
a per-cluster centroid, probe the `nprobe` nearest clusters, scan only their code
blocks. It composes better with what already exists in this channel than a graph
does:

- The 32-lane code blocks already carry `(centroid, radius)` bounds (§6). IVF makes
  those blocks **cluster-aligned**, which is the *same* requirement as task V13's
  warp ordering — one mechanism satisfies both, where a graph needed V13 as a
  separate, easily-forgotten constraint.
- A filter bitmap prunes whole clusters before any distance is computed, which is
  strictly cheaper than steering a traversal (§9).
- Out-of-core build is a sample k-means plus one assignment pass, rather than a
  partitioned graph construction.

**What is retained, and the distinction that matters.** Deprecating the graph
*kind* is not deprecating graph *techniques*. pg_turbovec kept its `coarse_graph`,
which navigates **centroids** rather than vectors, and notes that IVF's win partly
rests on it. A graph over a few thousand centroids is a different structure with a
different cost profile from a graph over a million vectors.
`include/weave/graph.h` is retained for that, and its header records the change so
nobody implements the withdrawn version from a stale comment.

**A second warning from the same release, folded into V9's gate.** pg_turbovec's
partitioned graph build coupled shard count to thread count, and shards cost
recall: GIST-1M R@10 fell **0.920 at P=4 to 0.605 at P=83**. A "60x parallel build
speedup" survived review because the parity test used 2.5k rows per shard at dim 64
where real workloads have ~12k at 960-d. Any partitioned build here must therefore
carry a **recall floor** in its gate, not merely a build-time number, and its
fixture must use realistic shard sizes. Ours does not exist yet, which is the
cheapest possible moment to learn that.

## 9. Filtering makes queries faster

Two mechanisms, both from `include/weave/channel.h`:

- `score_block()` takes a warp-indexed `allow` bitmap. A block whose `livemask`
  and `allow` do not intersect is skipped entirely — one AND and a branch, before
  any code is touched.
- `set_visit_filter()` hands the graph traversal the same bitmap *before* the
  descent starts, so the greedy search is steered into the surviving region
  rather than post-filtered.

That is the difference between a selective predicate making the query faster and
making recall collapse. The subtlety in the graph case: an excluded node must
still be **expanded**, just never **admitted**. Refusing to expand excluded nodes
is the classic mistake and it is precisely what makes filtered ANN recall fall
off a cliff — the excluded node may be the only bridge to an included region.

## 10. Durability

Every page mutation goes through `GenericXLogStart`/`GenericXLogFinish`. We do
**not** bypass PostgreSQL's WAL, we do not register a custom rmgr, and we do not
write outside the relation.

turbovec's `io_v7` commit protocol — alternating header slots with a delta digest
so a torn write is *detected* rather than merely prevented — is a good design for
a standalone file, and the idea that matters here is the *detection* discipline,
not the mechanism. Inside PostgreSQL, full-page WAL images already give us
atomicity; what we borrow is the habit of storing a digest of what a mutation
touched so `weave_check()` can find a block whose header no longer matches its
lanes. That is task V11 and its gate is a torn-write injection TAP test.

## 11. Honest limits

1. **Recall × latency × storage: pick two.** pg_turbovec measured recall 1.000 at
   2552 ms; pgvector HNSW measured 0.96 at 5.2 ms on the same 1M × 1024-d corpus.
   IVF (§8a) is the route to a sublinear point on that frontier — **28.4 ms at
   R@10 ≥ 0.98 on GIST-10M/960-d**, where a graph could not reach 0.98 at any
   latency. `weave.vec_recall = exact` always costs a scan.
2. **TQ+ calibration is a manual, one-shot fit with no drift detection.** If the
   corpus distribution moves away from the sample, recall degrades silently.
   Mitigation is weak: `WeaveVecMeta` records the sample size and fit timestamp so
   `weave_check()` can at least report staleness.
3. **Bit widths 2–4 only.** 1-bit is deliberately excluded, and pg_turbovec's
   v2.6.0/v2.7.0 sign-BQ work (2026-09-08) supports that while adding three
   constraints worth adopting now rather than rediscovering:

   - **Raw sign-BQ fails outright on some corpora**: they measured
     **GIST R@10 = 0.0** until the per-dimension corpus mean was subtracted before
     taking the sign. So 1-bit is not "4-bit but smaller"; it needs its own
     centering step and its own correctness argument.
   - **BQ cells must live in the RAW L2-normalised space, not the rotated space.**
     A sign code is the sign of a component, and probing rotated-space centroids
     with un-rotated codes (or the reverse) "probes the wrong cells and collapses
     recall". This is a direct constraint on §8a: our IVF clusters the warp, and
     §3 rotates before quantizing, so **if 1-bit is ever added the IVF centroids
     for it must be trained un-rotated** — a different structure from the
     TurboQuant centroids, not a shared one. Worth writing down before V9 rather
     than after.
   - **No recall/latency/QPS number exists for BQ yet.** Their own docs say it
     "still needs a real-corpus run". Their 1-bit is correctness-proven, not
     performance-proven, so it is not evidence for adopting it here.
4. **No near-lossless mode without the rerank sidecar**, which costs the storage
   win it exists to preserve.
5. **L1 distance has no useful compressed-domain bound** — the quantizer is built
   around inner products. Supported exact-only, costed as a full scan, graph
   unused.
6. **§6's measurement is d = 256, one synthetic distribution, one cluster
   tightness.** It must be repeated on Cohere-wiki 1024-d and GloVe 200-d before
   Phase V is done. GloVe especially: it is the documented worst case for the
   asymptotic-Beta assumption the whole codebook rests on.

## 12. Test plan

| gate | test | status |
|---|---|---|
| V2 rotation determinism | `test/hegel/test_quantize.c` P1–P3 + committed cross-arch fixture hash | properties pass; fixture owed |
| V3 codebook | `test_quantize.c` P4 (sorted, symmetric, `absmax < 6/√d`) + fixture | passing |
| V4 encode round-trip and unbiasedness | `test_quantize.c` P5, P6 | passing |
| V5 packing | `test/hegel/test_pack.c`: round-trip, lane isolation, `move_lane`/`zero_lane`, guard-byte bounds | passing, 1,909,440 checks |
| V6 kernel equivalence | `test/hegel/test_kernels.c`: every ISA path == scalar | not started |
| V7 crash safety | extend `t/001_crash_recovery.pl` to a vector index | not started |
| V8 shuttle contract | `test_quantize.c` P8 (C2 soundness) + `bench/bound_pruning.c` soundness assert | passing, 17741 checks |
| V9 graph recall/latency/storage | `bench/RESULTS_VECTOR.md` | not started |
| V13 warp ordering | `bench/bound_pruning.c` ≥ 90 % blocks pruned | harness exists, ordering not built |
| V14 block header maintenance | `weave_check()` recompute-and-compare | not started |

`test/hegel/test_quantize.c` currently runs 17,741 checks with 0 failures across
7 dimensions × 3 bit widths, linking `src/vector/quantize.c` and
`src/vector/pack.c` with no backend. That standalone linkability is the reason
`include/weave/quantize.h` is written the way it is; `include/weave/for.h` is the
same pattern and states the same intent.
