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
| channel score() calls | RRF `k'=100` | ≤ 0.2× RRF (this is the mechanism; if it is not much lower, the bounds are too loose and §2 is wrong) |
| recall vs exhaustive fused scan | — | ~~≥ 0.99 with graph on;~~ **1.000**, and it is 1.000 *by construction* now that the graph channel is withdrawn (§6) — every implemented channel is exact, so this row tests the scorer's pruning, not an approximation. A single miss is a (C2) violation, which makes it the most valuable row in the table rather than the weakest |

If the `score()` call ratio is not dramatically lower, stop and fix the bounds
before optimizing anything else — a loose bound makes the entire design pointless
and no amount of SIMD recovers it. Record the negative result in
`bench/RESULTS_FUSE.md` either way; `pg_fts/bench/` and
`pg_turbovec/docs/PARITY_GAPS.md` are the house style for that, and the retracted
"we win 2.3×" claim in the latter is exactly the mistake to avoid.

### 8b. STATUS: this section is BLOCKED, and by a wrong answer rather than a harness

`bench/fuse.sh` and `bench/prepdata.py` exist, run end to end on BEIR scifact, and
their first real dataset found **`doc/GAPS.md` G43**: the fused path returns a
different top-10 than two non-fused vector paths that agree with each other, admitting
a lower-scoring document and dropping a higher one. Block pruning is provably not
involved (`blkskip = 0`, every document a pivot); the leading hypothesis is that the
vector channel's ceiling is not a true upper bound, made reachable by F8 because the
fused scorer's abandonment prune *acts* on a bound the single-channel path computes and
ignores.

So no row of the table above has been measured on a real corpus, deliberately: hard
rule 8 says verify correctness before recording a latency. The harness's correctness
gate is red and that is the harness working. What the attempt did produce, beyond G43,
is the two instruments below and one correction to this document's own method — the
`fuse()` fallback is not an oracle at scale, for the reason §7a (1) gives, so the gate
compares against an exhaustive per-channel oracle built from `weave_search()` and
`weave_vec_scan()` instead.

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
