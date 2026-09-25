# Result: the vector weft's merge and vacuum paths, at scale (hard rule 12)

`bench/vecmerge.sh`, driven by `bench/aws/run.sh`'s `vecmerge` job.

## Why this file exists at all, which is the least comfortable part of it

AGENTS.md hard rule 12 says a release touching tombstones, merge or vacuum needs a run
at scale, because the sibling project shipped the same P0 through a green local gate
twice. An audit on 2026-09-23, made while looking for somewhere to run G27's gate,
found there was nowhere:

- no job in `bench/aws/run.sh` built a **vector-carrying weave index at scale** —
  `run_fuse` is the only vector index in the harness and its BEIR corpora are a few
  thousand documents; `run_p0merge` is lexical-only; every other vector job is a
  standalone C binary with no index in it;
- **no job called `weave_check()` at all.**

So every vector merge and vacuum change this project has shipped was gated by
`installcheck` at 300 rows. Hard rule 12 has not been *unenforced* for the vector
channel; it has been **unenforceable**. That is the finding this document leads with,
because it is older and larger than anything measured below.

## Run 1 — 2026-09-23, commit `3076580`, EC2 `c7i.8xlarge`, PG17

1M × 960-d GIST (999,990 rows after 10 zero-norm drops), built on 800,000 rows, then
four insert+merge cycles of 50,000, then a 10 % delete and three vacuum cycles with
the xid horizon advanced between them.

### What held

| assertion | result |
|---|---|
| `weave_check(deep)` after build, 4 merges, 3 vacuums | **clean, 8 of 8**, with `vector_block_stats_match_codes` present in every one |
| every merge rewrote the directory | blocks 25,000 → 26,563 → 28,125 → 29,688 → **31,250** |
| live-lane digest across every merge | **byte-identical** (`1720120588581082`), so nothing re-encoded and no lane moved onto another document |
| the weft across all three vacuum cycles | **byte-stable**: `vector_codes` 84,375, `vector_dir` 1,005, `vector_meta` 1, `vector_warp` 884, blocks 28,125, digest unchanged |

Build 591.9 s; merges 418.5 s each; vacuums 1230.8 / 545.9 / 1079.6 s. Each deep check
is ~5 minutes at 25–31k blocks, because it recomputes every directory record from the
codes stored beside it and then opens the page each record names.

### What failed: the index grew 1.49× across vacuum cycles, and it is NOT the weft

`relpages` 190,091 → 185,234 → **283,924** over cycles 1→2→3. Split against the
vector buckets, which is the measurement that answers the only question that matters
here:

| cycle | total pages | vector pages | non-vector pages |
|---|---|---|---|
| vacuum1 | 190,091 | 86,265 | 103,826 |
| vacuum2 | 185,234 | 86,265 | 98,969 |
| vacuum3 | 283,924 | 86,265 | **197,659** |

**Every vector bucket is constant to the page.** The growth is entirely in the lexical
half. So G27 is exonerated — by its own data, not by argument — and there is a separate
page-accounting problem in the non-vector path that only a scale run could show. It has
the shape of `doc/GAPS.md` **G18**, which L19 was supposed to have closed.

### But "unbounded growth" is NOT established, and the instrument was wrong

The same script at 20k × 96-d produces **2,577 → 4,039 → 2,577 → 4,039**: a period-2
**oscillation**, peak/trough 1.567×, reproducible. A `cycle1 vs cycle3` ratchet cannot
tell a trend from a swing, because its verdict depends on the parity of the cycle the
relocation pass happens to fire on. Run 1's verdict is therefore **provisional** in
exactly the way hard rule 11 means.

The vacuum legs now run **four** cycles and compare **matching parity** (3 against 1, 4
against 2), and the peak-to-trough ratio is reported either way — a transient 1.57× is a
real cost to a user even when it is bounded, and it is a *different defect* from a
ratchet. Run 2 exists to tell them apart.

### Two harness defects, both found by the failure rather than by review

Both are the same shape — **an assertion decided what else got measured**:

1. `fail()` exited where it stood, so the page ratchet took the **answer check** down
   with it.
2. Worse: `run_vecmerge` died on the clean arm's status **before the mutation
   control**, so the control never ran. The control is the only thing that makes those
   eight clean results mean anything (AGENTS.md's twelfth member: until a check has
   fired once, its silence is not evidence). **The strongest claim available from run 1
   is "the invariant reported clean eight times at 1M rows", not "the invariant
   held"** — and the difference is precisely what the control settles.

Fixed in `6e9be57`: `defer()` records and carries on with a non-zero exit at the end;
the control runs whatever the clean arm did, and both statuses are reported so "the
invariant works and something else is wrong" is distinguishable from "the invariant is
blind".

### One more disclosure

Run 1's host **failed one regression test** (`vecindex`, 1 of 19) and the harness
carried on, because `run_smoke` piped `make installcheck` into `tail` and read the
pipeline's status. That is fixed in `a254a3e` — a red `installcheck` is now fatal and
the override must be asked for by name. The red test was the G27 vacuum pin itself,
which passed locally only because the planner there chose a sequential scan; it says
nothing about page accounting, but the harness had no business deciding that.

## Run 2 — commit `6e9be57`, host GREEN, 4 vacuum cycles

Same corpus, same shape, four vacuum cycles instead of three. `installcheck` passed on
this host, so the new fatal gate works. Merges and deep checks clean again, 9 of 9.

**The page numbers came back BIT-IDENTICAL to run 1:** relpages `190091, 185234, 283924,
185234`, vector pages constant at 86,265 in every cycle. Two independent runs, the same
four integers — hard rule 11's second observation, obtained for free.

**And they refute run 1's verdict.** Cycle 4 equals cycle 2 *to the page*, so there is
no ratchet on even cycles at all. The odd cycles are cycle 1 and cycle 3, and cycle 1 is
not a steady-state datum — L19 established that the first VACUUM after a big delete
legitimately grows the index by the livedocs tombstone blob. So the parity ratchet
compared a steady-state cycle against the post-delete special case and called the
difference a trend. **That was the third wrong baseline this one assertion has had**, and
the pattern in all three is the same: a comparison asserted before the series was known.

What is actually true, and is now recorded as a measurement rather than a verdict: a
**recurring ~1.53× swing tied to the relocation pass**, visible in the wall clock as
clearly as in the pages — vacuums took 1279 / 554 / 1130 / 561 s, alternating — with the
trough exactly stable. Cycle 1 is excluded as a baseline, the first assertable pair is 5
against 3, and `VACCYC` defaults to 6 because that is the smallest run producing two
steady-state odd samples. Below 5 the script says out loud that no ratchet assertion is
possible instead of making one.

**Second wrong assertion, also mine:** `recall@10 >= 0.90`, a floor invented without
measuring what this index delivers. At 1M × 960-d it returns **0.8500**, per-query hits
`8 9 9 8 8 9 9 7 9 9`. Run 3 then measured recall at the **build** stage, before any
merge or vacuum: **also 0.8500**. So it is quantization loss, present from the build, and
the absolute floor was simply wrong. Recall is now **differential** — build versus final,
the index against itself, the only baseline here that was not invented — and it is a
strictly better test of the merge and vacuum paths this script exists for.

## Run 3 — commit `45fa7f5`, 6 vacuum cycles, and the control finally fired

Clean arm: **13 of 13 deep checks clean**, four merges, six vacuums, the alternating
wall-clock signature holding throughout (1219 / 554 / 1097 / 557 / 1119 / 550 s),
build-stage recall 0.8500.

Then the control reported that `weave_check()` did **not** catch a code-page pointer
mutated by +2 pages. The conclusion available from that alone — "the invariant is blind"
— is the wrong one, and the run's own data says why: `vector_codes` 75,000 pages over
25,000 blocks is exactly **three strip pages per block** at 960 dimensions, and all
three carry the same `blockno`. `firstpage + 2` named the same block's third strip, and
an invariant checking only page-kind plus `blockno == b` was right to pass it.

**What the scan does with that pointer is the other half.** `code_cur_block()` reads
`strips_per_block` pages from wherever it starts and follows `nextblk`, so a cursor
beginning at the third strip runs off the end of the block into block b+1, whose header
carries the wrong blockno, and the scan **refuses**. So the state was: a scan that
errors, and an offline checker that calls the index clean.

**That is worse than a blind checker.** `weave_check()` is the tool you reach for to
decide whether to trust a relation, and it was answering "healthy" about an index whose
queries fail.

### The fix, and the control that now demonstrates it

`src/am/amcheck.c`: `firstpage` must name the block's **first lane strip**, not merely
one of its strips. `WeaveVecStripHdr.j0` records the first coordinate stored on a page,
so only the first lane strip has zero; the centroid flag is excluded because a centroid
strip also starts at coordinate 0 and is not where the code cursor may begin. At three
strips per block this moves the undetectable wrong values from two per block to none.

Verified on EC2 (job `vecctl`, 200k rows, commit `3bd606b`) — **both legs fire**:

```
=== LEG 1: weave_check on the mutant ===
 vector_block_stats_match_codes | f | bolt 0 block 0: firstpage 1832 is not this
                                     block's first lane strip
=== LEG 2: a scan of the mutant ===
ERROR:  weave vector scan cannot read bolt 0 of index "vmut_weave"
DETAIL: the code chain: a vector code page does not carry the block the chain's
        block-major order calls for.
```

So G27's invariant has a demonstrated **true positive**, and the 13 clean results above
mean what they appear to mean. The control asks both questions separately now, because
checking only `weave_check()` is what made a detection gap look like a blind invariant —
different defects, different fixes. And a `vecctl` job runs the control alone: three
full runs went by without it ever firing, each discovery costing a five-hour clean arm,
and a control you cannot iterate on is a control you do not really have.

*One last time, in the gate written to catch exactly this:* the leg-2 pattern required
the explanation on the `ERROR:` line, but it is in `DETAIL:` — so the control printed
"the scan refused: no" two lines below a scan refusing. Fixed in `9f9d46f`.

## Run 4 — 2026-09-24, commit `65e7c3f`, EC2 `c7i.8xlarge`, PG17: option 4 at scale

Hard rule 12's run for the G47 fix (`bench/RESULTS_G47_VACATE.md`, `doc/GAPS.md` G47).
Same corpus, same history, same instance type as runs 2 and 3: GIST-1M × 960-d, build
800k rows, 4 × (insert 50k, `weave_merge`), delete every 10th, then **6** `VACUUM`
cycles — plus two new legs the harness gained for this run (per-cycle allocator
counters + DEMAND/BUDGET, and a `weave_vacuum()` floor leg).

### The fix holds at 1M, and cycles 1 and 2 are bit-identical to the pre-fix runs

| | runs 2 & 3 (pre-fix, bit-identical to each other) | run 4 (option 4) |
|---|---|---|
| relpages, cycles 1–6 | 190091, 185234, **283924**, 185234, … | 190091, 185234, **189283**, 185234, 189283, 185234 |
| peak / trough | **1.53×** | **1.03×** |
| grow-cycle growth | **+98,690 pages (~771 MB)** | **+4,049 pages (~32 MB)** |
| wall clock, cycles 1–6 | 1219 / 554 / 1097 / 557 / 1119 / 550 s | 708.8 / 564.6 / **566.5** / 566.3 / **569.6** / 590.0 s |

**Cycles 1 and 2 reproduce the pre-fix runs to the page** (190,091 and 185,234), which is
what licenses attributing the difference at cycle 3 to the change rather than to the
fixture: the history, the corpus and the instance type are the same, and the two cycles
where the fix does nothing are identical.

**The previously-expensive cycles collapsed onto the cheap ones.** The alternating wall
clock was the symptom that first identified the relocation pass; it is now 566.5 / 566.3 /
569.6 s — a 0.6 % spread — against 1,097–1,219 s before, so those cycles are **1.94×
faster**. Cycle 1 remains higher (708.8 s) for the reason L19 gives: the first VACUUM
after a big delete legitimately writes the livedocs tombstone blob.

### The DEMAND/BUDGET law predicted each grow cycle to the page

This is the second scale for the law that refuted G47's option 3 (hard rule 11), and it is
exact rather than approximate:

| cycle | demand | budget | shortfall predicted | extends measured |
|---|---|---|---|---|
| 2 → 3 | 94,641 | 90,592 | **4,049** | **4,049** |
| 4 → 5 | 94,641 | 90,592 | **4,049** | **4,049** |
| 1 → 2, 3 → 4, 5 → 6 | 94,641 | 95,449 / 94,641 | 0 or negative | **0** |

`lowfree_defer = 0` on every cycle, which is the signature that the vacate phase is no
longer running under the share lock — pre-fix it equalled that phase's own freed-page
count. A pass that frees no page it then has to ask for is the whole of the fix.

**One thing 1M shows that 20k could not:** at this scale the grow cycle's shortfall is
4.3 % of demand (4,049 of 94,641) rather than 8.5 %, so the residual swing is 1.03×
instead of 1.045×. The defect's *cost* shrinks with scale as a fraction; its *shape*
(period 2, never a fixed point under a share lock) is identical.

### The floor, and it is worth 708 MB that plain VACUUM cannot reach

The new AEL leg: `weave_vacuum()` took the settled index from **185,234 to 94,642 pages
in ONE call** (1,135.5 s; extend 94,641, reuse 94,641, defer 0), and a **second call moved
it not at all and performed ZERO allocations**. 94,642 is exactly the floor — 94,641 live
pages plus the metapage.

So at 1M the two callers differ by **1.96×, about 708 MB**, and the distinction G47 now
rests on is confirmed at scale: plain `VACUUM` reclaims what it can under a share lock and
oscillates 1.03× forever above a floor it cannot reach; `weave_vacuum()` reaches the floor
in one call and then does nothing. The zero-allocation assertion is what separates that
from "a stable size while still relocating", which is what the ablated arm did at 20k.

### Correctness, and the control that makes it mean something

- `weave_check(deep)` clean with `vector_block_stats_match_codes` **present** at every
  stage: build, all four merges, all six vacuum cycles, and the new `ael` stage.
- **recall@10 0.8500 at build → 0.8500 after 4 merges and 6 vacuums**, the differential
  check, unchanged. (0.8500 is quantization loss present from the build; see run 3.)
- Live-lane digest constant; 0 deferred failures; `all stages clean`.
- **The mutation control FIRED on this host** — `weave_check caught it: yes | the scan
  refused: yes` — so the clean results above are informative rather than merely silent.
- 282 TAP tests green on real PostgreSQL (19 files, including `t/001` and `t/002`, which
  the nix sandbox cannot run), and the EC2 host reproduced `t/015`'s new arms exactly:
  weft series 297 → 280 → 283 → 280 → 283 → 280, and `weave_vacuum()` 142 → 142 → 142
  with zero allocations.

### What this run does NOT establish

- **It is one run of the fixed arm.** Its baseline is two bit-identical pre-fix runs, and
  cycles 1 and 2 reproduce them to the page, which is the strongest within-arm evidence
  available without paying for a fourth five-hour run — but hard rule 10 asks for the arm
  against itself and this is not that. The page counts have been bit-identical across
  every independent run of this fixture so far, at both scales.
- **The non-convergence under a share lock is unfixed and probably unfixable**, as G47
  says. 1.03× is smaller than 1.53×; it is not a fixed point.
- **Nothing here is a latency claim** beyond the vacuum wall clock, which is a cost
  measurement on a dedicated host, not a query benchmark.

## Run 5 — 2026-09-25, commit `bc1723d`, EC2 `c7i.8xlarge`, PG17: term (4) at scale

Hard rule 12's run for `weave_index_is_compacted()`'s predictive fourth term
(`doc/GAPS.md` G47). Same corpus, history and instance type as runs 2–4.

### The settled cycles stopped doing anything, which is the whole point

| | runs 2 & 3 (pre-option-4) | run 4 (option 4) | **run 5 (+ term 4)** |
|---|---|---|---|
| relpages, cycles 1–6 | 190091, 185234, **283924**, 185234, … | 190091, 185234, 189283, 185234, 189283, 185234 | **190091, 185234, 185234, 185234, 185234, 185234** |
| peak / trough | 1.53× | 1.03× | **1.03×, then flat** |
| settled-cycle allocations | ~98,690 extends | 94,641 reuse + 4,049 extends | **0** |
| settled-cycle seconds | 1,097–1,219 | 566 | **0.1** |

Cycles 1 and 2 are **identical to run 4** (190,091 and 185,234, at 666 s and 540 s): the
passes that have work to do still do it, including the post-delete pass whose rewrite drops
tombstones. From cycle 3 the predicate declines: `extend=0 reuse=0 defer=0`, **0.1 s**, six
times over. That is ~2,264 s of pointless `VACUUM` — 38 minutes per four cycles, forever on
a quiet database — removed, along with ~740 MB of relocation and its GenericXLog per cycle.

### The regression the local matrix caught did NOT recur at scale

The first version of term (4) made the *floor* unreachable, because its prediction models a
pack-only pass while `weave_vacuum()` performs vacate+pack under `AccessExclusiveLock`. The
shipped version is restricted to share-lock callers, and this run confirms it at 1M:

- `weave_vacuum()`: **185,234 → 94,642 pages — exactly the floor — in one call** (1,065.8 s),
  and a second call moved it not at all with **zero allocations**. Unchanged from run 4.
- So the two-tier model is intact: plain `VACUUM` settles at 185,234 for free; the AEL path
  reaches 94,642 when asked. **1.96×, ~708 MB.**

### Correctness

`weave_check(deep)` clean with `vector_block_stats_match_codes` present at every stage
(build, 4 merges, 6 vacuum cycles, `ael`); **recall@10 0.8500 → 0.8500** on the differential
check; live-lane digest constant; 0 deferred failures; and **the mutation control fired**
(`weave_check caught it: yes | the scan refused: yes`), so the clean results are informative.

### What it does not establish

One run of the changed arm, against run 4 as its baseline — with cycles 1 and 2 reproducing
run 4 to the page, which is the available within-arm evidence (hard rule 10). And the
**floor is still AEL-only**: term (4) stops the waste, it does not make a plain `VACUUM`
reach 94,642, and `doc/PRODUCTION_READINESS.md` records that as a limitation.

## Status

| question | answer |
|---|---|
| G27's `firstpage` correct across merges at 1M? | **yes** — 13 of 13 deep checks clean, live-lane digest byte-identical across every merge |
| across VACUUM rewrites at 1M? | **yes** — same checks, and the weft is byte-stable across all six cycles |
| is the invariant able to fail? | **yes, demonstrated** on EC2 after being tightened to require the block's first lane strip |
| is the non-vector page swing a defect? | **open.** Reproducible to the page across two runs, bounded and period-2 in every series measured, tied to the relocation pass. Not G27 — every vector bucket is constant. Needs its own investigation |
| recall at 1M × 960-d 4-bit | **0.8500**, present from the build; not a merge or vacuum effect |

## Reproduce

```sh
AWS_PROFILE=hotdog AWS_REGION=us-east-2 \
  NROWS=1000000 BATCH=50000 NBATCH=4 DELFRAC=10 VACCYC=4 \
  bash bench/aws/run.sh c7i.8xlarge vecmerge
```

Artefacts: `bench/aws/out/<run>/vecmerge.tsv` (one row per stage per metric),
`check.<stage>.tsv` (the full `weave_check` output per stage), `mutant_check.log` (the
control).
