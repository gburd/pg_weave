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
**SUPERSEDED 2026-10-11 (independent review):** the original conclusion here said
"No disk format, scoring rule, coverage predicate, or ranked exhaustion condition
changed. No concurrent-update ordering guarantee beyond G94 is added."
That described the streaming patch, but not an unconditional correctness proof:
the inherited width-cap branch could start padding before ranked exhaustion.
The follow-up below fixes that branch. Disk format, scoring and coverage remain
unchanged; the new concurrency test checks retained-snapshot behavior, not a new
cross-snapshot ordering guarantee.


## Independent-review follow-up: cap is not exhaustion (2026-10-11)

Debian dev host **pgweave-dev-20261011-140738-2421512**, **us-east-1**, c7i.2xlarge;
PG17.11 / PG18.6. Production fix `7704808`, complete test tree `34478c3`.
No push or merge. Artifacts pulled incrementally to
`/scratch/pg_weave/g95-review-evidence/` (raw build/install/SQL/TAP logs,
library hashes and per-major regression/isolation diffs).

### Correctness change and scope

`weave_ord_grow()`, `weave_vec_grow()` and `weave_fuse_grow()` now raise SQLSTATE
**54000** if asked to widen past an unproven cap. Each error names the candidate
limit and supplies a hint. Proven exhaustion still returns false, including a
full 128-entry heap whose match-count bound is 128. Fused pending rows stay
behind the ranked frontier at the cap: otherwise a low-scoring pending row could
escape through LIMIT 129 before the grow function ever ran.

The sibling audit found no reachable edit-distance cap: the pass records only
next distances strictly above its threshold, with INT_MAX meaning exhausted.
Its redundant ceiling/progress guards are replaced by that assertion. Likewise,
vector `next <= veck` is impossible after `vecDone` and the cap check. The
`weave_search()` SRF's already-correct exhaustion guard is untouched.

**The latency numbers above stand; no remeasurement is claimed.** The lexical
change is the cap's error path, which none of the measured queries reached;
ordinary widening, exhaustion, padding and the SRF are unchanged. The fused
pending change applies only at the cap, and no measured G95 arm used fusion.

**SUPERSEDED 2026-10-11 (re-review):** the exact-cap guarantee above was
incomplete. A bound of 128 proves a 128-row answer complete, but overlapping
terms/channels can give a bound of 256 for the same 128 rows. `dbf22ed` then
raised 54000 despite exhaustion. The cap + 1 follow-up below supersedes that
claim; it does not restore cap-triggered padding.

### Reduced-cap red / green and release exclusion

The only override is the compile-time `WEAVE_TEST_ORD_WIDTH_MAX`, honored only
with `WEAVE_TEST_HOOKS`. It adds no GUC or release runtime branch. Reproduce on
the dev host with `bash bench/aws/g95_review_mutants.sh`; every build/install
status is checked directly and the installed `.so` is compared with the build.

Before production changes, hook commit `1be81df` (run from test tree `dd7268d`) was exactly `9b26d40`'s production behavior
with the guarded cap override added. Built with
`-DWEAVE_TEST_HOOKS -DWEAVE_TEST_ORD_WIDTH_MAX=128`, the ordering-plan oracle in
`bench/aws/g95_cap.sql` **failed: 172 positive rows fabricated at distance 1**.
The fixed hook build passes lexical/vector/fused refusal, pending-frontier,
small-LIMIT and proven-exhaustion controls (`base-cap.log`, `fixed-cap.log`,
`g95-review-mutants/cap-clean-sql.log`).

A release build made with **only** `-DWEAVE_TEST_ORD_WIDTH_MAX=128` ignores it:
it returns all 400 rows, with all 300 positive rows at non-padding distances.
In the same loaded backend `pg_settings` contains no `pg_weave.test_%` GUC;
the library has no test advisory-GUC string. Its SHA256 equals the subsequent
ordinary release rebuild. `t/036` repeats the loaded-session GUC absence check.
This is not a placeholder `SHOW` check.

### Mutation proofs

Each mutant **built, installed a changed hash, reached its NOTICE-marked site,
and failed its SQL/TAP oracle**; the restored release passed all 14 new TAP
assertions. `g95-review-mutants/` records each build/install/hash/site and error.

| mutant | observed failure |
|---|---|
| lexical cap returns false | 172 positive rows at distance 1; expected 54000 |
| vector cap returns false | sibling query completes rather than raises 54000 |
| fused cap returns false | pending-frontier query completes rather than raises 54000 |
| fused pending cap counted complete | low-scoring pending row escapes at LIMIT 129 |
| partial predicate bypassed | IDs `1,2,4,5,8,10,11` instead of `2,4,8,10`, both SRF and scan |

### Added test coverage and gate results

`t/036_padding_walk.pl` (also registered in flake.nix) first proves **six actual
LP_REDIRECT roots**, then requires the SRF to return those roots exactly once.
A cursor fetches one padding row; another backend commits a HOT update and an
insert, then VACUUM runs. Page inspection proves the retained heap-only version
is HOT-updated. The resumed cursor sees the old version once and excludes the
new insert. Partial and expression indexes each exercise the shared walk through
both SQL routes; the expression fixture's underlying wdoc column deliberately
has a different value, so using the wrong datum cannot pass.

The 1,280 randomized windows now require nonincreasing scores in emission order,
including no positive row after padding. An initial test version discovered a
**harness error**, not a scorer regression: joining SRF rows to roots may reorder
them. WITH ORDINALITY now captures SRF order before the join. The expected-output
refresh came from a full regression run; substituting only the edited echoed
SQL made old and new output byte-identical, including the 1,280 result. No
semantic result diff was accepted.

| gate | PG17.11 | PG18.6 |
|---|---:|---:|
| full regression | 33/33, empty diffs | 33/33, empty diffs |
| isolation | 2/2, no diffs | 2/2, no diffs |
| full TAP | 36 files / 1,501 assertions | 36 files / 1,521 assertions |
| new padding TAP | 14/14 | 14/14 |

Both build/install statuses and installcheck statuses were checked directly;
logs contain the suite's own markers, including `t/036`, not just exit 0.
Allocation, pd_lower, ASCII and rename lints all pass. The five mutation proofs
above ran on PG17. The initial expected-output mismatch and JOIN-order harness
failure are disclosed above; neither is counted as a passing gate or a caught
production mutant.

The host was killed after pulling both majors' logs, and AWS independently
reported **terminated**. Its security group and key pair were deleted. Only
this task's host was terminated; other agents' regional hosts were left alone.

## Re-review: overlapping exact-cap sets (2026-10-11)

Production **`28df190`**, test/measurement tree **`1d2b544`**. The re-review's
must-fix was reproduced before changing production: test commit `2140c2f`
adds `bench/aws/g95_cap_overlap.sql` to otherwise unchanged `dbf22ed` code.
With the existing compile-only cap of 128, both queries raise **54000**:

- Lexical `fox | dog`: 128 `fox dog` documents plus 128 empty documents.
  The true ranked set is 128; `min(df(fox)+df(dog), N)` is 256.
- Fused `fuse(d <=> 'fox', d <=> 'dog')`: 128 `fox dog` documents.
  The per-key bounds sum to 256; the 128-entry bolt heap fills.

The test also exposes the lexical error when draining the 128 ranked rows
plus 128 legitimate padding rows. Baseline build/install and installed-library
comparison succeeded; `base-g95_cap_overlap.status` is 3, and its log names
all three unexpected-54000 failures. The old `g95_cap.sql` still passed on
that baseline, demonstrating the coverage gap rather than a broken setup.

### Fix and allocation audit

At nominal cap, lexical and fused passes ask internally for **cap + 1**.
Completeness compares the returned count against this actual width, per bolt
for fusion. A full probe remains incomplete: lexical trims away its extra row,
and fusion's existing incomplete-pass truncation keeps its nominal width.
Thus the probe cannot leak through LIMIT cap + 1 or relax the pending frontier.
The grow functions still raise on an incomplete capped pass; no cap-triggered
padding was restored. Maxhits remains a valid cheap early stop, not a required
tight count. First-pass and widening widths below the cap remain unchanged.

`INT_MAX/32 + 1 = 67,108,864` fits int. Multiplication is in Size. On the tested
64-bit hosts `ScoredTid` is 16 bytes, so the probe heap is **1,073,741,824 bytes**,
one above MaxAllocSize. BMW and MaxScore candidate heaps now use the existing
huge-safe allocator, as does ordered-array growth (whose power-of-two rounding
can hit the same boundary). Fused heap, merged candidate/pending arrays and
fused seen-TID arrays already use Size and huge-safe allocation. The lexical
seen-TID array remains below MaxAllocSize at this width (6 bytes per TID).
This does not remove physical memory limits or the existing int-count ceiling
on corpus-scale accumulators; those are not new capacity claims.

### Dev gates and mutant evidence

Dev **pgweave-dev-20261011-144050-2511078, us-east-1**, Debian 13, c7i.2xlarge.
Artifacts were pulled incrementally to
`/scratch/pg_weave/g95-rereview-evidence/`.

| gate | PG17.11 | PG18.6 |
|---|---:|---:|
| full regression | 33/33, no diffs | 33/33, no diffs |
| isolation | 2/2, no diffs | 2/2, no diffs |
| full TAP | 36 files / 1,581 assertions | 36 files / 1,521 assertions |
| padding TAP | 14/14 | 14/14 |

These are the TAP totals printed by these runs, not the prior review's totals.
Build/install/library comparison and installcheck statuses
were checked, with suite markers saved. Successful pg_regress removed its diff
files: the artifact-copy commands reported absent files, and the explicit
nonempty-diff checks found none. No expected files changed. Allocation, ASCII,
rename and pd_lower lints passed.

Reduced-cap clean controls passed: overlapping lexical/fused exact cap,
exact-cap lexical plus padding, cap + 1 with and without LIMIT, and all older
single-term exact-cap, above-cap, vector and pending-frontier controls.
`bench/aws/g95_review_mutants.sh` now removes both `width++` probes in one
mutant. It **built, installed a changed hash, emitted both route site markers,
and failed both exact-cap oracles** with unexpected 54000. All five older
mutants also built, installed, reached their sites and failed: **6/6 killed**.
The macro-only release control passed again, and its library hash equals the
restored ordinary release. The restored padding TAP passed.

One harness failure is not counted as a mutant kill: the first new LIMIT
control asserted the root was an Index Scan, but it was correctly a Limit over
an Index Scan. `1d2b544` checks that child before running the query; the complete
mutation harness was rerun successfully. Both dev majors ran after restoration.
The dev host was killed; AWS independently reported **terminated**, and its
security group/key were deleted.

### Remeasurement, not a carried-forward latency claim

The huge-safe allocations also execute on ordinary LIMIT 10 paths, even though
they choose the same normal allocator at that width. Therefore the original
numbers were not assumed unchanged. Same commands as the opening reproduce
section, new run **pgweave-20261011-150523-8c8b, us-west-2**, `1d2b544`.
The refreshed reverse patch (`ffaf5a2`) restores genuine `f0b6ed7` production
source and verifies its saved SHA256 before building the baseline. This is
main-versus-current streaming, **not** a measurement isolating `dbf22ed` versus
the probe fix.

Same fixture, 200k and 1M, PG17.11 / Debian 13 / c7i.2xlarge, Xeon 8488C,
shared_buffers 6285MB, maintenance_work_mem 2GB, work_mem 256MB, JIT off.
A/A then A/B/A/B, three warmups and twelve timed samples per query/leg. All
**144 pre-timing correctness checks** passed. The host's smoke passed full
regression/isolation and 36 TAP files / 1,521 assertions, codec and regex
prefilter properties. Raw summaries: `bench/RESULTS_G95_REREVIEW.csv`.

Updated p50 milliseconds (run 1 / run 2):

| route / query / k | 200k main | 200k current | 1M main | 1M current |
|---|---:|---:|---:|---:|
| scan `!w1` / 10 | 33.561 / 33.597 | 0.853 / 0.857 | 166.203 / 166.300 | 4.178 / 4.198 |
| scan `!w1` / 1000 | 33.947 / 33.948 | 1.341 / 1.348 | 166.779 / 166.842 | 4.683 / 4.685 |
| scan `rare \| !w1` / 5000 | 36.585 / 36.585 | 4.426 / 4.417 | 174.403 / 174.844 | 11.014 / 10.979 |
| SRF `!w1` / 10 | 33.658 / 33.665 | 0.838 / 0.838 | 166.881 / 166.943 | 4.177 / 4.167 |
| SRF `!w1` / 1000 | 33.903 / 33.934 | 1.159 / 1.162 | 167.224 / 167.398 | 4.495 / 4.486 |
| SRF `rare \| !w1` / 5000 | 40.932 / 41.060 | 3.076 / 3.069 | 205.208 / 205.301 | 7.289 / 7.259 |

The ~40x `!w1` LIMIT 10 gain reproduces at both scales. Scan A/A p50 is
33.541 / 33.593 ms at 200k, 166.170 / 166.186 ms at 1M; current-arm spread
is 0.004 / 0.020 ms respectively. These are much smaller than the between-arm
delta. Scan p99 at k=10 is 33.634 / 33.734 -> 0.884 / 0.892 ms and
166.754 / 167.319 -> 4.200 / 4.254 ms; SRF p99 is
33.889 / 33.819 -> 0.849 / 0.862 and 167.603 / 167.712 -> 4.223 / 4.192.
Twelve samples still do not establish an SLO tail.

**Losses and scope:** absolute times are higher than the earlier host's on
both arms; cross-host differences do not isolate this patch. The covered
`w2 & !w1` scan is slightly slower: 2.876 / 2.889 -> 2.924 / 2.934 ms at
200k, 14.293 / 14.395 -> 14.467 / 14.430 ms at 1M. Do not label that control
unchanged or claim a cause from this comparison. `w1` at 1M is
0.312 / 0.313 -> 0.321 / 0.313 ms (mixed); `rare | !w1` LIMIT 10 is mixed
within arm variation. All controls, p99 and buffer ranges remain in the CSV.
The earlier sparse-predicate and ranked-generation limitations still apply.

Job exited 0 after `G95 BENCHMARK COMPLETE`. The harness terminated the west
host and deleted its security group/key; an independent AWS query confirmed
**terminated**. No other agent's host was touched. No push or merge.
