# Specification: fused-threshold top-k

Status: **design, unimplemented.** Task **F1**–**F4** in `doc/PHASES.md`.
Owner header: `include/weave/fuse.h`. Implementation: `src/am/fuse.c`.

This is the one genuinely novel component of pg_weave. Everything else is either
forked, imported, or a C reimplementation of a published algorithm. Read this
before touching `src/am/fuse.c`, and read `include/weave/channel.h` first.

## 1. The problem with what everyone does

Every hybrid retrieval system in production — zvec-grep, pg_turbovec's
`hybrid.rs`, ParadeDB, LangChain, Vespa's default profile — combines lexical and
vector results with **Reciprocal Rank Fusion**:

```
RRF(d) = Σ_channels  1 / (k + rank_channel(d)),    k = 60
```

Three things are wrong with it.

1. **It forces an over-fetch.** To get a correct top-10 you must retrieve `k' >>
   10` from *every* channel, because a document ranked 200th by BM25 and 3rd by
   the vector channel can win. In practice `k' = 100..1000`. You do 10–100× the
   necessary work in both channels and then throw almost all of it away.
2. **It discards magnitude.** A document that is *barely* the 3rd best vector
   match scores identically to one that is overwhelmingly the 3rd best. All
   calibration information in the scores is deliberately destroyed.
3. **The constant is unlearned.** `k = 60` is used universally because Cormack
   et al. used 60 in 2009. It is not tuned per corpus, per channel count, or per
   channel type, and nobody checks.

RRF is popular because it is *robust to incomparable score scales* — that is a
real problem and RRF is a real answer to it. But it solves that problem by
throwing away the information that would let you prune, which means it can never
be a top-k *algorithm*, only a post-processing step.

## 2. The observation

Both channel families admit a cheap, provable **upper bound** on the score any
document in a block can achieve.

- **Lexical.** `WeaveBlockHdr` already stores `max_tf` and `min_doclen` for each
  128-document block (`include/weave/am.h`). Substituting them into the BM25
  saturation function gives the block's maximum possible contribution. This is
  exactly block-max WAND, and the forked lexical channel already does it.
- **Vector, quantized. MEASURED 2026-09-13: THIS BOUND DOES NOT PRUNE ON REAL
  CORPORA.** It is sound, and it is useless — 0.00% of blocks skipped on
  GIST-960d, 0.01% on GloVe-200d, against 99.6% on the synthetic corpus it was
  first measured on (`bench/RESULTS_CODE_SCAN.md`). Two reasons, both structural:
  for L2-normalized data the Cauchy–Schwarz term `max‖rec‖·‖q‖` is ≈ 1.0 by
  construction while θ is always under 1.0, and the residual term is derived from
  `⟨q, rₛ − c⟩ ≤ ‖q‖·‖rₛ − c‖`, which assumes the residual points along `q` and is
  therefore loose by a factor growing with `√dim`. Real k-means blocks have mean
  radius 0.585 (GIST) to 1.018 (GloVe) on a unit sphere, so `‖q‖·R` alone clears θ.
  The description below is retained because the bound is still what the code
  computes and its soundness is asserted in `bench/code_scan.c`; it is no longer a
  claim that the vector channel prunes. A code block stores 32 vectors, and its
  header stores
  the block's centroid (itself as a quantized code) plus a radius
  `R = maxₛ‖rₛ − c‖`. Then `⟨q,rₛ⟩ ≤ ⟨q,c⟩ + ‖q‖₂·R` for every member. One
  lookup-table gather per block; no page reads beyond the header the shuttle is
  already holding. See `doc/specs/VECTOR_CHANNEL.md` §6.

  **This is the corrected bound.** The first draft of this document proposed the
  obvious analogue of `max_tf` — a per-coordinate maximum over the query lookup
  table — and it was measured to prune **0.0 %** of blocks, because in dimension
  *d* it is looser than plain Cauchy-Schwarz by a factor of order √d. The
  centroid+radius bound prunes 99.6 % on the same data. It also only works if
  the warp is ordered so that block members are spatially near each other, which
  turned a scheduling detail into a gated requirement (task V13). All of it is in
  `bench/RESULTS_BOUND_PRUNING.md`; that measurement is the reason §8 of this
  document insists the `score()`-call ratio be checked before anything is
  optimized.
- **Boolean channels** (fuzzy, regex, scalar) bound trivially at `±∞` per
  `channel.h` (C5).

If every channel can bound itself, the classic WAND argument generalizes.
Nothing about WAND requires the channels to be term postings; it requires only a
monotone cursor and a true block bound. That is precisely the shuttle contract.

## 3. The algorithm

Let the query name channels `1..m` with user weights `w_i ≥ 0`, a fused score

```
S(d) = Σ_i  w_i · s_i(d)
```

and a heap of the best `k` documents seen so far, whose smallest score is the
threshold `θ` (`θ = -∞` until the heap fills).

```
                                                     -- setup
for each channel i:
    Bi  <- w_i * shuttle_i.maxscore          -- bolt-wide ceiling
sort channels by descending Bi
partition into ESSENTIAL / NON-ESSENTIAL such that
    Σ_{i in NON-ESSENTIAL} Bi  <=  θ
    (initially every channel is essential, since θ = -inf)

                                                     -- document-at-a-time
p <- 0
loop:
    -- pivot selection: the smallest warp position that any essential channel
    -- can reach, since a document not reachable by ANY essential channel
    -- cannot beat theta by definition of the partition
    p <- min over ESSENTIAL i of  shuttle_i.seek(p)
    if p == WEAVE_WARP_END: break
    if not live(p): p <- p + 1; continue          -- one livedocs test, here only

    -- block-level prune: sum the per-block bounds of every channel that could
    -- contribute at p.  This is the step RRF cannot do.
    ub <- 0
    for each channel i with shuttle_i.cur <= p <= shuttle_i.blkend:
        ub <- ub + shuttle_i.block_max()
    if ub <= theta:
        -- skip the whole intersected block range, not just this document
        p <- 1 + min over contributing i of shuttle_i.blkend
        nblkskip++
        continue

    -- document-level evaluation, with incremental abandonment
    S <- 0
    remaining <- ub
    for each contributing channel i, in descending Bi order:
        S <- S + shuttle_i.score()
        remaining <- remaining - shuttle_i.block_max()
        if S + remaining <= theta:      -- cannot recover; stop scoring
            goto next
    heap_push(p, S)
    theta <- heap_min()
    repartition ESSENTIAL / NON-ESSENTIAL   -- theta grew; more channels may
                                            -- become non-essential
next:
    p <- p + 1
```

Three prunes, in increasing strength: the essential/non-essential partition
(MaxScore) removes whole channels from pivot selection; the block bound removes
whole 32- or 128-document ranges; incremental abandonment removes the remaining
per-document score calls.

### 3a. Five corrections to the pseudocode above — TASK F1, 2026-09-20

The loop was implemented (`include/weave/fuse.h`, `src/am/fuse.c`) and the
pseudocode above is **left as written** because four of these five are not typos,
they are places where a reasonable reading produces a silently wrong answer. The
header of `fuse.h` carries each one at length next to the code that avoids it;
this is the index.

1. **Pivot selection as written performs a backward seek.** `min over ESSENTIAL i
   of shuttle_i.seek(p)`: if channel A answers 50 and B answers 10, the pivot is
   10 and the next round calls `A->seek(11)` — below what A already returned.
   `channel.h` entitles a channel to **refuse** that, and both implemented
   shuttles do. The scorer must remember each channel's last returned position
   and seek only those standing below the target. Not an optimization: the literal
   loop errors out on the second iteration of any unaligned multi-channel query.

2. **A boolean channel is conjunctive, and (C5)'s "no special case" was false.**
   The loop sums only the *contributing* channels, so a gate that has advanced
   past the pivot contributes nothing rather than -INF and **the predicate is
   silently not applied**. Channels split into REQUIRED (intersect; excluded from
   the partition and from `ub`, because a gate's +INF bound would make `ub`
   infinite and kill the block prune) and SCORED (sum). (C5) in `channel.h` is
   corrected in place, and a channel now declares `required` on its shuttle —
   `kind` cannot carry it, since Z9's `<@>` distance channel is scored and labels
   itself `WEAVE_CH_FUZZY`.

3. **Non-essential channels must still be advanced to the pivot.** The
   contributing test `cur <= p <= blkend` reads like a filter over channels that
   happen to be nearby, but a non-essential channel is never seeked by pivot
   selection, so it sits at position 0 and the test excludes it for every `p > 0`
   — dropping its contribution from every score. What MaxScore saves is that a
   non-essential channel generates no *candidates* and can be abandoned before
   `score()`; it does not save the seek. Found by the property test at 40,574
   disagreements in 303,073 comparisons.

4. **`remaining` by subtraction, and the three NaN traps.** `remaining <- ub` then
   subtracting each bound is `INF - INF` with two contributing gates; `0 * INF` is
   a zero weight on a gate's ceiling; `-INF + INF` is the abandonment test with a
   vetoed document and an unscored gate. Each evaluates false against θ, so each
   *silently disables a prune* while the answers stay plausible. `remaining` is a
   suffix sum, weights must be `> 0` and finite (not merely non-negative as §7
   says), and a -INF running sum is discarded before it meets anything.

5. **§3 never defines the candidate set, and the definition is not free.** A
   position no scored channel reaches is **not** a result: it matches nothing, its
   fused score is 0, and padding the top-k with such rows answers "the ten best
   documents" with documents containing none of the query. The candidate set is
   the **union** of the scored channels' positions intersected with the required
   channels'; with no scored channel, the required intersection alone. Pivot
   selection gives the fast path exactly this, so §4's proof reaches the right
   answer — but *not by the argument it gives*: the pivot-skip case argues
   `S(d) <= Σ non-essential ≤ θ`, and at the start of a scan every channel is
   essential, so that sum is 0 while θ is -INF. The skip is right; this definition
   is why.

And one property that is not a correction but must be stated, because it decides
whether two plans agree: **float32 addition is not associative, so a fused score
is only well defined once the summation order is.** The order is required
channels first, then scored channels by descending weighted ceiling. Summing in
the caller's order instead produced last-ULP differences on 771 of 812,179
comparisons — the same rows, differing in the seventh digit — as soon as the
property test was given a fine-grained score lattice. Nothing was wrong with
either sum; there were two definitions of one number. The order must therefore
come from the channel set, which is stable, and never from the plan or from which
channels happened to be pruned. This is a constraint on F2 and F3.

## 4. Correctness

**Claim.** The algorithm returns exactly the top `k` documents by `S(d)`,
identical to a full scan, provided every channel satisfies (C1) and (C2) from
`channel.h`.

**Proof sketch.** A document `d` is discarded on exactly one of three paths.

- *Pivot skip.* `d` was not reachable by any essential channel. By construction
  of the partition, `Σ_{non-essential} B_i ≤ θ`, and `d`'s score is a sum over
  non-essential channels only, so `S(d) ≤ θ`. It cannot enter the heap.
- *Block skip.* `ub` is a sum of per-channel block bounds over exactly the
  channels that can contribute at `d`. By (C2) each term dominates that
  channel's score at `d`, so `S(d) ≤ ub ≤ θ`.
- *Incremental abandonment.* `S` holds exact contributions already computed;
  `remaining` holds the sum of bounds of channels not yet computed. By (C2),
  `S(d) ≤ S + remaining ≤ θ`.

θ is non-decreasing, so a document discarded against θ would also be discarded
against every later, larger θ. Hence no discarded document belongs in the final
top-k. ∎

The proof consumes (C2) three times and nothing else. This is why (C2) is a
correctness requirement with a mandatory property test and not a tuning knob: a
bound that is 1% too low silently loses rows, and no regression test that
compares against a fixed expected output will notice on a different corpus.

**Degenerate cases, which double as the regression tests:**

| Query shape | Reduces to | Test |
|---|---|---|
| one lexical channel | block-max WAND, byte-identical to today's ranked scan | `sql/fuse_degenerate.sql` |
| all channels boolean | bitmap AND, no scoring | same |
| `k = 1` | best-match with maximal pruning | same |
| all weights equal, one channel | plain BM25 top-k | same |
| `m` channels, `θ` never rises (all scores equal) | full scan, no pruning, still correct | `test/hegel/test_fuse_props.c` |

## 5. Essential / non-essential partition

Recomputing the partition on every heap push is `O(m log m)`; `m` is at most a
few dozen (query terms plus channels), so this is not the bottleneck, but the
partition should be maintained incrementally: channels are kept in a
descending-`B_i` array with a running suffix sum, and the split point only ever
moves in one direction as θ grows. `src/am/fuse.c` should implement it as a
single `int split` index into that array.

~~Heuristic worth measuring, not assuming: for a *vector graph* channel, being
non-essential is much more valuable than for a lexical channel, because a graph
shuttle that is never used for pivot selection can skip its traversal entirely
and degrade to a pure verifier. Consider biasing the sort to put graph channels
last. `bench/fuse_partition.sql` should A/B this.~~

**SUPERSEDED 2026-09-20, with §6 and task F4: there is no graph channel.** The
heuristic's whole value was that a graph shuttle demoted out of pivot selection can
skip its *traversal*; no other channel kind has a traversal to skip, so the bias has
nothing to bias. Descending `B_i` is the plain MaxScore order and stands unmodified,
and `bench/fuse_partition.sql` is not owed. What survives the withdrawal is the
observation underneath it, which is about cost asymmetry rather than graphs: when two
channels have comparable `B_i` but very different `score()` costs, the expensive one
is worth demoting first, because the partition's benefit is measured in avoided work
and not in avoided positions. The code scan's `score_block()` (32 lanes in a few SIMD
instructions) versus a posting-list decode is the live instance. **Unmeasured**, and
deliberately not implemented on that reasoning alone — descending `B_i` is what §4's
proof is written against, and any reordering has to preserve the invariant
`Σ_{non-essential} B_i ≤ θ` rather than merely look plausible.

## 6. Interaction with the vector graph channel

**SUPERSEDED 2026-09-20 — THIS SECTION HAS NO SUBJECT. The vector proximity graph
was withdrawn in Phase V, so nothing described below is built, and task F4, which
existed to build it, is withdrawn with it.** The section is left in place per hard
rule 13 rather than deleted, because the design is sound and would be the right
answer if a graph channel is ever re-justified.

What happened, so the withdrawal is checkable and not just asserted. pg_turbovec
deprecated its graph kind in v2.5.0 having measured that at R@10 ≥ 0.98 on
GIST-10M/960-d, IVF reached 28.4 ms while the graph could not reach 0.98 at **any**
latency (ceiling 0.873 at 181 ms) and built 57–90× slower. V9's IVF is separately
demoted. The ratified Phase V shape (2026-09-13) is a flat 32-lane code scan plus an
exact float32 top-25 rerank read from the heap — recall@10 0.9920 at n=1M on
GIST-960d. So `WEAVE_CH_VECTOR_GRAPH` has no implementation and no planned one, and
`set_visit_filter` in `channel.h` is an optional vtable slot that no channel fills.

Three consequences that matter to the scorer, all of them simplifications:

1. **Every shuttle's `seek` is monotone by construction.** Non-monotonicity was the
   graph's alone (best-first traversal order is not warp order), and it is the reason
   this section had to exist. (C1) is now a contract every implemented channel
   satisfies naturally rather than one that needed a preparation step to rescue.
2. **The fused loop's cursor is always the exhaustive code scan**, which is what
   `recall=exact` already meant. The approximate arm is gone, not disabled.
3. **§8's `recall ≥ 0.99 with graph on` row is struck**, because recall against an
   exhaustive fused scan is 1.000 by construction when no channel is approximate.
   Claim 3 in `doc/ARCHITECTURE.md` §9 — that queries get *faster* as predicates get
   more selective — loses the mechanism this section gave it (filter-steered
   traversal) and must rest on the other two: the boolean gate narrowing pivot
   selection, and the block bound skipping whole intersected block ranges. Both are
   §3 mechanisms, neither needs a graph, and neither is measured yet.

Superseded design follows.

The graph channel is the one shuttle whose `seek` is not naturally monotone —
Vamana traversal visits nodes in best-first order, not warp order. Two options,
and the spec picks the second:

1. **Materialize.** Run the traversal to completion for `k' = k · oversample`,
   sort the visited set by warp position, and expose it as a monotone cursor.
   Simple, but reintroduces the over-fetch we set out to eliminate.
2. **Two-phase (chosen).** The graph channel runs *first*, as a bound generator,
   not a cursor. Its traversal is steered by the visit filter built from the
   boolean channels (`set_visit_filter`, `channel.h`). It produces a warp-ordered
   candidate set *and* a per-block distance bound for the code-scan shuttle.
   The fused loop then runs over that candidate set with the code-scan shuttle
   supplying exact vector scores. The graph is thus a *bound source*, and the
   scan channel is the cursor.

Option 2 keeps the algorithm in §3 unchanged and confines the non-monotonicity
to a preparation step. It also makes the filter pushdown natural: the visit
filter is available before the traversal starts, which is the whole point.

The consequence to document honestly: with a graph channel present, pg_weave is
approximate in exactly the way HNSW is approximate — the graph may fail to
reach a true nearest neighbour. `recall=exact` disables the graph and uses the
code-scan shuttle as the cursor, at `O(n)` cost. That is the frontier from
`doc/ARCHITECTURE.md` §8.1 and there is no way around it.

## 7. SQL surface

```sql
-- Fused ranking.  Weights default to equal.  Negative weights are rejected:
-- they would break (C2) under weighting.
ORDER BY fuse(
    body      <=> 'postgres index'::wquery,   -- lexical, BM25
    embedding <=> $1::wvec,                   -- vector, cosine
    weights => '{0.4, 0.6}'
) LIMIT 10

-- The fused score is retrievable, and it means something (it is a weighted sum
-- of calibrated per-channel scores, not a rank reciprocal).
SELECT id, score() FROM ...

-- Per-channel breakdown, for debugging relevance.  Costs one extra score()
-- call per returned row; never called during pruning.
SELECT id, score_parts() FROM ...     -- returns float4[]

-- RRF remains available for users who want rank-only fusion, and is the
-- baseline the fused scorer is benchmarked against.
ORDER BY weave_rrf(ARRAY[...], k => 60)
```

`fuse()` is not a real function: it is recognized by the planner support
function and turned into an `ORDER BY` operator pushdown against the `weave`
index, the same way `<=>` is today. If the index cannot serve it (wrong
opclasses, no matching index) the planner must fall back to a Sort over an
executable implementation of the same arithmetic, so the query never simply
fails. `sql/fuse_fallback.sql` tests that path.

## 8. What must be benchmarked before this is called a win

The claim being made is "no over-fetch, better quality, lower latency". All
three need numbers on the same corpus, against RRF-with-over-fetch as the
control. `bench/fuse.sql` and `bench/RESULTS_FUSE.md`.

| Metric | Control | Gate to claim a win |
|---|---|---|
| nDCG@10 | RRF `k'=100` | ≥ RRF, on ≥2 public datasets (BEIR subset + MS MARCO passage) |
| p50 latency, k=10 | RRF `k'=100` | ≤ 0.5× RRF |
| p99 latency, k=10 | RRF `k'=100` | ≤ 0.7× RRF |
| channel score() calls | RRF `k'=100` | ≤ 0.2× RRF (this is the mechanism; if it is not much lower, the bounds are too loose and §2 is wrong) |
| recall vs exhaustive fused scan | — | ~~≥ 0.99 with graph on;~~ **1.000**, and it is 1.000 *by construction* now that the graph channel is withdrawn (§6) — every implemented channel is exact, so this row tests the scorer's pruning, not an approximation. A single miss is a (C2) violation, which makes it the most valuable row in the table rather than the weakest |

If the `score()` call ratio is not dramatically lower, stop and fix the bounds
before optimizing anything else — a loose bound makes the entire design pointless
and no amount of SIMD recovers it. Record the negative result in
`bench/RESULTS_FUSE.md` either way; `pg_fts/bench/` and
`pg_turbovec/docs/PARITY_GAPS.md` are the house style for that, and the retracted
"we win 2.3×" claim in the latter is exactly the mistake to avoid.

## 9. Prior art to cite, and to not reinvent

- Broder et al. 2003, **WAND** — the pivot/threshold idea.
- Ding & Suel 2011, **BlockMax WAND** — per-block bounds and block skipping.
- Turtle & Flood 1995 / Mallia et al. 2017, **MaxScore** — the
  essential/non-essential partition.
- Cormack et al. 2009, **RRF** — the baseline being displaced.
- Bruch et al. 2023, *An Analysis of Fusion Functions for Hybrid Retrieval* —
  the case that score-based fusion beats rank-based when scores are calibrated,
  which is the assumption this design rests on. If this paper is wrong, the
  quality gate in §8 will fail and RRF stays the default. Say so in the README
  if that happens.

The novelty here is not any one of those; it is applying block-max pruning
*across heterogeneous channels with incomparable score domains, under one
threshold*, which requires the shuttle abstraction and the per-channel bound
contract to exist first. That is the contribution.
