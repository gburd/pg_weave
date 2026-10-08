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
| full installcheck + TAP green on the final commit | **PASS** | `pgweave-20261006-032309-074a` on `8ce1576` (the branch merged with `main` at `1621f44`): `make installcheck` exit 0, `regression.diffs` empty, 33 TAP files / 1,282 tests, "All tests successful" (`t/033`'s bound reported `# TODO`). Before the merge: `pgweave-20261006-010652-fb9f` on `1a174b2` |

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

## L22 — the crash-loop size bound (`doc/PHASES.md` L22)

Branch `wt/l22`. Harness: `bench/aws/l22_job.sh` (t/033 under ablation arms),
`bench/aws/l22_gate.sh` (the gate), `bench/aws/g75_scale.sh` (now with a never-crashed
twin), `bench/aws/l22_scale_ablate.sh` (the scale arms). EC2 c7i.4xlarge, Debian 13, PG 17.11.

### The ablation (t/033, 11 cycles, two runs per arm)

The two diagnostic builds, which are not on the branch tip: `d48c485` logs the
trigger's inputs at every cleanup step, on both indexes, counted both from the FSM and
from the pages. `24a0b57` also logs which exit `weave_vacuum_compact()` takes. Each
arm makes one exact-once substitution, and every C arm built a `.so` with its own md5.

| arm | what it removes | excess at cycles 8 / 9 / 10 (run 1; run 2) | run |
|---|---|---|---|
| base | nothing | 2,626 / 5,299 / 10,776; same | `pgweave-20261008-005536-eed3` |
| `trig_pages` | the trigger counts free pages from the pages, not the FSM | 2,626 / 5,299 / 10,776; same | same |
| `trig_reusable` | the trigger counts only pages reusable now | 2,626 / 5,299 / 10,776; same | same |
| `trig_off` | the trigger | 844 / 844 / 844; 844 / 844 / 1,433 | same |
| `fit` | a share-lock pass runs only if reusable ≥ live (from the pages) | 589 / 0 / 0; 589 / 0 / 589 | `pgweave-20261008-014427-e5f8` |
| `swap` | t/033 inserts the twin first | −1,508 / 7 / −2,666; −2,371 / 7 / −2,666 (**the twin grows**: `t_w` 22,023 → 24,649 → 27,322 → 29,995) | same |
| `xid` | one `txid_current()` before the inserts | 255 / 5,306 / 5,306; same | same |

What the census showed (base, both runs):

- **The trigger fires on both indexes** from cycle 8, for example on the twin with
  9,989 FSM-free pages against a quarter of 5,505. The stale FSM entries a crash leaves
  (1,088 and 1,343 per cycle) are marked used by the reclaim *before* the trigger reads
  the map, so at the trigger the FSM and the pages agree on both indexes. **REFUTED:
  "stale FSM entries feed the trigger".**
- The twin's pass was stopped by `weave_any_free_page_recyclable()`, the 256-page probe.
  The crashed index's pass got past it with 10,244 reusable pages against 12,033 live.
  It packed what it could, extended 1,782, and freed the old copy under its own xid, so
  nothing was truncatable. The shortfall came back on every VACUUM after that.
- Which index got past the probe was decided by **INSERT order**, not by the crash. A
  merge stamps its frees with `ReadNextTransactionId()`. The next transaction is the
  first INSERT, and it cannot reuse pages stamped with its own xid, so it extends and
  leaves the low free pages old. The second INSERT reuses them. `swap` moves the growth
  to the never-crashed twin. **REFUTED: "the crash causes it".**

### The fix and the gate (`pgweave-20261008-022758-8a5a`, commit `40c9898`)

`weave_pack_fits_reusable()` in `src/am/amvacuum.c`. On a tombstone-free index, a
share-lock pass needs the live pages, counted from the pages, to fit in the pages reusable
now. With tombstones, the probe alone still decides.

| gate | result |
|---|---|
| smoke: `make installcheck`, 34 TAP files | PASS, 1,294 tests; `t/033` ok **with no TODO**; `t/015` ok |
| `t/033` bound, hard, two runs (control) | PASS: worst excess 1,064 (bound 2,255) and 844 (bound 1,421) |
| `t/015` (no ratchet), two runs | PASS; the G47 vector-weft arm still settles at 198 |
| mutant `nofit` (the probe alone again), built, distinct `.so` | **CAUGHT** by `t/033`: excess 2,626 / 5,299 / 7,972, the pre-fix numbers to the page |
| G75's four mutants | CAUGHT, as before |
| 1M rows, 14 crash cycles, with a twin | numerically PASS (worst 3,531, bound 6,750), **but see the next section** |

### A second growth at scale, which the compaction fix does not touch

The gate's scale run (above) measured this excess of the crashed index over its twin,
per cycle:

    0 2352 851 2672 1171 0 0 0 1177 1177 2354 2354 3531 3531

From cycle 9 the excess grew by 1,177 pages every two cycles. That is one 60k-row batch
of pending pages. The post-crash VACUUMs did **no allocation work at all**, so this is
not the compaction mechanism above. **The 14-cycle bound passed while the excess was
still growing**, so the bound alone was not evidence that the size is bounded.

**The scale run did not catch `nofit` either.** On the `nofit` mutant the scale run
printed the same sequence to the page and passed (`RESULT fail=0`). At 1M rows no
share-lock compaction pass ever started, so the twin bound there cannot discriminate the
first fix. `t/033` is what catches `nofit`.

**The ablation** (`pgweave-20261008-063547-9cdb`, `bench/aws/l22_scale_ablate.sh`, five
clusters on one host and one `.so`, 12 cycles, `checkpoint_timeout` 1h in every arm).
Each `INSERT` was bracketed by the allocator counters. Excess per cycle, cycles 8–11:

| arm | what it changes | excess | `INSERT s` at cycle 9 (fsm reuse / fsm defer / extend) |
|---|---|---|---|
| base | nothing | 0 / 1,177 / 1,177 / 2,354 | 0 / 1,177 / 1,177 |
| `ckpt` | a CHECKPOINT after each cycle's VACUUMs | 0 / 1,177 / 1,177 / 2,354 | 0 / 1,177 / 1,177 |
| `burn` | one `txid_current()` before `INSERT s` | 0 / 0 / 0 / 0 | 1,177 / 0 / 0 |
| `swap` | the twin is inserted first | 0 / 0 / 0 / −1,177 (the twin grows) | 1,177 / 0 / 0 |
| `drop` | the FSM loop drops a freed, not-yet-recyclable page and continues | 0 / 1,177 / 1,177 / 1,177 | 0 / 21,424 / 1,177 |

- **REFUTED: a crash-reverted free space map causes it.** The run had no checkpoint
  between crashes, and the reclaim's stale-entry count climbed every cycle (2,370 →
  15,760). `ckpt` fixed the stale count at 2,678, and the growth was the same.
- **CONFIRMED: an xid-stamp collision.** VACUUM stamps the pages it frees with
  `ReadNextTransactionId()`. Nothing assigns an xid before the next `INSERT s`, so that
  INSERT runs under the stamped xid and can reuse none of those pages. The FSM loop in
  `weave_new_buffer_internal()` then re-queued the first refused page and stopped, so each
  of the 1,177 allocations met the same page and extended. `burn` spends that xid
  elsewhere, and the growth is gone. `swap` hands the collision to the twin.
- **`drop` is the fix.** The first collision still extends one batch, because every
  candidate is refused (21,424 dropped, all freed by the cycle-8 merge). After that the
  excess stays at 1,177, and the base climbs. A dropped page comes back on the next
  VACUUM through the reclaim's re-record arm.

The crash is not needed for either growth. Both are idle-cluster effects of the recycle
gate's xid stamp, and the crash loop only provides a schedule where the first transaction
after a VACUUM is always an INSERT.

**The second fix.** The FSM loop skips a freed, not-yet-recyclable candidate, up to 64 per
call, and moves on. Every skipped page is recorded free again before the call returns.
The first version (`16d429f`) dropped them instead, as arm `drop` did. Its smoke
(`pgweave-20261008-081449-3b76`) failed `sql/weave.sql`'s recycling bound
(`size_bounded` f): `weave_vacuum()`'s compaction gathers its low-free list from the FSM,
and the reclaim cannot put back a page whose stamp is its own xid. That is the same reason
the reclaim never removes an FSM entry. **Superseded** by the skip-and-put-back version,
which leaves the map's contents unchanged and only moves the search past the skipped
pages. A live page with a stale entry keeps the old
re-queue-and-stop rule. Dropping those is the allocator change G75 retracted, which made
`t/028` fail 3 in 10. `g75_scale.sh` now also asserts that the excess stops growing over
the last four cycles: it may rise by at most what those cycles stranded, plus 64 pages
plus 1 % of the twin. Checked against the gate run's numbers offline, that assertion fails
the base (2,354 against an allowance of 866) and passes the `drop` arm (0).

Gate for the second fix: `pgweave-20261008-081449-3b76`, `bench/aws/l22_gate2.sh`, results below.
