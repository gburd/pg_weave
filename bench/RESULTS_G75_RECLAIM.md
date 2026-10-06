# RESULTS_G75_RECLAIM — VACUUM reclaims pages a crash stranded

Branch `wt/g75`. Design and safety argument: `doc/specs/SEGMENT_FORMAT.md` §10, "Pages a
crash strands between write and link". Defect and history: `doc/GAPS.md` G75 (and G72's
leak class). Every run was on EC2 Debian 13 with PG 17.11, via `bench/aws/run.sh`. Run ids
are under `bench/aws/out/`.

## Gate status

| gate (brief) | status | evidence |
|---|---|---|
| `t/031` asserts: after recovery to each WAL record of a flush and one VACUUM, 0 leaked pages and a clean `weave_check(deep)` | **PASS** | 14 of 14 points, `reclaimed N of N` at every one. `pgweave-20261005-180209-054c` onward |
| positive control: `t/031` fails with the reclaim disabled | **PASS (caught)** | mutant `noreclaim`: 39 `not ok` (`freed 0 of 1` …). `pgweave-20261005-195152-d889`, `pgweave-20261006-001850-1420` |
| `t/029` deep check still passes | **PASS** | every smoke since `0878356` |
| crash loop: relation size bounded | **PARTIAL: see "Losses"** | `t/033`: 0 leaked pages and a clean deep check after **every** cycle (hard). Excess over a never-crashed twin: **TODO** at 18 cycles |
| concurrency: a reclaim beside a mid-segment writer never frees a page that a later publish links | **PASS** | `t/032` phases A and B, each with evidence that its window was hit (below) |
| mutant: reclaim ignores the in-progress-writer guard | **PASS (caught)** | `noguard`, `nobarrier`, `nofence` each caught by **corruption** assertions, not only by evidence assertions (below) |
| hard rule 12, scale: 1M rows, repeated crash-during-flush, VACUUM, deep clean, answers equal the heap | **PASS** | `g75_scale.sh`, two runs (below) |
| full installcheck + TAP green on the final commit | **PASS** | `pgweave-20261006-010652-fb9f` on `1a174b2`: `make installcheck` exit 0, `regression.diffs` empty, 33 TAP files / 1,258 tests, "All tests successful" (`t/033`'s bound reported `# TODO`) |

## `t/032`: the window was hit, on the clean tree

- **Phase A (the barrier).** An oversized INSERT holds the segment-write lock and has
  1,141–1,960 unpublished pages on disk. The VACUUM is then seen *waiting* for that lock,
  both through `pg_locks` and through `log_lock_waits` (`still waiting for ExclusiveLock
  on page 4294967295`). Deep check and posting probe are clean right after the publish.
  Every control run hit this on try 1.
- **Phase B (the fence).** A writer that starts after the barrier writes into free pages
  below the length the reclaim's scan read. The reclaim reports them as `newer than the
  fence` (3,951 / 3,969 pages) and leaves them alone. The result is deep-clean and every
  row's posting chain is complete.
- **Phase 0.** A healthy index carrying every weft (lexical, positions, trigrams, surf,
  vector, cgram, docvalues, doclist, pending, tombstones) reclaims **0** pages over two
  VACUUMs and a `weave_vacuum()`.

## Mutants (each confirmed to build: distinct `.so` md5, applied by an exact-once substitution)

| mutant | what it removes | caught by | how |
|---|---|---|---|
| `noreclaim` | the free call | `t/031` | `freed 0 of N`, leaked > 0, deep check fails at every point |
| `noguard` | barrier **and** fence | `t/032` | phase A: deep check fails right after the publish; phase B freed 2,480 live pages, the next VACUUM raises `corrupt document list` |
| `nobarrier` | barrier | `t/032` | phase A try 1 freed 2,367 pages of the in-flight bolt. Deep check right after the publish: `3751 unreachable page(s)` |
| `nofence` | LSN fence | `t/032` | phase B freed 4,236 pages a post-barrier writer then published: `block … is on a live dictionary chain but is flagged freed` |

**The first version of `t/032` let `nobarrier` through on everything but its evidence
assertion**, for two reasons, and both were fixed:

1. The VACUUM started while the writer held the lock but had not yet written a page, so
   the fence alone protected every page.
2. The deep check ran only at the end of the phase. By then a later VACUUM's merge had
   *laundered* the freed pages: it read them, stopped at their reset `nextblk`, and wrote
   a self-consistent bolt. That is recorded as an owed fix in G75.

## Scale (hard rule 12), `bench/aws/g75_scale.sh`

1M rows; 8 cycles of a 60k-row pending batch, a VACUUM, and an immediate stop once the
flush has unpublished pages on disk; then restart, VACUUM and check.

| run | commit | crashes that stranded pages | total stranded | leaked after each VACUUM | deep check | index == heap (5 probes) |
|---|---|---|---|---|---|---|
| `pgweave-20261005-191556-34df` | `0878356` | 7 of 8 | 13,175 | 0, every cycle | clean, every cycle | yes, every cycle |
| `pgweave-20261005-235208-9265` | `344c5d7` | 8 of 8 | 10,998 | 0, every cycle | clean, every cycle | yes, every cycle |

**Cost of the pass.** The pass reads every page, as GIN's and GiST's cleanups do. On the
1M-row index (36,313 pages) a quiet VACUUM's pass took **16.1 ms and 18.8 ms** with a
warm cache. In crash cycles it took 18–37 ms over 40k–59k pages. A cold-cache figure is
unmeasured.

## Losses, retractions and what is open

- **OPEN: the crash-loop size bound** (`t/033`, `TODO`). At 18 cycles, the crashed
  index's excess over its twin reaches about 22,500 pages, reproduced on two runs
  (`pgweave-20261006-001850-1420`, `-010652-fb9f`). This is not stranded pages: every
  cycle is leak-free and deep-clean. From the first large merge onwards, every post-crash
  VACUUM of the crashed index runs the share-lock compaction (`lowfree_reuse` 10k–21k,
  `extend` 2k–8k), and the twin's never does. **The A/B against the base**
  (`pgweave-20261006-010652-fb9f`, two runs per arm, reproducing to within 2 pages) shows the base accumulating
  ~589 pages per crash from cycle 1 (excess 256 → 4,379 by cycle 7). The branch stays
  at 148–1,016 over the same cycles. From the first large merge (cycle 8), both grow by
  about a flush per cycle, reaching 24,483 on the base and 22,478 on the branch. The late
  growth is therefore **pre-existing on main**, and it belongs to the compaction trigger
  (L19); the reclaim removes the accumulating part. The full table is in G75. The scale run's final index was 82,772 pages against
  57,697 for a fresh build of the same rows, but it has no twin, so that ratio is not
  attributable.
- **RETRACTED (same branch): an allocator change.** Dropping live free-list candidates
  (nbtree's rule) made `t/028`'s truncation control fail 3 of 10 against 0 of 10 on the
  base. After the revert: branch 0 of 10 and 0 of 6, base 0 of 10. It is reverted.
- **RETRACTED (same branch): a "pre-existing growth defect"** that `t/033` appeared to
  show. It was the test giving the crashed index two VACUUMs per cycle and its twin one.
  A fix aimed at it (taking not-yet-recyclable pages out of the FSM) broke
  `weave_vacuum()`'s compaction, so `sql/weave.sql` and `sql/chanstats.sql` went red. It
  was reverted.
- **Two orders of the pass were measured wrong first:** recording freed pages in the FSM
  at once, and running the pass after the flush. Details in G75.
- **Owed: a merge launders a freed live page** (G75). The merge's chain walkers should
  raise an ERROR, not stop.
