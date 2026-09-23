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
executable implementation of the same arithmetic **over the
values the operators can compute outside an index — which is not the same answer, and
cannot be; see §7a (1)** — so the query never simply
fails. `sql/fuse_fallback.sql` tests that path. **Read §7a before implementing any of
this section: three of its claims are corrected there, including the direction of the
ordering and the existence of the two channels this example uses.**

## 7a. What F2 found when it read the code §7 describes (2026-09-21)

§7 is four sentences of SQL surface and it takes three corrections and two decisions
before any of it can be implemented. Same pattern as §3a: written here rather than
fixed quietly, because two of the three would otherwise become *plausible wrong
answers* — a query that returns a different top-k depending on the plan.

**(1) "Fall back to a Sort over an executable implementation of the same arithmetic"
cannot be satisfied, and not because of an implementation gap.** `weave_distance()`
(`src/query/rank.c:459-487`) is the `<=>` operator's out-of-index implementation, and
it is an **approximation by necessity**: a bare operator call does not know the corpus,
so it uses `df = 1` and `avgdl = |D|`. The index computes exact BM25. So the fallback
already ranks differently from the pushdown *for a single `<=>` channel*, today, with no
fusion involved — that is what pg_fts's `Limit[NO-INDEX]` plan has always been doing
(see L7 in `doc/PHASES.md`). Fusion inherits it and cannot repair it.

Consequences, and they bind `sql/fuse_fallback.sql`:

- The fallback's promise is **"the query never fails and computes the same formula over
  the values the operators can compute outside an index"** — not "the same answer".
  §7's wording is corrected to that.
- `sql/fuse_fallback.sql` may assert: the query runs, returns the right candidate
  **set** where the `WHERE` clause determines it, and the fallback's arithmetic matches
  a hand-written expression. It may **not** assert that pushdown and fallback return
  the same order, and it should contain one case where they demonstrably do not, so the
  divergence is a tested property rather than a surprise.
- F5 adds a second reason the word "identical" is unavailable: float32 addition is not
  associative, so even two exact implementations agree only if they sum in the same
  order (771 of 812,179 comparisons differed in the last ULP until the oracle summed
  required-first, then scored by descending weighted ceiling).

**(2) §7's example is direction-inconsistent with §7's own prose, and the house
convention settles it.** `body <=> 'q'::wquery` is a **distance** — `1/(1 + score)`,
smaller is better (`rank.c:460-462`) — so `ORDER BY fuse(...) LIMIT 10` with an implicit
`ASC` is a distance ordering, while the prose two lines later calls the fused value "a
weighted sum of calibrated per-channel scores", where larger is better. Decision:

- **`fuse()` takes SCORES and returns a DISTANCE**: `1 / (1 + Σ wᵢ·sᵢ)`, in `(0,1]`,
  ascending, which is exactly the map `<=>` already uses and for the reason stated
  there. §7's `LIMIT 10` example then works verbatim, and the *score* — the number that
  "means something" — stays retrievable through F3's `score()`.

  **SUPERSEDED 2026-09-21 by F2.1, which implemented it: `fuse()` returns the NEGATED
  weighted sum, `−Σ wᵢ·sᵢ`, and ascending order is still best-first.** `1/(1+S)` is
  total and monotone only because BM25 is `≥ 0`. A *fused* sum is not: cosine
  similarity lives in `[−1, 1]`, so a score recovered from `wvec <=> wvec` is negative
  whenever the vectors point apart, and a weighted sum containing it can reach the pole
  at `S = −1`, on either side of which the map is not monotone. The failure that
  produces is a **wrong order**, not an error — the class this project treats as worse
  than a refusal. Negation is total, exactly order-reversing, and needs no domain
  argument; the cost is only that the value is not in `(0,1]` like a `<=>` distance,
  which costs nothing for an `ORDER BY` expression. The reasoning also lives next to
  the arithmetic, in `src/am/fusepath.c`'s header comment, and in
  `sql/pg_weave--0.13.0--0.14.0.sql`.
- **The spelling `fuse(col <=> q, ...)` is planner-rewritten, not evaluated as written.**
  The planner sees each argument's operator, therefore knows each argument's channel,
  therefore knows the inverse of that channel's distance map (lexical `<=>`:
  `s = 1/d − 1`; `wvec <=> wvec` cosine distance: `s = 1 − d`). It emits either the
  pushdown or a fallback expression that recovers scores. A `fuse()` call whose
  arguments the planner cannot attribute to a channel is **still well defined** —
  its arguments are taken as scores, which is what the function's own declaration says —
  and it is not an error, because §7 requires that the query never simply fail. The one
  thing the implementation must never do is *guess* a distance map from a float.

**(3) Neither channel in §7's example can be an ORDER BY operand today, which is the
real scope discovery.** Checked in code before writing any:

- **The lexical channel has no shuttle.** Ranked lexical is a Broder/BMW WAND over
  `WandCursor` (`src/am/amscan.c:4361-4423`, pivot loop at `:5024-5157`), not a
  `WeaveShuttle`. The fused core can only consume shuttles. The good news is that the
  adaptation is **glue, not a format change**: `WandCursor` already carries the current
  docid, the decoded block (so `blkend` is `docids[blkcount-1]`), the per-block bound
  inputs `blk_max_tf`/`blk_min_dl` with `wand_block_max_contrib()` (`:4605-4616`)
  computing the bound, and the term-wide ceiling `max_contrib`. `WeaveBlockHdr`
  (`include/weave/am.h:350-363`) already stores `max_tf` and `min_doclen` per block, so
  **no new on-disk field is needed.** New task **F6**.
- **The vector channel has no ORDER BY operator.** `wvec_weave_ops`
  (`sql/pg_weave--0.7.0--0.8.0.sql:99`) is `STORAGE wvec` and nothing else; V8's scan is
  reachable only through the `weave_vec_scan()` SRF. The `<=>`, `<->`, `<#>`, `<+>`
  operators on `wvec` exist as ordinary functions (`sql/pg_weave--0.1.0--0.2.0.sql:119-134`),
  which is why the **fallback** works today, but none is an opfamily ORDER BY member, so
  core cannot push one into the index and `amrescan` has no scan key for it. New task
  **F7**.

So F2 is staged. **F2.1** is the SQL surface, the planner recognition, and the fallback —
which is precisely F2's stated gate, `sql/fuse_fallback.sql`, and is testable with zero
fusible channels because the interesting path is the *refusal*. **F2.2** is the AM-side
fused scan. Until F6 and F7 land, the only scored channel reachable by an ORDER BY key is
`<@>` (`WEAVE_STRAT_EDIST`, `src/query/edist.c`), so a pushdown would have one scored
channel plus boolean gates — the degenerate case F5 already tests as a property.

**How weights reach the AM, decided 2026-09-21 (maintainer decision: plan shape (A)).**
A hand-built `IndexPath` in `set_rel_pathlist_hook`, whose `indexorderbys` are the
individual `col <=> q` `OpExpr`s plus **one transport key** carrying the weights array:
a new opfamily ORDER BY member (`<~>` over `(wdoc, float4[])` and `(wvec, float4[])`)
that exists only so that `amrescan` can see a `float4[]`. The alternatives and why they
lost: a `CustomScan` (rejected — it would re-implement `nodeIndexscan`'s heap fetch,
visibility, qual recheck and EPQ, which is where MVCC bugs live); a single real operator
over a new composite query type (rejected — it works, needs no planner code at all, and
it makes the SQL surface stop looking like §7, so it is the fallback position if (A)
proves unworkable). A *qual* key was considered for transport and rejected on a specific
hazard: `indexqualorig` is re-evaluated during an EPQ recheck, so a marker operator in a
qual would eventually be executed for real; order-by expressions are not.

**`required` is not transported and must not be.** A channel is required because it came
from a `WHERE` clause (`src/query/gate.c` sets it), and scored because it came from an
ORDER BY key. The AM therefore derives it from *where the key arrived*, never from the
channel kind — see §3a (2) and `include/weave/channel.h`'s (C5) note, where the
kind-based inference is the falsification that would silently drop every row `<@>` ranks.

## 7b. What F2.2 found when it built the pushdown (2026-09-21)

§7a staged F2 and predicted that F2.2 was "the AM-side fused scan", implying the
only missing pieces were glue. One thing was missing that is not glue, and it is
recorded here because it decides which shapes the planner is allowed to offer.

**The channels do not share a position space, and the fused core drives exactly
one.** Checked in code, not inferred:

- The **lexical** shuttle publishes `weave_tid_to_docid()` **docids** as its warp
  positions (`src/query/lexshuttle.c`, `lex_publish()`). So does a **gate** shuttle
  built from a `TidSet` (`include/weave/gate.h`). These two agree.
- The **vector** shuttle's warp is a **segment-local dense lane index**
  (`src/vector/vecshuttle.c`), related to a docid only through the weft's warp map
  — a forward-only page chain with no index over its pages.
- The **`<@>`** shuttle's warp is a position in the **dictionary**, not in any
  document space at all (`src/query/edist.c`; `weave_edist_pass()` walks terms and
  then reads each admitted term's postings).

`gate.h` already stated this as an open question and assigned it here: "the two key
spaces are NOT the same, and reconciling them is Phase F's decision, not this
task's." **F2.2's decision is the docid space**, on the grounds that it is the
space two of the four channel families already use, it is bolt-independent (which
`vecshuttle.c`'s own comment gives as the reason its allowlist argument is docids
and not warps), and it needs no translation layer to be correct on day one.

**So F2.2 ships the shapes that live in that space and its planner refuses the
rest.** A fused path is offered for two or more lexical `<=>` channels — each
expanding to one shuttle per query term — plus the boolean gate a `WHERE` clause
becomes. A `fuse()` naming `<->`, `<#>` or `<@>` gets **no path**, and the Sort
over the fallback stands. That is a refusal, not an error; `sql/fuse_pushdown.sql`
asserts each one falls back without a message. The alternative — offering the path
and translating badly — is G39 in `doc/GAPS.md`, an access method that advertises a
plan and then refuses it at run time.

**What the vector channel needs, so the follow-up is a task and not a rediscovery.**
One adapter shuttle, and the design is settled by the fact that the warp map is
written **docid-ascending** (`vec_docid_order()` in the writer; the ordering guard
`t/017_vector_syncscan.pl` exists to keep it that way):

- Materialize the bolt's `warp -> docid` array once per pass, which is one forward
  pass of the warp-map chain — the same pass `vec_allow_from_docids()` already makes.
- `seek(target_docid)` binary-searches the array for the first lane whose docid is
  `>= target`, seeks the underlying vector shuttle to that **lane**, and reports the
  docid of the lane it landed on. Monotone in both spaces, so (C1) survives.
- `blkend` reports the docid of the block's last lane. (C2) survives and is not even
  weakened: the docid interval `[cur, blkend]` covers the block's lanes plus docids
  this bolt does not carry, at which the channel contributes nothing anyway, and the
  core only sums the bound of a channel standing exactly on the pivot.

Cost: `8 * nlanes` bytes per bolt per pass. That is the whole of it, and it is why
this is recorded as owed rather than attempted — an adapter is cheap, but adding it
in the same change as the first working fused scan would mean debugging two new
things at once, which is the argument hard rule 7 makes about Phase F as a whole.

**The `<@>` channel is a different and larger job**, and calling it an adapter would
be wrong. Its shuttle's positions are dictionary terms, and a document's `<@>`
distance is the *minimum over its terms*, computed by reading each admitted term's
postings — so there is no monotone map from its warp to a document at all. Fusing
it needs a **document-space** `<@>` shuttle, which needs the per-document minimum to
be reachable without materializing every posting of every admitted term. That is a
channel design question, not a plumbing one.

**Two smaller findings, both stated where they bind.**

1. **(C6) is not applied by the scorer on this path, and cannot be.** (C6) says
   tombstones are applied once, by the scorer, from a warp-indexed bitmap — which
   presumes a *dense* warp. A docid is sparse: a bitmap over it needs one bit per
   `(heap block x MaxHeapTuplesPerPage)` slot, tens of megabytes on a large heap, to
   carry information the channels already hold. So `live` is NULL and each channel
   filters its own: a `WandCursor` skips **its own segment's** tombstones, which is
   also a semantics a shared bitmap could not express (a docid deleted in segment A
   must not suppress a live document that reused the heap slot in a newer segment).
   The deviation is in `src/am/amscan.c`'s fused-pass header comment too.
2. **The channel cap has to be enforced at plan time.** `weave_fuse_init()` refuses
   more than `WEAVE_FUSE_MAX_CHAN` channels, and a lexical key becomes one shuttle
   per term, so a query with enough terms would be a path the AM refuses at rescan
   — G39 again. The planner therefore reads each key's `wquery` `Const` and bounds
   the term count by `nitems` (an RPN item count, hence an over-estimate, which is
   the safe direction). The price is that a **parameterized** `wquery` gets no fused
   path: the term count is unknowable until the scan runs.

## 7c. What F8 found building the adapter (2026-09-22)

§7b specified the vector adapter, priced it at "8 bytes per lane per bolt per pass",
and called it cheap. The arithmetic was right and the estimate of the *work* was
wrong in a way worth recording: **two of the four defects F8 fixed were not in the
adapter at all**, and one of them was a live wrong answer that had been printed into
a checked-in expected-output file and reviewed three times.

### (1) The flagship query was ranking the vector channel BACKWARDS

`doc/GAPS.md` G41. `fuse()` sums **scores**, higher better, and negates once at the
end so ascending is best-first (§7a). A channel argument spelled as a **distance**
therefore has to be recovered into a score first, which is what the support
function's rewrite does. 0.14.0 shipped that recovery for the lexical `<=>` and for
the cosine `wvec <=> wvec` and stopped there, because at that point no vector
channel could be fused and the point of the functions was the rewrite.

So `fuse(body <=> q, emb <-> v)` — the query this spec has advertised since §7 was
written — passed the raw L2 **distance** into the sum as if it were a score, and the
final negation put the **farthest** vector first. It parsed. It ran. It returned ten
plausible rows.

Two things about how it survived are the actual lesson:

- **Both arms were wrong in the same direction.** The fallback computed the inverted
  sum, and had the pushdown existed it would have inverted it identically. Every
  assertion this project writes about `fuse()` compares the two arms against each
  other, so no amount of that testing could have seen it. The fix is the discipline
  §7a already arrived at from a different direction: assert against an **oracle**
  built from the index's own per-channel scores, not against the other arm.
- **It was visible in the expected output.** `expected/fuse_pushdown.out` contained
  `Sort Key: (fuse(weave_lexscore((body <=> 'alpha')), (emb <-> '[...]'), ...))` —
  one argument wrapped, the other bare. That asymmetry *is* the bug, printed, in a
  file three reviews read. A recovery function being absent looks exactly like a
  recovery function not being needed.

0.17.0 adds `weave_l2score(d) = -d*d`, `weave_ipscore(d) = -d` and
`weave_edistscore(d) = -d`. `-d*d` and not `-d` for l2 because the domain is the
**channel's**, not the operator's: the weft scores l2 as `-||q-v||^2`, so `-d` would
rank a lone vector channel identically while weighting it differently from the index
at every distance except 1 — and a fused sum is arithmetic, not a ranking.
`weave_edistscore()` ships even though `<@>` cannot be fused until F9, because the
two halves of G41 are independent: the pushdown needs a shuttle, the fallback needs
only the function, and fixing two of three channels would be an arbitrary place to
stop.

### (2) The metric mismatch became a PLAN-TIME refusal, which is new

`VECTOR_CHANNEL.md` §8b and the long comment in `weave_rescan()` both state that a
metric mismatch cannot be a planner decision, because core matches a pathkey against
an operator **family** and the metric is a reloption. That is true of *core's* path
generation. It is not true of ours: `src/am/fusepath.c` builds the fused IndexPath
itself, so it can open the index, read the reloption, and simply not offer the path.

So `fuse(body <=> q, emb <-> v)` on an `ip` index is a Sort, chosen at plan time, and
no query fails — where the single-channel `ORDER BY emb <-> v` on the same index
still raises at rescan. This does not retire §8b's per-metric opclass split; it
narrows what that split is still needed for. It also required a second, **non-throwing**
reloption accessor: `weave_index_vec_metric()` raises on cosine and l1, `ALTER INDEX
... SET (metric = 'cosine')` is accepted without a rewrite, and a planner hook that
raises on a catalog state it merely inspected would make such an index unplannable
for queries that never touch its vector column.

### (3) Duplicate docids are refused, and the first version's argument for accepting them was wrong twice

The adapter originally accepted a non-strictly-ascending warp map, arguing that a
duplicate docid "breaks nothing here, because both lanes lie inside any interval
containing either". Writing the property test refuted it, and the error is worth
naming because it is easy to make again: **it conflated the docid interval with the
lane block the inner bound actually describes.**

- (C2). With `docid = [10, 10, 10, 50]` and a block size of 1, `seek(0)` lands on
  lane 0 and publishes `blkend = 10`. Lane 1 also has docid 10, so it is inside
  `[cur, blkend]` — but the inner bound describes lane 0's block alone, and lane 1's
  score may be arbitrarily larger. A bound too low drops rows silently.
- (C4), which is worse and has no bound to blame. `score()` is defined at **one**
  lane, so the other lanes sharing that docid contribute nothing however honest the
  bound is. A document's score would depend on which of its lanes the search landed
  on.

Neither is fixable inside a relabelling: a docid with several lanes is not a
relabelling, it is an **aggregation** (a max over the lanes), which is a different
channel. So the map must be strictly ascending, which nothing legitimate violates.

### (4) `WEAVE_FUSE_END` has to be LATCHED, and the symptom would have been an ERROR

The core may ask an exhausted channel again — `fuse.h` note 1 promises only that
targets increase. `lower_bound(target) < nlane` does **not** imply the inner channel
can still be asked: the two spaces run out independently, because the inner stops
when it has no contribution left, which can happen while plenty of lanes (and
therefore plenty of docids ≥ the target) remain. Calling the inner after it has
returned END hands it a target *below* the position it last returned, and every real
shuttle refuses that — `src/vector/vecshuttle.c` raises "cannot resolve warp %u after
warp %u". So this one is not a wrong answer, it is an **ERROR mid-scan on a perfectly
good index**, and it was found by driving the adapter through the real core rather
than by reading it.

Worth stating alongside: the property that found it needed the *synthetic* inner
channel to assert its own backward-seek refusal. A tolerant stand-in would have
reported nothing.

### (5) The vector channel filters its own tombstones, and had to be made to

§7b's (C6) note says `live` is NULL on the fused path and each channel filters its
own segment's tombstones. The lexical channel does. The vector channel did not — it
has no tombstone logic at all, because the single-channel `ORDER BY` path relies on
the heap visibility check to drop dead rows and on its widening ladder to refill.
Inside a fused run that is not enough: a docid deleted in this bolt would still be
published, enter the candidate union, occupy one of the `k` heap slots, and displace
a live document. No wrong row is *returned* — the visibility check still drops it —
so the symptom is a **missing** row.

The fix is free where it lands, which is the only reason it is not a separate task:
the adapter already makes one forward pass over the warp map, so the tombstone probe
rides along (docid-ascending, so a forward-resume sparsemap cursor is O(1)
amortized), and the result is the shuttle's own `allow` bitmap — which
`weave_vec_scan_block()` tests *before* it is handed any code bytes.

### What the property test cost, and what it bought

`test/hegel/test_vecdocmap.c`: 18,899,792 checks, 0 failures, two positive controls
firing at 32.4 % and 20.3 %. It found (3) and (4). It also produced three of its own
false alarms, all in the harness, and they are the same class hard rule 11 names —
a generator that makes a number up: adding to `WEAVE_FUSE_END` wrapped a uint32 into
a backward target; `lane_bind()` drew a *random* weight and was called once per arm,
so the two arms answered different questions; and a slack `block_max` needs the
channel's `maxscore` to cover the slack, or the core's suffix arithmetic is unsound
and the fused answer legitimately differs from the reference.

## 8. What must be benchmarked before this is called a win

The claim being made is "no over-fetch, better quality, lower latency". All
three need numbers on the same corpus, against RRF-with-over-fetch as the
control. `bench/fuse.sql` and `bench/RESULTS_FUSE.md`.

| Metric | Control | Gate to claim a win |
|---|---|---|
| nDCG@10 | RRF `k'=100` | ≥ RRF, on ≥2 public datasets (BEIR subset + MS MARCO passage) |
| p50 latency, k=10 | RRF `k'=100` | ≤ 0.5× RRF |
| p99 latency, k=10 | RRF `k'=100` | ≤ 0.7× RRF |
| ~~channel score() calls~~ | ~~RRF `k'=100`~~ | ~~≤ 0.2× RRF (this is the mechanism; if it is not much lower, the bounds are too loose and §2 is wrong)~~ **SUPERSEDED 2026-09-22 (maintainer decision; the three rows below replace it). Left visible under hard rule 13 because every figure this project published for this row — 0.648× / 0.903× / 0.541× raw, 0.571× / 0.875× / 0.513× normalized — was measured in this unit, and the reason it was wrong is the finding** |
| **lexical work: BM25 contributions** | the WAND control's own BM25 contributions | ≤ 0.20× |
| **vector work: CODE BLOCKS READ** | the control's own code scan, in blocks | ≤ 0.20× |
| **pivots per query** | — | **REPORTED, NOT GATED.** The RRF control has no pivot loop, so there is nothing to take a ratio against; a number with no denominator is a diagnostic, and calling it a gate would be inventing the denominator |
| recall vs exhaustive fused scan | — | ~~≥ 0.99 with graph on;~~ **1.000**, and it is 1.000 *by construction* now that the graph channel is withdrawn (§6) — every implemented channel is exact, so this row tests the scorer's pruning, not an approximation. A single miss is a (C2) violation, which makes it the most valuable row in the table rather than the weakest |

**AMENDED 2026-09-22 (night): if this table keeps a work row, the row should count PIVOTS as
well as `score()` calls.** The EC2 re-run of the shipping scorer (`bench/RESULTS_FUSE.md`,
fourth measurement) moved the two rows in **opposite directions**: the `score()`-call ratio
*improved* on all three corpora (0.648 → 0.571, 0.903 → 0.875, 0.541 → 0.513 of the control)
while fiqa's measured p50 got **2.0× slower**. The missing work is the pivot walk — one
`seek()` plus one `block_max()` per contributing channel per pivot, and fiqa's pivot count
rose **5.3×** (7,081,750 → 37,306,460) while its vector lane count moved 4.6 %. A work row
that counts only `score()` calls cannot predict the latency row it stands in for, which is the
whole reason it is in this table. The counter already exists (`weave_fuse_stats()`); what is
missing is the gate's arithmetic. **No number is invented for the restated row here** — a gate
is honest only if it is written down before it is measured against (§8d option (a) makes the
same point about the unit).

**DECIDED 2026-09-22 — MAINTAINER DECISION, and it is the amendment above carried out: the
work row is restated PER CHANNEL, each channel in the unit its own storage has, plus one
ungated diagnostic.** The three rows in the table are what the gate now reads; the single
`score()`-call ratio is struck through above rather than deleted. Two measurements forced
the restatement, and neither is about the fused core:

  - **A lane is not a unit of cost; a block is.** In `WEAVE_PACK_LANE`, coordinate *j* of
    lane *s* is one nibble at byte `j*16 + s/2` (`include/weave/vecpage.h:18`), so a
    one-lane read touches **every byte of its block** — scoring 1 lane costs the same
    memory traffic as scoring 32. This project measured that during the V15/V16 work for
    an unrelated reason (`bench/RESULTS_CODE_SCAN.md:330,417`), which is why the restated
    unit is not a new claim about the layout: it is a gate catching up with a measurement
    that was already on disk.
  - **The lane-based row could not predict the latency row, and on the run that mattered
    the two disagreed in SIGN.** In lanes, normalization made the fused arm *cheaper* on
    all three corpora (0.648 → 0.571, 0.903 → 0.875, 0.541 → 0.513). The clock said fiqa's
    p50 **doubled**. A work row that moves opposite to the row it stands in for is
    measuring the wrong thing. What tracks the clock is the **pivot count** — fiqa's pivots
    rose **5.3×** against a p50 of **2.0×** — which is why pivots per query are reported.

**And the restatement does NOT rescue the row. Stated plainly so nobody reads it as a
pass:** measured in **blocks**, the vector ratio is **1.000×** — exactly what it was in
lanes — so the vector half of the gate is still failed on all three corpora. The
restatement buys **honesty about the unit, not a pass**. What it does buy is that the two
halves can now fail separately and be fixed separately: under the restated row the
**lexical** half is **MET on fiqa** (0.052×) and missed on scifact (0.203×, just over) and
nfcorpus (0.380×), while the **vector** half is missed **everywhere at 1.000×**. Figures,
harness and the full-pass statement: `bench/RESULTS_FUSE.md` (fifth measurement) and §8d.

If the `score()` call ratio is not dramatically lower, stop and fix the bounds
before optimizing anything else — a loose bound makes the entire design pointless
and no amount of SIMD recovers it. Record the negative result in
`bench/RESULTS_FUSE.md` either way; `pg_fts/bench/` and
`pg_turbovec/docs/PARITY_GAPS.md` are the house style for that, and the retracted
"we win 2.3×" claim in the latter is exactly the mistake to avoid.

### 8b. STATUS: MEASURED 2026-09-22 on three BEIR corpora, RE-MEASURED FOR THE SHIPPING SCORER THE SAME NIGHT. The gate is NOT met: **three rows fail**, and p99 moved from PASS to FAIL because of the normalizer that fixed nDCG@10 (§8d).

`bench/RESULTS_FUSE.md` has both runs. Summary, because a spec that states a gate should
state whether it was cleared:

| row | gate | scifact | nfcorpus | fiqa | |
|---|---|---|---|---|---|
| recall vs exhaustive | 1.000 | 1.000 | 1.000 | 1.000 | **PASS**, and for the **raw** objective only — the oracle cannot express the normalized one (`doc/GAPS.md` G46) |
| nDCG@10, normalizer **on** (the default since 2026-09-22) | ≥ RRF | 1.053× | 1.010× | 1.114× | **MET** |
| p99 latency, normalizer **on** | ≤ 0.70× | 0.710× | 0.612× | **1.000×** | **FAIL on two of three — measured 2026-09-22 (night), run `pgweave-20260922-224507`** |
| p50 latency, normalizer **on** | ≤ 0.50× | 0.710× | 0.827× | **1.172×** | **FAIL**, and on fiqa the fused arm is **slower than the RRF control it replaces** |
| ~~`score()` calls, normalizer **on**~~ | ≤ 0.20× | 0.571× | 0.875× | 0.513× | **FAIL — and SUPERSEDED 2026-09-22 by the restated per-channel row (§8, maintainer decision). Left visible: it is the unit every published figure for this row was measured in** |
| lexical work (BM25 contribs), normalizer **on** | ≤ 0.20× | 0.203× | 0.380× | **0.052×** | **MET on fiqa, missed on scifact (just over) and nfcorpus** |
| vector work (**code blocks read**), normalizer **on** | ≤ 0.20× | 1.000× | 1.000× | 1.000× | **FAIL — the same 1.000× the lane unit reported, so restating the unit changed nothing about the verdict** |
| pivots per query, normalizer **on** | *reported, not gated* | 5,183 | 3,627 | 57,572 | against corpora of 5,183 / 3,633 / 57,600 documents — **the fused scan is a full pass over the docid space** |
| ~~p99 latency, raw weighted sum~~ | ≤ 0.70× | 0.609× | 0.560× | 0.633× | **PASSED — left visible and dated (hard rule 13). SUPERSEDED 2026-09-22 (night): this is the `pg_weave.fuse_normalize = off` arm, and the re-run of the same statement one GUC away FAILS. The row moved because the change moved it, not because the number went stale** |
| ~~p50 latency, raw weighted sum~~ | ≤ 0.50× | 0.582× | 0.795× | 0.578× | **FAILED then too; superseded 2026-09-22 (night) by 0.710× / 0.827× / 1.172×** |
| ~~nDCG@10, raw weighted sum~~ | ≥ RRF | 0.982× | 0.924× | 0.687× | **FAILED — this is the `pg_weave.fuse_normalize = off` arm. SUPERSEDED 2026-09-22 by §8d, and left in the table because it is the baseline the fix is measured against** |
| ~~`score()` calls, raw weighted sum~~ | ≤ 0.20× | 0.648× | 0.903× | 0.541× | **FAILED; the shipping arm's figures are the row above** |

**So the gate is 2 of 5** — recall and nDCG@10 — and it was **2 of 5** before the normalizer
too: recall and p99. **The change traded p99 for nDCG@10.** Both halves of that trade are
stated here because either one alone misdescribes the product.

**THE nDCG ROW IS MET AS OF 2026-09-22, and it was met in the product rather than in a
study.** `bench/normprod.sh` scores three arms that are the *same statement* differing
only in `pg_weave.fuse_normalize`, plus the RRF control, all through `bench/ndcg.py` on
the same MiniLM BEIR corpora the EC2 run used: nDCG@10 **0.7212 / 0.3455 / 0.3878**
normalized, against 0.6846 / 0.3422 / 0.3482 for RRF and 0.6720 / 0.3161 / 0.2393 for the
raw sum. The harness reproduces *both* recorded EC2 arms to four decimals, which is the
only reason the comparison is admissible; the arms, the positive controls and one recorded
loss are in `doc/GAPS.md` G44, and the mechanism is §8d. **It is not a clean sweep.** On
nfcorpus the normalized arm wins the gated row by 1.0 % and *loses* recall@100 (0.3206 vs
0.3251) and MRR@10 (0.5441 vs 0.5514) to RRF. The row is met; the dataset is a draw.

**Both latency rows and the `score()` row were measured on the pre-normalizer build**,
where `pg_weave.fuse_normalize` did not exist. Normalization changes no mechanism either
row depends on — it divides weights by constants before the scan starts — but the vector
half of it adds one LUT build and one directory pass **per bolt per vector key** ahead of
the first bolt, and **that cost is unmeasured**. So p50 (0.582× / 0.795× / 0.578×), p99
(0.609× / 0.560× / 0.633×) and `score()` (0.648× / 0.903× / 0.541×) stand as numbers for
the raw arm and need an EC2 re-run before any of them is quoted for the shipping default.
The p99 **PASS** is in exactly that position too: a pass measured on a build that is no
longer the default is not a pass for the default.

**MEASURED 2026-09-22 (night) — the re-run happened, and the last sentence of that paragraph
turned out to be the important one. Two things in it were wrong.** Run
`pgweave-20260922-224507` (`c7i.8xlarge`, PG17, extension 0.19.0, commit b0bd1b7, the same
three corpora and embeddings, RRF control over the same index, 50 queries × 7 reps alternated
per query with an A/A leg):

  - **"Normalization changes no mechanism either row depends on" is FALSE.** It changes θ and
    the MaxScore partition, therefore the pivot count, therefore the latency: fiqa's p50 is
    **+104 %** with the normalizer on (10.889 → 22.163 ms), scifact **+19 %**, nfcorpus **+3 %**,
    measured as the same statement one GUC apart. The p99 row **fails** at 0.710× / 0.612× /
    1.000× where the raw arm passed at 0.609× / 0.560× / 0.633×, and at p50 fiqa's fused arm is
    **1.172× the control — slower than the thing it replaces**.
  - **The cost is not in the pre-scan pass**, which is where this paragraph put it. fiqa's
    pivot count rises **5.3×** (7,081,750 → 37,306,460) and `fuse_scores_total` **5.6×**
    (7,011,737 → 39,365,038) while the vector lane count moves **4.6 %** (35,670,912 →
    37,324,800) — the kernel barely notices, the pivot loop pays for everything. The
    mechanism is §8d's dense-channel ceiling property, one step further along: every document
    becomes a pivot, and a pivot costs one `seek()` plus one `block_max()` per contributing
    channel whether or not it ends in a `score()`.

The differences are admissible under hard rule 10: |fused − `fused_aa`| at p50 is
**0.014 / 0.003 / 0.035 ms** against between-arm deltas 30×–320× larger. Quality and
correctness reproduced the local measurement exactly (nDCG@10 0.7212 / 0.3455 / 0.3878;
299 of 300 queries compared, 0 mismatched, 1 skipped for a tied oracle), which is a
cross-harness control on both. Full numbers: `bench/RESULTS_FUSE.md`, fourth measurement.

**SUPERSEDED 2026-09-22 by §8d, and left in place because the fix was derived from it.**
Every sentence in the next paragraph still describes the `pg_weave.fuse_normalize = off`
arm exactly, and that arm is still selectable, which is why it is worded in the present
tense rather than corrected.

**The nDCG failure is this document's problem, not the scorer's** (`doc/GAPS.md` G44).
The fused scan returns its objective exactly — the recall row is 1.000 — but the
objective is a sum of **raw** channel scores, and BM25 (~10–20) against a quantized
inner product (~[−1,1]) is a 33× scale mismatch, so `weights => '{0.5,0.5}'` is
effectively lexical-only. It loses to a control that is scale-free because RRF ranks on
reciprocal rank. §2's threshold algebra is untouched by this; what is missing is
per-channel normalization *before* the sum, and (C2) survives any monotone positive
rescaling, so there is room to add it.

**The `score()`-call failure is the one this section warned about, and the measurement
that predicted it was already on disk.** Split by channel:

| dataset | lexical fused/RRF | vector fused/RRF | vector share of fused calls |
|---|---|---|---|
| scifact | 0.353× | 0.991× | 71 % |
| nfcorpus | 0.581× | 0.985× | 87 % |
| fiqa | **0.149×** | 0.956× | 86 % |

The lexical side clears the gate on fiqa **by itself**. The vector side prunes nothing —
~~`vec_blocks_bound_skipped = 0` on all three datasets~~ — and being 71–87 % of all calls it
sets the combined ratio no matter how well the lexical side does.
`bench/RESULTS_BOUND_PRUNING.md` measured that bound pruning 0.0 % long before this run,
and G43 recorded the generalization that *a bound computed but not acted on* is a latent
defect. This section's own instruction — "stop and fix the bounds before optimizing
anything else" — therefore applies to the **vector block bound**, and was answerable from
existing data. The measurement existed; the inference did not.

**CITATION CORRECTED 2026-09-22, and the verdict above is unchanged: the counter struck
out in that paragraph is a TAUTOLOGY in a fused scan.**
`vec_blocks_bound_skipped` increments only on `WEAVE_VSCAN_SKIP_BOUND`
(`src/vector/vecscan.c:290-292`), which fires only when the vector shuttle's own floor —
set by `weave_vec_shuttle_set_threshold()` (`src/vector/vecshuttle.c:1078`), whose sole
caller is the `weave_vec_scan()` SRF driver at `vecshuttle.c:1336` — sits above a block's
bound. A fused scan calls nothing of the sort, the field stays at its `-INFINITY` init
(`vecshuttle.c:887`), and `bound <= threshold` is false for every finite bound. **The zero
is structural and says nothing about bound quality.** What this section should have cited,
and what the verdict now rests on, is two things it already had:

  - `bench/RESULTS_CODE_SCAN.md:43-44` — **0.00–0.01 %** of blocks skipped on the
    single-channel path even at *oracle* theta, i.e. against the best any block ordering
    could achieve, confirmed at n = 200k (`:55`);
  - (B2) = `max‖recon‖ · ‖q‖` is **≈ 1.0 by construction** on L2-normalized data with
    `metric = 'ip'`, so the bound is the domain's own maximum and cannot get under a
    realized score.

The counter that *can* move inside a fused scan is the core's own `blkskip`
(`weave_fuse_stats()`; `src/am/fuse.c:618`, `ub <= st->theta`, incremented at `:644`) — but
it compares the **sum** of every contributing channel's weighted block bound (`ub += b` at
`:611`) against theta, so it proves that range skipping happens and **cannot attribute it to
the vector channel**. No counter in the tree can, today. `doc/GAPS.md` G44 has the audit,
the six documents that quoted the tautology, and the measured `blkskip` figures.

**So the honest statement of where the design stands:** the fused threshold demonstrably
suppresses lexical work (0.149× on the largest corpus) and is genuinely faster end to end
(p99 0.56–0.63×, validated against an A/A noise floor 170–714× smaller than the delta).
It is not yet *better*, and it will not be until the vector bound prunes and the sum is
normalized. Neither is a rewrite.

**HALF OF THAT HAPPENED THE SAME DAY.** The sum is normalized (§8d) and the fused
objective now beats RRF on nDCG@10 on 3 of 3 corpora, measured in the product. The vector
block bound still prunes nothing — ~~`vec_blocks_bound_skipped = 0`~~, **corrected above:
that counter cannot move in a fused scan; read `bench/RESULTS_CODE_SCAN.md:43-44` and the
(B2) ≈ 1.0 construction instead** — so the `score()` and p50 rows are exactly where this
paragraph left them, and the sentence above remains the honest statement with one of its
two conditions discharged.

**And "exactly where this paragraph left them" was itself wrong about one of the two:
normalization moved the work counters, in both directions.** Measured locally 2026-09-22
(`bench/normprod.sh`; `doc/GAPS.md` G44 has the table): the gated total improved on all
three corpora, the lexical side improved a lot (fiqa 0.149× → **0.052×**), and the vector
side went to **exactly 1.000×** while range skipping collapsed (fiqa `blkskip` 3,444,538 →
6,732; scifact 97,028 → 0). The `score()` row fails either way, but it fails for a
different reason with the normalizer on, and that reason is the next subsection.

### 8b-history. The gate went red on first contact with a real corpus, and that is what it was for.

`bench/fuse.sh` and `bench/prepdata.py` run end to end on BEIR scifact, and their first
real dataset found **`doc/GAPS.md` G43**: the fused path returned a plausible,
correctly-ordered top-10 that was wrong in six of ten rows. The cause was neither the
vector channel nor any bound — it was `wand_skip_blocks()` declaring a posting cursor
exhausted one block early, by reading the block header that follows a term's final block
as though it belonged to that term. Fixed; `sql/orderby.sql`'s last section is the
regression, with a positive control.

**Two things about the diagnosis belong in this document rather than only in GAPS.**

First, the defect was **unreachable before F2/F8 and is not in the fused core.** The
plain ranked path never reaches that header inference at all — instrumented, zero times
on the same corpus and queries — because it advances with `wand_next()` and only a
second channel driving the pivot produces a seek that jumps past a block boundary while
postings remain. The fused scan is therefore the first consumer of the posting cursor's
seek contract at full generality, and it should be assumed to be the first consumer of
every other channel's too. **A channel that passes its single-channel tests has been
tested against one caller.**

Second, the first three hypotheses were all wrong and all *explained the symptom*: a
too-low vector ceiling, OR-accumulation, and early termination each predicted "admits a
lower document, drops a higher one". What discriminated was not analysis but the
experiment that REMOVES a component — weighting the vector channel down to `1e-6` and
finding the wrong answer bit-for-bit unchanged. That took two minutes and should have
been first.

Correctness gate as of the fix: **25 of 25 judged scifact queries, 0 mismatches** against
the exhaustive per-channel oracle — and, on the EC2 run above, **299 of 299 comparable
queries across three corpora**. What the blocked period also produced is the two
instruments below, the loop-exit reporting described in §8c, and one correction to this
document's own method: the `fuse()` fallback is not an oracle at scale, for the reason
§7a (1) gives, so the gate compares against an exhaustive per-channel oracle built from
`weave_search()` and `weave_vec_scan()` instead.

**And the gate itself had to be fixed before it proved anything.** Its tie test demanded
that no two documents anywhere in the corpus share a score, which is false on any corpus
of real size — so on nfcorpus and fiqa it skipped **100 of 100** queries, the mismatch
count stayed 0, and it printed "gate passed" having compared nothing. Only a tie
*straddling rank 10* makes a top-10 set ambiguous. It now tests that, reports `compared`
next to `attempted`, and **dies** when `compared` is 0.

### 8d. The per-key ceiling normalizer as built (2026-09-22): what it is, why it is ONE constant for the whole query, and what it costs

Per `fuse()` **key**, every channel of that key carries effective weight `w_key / N_key`,
where `N_key` is the key's **pre-scan score ceiling**. Per key and not per channel, for
the reason `doc/GAPS.md` G44 records at length: a lexical key expands to one channel *per
query term*, so dividing each channel by its own ceiling would rescale the query's terms
against each other and partially undo idf weighting, which is the thing BM25 is for.
`w/N` is positive and finite, which is all `include/weave/fuse.h` note 3 requires of a
weight, so **(C2), the suffix sums and §5's MaxScore partition are untouched** — the
normalizer is arithmetic on the weights and nothing in §2's algebra can tell it happened.
One property improves: each key's weighted ceiling now equals *exactly* its weight, so
`suffix[0]` is the sum of the weights, and a `weights` array means relative influence for
the first time rather than "whatever scale this channel happens to emit".

**The lexical key's `N_key`** is the sum, over the key's terms, of
`weave_bm25_term_bound(idf_t, max over ALL SEGMENTS of that term's max tf)`, computed at
the same `(idf, k1 = 1.2, b = 0.75, avgdl)` the cursors will use — a different `avgdl` or
`k1` here would normalize by a constant no channel can reach. It is accumulated inside
the loop in `src/am/amscan.c` that already reads every segment's dictionary entry to
compute the global idf, so **it costs zero extra I/O**: the max-tf-over-segments is one
more accumulator over bytes already in hand. A term absent from the whole index
(`idf < 0`) contributes no channel to the scan, and contributes nothing to `N_key` either.

**The vector key's `N_key`** is the maximum over bolts of `weave_vec_weft_maxscore()`
(`src/vector/vecshuttle.c`, declared in `include/weave/vector.h`): bound (B2) folded over
one directory pass, then `weave_vec_scan_maxscore()` to reach the metric's domain. Those
are the same two functions `weave_vec_shuttle_begin()` calls, *called* rather than
reproduced, because a second copy of the domain conversion is precisely the failure
`include/weave/vecscan.h`'s domain rule exists to prevent. The skip conditions are the
bolt loop's, verbatim and in the same order, so a bolt this pass will not score cannot
set the scale for a key that never reads it, and a refusal returns false with a reason
rather than throwing — the scan is about to make the same refusal for the same bolt with a
message the user can connect to their query. Cost: **one LUT build and one directory pass
per bolt per vector key. THAT COST IS UNMEASURED.** The directory is one record per 32
lanes, so it is kilobytes against a code weft of megabytes, and the pass duplicates one
`begin()` runs anyway — but that is an argument that it is small, not a measurement that
it is, and §8b's p50/p99 rows therefore predate the change and need an EC2 re-run.

**MEASURED 2026-09-22 (night), and the paragraph above is right about the pass and wrong
about the total.** The re-run (`bench/RESULTS_FUSE.md`, fourth measurement; run
`pgweave-20260922-224507`) puts the normalizer's p50 cost at **+19 % scifact, +3 % nfcorpus,
+104 % fiqa** — fiqa's p50 doubled, 10.889 → 22.163 ms, same statement one GUC apart. The
pre-scan pass is not where that lives. **The pivot walk is.** fiqa's pivot count rises
**5.3×** (7,081,750 → 37,306,460) and `fuse_scores_total` **5.6×** (7,011,737 → 39,365,038)
while the vector channel's lane count moves **4.6 %** (35,670,912 → 37,324,800) and `blkskip`
collapses (3,444,538 → 6,732). One pivot costs one `seek()` plus one `block_max()` per
contributing channel; with every document a pivot — which is exactly what the ceiling property
below forces — the loop, not the kernel, is the bill. Two consequences for this section:
**the ceiling property is a LATENCY finding as well as a work finding**, and the smallest
vector candidate set is now the single blocker for **three** §8 rows (p50, p99, `score()`)
rather than one.

**THE CONSTRAINT THAT IS NOT OBVIOUS, and it is the reason the previous paragraph cannot
be simplified: the normalizer must be ONE CONSTANT FOR THE WHOLE QUERY, not one per
bolt.** `weave_fuse_pass()` runs one bounded top-k **per bolt** and then merges the
per-bolt lists **by score** — `src/am/amscan.c` sorts the accumulated rows and truncates
to the pass width, under a comment asserting that merging exact per-bolt top-k lists
yields an exact global top-k. That assertion holds only while every bolt scored against
the *same objective*. `WeaveShuttle.maxscore` is per bolt, so the obvious implementation —
read the ceiling off the shuttle you just opened — would rank each bolt against a
different objective and make the answer **a function of the segment layout**: it would
change after an INSERT, after VACUUM and after a merge, with no error anywhere and a
plausible top-k every time. That is why the maximum is taken **before the first bolt is
scanned**, and it is the entire reason `weave_vec_weft_maxscore()` exists instead of a
read of `sh->maxscore`; holding one shuttle open per bolt up front is what the per-bolt
scratch context exists to avoid, so hoisting shuttle creation is not an alternative.
Independently: the studies in `bench/RESULTS_FUSE.md` normalized in SQL over the whole
corpus, i.e. they measured a **query-global** normalizer, so a per-bolt implementation
would not even have been the thing that was measured.

**The guard.** A non-positive or non-finite `N_key` **leaves the weight alone** rather
than dividing. The two failure directions are bad in different ways: a zero weight makes
`weave_fuse_init()` refuse the scan outright, and an infinity silently zeroes the channel —
the worse of the two, because it returns an answer. Both are reachable: a key whose every
term is absent from the index has ceiling 0, and so does a vector key with no weft in any
bolt. In both cases the channel has nothing to contribute, so falling back to the raw
weight costs nothing and keeps the scan runnable.

**The knob.** `pg_weave.fuse_normalize`, `PGC_USERSET`, **default on**:
`DefineCustomBoolVariable()` in `src/am/customscan.c`, the variable in `src/am/am.c`, the
`extern` in `include/weave/weave.h`, and **outside any `#ifdef`** — deliberately, because
AGENTS.md's twelfth member is a GUC that was placed inside `#ifdef WEAVE_TEST_HOOKS` and
therefore existed in no build anyone runs, while `SHOW` cheerfully echoed a placeholder
back. `PGC_USERSET` because it changes a ranking and nothing on disk, which is
`doc/CONVENTIONS.md`'s reloption/GUC split. Off restores the raw weighted sum, and that
is not a curiosity: it is the arm every figure in `bench/RESULTS_FUSE.md` was measured on,
and an A/B that cannot reproduce its own baseline is not an A/B (hard rule 10). The GUC's
*existence* was verified the way the twelfth member says you must — `pg_settings` shows a
non-null `short_desc` in a session that has already touched a `weave` index — and not by
`SHOW`.

**Regression coverage** is `sql/fuse_degenerate.sql` section (6), which takes a fused
top-10 across a `weave_merge()` that collapses two bolts into one and asserts the id
**set** is unchanged, having first asserted `weave_index_nsegments() > 1` so the section
cannot be silently vacuous; and section (5a), which pins the raw sum under
`pg_weave.fuse_normalize = off` so both objectives are tested rather than one. Both have
positive controls, and three earlier fixtures for (6) **could not fail** — the fixture
design lesson, including why bolts that differ in max tf do not discriminate the two
implementations (BM25's tf saturation puts the term bound at tf=1 and tf=8 about 25 %
apart, not 8×), is in `doc/GAPS.md` G44 and in that section's own header comment.

**The measurement is in `doc/GAPS.md` G44 and is not duplicated here.** What belongs here
is its consequence for §8b: the nDCG@10 row is **MET** at 1.053× / 1.010× / 1.114× RRF,
with a recorded loss on nfcorpus, and the p50 and `score()`-call rows are **untouched** by
this work and still fail — the `score()` failure is the vector block bound pruning
nothing, and normalization does not go near it.

**AMENDED 2026-09-22 by the work counters, and the last clause of that paragraph is too
kind: normalization does not leave the `score()` row untouched, it moves both halves of it
in opposite directions.** Measured locally (`bench/normprod.sh`; the table is in
`doc/GAPS.md` G44, and a count of `score()` calls is deterministic, so no EC2 was needed):
the gated total improves on all three corpora (0.648×→0.571×, 0.903×→0.875×,
0.541×→0.513×) and the lexical side improves a lot (fiqa 0.149×→**0.052×**, one nineteenth
of the WAND control's BM25 contributions), while the **vector side becomes exactly 1.000×**
and range skipping collapses (fiqa `blkskip` 3,444,538→6,732, scifact 97,028→0). The gate
is ≤ 0.20× and is missed on all three; the vector channel is 74–87 % of the fused total, so
it is the only column that can carry the row.

**THE PROPERTY THAT EXPLAINS IT, and it is a general statement about this algorithm rather
than a fact about one corpus: A DENSE CHANNEL WHOSE WEIGHTED CEILING SITS ABOVE THETA
FORCES THE PIVOT TO VISIT EVERY DOCUMENT.** §5's partition can only move a channel to the
non-essential side once theta exceeds that channel's weighted ceiling, and §2's pivot is
the first document at which the essential channels' bounds can still reach theta. After
normalization each key's weighted ceiling **equals its weight** — that is the property
§8d's opening paragraph advertises as an improvement — so with `weights => '{0.5,0.5}'` the
vector key's ceiling is 0.5 and the whole partition's ceiling is 1.0, reached only by a
document that maxes both keys at once. Real documents score 0.11–0.35 of it (measured, one
scifact query: theta settles at 0.110627), so theta never climbs past 0.5, the dense
channel is never non-essential, every document is a pivot, and every pivot scores it.
Before normalization the same query had theta 2.28239 against a vector ceiling of 1.01069:
the vector channel went non-essential, 72 % of documents were never pivoted — and that
*was* the ranking defect G44 opened on. **The nDCG win and the work loss are one mechanism,
not two findings**, and any future channel with a bounded, dense score domain will do the
same thing.

**WHY TUNING CANNOT RESCUE THE ROW, measured rather than argued.** A seven-point lex:vec
weight sweep on all three corpora (`doc/GAPS.md` G44 for the table) leaves the vector
column at **1.000× at every one of the 21 points** (0.997–0.999× at three). Weighting the
vector key *down* is worse than useless: it starts range skipping dramatically (fiqa
`blkskip` 0 → 4,096,432), still does not reduce the vector channel's lane count, and costs
nDCG@10 on every corpus. The reason is the code layout, and this project already measured
it for another purpose (`bench/RESULTS_CODE_SCAN.md:330,417`): in `WEAVE_PACK_LANE`,
coordinate *j* of lane *s* is one nibble at byte `j*16 + s/2`
(`include/weave/vecpage.h:18`), so **reading one lane touches every byte of the block** —
scoring 1 lane costs the same memory traffic as scoring 32. A probe anywhere in a block
scores the whole block, a non-essential channel is still probed at every candidate, and
candidates are scattered across docid space.

**So §8's `score()` row is NOT REACHABLE for the vector channel by tuning the objective or
tightening the bound.** Three structural options, all three with their cost, and **none is
chosen here** — the choice is the maintainer's:

  - **(a) Restate the row in the unit the layout has.** The vector channel's work is
    blocks, or bytes of code read, not lanes scored; a per-lane gate on a layout whose
    quantum is 32 lanes is measuring something the design never offered. No code, and it is
    honest only if the restated gate is stated before it is measured against.
  - **(b) A second, vector-major copy of the codes**, so a single lane can be scored
    without touching its 31 neighbours. This forfeits the storage gate — a second copy of
    the code weft — and `include/weave/vecpage.h:24-26` refuses `WEAVE_PACK_VECMAJOR` on
    the coordinate-split page layout, so it is a new on-disk shape, not a reloption.
  - **(c) Cluster-order the weft** so a query's candidates are contiguous and a block probe
    is not wasted. This **CONTRADICTS the strictly-ascending-docid requirement the fused
    vector channel depends on**: `include/weave/vecdocmap.h:35` derives (C2) from
    `docid[]` being strictly ascending, `:105` states it as the adapter's invariant, `:122`
    is the (C1) lower-bound search that needs it, and `:167` is the `init()` refusal that
    enforces it. Reordering the weft for locality and keeping docid order are the same
    knob turned two ways. `doc/PHASES.md` **V13** (warp ordering by cluster, whose own
    justification is already recorded as gone) is exactly that ordering, so **V13 and F8
    are not independent** — a conflict nothing in the tree had recorded before 2026-09-22.

**AND AS OF 2026-09-22 (night) THIS IS NO LONGER ONLY THE `score()` ROW'S PROBLEM.** The EC2
re-run measured the same mechanism costing **latency**: p99 fails at 0.710× / 0.612× / 1.000×
(it passed at 0.609× / 0.560× / 0.633× on the raw sum) and fiqa's p50 is **1.172× the RRF
control**, i.e. slower than the arm the design proposes to replace. So options (a)–(c) above
are the three routes out of **three** failing rows, not one, and (a) — restating the unit —
cannot help p50 or p99 at all, because the clock does not care what unit the gate is written
in. **A maintainer decision this forces, presented and not taken:** `pg_weave.fuse_normalize`
is **`PGC_USERSET` and default on**; on, the ranking beats RRF on three corpora and the scan
is slower than RRF on the largest; off, the scan is fast and the ranking loses to RRF on all
three, which is the state that made `doc/ARCHITECTURE.md` §9 claim 2 unsupported to begin
with. It is the first knob in this project whose two settings each fail a **different** gate
row, and a user can already pick per statement. `bench/RESULTS_FUSE.md` (fourth measurement)
has the table the decision should be made from.

**DECIDED 2026-09-22 — MAINTAINER DECISION: `pg_weave.fuse_normalize` STAYS ON BY DEFAULT.**
The paragraph above stays as written because it is the table the decision was made from. The
reasoning, recorded so it can be argued with later:

  - **A user chooses a RANKING, not a scan strategy.** With the normalizer off the fused
    objective **loses to a plain RRF control on all three corpora measured** — 0.982× /
    0.924× / 0.687× — and a fused scan that ranks worse than the two-query control it
    replaces has no reason to exist. With it on the ranking **beats RRF everywhere
    measured**: 1.053× / 1.010× / 1.114×.
  - **The price is recorded and not hidden.** p99 went from **PASS** (0.609× / 0.560× /
    0.633×) to **FAIL** (0.710× / 0.612× / 1.000×), p50 from 0.582× / 0.795× / 0.578× to
    0.710× / 0.827× / 1.172×, and on fiqa the fused arm is **slower than the control**.
  - **This is a default, not a fork in the design**, because the GUC is `PGC_USERSET`: a
    deployment that wants the old trade can have it per query or per session.
  - **The latency regression is not accepted as permanent.** It is charged to the one open
    blocker — the **size of the vector channel's candidate set** — and not to the
    normalizer, which is arithmetic on the weights (see the ceiling property above).

**INCREMENTAL ABANDONMENT IS FIRING CONSTANTLY, NOT RARELY, AND IT CANNOT HELP THE VECTOR
CHANNEL FOR AN ARITHMETIC REASON (2026-09-22).** On fiqa the normalized arm records
**36,709,890 abandonments over 37,306,460 pivots — 0.98 per pivot**. That is not a
diagnostic that has never fired; it is the mechanism doing its job, and it is what produced
the lexical improvement (**2.8× fewer BM25 contributions**, 5,884,038 → 2,065,310). It does
nothing for the vector channel, and the reason is the **summation order**, not the
implementation: the fused core sorts its scored channels by **descending weighted ceiling**
(`src/am/fuse.c:163-165`) and sums in that order, abandoning on
`s + csuffix[j+1] <= theta` (`:716`), and after normalization the vector channel carries by far the largest
weight — **0.4947 against 0.0110** for each lexical channel — so it is summed **FIRST** and
its score is computed **before any abandonment test can run**.

**The obvious repair — reverse the order, put the expensive channel last — is DEAD ON
ARITHMETIC, not on effort, and it was checked so that nobody spends a week on it.** The test
that would have to fire is

```
s_lex + w_vec * block_max_vec <= theta
```

and on L2-normalized data with `metric = 'ip'` the vector block bound is ≈ 1.0, so
`w_vec * block_max_vec` is **≈ 0.49** while **theta is ≈ 0.11** (measured, one scifact
query: 0.110627). The left side can never fall below the right, so the test can never fire —
with `s_lex` at exactly 0, 0.49 > 0.11 already. It would remain dead at **k = 10** rather
than the ladder's k = 128, because a tighter k raises theta but nowhere near 0.49. Reversing
the order would cost the abandonment the *lexical* channels currently get and buy nothing.

**And it follows from the same arithmetic that no per-block vector bound can help either:**
on L2-normalized data (B1), (B2) and (B3) are all ≈ 1.0 unless a block happens to be
**coherent in direction**, so the bound carries no information to act on. Every route to a
smaller vector candidate set therefore runs through the three structural options (a)/(b)/(c)
above — restated unit, a vector-major second copy, or a cluster-ordered weft that conflicts
with F8's ascending-docid requirement. **The 2026-09-22 decision takes (a), and takes it as
a MEASUREMENT decision only: it restates §8's work row in blocks and explicitly does NOT
claim the row.** In blocks the vector ratio is 1.000×, exactly as it was in lanes.

### 8c. Why the loop stopped, reported per bolt

`weave_fuse_run()` has four exits and they were indistinguishable from outside the core:
the counters look identical whether the channels ran out of candidates or a ceiling
ended the scan early, and a wrong answer from a top-k loop is nearly always a loop that
ended before the document set did. G43 cost hours to exactly that ambiguity — the
returned rows were a correct top-k of a docid *prefix*, which is the signature of the
ceiling test firing on an under-reported `maxscore`, and the ceiling test had never
fired.

`WeaveFuseState.stop` now records one of `WEAVE_FUSE_STOP_EXHAUSTED`, `_CEILING`,
`_REQUIRED` or `_NOCAND`, set at every break, so zero after a run means a fifth exit
exists. `src/am/amscan.c` reports it per bolt under `pg_weave.fuse_check_bounds`
alongside `theta`, the total ceiling, `k`, `nheap` and the partition split, and then one
line per channel with its final position, seek count, score count and `maxscore`. That
per-channel line is what turned G43 from a mystery into a two-minute diagnosis:
`chan 0 kind=lexical cur=4294967295 nseek=129 nscore=128` against a term with 211
postings.

A NOTICE rather than a counter column, deliberately: it needs no SQL version bump, and
it arrives per bolt, which is the granularity a stop actually has.

### 8a. The instrument, added 2026-09-22, and the two things it already says

The `score()`-call row above was unmeasurable for as long as it has existed, and not
for the reason the roadmap said. The core has counted since F1 — `WeaveFuseChan`
carries `nseek` and `nscore`, `WeaveFuseState` carries `npivot`, `nblkskip`,
`nrqskip`, `nlivedrop`, `nveto` and `nabandon`, and `src/am/fuse.c` increments all of
them — but those structs are per-bolt scratch that dies with the scan's memory
context, so **no query could read a single one of them.** The gate was not blocked on
a benchmark harness; it was blocked on a counter with no way out. `weave_fuse_stats()`
and `weave_fuse_stats_reset()` (extension 0.18.0) are that way out, and
`include/weave/weave.h` states the contract, including the two ways `scores` can be
misread into a wrong ratio: the widening ladder re-runs the whole pass per rung, and
the pass runs once per bolt, so `passes` and `runs` are reported beside it.

**A row this table is missing, and the instrument added a counter for it:**
`block_max()` calls (`WeaveFuseChan.nbmax`). A change that halves `score()` calls by
asking `block_max()` twice as often has moved work, not removed it, and on the vector
channel `block_max()` reads the block's stored bound rather than returning a
constant. There is no honest RRF control for it — RRF computes no bounds — so it is
not a gated row here; it is a constraint on how the `score()` row may be read, and
`bench/RESULTS_FUSE.md` must print both or neither.

**What the property test already measured, for free and with no corpus.**
`test/hegel/test_fuse_props.c` P4 asserted `fused ≤ reference` and now reports the
ratio too, over 1,140,000 trials and 39,765,994 checks:

| quantity | fused | exhaustive reference | ratio |
|---|---|---|---|
| `score()` calls | 55,809,045 | 242,068,130 | **0.231** |
| `seek()` calls | 200,282,067 | 263,264,311 | 0.761 |
| `block_max()` calls | 95,158,668 | — (the reference computes no bounds) | — |

941,991 of 1,140,000 trials pruned at least one `score()` call, and the run now FAILS
if that count is ever zero — a scorer whose prunes remove no scoring work on any input
is this section's stop-everything signal, and finding that out should not require EC2.

**This is not the gate and must never be quoted as it.** The control is an exhaustive
scan of the same channels, not RRF over two indexes; the channels are synthetic; and
synthetic score distributions are exactly what makes a bound look tight. What the
0.231 establishes is narrower and still worth having: **the mechanism exists.** The
prunes remove roughly three quarters of the scoring work on random input, which is the
precondition for the real ratio being worth measuring. It also shows the cost side
plainly — the fused scan calls `block_max()` **1.7× more often than it calls
`score()`** — and that number has no counterpart in the table above, which is why the
paragraph on `nbmax` is there.

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
