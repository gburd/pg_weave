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

That is the *maximum-recall* shape, and it is **not** the recommended one. The
window sweep below shows 100 candidates is 5x more than `recall@10 >= 0.99`
requires, and the window is what the query cost is proportional to. Do not quote
this paragraph as the conclusion; the frontier two sections down is the conclusion.

This is not a contradiction of §2.1.1's rule that "a rerank cannot lift recall
above its own compressed-domain ceiling". That rule is about a rerank done in a
*b*-bit representation. This rerank is done at float32, so the ceiling that binds
is float32's, which is 1.0.

The rule does, however, mean the rerank data cannot be a cheap quantized sidecar:
it has to be full precision. And a stored full-precision sidecar costs `4 * dim`
= 4,096 B/vector, which is worse than HNSW. **So the only shape in which both
claims survive is one where the rerank reads full precision from somewhere the
index does not pay for — the heap.** The original vector is already in the table.

That converts the storage problem into a latency problem, and the latency scales
with the window — which is why the window itself needed measuring.

## The window sweep: 100 is 5x more than the gate asks for

Added 2026-09-12, same harness, `lists=512 probes=512` (full probe), k=10,
self-check PASS on both corpora. Only windows 10 and 100 had ever been run, and
`R_w10` is a no-op for recall@10 by construction — exact rescoring of the top 10
cannot introduce an eleventh document, so `R_w10` *is* the compressed-domain
recall. Everything decision-relevant was in the gap.

GloVe-200d:

| bits | w10 | w15 | w20 | w25 | w30 | w40 | w50 | w75 |
|---|---|---|---|---|---|---|---|---|
| 2 | 0.7345 | 0.8535 | 0.9070 | 0.9315 | 0.9500 | 0.9730 | 0.9820 | **0.9965** |
| 3 | 0.8515 | 0.9600 | 0.9820 | **0.9930** | 0.9975 | 1.0000 | 1.0000 | 1.0000 |
| 4 | 0.9225 | **0.9915** | 0.9970 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
| 5 | 0.9570 | **0.9990** | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
| 6 | 0.9750 | **1.0000** | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |

GIST-960d, which binds throughout:

| bits | w10 | w15 | w20 | w25 | w30 | w40 | w50 | w75 |
|---|---|---|---|---|---|---|---|---|
| 2 | 0.6130 | 0.7320 | 0.8040 | 0.8510 | 0.8790 | 0.9180 | 0.9440 | 0.9790 |
| 3 | 0.7880 | 0.9100 | 0.9570 | 0.9780 | 0.9860 | **0.9950** | 0.9980 | 1.0000 |
| 4 | 0.8680 | 0.9780 | **0.9940** | 0.9990 | 0.9990 | 1.0000 | 1.0000 | 1.0000 |
| 5 | 0.9200 | **0.9960** | 0.9990 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
| 6 | 0.9660 | **1.0000** | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |

**Window 100 buys 1.0000. The gate asks for 0.99**, and the whole cost of this
shape is linear in the window, so the difference is the entire decision.

## The frontier, priced

Rerank I/O comes from `bench/RESULTS_RERANK_IO.md`: **2.388 page reads per
candidate** at 1024-d out of line, measured. Index bytes are `bits * 128` per
vector at 1024-d, against the ~855 B that 0.15x of pgvector HNSW allows.

| bits | window @ 0.99 | index B/vector | x HNSW | page reads/query | SIMD kernel |
|---|---|---:|---:|---:|---|
| 2 | > 75 | 256 | 0.045 | > 179 | yes |
| 3 | 40 | 384 | 0.067 | 96 | yes |
| **4** | **20** | **512** | **0.090** | **48** | **yes** |
| 5 | 15 | 640 | 0.112 | 36 | no |
| 6 | 15 | 768 | 0.135 | 36 | no |

**Every width from 3 to 6 clears the 0.15x storage budget once the rerank comes
from the heap.** Storage stops being the binding constraint and I/O becomes it, so
the right width is the one that minimizes reads while staying inside the budget —
the opposite of the "narrowest width that fits" instinct the earlier analysis had.

**The recommended shape is 4 bits plus an exact rerank of a top-20 window:**

- recall@10 **0.9940** (GIST) and 0.9970 (GloVe), both over the gate;
- index **512 B/vector = 0.090x HNSW**, comfortably inside 0.15x;
- **48 random page reads per query**, a 5x cut from the 3-bit/100-window shape;
- and it is the **widest width that keeps the SIMD kernel** (5-8 bits fall back to
  the scalar oracle — see the caveat below). Five bits saves 12 page reads per
  query and gives up the vectorized code scan to do it, which is the wrong trade
  when the code scan runs over every vector and the rerank runs over twenty.

Two bits is now clearly out: it does not reach 0.99 even at window 75 on GIST
(0.9790), so the cheapest codes are not on the frontier at all.

**Latency is still unmeasured, and 48 reads is not automatically safe.** Detoasting
is serialized in PostgreSQL 17 (`bench/RESULTS_RERANK_IO.md`), so 48 reads is on
the order of 17 ms cold on EBS gp3 — probably still over `p50 <= 2x pgvector HNSW`.
The 5x cut moves the problem within reach of a modest prefetch rather than
requiring an ambitious one; it does not by itself close the gate.

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
- **The window figures are the fragile half, and they are the ones the frontier
  rests on.** A window must be large enough to contain the true top-10 after
  quantization perturbs the ordering, and there is no reason that requirement is
  invariant in corpus size: 1M vectors put ~10x more near-neighbours in range to
  displace them. So `window @ 0.99` measured at n = 100k-200k is a **lower bound**
  on the gate corpus, and the recommended shape's 20 could be 30 or 50 at 1M. This
  needs re-measuring at n = 1M before the shape is committed to an on-disk format,
  because the page-read budget scales with it one-for-one.
- k=10, cosine only.
- **Widths 5-8 have no SIMD kernel.** The wide and AVX2 paths pack 8 lanes into a
  32-bit word, so they are structurally limited to 4 bits and anything wider falls
  back to the scalar oracle (`KERNEL_GROUP_BITS_MAX`, `src/vector/kernels.c`).
  Recall is unaffected — it is a property of the codes, not of the kernel — but any
  *latency* number at 5-8 bits would be a scalar-path number. This is what rules 5
  and 6 bits off the frontier above despite their smaller rerank windows, and it is
  why the recommended shape is 4 bits rather than 5.
- `lists=512` throughout. Irrelevant to the full-probe column by construction, and
  the lower-`nprobe` rows are in the raw logs for anyone who wants the probe curve.
