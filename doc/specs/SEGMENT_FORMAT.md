# Specification: segment (bolt) on-disk format

Authoritative description of what a `weave` index contains on disk. This document
distinguishes carefully between **what exists today (v4)** and **what is
specified but unbuilt (v5)**; the distinction is marked on every section, because
a reader who confuses the two will write code against structures that are not
there.

Header of record: `include/weave/am.h`.

## 1. The model on disk

A `weave` index is a metapage plus a size-tiered set of **bolts** (segments) plus
a pending write buffer.

- A **bolt** is immutable once written. It has its own dictionary, posting lists,
  tombstone bitmap, and whatever other wefts it carries.
- The **warp** is the bolt's dense local docid space, `0 .. ndocs-1`. Every weft
  in the bolt indexes into the same warp. This is the property the entire
  cross-modal-skipping argument rests on (`doc/ARCHITECTURE.md` §3).
- Deletion sets a bit in the bolt's livedocs bitmap and does nothing else.
- Reorganization happens only by **merge**: read *n* bolts, drop tombstoned docs,
  write one new bolt, free the old pages.

This is the Lucene/Tantivy consensus design grafted onto PostgreSQL's buffer
manager. Inherited from pg_fts, field-tested.

## 2. Page-kind bit allocation — AUTHORITATIVE

`WeavePageOpaqueData.flags`. **Do not improvise a bit.** Adding a kind means
editing this table, adding the `#define` beside its siblings in the owning
header, and extending `weave_check()` with an invariant for the new page type.

| bit | macro | owner | status |
|---:|---|---|---|
| 0 | `WEAVE_META` | segment machinery | v4, exists |
| 1 | `WEAVE_DICT` | lexical | v4, exists |
| 2 | `WEAVE_POSTING` | lexical | v4, exists |
| 3 | `WEAVE_PENDING` | segment machinery | v4, exists |
| 4 | `WEAVE_TRGM` | lexical (vocabulary trigram directory) | v4, exists |
| 5 | `WEAVE_TRGM_DATA` | lexical (trigram sparsemap blobs) | v4, exists |
| 6 | `WEAVE_LIVEDOCS` | segment machinery (tombstones) | v4, exists |
| 7 | `WEAVE_DICTINDEX` | lexical (sparse block index over dict pages) | v4, exists |
| 8 | `WEAVE_FREED` | segment machinery (page pending recycle) | v4, exists |
| 9 | `WEAVE_DOCLEN` | lexical (per-segment doclen sidecar) | v4, exists |
| 10 | `WEAVE_VMETA` | vector | v5, specified |
| 11 | `WEAVE_VCODES` | vector | v5, specified |
| 12 | `WEAVE_VGRAPH` | vector | v5, specified |
| 13 | `WEAVE_VRERANK` | vector | v5, specified |
| 14 | `WEAVE_SURF` | fuzzy (LOUDS-Sparse trie over the vocabulary) | v5, specified |
| 15 | `WEAVE_ULEV` | fuzzy (universal-Levenshtein aux, if needed) | reserved |
| 16 | `WEAVE_REGEX` | fuzzy (compiled-pattern cache page, if persisted) | reserved |
| 17 | — | fuzzy | reserved |
| 18 | `WEAVE_DOCVALS` | docvalues (scalar/facet forward store) | v5, specified |
| 19 | `WEAVE_CGRAM` | opt-in corpus-level character trigrams | v5, specified |
| 20 | `WEAVE_CHANDESC` | v5 per-bolt channel descriptor | v5, specified |
| 21–15 | — | unallocated | `flags` is `uint16`; bits 21+ do not exist |

`flags` is 16 bits. Allocation is therefore finite and bits 14–19 above already
exceed it if taken literally — **this is a real constraint that must be resolved
in v5**, not a detail. Two options, and v5 must pick one explicitly:

1. Widen `flags` to `uint32` in the page opaque area. Changes the page layout for
   every page, so it is a hard format break requiring REINDEX.
2. Stop using a bitmask and store a small integer *kind* plus a separate flag
   byte. Bits were only ever a bitmask because a page could plausibly be two
   things at once; in practice no page is, so this is the cleaner fix and it
   leaves room for 250 kinds.

Recommendation: option 2, taken as part of the v5 bump, since v5 is already a
format break. Whoever implements v5 must resolve this before adding the vector
pages, or the vector and fuzzy channels will collide.

## 3. Metapage — v4, EXISTS

Block 0. `WeaveMetaPageData` (`include/weave/am.h`).

| field | type | meaning |
|---|---|---|
| `magic` | `uint32` | `WEAVE_MAGIC` = 0x42324635 |
| `version` | `uint32` | 4 today; 3 still read |
| `ndocs` | `double` | corpus N: live segments + pending, minus tombstones |
| `sumdoclen` | `double` | corpus Σ doclen, giving avgdl |
| `nsegments` | `uint32` | live bolt descriptors |
| `pendinghead` / `pendingtail` | `BlockNumber` | pending chain, tail for O(1) append |
| `npending` | `uint32` | unmerged documents |
| `segs[128]` | `WeaveSegMeta` | the bolt directory |
| `generation` | `uint32` | bumped on any directory change; see §7 |

`ndocs`, `sumdoclen`, and the per-term `df` in the dictionary are what let BM25 be
answered **from the index alone** — no stats table, no heap rescan. That is
unusual among PostgreSQL search extensions and it is worth not breaking.

`WEAVE_MAX_SEGMENTS = 128` because `segs[]` must fit the metapage (~6 KB of 8 KB).
The size-tiered merge keeps the live count far below it; it is a backstop, and
hitting it is an error, not a silent degradation.

`generation` is placed **after** `segs[]` deliberately, so the on-disk offset of
every pre-existing field is unchanged and no REINDEX was needed when it was added.
Only the *change* in the value across a scan matters, never its absolute value —
so an index written by an older build, which has arbitrary bytes there, still
works. Keep this trick in mind for future additive fields.

## 4. Bolt descriptor — v4, EXISTS

`WeaveSegMeta`, one per live bolt:

| field | meaning |
|---|---|
| `dictstart` | first dictionary page |
| `trgmstart` | first trigram directory page, or Invalid |
| `livedocs` | first tombstone page, or Invalid |
| `ndocs` | documents including tombstoned |
| `sumdoclen` | Σ doclen in this bolt |
| `nterms` | distinct terms |
| `ndeleted` | tombstoned docs, for merge accounting |
| `livedocslen` | serialized size of the tombstone blob |
| `dictindexstart` | sparse block index over dict pages, or Invalid |
| `doclenstart` | doclen sidecar (v4), or Invalid for a v3 bolt with inline doclen |

`doclenstart` being per-bolt is what makes dual-read work: a v3 bolt and a v4 bolt
can coexist in one index and each is read according to its own descriptor. This is
the pattern to follow for v5.

## 5. Lexical weft — v4, EXISTS

**Dictionary.** Sorted `WeaveDictEntry` records (term, df, max_tf, first posting
pointer) chained across pages, plus a sparse block index (`WeaveDictIndexEntry`,
one entry per dict page recording its first term) giving O(log P) term lookup. An
FST would be smaller; this is FST-equivalent in complexity and far simpler.

**Postings.** Fixed 128-document blocks (`WEAVE_BLOCK_SIZE`), Lucene-style. Each
block:

```
WeaveBlockHdr { first_docid, max_tf, min_doclen, ... }
FOR column 1: docid gaps        bit-packed
FOR column 2: term frequencies  bit-packed
FOR column 3: doc lengths       bit-packed   (v3 only; v4 uses the sidecar)
FOR column 4: token positions   bit-packed, delta-coded, LAZY
```

`max_tf` and `min_doclen` are the block-max WAND bound. They are the lexical
channel's `block_max()` under `include/weave/channel.h`.

Column 4 exists only when the index was built `WITH (positions = on)` and is
skipped by byte length in one add when phrase/NEAR is not needed — so
`positions = on` costs approximately nothing for plain boolean or BM25 queries.
That laziness is why it can be a per-field option rather than a global tradeoff.

Plain FOR only: no PFOR, no patched outliers, no varbyte, no roaring. Measured on
the Wikipedia corpus, outlier patching would have affected ~7 % of columns and
< 0.5 % of index size — not worth the decode complexity. Recorded here so the
question is not reopened without new evidence.

**Codec.** `include/weave/for.h`, deliberately backend-independent so it is
property-tested and fuzzed standalone.

**Tombstones.** A vendored sparsemap blob per bolt.

**Doclen sidecar (v4).** One quantized length byte per doc plus a FOR docid-gap
column, in 128-doc blocks. Bought a 4.7× index-size reduction (1421 MB vs
6729 MB on the 2.19 M-article corpus) and introduced a per-scan decode tax that is
task **L3** in `doc/PHASES.md`.

## 6. v5: channel descriptors — SPECIFIED, NOT BUILT

The problem v5 solves: a bolt must **self-describe which wefts it carries**, so
that an index built without a vector channel costs literally zero vector bytes
rather than empty structures, and so a reader never infers geometry from a GUC
that may have changed since the build.

Design:

- `WeaveSegMeta` gains `BlockNumber chandesc`, pointing at a `WEAVE_CHANDESC`
  page holding an array of `WeaveChannelDesc { kind, attnum, flags, root }`.
- A v4 bolt has `chandesc = InvalidBlockNumber`, meaning "lexical only", so v4
  bolts remain readable **within a v4 metapage**.
- Adding a field to `WeaveSegMeta` changes the `segs[]` stride and therefore the
  metapage layout. So the metapage itself must be read through a versioned
  reader: `WeaveMetaPageDataV4` alongside `WeaveMetaPageDataV5`. The codebase
  already does exactly this for v3 (`WeaveMetaPageDataV3`); follow that pattern
  rather than inventing another.
- The page-kind bit exhaustion in §2 must be resolved in the same change.

## 7. Concurrency invariants — v4, EXISTS

- Readers take `AccessShareLock` + `BUFFER_LOCK_SHARE`. Merge under
  `RowExclusiveLock` + advisory; `weave_merge`/`weave_vacuum` under
  `AccessExclusiveLock`; autovacuum cleanup under `ShareUpdateExclusiveLock`.
- Lock order: metapage before segment pages; lower block number before higher
  within a chain.
- A scan snapshots `generation` and re-checks it before trusting results. If it
  moved, a concurrent merge may have freed and recycled pages the scan read
  through a stale descriptor, so the scan restarts from a fresh snapshot. **Any
  new code that caches a bolt descriptor across page reads must participate in
  this.**
- `weave_page_recyclable()` bypasses the deletion-XID check only when the caller
  provably holds `AccessExclusiveLock`, verified with `CheckRelationLockedByMe()`.
  This gate exists because of an AddressSanitizer-found SEGV: a reader holding a
  pointer into a page that `weave_vacuum()` had freed and reused.

## 8. Compatibility policy

1. Versions read: v3 and v4. Versions written: v4.
2. **An unknown or inconsistent version is an `ERROR`, never a best-effort read.**
   A best-effort read of a format we do not understand returns wrong answers,
   which is strictly worse than refusing. pg_turbovec's strict-versioning policy
   is the model here.
3. On-disk bytes are **not trusted**. Every decoder validates before use, and
   every on-disk structure gets a fuzz target in `test/fuzz/`. A corrupt page must
   produce a clean `ERROR`.
4. Additive fields go **after** existing ones when the containing struct is not
   an array element, so offsets do not move (see `generation`, §3). Fields added
   to an array element require a version bump and a versioned reader.
5. **Chain offsets must be COMPUTED from one place, never hand-summed per call
   site.** pg_turbovec has now hit the same bug **four times**: a running
   chain-offset sum omitted one count field, so a build wrote one chain on top of
   another's data (v2.7.0 fixed the fourth instance, `set_ivf_chains` omitting
   `bq_mean_count`, found by audit rather than a field report). When v5's channel
   descriptors add per-segment chains here, derive every offset from a single
   function over the descriptor array and have `weave_check()` assert that no two
   chains overlap — a bug class that recurs four times in a sibling project is not
   going to be avoided by care.

## 9. Invariants `weave_check()` must verify

One per line, each mechanically testable. Marked (v5) where the structure does not
exist yet.

- Metapage magic and version are recognized.
- `nsegments <= WEAVE_MAX_SEGMENTS`.
- Every `segs[i]` root block is within relation bounds and has the expected kind.
- `Σ segs[i].ndocs - Σ segs[i].ndeleted + npending == metapage ndocs`.
- Dictionary terms are strictly ascending within a bolt.
- Every `WeaveDictIndexEntry` first-term equals the first term of the page it
  names.
- Every dictionary posting pointer lands on a `WEAVE_POSTING` page.
- Within a posting list, block `first_docid` values are strictly ascending.
- For every block: `max_tf >= max(tf)` and `min_doclen <= min(doclen)` over the
  block. **A violation here is a live bound-soundness bug**, not cosmetic
  corruption — it breaks contract (C2).
- Every posting docid is `< segs[i].ndocs`.
- Livedocs blob length equals `livedocslen`; popcount equals `ndocs - ndeleted`.
- Trigram directory entries point at `WEAVE_TRGM_DATA` pages; every term ordinal
  in a trigram sparsemap is `< nterms`.
- Doclen sidecar covers exactly the bolt's docid range with no gaps or duplicates.
- Every page is reachable from the metapage, or flagged `WEAVE_FREED`. An
  unreachable unflagged page is a leak.
- (v5) Every `WeaveVecBlockHdr.smax`, `maxrecnorm`, `minnorm`, `cenrad` equals a
  recomputation from the block's live lanes. See
  `bench/RESULTS_BOUND_PRUNING.md` for why `cenrad` in particular must be
  recomputed against the centroid *as decoded from its stored code*.
- (v5) Graph: out-degree `<= R`; neighbour lists sorted ascending; no edge to a
  warp position `>= nnodes`; no edge to a tombstoned node; entry point live; every
  live node reachable from the entry point. The last is the expensive check and
  the one that actually catches a bad build, so it belongs behind `deep => true`.
- (v5) SuRF trie membership is exactly the bolt's dictionary term set.

## 10. WAL policy

100 % `GenericXLog`. Zero raw `XLogInsert`, `log_newpage`, or `smgrwrite`. No
custom resource manager.

pg_tre used a custom rmgr (140), and pg_weave deliberately did not inherit it.
GenericXLog writes more WAL — full-page images rather than tight deltas — and in
exchange:

- crash safety is auditable by inspection rather than by trusting a redo routine;
- physical replication works with no rmgr installed on the standby;
- the extension can be marked `trusted`, so a non-superuser can install it on a
  managed service.

Those three properties are a large part of why one would choose pg_weave over an
AGPL extension embedding its own engine. The extra WAL volume is the price and it
is not negotiable for a speedup.
