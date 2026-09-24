# pg_weave architecture

## 1. The one-sentence thesis

Put every retrieval channel a search application needs — BM25 text, quantized
vector ANN, fuzzy, regex, prefix, and scalar facets — into **one index access
method, one segment, and one document-id space**, so that a query can skip work
in one channel using bounds derived from another.

The reason to fuse rather than ship three extensions is not code reuse. It is
that a **shared segment-local docid** makes cross-modal skipping possible.
Post-filtering an ANN search by a lexical or scalar predicate is the single
biggest pain point in real pgvector deployments; it becomes nearly free when the
vector codes and the postings are addressed by the same integer in the same
segment, under the same tombstone bitmap.

Everything else in this document follows from that.

## 2. Vocabulary

The project name is not decorative; the metaphor is the data model, and the code
uses it.

| Term | Means |
|---|---|
| **warp** | the shared, dense, segment-local docid axis (`0 .. nlive-1`). Every channel indexes into it. Like the warp threads on a loom, it is under tension and runs the whole length of the fabric. |
| **weft** | one retrieval channel woven across the warp: lexical postings, positions, vector codes, the vector graph, the vocabulary trigram map, the SuRF trie, docvalues. |
| **shuttle** | a cursor that carries one weft across the warp during a scan. Every channel exposes the same shuttle interface (§5) so the fused scorer can drive them uniformly. |
| **bolt** | a segment: an immutable, self-contained set of wefts over one warp, plus its own tombstone bitmap. Merging bolts is the only way the index reorganizes. |

A `weave` index is a size-tiered sequence of bolts. `nlive` per bolt is fixed at
build/flush time; deletion sets a bit in the bolt's livedocs bitmap and nothing
else. This is the Lucene/Tantivy shape, grafted onto PostgreSQL's buffer
manager with complete `GenericXLog` coverage.

## 3. Why one segment and not three indexes

Consider the query every hybrid-search application actually runs:

```sql
SELECT id FROM docs
 WHERE body @@@ 'postgres AND replication'      -- lexical predicate
   AND tenant_id = 42                           -- scalar predicate
 ORDER BY embedding <=> $1                      -- vector ranking
 LIMIT 10;
```

With three separate indexes there are exactly two plans and both are bad:

1. **Vector-first.** Traverse the HNSW graph for `k' >> k` candidates, then
   recheck the lexical and scalar predicates on the heap. If the predicates are
   selective, `k'` must be enormous, and recall collapses because the graph's
   greedy descent was never steered toward the surviving region. This is the
   documented failure mode of every `pgvector + filter` deployment.
2. **Predicate-first.** Build a bitmap of predicate-satisfying ctids, then
   compute exact distances for all of them. Correct, but linear in the predicate
   cardinality — seconds when the predicate matches a million rows.

With one segment there is a third plan, and it is the point of this project:

3. **Fused.** Evaluate the lexical and scalar predicates *within the bolt* into
   a warp-indexed bitmap. Hand that bitmap to the vector graph traversal as a
   visit filter and to the code-scan kernel as a 32-lane block-skip mask.
   Maintain one top-k heap with one threshold, and prune each channel against
   that threshold using per-channel upper bounds (§6). Selective predicates make
   the query *faster*, because they shrink the reachable warp before any
   distance is computed.

Plan 3 is impossible across index boundaries. There is no way to hand a
`pg_fts` docid set to a `pgvector` HNSW traversal: they do not share an id
space, a page, a lock, or a visibility rule. That is the whole argument.

## 4. Provenance

pg_weave is not a greenfield project. Three of its channels are existing,
production-tested code by the same author, relicensed and renamed. Being
explicit about this is a correctness requirement, not modesty — a reviewer needs
to know which lines have five years of field exposure and which were written
last week.

| Subsystem | Origin | Status |
|---|---|---|
| Segment engine, metapage, tiered merge, WAL, vacuum, MVCC, CIC | **pg_fts 1.5.8** (PostgreSQL license) | Forked wholesale. Field-tested. `ci/fork-rename.sh` is the exact transformation. |
| Lexical channel: FOR codec, dictionary + sparse block index, block-max WAND, BM25/BM25F, positions, phrase/NEAR | **pg_fts 1.5.8** | Forked wholesale. |
| Vocabulary trigram map, bounded Levenshtein automaton | **pg_fts 1.5.8** | Forked wholesale. The key asymptotic idea (§7). |
| SuRF trie, universal-Levenshtein neighbourhood expansion, regex AST + trigram tiling, LIKE translation, TRE matcher glue | **pg_tre 3.2.1** (MIT, relicensed) | Imported, renamed, **not yet wired**. See `doc/specs/IMPORT_pg_tre.md`. |
| Vector channel: rotation, Lloyd–Max codebook, per-vector renormalization scale, 32-lane packing, SIMD scan | **turbovec 1.0.0** (MIT, Rust) | To be **reimplemented in C**, not ported mechanically. See `doc/specs/VECTOR_CHANNEL.md`. |
| Vamana graph over quantized codes; filter pushed into traversal | **pg_turbovec 2.1.0** (Rust) and **zvec** (Apache-2.0, C++) | **Ideas only.** zvec is Alibaba's Apache-2.0 code: not one line is copied. See `doc/LICENSING.md`. |
| Fused-threshold top-k | **new** | The one genuinely novel piece. `doc/specs/FUSED_TOPK.md`. |

## 5. The channel contract

Every weft implements the same three-function shuttle interface. The fused
scorer knows nothing about BM25, quantizers, or tries; it only knows how to
advance a shuttle and ask it for a bound.

```c
typedef struct WeaveShuttle
{
    /* Advance to the first warp position >= target that this channel can
     * possibly contribute to.  Returns WEAVE_WARP_END when exhausted.  Must be
     * monotone: repeated calls never move backwards. */
    WeaveWarp   (*seek) (WeaveShuttle *s, WeaveWarp target);

    /* An upper bound on the score this channel can contribute for ANY warp
     * position in [s->cur, s->block_end].  Must be a true upper bound: the
     * fused scorer's correctness proof depends on it, and a bound that is ever
     * too low silently drops results.  Cheap: no page reads. */
    float       (*block_max) (WeaveShuttle *s);

    /* The exact contribution at s->cur.  May read pages. */
    float       (*score) (WeaveShuttle *s);
} WeaveShuttle;
```

The `block_max` contract is the load-bearing one. `doc/specs/FUSED_TOPK.md`
proves the pruning is lossless given it, and `test/hegel/test_bounds.c` is the
property test that every channel's bound dominates its own score on random
input. A channel whose bound is merely *usually* correct is a correctness bug,
not a tuning issue.

Channels, and what supplies their bound:

| Channel | Bound source |
|---|---|
| lexical (BM25) | per-block `max_tf` + `min_doclen` in `WeaveBlockHdr` — classic block-max WAND |
| vector (quantized) | per-32-lane-block max code value × stored per-vector scale |
| fuzzy / regex / prefix | boolean: bound is `+inf` if any candidate term in the block survives the funnel, else `-inf` |
| scalar / facet | boolean, from the docvalues bitmap |

Boolean channels contribute a gate rather than a score, which is why the
interface returns `float` and not a fused struct: a gate is just a bound of
`-inf`.

## 6. The headline algorithm

Every existing hybrid system over-fetches each branch to depth `k' >> k` and
combines with Reciprocal Rank Fusion. RRF is a rank-only heuristic: it discards
score magnitude, it has an unlearned constant (60, universally, because that is
what the 2009 paper used), and it forces the over-fetch.

pg_weave instead runs **one** document-at-a-time top-k with **one** threshold
across heterogeneous channels, pruning each channel against that threshold using
its own `block_max`. No over-fetch, no rank-only fusion, and a score that means
something. See `doc/specs/FUSED_TOPK.md` for the algorithm, the correctness
argument, and the degenerate cases (single channel reduces exactly to block-max
WAND; all-boolean reduces exactly to a bitmap AND).

RRF remains available as `weave_rrf()` for users who want it, and as the
baseline the fused scorer is benchmarked against for both quality (nDCG) and
latency.

## 7. The asymptotic idea worth more than the rest

pg_tre inverts trigrams over the **corpus**: one posting list per trigram,
listing documents. At one million rows of short text that is 3.8 GB and 83.5
million trigram emissions, against pg_trgm's 159 MB — and an exact-match query
that pg_trgm answers in 35 ms takes 18.5 s, because candidate extraction is
I/O-bound across thousands of scattered posting pages. `pg_tre/LIMITATIONS.md`
documents build-time temp disk as the wall that makes it unusable past 500k rows
of long text.

pg_fts inverts trigrams over the **vocabulary**: one posting list per trigram,
listing *dictionary term ordinals*. By Heaps' law the vocabulary grows as
roughly `n^β` with β ≈ 0.5, so this structure is asymptotically smaller and it
stops growing almost entirely once the corpus is large. The funnel becomes:

```
pattern -> candidate TERMS (trigram map + SuRF + Levenshtein automaton)
        -> candidate DOCS  (the lexical channel's own posting lists)
        -> heap recheck with the exact matcher
```

The second arrow reuses machinery that already exists and is already fast. This
is why pg_weave can offer pg_tre's unique capabilities — index-accelerated k≥1
edit distance, character-class regex, `<@>` distance ordering — at a fraction of
its size and build cost.

**The limitation, stated plainly:** vocabulary trigrams are token-aligned. They
cannot answer a pattern that crosses a token boundary, such as
`LIKE '%tion refu%'`. For that, character-stream trigrams over the corpus are
required, and pg_trgm's GIN is close to optimal for that specific job. pg_weave
therefore ships an **opt-in** corpus-level trigram channel (`cgram`) and does
not claim to beat GIN on size when it is enabled. See
`doc/specs/FUZZY_CHANNEL.md`.

## 8. What pg_weave will lose

A design document that only lists wins is marketing. Four of these are
fundamental and no amount of engineering removes them; they are knobs, not bugs.

1. **Exact recall × sublinear latency × minimal storage: pick two.**
   pg_turbovec measured 1.000 recall at 2552 ms; pgvector HNSW measured 0.96 at
   5.2 ms, on the same 1M × 1024-d corpus. Those are two points on a frontier.
   pg_weave aims to *dominate the frontier* — graph traversal over quantized
   codes should land ~0.99 at HNSW-like latency with turbovec's 10× storage win
   — but it cannot abolish the frontier. `vec_recall` is a per-query GUC and
   `recall=exact` will always cost a scan.

   **Measured 2026-09-10, refined through 2026-09-13, and it still costs us part
   of that ambition** — though less than the first reading of it suggested.

   Quantized codes alone do not reach 0.99 at k=10 at any width we support. At full
   probe — zero probe-miss error, so this is the ceiling over every `nprobe` —
   recall@10 is 0.9225 (GloVe-200d) and 0.8680 (GIST-960d) at 4 bits, and even at
   **8 bits** it is 0.9950 on GloVe but only **0.9860 on GIST**
   (`bench/RESULTS_BITWIDTH_SWEEP.md`). **A single code width cannot deliver 0.99
   recall. That is measured, not feared.**

   An earlier version of this paragraph added that 8 bits "costs 1,024 B/vector at
   1024-d — 0.18× HNSW — so the width that clears 0.99 on the easier corpus already
   misses the storage claim". **That half is withdrawn.** It divided by an estimated
   HNSW size of ~5,700 B/vector; measured, pgvector HNSW is **8,056 B/vector** at
   m=16, ef_construction=64 on 999,990 × 960-d (`bench/RESULTS_PHASE_V_COLD.md`), so
   8 bits is 0.127× and fits the budget comfortably. The single-width limit is a
   **recall** limit, not a storage one. Correcting it does not rescue the ambition,
   but a wrong reason for a right conclusion is still a wrong reason.

   What does work, and is now the ratified shape (`doc/PHASES.md`): **4-bit codes
   plus an exact float32 rerank of a top-25 window, read from the heap** — recall@10
   **0.9920** at n = 1M on GIST-960d, at **512 B/vector = 0.064× HNSW**. The rerank
   must be full precision, and a *stored* float32 sidecar costs `4 * dim` bytes per
   vector, half of what HNSW spends per vector, so it would forfeit the storage
   budget by itself. The heap already holds the vector; the index pays nothing.

   The heap-rerank latency this paragraph used to call unmeasured **is now
   measured**, and it is not the obstacle: a 20-candidate window is **86 ms p50
   cold** at n = 1M and **0.598 ms warm**, against 6.2 s cold for pgvector HNSW at
   ef = 400 and 3.548 ms warm at ef = 10. HNSW's traversal is dependent random I/O;
   a rerank window's TIDs are all known before the first fetch.

   **The code scan is now measured too** (`bench/RESULTS_CODE_SCAN.md`). The per-block
   bound that was supposed to prune it prunes **0.00%** on real corpora, so a query
   scores every code. What makes that affordable is not an algorithm but a **kernel**:
   the scan had been costing about one cycle per coordinate because it did one LUT
   gather per coordinate, and a nibble-LUT AVX2 kernel (task V16) scores a coordinate's
   32 lanes with one byte shuffle. At n = 1M the bare scan is **58.7 ms against 297.7 ms**
   — about **5×**, reproduced across two runs — for a recall cost measured at 0.0020 at
   n = 200k and 0.0000 at n = 1M.

   The prefix scan (task V15) adds to that by an amount that depends on the recall
   required — measured at 100 queries, not 10. At **recall ≥ 0.9760**, the comparator's
   ceiling and what the gate's matched-recall term uses, prefix 0.5 with a window of
   8,000 is **56.2 ms at recall 0.9880 — 0.76× pgvector HNSW**. At **recall ≥ 0.99** the
   cheapest prefix configuration is 75.6 ms against the full-dim pipeline's 80.2 ms,
   which is only **1.06×**. An earlier revision claimed "1.44× at no recall cost" from a
   10-query sample; that sample was optimistic by up to 7 points and the claim is
   withdrawn.

   Two corrections this forced, both worth keeping visible:

   - V15's verdict moved **three times in two days** — gate-passing lever, then demoted
     as useless, then re-promoted as the best configuration — and every move was
     downstream of one harness error: stage 2 was scoring a whole 32-lane block per
     survivor. The lesson is not about the prefix scan. It is that two of those three
     verdicts were published, and the thing that finally settled it was reading the code
     that produced the number rather than reasoning about the number.
   - The scan is no longer compute-bound. `lut-byte`'s cost per vector rises with n
     (36.7 → 58.6 → 60.7 ns) instead of staying flat, and 480 B in 60.7 ns is 7.91 GB/s
     against a **measured** 11.8 GB/s single-core wall, which reproduces across instances
     to under 1%. Roughly 1.4× remains to any kernel on this hardware, and it is bounded
     by memory rather than by effort.

   So the honest position is:

   - **recall and storage:** 0.9920 at 0.064× measured, one corpus, n = 1M. The
     earlier fallback framing — "~0.92 recall at ~0.12× storage, or ~1.00 recall at
     0.067× plus an untimed heap-fetch cost" — is superseded.
   - **latency:** the *scan* is measured and passes at iso-recall — 0.76× at the
     comparator's recall ceiling. A whole query is still not measured, because V7 and
     V8 do not exist, so no pg_weave vector query can be timed end to end. V16's
     kernel is now in `src/vector/kernels.c`; the prefix scan lives only in
     `bench/code_scan.c`.
   - **recall:** now evidenced at n = 1M with 100 queries (1,000 slots) rather than
     10 — 0.9930 at full dim and at prefix 0.5 with a 20,000 window. The earlier
     1.0000 figures were a 10-query artifact, optimistic by up to 7 points.

   So the storage and recall halves of "0.99 at 0.15×" are supported by measurement
   on one corpus, and the latency half is supported **for the scan in isolation**,
   at matched recall, on one corpus. **That still does not license a performance
   claim in the README**, for a specific reason rather than a cautious one: no
   pg_weave vector query exists, so nothing has been measured that includes page
   reads, visibility checks or tuple machinery. Do not write a latency or throughput
   comparison against pgvector until a real query has been timed, and do not write
   "0.99 at 0.15×" as a headline until a second corpus at a different dimensionality
   agrees.

   Note also what the comparison depends on: pgvector HNSW's recall ceiling of
   0.9760 at m = 16 sets the matched-recall point. A better-built graph would raise
   that ceiling **and** its own latency, moving both sides. Until that sweep is run,
   every "×HNSW" latency figure here is conditional on those build parameters.

2. **Unanchored cross-token substring search.** See §7. With `cgram` off we
   cannot answer it from the index; with `cgram` on we are not smaller than
   pg_trgm. Do not claim otherwise in the README.

3. **Positional data costs storage** roughly linearly in token count. Opt-in per
   field is the best available answer, and it is what we do.

4. **Operational simplicity.** pgvector is small, ubiquitous, and boring. A
   single extension with seven channel types is a harder thing to trust. There
   is no technical fix; the mitigations are a drop-in compatibility surface
   (`doc/MIGRATION.md`), per-channel opt-in so unused machinery is absent from
   the index rather than merely idle, and `weave_check()` returning something a
   DBA can act on.

5. **No parallel ranked scan.** Not merely unbuilt — measured and rejected. pg_fts
   built a complete parallel ranked CustomScan, verified it byte-exact, and reverted
   it: Amdahl p=0.88 caps an 8-worker best case at 8.3 ms against a competitor's
   2.12 ms, `nsegments=1` is enforced by insert-time tiered merge so per-segment
   parallelism divides by one, and workers refused to launch from inside
   `ExecCustomScan`. A competitor that *can* parallelize therefore keeps an
   advantage on scan-bound queries that pg_weave cannot answer with more CPUs.

Not fundamental, merely unbuilt, and tracked in `doc/PHASES.md`:
pg_fts's common-term ranked latency (decode-bound, wants impact-ordered postings);
pg_tre's build wall (dissolves under §7); the RRF over-fetch (dissolves under
§6).

## 9. What pg_weave can honestly claim

Four things, and it should claim exactly four things:

1. Lexical, vector, fuzzy, regex, and facet queries answered from **one** index
   with one WAL stream, one vacuum, and one visibility rule.
2. **Fused-threshold top-k** rather than over-fetch-plus-RRF: one threshold, no
   over-fetch, scores with meaning.

   **Measured 2026-09-13, and this claim is currently half-supported.** The
   mechanism needs a per-block upper bound on each channel's score. The *lexical*
   bound — block-max WAND over the `max_tf`/`min_doclen` already in
   `WeaveBlockHdr` — works and is separately validated. The *vector* bound does
   not: `bench/RESULTS_CODE_SCAN.md` measures it pruning **0.00%** of blocks on
   GIST-960d and **0.01%** on GloVe-200d, against 99.6% on the synthetic corpus
   it was originally measured on. For L2-normalized vectors the Cauchy–Schwarz
   term is ≈ 1.0 by construction while θ is always below 1.0, and the
   centroid+radius term is loose by a factor growing with √dim because it assumes
   a residual aligned with the query. **Do not make this claim about the vector
   channel until a candidate-reduction mechanism that measurably works is in
   place.** The claim as stated is safe for lexical, fuzzy and regex; it is not
   yet safe for vector, and saying so here is cheaper than being told.

   **CONFIRMED END TO END 2026-09-22, and a SECOND, INDEPENDENT deficiency found
   (`bench/RESULTS_FUSE.md`, `doc/GAPS.md` G44).** The first real-corpus run of
   §8 — three BEIR datasets, real embeddings, an RRF control over the same single
   index — reproduces the paragraph above exactly: the lexical side clears §8's
   `score()`-call gate unaided on the largest corpus (**0.149×** against a 0.20×
   bar), while the vector side is **0.956–0.991×** of the control. Since the vector channel is
   **71–87 %** of all fused `score()` calls, it sets the combined ratio no matter
   how well the lexical side prunes, and the gate fails at 0.54–0.90×.

   **CORRECTION 2026-09-22 (evening), left here rather than applied silently:** this
   paragraph read *"with `vec_blocks_bound_skipped = 0` on every dataset"*. That counter is
   **structurally zero in any fused scan** and witnesses nothing — it fires only on
   `WEAVE_VSCAN_SKIP_BOUND` (`src/vector/vecscan.c:291`), which needs the vector shuttle's
   own threshold floor, set only by `weave_vec_shuttle_set_threshold()` whose sole caller is
   the `weave_vec_scan()` SRF driver (`src/vector/vecshuttle.c:1336`); on the fused path the
   field keeps its `-INFINITY` init (`:887`). `src/vector/vecshuttle.c:609-614` and `:44-46`
   already said so and nobody propagated it. The measured 0.956–0.991× and the failing gate
   are unaffected; the bound's uselessness rests on 0.00–0.01 % of blocks pruned at an
   **oracle** θ (`bench/RESULTS_CODE_SCAN.md:43-44,55`) and B2 ≈ 1.0 by construction
   (`:72,87`). `blkskip` is the informative counter but is a **combined** bound over all
   contributing channels (`src/am/fuse.c:610,618,644`), so it cannot attribute a skip to the
   vector channel. `doc/GAPS.md` **G46**.

   *The uncomfortable part is that this was written here nine days earlier and the
   gate was still attempted as though it might pass.* The measurement existed, the
   inference existed, and it lived in the claims section instead of in the gate.
   **A constraint recorded next to a claim has to be propagated to the test that
   would otherwise contradict it**, or it is just a note that turns out to have
   been right.

   **The new finding is about the OBJECTIVE, and it is not about bounds at all.**
   nDCG@10 came in **below** RRF on all three datasets — 0.982×, 0.924×, and
   0.687× on fiqa — with recall@100 worse too. The fused scan is exact (§8's
   recall row is 1.000 on 299 of 299 comparable queries), so the ranking, not the
   scan, is what loses: `fuse()` sums **raw** channel scores, and BM25 (~10–20)
   against a quantized inner product (~[−1,1]) is a ~33× scale mismatch, so equal
   weights are effectively lexical-only while RRF is scale-free by construction.

   **So this claim is UNSUPPORTED, not retracted, and the distinction is the
   point.** Every mechanism it names works and is measured: one threshold, no
   over-fetch, an exact top-k, and a genuine latency win (p99 0.56–0.63× of RRF,
   against an A/A noise floor 170–714× smaller than the delta). But a user
   choosing between this and RRF is choosing a **ranking**, and today the ranking
   is worse. Being faster at computing a worse objective does not support "rather
   than over-fetch-plus-RRF". **Do not quote claim 2 as measured until per-channel
   score normalization lands and the nDCG row is at parity.** (C2) survives any
   monotone positive rescaling — `w·bound ≥ w·score` needs only `w > 0` — so there
   is room to fix it without touching §2's algebra.

   **And that room was measured the same day, offline, before any code: normalizing
   each key by its realized per-query maximum beats RRF on all three datasets**
   (1.049×, 1.006×, 1.021×) where the raw sum loses on all three (0.982×, 0.924×,
   0.687×). So the deficiency is the objective's *scaling* and nothing deeper, and
   this claim is recoverable rather than wrong. It is still UNSUPPORTED, because a
   single-pass threshold scan cannot know a realized maximum before it starts and the
   pre-scan substitute is not yet shown to hold that parity (`doc/GAPS.md` G44). The
   useful correction to this paragraph's own instinct: **the normalizer need not be a
   bound** — note 3 asks only for a positive finite weight — so it may be a
   statistical estimate rather than a ceiling, which is a larger design space than
   "make the bound tighter".

   **STATUS REVISED 2026-09-22 (later the same day; the UNSUPPORTED note above stays
   as history, dated 2026-09-22, because it was correct when written). The RANKING
   half of this claim is now SUPPORTED on three public datasets; the WORK-REDUCTION
   half is not, and that is the part to keep quiet about.** The pre-scan normalizer
   shipped — per `fuse()` **key**, each key divided by its own ceiling, query-global
   rather than per bolt, behind `pg_weave.fuse_normalize` (default on) — and was
   measured in the product rather than offline (`bench/normprod.sh`, three arms that
   are the same statement differing only in the GUC, plus the RRF control):
   nDCG@10 **0.7212 / 0.3455 / 0.3878** against RRF's 0.6846 / 0.3422 / 0.3482, i.e.
   **1.053× / 1.010× / 1.114×**, with recall@100 up as well (scifact 0.8892 → 0.9683,
   fiqa 0.5141 → 0.7079) so it is not a top-10 reshuffle. **The caveat travels with
   the claim: on nfcorpus the normalized arm loses recall@100 (0.3206 vs 0.3251) and
   MRR@10 (0.5441 vs 0.5514) to RRF** while winning nDCG@10 by 1.0 %. nfcorpus is a
   gate-row win, not a clean one.

   So what may now be said is exactly this: *the fused-threshold top-k is exact, and
   its objective ranks at least as well as RRF on three BEIR corpora with one corpus
   winning only the headline metric.* What may **not** be said is that it gets there
   by doing less work. §8's `score()`-call row still fails at 0.648× / 0.903× /
   0.541× against a 0.20× gate, because the vector block bound prunes nothing on this
   data and the vector channel sets
   **71–87 %** of all fused `score()` calls; p50 is 0.582× / 0.795× / 0.578× against
   ≤ 0.50×.    **And the latency figures that used to back the "no over-fetch is also
   cheaper" reading are STALE**: they were taken before the normalizer, which adds an
   unmeasured pre-scan pass, so an EC2 re-run is owed before any p50 or p99 is quoted
   for the shipping scorer (`bench/RESULTS_FUSE.md`, marked in place). `doc/GAPS.md`
   **G44**, `doc/specs/FUSED_TOPK.md` **sect. 8d**.
   **[RE-RUN DONE 2026-09-22 (night) — see the dated block below: the figures were not
   merely stale, the shipping scorer's p99 FAILS and fiqa is slower than the control, and
   the "unmeasured pre-scan pass" was the wrong suspect.]**

   **THE WORK-REDUCTION HALF IS STILL UNSUPPORTED, AND AS OF 2026-09-22 (evening) THE
   REASON HAS CHANGED. The notes above stay visible as history; they were correct about
   the verdict and wrong about the cause.** Two corrections, in order of how much they
   move:

   1. **The counter this claim kept citing says nothing.** The sentence above read
      *"(`vec_blocks_bound_skipped = 0` on all three datasets)"*; that counter is
      **structurally zero in any fused scan**, because the fused driver never sets the
      vector shuttle's threshold floor (`weave_vec_shuttle_set_threshold()`'s sole caller
      is the `weave_vec_scan()` SRF, `src/vector/vecshuttle.c:1336`; the field keeps its
      `-INFINITY` init at `:887`). This is a correction of **reasoning, not of result** —
      the ratios are direct measurements, and the bound's failure to prune is supported by
      0.00–0.01 % pruned at an **oracle** θ plus B2 ≈ 1.0 by construction
      (`bench/RESULTS_CODE_SCAN.md`).
   2. **The cause is not "the bound does not prune". It is that A DENSE CHANNEL'S WEIGHTED
      CEILING SITS ABOVE θ, AND THE PACK LAYOUT MAKES A ONE-LANE PROBE COST A WHOLE
      BLOCK.** Observed, not inferred (per-bolt diagnostic `NOTICE`, one scifact query):
      with the shipping normalizer each lexical channel carries w = 0.0110 against the
      vector channel's w = 0.4947, θ = 0.1106, the partition ceiling is exactly 1.0, and
      the scan visits **all 5,183** documents; with the normalizer off, w = 0.5 everywhere,
      θ = 2.2824, ceiling 23.335, and it visits 1,450 (28 %). θ cannot climb to the vector
      channel's 0.49 because a real document scores ~0.11–0.35 of the 1.0 that would need
      both channels maxed at once. **The ranking win and the work loss are the same
      phenomenon.** And tuning cannot separate them: over seven lexical:vector ratios from
      0.0625 to 4 the vector column is 1.000× at every point while nDCG falls away from its
      optimum; a 16× vector de-weighting starts the core's range skipping dramatically
      (fiqa `blkskip` 0 → 4,096,432) and still does not cut the vector channel's lane count,
      because in `WEAVE_PACK_LANE` reading one lane touches every byte of its block —
      scoring 1 lane costs the same memory traffic as scoring 32
      (`bench/RESULTS_CODE_SCAN.md:330,417`).

   **What follows for this claim.** "Fused-threshold top-k rather than
   over-fetch-plus-RRF" may be claimed on **ranking** and on **exactness**, and must not be
   claimed on **work saved for the vector channel** — not pending a tighter bound, which is
   what the earlier notes implied, but pending a maintainer decision among: restating §8's
   row in **blocks or bytes** rather than lanes; a second **vector-major** copy of the codes,
   forfeiting the storage gate; or a **cluster-ordered weft**, which contradicts the
   strictly-ascending-docid requirement the fused vector channel depends on
   (`include/weave/vecdocmap.h:35,105,122`) — i.e. **claim 3's clustering lever and this
   claim's docid adapter are not independent**, which nothing in the tree had recorded before
   today. Full numbers in `bench/RESULTS_FUSE.md` (third measurement); `doc/GAPS.md` **G46**,
   `doc/specs/FUSED_TOPK.md` **sect. 8d**.

   **AND THE LATENCY BACKING IS NOW MEASURED, AND IT IS NEGATIVE ON THE LARGEST CORPUS
   (2026-09-22, night; run `pgweave-20260922-224507`, `bench/RESULTS_FUSE.md` fourth
   measurement). Everything above stays as history.** The EC2 re-run the "STALE" note in the
   previous block asked for has happened, for the shipping `pg_weave.fuse_normalize = on`
   scorer, with an RRF control over the same index and an A/A leg:

   - **p99 is 0.710× / 0.612× / 1.000× against a ≤ 0.70× gate — FAIL on two of three, where
     the raw sum PASSED at 0.609× / 0.560× / 0.633×.** The two arms are the same statement one
     GUC apart, so this is a regression caused by the normalizer that fixed the ranking half of
     this claim, not a number that went stale.
   - **p50 is 0.710× / 0.827× / 1.172× against ≤ 0.50×, and on fiqa the fused arm is SLOWER
     than the RRF control it is offered as a replacement for.** The normalizer's own p50 cost
     is **+19 % / +3 % / +104 %**; fiqa's doubled.
   - The cause is the **pivot walk**, not the pre-scan pass the earlier notes guessed: fiqa's
     pivots rise 5.3× (7,081,750 → 37,306,460) and `fuse_scores_total` 5.6× while the vector
     channel's lane count moves 4.6 %. Same ceiling mechanism as the work half, one step on.
   - Hard rule 10 is satisfied: |fused − fused_aa| p50 = 0.014 / 0.003 / 0.035 ms against
     between-arm deltas 30×–320× larger.

   **So the sentence this claim is allowed to make gets narrower again, and the narrowing is
   the point: "no over-fetch" may be claimed, "better ranking than RRF" may be claimed on three
   BEIR corpora with the nfcorpus caveat, and "NO OVER-FETCH IS ALSO CHEAPER" MAY NOT BE STATED
   AT ALL** — not as measured, not as expected, not hedged. It is measured, and on the largest
   corpus in the set it is false. The §8 gate is **2 of 5** (recall, nDCG) and was **2 of 5**
   before the normalizer (recall, p99): **the change traded p99 for nDCG.** ~~A maintainer
   decision is open on whether~~ **[DECIDED 2026-09-22: it stays on — see the dated note
   below.]** `pg_weave.fuse_normalize` should stay on by default — on, the
   ranking wins and the scan is slower than RRF on fiqa; off, the scan is fast and the ranking
   loses on all three, which is the state that made this claim unsupported in the first place —
   and the third route, a smaller vector candidate set, is now the **single blocker for three of
   the five rows**. The GUC is `PGC_USERSET`, so a user can already choose per query.
   `doc/GAPS.md` **G44** and **G46**, `doc/specs/FUSED_TOPK.md` **sect. 8b** and **8d**.

   **DECIDED 2026-09-22, AND THIS CLAIM GAINS NOTHING FROM IT — the note is here so nobody
   reads a settled decision as a new claim.** The maintainer decided that
   `pg_weave.fuse_normalize` **stays on by default**, with the price named rather than
   absorbed: the ranking half of this claim is bought at **p99 FAIL** (0.710× / 0.612× /
   1.000×, where the raw sum passed at 0.609× / 0.560× / 0.633×) and **p50 0.710× / 0.827× /
   1.172×**, fiqa slower than the control. The reason recorded with the decision is that a
   user chooses a **ranking**, not a scan strategy, and with the normalizer off this claim's
   ranking half is simply false (0.982× / 0.924× / 0.687×); the GUC is `PGC_USERSET`, so the
   old trade is available per query. **What does not change is the work-reduction half: it
   remains UNSUPPORTED in the restated units too.** §8's work row was restated the same day
   into per-channel units — lexical BM25 contributions, **vector code blocks read**, and
   pivots per query reported but not gated — and **the vector ratio is 1.000× in blocks
   exactly as it was in lanes**, with the normalized arm pivoting once per document in the
   corpus (5,183 / 3,627 / 57,572 against 5,183 / 3,633 / 57,600). The lexical half is met on
   fiqa alone (0.052×). So "no over-fetch is also cheaper" still **may not be stated**, and
   the sentence this claim is allowed to make is the one above, unchanged. `doc/GAPS.md`
   **G44**, `doc/specs/FUSED_TOPK.md` **sect. 8** and **8d**, `bench/RESULTS_FUSE.md` (fifth
   measurement).
3. Queries that get **faster** as predicates get more selective, because the
   predicate is pushed into the SIMD block mask instead of collapsing recall.

   **MEASURED IN FULL 2026-09-23 — work, pages AND latency — AND THE CLAIM IS
   SUPPORTED, with one scope correction that is part of the claim now**
   (`bench/RESULTS_GATE_SWEEP.md`). Three passes, in this order, because each one
   corrected the reading of the one before it:

   - **The work the CPU does: selectivity, to three digits.** Pivots and vector
     `score()` calls are 1.000× / 0.101× / 0.010× / 0.001× at 100 % / 10 % / 1 % /
     0.1 % on scifact, nfcorpus and fiqa, and scored code blocks follow
     `1 − (1 − s)^32` — predicted first, then matched at nine of nine points.
   - **The pages a query touches: was flat, now falls.** The first pass measured
     502 / 515 / 502 / 424 buffers across the four selectivities — **flat** — because
     the code cursor walked the whole `WEAVE_PK_VCODES` chain and nothing recorded
     where a block's strips were. `doc/GAPS.md` G27 fixed that by storing
     `firstpage` in the directory record (commit `33dcc1b`, 288 B per record, zero
     extra pages): at 0.1 % selectivity scifact went 1045 → 513 buffers (**2.04×**)
     and fiqa 8727 → 1823 (**4.79×**), with all twelve work-counter rows
     bit-identical. **The flat numbers above are SUPERSEDED, not deleted** — they are
     what the design did before the pointer existed, and they are why it exists.
   - **Latency, on a quiet machine: 5.7× to 13.8× faster at 0.1 % selectivity**,
     p50 *and* p99, nine of nine gated points falling, every step clearing its own
     A/A noise floor by at least 21× (EC2 `c7i.8xlarge`; scifact 0.141×, nfcorpus
     0.175×, fiqa 0.072× against their unfiltered selves).

   - **The mechanism, not just the monotonicity.** Against a control that is *our own
     index* answering the same query by the over-fetch-and-recheck route (same
     `Index Cond:`, same corpus, so no engine or plan-shape confound), the fused scan is
     **97–158× fewer buffers at 0.1 % selectivity** and discards **zero** candidates
     where the control discards up to 227,054 to return 100 rows. `Rows Removed by
     Index Recheck` is 0 at every point on every corpus. That is the mask doing what
     this claim says it does.

   **AND THE LOSS, which belongs in the claim: with no predicate the fused path is
   0.7–0.8×, i.e. ~30 % MORE expensive**, because it scans the lexical channel with no
   gate to pay for it. The crossover is between 10 % and 1 % selectivity on all three
   corpora. So the claim is not "fused is faster"; it is **"fused converts predicate
   selectivity into speed, and costs about 30 % when there is none to convert"**.

   **Quote 5.7–13.8×, never 1,000×.** The pivot count falls as selectivity exactly,
   and reading that column alone over-promises by two orders of magnitude. What the
   clock tracks is the **blocks-scored** curve — latency is within 1.2–1.4× of it at
   the 1 % point on all three corpora, and 24–37× away from pivots. That the
   design's own unit of vector work is what predicts the user-visible number is the
   strongest form this claim has; it is also the discipline the claim has to keep.

   **THE SCOPE CORRECTION, which belongs in the claim and not in a footnote:** the
   only predicate that can be pushed into the index today is another **lexical
   term**, because `WEAVE_CH_DOCVALS`'s page kind is still reserved. So "as
   predicates get more selective" means "as a second `@@@` term gets rarer", not yet
   `WHERE category = 'x'`. Both honest phrasings — "the scan scores fewer documents"
   and "the query reads less and returns sooner" — are now measured; the word
   *predicates* is the part still writing a cheque the code does not cash.
   (The "graph traversal" half of this sentence is stale — the Vamana plan was
   withdrawn in V9's history. The mask is the live mechanism, and it is
   *predicate*-driven, which is why the failure of the *score*-driven block bound
   in claim 2 does not touch this claim: `weave_score_block()` skips a masked
   lane without reading a code byte.)
4. C, PostgreSQL-licensed, MVCC- and WAL-native, `trusted`, no external engine —
   therefore on the contrib track, which an AGPL extension embedding a
   non-PostgreSQL storage engine structurally cannot be.

Claiming a fifth is the thing that gets the other four dismissed.

## 10. Repository map

```
include/weave/           private headers, one per subsystem
  weave.h                SQL types wdoc/wquery, shared macros
  am.h                   metapage, bolt/segment, page layout, dict, postings
  for.h                  frame-of-reference codec (backend-independent)
  channel.h              the shuttle contract (§5)
  fuse.h                 fused-threshold top-k
  vector.h  quantize.h  graph.h      vector channel
  surf.h  uleven.h  regex_ast.h  tiling.h  like_translate.h   fuzzy channel
  sparsemap.h  docvalid.h  utf8.h  popcount.h  hash.h
src/am/                  access-method entry points, scan, custom scan
src/pages/               page-level readers/writers per channel
src/postings/            posting encode/decode
src/query/               parse, analyze, rank, match, and the fuzzy funnel
src/vector/              quantizer, packing, SIMD kernels, graph
src/util/                sparsemap, utf8, migration
sql/                     install script + regression inputs
expected/                regression expected output
t/                       TAP: crash, replication, corruption, encodings, ...
test/isolation/          isolation specs
test/hegel/              property-based tests of backend-independent cores
test/fuzz/               libFuzzer harnesses
bench/                   reproducible benchmarks and their recorded results
doc/                     this directory
doc/specs/               per-subsystem specifications; the build-out contract
```

`src/am/` is four translation units -- `am.c` (AM core, page/segment/metapage
machinery), `ambuild.c` (build, insert, segment writers, merge), `amvacuum.c`
(bulkdelete, cleanup, compaction) and `amscan.c` (scan) -- with the interface
between them declared, and justified declaration by declaration, in
`include/weave/am.h`. It was a single 7,300-line unity build inherited from
pg_fts, which `#include`d `amscan.c`, `../query/lev.c` and `../pages/trgm_page.c`
as text; task **L1** split it, deleted the `make check-unity` guard that existed
to stop anyone "fixing" it casually, and proved the change non-semantic by
comparing the LTO'd shared object function by function.

## 11. Where to start reading

- Building the thing: `doc/PHASES.md`, then the relevant `doc/specs/*.md`.
- Understanding the format: `doc/specs/SEGMENT_FORMAT.md`.
- Understanding the novel part: `doc/specs/FUSED_TOPK.md`.
- Writing code: `doc/CONVENTIONS.md`. It is not optional; this project matches
  PostgreSQL core style, including the parts you disagree with.
- Testing: `doc/TESTING.md`. A channel is not done until its bound has a
  property test and its codec has a fuzz target.
