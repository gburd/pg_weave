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

## Confirmed at 1M — 2026-09-24, run `pgweave-20260924-200357`

Hard rule 12's run is done (`bench/RESULTS_VECMERGE_SCALE.md` run 4), and it reproduces
every claim above at 50× the scale:

| | 20k × 96-d | 1M × 960-d |
|---|---|---|
| `VACUUM` peak / trough, before | 4039 / 2578 = 1.568× | 283924 / 185234 = 1.53× |
| `VACUUM` peak / trough, after | 2693 / 2578 = **1.045×** | 189283 / 185234 = **1.03×** |
| grow-cycle extends, after | 115 | 4,049 |
| shortfall as a fraction of demand | 8.5 % | 4.3 % |
| `weave_vacuum()` (AEL) | 1,347 = floor, one call, then 0 allocations | **94,642 = floor, one call, then 0 allocations** |
| plain `VACUUM` above the floor | 1.91× | 1.96× |

Two things are stronger than a reproduction. **The DEMAND/BUDGET law predicted each grow
cycle's extends to the page at 1M** — cycle 2 reported a shortfall of 4,049 and cycle 3
extended exactly 4,049; same for 4 → 5 — which is the law that refuted option 3, now
holding at a second scale (hard rule 11). And **cycles 1 and 2 of the fixed run are
bit-identical to the two pre-fix runs** (190,091 and 185,234), so the change is
attributable rather than merely correlated: the cycles where the fix does nothing are
unchanged.

The wall clock confirms the cost was real and not bookkeeping: the previously-expensive
cycles went from 1,097–1,219 s to 566 s (**1.94×**) and the alternation that first
identified the relocation pass is gone (0.6 % spread across cycles 3–5).

`lowfree_defer = 0` on every cycle at both scales — the signature that the vacate phase is
no longer running under the share lock.

**Still not a fixed point under a share lock**, at either scale. That half of G47 is open
and probably unfixable; see the entry.


---

## Term (4): the waste half closed, 20k and 1M — 2026-09-25

Option 4 made the vacate phase AEL-only, which removed 92 % of the churn and the whole
file-size swing. It left the pass still **rewriting the entire live segment on every
`VACUUM`** — 94,641 relocations and ~566 s per cycle at 1M, forever. Term (4) of
`weave_index_is_compacted()` predicts what a low-bias pack would leave the file at and
declines a pass that cannot shrink it.

| | 20k × 96-d | 1M × 960-d |
|---|---|---|
| series, before | 2578 ↔ 2693 | 190091 185234 189283 185234 189283 185234 |
| series, after | **2578 ×6** | **190091 185234 185234 185234 185234 185234** |
| settled-cycle allocations | 115 extends → **0** | 94,641 reuse + 4,049 extends → **0** |
| settled-cycle seconds | — | 566 → **0.1** |
| the reclaim option 2 would have skipped | 2,904 → 2,578 still happens | cycles 1–2 unchanged |
| `weave_vacuum()` floor | 1,347, one call | **94,642, one call** |

Three things this rests on, each measured rather than argued:

1. **The predictor was validated before it became a guard.** `g47pred.sh` predicts, runs the
   real `VACUUM`, compares: 6 of 6 states exact on both branches.
2. **Its precondition is a tombstone fraction of zero**, because the one state it
   mispredicts — by 409 pages, in the direction that skips a useful pass — is the one where
   the rewrite drops tombstones and the live count changes mid-pass. The condition is
   checkable only *inside* the pass: a probe at the decision point read `tombfrac=0.100000`
   there and `0.000000` in every settled state. Exposing `ndeleted` to SQL (0.22.0) does
   **not** serve this and it is recorded as a loss in `doc/GAPS.md` G47 — `ambulkdelete`
   sets it and the same `VACUUM` clears it, so SQL always samples zero.
3. **It is restricted to share-lock callers, and the matrix is why.** Unrestricted, it told
   `weave_vacuum()` the index was compacted and the floor became unreachable — 2,578 instead
   of 1,347, a 1.91× regression in the one path that worked, while six cycles of the arm
   under test looked perfect.
