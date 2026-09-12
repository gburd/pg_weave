# Specification: the vector channel

Status: **codec implemented and property-tested; scoring kernels partially
implemented (three exact paths, §8); storage and IVF unimplemented.** Tasks
**V1**–**V14** in `doc/PHASES.md`.

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
inner-product estimator.

This is asserted, not assumed: `test/hegel/test_quantize.c` property P6 checks
both that `⟨v, rec⟩ ≈ ‖v‖²` per vector and that the mean signed error over random
query directions is small relative to the mean `|⟨q,v⟩|`.

### 2.1 What unbiasedness does not buy — measured 2026-09-10

This section used to end "...which is what removes the need for a float32 rerank
pass at moderate *k*", and it said that if P6 ever failed, the rerank sidecar
"stops being optional". **P6 passes and the claim was still false.** That is the
most useful thing in this section, so it is stated before the numbers:
unbiasedness constrains the *mean* signed error, while recall@10 depends on the
*ranking* under per-vector error. An unbiased estimator with nonzero variance
still permutes a top-10 list. P6 was not a weak test of the right property; it was
a correct test of a property that was never sufficient.

`bench/RESULTS_IVF_RECALL.md` measured recall@10 at **full probe** — every cluster
probed, so probe-miss error is exactly zero and what remains is quantization
error alone. Full probe is the ceiling over all `nprobe`, so these are upper
bounds on any IVF configuration:

| corpus | 2 bits | 3 bits | 4 bits |
|---|---:|---:|---:|
| GloVe 6B, 200-d, 200k vectors | 0.7345 | 0.8515 | 0.9205 |
| TEXMEX GIST-1M, 960-d, 100k vectors | 0.6130 | 0.7880 | 0.8780 |

Phase V's gate is `recall@10 >= 0.99`. **It is unreachable in the
compressed-domain-only configuration at any probe count and any bit width tested,
on two real corpora at their native dimensionality.** A rerank window of 100 over
full-precision vectors closed the gap on both (window 1000 at 2 bits).

Three consequences, all load-bearing:

1. **`WEAVE_VRERANK` is required, not optional.** Task V10's "optional
   full-precision rerank sidecar" is a 0.99 prerequisite. §12's page table is
   updated accordingly.
2. **The recall gate and the storage gate are in tension and may be jointly
   unsatisfiable.** A full-coverage float32 sidecar costs `4 * dim` bytes per
   vector — 4,096 at 1024-d — which is by itself about what pgvector HNSW spends
   on the vector it stores. Phase V's `size <= 0.15× pgvector HNSW` assumed the
   codes were the whole index. They are not, if 0.99 is required. This needs
   measuring against a real pgvector HNSW index before either gate is trusted.
3. **TQ+ affine calibration is not the missing fix.** It moved recall the *wrong*
   way at 3 of 4 measured points (GloVe 4-bit 0.9205 → 0.8620; GIST 4-bit 0.8780
   → 0.6850).

What this does **not** show: these are 200k × 200-d and 100k × 960-d, not the
gate's 1M × 1024-d Cohere-wiki, and k=10 cosine only. The *quantization* ceiling
is the robust half — it is a property of the codebook and the estimator, measured
with probe-miss eliminated by construction, and a better partition cannot raise
it. The probe-miss half of the same measurement is pessimistically biased, because
the harness k-means is deliberately crude.

Borrowed from RaBitQ's length-renormalization step, adapted to a Lloyd–Max
codebook rather than a sign code.

#### 2.1.1 What the storage budget then forces — derived 2026-09-11

The maintainer's resolution of the reopened Phase V gate (see `doc/PHASES.md`) is
to keep both `recall@10 ≥ 0.99` and `size ≤ 0.15× pgvector HNSW` and find a rerank
representation cheaper than float32. Two steps of arithmetic narrow that to one
shape before any benchmark runs.

**A rerank representation cannot lift recall above its own ceiling.** Reranking a
top-*W* window with a *b*-bit representation produces the *b*-bit ranking of that
window. The numbers above are therefore not just a statement about a scan — they
are a statement about reranking: a 4-bit rerank tops out at the same 0.9205 and
0.8780. So the minimum viable *b* is the smallest whose full-probe recall@10
reaches 0.99, and the size budget has to cover *b*, not *b* plus a scan width.

**The budget is about 6.68 bits per coordinate.** At 1M × 1024-d, HNSW spends
4,096 B on the vector plus graph links, on the order of 5,700 B per vector at
`m = 16` (unmeasured on this corpus — measure before quoting it). 0.15× is ≈ 855 B
per vector, which at 1024-d is 6.68 bits per coordinate for everything on disk.
An 8-bit sidecar alone is 1,024 B (0.18×) and 4-bit codes plus that sidecar are
1,536 B (**0.27×**), so the codes-plus-sidecar shape is refuted by arithmetic.

What survives is **one code width serving both the scan and the final ranking,
with `b ≤ 6`** — which would collapse V10's `WEAVE_PK_VRERANK` sidecar into a
wider code rather than a second structure. Whether such a *b* exists is the next
measurement: sweep *b* = 5, 6, 7, 8 at full probe on both corpora and report the
smallest reaching 0.99. Extrapolating the 2/3/4-bit points suggests ~7 for
GloVe-200d and possibly unreachable at 8 for GIST-960d, which would fail the
storage claim — but extrapolation is not measurement, and the extrapolation is
exactly why the sweep is worth its cost rather than a formality.

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

The stationarity conditions are Lloyd–Max: centroid = conditional mean of its
cell, boundary = midpoint of neighbouring centroids, with conditional means from
adaptive Simpson quadrature against the Beta density. Memoized per `(bits, d)` in
an 8-slot process-local table. The memo needs no locking: the result is a pure
function of the key, so a racing writer can only store the same bytes. Nothing
about the codebook is on disk — it is derived from `(bits, d)`, both already
recorded in the segment — so a solver change needs no format version bump.

Four implementation notes that are the difference between a correct codebook and a
plausible one. The first two were known; the last two were found on 2026-09-11
when the width ceiling was raised from 4 to 8, and each of them **silently
degraded the wide codebooks while every existing assertion passed**:

- **Clip the integration domain.** The density concentrates in `|x| ≲ 1/√d`; at
  d = 1024 the exponent is 510.5 and `(1−x²)^510.5` is zero beyond |x| > 0.4.
  Adaptive Simpson seeded on [−1, 1] with three points sees `f(−1)=f(1)=0`,
  `f(0)=1`, and a smooth-looking estimate — it misses the spike. So clip to where
  the log-density exceeds −80 and treat the rest as exactly zero.
- **Cap the recursion depth at 14, not 40.** With a tolerance tight enough to
  resolve the spike, depth 40 is 2⁴⁰ evaluations and the solve never returns.
- **Seed the centroids at the Beta's quantiles, not uniformly over the clipped
  domain.** The clipped domain depends on the distribution shape but *not* on the
  level count: at d = 1536 it is 12.3 σ wide, because −80 log-density is all it
  means. At 4–16 levels every uniform cell still catches mass. At 64–256 levels
  the outer cells land in the dead tail, their mass underflows, and the solver's
  empty-cell guard — which exists to keep the ladder sorted for the branchless
  quantizer — pins them exactly where they were seeded, forever:

  Counting a level **unreachable** when its cell holds under 1e−6 of the fair
  share 1/n — so under one coordinate in 1e6/n would ever select it — at d = 1536:

  | bits | levels | unreachable | under 1 % of fair share | smallest cell mass, ×(1/n) |
  |---:|---:|---:|---:|---:|
  | 4 | 16 | 0 | 0 | 1.3e−01 |
  | 5 | 32 | 0 | 0 | 1.9e−02 |
  | 6 | 64 | 2 | 20 | 8.5e−09 |
  | 7 | 128 | 42 (33 %) | 72 | 4.1e−14 |
  | 8 | 256 | 122 (48 %) | 168 (66 %) | 5.4e−25 |

  A level in dead space is a level no coordinate ever maps to, so an "8-bit" code
  carried under 6.5 bits of real resolution, and `absmax` reported 10.6 σ where
  the true Lloyd–Max outermost centroid is 4.59 σ. An equiprobable seed gives
  every initial cell mass ≈ 1/n at every *n* and every *d*, which removes the
  dependence on the domain being level-count-aware. After the fix: **zero
  unreachable levels at every `(bits, d)`**, and smallest cell mass 1.4e−03 ×(1/n)
  at 8 bits — the outermost cell of a correct Lloyd–Max quantizer, whose share the
  companding law asserted by `test_quantize.c` P4d independently predicts at
  1.6e−03.

- **Solve the conditions by Newton, not by Lloyd's alternating iteration.**
  Lloyd's map propagates a correction one cell per sweep along a chain of *n*
  cells, so it needs O(n²) sweeps: measured 54 at n = 4, 700 at n = 16, 10k at
  n = 64, 130k at n = 256. The 200-sweep cap therefore meant the solver had
  **never converged at any width**, and the shortfall grows with *n* — at 8 bits
  the 200-sweep answer put the outermost centroid at 3.86 σ against a true 4.59 σ.
  Over-relaxation (ω ≤ 1.9) buys 1.4×; Gauss–Seidel is 3× *worse*. But the cell
  depends only on `c[i−1], c[i], c[i+1]`, so the Jacobian of
  `F_i(c) = E[X | cell_i] − c_i` is **tridiagonal** and one Newton step is a
  Thomas solve: 3–7 iterations at every `(bits, d)`, reaching the same fixed
  point as 130k Lloyd sweeps.

Bit widths 2–8. Fixed-rate scalar quantization of a smooth source is within
roughly 2.7× of the Shannon rate–distortion bound; that factor is **cited from
the literature, not measured here.** What *is* measured here, by
`test_quantize.c`, is that the mean squared quantization error falls by
0.252–0.294× per added bit across 2–8 bits and 64–1536 d, against the 0.25×
high-resolution theory predicts — which is the end-to-end evidence that each
added bit is real.

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
| 13 | `WEAVE_VRERANK` | full-precision sidecar. **Required for `recall@10 >= 0.99`, not just for `recall=exact`** — §2.1 measured the compressed-domain ceiling at 0.9205/0.8780 |

Two pack layouts, recorded in `WeaveVecMeta` because a reader that guesses wrong
returns wrong distances rather than an error: `WEAVE_PACK_LANE`
(coordinate-major, what byte-LUT kernels want) and `WEAVE_PACK_VECMAJOR`
(vector-major, what int8-dot kernels want). The layout is a *parameter* of every
scoring entry point, never an assumption — see §8.

**A note for V7, which owns the on-disk image.** `weave_block_codebytes()` returns
`ceil(dim·bits/8) · 32`, which is 0–28 bytes more than the tight `4·dim·bits` a
`WEAVE_PACK_LANE` block actually occupies (the per-vector `ceil` is rounded up 32
times instead of once). Those slack bytes are **never written** by
`weave_pack_lane()` and never read by any kernel — correctness is unaffected, and
`test/hegel/test_kernels.c` `memset`s them so its blocks are deterministic. But a
page image containing them is nondeterministic unless the writer zeroes them, which
matters for two things V7 introduces: a `GenericXLog` delta over a page whose
uninitialized tail changes between rewrites, and any cross-architecture fixture
hash of a `WEAVE_VCODES` page. **The block writer must zero the block buffer before
packing into it.** V6 has no writer, so this is recorded and not fixed.

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

The ISA matrix as originally drafted, and what V6 actually shipped:

| ISA | strategy | status |
|---|---|---|
| scalar | reference | **implemented** (`scalar`); the oracle, and the only path that reads codes through the pack API rather than assuming a layout |
| portable wide-word | exact float LUT | **implemented** (`lut-wide`); not in the original table, and it is the baseline every vector path must beat |
| AVX2 | exact float LUT: `vpsrlvd` + `vpgatherdps` + `vcvtps2pd` | **implemented** (`lut-avx2`), verified on x86-64 |
| SSE2 | byte LUT | **not implemented** — see below |
| AVX-512BW | byte LUT | not implemented; no AVX-512 host or emulator available to verify on |
| AVX-512 VNNI | int8 dot | not implemented — approximate, see below |
| NEON | byte LUT | not implemented — see below; aarch64 runs `lut-wide` |
| NEON SDOT / I8MM SMMLA | int8 dot | not implemented — approximate, see below |

**The gate and the strategy table were in conflict, and the gate won.** V6's gate
is "every ISA path produces results identical to the scalar path". A nibble-split
byte-LUT or an int8-dot kernel quantizes the *query lookup table* to 8 bits before
gathering, so it is not one ULP from the scalar reference, it is one quantization
step from it — it cannot pass that gate, and it owes a recall budget and a recall
measurement that do not exist yet. Both approximate families were therefore left
unimplemented rather than shipped against a tolerance nobody had derived. That
choice is reversible; the sequence is not. Measure the error first.

**Held to exactness, scoring is gather-bound, which shortens the matrix a second
time.** The exact kernel is 32 independent table lookups per coordinate plus a
double accumulate. SSE2 has neither a gather nor a variable shift, and neither
does baseline NEON, so an exact kernel on either ISA is `lut-wide` plus register
shuffling — which is why neither was written and why aarch64 currently runs the
portable path. AVX2 is the first x86 ISA with both. This is the same shape of
conclusion pg_turbovec reached for their Hamming kernel (below): start wide-word
scalar, and do not assume a vector ISA helps.

**Bit-identity is affordable because the parallelism is across lanes, not across
coordinates.** Each lane's sum stays in strict ascending *j* order in a double,
exactly as `weave_lut_score_code()` does it, so running 8 lanes at once
reassociates nothing. `test/hegel/test_kernels.c` compares with `memcmp` and not a
tolerance (308,278 checks), which means a future kernel either matches or is
rejected — no judgement calls. A kernel that used multiple partial sums per lane,
or FMA, would forfeit that and is not worth the speed.

Also not present: the `perm0` lane interleave. It exists to let a VPSHUFB
byte-LUT gather cross the 128-bit lane boundary in one instruction; a
`vpgatherdps` kernel indexes lanes directly, so introducing one would only create
an opportunity to permute the output.

### The unmet gate, kept as an unmet gate

V6's gate as written in `doc/PHASES.md` was **the seven-ISA matrix above, both
strategies, every path identical to scalar.** Three paths ship. The rest is
**not met**, and it is recorded here rather than deleted because a gate you have
not met is precisely what `AGENTS.md` rule 8 and this document's own §6 exist to
keep visible. Restated so it can be checked rather than argued about:

| owed | what would close it | why it is open |
|---|---|---|
| SSE2, baseline NEON | an exact kernel on each, `memcmp`-identical | held to exactness there is nothing to gain: no gather, no variable shift, so the kernel *is* `lut-wide` plus shuffles. Closing this means measuring `lut-wide` against a hand-written SSE2/NEON form and keeping it only if it wins |
| AVX-512BW, AVX-512 VNNI, NEON SDOT/SMMLA | a host or emulator that runs them, plus the differential test green on it | no such host or runner is wired up here. An ISA path nobody executed is the fast-but-wrong shape rule 8 is about, so none was written rather than written and hoped for |
| nibble-split byte-LUT, int8 dot (either ISA) | **a different gate**: an error budget, a recall measurement against the exact path, and a tolerance derived from it | both quantize the query table to 8 bits, so "identical to scalar" is unreachable *by construction* — this is the one entry where the original gate is wrong rather than merely unsatisfied, and replacing it needs a measurement, not a decision |
| `bench/kernels.c` | a per-host A/B of the three verified paths | not written. `auto` therefore picks the widest ISA by convention, which for a gather-bound kernel is a weaker assumption than usual |

**And the strategy guidance the original draft carried, which still stands:**
nibble-split byte-LUT is expected to win at low bit widths and small *d*, and
int8 dot to win where the hardware has a dot-product instruction and *d* is large.
**Do not guess between them** — `bench/kernels.c` should A/B them per host, and
`pg_weave.vec_kernel` exists to force a path when reproducing a bug report from a
different machine. Nothing in this repository has measured either family, so treat
those two sentences as the hypothesis they are.

Which of the three *verified* paths is fastest on a given host is likewise **not
measured**: `bench/kernels.c` is still owed, and `pg_weave.vec_kernel`
(`auto`/`scalar`/`lut`/`dot`) exists to force a path for A/B work and to reproduce
a bug report from another machine. Since every implemented path is bit-identical,
that GUC changes speed and never answers.

### The pack layout is a parameter, never an assumption

`WeaveVecKernelOps.score_block()` and `WeaveScoreBlock` both carry a
`WeavePackLayout`, threaded from the segment's `WeaveVecMeta` (§7). This is not
tidiness. `src/vector/pack.c` states the failure mode: a reader that guesses the
layout wrong *"does not fail, it returns wrong distances."* So:

- the oracle is **layout-agnostic** — it reaches codes only through
  `weave_unpack_lane()`, so it handles both layouts today and a third one for
  free;
- `lut-wide` and `lut-avx2` assume `WEAVE_PACK_LANE`'s byte-aligned 8-lane groups
  and **decline** anything else explicitly, by delegating to the oracle rather
  than by reading the bytes some other way;
- a layout value that is neither of the two defined ones is **rejected**, because
  it arrived from a page and defaulting it would be exactly the wrong-distances
  case above.

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

**A third warning, from a later release, not yet folded into V9's gate — it
should be.** pg_turbovec's v2.7.4 measured that **probe count sets a hard
recall ceiling that a wider exact-rerank window cannot break**
(`docs/BQ_RECALL_BENCH.md` §0.6a, `bq_ivf_20260909`): 250k × 1024-d
Cohere-wiki, `bit_width = 1`, `lists = 512`, probes swept 8→128 against rerank
windows 256–2000. At every probe count, R@10 saturates and then stays flat
across every window wider than it needs:

| probes (of 512 lists) | R@10 ceiling | R@100 ceiling |
|---:|---:|---:|
| 8 | 0.846 | 0.784 |
| 16 | 0.906 | 0.852 |
| 32 | 0.954 | 0.905 |
| 64 | 0.978 | 0.933 |
| 128 | 0.984 | 0.945 |
| flat (all cells) | **0.994** | 0.948 |

No probe count up to 128/512 (25 % of `lists`) reached flat's own R@10 = 0.994,
let alone 0.99, and flat was already cheaper than IVF at this scale — their
guidance is "below ~1M, prefer flat BQ; above it, unmeasured." The mechanism is
general, not a property of 1-bit codes: **the probe count and the rerank
window fix two different failure modes.** A neighbour whose cell was never
probed cannot be recovered by reranking a wider *retrieved* set, no matter how
wide — that is retrieval-bound loss. pg_turbovec's own docs call out the
mirror-image case from an earlier release (their "Gap-B", v1.25.0): high-dim
recall loss that was **not** retrieval-bound — cell recall was already
0.98–0.996 — where a wider exact window *did* fix it. Same symptom in a naive
read (recall too low at the default window), opposite cause and opposite fix.
Diagnose which one a given recall gap is before reaching for either knob.

**This bears directly on V9 and on Phase V's `recall@10 ≥ 0.99` gate.** pg_weave's
quantizer is not 1-bit — it is the 2–4 bit Lloyd-Max codebook of §4, closer to
pg_turbovec's ordinary (non-BQ) code than to sign-BQ — so the ceiling *values*
above do not transfer. But the mechanism is unconditional on bit width: it
concerns which cells get probed, not how vectors within a probed cell are
scored, so it applies to this design's IVF exactly as written. The nearest
same-author, non-BQ data point is encouraging but is not a substitute for
measuring our own: on 500k × 1024-d Cohere-wiki (`docs/BENCHMARKS.md`,
"Recall-vs-p50 frontier", v1.11.x/Phase A-2 — an older, separate measurement,
cited here as context for V9's risk and **not** one of the two claims this
section otherwise verifies), 4-bit IVF reached R@10 = 0.990 at `probes = 256`
(36 % of `lists = 707`, p50 25.3 ms) and R@10 = 1.000 at `probes = lists`
(p50 41.4 ms — necessarily equal to flat's 41.4 ms, since probing every cell
*is* the full scan). On that same corpus, **pgvector HNSW never reached
R@10 = 0.99 at any `ef`** (`ef = 400` topped out at 0.983). If that holds on
Phase V's own 1M × 1024-d gate corpus, `p50 ≤ 2× pgvector HNSW` at
`recall@10 ≥ 0.99` is comparing against a baseline that cannot itself clear
the recall bar, which is worth resolving explicitly rather than discovering at
gate time.

Per `AGENTS.md` rule 9: **the thing V9's design rests on — that some `nprobe`
reaches `recall@10 ≥ 0.99` within the gate's latency budget, on our own
codebook and corpus geometry — has not been measured.** The 4-bit precedent
above makes it plausible, not proven. `bench/RESULTS_VECTOR.md` should carry a
probes-vs-recall-vs-p50 sweep shaped like `bq_ivf_20260909`, not a single
`(probes, recall, p50)` triple chosen after the fact to clear 0.99.

## 9. Filtering makes queries faster

Two mechanisms, both from `include/weave/channel.h`:

- `score_block()` takes a warp-indexed `allow` bitmap. A block whose `livemask`
  and `allow` do not intersect is skipped entirely — one AND and a branch, before
  any code is touched.
- `set_visit_filter()` hands the graph traversal the same bitmap *before* the
  descent starts, so the greedy search is steered into the surviving region
  rather than post-filtered.

**The bitmap travels with its length.** `score_block()` takes `nwarp` alongside
`allow`, and a block whose lanes fall outside `[0, nwarp)` is a corrupt page and
raises. `WeaveVecBlockHdr.firstwarp` is what indexes the bitmap and it comes off
disk, so a bitmap without a length is an unbounded out-of-bounds read — the case
`doc/CONVENTIONS.md` rule 2 rules out. A short tail block (a weft whose warp count
is not a multiple of 32) is *trimmed* to the warps that exist; a block that starts
outside the segment is *refused*. `test/hegel/test_kernels.c` allocates the bitmap
to exactly `nwarp` bits so that an over-read is an ASan report and not a quiet
pass.

**The granularity at which masked lanes are skipped differs per kernel, and it is
the mechanism this section's claim rests on, so it is stated rather than left to be
inferred.** The oracle skips one lane at a time. `lut-wide` and `lut-avx2` skip a
**group of 8 lanes** at a time, because the group is the unit their addressing is
built on (§8). Consequences, in order of how much they matter:

- A filter selective enough to empty whole blocks costs nothing to exploit: the
  shuttle short-circuits before a kernel is called at all. This is the case the
  channel is designed around and the one §6's bound serves.
- A filter that empties whole 8-lane groups saves the fast paths the same
  proportion of work it saves the oracle.
- A filter that is selective but *scattered* — one surviving lane per group — saves
  the fast paths nothing, while it saves the oracle 7/8. Finer masking inside a
  group is possible (gather anyway, then blend), but the gathers are the cost and
  they would still be issued, so what it saves is the adds.

Whether that last case is worth code is **not measured**: it needs
`bench/kernels.c`, which is owed. It cannot be settled by
`test/hegel/test_kernels.c`, because the output is identical either way — only the
work differs. Warp ordering (§6, task V13) is what makes surviving lanes clustered
rather than scattered, so it is load-bearing here for a second, independent reason.

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
3. **Bit widths 2–8 only.** 1-bit is deliberately excluded, and pg_turbovec's
   sign-BQ work across v2.6.0–v2.8.1 (2026-09-08 through 2026-09-10) supports
   that while adding constraints worth adopting now rather than rediscovering:

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
   - **1-bit is a high-dimension technique, and numbers now exist.** An earlier
     draft of this document said no recall/latency/QPS number existed for BQ yet
     — **that is now false and is corrected here.** pg_turbovec's v2.7.5
     dimension sweep (`docs/BQ_RECALL_BENCH.md` §0.6c, 250k rows × {256, 512,
     1024}-d Cohere-wiki, 100 held-out queries; the 1024-d arm reproduced its own
     earlier published recall **bit-identically**, which validates the harness)
     measured the rerank window their 1-bit code needs for R@10 ≥ 0.95, against
     their 2-bit code at the same dim:

     | dim | 1-bit window | 2-bit window | penalty |
     |---:|---:|---:|---:|
     | 256 | 4000 | 100 | **125×** |
     | 512 | 800 | 32 | 25× |
     | 1024 | 256 | 32 | **8×** |

     At 256-d, R@10 ≥ 0.99 needs a window of 16 000 — reranking 6.4 % of a 250k
     corpus — which they call "effectively unusable"; storage moves the same
     direction (1.902×→1.971× vs 2-bit as dim rises 256→1024, because 1-bit's
     fixed per-index overhead amortises away). Their conclusion: **prefer
     768-d and up.** Caveat they recorded and this document repeats: the
     256-d and 512-d arms are **prefix slices** of a 1024-d embedding, not
     natively-trained low-dim vectors, so the measured penalty is an *upper
     bound* on a native low-dim model's penalty, not a calibrated one.

     **This does not transfer number-for-number to a future 1-bit addition
     here, and reading it as if it did would overclaim.** pg_turbovec's 1-bit is
     a corpus-mean-centered sign code; this project's quantizer at every bit
     width, including a hypothetical 1, would be a Lloyd-Max cell over a
     *rotated* coordinate (§3, §4) — a different distribution assumption at
     every width, not just at 1 bit. What the result establishes safely is
     narrower but still useful: *some* 1-bit coordinate-wise code's rerank
     penalty is strongly dimension-dependent, which is a plausible — not
     proven — property of any 1-bit-per-coordinate code, including a rotated
     Lloyd-Max one. **It bounds the question rather than answers it: if a
     1-bit `WEAVE_BITS_MIN` is ever proposed, run the dimension sweep against
     the rotated-Beta codebook specifically (§5's calibration harness is the
     right place) before offering any `dim < 768` configuration, rather than
     assuming this number carries over.** `include/weave/quantize.h`'s comment
     above `WEAVE_BITS_MIN` records the same pointer.
   - **A composed, and separate, finding: IVF's probe count — not the rerank
     window — is what actually gates BQ's recall at scale, and that mechanism
     is not specific to 1-bit either.** v2.7.4 measured that `lists = 512`
     IVF+1-bit-BQ tops out at R@10 = 0.984 (`probes = 128`) with no window able
     to push it higher, and never reaches flat's own 0.994 within the tested
     range. §8a above has the full table and, more importantly, why this is a
     live risk for **this project's** V9 and Phase V's `recall@10 ≥ 0.99` gate
     — not a BQ-specific curiosity.

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
| V3 codebook | `test_quantize.c` P4 (sorted, symmetric, in range), P4b (nearest-neighbour boundaries), P4c (centroid condition), P4d (no dead levels), P4e (level-count-aware `absmax` ceiling), P4f (monotone distortion) + fixture | passing, 2–8 bits |
| V4 encode round-trip and unbiasedness | `test_quantize.c` P5, P6 | passing |
| V5 packing | `test/hegel/test_pack.c`: round-trip, lane isolation, `move_lane`/`zero_lane`, guard-byte bounds | passing, 1,909,440 checks |
| V6 kernel equivalence | `test/hegel/test_kernels.c`: every ISA path == scalar | passing, 308,278 checks over `scalar`, `lut-wide`, `lut-avx2`, including ⟨q, reconstruct(code)⟩ agreement (K5) and rejection of an out-of-range `firstwarp` (K4, under ASan); **the rest of the ISA matrix is unmet, tabulated as unmet in §8** |
| V7 crash safety | extend `t/001_crash_recovery.pl` to a vector index | not started |
| V8 shuttle contract | `test_quantize.c` P8 (C2 soundness) + `bench/bound_pruning.c` soundness assert | passing, 17741 checks |
| V9 IVF recall/latency/storage, **including a probes-vs-recall sweep** (§8a: probe count, not rerank window, is what a fixed-`nprobe` recall ceiling needs) | `bench/RESULTS_VECTOR.md` | not started |
| V13 warp ordering | `bench/bound_pruning.c` ≥ 90 % blocks pruned | harness exists, ordering not built |
| V14 block header maintenance | `weave_check()` recompute-and-compare | not started |

`test/hegel/test_quantize.c` currently runs 17,741 checks with 0 failures across
7 dimensions × 3 bit widths, linking `src/vector/quantize.c` and
`src/vector/pack.c` with no backend. That standalone linkability is the reason
`include/weave/quantize.h` is written the way it is; `include/weave/for.h` is the
same pattern and states the same intent.
