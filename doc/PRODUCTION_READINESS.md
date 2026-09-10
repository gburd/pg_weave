# Production readiness

**Short answer: no. Do not put pg_weave in production. Do not put it in staging.**

As of 0.1.0 (2026-09-06) this is a repository with a working forked lexical
engine, a tested quantizer, a design corpus, and two of its four advertised
capabilities entirely unimplemented. The headline feature — fused-threshold
top-k, the thing that justifies the project existing — does not exist as code.

This document is the honest gate list. It is deliberately harsh, because the
failure mode for a project like this is a README that reads like a product and a
codebase that is a prototype.

## What actually works today

pg_weave's product definition is **one index replacing the combination of a BM25
index and a vector-similarity index**. Read the table with that in mind: the left
half of the product is real and competitive, the right half does not reach disk yet.

| capability | state | evidence |
|---|---|---|
| BM25 lexical search, boolean, phrase, NEAR, prefix | **works** | forked from pg_fts 1.5.8, current with upstream 1.6.0; 6 regression + 2 isolation + 105 TAP green on PG 17 and 18 |
| Index-native `count(*)` | **works** | inherited; measured ~200× faster than tsvector+GIN at 2 M docs (`bench/RESULTS_LEXICAL.md`) |
| Ranked top-k latency | **works, competitive at k=100** | after L14/L15/L17: rare k=10 1.51 ms (1.20× pg_textsearch), k=100 **wins** rare 3.96× and mid 1.27×; common k=10 still 4.75× behind (task L2). `bench/RESULTS_L17.md` |
| Build time | **at GIN parity** | 192.5 s vs GIN 202.7 s, pg_textsearch 49.2 s (3.91× behind). `bench/RESULTS_L15.md` |
| Index size | **best of the three** | 626 MB vs pg_textsearch 873 MB, tsvector+GIN 1120 MB |
| Crash recovery, replication, MVCC, CIC/REINDEX | **works** | inherited; `t/001`–`t/009` |
| Vacuum, tombstones, tiered merge | **works** | inherited; `t/008` reclaims 4688 → 2199 pages |
| Multi-channel on-disk substrate: per-bolt channel descriptors, extended page-kind space, versioned metapage reader | **works** | phase X (format v6); `t/010` upgrades a 20,000-row v5 index and proves byte-identical answers; `t/011` proves a corrupted descriptor `ERROR`s cleanly; 852,070 exhaustive kind-space checks |
| `weave_check()` invariant verification | **partial** | 11 invariants, incl. chandesc reachability and page-kind classification. Before phase X only `weave_check_meta()` existed. ~14 of `SEGMENT_FORMAT.md` §9 still owed |
| Vector quantizer: rotation, codebook, encode/decode, packing | **works** | 17,741 + 1,909,440 property checks, 0 failures (tasks V2–V5) |
| Vector block scoring kernels | **partial** | scalar oracle + `lut-wide` + `lut-avx2`, verified `memcmp`-identical over 308,278 differential checks; dispatch resolved once at `_PG_init`. AVX-512, NEON and the approximate families are unmet and tabulated as unmet (`VECTOR_CHANNEL.md` §8) |
| `wvec` type: I/O, typmod, casts, 4 distance operators, arithmetic, btree | **works** | task V1; `sql/wvec.sql`, green on PG 17 and 18 |
| Quantizer reachable from SQL (`weave_quantize_roundtrip`) | **works** | lets reconstruction error be measured on a real corpus before the index exists |
| Vector *indexing* (the AM accepting a `wvec` column) | **does not exist** | tasks V7–V9. **This is the gap between pg_weave and its own product definition** |
| Vector storage pages, IVF, ANN | **does not exist** | tasks V7, V9, V13, V14 |
| Fused-threshold top-k | **does not exist** | specified only |
| pgvector / tsvector compatibility | **does not exist** | specified only; M1–M3, and 1.0 requirements rather than polish |
| Fuzzy / regex / prefix channel | **compiles, unreachable — and deliberately post-1.0** | Z1/Z2 done (TRE vendored at `f864ed0` and current, GUCs wired); no routing (Z3–Z7). Removed from the 1.0 path 2026-09-10: it is a third channel on a two-channel product |

So: pg_weave today is *a measurably improved pg_fts, plus a tested quantizer and
verified scoring kernels that no on-disk format calls yet.* The L-phase work (L7,
L8, L10, L12, L14, L15, L17) has made the **lexical channel measurably better than
what it forked from** on size, build time, `count(*)`, keyless `ORDER BY`, and
deep-page ranked latency, all recorded in `bench/`. Phase X made the substrate
multi-channel. But the sentence that matters is this one: **the vector channel
still cannot index a column**, so pg_weave is not yet a replacement for the pair it
intends to replace, and anyone needing both capabilities today needs both
extensions.

If you want the lexical capability alone in production today, **pg_fts is still the
safer choice** — longer track record, real release history, and pg_weave's
advantages are measured on one synthetic corpus.

## The gate list

Production readiness is not a feeling. These are the conditions, each
mechanically checkable. `doc/PHASES.md` has the task-level detail.

### Blocking — cannot be called usable without these

1. **A second channel must exist and work.** One channel is pg_fts. Two channels
   is the product. Phase V gates V1–V9.
2. **The fused scorer must exist and pass its correctness gate** — property test
   F5, 10⁶ generated cases, fused top-k identical to brute force. Until then the
   central claim is a document.
3. **Every channel must have a bound property test.** A too-low `block_max()`
   silently drops rows and no regression test catches it
   (`doc/TESTING.md`). Non-negotiable.
4. **`weave_check()` must verify every invariant** in
   `doc/specs/SEGMENT_FORMAT.md` §9, and there are 20-odd. **Corrected
   2026-09-10:** the earlier claim that it "covers the inherited lexical ones
   only" was wrong -- there was no `weave_check()` at all, only
   `weave_check_meta()` validating the metapage magic and version. Task X3 built
   the function and eleven invariants: the metapage version gate, `nsegments`
   bound, per-bolt chain kinds, `page_kinds_decodable`, uninitialized-page count,
   the four channel-descriptor invariants, and (behind `deep`)
   `pages_reachable_or_freed` and `chains_do_not_overlap`. The lexical ones -- the
   dictionary ordering, the block-max bound recompute that is a live contract-(C2)
   check, the livedocs popcount, the trigram ordinals, the doclen sidecar
   coverage -- are still owed, and are task M6.
5. ~~**Format v6 must resolve the page-kind bit exhaustion.**~~ **DONE 2026-09-10
   (0.6.0), tasks X1-X4.** `flags` bit 15 is now a reserved escape selecting an
   extended integer kind space held in the second page-opaque word; the ten shipped
   kinds keep their one-hot bits, so a v6-written lexical page is byte-identical to
   a v5-written one, no page is rewritten, and the vector and fuzzy channels have
   ten reserved ids between them (`WEAVE_PK_VMETA`..`WEAVE_PK_CGRAM`,
   `include/weave/pagekind.h`) instead of the ten bit positions that did not
   fit. Chosen
   over widening `flags` to `uint32`, which moves the opaque area on every page of
   every existing index. It follows L17's pattern -- a per-*object* self-describing
   discriminator in spare bits of an existing field, which is what let one relation
   hold both doclen-sidecar encodings across an upgrade instead of needing a
   per-index version that cannot describe a mixed index. The hard guarantee that a
   v5 `.so` cannot misread a v6 index is `weave_check_meta()`'s version gate
   (`src/am/am.c:1695`), which refuses before any page kind is examined; the
   fail-closed bit encoding (a v5 reader matches no kind on a v6 page, rather
   than mistaking kind 20 for `POSTING|TRGM`) is defence in depth for readers
   that already understand format version 6, and is proved exhaustively over
   all 2^16 flag words by `test/hegel/test_pagekind.c`. The same break added
   per-bolt weft descriptors
   (`WeaveSegMeta.chandesc`, `SEGMENT_FORMAT.md` §6), the versioned metapage
   reader, and `weave_check()`.
6. **Upgrade path.** Partially addressed: `sql/pg_weave--0.1.0--0.2.0.sql` now
   exists and `sql/wvec.sql` exercises it on every regression run, which caught a
   `flake.nix` `installPhase` that hardcoded one SQL filename and silently dropped
   every new one. **The upgrade over an index containing data is now covered**
   (2026-09-10, task X4): `t/010_format_v6_upgrade.pl` builds a 20,000-row index,
   manufactures a pre-v6 metapage image with the server down, and asserts
   byte-identical answers before and after, then again after an in-place upgrade by
   insert + merge. It also replaces the out-of-tree-only compatibility check
   `t/009_doclen_sidecar.pl` admitted to. **Still owed: a `pg_upgrade` test**
   (gate 15).

### Blocking — correctness under adversity

7. **Crash recovery and replication TAP coverage for every new channel.** The
   inherited tests cover the lexical weft only. A vector weft that survives no
   crash test is a data-loss risk.
8. **Fuzz targets for every new on-disk structure.** On-disk bytes are not
   trusted; a corrupt page must `ERROR`, never crash and never return a wrong
   answer. Current coverage: `fuzz_for`, `fuzz_docvalid`, `fuzz_block`, and
   (2026-09-10) `fuzz_chandesc` for the v6 descriptor page. Each of the last two
   ships a planted-bug variant that must abort, so a toothless harness fails
   instead of passing vacuously.
9. **Torn-write detection** (task V11) with an injection test.
10. **ASan/UBSan clean** on the full suite, not just a normal build. The
    inherited code has one ASan-found SEGV in its history
    (`weave_page_recyclable`); new page types will have their own.
11. **Concurrency proof for new channels.** `t/005` performed 58,049 concurrent
    reads with zero wrong results against the lexical channel. Every new channel
    owes the same test.

### Blocking — operability

12. **Cost model calibrated** against measured latencies (task P4). An
    uncalibrated cost model with `amcanorderbyop` means the planner silently stops
    choosing the index on large tables.
13. **A competitive benchmark matrix that is reproducible** (task P3), including
    the losses.
14. **Documented resource behaviour**: build memory, build temp disk, index size
    per row per channel, and what happens at the 128-segment cap. pg_tre shipped
    without this and a production user hit a temp-disk wall the docs did not
    predict.
15. **`pg_upgrade` compatibility test.**
16. **DocBook reference docs** for every SQL-visible object.

### Non-blocking but expected before anyone should trust it

17. Parallel scan; parallel vacuum.
18. A page recycler, so merge does not leave space only REINDEX reclaims.
19. Predicate locks, hence SSI support.
20. `EXPLAIN` output that shows per-channel work, so a slow query is diagnosable.

## Known permanent limitations

These will not be fixed and belong in any evaluation:

- **No index-only scans.** The index is non-covering by design.
- **Exact recall × sublinear latency × minimal storage: pick two.**
- **No unanchored cross-token substring search** without the opt-in corpus-trigram
  channel, and with it we are not smaller than `pg_trgm`.
- **128 segments per index** (metapage-size limit).
- **Ranked scans do not see unflushed pending rows**; `@@@` and `weave_count()` do.

## Honest timeline

`doc/PHASES.md` estimates **18–30 months of single-maintainer work** to get through
phases L, Z, V, F, M, and P. Nothing since has changed that estimate. The bound
measurement in `bench/RESULTS_BOUND_PRUNING.md` arguably *added* time by
discovering that the vector channel needs per-block centroids and a cluster-ordered
docid space.

The first 6 months produce something **worse** than using pg_fts, pgvector, and
pg_trgm separately, because the channels will be half-built while the separate
extensions are finished.

## The route from here, in dependency order

**The product is BM25 + vector in one index.** That was settled on 2026-09-10 and it
reorders everything below. pg_weave replaces the *combination* of a BM25 index and a
vector-similarity index — the pgvector + pg_fts/pg_textsearch stack — with one index
over one docid space. Fuzzy, regex and prefix remain in the architecture and their
code remains imported and compiling, but **phase Z is no longer on the path to 1.0**:
it is a third channel on a product that needs two.

That single decision is worth more than any task below, because it removes a whole
phase from the critical path rather than making one faster. The former Stage 3 (Z)
was justified as "the cheap second channel" — cheap, and the wrong one. Vector is
expensive and it is the one the product definition names.

29 of 66 tasks are done (`doc/PHASES.md`), phase X included. The ordering is now
forced by two things rather than three: hard rule 7 as amended (F waits for L and V,
not for Z), and every new on-disk structure owing the adversity gates (7–11) before
it counts. The page-kind exhaustion that used to force the ordering is closed.

### Stage 1 — finish the lexical channel (weeks)

Half the product, already competitive, and the cheapest remaining wins.

| task | why now |
|---|---|
| ~~L17 follow-ups~~ | **DONE 2026-09-10.** Both shipped: the pre-bisect walk is now gated by a window test (`weave_for_get` calls/lookup ~12.0 → ~6.55 against a plain bisect's ~7) and the cursor copies the whole sidecar page (buffer hits 767 → 90 on the profiled query, 100k-doc scratch corpus). The gated walk is now itself under property test — `test/hegel/test_doclen_block.c` went 2,839,534 → 22,751,425 checks after the walk was lifted into `include/weave/for.h` so a no-backend test could reach it |
| L2 | Re-aimed: owns the `common` band only (4.75× behind, 1.74 M postings genuinely read) |
| L5 | Positions default decision — phrase is unusable with positions off, and this is a *documented decision*, not code |
| L1 | Split the `am.c` unity build. Blocks nothing, but every task above grows a 7,300-line file |
| L6 | `read_stream` prefetch: the only cold-cache work; all current numbers are warm |

**Exit gate:** G13 at ≤2× pg_textsearch in every band, or the residual documented
as permanent in `doc/ARCHITECTURE.md` §9.

### Stage 2 — format v6, before either channel — DONE 2026-09-10 (0.6.0)

Blocking gate 5, and it had to come first: the vector and fuzzy page-kind bits did
not both fit the `uint16` flags field, so shipping either channel first would have
baked in a collision. Delivered as `doc/PHASES.md` phase X (tasks X1-X4), following
L17's pattern — per-*object* self-description in spare bits of an existing field.

The `WeaveSegMeta` stride question `SEGMENT_FORMAT.md` §2 raised is resolved and
the answer was "it does not move": `chandesc` fits the four bytes of tail padding
the struct already carried for its `double` members, so `sizeof` stays 56, the
`segs[]` stride is unchanged, and the metapage's `generation` does not move either.
The versioned reader (`WeaveMetaPageDataV5` alongside the live struct, with a
`StaticAssertStmt` on the stride) was still built, because the next field added will
not fit the padding and at that moment the reader has to already be right.

**Still open from this stage:** the `pg_upgrade` test (gate 15). The
upgrade-over-an-index-with-data half of gate 6 is closed by
`t/010_format_v6_upgrade.pl`.

### Stage 3 — V, the other half of the product (many months)

Promoted over Z on 2026-09-10. This is now the long pole and the only thing between
pg_weave and the claim it exists to make.

V1–V5 are done — quantizer, `wvec` type, and the 32-lane packing layout, at
17,741 + 1,909,440 property checks. V6 is **partial**: the scalar scoring oracle
exists and three paths (`scalar`, `lut-wide`, `lut-avx2`) are verified `memcmp`-
identical to it over 308,278 differential checks, with dispatch resolved once in
`weave_vec_kernels_init()`. Everything that touches disk remains.

- V7 `WEAVE_PK_VCODES`/`WEAVE_PK_VMETA` pages (kind ids reserved by X1 — read them
  with `WeavePageHasKind()`, never a bitwise AND, and **zero the block buffer
  before packing** or the page image is nondeterministic); V8 code-scan shuttle;
  **V9 IVF**, not Vamana.
- **V13 and V14 are not optional.** `bench/RESULTS_BOUND_PRUNING.md` measured the
  spec'd per-coordinate bound pruning **0.0%** of blocks; centroid+radius prunes
  99.6% *only* with a cluster-ordered warp. Skipping either yields a correct index
  with no pruning, i.e. a linear scan.
- V10 exact path; V11 torn-write detection with an injection test (gate 9).
- The remaining ISA matrix (AVX-512BW/VNNI, NEON, NEON SDOT) and the approximate
  byte-LUT / int8-dot families are unmet and tabulated as unmet in
  `doc/specs/VECTOR_CHANNEL.md` §8. The approximate families quantize the query LUT
  and therefore *cannot* satisfy V6's "identical to scalar" gate; they owe a recall
  budget nobody has derived.

**The Phase V gate may not be reachable as specified, and that is now on the
record.** pg_turbovec v2.7.4 measured IVF's probe count imposing a hard recall
ceiling — 0.846/0.906/0.954/0.978/0.984 at probes 8/16/32/64/128 at `lists=512` —
which no widening of the rerank window moves, because probe count and rerank window
fix different failure modes. That is a property of IVF, not of 1-bit codes, so it
applies to our IVF too. Worse for the comparator: on pg_turbovec's own 500k × 1024-d
corpus, **pgvector HNSW never reached 0.99 either**. So `recall@10 ≥ 0.99` may be a
gate no available design clears on some corpora, and "some setting clears it" is not
evidence that a *fast* setting does. V9's gate now requires a probes-vs-recall-vs-p50
sweep, not a single passing triple. `doc/specs/VECTOR_CHANNEL.md` §8a.

**Exit gate:** the four adversity gates (7, 8, 9, 11) for the new weft
specifically, plus a matched-recall comparison against pgvector HNSW — recall held
equal *and demonstrated reachable by both*, then latency and size compared.

### Stage 4 — F, the actual thesis (months)

Only after L and V gate (hard rule 7 as amended 2026-09-10 — Z was removed from the
precondition when it left the 1.0 path; the rule's *reason* is unchanged, which is
that a fused scorer debugged against a half-working channel costs more than both).
F1–F4, and F5's property test: fused top-k identical to brute force over 10⁶
generated cases. Until F5 passes, the central claim of the project is a document.

Note what fusing two channels instead of three does *not* change: F1's pivot
selection, the essential/non-essential partition, and F5's property test are all
channel-count agnostic. Z arriving later costs F nothing.

### Stage 5 — M, P, R: shippable (months)

**M is where the product definition gets cashed.** "A replacement for pgvector +
BM25" is a compatibility claim before it is a performance claim, so M1/M2 (pgvector
surface and coexistence) and M3 (`tsvector`/`tsquery` casts and a `@@`-compatible
operator) are 1.0 requirements, not polish. M4 (pg_trgm surface) follows Z and
therefore slips past 1.0 with it. M5's in-place index swap and M6's `weave_check()`
health reporting stay in.

Then **P4 cost-model calibration** (without it `amcanorderbyop` silently stops
choosing the index on large tables), P3 the reproducible competitive matrix
including losses, then R1–R5: DocBook docs, examples, PGXN, managed-service
readiness, contrib submission.

### After 1.0 — Z, the third channel

Not cancelled, not deferred by neglect: **deliberately sequenced after the product
exists.** The code is imported and compiling, TRE is current (`f864ed0`, carrying
the `INT_MAX` crash fix and the backref wrong-answer fix, both with regression
coverage in `test/hegel/`), and Z3–Z9 are unchanged in `doc/PHASES.md`. Z7 still owes
the other half of pg_tre `4a9c86c` — the prefilter must refuse to reject when
`always_true` is set, with a case-insensitive-anchored-pattern regression test.

The two bugs just ported are worth a note on why this ordering is safe: both were
**latent** here, unreachable from SQL precisely because the channel is unrouted. An
unrouted channel cannot return a wrong answer. That is what makes deferring Z cheap
and deferring a *routed* half-built channel expensive.

### Cross-cutting, continuous — not a stage

ASan/UBSan on the full suite (gate 10), a fuzz target per on-disk structure
(gate 8), `weave_check()` covering all ~20 `SEGMENT_FORMAT.md` §9 invariants
(gate 4 — `weave_check()` now exists and covers 11; before phase X there was only
`weave_check_meta()`), and documented resource behaviour (gate 14 — pg_tre shipped
without it and a user hit a temp-disk wall the docs did not predict). Also: **a
Codeberg CI runner is still not registered**, so `.forgejo/workflows/ci.yml` has
never run. That is a repo-settings action and it gates everything above.

## A lexical-only 1.0 is still a real option — but it is now a different trade

The staged plan above remains 18–30 months, and dropping Z from the critical path
does not change that, because Z was never the long pole. V is.

pg_weave's lexical channel is **already better than the alternatives on four
measured axes** — index size (626 MB vs 873/1120), build time (at GIN parity),
`count(*)` (~200×), keyless `ORDER BY` (index path where GIN seq-scans) — and near
parity on ranked latency, winning outright at k=100. That is a shippable product
with a defensible claim, reachable in weeks rather than years.

What changed on 2026-09-10 is the cost of choosing it. With the product defined as
BM25 + vector, a lexical-only 1.0 is no longer "ship early and add channels later" —
it is **shipping something the product definition says is not the product.** It
would compete with pg_fts and pg_textsearch, which is a fight over a channel we
inherited rather than the fight the architecture argues for.

The honest framing: a lexical-only release is a good *0.x* and a bad *1.0*. Version
numbers are cheap and the fused thesis is not.

This is a maintainer decision, not a technical one, and it should be made on
purpose.

## What to do instead, today

| need | use |
|---|---|
| BM25 text search in Postgres | **pg_fts** (same code, real release history) or Timescale's pg_textsearch |
| Vector search in Postgres | **pgvector** (HNSW), or VectorChord |
| Fuzzy / substring | **pg_trgm**; add **pg_tre** only for k≥1 edit distance at modest scale |
| Hybrid at scale, managed, willing to leave Postgres | **turbopuffer** or ParadeDB — see `doc/COMPETITIVE.md` |
| Storage-optimal exact vector recall where an O(n) scan fits | pg_turbovec |

## How this document gets updated

Every entry above moves from blocking to done **only** when its gate command
passes and the output is recorded. Not when the code is written. Not when it looks
right. If you are reading this and the gate list is unchanged but the README has
grown confident, the README is wrong.
