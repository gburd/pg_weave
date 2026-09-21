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

| corpus | 2 bits | 3 bits | 4 bits | 5 bits | 6 bits | 7 bits | 8 bits |
|---|---:|---:|---:|---:|---:|---:|---:|
| GloVe 6B, 200-d, 200k vectors | 0.7345 | 0.8515 | 0.9225 | 0.9570 | 0.9750 | 0.9860 | **0.9950** |
| TEXMEX GIST-1M, 960-d, 100k vectors | 0.6130 | 0.7880 | 0.8680 | 0.9200 | 0.9660 | 0.9780 | 0.9860 |

Widths 5–8 and the corrected 4-bit column come from
`bench/RESULTS_BITWIDTH_SWEEP.md` (2026-09-12, converged codebook). The 4-bit
figures previously read 0.9205 and 0.8780, from a codebook 200 Lloyd sweeps short
of its fixed point; correcting it moved GloVe **up** 0.0020 and GIST **down**
0.0100 — a lower-MSE codebook is not obliged to score better on a ranking metric.

Phase V's gate is `recall@10 >= 0.99`. **Only GloVe reaches it, and only at 8
bits; GIST-960d does not reach it at any supported width**, with decaying
increments (0.0180, 0.0120, 0.0080 over the last three) that put it past 8 bits.
Since 8 bits is 1,024 B/vector at 1024-d — 0.18× HNSW against a 0.15× budget — the
width that clears 0.99 on the easier corpus already misses the storage claim. A
rerank window of 100 over full-precision vectors reaches **1.0000 from 3 bits** on
both corpora, which is the shape that survives; see §2.1.1 and the results file.

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
are a statement about reranking: a 4-bit rerank tops out at the same 0.9225 and
0.8680. So the minimum viable *b* is the smallest whose full-probe recall@10
reaches 0.99, and the size budget has to cover *b*, not *b* plus a scan width.

**The budget is about 6.68 bits per coordinate.** At 1M × 1024-d, HNSW spends
4,096 B on the vector plus graph links, on the order of 5,700 B per vector at
`m = 16` (unmeasured on this corpus — measure before quoting it). 0.15× is ≈ 855 B
per vector, which at 1024-d is 6.68 bits per coordinate for everything on disk.
An 8-bit sidecar alone is 1,024 B (0.18×) and 4-bit codes plus that sidecar are
1,536 B (**0.27×**), so the codes-plus-sidecar shape is refuted by arithmetic.

What survives is **one code width serving both the scan and the final ranking,
with `b ≤ 6`** — which would collapse V10's `WEAVE_PK_VRERANK` sidecar into a
wider code rather than a second structure. **Measured 2026-09-12
(`bench/RESULTS_BITWIDTH_SWEEP.md`): no such *b* exists.** GloVe-200d needs 8 bits
(0.9950) and GIST-960d does not reach 0.99 at any supported width (0.9860 at 8
bits, with increments decaying 0.0180 / 0.0120 / 0.0080). 8 bits is 1,024 B at
1024-d = 0.18×, so the width that clears the easier corpus already misses the
budget. The extrapolation above said ~7 for GloVe and "possibly unreachable at 8"
for GIST: optimistic by one width on GloVe, right about GIST. Close enough to have
been tempting, and wrong enough to have justified the sweep.

**What survives instead: b-bit codes plus an exact float32 rerank of a top-w
window, read from the heap.** That does not violate the rule above, because the
rerank is done at float32 rather than in a *b*-bit representation — so the ceiling
that binds is float32's, which is 1.0. But it does mean the rerank data cannot be a
cheap quantized sidecar; it has to be full precision, and a stored float32 sidecar
is `4 * dim` = 4,096 B/vector, worse than HNSW. The only shape in which both claims
survive therefore reads full precision from the **heap**, where the original vector
already lives and the index pays nothing for it.

**Both `b` and `w` were then measured, at the gate's own corpus size, and the
answer is not the narrowest width.** `bench/RESULTS_BITWIDTH_SWEEP.md` swept the
window as well as the width; `bench/RESULTS_PHASE_V_COLD.md` re-measured the window
at n = 1M and measured the HNSW denominator instead of estimating it. Windows are at
**n = 1M on GIST-960d**, index bytes at 1024-d:

| bits | window @ 0.99 | index B/vector | × HNSW | SIMD kernel |
|---|---|---:|---:|---|
| 3 | 50 | 384 | 0.048 | yes |
| **4** | **25** | **512** | **0.064** | **yes** |
| 5 | 20 | 640 | 0.079 | no |
| 6 | ~20 | 768 | 0.095 | no |
| 8 | — (0.9860 ceiling) | 1024 | 0.127 | no |

**The recommended shape is 4 bits with a top-25 window:** recall@10 **0.9920** at
n = 1M, index **512 B/vector = 0.064× HNSW**, and the widest width that keeps the
SIMD code-scan kernel (§9: 5–8 bits fall back to the scalar oracle). The code scan
touches every vector while the rerank touches twenty-five, so giving up vectorized
scoring to save five candidates is the wrong trade. Two bits is off the frontier
entirely: it misses 0.99 even at window 75.

**The window does grow with n, by about +25% per decade, uniformly across widths.**
The concern was that a window measured at n = 100k–200k is a lower bound for the
gate corpus, since ten times the vectors put ten times more near-neighbours in
range to displace the true top-10. Measured: 40 → 50 at 3 bits, 20 → 25 at 4,
15 → 20 at 5. Real, modest, and it does not reorder the frontier. Two controls make
it attributable to n rather than to the machine: the same binary reproduced the
n = 100k row exactly on the same host, and the n = 1M 3-bit row reproduced
**bit-identically on a second machine with `lists=1` instead of `lists=1024`** — a
direct confirmation that the full-probe column is partition-independent, and the
reason `lists=1 probes=1` is now the cheap way to measure a ceiling.

**The 0.15× budget is looser than every earlier figure assumed, because the
denominator was a guess.** This section used to price it from "on the order of
5,700 B per vector at m = 16 (unmeasured on this corpus — measure before quoting
it)", and it was then quoted repeatedly. Measured on 999,990 × 960-d at m = 16,
ef_construction = 64: **8,056 B/vector** (7,683 MB index). So 0.15× is
~1,208 B/vector, not ~855.

**That overturns a stated conclusion.** The bit-width sweep argued that 8 bits at
1,024 B/vector is "0.18× HNSW against a 0.15× budget, so the width that clears 0.99
on the easier corpus already misses the storage claim". Against the measured
denominator, 8 bits is **0.127×** and fits comfortably. The single-width conclusion
survives, but on **recall** rather than storage — 8 bits reaches only 0.9860 on
GIST at full probe. Any "jointly unsatisfiable" argument that leaned on the storage
half was leaning on an estimate.

**And the baseline has no operating point at the recall the gate names.** pgvector
HNSW at m = 16, ef_construction = 64 on this corpus reaches 0.4400 / 0.7200 /
0.8560 / 0.9160 / 0.9600 / **0.9760** at ef = 10 / 40 / 100 / 200 / 400 / 800,
against an exact sequential scan. It never reaches 0.99, independently reproducing
what §8a imported from pg_turbovec. So `p50 ≤ 2× pgvector HNSW at recall@10 ≥ 0.99`
has nothing to be 2× of, and needs restating rather than passing or failing. The
limiting caveat: m = 16 is modest for 960 dimensions and pgvector's own guidance is
to raise it, so this is a ceiling for *these build parameters*. An `m` /
`ef_construction` sweep is the next measurement.

**Cold, the rerank is not what decides the gate.** Measured at n = 1M
(`bench/RESULTS_PHASE_V_COLD.md`): a 20-candidate window is **86 ms** p50 cold,
interpolating to ~100 ms at 25, against **6.2 s** for pgvector HNSW at ef = 400
(recall 0.9600) and 11.5 s at ef = 800. The mechanism is the predicted one — HNSW's
traversal is dependent random I/O, unable to know its next node until the current
one is scored, while a rerank window's TIDs are all known before the first fetch.
Warm, both sides are fast and HNSW is the slower of the two measured points
(0.598 ms for a 10-candidate rerank versus 3.548 ms at ef = 10, both verified warm
by reading zero pages).

**What is still unmeasured is the half that matters now: the code scan.** V7 and V8
are not implemented, so no pg_weave vector query exists to time. At 4 bits, 1M
codes is 512 MB to read and score. Nothing above licenses a claim that this channel
beats pgvector; it licenses only that the heap rerank, which is what reopened the
Phase V gate, is not what will close it.

**The page counts below are cache-state- and scale-dependent, not layout alone.**
`bench/RESULTS_RERANK_IO.md` called them "device-independent" because they follow
from the storage layout; `bench/RESULTS_PHASE_V_COLD.md` falsified that. It
predicted ~48 reads and ~12 ms for a 20-candidate window; measured cold p50 at
n = 1M is **86 ms**, about 7×. Mechanism: at 250k rows with a 32 MB pool the toast
*index* stays largely resident and descents are nearly free, while at 1M rows on a
genuinely cold cache every descent pays its full depth.

**Read the cost through TOAST, not through "a heap fetch".** `wvec` is
`STORAGE = external`, so past about 490 dimensions the vector is out of line and a
candidate costs a toast-index descent plus chunk reads. An earlier draft of this
section priced the window at "up to 100 heap fetches"; the measured figure is 2.4×
that. It is not the 5× that one-page-per-chunk arithmetic predicts, because a
value's chunks are written consecutively and about four pack into one 8 KB page —
at 1536-d the toast relation holds exactly 1.000 pages per value. Two consequences
worth carrying:

- the cost is **bimodal**, not linear: when the vectors fit in the buffer pool
  these reads go to zero and the rerank is pure CPU. Since the codes are 0.064× of
  what HNSW must keep resident, there is a corpus range where the codes fit and
  HNSW's index does not;
- 1024-d reads **more** pages per candidate than 1536-d (2.388 vs 2.043), because
  3 chunks straddle page boundaries while 4 chunks fill a page exactly. If rerank
  data is ever stored deliberately, size it to land on that boundary.

The whole window's TIDs are known before the first fetch, since the fused top-k
produces them together, so the reads can be issued concurrently — where HNSW's
traversal cannot, being dependent by construction. **The gate turns on prefetch
depth**, which is a property of this shape rather than a tuning knob, and that is
what the cold EC2 run has to measure.

Task V10 changes shape accordingly: `WEAVE_PK_VRERANK` as a stored sidecar is no
longer the plan, and the rerank source becomes the heap. That is a maintainer
decision, recorded in `doc/PHASES.md`'s Phase V gate.

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

Page kinds. **These are no longer bits.** Task X1 (2026-09-10) replaced the flat
`uint16` bitmap with an escape bit plus an integer kind space, precisely because the
vector and fuzzy channels each wanted four more bits and they did not both fit; the
ids below live in `WeavePageOpaqueData.kind` and are read with `WeavePageHasKind()`,
never with a bitwise AND. The authority is `include/weave/pagekind.h`.

| id | kind | contents |
|---|---|---|
| 17 | `WEAVE_PK_VMETA` | `WeaveVecMeta`: dim, bits, metric, pack layout, code chain root, calibration pointer, calibration sample size and date |
| 18 | `WEAVE_PK_VCODES` | 32-lane code blocks, each preceded by `WeaveVecBlockHdr` |
| 19 | `WEAVE_PK_VGRAPH` | IVF centroids + cluster directory; optionally a centroid graph (`include/weave/graph.h`, §8a) |
| 20 | ~~`WEAVE_PK_VRERANK`~~ | **WITHDRAWN 2026-09-13, id left reserved.** It read "full-precision sidecar, required for recall@10 >= 0.99". The ratified shape reranks from the **heap**: a stored float32 sidecar is `4*dim` = 4,096 B/vector at 1024-d, half of what a measured pgvector HNSW index spends per vector, which forfeits the storage gate. 4 bits plus a top-25 heap rerank measured recall@10 **0.9920** at n=1M on GIST-960d. See `doc/PHASES.md` V10 |

### The paging question V7 has to answer first, with the arithmetic

"32-lane code blocks, each preceded by a `WeaveVecBlockHdr`" does not say how a
block relates to an 8 kB page, and **at the ratified width it cannot fit in one.**
A block is `WEAVE_VEC_BLOCK` (32) lanes of `ceil(dim*bits/8)` bytes, plus a header
carrying a `dim`-wide centroid code, plus 32 `WeaveVecLane` sidecars of 8 bytes. At
4 bits, with 8,160 usable bytes per page (8,192 less the page header and our 8-byte
opaque area):

| `dim` | codes | header | lanes | total | pages | blocks/page | waste |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 128 | 2,048 | 92 | 256 | 2,396 | 1 | 3 | 12 % |
| 256 | 4,096 | 156 | 256 | 4,508 | 1 | 1 | 45 % |
| 384 | 6,144 | 220 | 256 | 6,620 | 1 | 1 | 19 % |
| 768 | 12,288 | 412 | 256 | 12,956 | **2** | -- | 21 % |
| 960 | 15,360 | 508 | 256 | 16,124 | **2** | -- | 1 % |
| 1536 | 24,576 | 796 | 256 | 25,628 | **4** | -- | 21 % |

So the format needs a rule in **both** directions: several blocks must share a page
at small `dim` or nearly half the file is padding, and a block spans pages above
`dim` ~ 500 at 4 bits -- which includes GIST-960d, every figure in
`bench/RESULTS_CODE_SCAN.md`, and the 1536-d embeddings most users arrive with. Two
shapes are viable, and the choice is **not** an implementation detail, because
getting it wrong costs a REINDEX:

**(A) A byte stream over a page chain.** Treat the code array as a blob, the way the
livedocs and SuRF images already are, with blocks at fixed offsets in the stream.
Simplest writer, no waste, works at any `dim`. The cost lands on the scan path: a
block that spans pages is not contiguous, so scoring it needs either a memcpy into a
scratch buffer -- an extra pass over the bytes, and V17 measured the byte kernel at
7.6 GB/s against an 11.8 GB/s wall, so a second pass is not free -- or fragment-wise
scoring, which is shape (B) without saying so.

**(B) Coordinate-sliced pages.** Define a `WEAVE_PK_VCODES` page as holding a
*coordinate range* `[j0, j1)` of one block's 32 lanes. This is natural for
`WEAVE_PACK_LANE`, where coordinate `j` of lane `s` is one nibble at byte
`j*16 + s/2`: a page break is a coordinate break by construction, and the byte-LUT
kernel already walks coordinates in order and widens its accumulators every <= 256
of them, so it can cross a page boundary with no copy and no scratch buffer. It does
not work for `WEAVE_PACK_VECMAJOR`, where a page break splits a vector -- acceptable
only because no scanning kernel uses that layout.

**(B) has a second-order argument that may dominate the first.** V15's prefix stage
scores the leading `m` of `dim` coordinates; under (A) that saves compute while still
reading every byte, which is why it measured 1.43x on a bandwidth-bound scan. Under
(B) the leading coordinates are the leading *pages*, so a prefix scan reads `m/dim`
of the bytes. On a scan that is ~69 % bandwidth-bound, turning a compute saving into
an I/O saving is worth more than the 1.43x already recorded -- and it is measurable
before either is built.

**RATIFIED 2026-09-15: shape (B).**
### 7.1 The strip format, ratified 2026-09-15 (shape B)

A `WEAVE_PK_VCODES` page holds a **strip**: one coordinate range of one block's 32
lanes. Three facts make this exact rather than approximate.

1. **A coordinate is byte-aligned in `WEAVE_PACK_LANE`.** Code (coordinate `j`, lane
   `s`) sits at bit `(j*32 + s)*bits` (`src/vector/pack.c`), so coordinate `j`
   occupies bits `[j*32*bits, (j+1)*32*bits)` -- exactly `4*bits` bytes, an integer
   for every supported width (8, 12, 16 bytes at 2, 3, 4 bits). A coordinate
   boundary is therefore a byte boundary, and a page break placed on one splits
   nothing.
2. **The byte-LUT kernel already crosses that boundary for free.** It walks
   coordinates in order and widens its 16-bit accumulators every <= 256 of them, so
   a strip boundary is a place it was going to pause anyway. No copy, no scratch
   buffer, no reassembly of a spanning block.
3. **`WEAVE_PACK_VECMAJOR` cannot be stored this way** -- a vector's coordinates are
   contiguous there, so a coordinate cut splits vectors. That is acceptable because
   no scanning kernel uses VECMAJOR (§8), and the layout is recorded in
   `WeaveVecMeta` so a reader refuses rather than guesses. A VECMAJOR segment, if one
   is ever written, needs a different page rule and must not silently use this one.

**Block-major, not coordinate-major across the segment.** Strips are laid out block
by block: block 0's coordinate ranges, then block 1's. The alternative -- a true
column store, all blocks' coordinate 0, then all blocks' coordinate 1 -- makes a
prefix scan perfectly sequential, and was rejected because it makes single-block
access pathological: at n=1M/960-d one coordinate of all blocks is 500 kB, so
reading *one* block would touch 960 pages for 16 bytes each. V10's rerank window and
vacuum's lane update both do exactly that. Block-major keeps single-block access at
`ceil(dim/coords_per_page)` pages (2 at 960-d) and still gives V15's prefix stage its
`m/dim` byte reduction, at the cost of a strided rather than sequential read.

**Correction, from V7's reader: single-block access is not O(1) today, it is O(pages
in the weft).** Block-major makes a block's strips *consecutive* on the code chain,
but nothing records *where* they start, and `WeaveVecDirRec` is full at 284 bytes --
fixed by the O(1)-addressing requirement the directory exists for -- so there is no
room to record it without a format change. `weave_vec_block_read()` therefore walks
from `codestart` and takes the strips whose header names the block it wants. V7's
callers (`weave_check()`, the round-trip test, `weave_vec_strips()`) walk the whole
weft anyway, so it costs them nothing; **V10's rerank window and vacuum's lane update
cannot afford it, and the task that needs them owes the format an index over the
strips.** Stated here rather than left implicit because the block-major argument above
is what makes single-block access sound cheap, and by itself it does not.

**The slicing is asserted end to end, at a dim that slices.** `weave_vec_strips()`
reports every code page's header as stored, and `sql/vecindex.sql` pins the
`(blockno, j0, ncoords, centroid)` sequence of a 1024-dimension weft -- three lane
strips per block plus one centroid strip -- against the chain order. That test exists
because the format's central rule was **untested by construction** until 2026-09-17:
every dim in the suite was under 509, so every block was one strip, every `j0` was 0,
and a writer that ignored the strip plan's `j0` entirely passed everything.

**Per-block metadata is split, and the split is forced by `WEAVE_MAX_DIM`.**
`WeaveVecBlockHdr` ends in a `dim`-wide centroid code, which at 16,384 dimensions and
4 bits is 8,192 bytes -- larger than a page -- so a "prologue at the head of the
first strip" rule is unimplementable at the declared maximum. Instead:

- **The fixed part goes in a block directory**: `WeaveWarp firstwarp`, `uint32
  livemask`, the four bound floats, and the 32 `WeaveVecLane` sidecars. 284 bytes at
  every `dim`, so directory record `i` is at a computable page and offset -- O(1),
  which is what "score block `i`" needs. 28 records per page; 3,125 blocks at
  n=1M/960-d is 112 pages against 3,847 pages of codes, under 3 %.
- **The centroid code becomes strips of its own**, sliced by coordinate exactly like
  the lanes and written after the block's code strips. It is an input to bound (B3)
  only, and `bench/RESULTS_BOUND_PRUNING.md` measured that bound pruning **0.00 %**
  of blocks on both real corpora, so a scan that does not prune never reads these
  pages at all. Cost is `1/32` of the code bytes, matching the 3 % the note above
  `weave_codebook_solve()` already predicted.

Every page self-describes, following L17's precedent (a per-object discriminator, not
a per-index one, because after an upgrade one relation holds both generations):

    WeaveVecStripHdr { uint32 blockno; uint16 j0; uint16 ncoords; uint16 flags;
                       uint16 pad; }        /* 12 bytes */

`flags` distinguishes a lane strip from a centroid strip. 12 bytes of 8,160 is 0.15 %,
and it buys a `weave_check()` that can validate any page in isolation and a reader
that cannot mistake one block's strip for another's.

**Geometry, 4 bits, 8,160 usable bytes per page** (8,192 less the 24-byte page header
and our 8-byte opaque area), 16 bytes per coordinate, so `510` coordinates per page --
**509 as shipped**, because the 12-byte `WeaveVecStripHdr` comes off the payload
first: `weave_strip_coords_per_page()` computes `(8160 - 12) / 16`. The table below
keeps the round number it was ratified with; the writer, the reader and
`sql/vecindex.sql` all use the function, and the one number a test may state
literally is the function's:

| `dim` | code strips/block | centroid strips/block | pages/block | vs. one-block-per-page |
|---:|---:|---:|---:|---:|
| 128 | 1 (128 coords, 2,048 B) | 1 | 2 | packs 3/page under (A); see below |
| 256 | 1 | 1 | 2 | (A) wasted 45 % |
| 960 | 2 (509 + 451) | 1 | 3 | (A) needed 2 and could not share |
| 1536 | 4 (509x3 + 9) | 1 | 5 | (A) needed 4 |

Two corrections in that table, both from the shipped geometry rather than from the
estimate: the lane split is 509-wide, and **a centroid needs one strip, not `dim`/510
of them.** A centroid coordinate is `bits` *bits* (not 32 lanes' worth), so 16,296 of
them fit a page at 4 bits and every `dim` this AM accepts fits in a single centroid
strip. That is the 1/32 pricing this section already promised, arrived at by
`weave_censtrip_build()`'s own stride; `include/weave/vecweft.h` explains why
`weave_strip_build()` could not have been used for it.

**The small-`dim` waste is real and is not fixed by this shape.** A 128-d block's
lane strip is 2,048 bytes on an 8,160-byte page: 75 % waste, worse than (A)'s 12 %.
The fix is to let one page carry several strips -- the header already names
`blockno`, so a page holding strips for blocks `b, b+1, b+2` needs no new field --
and the writer packs greedily while a strip fits. That is a writer-side decision with
no format consequence, which is the reason to state it here and implement it once
there is a low-`dim` corpus to measure it on. Until then the writer emits one strip
per page and the waste is recorded rather than claimed away.

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

### 7.2 How a `wvec` column reaches the access method — landed 2026-09-16

`CREATE INDEX docs_weave ON docs USING weave (body wdoc_lex_ops, embedding
wvec_weave_ops)` is the declaration. Three things had to change for it to be
accepted, and the small one was the flag.

**`amcanmulticol = true`.** Column order is not a capability question the way it
is for btree: no channel's on-disk structure is shared with another's, so there
is no leading-column prefix rule to respect, and `USING weave (embedding
wvec_weave_ops, body wdoc_lex_ops)` is exactly as valid. That freedom is what
makes the routing mandatory rather than cosmetic.

**The routing, `weave_index_layout()` in `src/am/am.c`.** The AM's
single-attribute assumption was **four sites**, not the seventeen a `values[0]`
grep suggests — the build callback and `weave_insert` (`src/am/ambuild.c`), the
scan-side exact recheck (`src/am/amscan.c`) and the planner's count-pushdown
column match (`src/am/customscan.c`). Everything else spelled `values[0]` is a
tuplestore output array. Each of the four now indexes by attnum; the build path
resolves the layout once into `WeaveBuildState` rather than per heap tuple.

**The discriminator is the operator family, not the column type.** A type-keyed
map works today and breaks at Z4/Z8: `doc/specs/FUZZY_CHANNEL.md` declares the
corpus-n-gram channel as `USING weave (sku gram_ops)` over an ordinary `text`
column, so two weft kinds will share one input type. It is keyed on the family
*name* rather than an OID because the extension is `relocatable = true`, and on
the family rather than the class because the relcache caches `rd_opfamily[]` per
index column and does not cache opclass OIDs at all — `pg_index.indclass` is a
varlena, reachable only by deforming the catalog tuple. `amvalidate` rejects a
family the registry does not know, so adding a channel is one row in
`weave_opfamily_kinds[]`.

`wvec_weave_ops` deliberately declares **no operator members**: V7 is the storage
half and V8 is the scan. An opclass advertising `<=>` before the AM can execute a
vector ordering would make the planner build paths that fail at run time, on the
query shape every pgvector user writes first.

**Two combinations are refused rather than half-supported.** More than one column
of the same kind (which document is "the" document is arbitrary), and — the one
that is a real restriction — **an index with no lexical column at all**. The
docid space every channel indexes into is assigned by the lexical build, so a
vector-only weave index would build with no documents in it and answer every
query with zero rows: a wrong answer, not a slow one. Lifting it means letting a
vector weft create a bolt on its own, and it is not on V7's path.

**What the test can and cannot see.** `sql/vecindex.sql` builds the index with the
vector column *first*, because "the first column" was the assumption all four
sites shared. Eight mutations that reintroduce `values[0]` at one site or drop one
refusal are all caught — but only after the recheck arm was rewritten twice, and
both failures are the same lesson: **the first version of that arm never reached
the code it was testing.** `weave_recheck_exact()` runs only for query shapes the
posting lists over-generate (PHRASE/NEAR/fuzzy/regex), so a plain two-term AND
never calls it; and once a phrase was used, the *planner* answered it with a
bitmap heap scan whose executor recheck re-evaluates `@@@` itself, so the mutation
still passed. Only `weave_count()` and `weave_search()`, which enter the scan
machinery directly and have no executor recheck to fall back on, actually exercise
the site.

### 7.3 The writer, its two producers, and why merge must not re-encode — decided 2026-09-16

A segment's vector weft is written by **one** function. Two things produce the
lanes it writes, and the difference between them is the whole of this section.

**Producer 1, the build.** `weave_build_callback()` has the wvec in
`values[vecattno - 1]`; it encodes and hands over 32-lane blocks.

**Producer 2, the merge — landed 2026-09-17.** A merge changes which documents
share a bolt, so every lane changes warp position and lanes move between blocks. It
reads each input weft **block by block**, takes each live lane's code bytes out with
`weave_unpack_lane()`, and appends them to the **same accumulator producer 1 fills**
(`weave_vec_accum_add_encoded()`); `weave_vec_write_weft()` then does the geometry,
the directory, the strips and the statistics. One writer, two producers.

*Not* `weave_pack_move_lane()`, and the reason is worth recording because §7.3 named
it for two days. That primitive moves a lane **within a pair of blocks that both
already exist**, which is the vacuum swap-remove it was built for. A merge does not
have the destination block: the output's block membership is only known once every
input's lanes have been collected and sorted into output docid order, because a
document's output warp depends on which *other* documents survived. Unpacking to code
bytes and repacking through the tested writer moves the identical bytes, reuses the
`weave_pack_lane`/`weave_unpack_lane` pair every build already exercises, and needs no
second implementation of the block geometry. Reading the block whole costs nothing
either: in `WEAVE_PACK_LANE` coordinate `j` of lane `s` is at bit `(j*32+s)*bits`, so
one lane's code touches every page of the block anyway.

**What the merge needed that the format did not have: warp → docid.** A warp is an
ordinal. Nothing in a v1 weft said which *document* a lane belonged to, and the
derivation this spec offered instead — "warp i is the i-th smallest docid in the
bolt, and the bolt's docids are all in its lexical weft" — is **false**. Producer 1
gives a lane to every document whose lexical column is non-NULL; a document reaches
the lexical weft only if it has at least one posting; and a non-NULL `wdoc` with no
terms (empty text, stopwords only) has none. One such document shifts every later
warp's derived docid by one, mis-attributing every vector after it, and nothing
counts wrong. A merge needs the docid to drop a tombstoned document's lane and to
place a moved lane at the output's rank; V8 needs it to turn a warp back into a heap
tid.

So a weft carries a fourth chain, `WEAVE_PK_VWARP`: `nvec` dense `uint64` docids,
addressed by arithmetic, 8 bytes per lane — 1.7 % of the code bytes at 960-d and 4
bits. Its own chain rather than four more fields in `WeaveVecDirRec` because that
record is sized so 28 fit a page and record `i` is O(1), and it is read for **every**
block a scan considers (bound pruning measured 0.00 %, so "every" is literal), to
carry a value needed once per **returned** row. `WEAVE_VMETA_VERSION` is 2; a v1 weft
is refused rather than read best-effort, and no released version ever wrote one.

**The merge MUST move codes verbatim. It must never decode to float and
re-encode** — and the reason this section originally gave for that is **wrong**,
which the mutation run found and which is worth more than the rule itself.

*The claim was:* quantization is lossy, so decode-then-re-encode compounds the error
on every merge and an index's recall decays with its *merge history* rather than its
contents. *The measurement* (`test/hegel/test_quantize.c`,
`test_reencode_idempotent`): **a code is a fixed point of decode-then-encode.**
Dequantizing yields exactly the codebook levels, and re-quantizing those returns the
same levels — 0 of 2,100 codes moved over `bits` 2–8 at 64-d and 768-d, and 4 of
9,800 over dim 4–1536 in a wider throwaway sweep, each of those by a single byte at a
decision boundary. A merge that decoded and re-encoded, *keeping the stored scale*,
therefore produces the same bytes: it is an **equivalent mutation**, and it survives
the whole suite. Error does not accumulate with merge history.

*What is actually true, and what the rule protects:*

1. **The SCALE is not a fixed point.** A reconstruction's norm is not its original's,
   so a producer that re-encoded *and took the re-encoded scale* changes what the lane
   dequantizes to even where its bytes did not move — 264 of those same 2,100 round
   trips moved the scale, and on the structured vectors `sql/vecindex.sql` indexes it
   moves for most rows. That mutation **is** caught, by comparing the stored
   `(scale, norm)` pair across the merge. So the operative rule is: **carry the stored
   sidecar pair; never recompute it from a reconstruction.**
2. **Idempotency is a property of THIS codebook at THIS width.** It fails the moment
   the output's codebook, width or TQ+ calibration (§5) differs from the input's —
   which is exactly what the geometry-agreement guard below refuses, and exactly what
   a future calibration would introduce.
3. **Cost.** A decode plus an encode per lane is a rotation, a `dim`-wide dequantize
   and a `dim`-wide quantize on the merge's critical path, to reproduce bytes it
   already had.

The general lesson is the one AGENTS.md keeps: a rule with a wrong reason attached is
a rule someone will discard when they disprove the reason. The property is now a
test, so if idempotency ever breaks, the day it breaks is the day something fails.

**But the per-block statistics ARE recomputed on merge, and that is not
re-encoding.** A merge changes which lanes share a block, so `livemask`, `smax`,
`maxrecnorm`, `minnorm`, `censcale`, the centroid code and `cenrad` all change.
They are recomputed by `weave_vecblock_stats()` from the lanes **dequantized from
the codes that were moved** — which is required rather than merely acceptable,
because `cenrad` must be measured against the centroid as reconstructed from
`cencode` and not against an exact float centroid (see §7.1 and the note above
`WeaveVecBlockHdr`; measuring against the float centroid is the unsoundness the
V7 bench fix removed). Moving codes and keeping stale statistics is a silent (C2)
violation.

**What is left of the interim rule, and why it kept its shape.** The interim rule
was "the merge SKIPS any group containing a vector-bearing segment". What survives is
narrower: **all the input wefts must agree on (dim, bits, layout, metric), and a
group whose wefts disagree is skipped.** Re-quantizing to a common width is
forbidden by the paragraph above, so there is nothing else a merge could do. It is
still a skip and not an `ereport`, because an error reachable from VACUUM's cleanup
is how an index becomes permanently unvacuumable (G15, G20), and still not a drop,
because dropping a weft is data loss nothing notices before V8. Skipping is always
safe: a merge is optional work.

The rule is enforced at the two **chokepoints** (`weave_merge_selected()` and
`weave_merge_group_to_seg()`), before a page is allocated, and the four candidate
selectors apply it to their lists as well so a mixed index still compacts the bolts
it can. Both halves are needed for the reason the interim comment already gave: a
rule enforced only in the selectors is a rule the next selector forgets.

**Is a mismatch reachable? YES, since 2026-09-19** (`doc/GAPS.md` G26, closed). It was
not, and the entry recorded that as a gap rather than claiming it as a guarantee: only
a *build* wrote a weft, and one build reads the `bits` reloption once, so the two
bolts a mismatch needs could not both exist. **G23 changed that, exactly as G26
predicted it would.** A pending flush now writes a weft, and it uses the CURRENT
reloption — which is correct for a brand-new segment built from raw vectors and is
precisely what a merge must not do — so `ALTER INDEX ... SET (bits = ...)` followed by
an `INSERT` and a `weave_merge()` produces two wefts of different widths in one index,
three statements from a standing start. The last block of `sql/pendingvec.sql` is that
sequence: the merge declines to combine them, the index keeps answering, every
`weave_check()` invariant holds, and the mismatch-skip mutation is no longer a
surviving one.

`dim` remains unreachable for the reason it always was: `weave_vec_accum_add()` throws
on a dim change *within* a segment, so two bolts of different dims need a flush
boundary landing exactly on the dim change. `layout` and `metric` are constants today.

**A third producer's worth of input, without a third producer.** The flush feeds
producer 1 from a pending page rather than from a heap tuple, which is why
`WeavePendingItem` stores the `wvec` **verbatim rather than pre-quantized**: a code
would bake `bits` into the pending buffer, and re-widening it means reconstructing the
vector and so recomputing the `(scale, norm)` pair from a reconstruction — forbidden
in §7.3 for the same reason the merge may not re-encode. Storing the vector raw costs
~8x the bytes on a pending page (3,848 against 480 at 960-d/4-bit) and buys **one
quantization path instead of two.**

**The interim's cost is paid off.** "A vector index does not compact, so its segment
count only grows, and it will eventually reach `WEAVE_MAX_SEGMENTS`" no longer holds:
compaction is a merge, and a merge now carries the weft. That was the reason V8 was
blocked on this task.

**MEMORY: the vector half of a merge is NOT streaming, and that is a deliberate,
bounded debt.** `weave_merge_segments_streaming()` is bounded to one term's postings
at a time, specifically so a full compaction of a large index does not buffer the
index in RAM. Producer 2 does the opposite: the accumulator holds **every output
lane's code** before the writer runs, so a vector merge is `O(nvec · codebytes)`
resident — **480 MB of codes at a million documents, 960 dimensions and 4 bits**,
plus 8 bytes per lane of docid and 9 of sidecar and one transient 8-byte-per-lane
sort array, so ~500 MB — and it is charged to the merge's own context rather than to
`maintenance_work_mem`. That is acceptable at the scale this AM is tested at and it
is *not* acceptable at 10M. The streaming shape (merge the input wefts as
sorted-by-docid runs, write each block as it fills) is `doc/GAPS.md` G25 with the
threshold worked out. The in-memory version shipped because it reuses the one tested
writer; the follow-up is recorded rather than assumed.

**The second cost of the interim is now paid too: the free path is reachable.**
Every merge frees its inputs, so `weave_free_segment()`'s `WEAVE_WK_VECTOR` arm and
all four of `weave_vec_free_weft()`'s chain frees now run on the first merge of a
vector-bearing bolt, and `weave_check(deep)`'s reachability walk is what catches a
forgotten one — a leak of every code page of every merged bolt. `sql/vecindex.sql`
merges and then checks deep; the mutation table's `free-omits-strips` leg, which
survived V7 by construction, is **caught**. The history is kept below because the
diagnosis matters more than the fix.

*Before this commit:* **the free path was unreachable, so it was untested.** `weave_free_segment()`'s `WEAVE_WK_VECTOR` arm
calls `weave_vec_free_weft()`, and nothing calls `weave_free_segment()` except the
two merge commit paths — which, by the rule above, never take a vector-bearing bolt
as an input. Nothing else reclaims a weft page by page either: compaction *is* a
merge, `ambulkdelete()` only rewrites the livedocs bitmap, and `REINDEX`,
`VACUUM FULL` and `DROP INDEX` all discard a whole relfilenode. A build whose
`weave_vec_free_weft()` begins with `elog(ERROR)` passes the entire suite, so a
mutation that deletes the strip-chain free cannot be caught by any test that exists.
The function is kept and the exception is written down (`doc/GAPS.md` G24) rather
than closed with a test-only entry point, because **the merge producer above is
exactly what makes it reachable**: on the first merge after that exclusion comes
out, a free path that forgot the strip chain leaks every code page of every merged
bolt. The merge producer's commit therefore owes this gate — merge two
vector-bearing bolts, `weave_check(deep)` reports zero unreachable pages, and the
`meta.codestart` mutation is proven to fail it.

Three traps in the surrounding code, all of which cost something if missed:

- **Weft order.** `weave_chandesc_check()` requires the descriptor array to be
  strictly ascending by `(kind, attnum)`. `WEAVE_WK_VECTOR` is 2 and
  `WEAVE_WK_FUZZY` is 3, so the vector descriptor is emitted **between** the
  lexical and fuzzy ones in `weave_chandesc_for_segment()`, not appended.
- **Determinism.** `weave_block_codebytes()` returns 0–28 slack bytes that no
  pack function ever writes, so the block buffer must be **zeroed before
  packing** or two indexes holding identical vectors differ on disk.
- **Reachability.** `weave_free_segment()` and `weave_check()`'s deep
  reachability walk both enumerate a bolt's chains explicitly. A vector weft that
  is written but not added to both shows up immediately as leaked pages — which
  is the good failure, and `weave_index_size_detail()` needs its buckets in the
  same commit for the same reason.

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

> **SUPERSEDED FOR OUR CONFIGURATION, 2026-09-18.** The table below is v2.5.0 data and
> it is what demoted the graph, which still holds. But read as a positive case for IVF it
> is now stale: at **v2.8.3** the same project measured 1M x 1024-d and found **4-bit flat
> beating `lists=1024` at every target** (4-bit flat 6.08 ms at R@10 1.000 -- a 4-thread
> figure, see `bench/RESULTS_CODE_SCAN.md`), with **4-bit IVF's recall ceiling at 0.959**
> (p=128) and unmoved by any rerank window. So at *our* 4 bits, IVF does not reach 0.98 at
> all, and the "yes" in the right-hand column does not apply to us. Their IVF/flat
> crossover survives only at **1 bit**, where the quantizer is lossy enough to demand a
> wide rerank window (w=800 at 1 bit against 25-32 at 4). That is consistent with our
> 4-bit choice rather than a warning about it, and it independently corroborates V9's
> demotion. The withdrawal of the graph rests on the table; the withdrawal of IVF now
> rests on this note.

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

## 8b. The code-scan shuttle — designed 2026-09-17 (task V8)

This is the **first implementation of `include/weave/channel.h` in the tree.**
Nothing implemented `WeaveShuttleOps` before it, lexical retrieval included: the
scan in `src/am/amscan.c` runs its own WAND cursors. So V8 is simultaneously the
vector channel's query path and the first test of whether the shuttle contract is
implementable as written. Where the contract turned out to be under-specified,
that is recorded here rather than quietly worked around.

### The seam: a decision core with no backend, and glue with no policy

`include/weave/vecscan.h` + `src/vector/vecscan.c` hold the per-block decision —
mask, then bound, then score — and nothing else. No `Relation`, no `Buffer`, no
`palloc`. `src/vector/vecshuttle.c` reads pages and contains no policy.

The reason is (C1) and (C2). Both are the class of requirement where **a wrong
implementation returns plausible answers**: a bound 1 % low drops rows that look
like they were merely outranked. That makes a property test mandatory (hard rule
1), and a property test reachable only through a built index, a heap and a page
cache will not be run at the scale that finds anything. `test/hegel/test_vecscan.c`
links the core with a bare compiler, as `test_vecweft.c` and `test_vecbound.c` do.

### (C1) is a property of untrusted data, so it is checked, not assumed

`WeaveVecDirRec.firstwarp` comes off a page. The writer sets it to
`blockno * 32` for every block and `weave_check()` asserts that equality — but a
*scan* that assumes it is taking a number from a possibly-corrupt page and using
it to index an allowlist. The core therefore requires **both** halves: the O(1)
formula, and a strictly ascending sequence across the blocks it visits. A
mismatch is a refusal.

When V13 assigns warps in cluster order the formula half has to be lifted. The
ascent half must not be, because the ascent *is* what (C1) says.

### The order of the three outcomes is the order of their cost

| outcome | cost | does it pay? |
|---|---|---|
| skip on the allowlist | one AND, one branch. No code byte read, no strip scattered, no kernel call | **yes** — this is the mechanism behind claim 3 |
| skip on the block bound | one LUT pass over the centroid code (`dim` gathers = 1/32 of scoring the block), three comparisons | **no** — measured to prune **0.00 %** of blocks on both real corpora (`bench/RESULTS_CODE_SCAN.md`) |
| score | 32 lanes × `dim` gathers | — |

The bound is implemented anyway, because (C2) is a contract: the fused loop is
entitled to a true upper bound from every channel, and a channel that returns
`+inf` to avoid the arithmetic has removed itself from the algorithm. What is
*not* permitted is quoting its existence as a speedup. On the corpora measured so
far it is a ~3 % tax, and `§6` explains why it is structural rather than a tuning
failure.

### The finding that shaped the traversal: there is no block→page index

`weave_vec_block_read()` walks the **entire** codes chain from `codestart` on
every call, matching `blockno` as it goes. That is fine for `weave_check()` and
for introspection. For a scan it is O(blocks × pages) — quadratic in the weft —
so **V8 does not use it.** The shuttle carries a *forward-only sequential cursor*
over the codes chain, advanced in lockstep with a second cursor over the
directory chain, which makes a full scan O(pages) as it must be.

Two consequences, and the second one limits a claim:

1. **`seek()` is forward-only.** A backward target is a caller bug and raises.
   This is consistent with (C1) — the contract already says seek walks in
   ascending warp order — but the contract did not say what a *backward* seek
   does, and "returns the current position" would let a fused-loop bug become a
   wrong answer instead of an error.
2. **The mask short-circuit saves scoring and strip scatter. It does not save
   page reads.** The chain must still be walked to find the next link, so a
   skipped block costs its `ReadBuffer`s regardless. Since the scan is
   compute-bound at every measured size for the exact kernels (1.6 GB/s against
   an 11.8 GB/s wall, `bench/RESULTS_CODE_SCAN.md`) this is the cheap half of the
   cost — but claim 3 must be stated as *less scoring*, not *less I/O*, until a
   block→page index exists. Opened as a gap rather than implemented here: it is a
   format change, and V8 is not.
3. **The lazy scatter is decided by the MASK and cannot be decided by the BOUND**,
   and that is a consequence of the on-disk order rather than a choice. A block's
   strips are block-major with the lane strips first and the centroid strips after
   them, while the bound needs the centroid — so by the time the core could say
   "this block is pruned", its lane strips are already behind the forward-only
   cursor. The mask needs only the directory record, which the directory cursor has
   produced already, and the shuttle asks with `weave_lane_avail_mask()`, the *same*
   shared inline the core and every kernel use, so this is one function answering a
   resource question and not a second copy of the policy. Consequence: a block
   skipped on the **bound** still pays its strip scatter. That costs nothing
   measurable — the bound prunes 0.00 % of blocks on real corpora and a fused-loop
   driver passes `-INFINITY` — but a block→page index (point 2) would not fix it
   either; only a format that put the centroid strip first would.

### Scan-time quantizer reconstruction, and the guard it needs

A scan needs a `WeaveQueryLut`, which needs the `WeaveQuantizer` the codes were
built with. Neither the codebook nor the rotation is stored on disk, and neither
needs to be: `weave_codebook_get(bits, dim, ...)` is a memoized pure solve and
`weave_rotation_init(dim, ...)` derives its permutation and signs from a fixed
seed. Both are bit-identical whenever recomputed from `(dim, bits)`, and both are
in `WeaveVecMeta`. So the scan rebuilds the quantizer exactly.

**That holds only while TQ+ calibration is off.** `WeaveVecMeta` reserves
`calibstart`/`calibsample`/`calibtime` for a calibration blob, there is no
on-disk format for one, and the writer always sets `calibstart =
InvalidBlockNumber`. A scan that built `cal = NULL` against a weft encoded *with*
a calibration would get a syntactically valid, silently wrong quantizer. So the
shuttle **refuses a weft whose `calibstart` is not `InvalidBlockNumber`**. That
turns a future silent-wrong-answer into a startup error, and it costs one
comparison.

### `maxscore` is (B2), from one directory pass

`WeaveShuttle.maxscore` must dominate `block_max()` everywhere. Folding (B3) over
every block would need every centroid *code*, which lives on the code pages — a
full pass over the weft before the scan starts. (B2) needs one float per
directory record, and the directory is roughly 1/56 of the code pages at 960
dimensions. So `begin()` makes one directory pass and folds
`max maxrecnorm × ||q||`. Looser, and looser is right for a value whose only job
is the MaxScore partition.

**Correction, from implementing it.** This section said the same pass "also
validates (C1) across the whole weft once instead of incrementally". It does not,
and it cannot through the core's API as declared: `weave_vec_scan_maxscore_fold()`
takes an accumulator and a record, holds no state and has no `why` to report
through, and the (C1) witness lives inside `weave_vec_scan_block()`, which needs a
query LUT and a centroid code — i.e. the code pages this pass exists to avoid
touching. So (C1) is checked **incrementally**, one block at a time, as the scan
reaches it. That is strictly better anyway: a weft whose 400th block breaks the
ascent still answers the first 399 blocks' worth of query correctly and then
errors, instead of refusing every query on the weft before reading a code.

### The metric was a placeholder, and V8 is where that comes due

V7 wrote `WeaveVecMeta.metric = WEAVE_METRIC_L2` into every weft with a comment
saying so: nothing in the catalog selected a metric, `wvec_weave_ops` declares no
operator members, and 0 is not a valid `WeaveMetric` so a zeroed field must not
validate as one. It changed no stored byte — codes are metric-independent — and it
left the decision to V8. Two things had to be settled here.

**First, the domain.** A kernel returns an inner product and is handed no norms
at all, on purpose. `weave_block_bound_l2()` returns a bound on −‖q−v‖². Those are
different quantities in different units, so a channel that reported an L2 bound
next to an IP score would not have a bound that is slightly wrong — it would have
two numbers that cannot be compared, and (C2) would be *meaningless* rather than
violated. The conversion therefore lives in the decision core, in one function,
and the bound goes through the same switch. A caller never sees a raw kernel
score. For L2 the per-lane term is the lane's **stored** norm — the second half of
the interleaved `(scale, norm)` pair in the directory record, which the kernel
does not receive and the core does — and the bound uses the block's `minnorm`,
because subtracting the smallest norm is what maximizes the expression, which is
what an upper bound needs.

Cosine is **refused**, not approximated: a sound bound has to switch on the sign
of the numerator, dividing by the smallest norm when it is positive and by a
largest norm when it is negative, and no maximum *true* norm is stored.
`WEAVE_METRIC_HAS_BOUND()` admits cosine because a bound exists; V8 does not
implement it, and inventing unmeasured arithmetic to fill a table cell is how §6's
bound got specified wrong the first time.

**Second, where the metric comes from.** Not a GUC: two segments of one index
could then disagree, which is the G26 failure class. The eventual user-facing form
is **one operator family per metric** — `wvec_l2_ops`, `wvec_ip_ops` — because §7.2
established that the AM can only discriminate on the *family* (the relcache caches
`rd_opfamily[]` and no opclass OIDs, and `indclass` is `CATALOG_VARLEN` so it does
not compile), and because that is the shape pgvector users already know. That
wiring belongs with the `ORDER BY … <-> …` path, which V8 does not build.

So V8 takes the smaller step that makes the field mean something today: a
**reloption**, recorded per segment in the field that already exists. This is
consistent with `doc/CONVENTIONS.md` rule 1 read carefully — the rule's subject is
"anything that changes bytes on disk", and while the metric changes none, the
clause that decides it here is the second one: *"the value used is recorded in the
segment so a reader never has to guess"*. That is precisely what
`WeaveVecMeta.metric` is for. Default `l2`, because every weft already on disk
says `l2` and format v8 shipped two days ago.

**UPDATE, task F7 (2026-09-21) — SUPERSEDED THE SAME DAY. Read the correction that
follows before relying on any sentence in this paragraph; what it calls a "named
divergence" is a wrong answer. The ORDER BY path exists and the per-metric family
split still does not.** F7 added `<=>` on `(wvec, wvec)` to `wvec_weave_ops` as an
ORDER BY member (strategy 1; see `include/weave/am.h`, `WEAVE_STRAT_VEC_DISTANCE`),
because until it did, a vector query could not reach `amrescan` at all and
`ORDER BY embedding <=> $1 LIMIT 10` was answered by a Seq Scan and a top-N Sort —
task L7's 7,000× cliff with a different column type. That leaves a **named
divergence** rather than a clean story, and it is stated here rather than only in the
code: the operator is spelled for **cosine** distance, and the ordering the index
produces is the weft's **`metric`** (`l2` by default). For unit-normalized vectors —
what the embedding models this channel targets emit — cosine, `l2` and inner product
induce the same ordering and the two agree; for unnormalized vectors they do not.
`sql/vecorderby.sql` therefore **records** the number of positions on which the index
ordering differs from an exact float `<=>` ordering, with an `EXPLAIN` over each arm,
instead of asserting agreement — the quantized codes are the second, independent
source of the same divergence, and neither is a defect. The eventual fix is still the
family-per-metric split above (`wvec_l2_ops`, `wvec_ip_ops`), which needs a second
opclass, a second `weave_opfamily_kinds[]` row and a migration for existing indexes,
and is therefore a task rather than a line. `xs_recheckorderby` stays **false**: a
reorder queue would require the index's distance to be a proven *lower bound* on the
operator's, and a quantized score is not one.

**CORRECTION, task F7 (2026-09-21): `<=>` was withdrawn as a member, replaced by one
member per metric the scan core can serve.** The mechanism the paragraph above got
wrong: `include/weave/vecscan.h:53` says the scan core serves **IP and L2 and refuses
everything else**, and the weft's metric is `WeaveVecMeta.metric` (from the `metric`
reloption, default `l2`). Registering `<=>` therefore did not produce an
approximation of cosine — it produced an **`l2` ordering under a cosine operator**,
for every row, silently. And `sql/vecorderby.sql`'s "divergence" section was
measuring that mismatch (24 of 25 positions differing, overlap 19 of 25) while
attributing the number to quantization. What landed instead:

- **Two ORDER BY members.** `<->` = `wvec_l2_distance`, strategy **1**
  (`WEAVE_STRAT_VEC_L2`); `<#>` = `wvec_negative_inner_product`, strategy **4**
  (`WEAVE_STRAT_VEC_IP`). Both are ascending-is-nearest in pgvector's convention —
  `<#>` returns *−*Σaᵢbᵢ for exactly that reason — and the weft's score domain is
  higher-is-better in both metrics, so the index's ordering value is `-score`
  ascending in both. Strategy 4 is why `amroutine->amstrategies` was raised from 3 to
  4: `ALTER OPERATOR FAMILY` validates a member number against it, and 2 and 3 are
  already `WEAVE_STRAT_DISTANCE` and `WEAVE_STRAT_EDIST` while `weave_rescan()`
  dispatches order-by keys on `sk_strategy` alone.
- **`<=>` is deliberately not a member of any weave family on `wvec`.** Cosine has no
  sound compressed-domain bound here (no maximum true norm is stored), so the core
  refuses it and `CREATE INDEX ... WITH (metric = 'cosine')` is refused outright. A
  cosine member could only ever be served in some other metric. With no member,
  `ORDER BY v <=> q` gets no index path and is answered by a Sort over a Seq Scan —
  the honest plan, and `sql/vecorderby.sql` now asserts it.
- **A metric mismatch is refused, not answered.** `weave_rescan()` compares the
  metric the strategy names against the index's and `ereport(ERROR)`s, naming the
  operator and the metric, with a hint to use the other operator or rebuild. It is a
  *run-time* error because the metric is a reloption and path generation never looks
  at one; the scan is the last place that can refuse.
- **The only divergence from the operator that remains is the quantizer**, which is
  what an ANN index is, and `sql/vecorderby.sql` records it. `xs_recheckorderby`
  stays **false** for the reason given above.

**The family-per-metric split is still the fix**, for two reasons the refusal makes
visible rather than removes. First, it moves the decision into the planner: an `ip`
index would have no `<->` member, so no path is generated and no error is needed.
Second, the `metric` reloption carries `AccessExclusiveLock` and can be changed by
`ALTER INDEX ... SET (metric = ...)` **without** a `REINDEX`, which rewrites no weft
— so the reloption and `WeaveVecMeta.metric`, which is the scoring authority, can
disagree. A reloption is the wrong home for something the planner must see and a
reader must trust; an opclass, the way pgvector does it, is the right one.

### Warp → docid needs no random access, because the scan is monotone

The dense warp→docid map on the `WEAVE_PK_VWARP` chain has only a sequential
cursor; `src/vector/vecwrite.c` says an O(1) by-warp reader "is not written until
there is a caller" and names V8 as that caller. It turns out V8 is not, and the
reason is worth stating because it is a general property of this channel: **the
scan visits warps in ascending order, and candidates therefore enter the top-k
heap in ascending warp order too.** A third forward-only cursor over the warp map,
advanced in lockstep with the other two, resolves every docid the scan needs at
`O(pages)` total. Random access would be `O(pages)` *per lookup* over an unindexed
chain — the G27 shape — and it is not needed at all.

Three lockstep monotone cursors — directory, codes, warp map — and no random read
anywhere. That is the whole traversal.

### What implementing the contract found in the contract

V8 is the first executed implementation of `include/weave/channel.h`, so three
things it left open had to be decided. Recorded here because the next channel hits
them too.

1. **A backward `seek()` raises, and so does a repeated one.** The check is
   `target < s->cur`, i.e. against the previous *return*, not the previous target.
   So `seek(0)` twice is an error whenever the first call returned a warp above 0.
   That is deliberate — the cursors are one block ahead by construction, and the
   alternative is a shuttle that quietly re-answers from a block it may already
   have walked past — but it means **`seek()` is not idempotent**, which (C1) does
   not say and a fused loop must not assume.
2. **`score_block()`'s `allow` argument has no length**, while every other
   consumer of a warp-indexed bitmap in this channel requires one, because
   `firstwarp` comes off a page and `include/weave/kernels.h` is explicit that an
   untrusted index into an unlengthed bitmap is an unbounded out-of-bounds read.
   The vector shuttle therefore honours a non-NULL `allow` here only within the
   length its own allowlist was opened with, and refuses it otherwise. The
   contract should grow an `nwarp` parameter; changing it is not V8's business.
3. **`score_block()` has no weight-applying wrapper.** `weave_shuttle_score()` and
   `weave_shuttle_block_max()` exist precisely so that `WeaveShuttle.weight` is
   applied in exactly one place, and (C2) survives weighting because both go
   through it — but the bulk path the scorer is told to *prefer* has no such
   wrapper, so a scorer that calls `ops->score_block()` directly gets unweighted
   scores next to weighted bounds. V8's own driver uses weight 1.0, so nothing is
   wrong today; the fused scorer must either add the wrapper or apply the weight
   itself.

### What V8 is not

No exact rerank (V10), no coordinate-prefix first stage (V15), no graph. Every
score V8 produces is a quantized-domain score, so **no recall number comes out of
this task** — the quantizer alone tops out at 0.8780 recall@10 on GIST-960d
(§2.1), and closing that is V10's job. A `weave_vec_scan()` SRF exists so the
scan machinery is reachable *directly* from SQL, which after 2026-09-16 is a
requirement and not a convenience: a mutation in scan code that can only be
reached through the planner may be answered by a bitmap heap scan's own recheck
and survive the entire suite (`AGENTS.md`). Its allowlist argument is a **docid**
array converted to warps per bolt, not a warp array: a warp is segment-local, so
warp 7 names a different document in every bolt and a warp array is ambiguous the
moment an index has two of them.

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
