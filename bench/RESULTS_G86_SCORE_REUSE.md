# G86 score reuse: EC2 measurement

**Run:** `pgweave-20261007-001351-226c`, c7i.8xlarge, Debian 13, PostgreSQL 17, commit
`78a2870` (the code is identical to the final branch; later commits change tests and docs).
Job script: `/scratch/pg_weave/g86job.sh` stage D (not in the repository).

**What is compared:** `SELECT id FROM lh WHERE d @@@ q ORDER BY d <=> q LIMIT k` with
`pg_weave.reuse_distance` off and on. `enable_seqscan`, `enable_bitmapscan` and `enable_sort`
are off, as are `jit` and parallelism. Latency is the median of 25 warm runs, after 5 discarded.
Each arm ran twice (hard rule 10) at two scales per document kind (hard rule 11).

**Evidence each arm ran what it says:** every cell records `pg_settings`' value of the GUC, the
count of `weave_current_distance` lines in its own `EXPLAIN VERBOSE` (2 on, 0 off), the row
count, and the `weave_distance` call count from `track_functions` (on: 0; off: one per row).

**Corpus:** synthetic, G87's term mix: 'a' 30 %, 'b' 20 %, 'c' 10 %, 'rare' 0.2 %.
- short: `repeat('x<n> ', 1 + g % 23)`, avg 100 bytes, inline, no TOAST.
- long: 700 pseudo-random words per document, avg 10,983 bytes, TOASTed (586 MB at 50k,
  2,344 MB at 200k).

`rare` at 50k has only 100 matching documents, so its LIMIT 400 returns 100 rows.

| kind | n | query | LIMIT | rows | wd calls off / on | ms off (run 1 / 2) | ms on (run 1 / 2) |
|---|---:|---|---:|---:|---|---|---|
| long | 50000 | `rare` | 10 | 10 | 10 / 0 | 0.226 / 0.227 | 0.021 / 0.020 |
| long | 50000 | `rare` | 400 | 100 | 100 / 0 | 2.257 / 2.256 | 0.051 / 0.051 |
| long | 50000 | `c` | 10 | 10 | 10 / 0 | 0.230 / 0.233 | 0.024 / 0.024 |
| long | 50000 | `c` | 400 | 400 | 400 / 0 | 8.934 / 8.939 | 0.131 / 0.131 |
| long | 50000 | `a \| b` | 10 | 10 | 10 / 0 | 1.041 / 1.043 | 0.832 / 0.834 |
| long | 50000 | `a \| b` | 400 | 400 | 400 / 0 | 10.465 / 10.415 | 1.737 / 1.736 |
| long | 200000 | `rare` | 10 | 10 | 10 / 0 | 0.228 / 0.228 | 0.021 / 0.021 |
| long | 200000 | `rare` | 400 | 400 | 400 / 0 | 9.338 / 9.300 | 0.234 / 0.233 |
| long | 200000 | `c` | 10 | 10 | 10 / 0 | 0.263 / 0.263 | 0.056 / 0.056 |
| long | 200000 | `c` | 400 | 400 | 400 / 0 | 8.963 / 8.979 | 0.195 / 0.196 |
| long | 200000 | `a \| b` | 10 | 10 | 10 / 0 | 3.498 / 3.497 | 3.275 / 3.275 |
| long | 200000 | `a \| b` | 400 | 400 | 400 / 0 | 15.272 / 15.274 | 6.645 / 6.635 |
| short | 200000 | `rare` | 10 | 10 | 10 / 0 | 0.077 / 0.078 | 0.077 / 0.078 |
| short | 200000 | `rare` | 400 | 400 | 400 / 0 | 0.353 / 0.356 | 0.328 / 0.328 |
| short | 200000 | `c` | 10 | 10 | 10 / 0 | 0.146 / 0.146 | 0.145 / 0.147 |
| short | 200000 | `c` | 400 | 400 | 400 / 0 | 2.450 / 2.393 | 2.387 / 2.408 |
| short | 200000 | `a \| b` | 10 | 10 | 10 / 0 | 3.347 / 3.344 | 3.342 / 3.339 |
| short | 200000 | `a \| b` | 400 | 400 | 400 / 0 | 8.231 / 8.237 | 8.194 / 8.196 |
| short | 1000000 | `rare` | 10 | 10 | 10 / 0 | 0.291 / 0.289 | 0.289 / 0.290 |
| short | 1000000 | `rare` | 400 | 400 | 400 / 0 | 1.849 / 1.810 | 1.797 / 1.817 |
| short | 1000000 | `c` | 10 | 10 | 10 / 0 | 0.318 / 0.319 | 0.314 / 0.321 |
| short | 1000000 | `c` | 400 | 400 | 400 / 0 | 2.731 / 2.733 | 2.778 / 2.683 |
| short | 1000000 | `a \| b` | 10 | 10 | 10 / 0 | 16.512 / 16.522 | 16.502 / 16.508 |
| short | 1000000 | `a \| b` | 400 | 400 | 400 / 0 | 34.638 / 34.481 | 34.574 / 34.568 |

## What it says

- **Long, TOASTed documents: a large win at both scales.** A 400-row ranked query on a single
  term drops from ~9 ms to 0.13–0.23 ms (40–68x). The operator's per-row cost is a detoast of
  ~11 KB plus a BM25, about 22 µs per row. A two-term OR at LIMIT 400 drops from 15.3 to
  6.6 ms at 200k: the scan's own cost stays and only the re-evaluation goes. At LIMIT 10 the
  single-term query drops from 0.26 to 0.056 ms.
- **Short inline documents: almost nothing, at 200k or at 1M.** One cell clears its
  within-arm spread: 200k `rare` LIMIT 400, 0.353/0.356 -> 0.328/0.328 ms (7 %), about
  0.07 µs per row. It does NOT reproduce at 1M (1.849/1.810 -> 1.797/1.817, overlapping),
  so by hard rule 11 it is provisional. Every other short cell overlaps. **This is a loss
  against the expectation, recorded as one:** with 100-byte documents the re-evaluation
  was never where the time went.
- **The OR at LIMIT 10 barely moves on long documents** (3.50 -> 3.28 ms at 200k). Ten
  re-evaluations are ~0.2 ms of a scan dominated by decoding postings, as in G87.
- Synthetic corpus: this measures the mechanism, not a real-corpus speedup. The win scales
  with document size, so a real corpus of long documents (Wikipedia articles) should see it,
  and one of short titles should not.

## The fused route: the prize, measured before building (2026-10-08, `wt/g86f`)

**Run:** `pgweave-20261008-041234-7383`, c7i.4xlarge, Debian 13, PostgreSQL 17, commit
`de5b10e`'s successor (smoke green on the same host first). Job script:
`bench/aws/g86f_prize.sh`. Same corpus as above plus an 8-d vector per row (every 997th
NULL). Median of 25 warm runs after 5 discarded, `jit` and parallelism off, two runs per arm.

Three arms per cell, all over the SAME k rows:
- **q**: `SELECT id FROM lh ORDER BY fuse(...) LIMIT k`, the query as written (the fused
  index scan; every cell's `EXPLAIN VERBOSE` shows the `<~>` transport key).
- **evalsum**: `sum(fuse(...))` over those k rows fetched by a TID scan. This is the cost of
  the hidden key alone and an upper bound on what reuse can remove. Its control, `count(id)`
  over the same TID scan (heap fetch and deform, no detoast), is 0.006-0.09 ms everywhere.
- **srf**: `weave_fuse_search(..., k)`, the same fused scan with nothing re-evaluated.

Per-row calls at LIMIT 400, from `track_functions`: `lexlex` = `fuse(d <=> 'c', d <=> 'rare')`
made 400 `fuse`, **800 `weave_distance`, 800 `weave_lexscore`** (G86's figure, reproduced);
`lexvec` = `fuse(d <=> 'c', v <-> q)` made 400 each of `weave_distance`, `weave_lexscore`,
`wvec_l2_distance`, `weave_l2score`.

| kind | n | query | LIMIT | q ms (run 1 / 2) | evalsum ms | srf ms |
|---|---:|---|---:|---|---:|---:|
| long | 50000 | lexlex | 10 | 0.468 / 0.470 | 0.421 / 0.428 | 0.048 / 0.048 |
| long | 50000 | lexlex | 400 | 18.146 / 17.788 | 17.591 / 17.557 | 0.214 / 0.216 |
| long | 50000 | lexvec | 10 | 4.899 / 4.925 | 0.215 / 0.218 | 4.747 / 4.765 |
| long | 50000 | lexvec | 400 | 20.355 / 21.402 | 9.144 / 9.201 | 10.664 / 10.427 |
| long | 200000 | lexlex | 10 | 0.542 / 0.536 | 0.433 / 0.428 | 0.117 / 0.117 |
| long | 200000 | lexlex | 400 | 18.028 / 18.076 | 17.537 / 17.526 | 0.361 / 0.349 |
| long | 200000 | lexvec | 10 | 18.711 / 18.753 | 0.214 / 0.225 | 19.012 / 18.932 |
| long | 200000 | lexvec | 400 | 51.409 / 51.457 | 9.259 / 9.221 | 39.974 / 38.871 |
| short | 200000 | lexlex | 10 | 0.235 / 0.232 | 0.009 / 0.009 | 0.365 / 0.367 |
| short | 200000 | lexlex | 400 | 1.827 / 1.830 | 0.146 / 0.148 | 1.770 / 1.770 |
| short | 200000 | lexvec | 400 | 37.023 / 36.837 | 0.149 / 0.148 | 36.595 / 37.157 |
| short | 1000000 | lexlex | 10 | 0.599 / 0.597 | 0.009 / 0.009 | 0.729 / 0.737 |
| short | 1000000 | lexlex | 400 | 2.569 / 2.571 | 0.147 / 0.149 | 2.505 / 2.543 |
| short | 1000000 | lexvec | 400 | 182.942 / 181.759 | 0.152 / 0.150 | 182.119 / 182.461 |

What it says, before anything is built:

- **Long, TOASTed documents: the hidden key IS the query.** Two lexical channels at LIMIT
  400 take 18 ms, and 17.6 ms of that is re-evaluating `fuse()`: two detoasts and two N = 1
  BM25s per row, about 22 µs per channel per row, as for the lexical route. The same scan
  without it takes 0.21 ms at 50k and 0.35 ms at 200k, a 50-85x ceiling. With a vector
  channel the lexical half is one detoast per row (9.2 ms), and the ceiling is 20.4 -> 10.5
  ms at 50k and 51.4 -> 39 ms at 200k. The vector channel's own scan dominates there.
- **Short inline documents: nothing.** At 200k and at 1M the hidden key costs 0.07 ms
  above its control, against 1.8-2.6 ms for the query. Every short cell's `q` and `srf`
  overlap or differ by less than that. Reuse is not expected to move them, and the same
  holds for the lexical route.
- A third query (`fuse(d <=> 'a | b', d <=> 'c')`) produced no numbers: the job split its
  arguments on `|`. It is a harness bug, not a result, and the cell was dropped.

## The fused route, built: reuse off vs on (2026-10-08, `wt/g86f`)

**Run:** `pgweave-20261008-074244-b985`, c7i.4xlarge, Debian 13, PostgreSQL 17, commit
`3008a6a`. Job script `bench/aws/g86f_job.sh` stage D, the prize job's corpus and queries,
`pg_weave.reuse_distance` off and on, median of 25 warm runs after 5 discarded, two runs per
arm. The smoke on that host was red only on `score_reuse` (its expected file was owed) and
`limit_hint_fuse` (the two substituted `Output:` lines, intended; see G86).

**Evidence each arm ran what it says:** each cell records the GUC from `pg_settings`, the
count of `weave_current_fused_distance` lines in its own `EXPLAIN VERBOSE` (2 on, 0 off), the
`<~>` transport (1 in every cell: a fused index scan), the md5 of the returned id list, and
the per-row function calls from `track_functions`. **Identical md5 on and off in every cell**,
so the answer did not change. Calls with reuse on: only `weave_current_fused_distance`, once
per row. With it off: `lexlex` 400 `fuse`, 800 `weave_distance`, 800 `weave_lexscore` at
LIMIT 400; `lexvec` 400 each of `fuse`, `weave_distance`, `weave_lexscore`,
`wvec_l2_distance`, `weave_l2score`.

| kind | n | query | LIMIT | ms off (run 1 / 2) | ms on (run 1 / 2) |
|---|---:|---|---:|---|---|
| long | 50000 | lexlex | 10 | 0.530 / 0.532 | **0.047 / 0.049** |
| long | 50000 | lexlex | 400 | 20.199 / 20.194 | **0.256 / 0.254** |
| long | 50000 | lexvec | 10 | 5.467 / 5.480 | **5.154 / 5.175** |
| long | 50000 | lexvec | 400 | 22.418 / 22.472 | **11.701 / 11.854** |
| long | 200000 | lexlex | 10 | 0.609 / 0.608 | **0.125 / 0.127** |
| long | 200000 | lexlex | 400 | 20.279 / 20.925 | **0.416 / 0.414** |
| long | 200000 | lexvec | 10 | 20.698 / 21.708 | 20.351 / 21.145 |
| long | 200000 | lexvec | 400 | 56.448 / 56.505 | **46.329 / 44.959** |
| short | 200000 | lexlex | 10 | 0.257 / 0.260 | 0.259 / 0.264 |
| short | 200000 | lexlex | 400 | 2.059 / 2.066 | **2.011 / 1.998** |
| short | 200000 | lexvec | 10 | 21.394 / 21.121 | 21.551 / 20.282 |
| short | 200000 | lexvec | 400 | 45.380 / 43.308 | 44.968 / 42.399 |
| short | 1000000 | lexlex | 10 | 0.677 / 0.673 | 0.672 / 0.703 |
| short | 1000000 | lexlex | 400 | 2.899 / 2.882 | **2.832 / 2.829** |
| short | 1000000 | lexvec | 10 | 98.822 / 95.922 | 98.618 / 105.264 |
| short | 1000000 | lexvec | 400 | 192.445 / 206.198 | 195.695 / 194.191 |

What it says:

- **Long, TOASTed documents: the prize was collected.** Two lexical channels at LIMIT 400
  drop from 20.2 to 0.255 ms at 50k (79x) and from 20.3-20.9 to 0.415 ms at 200k (49x). Both
  land where `weave_fuse_search()` sat in the prize run (0.21 / 0.35 ms), so nothing is left
  over. At LIMIT 10 the drop is 0.53 -> 0.048 ms and 0.61 -> 0.126 ms. With a vector channel
  only the lexical half's detoast goes: 22.4 -> 11.8 ms at 50k and 56.5 -> 45.6 ms at 200k
  at LIMIT 400. The rest is the vector channel's own scan, which this does not touch.
- **Short documents: nothing, as predicted.** Two cells clear their spread: `lexlex` LIMIT
  400 at 200k (2.06 -> 2.00 ms, 3 %) and at 1M (2.89 -> 2.83 ms, 2 %). Both are about the
  0.07 ms the prize run measured as the hidden key's whole cost, and they reproduce at both
  scales. Every `lexvec` short cell overlaps, with a within-arm spread (up to 14 ms at 1M)
  far larger than anything reuse could remove.
- **`lexvec` LIMIT 10 at 200k overlaps** (20.7/21.7 -> 20.4/21.1 ms): ten re-evaluations are
  ~0.2 ms of a vector-dominated scan.
- Synthetic corpus, as above: a mechanism measurement, not a real-corpus claim.
