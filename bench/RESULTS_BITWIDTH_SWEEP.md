# Result: what code width reaches `recall@10 >= 0.99`, and does it fit the storage budget?

Date: 2026-09-12. Instance: EC2 `c7i.8xlarge` (Xeon Platinum 8488C, 32 vCPU,
61 GB), us-east-2, run `pgweave-20260912-140417`. Harness: `bench/ivf_recall.c`
at commit `be64656`, linking the shipping codec unmodified. Reproduce with
`bench/aws/run.sh c7i.8xlarge bitsweep`.

This supersedes the width table in `bench/RESULTS_IVF_RECALL.md`, whose 4-bit
figures were produced by a codebook that had not converged (`758339e` fixed the
solver; the 200-sweep cap was short of the ~700 sweeps a 16-level solve needs).

**The harness self-check passed on both corpora:** at `nprobe == lists`, exact
rescoring reproduces brute force for every query. That is what makes the full-probe
column a ceiling rather than a sample — probe-miss error is zero by construction,
so no partitioning, no `nprobe`, and no better k-means can raise it.

## Full-probe compressed-domain recall@10 — the ceiling, by width

`R_w10`: the top-10 are taken from the compressed domain alone, no rerank.

| bits | GloVe 6B 200-d, 200k | GIST-1M 960-d, 100k | bytes/vector at 1024-d |
|---|---|---|---|
| 2 | 0.7345 | 0.6130 | 256 |
| 3 | 0.8515 | 0.7880 | 384 |
| 4 | 0.9225 | 0.8680 | 512 |
| 5 | 0.9570 | 0.9200 | 640 |
| 6 | 0.9750 | 0.9660 | 768 |
| 7 | 0.9860 | 0.9780 | 896 |
| 8 | **0.9950** | 0.9860 | 1024 |

**The answer: no supported width reaches 0.99 on GIST-960d.** GloVe needs **8
bits**; GIST does not get there at 8 and the increments are decaying (0.0180,
0.0120, 0.0080 over the last three widths), so 9 or 10 bits would be needed —
which is past the point where quantization is the reason the channel exists.

Against the budget derived in `doc/specs/VECTOR_CHANNEL.md` §2.1.1 — about 855 B
per vector, i.e. 6.68 bits per coordinate at 1024-d, for `size <= 0.15x pgvector
HNSW` — 8 bits is 1024 B, or **0.18x**. So the width that clears 0.99 on the
easier corpus already misses the storage claim, and the harder corpus is not
reachable at all.

**`recall@10 >= 0.99` and `size <= 0.15x HNSW` are therefore jointly unsatisfiable
with a single code width.** That was the question the phase turned on, and it is
now answered by measurement rather than extrapolation. (The extrapolation in
§2.1.1 predicted ~7 bits for GloVe and "possibly unreachable at 8" for GIST. It
was optimistic on GloVe by one width and right about GIST — which is the argument
for having run it.)

## The reprieve: an exact rerank of a top-100 window

`R_w100` re-scores the top 100 compressed-domain candidates at **full precision**
and returns the best 10:

| bits | GloVe `R_w10` -> `R_w100` | GIST `R_w10` -> `R_w100` |
|---|---|---|
| 2 | 0.7345 -> 0.9985 | 0.6130 -> 0.9890 |
| 3 | 0.8515 -> **1.0000** | 0.7880 -> **1.0000** |
| 4 | 0.9225 -> 1.0000 | 0.8680 -> 1.0000 |
| 8 | 0.9950 -> 1.0000 | 0.9860 -> 1.0000 |

**Three bits plus an exact rerank of 100 candidates is 1.0000 on both corpora** —
better than 8 bits alone, at 384 B per vector instead of 1024.

This is not a contradiction of §2.1.1's rule that "a rerank cannot lift recall
above its own compressed-domain ceiling". That rule is about a rerank done in a
*b*-bit representation. This rerank is done at float32, so the ceiling that binds
is float32's, which is 1.0.

The rule does, however, mean the rerank data cannot be a cheap quantized sidecar:
it has to be full precision. And a stored full-precision sidecar costs `4 * dim`
= 4,096 B/vector, which is worse than HNSW. **So the only shape in which both
claims survive is one where the rerank reads full precision from somewhere the
index does not pay for — the heap.** The original vector is already in the table.

That converts the storage problem into a latency problem:

- index bytes: 3-bit codes only, **384 B/vector = 0.067x HNSW** at 1024-d, well
  inside the 0.15x budget;
- query cost: up to 100 heap fetches per query, against `p50 <= 2x pgvector HNSW`.

**Unmeasured, and it is the next thing to measure.** 100 random heap fetches is
plausibly a few hundred microseconds warm and several milliseconds cold, so the
claim is not obviously safe — and a cold-cache result is the one that matters.

## What the converged codebook changed

2- and 3-bit codebooks were already at their fixed point and reproduce the old
figures exactly (0.7345, 0.8515, 0.6130, 0.7880 — to four decimals). Only 4 bits
moved, and it moved **in opposite directions on the two corpora**:

| | published (unconverged) | measured (converged) | delta |
|---|---|---|---|
| GloVe 4-bit | 0.9205 | 0.9225 | +0.0020 |
| GIST 4-bit | 0.8780 | 0.8680 | **-0.0100** |

A correct codebook minimizes mean squared quantization error, which is not the
same objective as recall@10, so a lower-MSE codebook is not obliged to score
better on a ranking metric. GIST's queries are 100, so recall@10 has granularity
0.001 and -0.0100 is ten result slots — small, but it is a *decrease*, and
recording only the corpus where the fix helped would be exactly the reporting bias
this project has a rule against.

Neither delta disturbs any conclusion: both remain far below 0.99, so "4 bits
alone does not reach 0.99" holds, and the Phase V gate reopening that followed
from it stands.

## Caveats

- 200k x 200-d and 100k x 960-d, not the gate's 1M x 1024-d Cohere-wiki. The
  *quantization* ceiling is a property of the codebook and the estimator and is
  the robust half; the corpus geometry still differs from the gate's.
- k=10, cosine only.
- **Widths 5-8 have no SIMD kernel.** The wide and AVX2 paths pack 8 lanes into a
  32-bit word, so they are structurally limited to 4 bits and anything wider falls
  back to the scalar oracle (`KERNEL_GROUP_BITS_MAX`, `src/vector/kernels.c`).
  Recall is unaffected — it is a property of the codes, not of the kernel — but any
  *latency* number at 5-8 bits would be a scalar-path number. This is a further
  argument for the 3-bit-plus-rerank shape: 3 bits keeps the SIMD path.
- `lists=512` throughout. Irrelevant to the full-probe column by construction, and
  the lower-`nprobe` rows are in the raw logs for anyone who wants the probe curve.
