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
- **Vector, quantized.** A code block stores 32 vectors, and its header stores
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

Heuristic worth measuring, not assuming: for a *vector graph* channel, being
non-essential is much more valuable than for a lexical channel, because a graph
shuttle that is never used for pivot selection can skip its traversal entirely
and degrade to a pure verifier. Consider biasing the sort to put graph channels
last. `bench/fuse_partition.sql` should A/B this.

## 6. Interaction with the vector graph channel

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
| recall vs exhaustive fused scan | — | ≥ 0.99 with graph on; **1.000** with `recall=exact` |

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
