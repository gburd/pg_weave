# G95: streaming zero-score padding

2026-10-11. **The whole-match-set padding startup is removed.** Ranked generation
is unchanged; this is not a claim of constant-time NOT queries, nor of sublinear
prefix/fuzzy/regex ranked generation.

## Reproduce

From the committed tree, after polling capacity in the same region:

```sh
export AWS_PROFILE=kiwi AWS_REGION=us-west-2 AWS_DEFAULT_REGION=us-west-2
bash /scratch/pg_weave/capacity.sh 8
SCRIPT=bench/aws/g95_job.sh SCRIPT_PULL=60 JOB_MINUTES=100 \
  bash bench/aws/run.sh c7i.2xlarge script
```

Run **pgweave-20261011-134626-a1aa, us-west-2**, commit `1e9b12e`.
Debian 13, PostgreSQL 17.11, c7i.2xlarge, EBS gp3; harness tuning
`shared_buffers=6285MB`, `maintenance_work_mem=2GB`. Same host and corpus for
all arms and both scales. Main production source is exactly `f0b6ed7`'s
`amscan.c`, restored by a committed reverse patch and checked by SHA256;
other production files are unchanged. Baseline and streaming libraries are
rebuilt separately, installed with explicit PG_CONFIG, and hashes recorded.

Corpus: G94's fixture, w1 in 10%, rare in 0.2%, w2 in 50%, eight filler terms
per document. INSERT-only for timing. Autovacuum disabled, VACUUM ANALYZE after
build. Warm sessions: three discarded samples, then twelve whole-query
EXPLAIN ANALYZE Execution Time samples. A/A first, then A/B/A/B at each scale.
`bench/RESULTS_G95.csv` preserves all 144 summary rows (p50, interpolated p99,
buffer range), including controls. Twelve samples do not characterize an SLO tail.

Before timing **each query on each arm**, the script asserts count, uniqueness,
root-TID membership against a forced sequential reference, and the number of
ranked `rare` rows. It asserts the ordering Index Scan plan. All 144 query/leg
correctness checks passed. The run's smoke also passed regression, isolation,
and 35 TAP files / 1,487 assertions. The job exited 0; its instance was verified
`terminated`, not merely shutting down.

## Measured p50 (milliseconds, run 1 / run 2)

| route / query / k | 200k main | 200k streaming | 1M main | 1M streaming |
|---|---:|---:|---:|---:|
| scan `!w1` / 10 | 29.475 / 29.526 | 0.737 / 0.746 | 146.723 / 147.048 | 3.700 / 3.681 |
| scan `!w1` / 1000 | 29.887 / 29.766 | 1.197 / 1.194 | 146.922 / 146.957 | 4.159 / 4.125 |
| scan `rare \| !w1` / 5000 | 32.281 / 32.289 | 3.978 / 3.982 | 154.181 / 154.595 | 9.919 / 9.872 |
| SRF `!w1` / 10 | 29.629 / 29.609 | 0.729 / 0.714 | 147.040 / 147.387 | 3.687 / 3.671 |
| SRF `!w1` / 1000 | 29.965 / 29.872 | 1.025 / 1.022 | 147.523 / 147.608 | 4.018 / 3.964 |
| SRF `rare \| !w1` / 5000 | 36.020 / 36.081 | 2.742 / 2.731 | 181.397 / 181.171 | 6.527 / 6.470 |

The `!w1` k=10 reduction is about **40x at both scales**, with correct answers
on both arms. Scan A/A p50 was 29.532 / 29.472 at 200k and 146.981 / 146.820
at 1M; SRF A/A was 29.592 / 29.530 and 146.976 / 147.516. The between-arm delta
is much larger than either A/A or within-arm spread. For `rare | !w1` k=5000,
scan gains 8.1x / 15.6x, SRF 13.2x / 27.9x (200k / 1M).

The p99 direction agrees: scan `!w1` k=10 main 29.632 / 29.658 becomes
0.791 / 0.789 at 200k, and 147.613 / 147.339 becomes 3.741 / 3.741 at 1M.
The CSV records every other p99, including controls.

**Controls and losses:** `rare | !w1` k=10 never pads, and the covered `w1` and
`w2 & !w1` queries are essentially unchanged. Small mixed deltas remain:
covered `w1` scan is 0.068/0.068 -> 0.069/0.068 at 200k and
0.271/0.270 -> 0.272/0.271 at 1M; do not sell these microseconds as a win.
The total `!w1` time still rises about 5x with corpus size: ranked candidate
generation still reads w1's postings to establish exhaustion. Streaming
removes padding's O(match set) collection, not ranked work.

## Correctness gates and positive controls

Dev run **pgweave-dev-20261011-130826-2003472, us-east-1**, production `c1a70ef`:
PG17.11 and PG18.6 each passed **33 regression + 2 isolation + 35 TAP files /
1,487 assertions**. Regression diffs were empty; both isolation tests passed.
All four allocation/pd_lower/ASCII/rename lints passed. This host was killed
and `dev.sh status` checked before the measurement host was launched.

Tests were committed before production changes. Main's full-run result passed
the new 1,280 randomized LIMIT/OFFSET/WITH TIES windows and SRF checks; its old
expected-output prefix was byte-identical. New output was adopted only after
that check. Coverage includes NULL/empty documents, pending inserts, deletes,
HOT updates, exact predicate rechecks, below/at/above-count LIMITs, and ties.
Existing deterministic G94 coverage checks ranked exhaustion/order, cursor
interleaving and rescans.

`bench/aws/g95_mutants.sh`: **3/3 killed**. Each BUILD and INSTALL succeeded,
the installed PG17 library hash changed, and its NOTICE proved the mutated
site ran before the SQL oracle failed:

- Exact recheck skipped: `G95 full SRF set mismatch: w4*`.
- Ranked-TID exclusion skipped: full SRF set mismatch (duplicates).
- Physical TID returned instead of root: `G95 full SRF set mismatch: wx~1`.

Clean reinstall and rerun were byte-identical to the clean control. An initial
harness attempt installed into the wrong major because sudo stripped the
PG_CONFIG environment. The unchanged-library hash guard stopped it **before
SQL**; it is not counted as a killed mutant. Both harnesses now pass PG_CONFIG
on the sudo make command line.

**A failed instrument, not a red test:** the proposed first-row buffer budget
passed on main (40/17 accesses, scan/SRF). Compressed-set materialization is
CPU work with few buffers, so this was not a valid detector. The assertion was
removed; `g95_stream.sql` remains diagnostic only. No pre-change failing cost
assertion is claimed. The latency A/A and A/B measurement above is the evidence
of the old cost and its removal.

## Boundaries

The shared iterator walks heap roots in ascending TID-derived docid order,
fetches through the snapshot, applies partial-index predicates and exact query
matching, and skips ranked TIDs. No index chain is cached across yields. The
ordering scan stops walking when the executor stops pulling; the SRF drains
only its requested k, but still materializes that k before yielding its first
SRF row (an outer SQL LIMIT cannot reduce its requested k).

Sparse predicates may walk the entire heap. That tradeoff is **unmeasured** on
large sparse prefix/fuzzy/regex corpora, not a claimed win. Those query shapes
also retain the existing whole-set gate in ranked generation, before padding.
No disk format, scoring rule, coverage predicate, or ranked exhaustion condition
changed. No concurrent-update ordering guarantee beyond G94 is added.
