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
