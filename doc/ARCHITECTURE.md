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

   **Measured 2026-09-10, and it costs us part of that ambition**
   (`bench/RESULTS_IVF_RECALL.md`): quantized codes alone do not reach 0.99 at
   k=10. At full probe — zero probe-miss error, so this is the ceiling over every
   `nprobe` — recall@10 tops out at 0.9205 (GloVe-200d) and 0.8780 (GIST-960d) at
   4 bits. Reaching 0.99 requires a full-precision rerank pass, and a
   full-coverage float32 sidecar costs `4 * dim` bytes per vector, which is about
   what pgvector HNSW spends on the vector it stores. So the "10× storage win *at*
   0.99" formulation is not yet supported by anything we have measured, and the
   honest position until it is: **pg_weave can offer 0.92-ish recall at roughly
   0.12× the storage, or ~0.99 recall at roughly pgvector's storage, and picking
   between those is the user's knob, not a defect we are hiding.** Do not write
   "0.99 at 0.15×" in the README until a measurement says so.

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
3. Queries that get **faster** as predicates get more selective, because the
   predicate is pushed into graph traversal and into the SIMD block mask,
   instead of collapsing recall.
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
