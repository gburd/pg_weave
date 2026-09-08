# Result: L14 on a realistic corpus — G13 narrowed, and partly inverted

Date: 2026-09-08. Harness `bench/compete`, corpus **`synth-2m-long`** (2,000,000
docs, **120.1 words/doc**), `r6id.4xlarge`, PostgreSQL 17.6 from source, N=200
after 10 warmup, all in one session. Corpus identity gate PASS across all three
engines (`a6461d245bf2cd8dc37ede76f707c33e`).

This is the first competitive run on a corpus that can measure per-posting work.
`bench/RESULTS_PORT_1_5_10.md` established why: at 11.6 words/doc nearly every term
frequency is 1, the tf column packs at one bit, and decode optimizations are
invisible. At 120 words/doc they are not.

## Ranked latency

| query | pg_weave | pg_textsearch | verdict |
|---|---:|---:|---|
| rare k=10 | 2.938 | **1.251** | behind 2.35× |
| rare k=100 | **3.567** | 8.766 | **ahead 2.46×** |
| mid k=10 | 10.561 | **1.628** | behind 6.49× |
| mid k=100 | 11.167 | **9.174** | behind 1.22× (near tie) |
| common k=10 | 15.415 | **3.136** | behind 4.92× |
| common k=100 | 16.052 | **14.851** | behind 1.08× (tie) |

**G13 narrowed from 7.6–18.5× to 2.35–6.49× at k=10, and inverted at k=100.**

## Why: L14 changed the shape of the curve

Ratio of k=100 latency to k=10 latency — how much a deeper page costs:

| engine | rare | mid | common |
|---|---:|---:|---:|
| **pg_weave** | **1.21** | **1.06** | **1.04** |
| pg_textsearch | 7.01 | 5.64 | 4.74 |

pg_weave now serves a 100-row page for almost exactly the cost of a 10-row page,
because L14 stopped discarding three quarters of every over-fetched pass: one pass
at `wand_initial_k = 32` over-fetches to width 128 and the scan now MVCC-probes
that whole array incrementally instead of stopping at 32. pg_textsearch degrades
4.7–7× with k.

Note this is the *opposite* of the pre-L14 diagnosis. Before, pg_weave's latency
was flat in k because a `LIMIT 10` query paid for a k=100 pass — flat for a bad
reason. It is flat now because a `LIMIT 100` query is served by one pass — flat for
a good one. Same number, inverted meaning, which is why the `wand_initial_k` sweep
below matters more than the ratio alone.

## The sweep now confirms the default, for a different reason than it was chosen

| `wand_initial_k` | rare L10 | rare L100 | mid L10 | mid L100 | common L10 | common L100 |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 2.679 | 7.137 | 10.073 | 22.151 | 12.021 | 35.596 |
| 8 | 2.677 | 7.089 | 10.056 | 22.132 | 12.038 | 35.542 |
| 16 | 2.686 | 7.095 | 10.035 | 22.127 | 12.043 | 35.542 |
| **32** | 2.938 | **3.549** | 10.521 | **11.149** | 15.398 | **16.142** |
| 64 | 3.838 | 4.466 | 11.491 | 12.132 | 22.861 | 23.634 |
| 100 | 5.622 | 6.236 | 12.886 | 13.531 | 29.391 | 30.216 |
| 200 | 12.423 | 13.047 | 18.560 | 19.169 | 53.966 | 54.814 |

k ≤ 16 are now indistinguishable from each other, because the over-fetch floor is
`Max(4k, 64) = 64` for all of them — the knob has no effect below k=16. At k=32 the
width becomes 128, which covers a 100-row page **in one pass**, and L100 drops
2.0× while L10 rises only 9%. Above 32 it is pure waste, monotonically.

So k=32 is the smallest value whose over-fetch covers a first page of 100, and the
frontier is now monotonic on both sides of it rather than the non-monotonic mess
the pre-L14 sweep produced. The default was set to 32 before L14 as a least-bad
compromise; it is now correct for a stateable reason. (The theoretical optimum is
25, since 4×25 = 100; not worth tuning to.)

## Index size — pg_weave wins

| engine | index | vs pg_weave |
|---|---:|---|
| **pg_weave** | **625 MB** | — |
| pg_textsearch | 873 MB | 1.40× larger |
| tsvector + GIN | 1120 MB | 1.79× larger |

## Build time — now the worst gap

| engine | build |
|---|---:|
| pg_textsearch | **45.1 s** |
| tsvector + GIN | 235.8 s |
| **pg_weave** | **495.9 s** |

**11.0× behind pg_textsearch, 2.1× behind GIN.** G5 is worse on the long corpus
than the 2.5× measured on the short one, and it is now the largest single deficit.
Task L12 (bias the end-of-build merge's allocation low so the vacate pass is
unnecessary) is the identified fix and has not been started.

## Other axes

| query | pg_weave | GIN |
|---|---:|---:|
| `count(*)` OR 2-term | **1.407** | 54.799 (**39× win**) |
| `count(*)` AND | **0.828** | 1.047 |
| `count(*)` NOT | 69.685 | — (GIN excluded, see below) |
| bare `ORDER BY` rare | **2.945** | 2392.061 (**812× win**) |
| bare `ORDER BY` mid | **10.520** | 2391.501 (**227× win**) |
| bare `ORDER BY` common | **15.404** | 2454.505 (**159× win**) |

The bare-`ORDER BY` margins are enormous here because GIN cannot use its index for
that shape at all and seq-scans 2.6 GB of text. pg_textsearch has no row for it
because its ranked form *is* the bare form — those numbers are in the table above.

## Five GIN measurements were excluded, correctly

The access-path gate failed for `gin / ranked_common_k10`, `ranked_common_k100`,
`count_common`, `count_not` and `count_prefix`. The common band's term appears
4,102,555 times across 2,000,000 documents, so it is in essentially every document
and PostgreSQL correctly chooses a sequential scan over a GIN index scan. The gate
excluded those five rather than reporting a seq-scan number as an index number,
which is exactly its job — and is what pg_fts's published "24 s ranked latency" was
missing.

## What this does not tell us

- **Synthetic text at 120 words/doc, not Wikipedia at 485.** `wiki-2m` now exists
  in the harness (`bench/compete/corpus/build.sh`) but has not been run; the two
  known blockers were fixed in this cycle, so it is the next measurement.
- **k=10 and k=100 only.** The interesting middle is where the over-fetch floor and
  the widening boundary interact, and it is unmeasured.
- **Single-stream latency, warm.** No concurrency, no cold cache, no
  larger-than-RAM configuration.
- **pg_textsearch reports no match counts** (it has no match predicate), so unlike
  pg_weave and GIN its latency is not cross-checked against the same document set.
