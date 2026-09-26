# The scalar-facet prize: sizing the docvals channel before building it

Hard rule 9 (measure before building) applied to the scalar/facet channel
(`WEAVE_CH_DOCVALS`, `include/weave/channel.h:128`; page kind `WEAVE_PK_DOCVALS` id 25,
`include/weave/pagekind.h:178`, reserved-not-built). A **throwaway spike** (2026-09-26),
not a shipped harness: `/scratch/pg_weave/dvprize.sh`, local cluster, work counters only
(deterministic, host-independent — AGENTS.md), latency deliberately not measured.

## The question

Claim 3 (`ARCHITECTURE.md` §9) — a query gets *faster* as its predicate gets more
selective — is measured for a **lexical-term** gate (`bench/RESULTS_GATE_SWEEP.md`), which
is the weak, near-circular form because the gate *is* the lexical channel. The compelling
form is a scalar facet: `WHERE price < x ORDER BY emb <#> q`. Today that predicate cannot
enter the index (`WEAVE_CH_DOCVALS` is reserved), so it becomes an executor Filter on top
of the vector scan. **How much does that cost, and how much would a docvals gate shuttle
recover?**

## Method

Same corpus, same fused objective `fuse(body <=> wq, emb <#> qv, weights => {0.5,0.5})`
ORDER BY, two gates of matched selectivity:

- **Arm S (scalar facet, today):** `WHERE id % N = 0` — a pure executor Filter (no matching
  index), uniform selectivity 1/N. Chosen over an added `fct` column because
  ALTER+UPDATE churns every row and breaks the fused warp map's ascending-docid
  precondition (`ERROR: vector warp map is not in strictly ascending docid order`); a
  modulus on the existing `id` needs no schema change and no reindex.
- **Arm L (pushable lexical gate):** `WHERE body @@@ term` with the term's df matched to the
  target selectivity — the gate the fused scan *can* turn into a docid bound.

Primary metric: `weave_work_stats().vec_blocks` / `vec_lanes` (vector kernel work, any
path). 20 queries/point. **A FIRST PASS WITH A PLAIN `ORDER BY emb <#> q` (no fuse())
RETURNED PRIZE 1.0× AND IS THE INSTRUCTIVE NEGATIVE:** a plain vector scan scores every
block regardless of the gate (lexical Index Cond *or* scalar Filter), so claim 3's savings
do not exist there at all — they live entirely in the FUSED scan, where the gate's docid
bound skips vector work. The measurement below is therefore of the fused objective.

## Result — the prize grows with selectivity, and the scalar case is INVERTED

| corpus | target sel | Arm S blocks (scalar Filter) | Arm L blocks (pushed) | prize | Arm S Rows Removed |
|---|---|---|---|---|---|
| fiqa (57,600) | 0.1 | 41,400 | 35,080 | 1.2× | 67 |
| fiqa | 0.01 | 108,000 | 10,380 | 10.4× | 1,628 |
| fiqa | 0.001 | 165,600 | 2,000 | **82.8×** | 11,820 |
| scifact | 0.1 | 4,050 | 3,240 | 1.2× | 52 |
| scifact | 0.01 | 9,558 | 1,700 | 5.6× | 748 |
| scifact | 0.001 | 12,960 | 100 | **129.6×** | 5,178 |

Two facts, and the first is the one that matters:

- **The scalar-facet query gets SLOWER as the predicate tightens.** Arm S vector work
  *grows* (fiqa 41k → 108k → 166k blocks) because the fused scan has no bound from the
  facet, so it must consume more of the vector-ordered stream to find k rows that survive
  the Filter — `Rows Removed by Filter` grows 67 → 1,628 → 11,820. **This is precisely the
  over-fetch / "collapsing recall" failure mode claim 3 is defined against**, and it is what
  a filtered ANN with no shared docid space has to do. Claim 3 does not merely fail to hold
  for scalar facets today; it is inverted.
- **A pushable gate holds claim 3** (Arm L falls 35k → 10k → 2k), so the mechanism is
  present in the fused core — it is the scalar predicate's *inability to reach it* that
  costs. The prize a docvals gate shuttle would recover is that gap: **up to 82.8× (fiqa)
  / 129.6× (scifact) fewer vector blocks at 0.1 % selectivity**, rising monotonically with
  selectivity.

Reproduces at two corpora (rule 11 satisfied for a local, work-only measurement); the
0.001 prizes differ (82.8× vs 129.6×) partly because the matched lexical term's selectivity
is not identical to the modulus arm's (escrow 0.0018 vs tax 0.0010). Latency and a 1M scale
are unmeasured and belong to the build, not the sizing.

## Recommendation

Build the scalar docvals channel. It is the largest lever measured in the project and the
only one that generalizes claim 3 — the differentiator — from "another lexical term" to
"any scalar facet". This is an **architectural** change (new on-disk channel: page kind,
writer at build/flush/merge, reader, `block_max` bound property test per hard rule 1, fuzz
target, crash-recovery + concurrency TAP, upgrade path) and needs its own design + approval
before implementation. `doc/PHASES.md` V17's last sentence already flags this gap.

## MEASURED WITH THE CHANNEL — 2026-09-26 (the channel is now built; docvals int8 slice)

The sizing above compared arm S (un-pushable scalar Filter) against arm **L** (a pushable
LEXICAL term of matched df) as a *proxy* for the prize a docvals gate would recover. The
docvals channel now exists (`int8_docval_ops`, ext 0.25.0), so this re-runs the same spike
with a real arm **D**: `WHERE price <op> c` on an int8 column indexed `int8_docval_ops`,
where `price` is a scattered 1..N rank (`row_number() OVER (ORDER BY hashint8(id))`) so
`price <= round(N*sel)` has exactly the target selectivity and is uncorrelated with docid
(the general "any scalar facet", not a docid-contiguous best case). Harness
`/scratch/pg_weave/dvprize8.sh` (throwaway, work counters only, deterministic /
host-independent — `weave_work_stats().vec_blocks`, 20 queries/point, `fuse(body<=>wq,
emb<#>qv, weights={0.5,0.5})` ORDER BY, LIMIT 10). Local `lpg`; latency deliberately not
measured. **Correctness checked first (the G51 / rule-15 discipline: an empty gate would
read as `vec_blocks=0` = a fake infinite prize)** — every arm D selectivity agrees with the
heap as a set before any block is counted.

| corpus | target sel | arm S blocks (Filter) | arm L blocks (lexical) | arm **D** blocks (docvals) | prize S/D |
|---|---|---|---|---|---|
| fiqa (57,600) | 0.1 | 41,400 | 35,420 | 11,680 | 3.5× |
| fiqa | 0.01 | 108,000 | 9,860 | 2,380 | 45.4× |
| fiqa | 0.001 | 165,600 | 1,100 | **320** | **517.5×** |
| scifact (5,183) | 0.1 | 4,050 | 3,240 | 340 | 11.9× |
| scifact | 0.01 | 9,558 | 1,800 | 40 | 238.9× |
| scifact | 0.001 | 12,960 | 100 | **20** | **648.0×** |

Three facts, and the first is the claim:

- **Arm D's vector work FALLS monotonically as the predicate tightens** — fiqa
  11,680 → 2,380 → 320, scifact 340 → 40 → 20 — while arm S (the same predicate as an
  executor Filter, which is what `WHERE price<x` was before this channel) *rises*
  41,400 → 108,000 → 165,600. **Claim 3 (`ARCHITECTURE.md` §9) now holds for a scalar
  facet**, not just for another lexical term: the docvals gate's docid bound skips vector
  work in the fused scan, and the more selective the facet the more it skips. This is the
  differentiator, demonstrated end to end with the channel built — the inverted curve the
  sizing measured is now a falling one.

- **Arm D beats the lexical proxy (arm L)** at every selectivity below 0.1, and by a wide
  margin at 0.001 (fiqa 320 vs 1,100; scifact 20 vs 100). The sizing under-projected the
  prize (it capped at arm L's 82.8× / 129.6×) because a matched-df lexical term is only
  *approximately* the target selectivity, whereas the docvals gate is *exact* — the recovered
  prize is 517× (fiqa) / 648× (scifact) against the un-pushable baseline, larger than the
  proxy predicted.

- **The scattered facet does not defeat the pruning.** The open question the plan flagged
  (does the prize hinge on facet↔docid spatial correlation, the `RESULTS_BOUND_PRUNING`
  lesson?) is answered NO for this mechanism: `price` here is a hash-scattered rank,
  uncorrelated with docid, and the block work still falls with selectivity, because the
  fused gate bounds the *candidate docid set* the vector channel scores, not a contiguous
  block range.

Caveats (rule 8 / rule 11 / rule 15): work counters only, two corpora, LIMIT 10, warm,
local. The scifact 0.001 cell is a **sub-k regime** — `round(5183·0.001)=5` gate rows < the
10-row LIMIT — so its 20 blocks is the scan exhausting a 5-row gated set, a legitimate bound
but not a top-10 fill; the fiqa cells and scifact 0.1/0.01 are all ≥ k. This is a
work-reduction ratio, **not** a latency or a 1M-scale result (those are owed, and per
rule 15 the falling curve shows no floor within the swept grid — it is not a wall). The
falls reproduce across both corpora (rule 11). Found and fixed en route: **G51** — the
first run returned `vec_blocks=0` for arm D because the segment merge dropped the docvals
weft (silent empty gate); the numbers above are post-fix, with the index REINDEXed and the
gate verified against the heap.
