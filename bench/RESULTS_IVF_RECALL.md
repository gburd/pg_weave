# Result: is Phase V's `recall@10 >= 0.99` reachable with an IVF over pg_weave's own codes?

Date: 2026-09-10. Harness: `bench/ivf_recall.c`. No PostgreSQL backend; the
shipping codec (`src/vector/quantize.c`, `src/vector/pack.c`) is linked in
unmodified and nothing in `src/` was touched. Reproduce with

```
gcc -O2 -Wall -Wextra -I include -o /scratch/ivf_recall bench/ivf_recall.c \
    src/vector/quantize.c src/vector/pack.c -lm

# real text embeddings, natively 200-d
/scratch/ivf_recall glove /scratch/pgw-v9-corpus/glove.6B.200d.txt 200000 200 \
    lists=256,512,1024 bits=2,3,4 probes=1,2,4,8,16,32,64,128,256,512 \
    windows=10,100,1000 k=10 iters=12 sampleper=32

# real image descriptors, natively 960-d
/scratch/ivf_recall fvecs /scratch/pgw-v9-corpus/gist/gist_base.fvecs 100000 100 \
    lists=128,512 bits=2,3,4 probes=1,2,4,8,16,32,64,128,256 \
    windows=10,100,1000 k=10 iters=10 sampleper=24

# the extra arms: one more lists point at 960-d, one more at 200-d, and the
# TQ+ calibration contrast (lists is irrelevant to the full-probe row that
# isolates quantization, so those two use a cheap lists and probes=1)
/scratch/ivf_recall fvecs /scratch/pgw-v9-corpus/gist/gist_base.fvecs 100000 100 \
    lists=1024 bits=4 probes=1,2,4,8,16,32,64,128,256,512 \
    windows=10,100,1000 k=10 iters=8 sampleper=12
/scratch/ivf_recall glove /scratch/pgw-v9-corpus/glove.6B.200d.txt 200000 200 \
    lists=2048 bits=4 probes=1,2,4,8,16,32,64,128,256,512,1024 \
    windows=10,100,1000 k=10 iters=12 sampleper=16
/scratch/ivf_recall glove /scratch/pgw-v9-corpus/glove.6B.200d.txt 200000 200 \
    lists=16 bits=2,3,4 probes=1 windows=10,100,1000 iters=8 calib=1
/scratch/ivf_recall fvecs /scratch/pgw-v9-corpus/gist/gist_base.fvecs 100000 100 \
    lists=16 bits=2,4 probes=1 windows=10,100,1000 iters=8 calib=1
```

Every configuration is deterministic — one xoshiro256** stream, seeded by
`seed=`, drives the k-means sample, the initial centroids, the empty-cluster
re-seeds and the base/query split — so a rerun reproduces a row exactly. That was
checked rather than assumed: `lists=256 bits=4` on GloVe was re-run against the
committed source after the harness gained its `calib=` arm and reproduced
0.9555 / 0.9885 / 1.0000 and 0.8920 / 0.9130 / 0.9205 **bit-identically**.

The corpora are not in the repository (they are 693 MB and 2.6 GB); they were
fetched into `/scratch/pgw-v9-corpus/`, per `AGENTS.md`'s rule that everything
which is not the repository lives in `/scratch`:

```
curl -LO https://nlp.stanford.edu/data/glove.6B.zip   # unzip glove.6B.200d.txt
curl -O ftp://ftp.irisa.fr/local/texmex/corpus/gist.tar.gz   # tar xzf, use gist_base.fvecs
```

This measurement was taken **before** V7's on-disk work, on purpose, and for the
same reason `bench/RESULTS_BOUND_PRUNING.md` was taken before the fused scorer:
`doc/PHASES.md` task V9 chose an IVF coarse quantizer specifically to reach
Phase V's `recall@10 >= 0.99` gate, `doc/specs/VECTOR_CHANNEL.md` §8a records in
so many words that nobody had measured whether that is reachable on *our* codes
and *our* corpus geometry, and pg_turbovec had already measured a hard
probe-count recall ceiling on an IVF of their own. Per `AGENTS.md` rule 9, that
is the thing the design rests on, so it was measured before anything was built
on it. It cost an afternoon.

**What pg_turbovec's numbers do and do not say about ours.** Their ceiling
(0.846/0.906/0.954/0.978/0.984 at probes 8/16/32/64/128, `lists = 512`) is
measured on a **1-bit corpus-mean-centered sign code**. This harness scores with
the **2–4 bit rotated Lloyd–Max codebook** of `doc/specs/VECTOR_CHANNEL.md` §4 —
a different code, a different distribution assumption, a different error
magnitude. Their *values* therefore do not transfer and no number below is
compared to theirs. What transfers is the *mechanism*, which is about which cells
get visited and not about how a visited cell is scored: it is bit-width agnostic
by construction. That mechanism is what this harness set out to quantify here.

## Corpus provenance — read this before any number

The authority of everything below depends on this section, so it comes first.

Nothing in this repository or on this host shipped an embedding corpus:
`bench/corpus.sql` generates *lexical* text for BM25 and has no vectors, `bench/`
has no vector fixture, and the P1 corpora named in `doc/PHASES.md` (Wikipedia
20231101.en, MS MARCO, Cohere-wiki, BEIR) were not present — the only local
HuggingFace cache held BeIR **text** (nfcorpus, scifact) with no embedding model
installed to encode it. Rather than fall back to uniform random vectors, which
have no cluster structure and would make an IVF measurement worthless in either
direction, two **real** corpora were downloaded and used:

| corpus | what it is | rows used | native dim | metric |
|---|---|---:|---:|---|
| **GloVe 6B 200d** (`nlp.stanford.edu/data/glove.6B.zip`) | real text embeddings — 400k-word GloVe, the 200-d model | 200,000 base + 200 held-out queries | 200 | cosine |
| **GIST-1M** (`ftp.irisa.fr/local/texmex/corpus/gist.tar.gz`) | real image descriptors, the TEXMEX/ann-benchmarks GIST base set | 99,999 base + 100 held-out queries | 960 | cosine |

Both are read at their **native width**. Nothing here is a prefix slice of a
wider embedding: `doc/specs/VECTOR_CHANNEL.md` §11 item 3 caveats pg_turbovec's
dimension sweep exactly that way (its 256-d and 512-d arms are slices of a 1024-d
model, making its low-dim penalty an upper bound rather than a calibrated
number), and importing that flaw would have made the dimension contrast here
worthless too.

Why these two: GloVe 200-d is named in `doc/specs/VECTOR_CHANNEL.md` §11 item 6
as a corpus Phase V owes a measurement on, and as *"the documented worst case for
the asymptotic-Beta assumption the whole codebook rests on"*. GIST 960-d is the
corpus §8a's IVF-beats-graph result was measured on, and at 960-d it is the
closest real corpus available to Phase V's own 1024-d gate corpus. Neither is
Cohere-wiki, which is the gate corpus and is still owed (see "What this does not
tell us").

**A third, clearly-labelled arm** (`synth` mode) generates explicit cluster
structure — *nclust* isotropic Gaussian blobs at a stated sigma — and labels
every row it produces `synthetic`. It exists as the sanitizer fixture and as a
contrast, and it is reported separately below precisely because it makes IVF look
far better than either real corpus does. That gap is the reason the requirement
to use real geometry is not a formality.

Deviations from each dataset's own convention, stated because they change the
neighbour sets: rows are **L2-normalized** and the metric is **inner product**
(= cosine), which on unit vectors ranks identically to L2, and which is the
metric this channel's compressed-domain estimator is built for. Ground truth is
therefore **recomputed by brute force here** rather than taken from GIST's
shipped `gist_groundtruth.ivecs`, whose neighbours are un-normalized L2. Queries
are held out of the base set, and base/query split is a deterministic shuffle
rather than a head/tail cut, because GloVe rows are frequency-ordered and a tail
split would query rare words against a base of common ones.

## What is measured, and the three arms

Ground truth is exact top-10 by brute force over every base row, in double
precision. Work is reported as **`cand`, the number of candidate vectors
scored**, per query, plus `candfrac = cand / nbase`. It is a count, not a
wall-clock: a count reproduces on any host, and this box was under a load average
of 30–44 from unrelated work for the whole run, which would have made every
latency here fiction. Same discipline as counting buffer hits in the lexical
channel.

Every row reports recall@10 three ways **over the same probed candidate set**, so
the loss can be attributed instead of merely observed:

| column | how | what its error consists of |
|---|---|---|
| `cellrec` | fraction of the true top-10 whose cluster is among the probed ones. No scoring at all. | probe-miss only |
| `R_exact` | probed candidates rescored with exact float inner products | probe-miss only |
| `R_w10` | probed candidates scored **only** in the compressed domain by `weave_lut_score_code()`, top 10 returned. No rerank. | probe-miss + quantization |
| `R_w100`, `R_w1000` | top 100 / top 1000 by compressed-domain score, those rescored exactly, best 10 returned | probe-miss + quantization surviving a rerank window |

`cellrec` and `R_exact` agree to the fourth decimal in every row of every table
below. That is expected — exact rescoring of probed candidates returns the top-10
*of* the probed set, so a true neighbour in a probed cell is necessarily
returned — and it is useful anyway, because the two are computed by completely
independent routes (one from the assignment array, one from actual float
scoring). Their agreement is a second, free consistency check on the harness.

**The self-check, run before any number was recorded** (`AGENTS.md` rule 8;
pg_turbovec had to retract a headline number that came from a fast-but-wrong
path). At `nprobe == lists` the harness probes every cluster, so it is brute
force by another route, and it asserts **per query** that `R_exact == 1.000`,
`cellrec == 1.000`, and `cand == nbase`. It exits non-zero on the first
violation. Every run reported here printed:

```
# nprobe==lists self-check: PASS (exact rescoring at full probe reproduces brute force for every query)
```

Recall is tie-tolerant — a retrieved vector counts as a hit if its exact score is
`>= ` the 10th-best exact score, which is the ann-benchmarks convention. Without
it a *correct* full scan reports recall < 1.000 whenever two corpus rows are
equidistant from a query, and the self-check above would fire as a false alarm.
Real corpora do contain such rows, and worse: GIST's first 100,100 rows contain 1
all-zero descriptor and its first 200,100 contain 5. `weave_encode()` refuses a
zero vector by design (it has no direction and no defined renormalization scale),
so the harness drops those rows, reports how many, and reduces `nbase` — which is
why the GIST arms below say 99,999 and not 100,000.

The harness also compiles clean under `-Wall -Wextra` and runs clean under
`-fsanitize=address,undefined` (small `synth` configuration, all three bit widths).

## GloVe 6B 200d — 200,000 rows, 200 queries, k = 10

The probe ceiling is independent of bit width, since `cellrec` involves no codes
at all. One column per `lists`, and the work it costs:

| nprobe | 256: candfrac | 256: ceiling | 512: candfrac | 512: ceiling | 1024: candfrac | 1024: ceiling | 2048: candfrac | 2048: ceiling |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.006 | 0.3680 | 0.003 | 0.4065 | 0.002 | 0.3820 | 0.001 | 0.3370 |
| 2 | 0.011 | 0.4970 | 0.006 | 0.5120 | 0.003 | 0.4990 | 0.002 | 0.4520 |
| 4 | 0.021 | 0.6220 | 0.011 | 0.6225 | 0.006 | 0.5985 | 0.004 | 0.5620 |
| 8 | 0.040 | 0.7245 | 0.020 | 0.7220 | 0.011 | 0.6940 | 0.007 | 0.6490 |
| 16 | 0.077 | 0.8240 | 0.039 | 0.8000 | 0.021 | 0.7700 | 0.014 | 0.7285 |
| 32 | 0.147 | 0.8960 | 0.075 | 0.8830 | 0.040 | 0.8370 | 0.026 | 0.8045 |
| 64 | 0.283 | 0.9555 | 0.145 | 0.9285 | 0.076 | 0.9075 | 0.050 | 0.8665 |
| 128 | 0.542 | 0.9885 | 0.282 | 0.9700 | 0.148 | 0.9530 | 0.096 | 0.9195 |
| 256 | 1.000 | 1.0000 | 0.542 | 0.9925 | 0.285 | 0.9790 | 0.178 | 0.9565 |
| 512 | — | — | 1.000 | 1.0000 | 0.546 | 0.9940 | 0.331 | 0.9830 |
| 1024 | — | — | — | — | 1.000 | 1.0000 | 0.596 | 0.9970 |
| 2048 | — | — | — | — | — | — | 1.000 | 1.0000 |

The first cell at or above 0.99 is `lists=256, nprobe=256` (**candfrac 1.000**),
`lists=512, nprobe=256` (**candfrac 0.542**), `lists=1024, nprobe=512`
(**candfrac 0.546**), `lists=2048, nprobe=1024` (**candfrac 0.596**). An eightfold
increase in `lists` does not move the corpus fraction that 0.99 costs; the
`lists=2048` arm, trained on 16 sample rows per centroid rather than 32, is if
anything slightly worse.

**The cliff is between 0.98 and 0.99, and it is steep.** At `lists=2048`, recall
0.9830 costs candfrac 0.331 and 0.9970 costs 0.596 — so the last 1.4 points of
recall cost 80 % more candidates than everything before them. A gate written at
0.98 and a gate written at 0.99 are not the same engineering problem on this
corpus.

Compressed-domain recall, i.e. what the channel returns with no rerank sidecar
(`R_w10`), at `lists=1024`:

| nprobe | candfrac | ceiling | bits=2 | bits=3 | bits=4 |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.002 | 0.3820 | 0.3475 | 0.3645 | 0.3745 |
| 8 | 0.011 | 0.6940 | 0.5845 | 0.6475 | 0.6735 |
| 32 | 0.040 | 0.8370 | 0.6655 | 0.7620 | 0.8010 |
| 64 | 0.076 | 0.9075 | 0.7015 | 0.8080 | 0.8630 |
| 128 | 0.148 | 0.9530 | 0.7185 | 0.8315 | 0.8935 |
| 256 | 0.285 | 0.9790 | 0.7275 | 0.8415 | 0.9085 |
| 512 | 0.546 | 0.9940 | 0.7335 | 0.8490 | 0.9170 |
| **1024 = lists** | **1.000** | **1.0000** | **0.7345** | **0.8515** | **0.9205** |

The last row is the whole corpus scanned with zero probe-miss, so **its gap below
1.000 is pure quantization error**: 0.2655 at 2 bits, 0.1485 at 3, 0.0795 at 4.

Rerank window at `lists=1024`, bits=4 (the same probed sets, three windows):

| nprobe | ceiling | `R_w10` | `R_w100` | `R_w1000` |
|---:|---:|---:|---:|---:|
| 64 | 0.9075 | 0.8630 | 0.9075 | 0.9075 |
| 256 | 0.9790 | 0.9085 | 0.9790 | 0.9790 |
| 512 | 0.9940 | 0.9170 | 0.9940 | 0.9940 |
| 1024 | 1.0000 | 0.9205 | 1.0000 | 1.0000 |

A window of 100 recovers the quantization loss **completely** and never exceeds
the probe ceiling. That is the two-different-failure-modes mechanism of §8a,
reproduced on this project's own codebook rather than assumed from
pg_turbovec's.

## GIST-1M 960d — 99,999 rows, 100 queries, k = 10

| nprobe | lists=128 candfrac | lists=128 ceiling | lists=512 candfrac | lists=512 ceiling | lists=1024 candfrac | lists=1024 ceiling |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.018 | 0.3860 | 0.005 | 0.2780 | 0.004 | 0.2300 |
| 2 | 0.034 | 0.5720 | 0.010 | 0.4290 | 0.007 | 0.3730 |
| 4 | 0.065 | 0.7520 | 0.019 | 0.5840 | 0.014 | 0.5280 |
| 8 | 0.130 | 0.8890 | 0.036 | 0.7290 | 0.026 | 0.6570 |
| 16 | 0.243 | 0.9620 | 0.071 | 0.8740 | 0.050 | 0.8130 |
| 32 | 0.451 | 0.9930 | 0.134 | 0.9530 | 0.093 | 0.9190 |
| 64 | 0.755 | 1.0000 | 0.253 | 0.9900 | 0.175 | 0.9760 |
| 128 | 1.000 | 1.0000 | 0.453 | 0.9980 | 0.311 | 0.9970 |
| 256 | — | — | 0.754 | 1.0000 | 0.526 | 1.0000 |
| 512 | — | — | 1.000 | 1.0000 | 0.797 | 1.0000 |
| 1024 | — | — | — | — | 1.000 | 1.0000 |

First cell at or above 0.99: `lists=128, nprobe=32` (**candfrac 0.451**),
`lists=512, nprobe=64` (**candfrac 0.253**), `lists=1024, nprobe=128`
(**candfrac 0.311**). More lists helps from 128 to 512 and then **does not** from
512 to 1024 — but the `lists=1024` arm was trained on 12 sample rows per centroid
against 24 for the other two (a runtime concession under host load), so that
reversal is confounded with a weaker partition and must not be read as a property
of `lists`. It is a reason to distrust the trend, not evidence of a floor.

Compressed-domain recall and rerank, `lists=512`:

| nprobe | candfrac | ceiling | `R_w10` b2 | `R_w10` b3 | `R_w10` b4 | `R_w1000` b4 |
|---:|---:|---:|---:|---:|---:|---:|
| 16 | 0.071 | 0.8740 | 0.5910 | 0.7330 | 0.8000 | 0.8740 |
| 32 | 0.134 | 0.9530 | 0.6070 | 0.7720 | 0.8500 | 0.9530 |
| 64 | 0.253 | 0.9900 | 0.6120 | 0.7860 | 0.8730 | 0.9900 |
| 128 | 0.453 | 0.9980 | 0.6120 | 0.7870 | 0.8760 | 0.9980 |
| **512 = lists** | **1.000** | **1.0000** | **0.6130** | **0.7880** | **0.8780** | **1.0000** |

Pure quantization error at full probe: **0.387 at 2 bits, 0.212 at 3, 0.122 at
4**. Worse than GloVe at every width, on a corpus 4.8× wider, which is the
opposite direction from the usual "more dimensions, more forgiving" intuition and
is not explained by anything measured here.

## The two error sources, separated

This is the analytically load-bearing table. Probe-miss is read at a fixed
recall target; quantization is read at `nprobe == lists`, where probe-miss is
zero by construction.

| corpus | dim | probe-miss: candfrac needed for ceiling >= 0.99 | quantization-only recall@10 at full probe, no rerank (bits 2/3/4) | quantization-only, rerank window 100 |
|---|---:|---:|---|---:|
| GloVe 6B | 200 | 0.542 (lists=512), 0.546 (1024), 0.596 (2048) | 0.7345 / 0.8515 / 0.9205 | 0.9985 (b2), 1.0000 (b3, b4) |
| GIST-1M | 960 | 0.451 (lists=128), 0.253 (512), 0.311 (1024) | 0.6130 / 0.7880 / 0.8780 | 0.9890 (b2), 1.0000 (b3, b4) |

A third free consistency check falls out of that middle column: the full-probe
quantization ceiling is **bit-identical across every `lists` value** on both
corpora (GloVe 0.7345/0.8515/0.9205 at lists 256, 512, 1024 and 0.9205 at 2048;
GIST 0.6130/0.7880/0.8780 at lists 128, 512, 1024). It has to be — probing every
cell is a full scan, and a full scan does not know how the corpus was partitioned
— and any drift there would have meant the partition was leaking into the scored
set. At 2 bits the rerank window has to widen to 1000 to reach 1.0000 on both
corpora; at 3 and 4 bits a window of 100 suffices.

Read together, they say two independent things, and conflating them would blame
the wrong component:

1. **Probe-miss alone puts 0.99 at a quarter to 60 % of the corpus scanned.** No
   `lists` value tested on either corpus reaches 0.99 while scanning a small
   fraction. On GloVe the fraction is flat across `lists` 256→2048; on GIST it
   improves 0.451→0.253 from 128 to 512 lists and then worsens to 0.311 at 1024,
   where the partition was trained on half the sample per centroid. The trend is
   worth pushing further before concluding anything about the achievable floor.
2. **Quantization alone puts 0.99 out of reach at every probe count, unless an
   exact rerank pass is added.** The best no-rerank number measured anywhere in
   this sweep is **0.9205** (GloVe, 4 bits, whole corpus scanned). A rerank
   window of 100 closes that gap entirely — but reranking requires
   full-precision vectors, i.e. the `WEAVE_VRERANK` sidecar of
   `doc/specs/VECTOR_CHANNEL.md` §7, whose storage cost is the thing the
   quantizer exists to avoid, and which §11 item 4 already lists as an honest
   limit.

Point 2 bears directly on a claim in this project's own documents.
`include/weave/quantize.h` and §2 state that the renormalization trick makes the
compressed-domain estimator unbiased and that this *"removes the need for a
float32 rerank pass at moderate k"*. At k = 10 on both real corpora, it does not:
unbiased is not low-variance, and at k = 10 the score gap between the 10th and
11th true neighbour is far smaller than the estimator's spread. The claim is not
contradicted at every k — it was not tested at large k — but as stated for
moderate k it is not what these two corpora show.

**TQ+ calibration was measured too, so it cannot be offered as the unmeasured
fix.** §5 predicts calibration matters most where the asymptotic-Beta assumption
is weakest, i.e. at low dimension. Fitted per `weave_calibration_fit()` over 8192
rows, at full probe (`calib=1`):

| corpus | bits | uncalibrated | calibrated |
|---|---:|---:|---:|
| GloVe 200-d | 2 | 0.7345 | 0.7140 |
| GloVe 200-d | 3 | 0.8515 | 0.8160 |
| GloVe 200-d | 4 | 0.9205 | 0.8620 |
| GIST 960-d | 2 | 0.6130 | 0.6160 |
| GIST 960-d | 4 | 0.8780 | 0.6850 |

Calibration **hurt** on the corpus where §5 expected it to help most, was neutral
at 960-d / 2 bits, and cost 0.19 recall at 960-d / 4 bits. It moves the
quantization ceiling the wrong way at three of the four points measured, so it is
not the unmeasured fix for the finding above. That is a measured property of the
shipping fit as called from this harness — a one-shot per-coordinate affine map
fitted over 8192 rotated rows — recorded here without a fix;
`src/vector/quantize.c` was not modified. Whether the fit itself, its 8192-row
sample, or the harness's use of it is at fault is not established by this
measurement, and it should not be read as a bug report against the fit.

## Synthetic-geometry contrast — why the corpus choice was not a formality

`synth` mode, 40 isotropic Gaussian blobs at sigma = 0.30, d = 128, 4000 rows,
lists = 32, 4 bits, 40 queries. **Synthetic geometry, not an embedding corpus:**

| nprobe | candfrac | ceiling | `R_w10` | `R_w100` |
|---:|---:|---:|---:|---:|
| 1 | 0.043 | **1.0000** | 0.7250 | 1.0000 |
| 4 | 0.244 | 1.0000 | 0.7250 | 1.0000 |
| 32 = lists | 1.000 | 1.0000 | 0.7250 | 1.0000 |

Probing **one** cluster out of 32 already has zero probe-miss, because a blob's
members are each other's only near neighbours. Any IVF looks perfect on this
corpus at any probe count, and a sweep run only on generated blobs — or on
uniform random vectors, where the failure is the mirror image — would have
supported outcome 1 with no evidence at all.

## Which outcome the data supports

Stated in the terms this task set out, with the same prominence a positive result
would get (`AGENTS.md` rule 8). No statistics are computed here and no headline
number is claimed; that is the coordinator's call.

- **Outcome 1 (0.99 at a small fraction of the corpus) is not supported by any
  cell in this sweep.** The cheapest cell reaching 0.99 anywhere is GIST
  `lists=512, nprobe=64`, at **25.3 % of the corpus scanned** and only with an
  exact rerank window. That is not the "small fraction" outcome 1 describes.
- **Outcome 2 is what the data supports, with a qualification.** 0.99 is
  reachable — but only (a) at a probe count scanning 25–60 % of the corpus, and
  (b) with an exact rerank window of ~100 over full-precision vectors. Both
  conditions have to hold. Under those conditions IVF is buying a **1.7–4×**
  reduction in candidates scored (candfrac 0.253 at best, 0.596 at worst), not
  the order of magnitude V9's design assumes, and it is buying it while paying
  the rerank sidecar's storage — which interacts with V9's own
  `storage <= 0.15x pgvector HNSW` gate.
- **Outcome 3 holds for the specific configuration §2 describes** — compressed
  domain only, no float32 rerank. In that configuration **0.99 is not reachable
  at any probe count, including probing every cluster**, on either real corpus at
  any bit width 2–4. The ceiling is 0.9205 (GloVe, 4 bits) and 0.8780 (GIST, 4
  bits), and probing more cells cannot raise it because it is not probe-miss
  error.

The `lists` trend on GIST (candfrac at 0.99 of 0.451 → 0.253 → 0.311 as lists
goes 128 → 512 → 1024) is the one result here that could move outcome 2 toward
outcome 1, and it is also the least trustworthy number in this document, because
the 1024 arm's k-means got half the training sample the other two did. The
cheapest next measurement is therefore to re-run the `lists` arm at 960-d with an
equal and larger training budget per centroid: if candfrac at 0.99 keeps falling
with lists, the design question changes from "is IVF sublinear enough" to "how
many lists does 0.99 need, and what does searching that many centroids cost".
Nothing in this sweep answers that.

## What this does not tell us

- **Not the gate corpus.** Phase V's gate is 1M × 1024-d Cohere-wiki. This is
  200k × 200-d GloVe and 100k × 960-d GIST, because no embedding corpus was
  present locally and Cohere-wiki ships as Parquet with no reader available here.
  Recall-vs-probe curves are corpus-specific; two real corpora agreeing on the
  *shape* is evidence, not a substitute.
- **Not 1M rows.** Both arms are 100k–200k. §8a already quotes pg_turbovec's
  guidance that below ~1M their flat scan beat their IVF outright and that above
  1M is unmeasured, so the scale gap cuts in the direction of caution: an IVF's
  candidate fraction at fixed recall can improve with corpus size, and this
  sweep cannot see that.
- **No latency, and deliberately so.** Every number is a count. The host was
  under load average 30–44 from unrelated work throughout, and V9's gate also
  demands a p50 within 2× of pgvector HNSW at matched recall — which this says
  nothing about. §8a's note that HNSW itself never reached 0.99 on
  pg_turbovec's 500k × 1024-d corpus remains unresolved and is still the right
  thing to settle before that comparison is used as a gate.
- **The k-means is a harness k-means.** Lloyd's algorithm, random init,
  8–12 iterations, a sample of 12–32 rows per centroid (12 for GIST lists=1024
  and 16 for GloVe lists=2048, both runtime concessions under host load), empty
  clusters re-seeded onto a random sample row. A better-trained partition
  (k-means++ init, more iterations, a larger sample) should *raise* the ceiling
  curves rather than lower them, so every probe-miss number here is likely
  pessimistic by an unmeasured amount. That is the single largest methodological
  caveat in this document, and closing it is cheap: re-run with `iters=` and
  `sampleper=` raised and see whether the candfrac at 0.99 moves. The two arms
  with the smaller training budget are called out where they appear, because they
  are the two that break an otherwise monotone trend.
- **The work proxy excludes the centroid scan.** `cand` counts candidate vectors
  only; a real query also computes `lists` centroid distances to order the probes.
  That is at most 2 % of `cand` in every cell here (2048 centroids against
  ~119,000 candidates at the worst-case cell) and it is *not* negligible for a
  configuration with far more lists, which is exactly the direction the GIST trend
  points. A future large-`lists` arm has to count it.
- **k = 10 only, one metric.** Cosine on normalized rows. The quantization
  finding in particular is a k = 10 finding; §2's no-rerank claim may well hold
  at larger k, which is untested here.
- **One host, one seed per configuration.** Recall figures are means over 100–200
  queries (1,000–2,000 neighbour slots), so the fourth decimal place is not
  meaningful; the harness prints four only so nothing is lost to rounding before
  the coordinator aggregates.
