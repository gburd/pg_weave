# Result: porting pg_fts 1.5.10 — a null result, and why

Date: 2026-09-07. Harness: `bench/compete`, corpus `synth-2m`, `r6id.4xlarge`,
PostgreSQL 17.6, N=100 after 10 warmup, all gates PASS.

## What was ported

Two upstream perf commits, both aimed at G13 (ranked latency, the largest gap):

- `6448a70` — `weave_for_get()` was still extracting bit by bit; it now does a
  word load, shift and mask. Upstream measured **1.56×** on common-term ranked.
- `6ed0d19` — ascending-resume hint in the doclen lookup. Upstream: **1.32×**.

## What it delivered here

| band | before | after | gain | pg_textsearch | remaining gap |
|---|---:|---:|---:|---:|---:|
| rare | 2.335 | 2.738 | **0.85×** | 0.307 | 8.9× |
| mid | 11.463 | 11.607 | **0.99×** | 0.628 | 18.5× |
| common | 19.018 | 17.996 | **1.06×** | 2.401 | 7.5× |

Essentially nothing, against an upstream-measured 1.56×.

## Why: the instrument, not the port

`weave_for_get()` cost `width` branches per call, where `width` is derived from the
largest value in the packed column. The corpus decides how large that is.

| corpus | avg words/doc | tf dynamic range | FOR width for tf |
|---|---:|---|---|
| `synth-2m` (this run) | **11.6** | almost every tf is 1 | **~1 bit** |
| Wikipedia (upstream) | **485** | tf up to ~20 | ~5 bits |

At 11.6 words per document the loop the optimization removed had **one iteration**.
There was nothing to save. On documents 42× longer the same change removes 4–8
branches on the per-posting hot path, which is where upstream's 1.56× comes from.

So the corpus **systematically understates every per-posting decode optimization**
— which is the entire class of work aimed at the project's largest competitive
gap. An instrument that cannot see the change it is measuring is worse than no
instrument, because it produces a confident null result. This one nearly caused a
correct optimization to be judged worthless.

The `rare` band getting *slower* (0.85×) is the same story with noise on top: at
2.3 ms with a 1-bit column there is no signal, so the measurement is reporting
run-to-run variance and the disjoint-CI test in the analyzer is comparing across
runs, which it is not designed to do.

## Fixed

`bench/compete/corpus/build.sh` now has document length as a first-class corpus
dimension, reports the achieved words-per-document, and encodes the reasoning
where the shape is chosen:

| corpus | words/doc | for |
|---|---:|---|
| `synth-2m` | ~12 | selectivity, boolean behaviour, count pushdown |
| `synth-2m-long` | ~120 | tf dynamic range; per-posting decode work is visible |

`wiki-2m` (avgdl 485) is still owed and is what makes these numbers directly
comparable to pg_fts's published 5-way. Until it exists, **no per-posting decode
optimization should be evaluated on `synth-2m`**.

## Standing G13 gap, unchanged

7.5×–18.5× behind Timescale pg_textsearch on ranked `LIMIT 10`. The two candidate
routes remain L14 (incremental WAND growth, cheaper) and L2 (impact-ordered
postings). Neither is affected by this null result, but both must be measured on
`synth-2m-long` or `wiki-2m` rather than `synth-2m`.

## What this does not tell us

The port may well be worth its 1.56× on a realistic corpus — upstream measured it
on one. This result says only that **this corpus cannot see it**, not that the
change is worthless. It stays in, with 3,404,085 oracle checks behind its
correctness (`test/hegel/test_for_get.c`).
