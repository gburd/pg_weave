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
| BM25 lexical search, boolean, phrase, NEAR, prefix | **works** | inherited from pg_fts 1.5.8; 3 regression + 2 isolation + 81 TAP tests green on PG 17 and 18 |
| Index-native `count(*)` | **works** | inherited; measured 3.8 ms vs pg_search's 16.31 ms at 2.19 M docs |
| Crash recovery, replication, MVCC, CIC/REINDEX | **works** | inherited; `t/001`–`t/009` |
| Vacuum, tombstones, tiered merge | **works** | inherited; `t/008` reclaims 4688 → 2199 pages |
| Vector quantizer: rotation, codebook, encode/decode, packing | **works** | 17,741 property checks, 0 failures |
| `wvec` type: I/O, typmod, casts, 4 distance operators, arithmetic, btree | **works** | task V1; `sql/wvec.sql`, green on PG 17 and 18 |
| Quantizer reachable from SQL (`weave_quantize_roundtrip`) | **works** | lets reconstruction error be measured on a real corpus before the index exists |
| Vector *indexing* (the AM accepting a `wvec` column) | **does not exist** | tasks V7–V9 |
| Fuzzy / regex / prefix channel | **imported, not compiled** | sources present, not in `OBJS` |
| Vector storage, SIMD kernels, ANN graph | **does not exist** | stubs that `ereport(ERROR)` |
| Fused-threshold top-k | **does not exist** | specified only |
| pgvector / tsvector / pg_trgm compatibility | **does not exist** | specified only |

So: pg_weave today is *pg_fts with a rename, plus a tested quantizer nothing calls
yet.* If you want the lexical capability in production today, **use pg_fts** — it
is the same code with a longer track record and a real release history.

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
   `doc/specs/SEGMENT_FORMAT.md` §9, and there are 20-odd. Today it covers the
   inherited lexical ones only.
5. **Format v5 must resolve the page-kind bit exhaustion.**
   `doc/specs/SEGMENT_FORMAT.md` §2 documents that the vector and fuzzy channels'
   proposed bits do not both fit in the `uint16` flags field. Shipping either
   channel before fixing this bakes in a collision.
6. **Upgrade path.** Partially addressed: `sql/pg_weave--0.1.0--0.2.0.sql` now
   exists and `sql/wvec.sql` exercises it on every regression run, which caught a
   `flake.nix` `installPhase` that hardcoded one SQL filename and silently dropped
   every new one. Still owed: a `pg_upgrade` test and an upgrade over an index
   containing data.

### Blocking — correctness under adversity

7. **Crash recovery and replication TAP coverage for every new channel.** The
   inherited tests cover the lexical weft only. A vector weft that survives no
   crash test is a data-loss risk.
8. **Fuzz targets for every new on-disk structure.** On-disk bytes are not
   trusted; a corrupt page must `ERROR`, never crash and never return a wrong
   answer.
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
