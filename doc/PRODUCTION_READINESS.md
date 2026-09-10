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

| capability | state | evidence |
|---|---|---|
| BM25 lexical search, boolean, phrase, NEAR, prefix | **works** | forked from pg_fts 1.5.8, current with upstream 1.6.0; 5 regression + 2 isolation + 61 TAP green on PG 17 and 18 |
| Index-native `count(*)` | **works** | inherited; measured ~200× faster than tsvector+GIN at 2 M docs (`bench/RESULTS_LEXICAL.md`) |
| Ranked top-k latency | **works, competitive at k=100** | after L14/L15/L17: rare k=10 1.51 ms (1.20× pg_textsearch), k=100 **wins** rare 3.96× and mid 1.27×; common k=10 still 4.75× behind (task L2). `bench/RESULTS_L17.md` |
| Build time | **at GIN parity** | 192.5 s vs GIN 202.7 s, pg_textsearch 49.2 s (3.91× behind). `bench/RESULTS_L15.md` |
| Index size | **best of the three** | 626 MB vs pg_textsearch 873 MB, tsvector+GIN 1120 MB |
| Crash recovery, replication, MVCC, CIC/REINDEX | **works** | inherited; `t/001`–`t/009` |
| Vacuum, tombstones, tiered merge | **works** | inherited; `t/008` reclaims 4688 → 2199 pages |
| Vector quantizer: rotation, codebook, encode/decode, packing | **works** | 17,741 property checks, 0 failures (tasks V2–V4) |
| `wvec` type: I/O, typmod, casts, 4 distance operators, arithmetic, btree | **works** | task V1; `sql/wvec.sql`, green on PG 17 and 18 |
| Quantizer reachable from SQL (`weave_quantize_roundtrip`) | **works** | lets reconstruction error be measured on a real corpus before the index exists |
| Vector *indexing* (the AM accepting a `wvec` column) | **does not exist** | tasks V7–V9 |
| Fuzzy / regex / prefix channel | **compiles, unreachable** | tasks Z1/Z2 done (TRE vendored, GUCs wired); no channel routing yet (Z3–Z7) |
| Vector storage, SIMD kernels, ANN graph | **does not exist** | stubs that `ereport(ERROR)` |
| Fused-threshold top-k | **does not exist** | specified only |
| pgvector / tsvector / pg_trgm compatibility | **does not exist** | specified only |

So: pg_weave today is *pg_fts with a rename, plus a tested quantizer nothing calls
yet* — with one qualification that has grown real since this line was written. The
L-phase work (L7, L8, L10, L12, L14, L15, L17) has made the **lexical channel
measurably better than what it forked from** on size, build time, `count(*)`,
keyless `ORDER BY`, and deep-page ranked latency, all recorded in `bench/`. If you
want the lexical capability in production today, **pg_fts is still the safer
choice** — longer track record, real release history, and pg_weave's advantages are
measured on one synthetic corpus. But the honest statement is no longer "identical
code"; see "A lexical-only 1.0 is a real option" below.

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

24 of 63 tasks are done (`doc/PHASES.md`), phase X included. The ordering below is forced by three
things: the page-kind bit exhaustion blocks *both* remaining channels, hard rule 7
forbids starting F before L/Z/V gate, and every new on-disk structure owes the
adversity gates (7–11) before it counts.

### Stage 1 — finish the lexical channel (weeks)

The only phase where pg_weave already competes, and the cheapest remaining wins.

| task | why now |
|---|---|
| L17 follow-ups | Both specified and cheap from `bench/RESULTS_L17.md`: the 8-step pre-bisect walk is counterproductive at sparse stride (~15 `weave_for_get` calls/lookup), and a whole-page copy now cuts buffer hits ~15,000 → ~584 per query — an idea correctly rejected for v4 that v5 makes viable because it no longer decodes |
| L2 | Re-aimed: owns the `common` band only (4.75× behind, 1.74 M postings genuinely read) |
| L5 | Positions default decision — phrase is unusable with positions off, and this is a *documented decision*, not code |
| L1 | Split the `am.c` unity build. Blocks nothing, but every task above grows a 6,800-line file |
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

### Stage 3 — Z, the cheap second channel (months)

Two channels is the product; fuzzy is far cheaper than vector because the code is
already imported and compiling.

- **First:** vendor TRE `d0e0c997` → `f864ed0` (`IMPORT_pg_tre.md`). Carries an
  `INT_MAX` crash fix and a backref wrong-answer fix; pg_tre already rebased the
  progress-hook patch, so reuse it.
- Z3 (SuRF over the *vocabulary*, not the corpus) → Z4/Z5/Z6 routing → Z7 shuttle.
- **Z7 owes the other half of pg_tre `4a9c86c`**: the prefilter must refuse to
  reject when `always_true` is set, with a case-insensitive-anchored-pattern
  regression test. The extraction half is already ported.
- Z8 (`cgram`) is opt-in and can slip; Z9 (`<@>` KNN) needs a real bound.

**Exit gate:** bound property test (gate 3), fuzz target (gate 8), crash +
replication TAP (gate 7), concurrency test matching `t/005`'s 58,049 reads
(gate 11) — for the new weft specifically.

### Stage 4 — V, the expensive channel (many months)

V1–V4 are done (quantizer + type, 17,741 property checks). What remains is
everything that touches disk.

- V6 SIMD kernels with runtime dispatch; V7 `WEAVE_VCODES`/`WEAVE_VMETA` pages;
  V8 code-scan shuttle; **V9 IVF** (not Vamana — withdrawn on pg_turbovec's
  matched-recall evidence that the graph never reached R@10 0.98).
- **V13 and V14 are not optional.** `bench/RESULTS_BOUND_PRUNING.md` measured the
  spec'd per-coordinate bound pruning **0.0%** of blocks; centroid+radius prunes
  99.6% *only* with a cluster-ordered warp. Skipping either yields a correct index
  with no pruning, i.e. a linear scan.
- V10 exact path; V11 torn-write detection with an injection test (gate 9).
- Adopt pg_turbovec's measured lessons rather than rediscovering them: 1-bit BQ
  needs mean-centering and un-rotated centroids, and its chain-offset-sum bug
  recurred four times because descriptor offsets were not single-sourced.

**Exit gate:** same four adversity gates as Stage 3, plus a matched-recall
comparison against pgvector HNSW — recall held equal, then latency and size
compared.

### Stage 5 — F, the actual thesis (months)

Only after L, Z and V gate (hard rule 7). F1–F4, and F5's property test: fused
top-k identical to brute force over 10⁶ generated cases. Until F5 passes, the
central claim of the project is a document.

### Stage 6 — M, P, R: shippable (months)

M1–M6 migration surfaces (pgvector/tsvector/pg_trgm), **P4 cost-model calibration**
(without it `amcanorderbyop` silently stops choosing the index on large tables),
P3 the reproducible competitive matrix including losses, then R1–R5: DocBook docs,
examples, PGXN, managed-service readiness, contrib submission.

### Cross-cutting, continuous — not a stage

ASan/UBSan on the full suite (gate 10), a fuzz target per on-disk structure
(gate 8), `weave_check()` covering all ~20 `SEGMENT_FORMAT.md` §9 invariants
(gate 4), and documented resource behaviour (gate 14 — pg_tre shipped without it
and a user hit a temp-disk wall the docs did not predict). Also: **a Codeberg CI
runner is still not registered**, so `.forgejo/workflows/ci.yml` has never run.
That is a repo-settings action and it gates everything above.

## A lexical-only 1.0 is a real option

The staged plan above is 18–30 months. There is a shorter path worth deciding
explicitly rather than by default.

After Stage 1, pg_weave's lexical channel is **already better than the alternatives
on four measured axes** — index size (626 MB vs 873/1120), build time (at GIN
parity), `count(*)` (~200×), keyless `ORDER BY` (index path where GIN seq-scans) —
and near parity on ranked latency, winning outright at k=100. That is a shippable
product with a defensible claim, reachable in weeks rather than years, and it does
not foreclose the rest: Stage 2's format break is designed to be additive.

The cost of choosing it: the fused top-k thesis stays unproven, and pg_weave ships
as "a better pg_fts" rather than as the thing `doc/ARCHITECTURE.md` §3 argues for.
The cost of *not* choosing it: 18–30 months during which the honest recommendation
in the table below stays "use something else".

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
