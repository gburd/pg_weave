# G47: which fix can reach a fixed point — the vacate phase measured against its own premise

**Date:** 2026-09-24. **Host:** workstation (local). **Why local is legitimate here:**
every number below is a **page count** or an **allocator decision count**. Both are
deterministic and host-independent, and no latency is recorded, so the noisy machine
does not matter. Hard rule 12 still applies to any *fix*: the 1M `vecmerge` run is
owed before a change to this path counts as done.

**Instruments:** `bench/../scratch/pg_weave/g47opt.sh` (per-cycle arm runner),
`g47fix.sh` (fixture), `g47matrix.sh` (the matrix), `weave_alloc_stats()` (L19),
`pg_freespace()`, `weave_check(deep)`, `weave_vec_lanes()`.

## What was in question

`doc/GAPS.md` G47 listed three fix options and called the third — *teach the pack
phase to place the weft genuinely front-packed* — "the only one that fixes it without
a policy change, and also the largest; nothing here has measured whether it is
possible with write-before-free."

It is measurable **without building it**, because the pack phase's best case is bounded
by arithmetic rather than by cleverness:

```
DEMAND = live pages the pass must relocate
BUDGET = free pages below the live data that are recyclable AT PASS START
```

A page freed by the current transaction is never recyclable within it
(`weave_free_page` stamps `ReadNextTransactionId()`, `weave_page_recyclable` asks
`GlobalVisCheckRemovableXid()`). So no placement policy can put more than `BUDGET`
pages at the front in one pass: if `DEMAND > BUDGET` the pass **must** extend by the
shortfall. That shortfall is option 3's ceiling, and it is a number we can read off
the free space map before writing any code.

## The fixture

20k × 96-d weft index, history = build 12,000 rows → 4 × (insert 2,000, `weave_merge`)
→ delete every 10th row → **one priming `VACUUM`**.

The priming vacuum is part of the fixture for two reasons. It is the L19 cycle that is
not a steady-state datum (the first vacuum after a big delete grows the index by the
livedocs tombstone blob and physically drops the tombstoned lanes, 12,000 → 10,800 —
exactly the deleted fraction), and it makes the fixture **deterministic**: 2,578 pages
on three consecutive rebuilds.

*Without* it the fixture is not deterministic, which is a finding in its own right:
the same logical history landed on 2,904 pages three times and **3,755** the fourth,
because a merge reuses a freed page only when the horizon has moved past its freeing
xid. **An index's size after a history is a function of the transaction horizon as
well as of the history.** Any harness that compares two arms after rebuilding a
fixture must pin the start state and assert it, which `g47matrix.sh` does.

Start state, every leg: **2,578 pages, DEMAND 1,346, BUDGET 1,231, shortfall 115**,
10,800 live lanes, floor 1,347 pages.

## The matrix

Six vacuum cycles per leg, xid horizon burned and asserted advancing between cycles,
fixture rebuilt and re-pinned for every leg, **two reps per arm**. `plain` is `VACUUM`
(ShareUpdateExclusiveLock — autovacuum's path and what a user gets for free); `weave`
is `weave_vacuum()`, which holds **AccessExclusiveLock**.

| caller | vacate phase | relpages, cycles 1–6 | extends/cycle | verdict |
|---|---|---|---|---|
| `VACUUM` (SUEL) | **on (today)** | 4039 2578 4039 2578 4039 2578 | 1461 / 0 | **no fixed point**, 1.568× swing |
| `VACUUM` (SUEL) | off | 2693 2578 2693 2578 2693 2578 | **115** / 0 | no fixed point, **1.045×** swing |
| `weave_vacuum()` (AEL) | **on (today)** | **1347 ×6** | 1346 then **0** | **fixed point AT THE FLOOR** |
| `weave_vacuum()` (AEL) | off | 2693 ×6 | 115 **every** cycle | fixed point, 2.00× above floor |

Both reps of all four arms are **bit-identical** (hard rule 10). `weave_check(deep)`
clean with `vector_block_stats_match_codes` **present** on every cycle of every leg,
and the live-lane digest constant — a smaller index is not a correct one.

Cross-check at the same scale on an independent fixture (the long-lived `vm`, a
different history): shortfall 116, control 2577↔4039 at 1,462 extends, ablation
2577↔2693 at 116 extends. Same behaviour to ±1 page.

## What the matrix settles

**1. Option 3 is refuted, on two independent grounds.** From the trough,
`DEMAND 1,346 > BUDGET 1,231`, so any single write-before-free pass must extend at
least 115 pages — arithmetic, not policy. And the ablated pack phase lands on exactly
`2,578 + 115 = 2,693`, which **is** that bound: the pack phase is already as
front-packed as write-before-free permits. There is no headroom for a smarter
placement policy to win, because the destination set is fixed by what was already
free, not by how the pass chooses among it.

**2. The vacate phase IS load-bearing — under AccessExclusiveLock.** With it,
`weave_vacuum()` converges to the exact floor (1,347 = 1,346 live + metapage) in **one
call**, then does literally nothing for five more cycles (0 extends, 0 reuses). Ablate
it and the AEL path stalls at 2,693 — 2.00× the floor — and keeps paying 115 extends
per cycle forever. **"Just delete the vacate phase" is refuted too.**

**3. Under a share lock the vacate phase cannot work, and there it is pure cost.**
Same fixture, plain `VACUUM`: the vacate contributes **1,346 of the 1,461 extends
(92.1 %)** and the entire user-visible file-size swing, and buys nothing — both arms
sit at the **same trough, 2,578 pages**. Removing it is 12.7× fewer extends and takes
the swing from 1.568× to 1.045×.

**4. G47's option 1 is ALREADY IMPLEMENTED, one day after being written down as an
option.** `weave_page_recyclable()` already bypasses the recycle gate when the caller
holds AccessExclusiveLock (`src/am/am.c`, "safe to bypass ONLY when no concurrent scan
can exist"), and row 3 of the matrix is the consequence: the floor, in one call. There
is nothing to build. (*An option list is a cache and it goes stale* — AGENTS.md. This
one went stale in a day, in the entry that documents the rule's last victim.)

**5. Option 2's prize shrank by 92 %.** "Skip the pass under a share lock" was worth
1,461 extends per cycle when it was written. With the vacate phase conditional it is
worth 115 — while still being the "skip forever" trap `amvacuum.c:500-509` warns
about.

## Option 4, which nobody had listed

**Make the vacate phase conditional on the lock actually held.** Run it under
AccessExclusiveLock, where it reaches the floor in one call; skip it under a share
lock, where it provably cannot help. The predicate is already in the file — the L19
probe skip at `amvacuum.c:510` uses
`CheckRelationLockedByMe(index, AccessExclusiveLock, true)` for the same reason.

It is **not** option 2: option 2 skips the whole pass, option 4 skips only phase 1 and
still runs the pack. And it changes **no policy** — plain `VACUUM` reclaims to exactly
the same 2,578 pages it reaches today.

| | today | option 4 |
|---|---|---|
| `weave_vacuum()` (AEL) | 1,347 (floor), 1 call | unchanged |
| `VACUUM` trough | 2,578 | 2,578 |
| `VACUUM` peak | 4,039 | 2,693 |
| extends per grow cycle | 1,461 | 115 |
| file-size swing a user sees | 1.568× | 1.045× |

## What option 4 does NOT fix, stated here rather than discovered later

**Plain `VACUUM` still has no fixed point** (2,578 ↔ 2,693), and it cannot have one.
Reaching the floor requires recycling pages freed by the same transaction, which under
a share lock would hand a concurrent scan a page it is still reading — the field-
reported crash `weave_page_recyclable()`'s gate exists to prevent. So G47 is really
**two defects**, and only one of them is fixable:

- **the waste** — 92 % of the churn and the whole visible swing, for no net change.
  Fixable now, by option 4.
- **the non-convergence** — the share-lock caller sits at 1.91× the floor forever.
  **Not fixable without giving up the recycle gate.** The floor remains reachable only
  under AccessExclusiveLock (`weave_vacuum()`, `REINDEX`), which is how it works today.

One more observation the matrix produced, which belongs to the guard rather than to the
fix: `novacate/weave` holds a **stable 2,693 pages while extending 115 pages every
single cycle, forever**. A stable size with permanent work is invisible to
`weave_index_is_compacted()`, whose term (1) sees 1,346 free pages below live against a
threshold of 8 and concludes there is work to do. A hole the pass cannot fill is not a
reason to run the pass — the same blindness G47's original entry recorded, wearing the
other arm's clothes.

## Provisional

Hard rule 11: one scale (20k), two fixtures, two reps. **Provisional until the 1M
`vecmerge` run**, which hard rule 12 requires anyway before a change to the vacuum path
is done.
