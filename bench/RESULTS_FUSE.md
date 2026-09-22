# Fused top-k vs RRF over-fetch — first real-corpus measurement

**Date:** 2026-09-22 · **Commit:** d30ee5c · **Extension:** 0.19.0 · **PostgreSQL:** 17
**Host:** EC2 `c7i.8xlarge` (32 vCPU, 64 GB), us-east-2, gp3 8000 IOPS / 500 MB/s
**Embeddings:** `all-MiniLM-L6-v2`, 384-d, CPU, computed on the instance
**Harness:** `bench/aws/run.sh c7i.8xlarge fuse` → `bench/prepdata.py` + `bench/fuse.sh`
**Tuning:** `shared_buffers` 25 GB, `work_mem` 256 MB, `jit = off` (recorded in `tuning.log`)

## Verdict: the Phase F gate is NOT met. Two of five rows fail, and one of them fails for a reason this project already measured and did not act on.

| `FUSED_TOPK.md` §8 row | Gate | scifact | nfcorpus | fiqa | |
|---|---|---|---|---|---|
| recall vs exhaustive fused scan | 1.000 | **1.000** | **1.000** | **1.000** | **PASS** |
| p99 latency, k=10 | ≤ 0.70× RRF | **0.609×** | **0.560×** | **0.633×** | **PASS**, **STALE 2026-09-22** |
| p50 latency, k=10 | ≤ 0.50× RRF | 0.582× | 0.795× | 0.578× | **FAIL** (faster, not 2× faster), **STALE 2026-09-22** |
| nDCG@10 | ≥ RRF | 0.982× | 0.924× | 0.687× | **FAIL** — **SUPERSEDED 2026-09-22, now MET** |
| channel `score()` calls | ≤ 0.20× RRF | 0.648× | 0.903× | 0.541× | **FAIL**, unchanged |

**Two rows of this table no longer describe the shipping scorer, and one of them has
been overturned — both later the same day; see "Second measurement" below.** The **nDCG**
row was re-measured in the product with a per-key ceiling normalizer and is now **MET**
(1.053× / 1.010× / 1.114×), so §8's gate is **3 of 5 passing** and 2 of 5 failing (p50,
`score()` calls). The **p50 and p99** rows are **STALE, not retracted** (hard rule 13):
they were correctly measured and the scan mechanism they measured is unchanged, but they
were taken on the raw sum — now `pg_weave.fuse_normalize = off` — and the normalizer adds
a pre-scan pass whose cost is **unmeasured**. They need an EC2 re-run of `bench/fuse.sh`
before being quoted again.

Both arms read **one** `weave` index over `(body, emb)`; nothing differs but the
scorer. The control does not pay the storage cost of a real two-index RRF stack, so
every margin here is a **lower bound** on the margin against a genuine deployment —
which makes the two failures worse, not better.

## The two failures

### 1. The linear-sum objective ranks worse than RRF, and worst where the vector channel matters most

**SUPERSEDED 2026-09-22, later the same day, by the per-key ceiling normalizer — see
"Second measurement" below. Left in place (hard rule 13):** this is the measurement of the
raw sum, which is still reachable as `pg_weave.fuse_normalize = off` and is the arm every
figure in this section describes.

| dataset | docs | nDCG@10 fused | nDCG@10 RRF | ratio | recall@100 fused | recall@100 RRF |
|---|---|---|---|---|---|---|
| scifact | 5,183 | 0.6720 | 0.6846 | 0.982× | 0.8892 | 0.9517 |
| nfcorpus | 3,633 | 0.3161 | 0.3422 | 0.924× | 0.2908 | 0.3251 |
| fiqa | 57,600 | 0.2393 | 0.3482 | **0.687×** | 0.5141 | 0.6932 |

**This is not a correctness defect.** The fused scan computes its objective exactly —
the recall row is 1.000 on every dataset, verified against an exhaustive per-channel
oracle. The objective itself is the problem.

`fuse(body <=> q, emb <#> v, weights => '{0.5,0.5}')` is a weighted sum of **raw,
unnormalized** channel scores. BM25 is unbounded and runs to ~10–20 on these corpora;
a quantized inner product lives in ~[−1, 1]. Measured on scifact: max BM25 ≈ 10.0,
max vector score ≈ 0.305 — a **33× scale mismatch**. At equal weights the vector term
cannot reorder the BM25 ranking by more than a rounding nudge, so the fused arm is
**effectively lexical-only**, and it loses to a control that is scale-free by
construction. RRF ranks on *reciprocal rank*, so a channel's units never enter.

The evidence that this is the mechanism, rather than a guess: **the deficit tracks how
much the dense channel should contribute.** fiqa is the dataset in this set where dense
retrieval carries the most signal, and it is where the fused arm loses by 31 %. On
scifact, where BM25 alone is already strong, the arms are within 2 %.

**So "better quality" cannot be claimed, and the gate as written cannot be met by
tuning the scorer.** What it needs is per-channel score normalization or calibration
before the sum — a design gap, tracked as `doc/GAPS.md` **G44**. Until then the honest
statement is: *the fused scan computes a weighted sum faster than RRF computes a rank
fusion, and the weighted sum is the worse ranking function.*

### 2. The `score()`-call mechanism fails, and the vector block bound is why

This is the row §8 singles out: *"If the `score()` call ratio is not dramatically lower,
stop and fix the bounds before optimizing anything else — a loose bound makes the entire
design pointless."*

Split by channel, the ratio says exactly where the problem is:

| dataset | lexical calls fused/RRF | vector calls fused/RRF | combined | vector share of fused calls |
|---|---|---|---|---|
| scifact | 0.353× | **0.991×** | 0.648× | 71 % |
| nfcorpus | 0.581× | **0.985×** | 0.903× | 87 % |
| fiqa | **0.149×** | **0.956×** | 0.541× | 86 % |

**The lexical side works.** On fiqa it clears the 0.2× gate on its own (0.149×) — the
fused threshold really does suppress lexical scoring work, and `blkskip` confirms it:
3,444,538 blocks skipped on fiqa against 7,081,750 pivots.

**The vector side does essentially nothing.** 0.956–0.991× of the control, and
`vec_blocks_bound_skipped = 0` on **all three datasets** — the vector block bound
pruned **zero** blocks, so the vector channel scores every live lane in both arms. And
because the vector channel is 71–87 % of all fused `score()` calls, it drags the
combined ratio to 0.54–0.90× however well the lexical side prunes.

> **CORRECTION 2026-09-22 (evening) — `vec_blocks_bound_skipped` IS THE WRONG WITNESS,
> and this is a correction of REASONING, not a reversal of RESULT.** The counter cited in
> the paragraph above is **structurally zero in any fused scan**, so it is evidence of
> nothing at all. It increments only on `WEAVE_VSCAN_SKIP_BOUND`
> (`src/vector/vecscan.c:291`), which requires the vector shuttle's *own* threshold floor;
> that floor is set only by `weave_vec_shuttle_set_threshold()`, whose **sole caller** is
> the `weave_vec_scan()` SRF driver (`src/vector/vecshuttle.c:1336`). Nothing on the fused
> path calls it, so the field stays at its `-INFINITY` init
> (`src/vector/vecshuttle.c:887`) and `bound <= threshold` is false for every finite
> bound. A fused scan therefore reports 0 whether the bound is perfect or useless. The
> tree **already said so in two places** — `src/vector/vecshuttle.c:609-614` and the file
> header at `:44-46` — and nobody carried it into the benchmark write-ups. That is the
> reusable part: a comment that contradicts a published inference is worthless until it is
> propagated to the document making the inference. Twelfth-member territory (AGENTS.md):
> *a counter's silence is not evidence until the counter has been shown able to fire.*
>
> **The conclusion still stands, on other evidence.** The vector block bound really does
> fail to prune on this data: `bench/RESULTS_CODE_SCAN.md:43-44,55` measures **0.00–0.01 %**
> of blocks pruned *even at an oracle threshold*, which rules out scan order as the cause,
> and `B2 = max‖rec‖·‖q‖` is **≈ 1.0 by construction** on L2-normalized data
> (`RESULTS_CODE_SCAN.md:72,87`) while θ never reaches 1.0. The 0.956–0.991× ratio is
> itself a direct measurement and is untouched by this correction.
>
> **The counter that IS informative is `blkskip`** (`weave_fuse_stats()`; the test at
> `src/am/fuse.c:618`, incremented at `:644`) — and its limitation has to travel with it:
> it is a **combined** bound summed over every contributing channel (`ub += b`,
> `src/am/fuse.c:610`), so a non-zero `blkskip` proves that range skipping *happens* and
> **cannot attribute it to the vector channel**. Nothing in the tree isolates
> vector-channel block pruning inside a fused scan. `doc/GAPS.md` **G46**.

**This was already known and is the sharpest process lesson in this run.**
`bench/RESULTS_BOUND_PRUNING.md` measured the vector block bound pruning **0.0 %** of
blocks, and `doc/GAPS.md` G43 recorded the generalization — *any bound this project
computes but does not act on is in the same position*. The fused scorer is the first
consumer that depends on that bound doing work, and it does not. The mechanism row was
therefore predictable from a measurement already on disk; nobody connected the two until
the gate failed. Hard rule 9 says measure the thing the design rests on before building
on it. The measurement existed. The *inference* did not.

## Follow-up the same day: normalization overturns the nDCG result (G44)

Before writing any scorer code — hard rule 9 — the candidate objectives were scored
offline from the index's **own** exhaustive per-channel scores, through the same run-file
and `bench/ndcg.py` path the two real arms use. `FUSE_NORMSTUDY=1` in `bench/fuse.sh`.

| dataset | raw sum (shipping) | RRF (control) | **maxn** | mmn | maxn ÷ RRF |
|---|---|---|---|---|---|
| scifact | 0.6720 | 0.6846 | **0.7182** | 0.7189 | **1.049×** |
| nfcorpus | 0.3161 | 0.3422 | **0.3444** | 0.3208 | **1.006×** |
| fiqa | 0.2393 | 0.3482 | **0.3556** | 0.3348 | **1.021×** |

`maxn` = each **key** divided by its realized per-query maximum. `mmn` = per-key min-max.
**The shipping objective loses to RRF on 3 of 3 datasets; `maxn` beats it on 3 of 3.**

**Positive control:** the `raw` and `rrf` arms reproduce the EC2 numbers above to four
decimals on all three datasets. A study that cannot reproduce the thing it claims to
improve is measuring something else — and this one needed no EC2, because nDCG is
deterministic and host-independent. Only latency needs a quiet machine.

**`mmn` is rejected on semantics, not on one dataset.** It wins on scifact and loses on
the other two. Min-max shifts each channel's floor to the corpus minimum, which destroys
BM25's "an absent term contributes exactly 0" and lifts every non-matching document off
the floor. Dividing by the max keeps 0 at 0.

**So the nDCG row is fixable, and the fix is not in the scan.** What is *not* yet
established is that a **one-pass** normalizer reaches `maxn`: a threshold scan cannot know
a realized maximum before it starts. Measured over 25 scifact queries, substituting the
pre-scan ceiling leaves a median **1.37×** relative misweighting between the two keys
(range 0.88–2.09×) against the raw sum's **33×**. The vector ceiling is a constant 1.0107
— L2-normalized vectors under ip — so every bit of that distortion is the lexical
ceiling's assumption that all terms hit max tf in the shortest document at once.

**And the one-pass scheme was then measured too, with `bench/normsweep.sh`: it beats RRF
on 3 of 3.** Dividing each key by its pre-scan ceiling corresponds to an effective
lexical:vector ratio of **0.73** — *below* 1, because the lexical ceiling is the looser of
the two (2.07× vs 1.52×), so dividing by it shrinks the lexical side more. (`1/1.37`, not
`1.37`; taking that direction the wrong way makes fiqa read as a loss and would have
rejected the implementable scheme on an arithmetic slip.)

| dataset | RRF | raw sum (shipping) | **ceiling-normalized (r=0.73)** | vs RRF |
|---|---|---|---|---|
| scifact | 0.6846 | 0.6720 | **0.7133** | **1.042×** |
| nfcorpus | 0.3422 | 0.3161 | **0.3489** | **1.020×** |
| fiqa | 0.3482 | 0.2393 | **0.3763** | **1.081×** |

So the nDCG row is fixable **without a second pass, without new statistics and without new
on-disk state** — by dividing each key by a constant the scan already computes for its
MaxScore partition. On nfcorpus that ratio is the best point in the entire sweep; on fiqa it
beats even the realized-max scheme.

Two things kept honest about it. **The ceiling's helpful direction is empirical, not
derived** — it happens to push toward more vector weight, and more vector weight is what all
three of these corpora want (best points at 0.25, 0.50, 0.73, every one below equal); a
corpus wanting more lexical weight would be pushed the wrong way. And **the third dataset
overturned the second's conclusion**: scifact swings 3.0 % across the whole ratio range and
nfcorpus 6.9 %, which after two datasets supported "nDCG is flat, the weights knob is
forgiving" — while **fiqa swings 43.4 %**, falls monotonically across the range, and drops
below RRF at ratio 1.37. That is hard rule 11 in person, and the conclusion two datasets
supported was wrong. fiqa's optimum is also at or below the lowest ratio swept and still
falling, so the sweep does not contain it.

Full numbers, the per-key-not-per-channel constraint, and the implementation spec in
`doc/GAPS.md` G44.

## Second measurement, 2026-09-22 (later the same day): the normalizer measured IN THE PRODUCT — the nDCG row is MET, two rows still fail, one cost is unmeasured

**What still fails, first, because the normalizer touches none of it.** Both failing rows
are unchanged by this work:

| `FUSED_TOPK.md` §8 row | Gate | scifact | nfcorpus | fiqa | |
|---|---|---|---|---|---|
| p50 latency, k=10 | ≤ 0.50× RRF | 0.582× | 0.795× | 0.578× | **FAIL**, and now stale (below) |
| channel `score()` calls | ≤ 0.20× RRF | 0.648× | 0.903× | 0.541× | **FAIL** |

The `score()` row fails for the reason already recorded above and for no new one: the
vector block bound prunes nothing on this data, and the vector channel is **71–87 %** of
all fused `score()` calls. Normalizing the objective changes which documents win; it does
not make a bound prune.

**CORRECTION 2026-09-22 (evening):** as first written this sentence offered
`vec_blocks_bound_skipped = 0 on all three datasets` as the evidence. That counter is
structurally zero in any fused scan and says nothing — see the boxed correction under
failure 2 above for the mechanism, for the evidence that does support the claim
(`RESULTS_CODE_SCAN.md` 0.00–0.01 % pruned at an *oracle* θ, and B2 ≈ 1.0 by
construction), and for `blkskip`'s combined-bound caveat. `doc/GAPS.md` **G46**.
**Also corrected by the third measurement below:** the ratios in the table above are the
**raw** arm's. The shipping (normalized) arm is 0.571× / 0.875× / 0.513× — better, and
still a FAIL.

**And one cost is UNMEASURED, which is stated rather than glossed because it is not yet a
number.** The normalizer adds a **pre-scan pass** — one query-LUT build and one directory
fold per bolt per vector key. The directory is kilobytes against a code weft of megabytes,
so the cost is *expected* to be small, but **no latency figure has been taken since the
change**. Two consequences:

- The p50 and p99 rows in this document measure **a scorer that no longer ships**: they
  were taken on the raw sum, which is now `pg_weave.fuse_normalize = off`. They are **not
  retracted** — correctly measured, and the scan mechanism they measured is unchanged — but
  they are **stale** and need an **EC2 re-run of `bench/fuse.sh`** before being quoted
  again. Marked in place in the verdict table above and in the latency section below (hard
  rule 13).
- Nothing here says what the normalized scorer's p50 or p99 is. Do not infer it from the
  quality numbers.

### The win: the nDCG row is MET, and it is measured in the product rather than offline

`bench/normprod.sh` — three arms that are the **same statement** differing only in
`pg_weave.fuse_normalize`, plus the RRF control, all scored through `bench/ndcg.py`. Real
MiniLM BEIR corpora from the 2026-09-22 EC2 run, restored locally. **This needed no EC2 and
cost nothing**: nDCG is deterministic and host-independent; only latency needs a quiet
machine.

| dataset | raw sum (the old objective) | RRF control | ceiling-normalized | norm ÷ RRF |
|---|---|---|---|---|
| scifact (300 q) | 0.6720 | 0.6846 | **0.7212** | **1.053×** |
| nfcorpus (323 q) | 0.3161 | 0.3422 | **0.3455** | **1.010×** |
| fiqa (648 q) | 0.2393 | 0.3482 | **0.3878** | **1.114×** |

**Positive control:** the `raw` arm reproduces the recorded EC2 numbers to four decimals on
all three (0.6720 / 0.3161 / 0.2393), and the RRF arm reproduces 0.6846 / 0.3422 / 0.3482.
An arm that cannot reproduce the thing it claims to improve is measuring something else.

**It is not a top-10 reshuffle.** recall@100 improves too: scifact 0.8892 → **0.9683** and
fiqa 0.5141 → **0.7079**, against RRF's 0.9517 and 0.6932.

### The loss, in the same run: nfcorpus is NOT a clean win

| nfcorpus metric | ceiling-normalized | RRF | |
|---|---|---|---|
| nDCG@10 | **0.3455** | 0.3422 | +1.0 %, and it is the gate row |
| recall@100 | 0.3206 | **0.3251** | **LOSES** |
| MRR@10 | 0.5441 | **0.5514** | **LOSES** |

The §8 gate row is nDCG@10, so the row is met on three of three datasets. But on nfcorpus
the normalized arm is **one metric ahead and two behind**, and it must not be presented as
a clean win — a reader choosing on recall or on reciprocal rank would pick RRF there.

### Gate state after this run: 3 of 5 rows pass

| row | gate | state |
|---|---|---|
| recall vs exhaustive fused scan | 1.000 | **PASS** (unchanged) |
| p99 latency, k=10 | ≤ 0.70× RRF | **PASS** on the raw scorer — **stale** for the shipping one |
| nDCG@10 | ≥ RRF | **PASS** — 1.053× / 1.010× / 1.114× |
| p50 latency, k=10 | ≤ 0.50× RRF | **FAIL** — 0.582× / 0.795× / 0.578×, and stale |
| channel `score()` calls | ≤ 0.20× RRF | **FAIL** — 0.648× / 0.903× / 0.541× (raw arm; the shipping arm is 0.571× / 0.875× / 0.513×, measured below, still FAIL) |

### The mechanism, because the shape of the normalizer is the load-bearing part

Every channel of a `fuse()` **KEY** — not every channel — carries effective weight
`w_key / N_key`, where `N_key` is that key's **pre-scan ceiling**: for a lexical key, the
sum over its terms of the BM25 term bound at that term's max tf over **all** segments
(free, because the loop that computes global idf already reads every segment's dictionary
entry); for a vector key, the max over bolts of a new `weave_vec_weft_maxscore()` (one
directory fold per bolt). New GUC **`pg_weave.fuse_normalize`, default on**; `off` restores
the raw sum, which is the arm every previously recorded figure in this document was
measured on.

**The normalizer is deliberately QUERY-GLOBAL rather than per bolt, and that is
correctness rather than tidiness.** `weave_fuse_pass()` runs one bounded top-k per bolt and
merges the per-bolt lists **by score**, so a per-bolt normalizer would make the answer a
function of the segment layout: it would change after an INSERT, after VACUUM and after a
merge, silently. `sql/fuse_degenerate.sql` section (6) asserts bolt-count independence,
with a **positive control from a deliberately mutated build** whose normalizer used each
bolt's own ceilings — under that mutant the assertion reads `f`.

Design, the per-key-not-per-channel constraint and the spec: `doc/GAPS.md` **G44** and
`doc/specs/FUSED_TOPK.md` **sect. 8d**.

**The work counters behind that last row were re-measured the same evening, for the
shipping scorer rather than the raw one, and the row's blocker is now characterized:
see "Third measurement" below.**

## Third measurement, 2026-09-22 (evening): work counters for the SHIPPING scorer — the row still fails, and it is now CHARACTERIZED rather than merely measured

**What fails, first.** The `score()`-call row is missed on all three corpora by the
shipping normalized scorer as well as by the raw one, and the new result is *not* that the
number moved: it is that **no setting of the objective's weights and no tightening of the
vector block bound can move it**, for a reason in the storage layout rather than in the
scorer. Read the consequence section before proposing a fix.

Local, deterministic, **no EC2** — `bench/normprod.sh`, work counters plus the RRF
control, fused and RRF arms over the same single index. A count of `score()` calls does not
care whose machine it runs on; this is the same argument that made the nDCG runs
host-independent, and only latency needs a quiet machine. **Positive control:** the `raw`
rows reproduce the recorded EC2 numbers of this document exactly.

| dataset | arm | lexical | vector | total | `blkskip` |
|---|---|---|---|---|---|
| scifact | raw | 0.353× | 0.991× | 0.648× | 97,028 |
| scifact | normalized | 0.203× | **1.000×** | **0.571×** | **0** |
| nfcorpus | raw | 0.581× | 0.985× | 0.903× | 4,339 |
| nfcorpus | normalized | 0.380× | **1.000×** | **0.875×** | 88 |
| fiqa | raw | 0.149× | 0.956× | 0.541× | 3,444,538 |
| fiqa | normalized | 0.052× | **1.000×** | **0.513×** | 6,732 |

All ratios fused ÷ RRF; gate is ≤ 0.20×. The normalizer **improved** the gated row on all
three corpora (0.648 → 0.571, 0.903 → 0.875, 0.541 → 0.513) and improved the lexical side a
lot — fiqa 0.149× → **0.052×**, one nineteenth of the WAND control's BM25 contributions.
In the same run the **vector side became exactly 1.000×** and the fused core's own range
skipping **collapsed** (fiqa 3,444,538 → 6,732; scifact to zero). The gate is still missed
on all three, and the vector channel is **74–87 %** of the fused total.

### The mechanism, OBSERVED rather than inferred

A per-bolt diagnostic `NOTICE`, one scifact query, five lexical channels and one vector
channel:

| | per-lexical `w` | vector `w` | θ | partition ceiling | documents visited |
|---|---|---|---|---|---|
| normalizer **on** | 0.0110 | 0.4947 | 0.1106 | exactly **1.0** | **all 5,183** |
| normalizer **off** | 0.5 | 0.5 | 2.2824 | 23.335 | 1,450 (28 %) |

The ceiling of 1.0 is the sum of the weights, which is what per-key normalization makes it.
**A DENSE CHANNEL WHOSE WEIGHTED CEILING SITS ABOVE θ FORCES THE PIVOT TO VISIT EVERY
DOCUMENT** — the vector channel alone can bound 0.4947, so no range whose only survivor is
a vector contribution can be skipped. And θ cannot climb to meet it: a real document scores
~0.11–0.35 of the 1.0 that would require **both** channels maxed at the same document, so θ
never rises above the vector channel's 0.49. **The nDCG win and the work loss have one
cause.** Giving the dense channel enough weight to reorder the lexical ranking is exactly
what makes its ceiling unskippable.

### The obvious fix fails, and it was measured rather than argued

Over **seven** lexical:vector weight ratios from 0.0625 to 4, the **vector column reads
1.000× at every point** (0.997–0.999× at three of them) while nDCG falls away from its
optimum. Pushing the vector weight down 16× does start the fused core's range skipping
dramatically — fiqa `blkskip` **0 → 4,096,432** — and **still does not reduce the vector
channel's lane count**, because in `WEAVE_PACK_LANE` reading one lane touches every byte of
its block: **scoring 1 lane costs the same memory traffic as scoring 32**, measured in this
project for other reasons (`bench/RESULTS_CODE_SCAN.md:330,417`). A probe anywhere in a
block scores the whole block, and scattered candidates touch nearly every block. So the two
knobs a reader would reach for first — reweight the objective, tighten the bound — are both
spent.

### Consequence: a maintainer decision, PRESENTED and not taken

**§8's `score()` row is not reachable for the vector channel by tuning the objective or by
tightening the bound.** Three options, with what each costs:

| option | what it costs |
|---|---|
| **(a)** restate the row in the unit the layout actually has — **blocks or bytes** rather than lanes | changes a published gate's definition; needs the §8 row rewritten and every earlier lane-unit figure re-read |
| **(b)** a second, **vector-major** copy of the codes so a lane can be read without its block | **forfeits the storage gate** — one index smaller than the stack it replaces is a product claim |
| **(c)** **cluster-order the weft** so candidates are contiguous | contradicts the **strictly-ascending-docid** requirement the fused vector channel depends on (`include/weave/vecdocmap.h:35,105,122`) — a conflict **nothing in the tree had recorded before today** |

Option (c)'s conflict is the genuinely new fact: V13-style cluster ordering and F8's docid
adapter have been described as independent, and they are not. `doc/GAPS.md` **G46**,
`doc/specs/FUSED_TOPK.md` **sect. 8d**, and the ranking gap they follow from is **G44**.

### Recorded without acting on it: the shipping weight default may be wrong

Every corpus has an **interior optimum** in the lexical:vector ratio, and **r = 0.5** (more
vector) beats the shipping equal-weight default on **2 of 3**:

| dataset | nDCG@10 at r = 0.5 | nDCG@10 at the shipping default | |
|---|---|---|---|
| nfcorpus | **0.3535** | 0.3455 | r = 0.5 wins |
| fiqa | **0.4056** | 0.3878 | r = 0.5 wins |
| scifact | 0.7138 | **0.7212** | r = 0.5 loses by 1 % |

Changing a default on the evidence of three corpora is precisely what **hard rule 11**
exists for — a number is provisional until it reproduces at a second scale — so this is
recorded and **left to the maintainer**, not applied.

## Latency: a real win, and the A/A leg says so — **STALE 2026-09-22, re-run owed**

**STALE, not retracted (hard rule 13).** Every number in this section was measured with
`pg_weave.fuse_normalize` effectively **off** — the raw sum — because the normalizer did
not exist yet. The scan mechanism these figures measure is unchanged, and the A/A
reasoning below still stands, but the shipping scorer now runs an extra pre-scan pass (one
LUT build and one directory fold per bolt per vector key) whose cost is **unmeasured**.
Quote these figures only as the raw-sum arm, and take an EC2 re-run of `bench/fuse.sh`
before quoting a p50 or p99 for the product.

| dataset | p50 fused | p50 RRF | p99 fused | p99 RRF | p50 A/A repeat | noise floor | delta ÷ noise |
|---|---|---|---|---|---|---|---|
| scifact | 2.135 | 3.668 | 2.949 | 4.840 | 2.144 | 0.009 ms | **170×** |
| nfcorpus | 1.488 | 1.871 | 1.667 | 2.978 | 1.489 | 0.001 ms | **383×** |
| fiqa | 12.687 | 21.968 | 19.804 | 31.277 | 12.674 | 0.013 ms | **714×** |

All milliseconds; 50 queries × 7 reps, arms **alternated per query**, first rep dropped.

`fused_aa` is the **same arm measured a second time**, in the third slot of each query's
rotation — after the RRF run, so it carries whatever that did to the cache rather than
sharing a warm one. This is AGENTS.md hard rule 10, and it is the only reason the
latency numbers above are admissible: the within-arm spread is **0.001–0.013 ms**, while
the between-arm delta is **170–714× larger**. The p99 win is genuine and the p50 win is
genuine; the p50 win is simply not the 2× the gate asks for.

Both plans were asserted, not assumed — `fuse_plans` in each log shows an
`Index Scan using fd_weave` for the fused arm and one per channel for RRF. A flat
latency curve is usually the planner having ignored the index.

## Correctness: 299 of 299 comparable queries exact

| dataset | queries attempted | **compared** | mismatched vs oracle | skipped (ambiguous oracle) |
|---|---|---|---|---|
| scifact | 100 | 100 | **0** | 0 |
| nfcorpus | 100 | 99 | **0** | 1 |
| fiqa | 100 | 100 | **0** | 0 |

The oracle is exhaustive and per-channel — `weave_search()` at k = |D| plus
`weave_vec_scan()` at k = |D|, summed outside the index. It is deliberately **not** the
`fuse()` fallback, which has no corpus and scores with df = 1 (§7a (1)); the fallback
disagreed on 100/85/100 queries and that number is a diagnostic, not a failure.

This row is the reason the run happened at all: it is the same gate that caught
**G43** (a posting cursor reporting itself exhausted one block early) on first contact
with a real corpus, and it is why no EC2 time was spent while that was open.

**`compared` is the load-bearing column, and it exists because of this run.** The first
attempt reported "gate passed" on nfcorpus and fiqa having compared **0 of 100** queries:
the tie test demanded every score in the whole corpus be distinct, which is false on any
corpus of this size, so every query was skipped and the mismatch count stayed 0. Only a
tie *straddling rank 10* makes a top-10 set ambiguous, which is what is tested now, and
the gate now **dies** when it compares nothing. A gate that reports on something other
than the thing under test is this project's most recurrent failure mode; that makes three
this week (G42, the phantom GUC in G43, this).

## What was NOT measured, and why

- **The normalizer's own cost.** The shipping scorer's pre-scan pass — one query-LUT build
  and one directory fold per bolt per vector key — has **no latency figure at all**. The
  directory is kilobytes against a code weft of megabytes, so it is expected to be small;
  expected is not measured, and the p50/p99 rows above predate it. An EC2 re-run of
  `bench/fuse.sh` is owed before any latency row here is quoted for the product.

- **MS MARCO passage.** §8 names it explicitly and it is **missing**. The upstream
  source `bench/prepdata.py` fetches — `msmarco.z22.web.core.windows.net/msmarcoranking/`
  — now returns **HTTP 404** for `queries.dev.small.tsv`; the hosting moved. Tracked as
  `doc/GAPS.md` **G45**. It would not change the verdict: the nDCG gate fails on three
  BEIR datasets already, and a fourth cannot turn three losses into a win. **Updated
  2026-09-22: the nDCG row is now MET on all three, so the standing statement is narrower
  — the row is met on three BEIR corpora, not on the four §8 names.**
- **A second embedding model.** Every number here is `all-MiniLM-L6-v2`. The scale
  mismatch behind failure 1 is a property of *BM25 vs cosine-scale vectors* generally,
  but the size of the nDCG deficit is model-specific and should not be quoted as if it
  were not.
- **Concurrency.** Single client throughout. Nothing here says anything about the fused
  scan under contention.

## Reproducing

```sh
AWS_PROFILE=hotdog AWS_REGION=us-east-2 \
  FUSE_DATASETS="scifact nfcorpus fiqa" FUSE_LIMIT=200000 \
  REPS=7 LATN=50 CHECKN=100 \
  bash bench/aws/run.sh c7i.8xlarge fuse
```

Datasets are downloaded and embedded on the instance; `manifest.json` per dataset
carries the sha256 of every emitted file. Hard rule 11: **these numbers are provisional
until they reproduce at a second scale.** The three corpora here span 3,633 → 57,600
documents, which is a 16× range and is why the per-dataset trend in failure 1 can be
read at all — but it is one run on one instance type with one model.
