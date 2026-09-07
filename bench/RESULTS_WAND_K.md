# Result: the WAND initial-k frontier

Date: 2026-09-07. Harness: `bench/compete` (`wandk*` sweep in `engines/weave.sh`).
Corpus `synth-2m` (2,000,000 docs), `r6id.4xlarge`, PostgreSQL 17.6 from source,
N=100 samples after 10 warmup, all in one session, access path asserted on every
query.

## Why this was measured

PostgreSQL gives an index access method **no way to learn the query's `LIMIT`**.
A ranked block-max WAND scan therefore starts at some `k` and grows ×4 when the
executor asks for more rows. That start was hardcoded at 100.

The first competitive run showed the consequence: pg_weave's ranked latency was
**completely independent of `LIMIT`** — k100/k10 ratios of 1.003, 1.007, 0.999
across the rare, mid and common bands — because a `LIMIT 10` query did a k=100
pass. Timescale pg_textsearch, on a byte-identical corpus, scaled 1.9–4.1× with k
and was 10–21× faster at k=10. A top-k engine whose cost does not fall as k
shrinks is not pruning for the dominant query shape, which is a first page of ten
results.

The prior value was documented as a measured trade, but the measurement compared
k=64 against k=100 and never asked what k=10 would cost — because without a
k-scaling benchmark the symptom is invisible.

## The frontier (p50 ms)

| `wand_initial_k` | rare L10 | rare L100 | mid L10 | mid L100 | common L10 | common L100 |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 4.677 | 22.391 | 22.986 | 55.992 | 37.626 | 99.495 |
| 8 | 5.087 | 12.791 | 23.177 | 37.502 | 38.422 | 64.351 |
| 16 | **2.342** | 20.057 | **11.454** | 44.230 | **18.813** | 80.650 |
| **32** | 2.754 | **10.468** | 11.667 | **25.906** | 19.566 | **45.718** |
| 64 | 4.549 | 17.699 | 12.431 | 33.127 | 21.414 | 62.136 |
| 100 (old default) | 6.268 | 6.251 | 13.194 | 13.236 | 23.843 | 23.861 |
| 200 | 11.448 | 11.471 | 17.370 | 17.367 | 33.355 | 33.380 |

## What the shape says, which is not what the sweep was looking for

The frontier is **non-monotonic**, and that is the finding. k=8 beats k=4 at
LIMIT 100; k=32 beats k=16 at LIMIT 100 by 1.9×; k=16 beats k=8 at LIMIT 10 by
2.2×. A simple "lower k is better for small LIMIT" model does not fit.

The ×4 growth ladder explains it. From k=16 a `LIMIT 100` query runs passes at
16, 64, 256 — **three complete WAND passes**, because each growth recomputes from
scratch rather than extending the previous heap. From k=32 it runs 32, 128 — two.
From k=100, one. The cost of a deep page is therefore dominated by *how many
times the whole scan is redone*, not by the width of any single pass.

So the initial value is the wrong knob to be tuning, and the real fix is
**incremental growth**: keep the accumulated heap and the per-term cursor state
across a growth so a recompute extends the previous pass instead of repeating it.
That is new task **L14**, and it should make a low initial k strictly better at
every LIMIT rather than a trade.

## Default chosen: 32

Not the best value for either workload, which is the honest reason to state the
trade explicitly:

- vs the old 100, at `LIMIT 10`: **2.3× faster** on rare, 1.13× on mid, 1.22× on
  common.
- vs the old 100, at `LIMIT 100`: **1.67× slower** on rare, 1.96× on mid, 1.92× on
  common.

A first page of results is the overwhelmingly common shape and `LIMIT 100` is
still served in one growth rather than two, so 32 is where the frontier is least
bad in both directions. `pg_weave.wand_initial_k` is a `PGC_USERSET` GUC, so an
application that genuinely paginates deep can set it back to 100 per session.

## The gap that remains

Even at its best k, pg_weave loses ranked retrieval to Timescale pg_textsearch:

| band | weave best L10 | textsearch L10 | loss |
|---|---:|---:|---:|
| rare | 2.342 (k=16) | 0.308 | **7.6×** |
| mid | 11.454 (k=16) | 0.628 | **18.2×** |
| common | 18.813 (k=16) | 2.389 | **7.9×** |

Narrowed from 20.5× / 21.3× / 9.9×, but still the project's largest competitive
deficit and the reason `doc/GAPS.md` G13 stays open after this change. L14
(incremental growth) and L2 (impact-ordered postings) are the two candidate
routes; L14 is cheaper and should be measured first.

## What this does not tell us

- One corpus shape, synthetic, `english` analyzer, warm cache.
- `LIMIT 10` and `LIMIT 100` only. The interesting middle (`LIMIT 20`, `LIMIT 50`)
  is unmeasured and is exactly where the growth ladder's boundaries fall.
- pg_textsearch reports no match counts (it has no match predicate), so its
  latency is not cross-checked against the same document set the way pg_weave's,
  pg_fts's and GIN's are — those three agree exactly at 9,958 / 37,481 / 358,593.
