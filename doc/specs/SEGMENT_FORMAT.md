# Specification: segment (bolt) on-disk format

Authoritative description of what a `weave` index contains on disk. This document
distinguishes carefully between **what exists today (v6)** and **what is
specified but unbuilt**; the distinction is marked on every section, because a
reader who confuses the two will write code against structures that are not
there.

Headers of record: `include/weave/am.h`, `include/weave/pagekind.h` (the kind
space), `include/weave/chandesc.h` (the channel-descriptor page and its
validator).

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

## 2. Page-kind allocation — AUTHORITATIVE

`WeavePageOpaqueData` is two 16-bit words plus `nextblk`:

```c
typedef struct WeavePageOpaqueData
{
	uint16		flags;		/* legacy one-hot kinds, the FREED state, the escape */
	uint16		kind;		/* extended WeavePageKind; valid only under the escape */
	BlockNumber nextblk;
} WeavePageOpaqueData;
```

**Do not improvise a kind.** Adding one means editing this table, adding the id to
the `WeavePageKind` enum in `include/weave/pagekind.h` (channel headers *alias*
it beside the struct they describe — `WEAVE_VMETA` in `include/weave/vector.h`),
and extending `weave_check()` with an invariant for the new page type.

| id | macro | on-disk representation | owner | status |
|---:|---|---|---|---|
| 1 | `WEAVE_PK_META` | `flags` bit 0 | segment machinery | v4, exists |
| 2 | `WEAVE_PK_DICT` | `flags` bit 1 | lexical | v4, exists |
| 3 | `WEAVE_PK_POSTING` | `flags` bit 2 | lexical | v4, exists |
| 4 | `WEAVE_PK_PENDING` | `flags` bit 3 | segment machinery | v4, exists |
| 5 | `WEAVE_PK_TRGM` | `flags` bit 4 | lexical (vocabulary trigram directory) | v4, exists |
| 6 | `WEAVE_PK_TRGM_DATA` | `flags` bit 5 | lexical (trigram sparsemaps) **and every livedocs blob** | v4, exists |
| 7 | `WEAVE_PK_LIVEDOCS` | `flags` bit 6 | segment machinery (tombstones) | **allocated, never written** |
| 8 | `WEAVE_PK_DICTINDEX` | `flags` bit 7 | lexical (sparse block index over dict pages) | v4, exists |
| — | `WEAVE_FREED` | `flags` bit 8 | segment machinery | **a state, not a kind** |
| 9 | `WEAVE_PK_DOCLEN` | `flags` bit 9 | lexical (per-segment doclen sidecar) | v4, exists |
| — | — | `flags` bits 10–14 | — | **reserved, must read as zero** |
| — | `WEAVE_PAGE_KIND_EXT` | `flags` bit 15 | segment machinery | the escape bit |
| 16 | `WEAVE_PK_CHANDESC` | `kind` = 16 | segment machinery (per-bolt weft descriptor) | **v6, exists** |
| 17 | `WEAVE_PK_VMETA` | `kind` = 17 | vector | reserved |
| 18 | `WEAVE_PK_VCODES` | `kind` = 18 | vector | reserved |
| 19 | `WEAVE_PK_VGRAPH` | `kind` = 19 | vector | reserved |
| 20 | `WEAVE_PK_VRERANK` | `kind` = 20 | vector | reserved |
| 21 | `WEAVE_PK_SURF` | `kind` = 21 | fuzzy (LOUDS-Sparse trie over the vocabulary) | **v7, exists** |
| 22 | `WEAVE_PK_ULEV` | `kind` = 22 | fuzzy (universal-Levenshtein aux) | reserved |
| 23 | `WEAVE_PK_REGEX` | `kind` = 23 | fuzzy (compiled-pattern cache) | reserved |
| 24 | `WEAVE_PK_FUZZY_SPARE` | `kind` = 24 | fuzzy | reserved |
| 25 | `WEAVE_PK_DOCVALS` | `kind` = 25 | docvalues (scalar/facet forward store) | reserved |
| 26 | `WEAVE_PK_CGRAM` | `kind` = 26 | corpus-level character trigrams (weft root) | **exists** |
| 27 | `WEAVE_PK_VDIR` | `kind` = 27 | vector (per-block directory records) | **exists** |
| 28 | `WEAVE_PK_VWARP` | `kind` = 28 | vector (warp → docid) | **exists** |
| 29 | `WEAVE_PK_PENDING_V9` | `kind` = 29 | segment machinery (pending page, v9 item layout) | **read-only legacy** |
| 30 | `WEAVE_PK_CGRAM_DICT` | `kind` = 30 | cgram (trigram → docid dictionary) | **exists** |
| 31 | `WEAVE_PK_CGRAM_DICTINDEX` | `kind` = 31 | cgram (sparse block index) | **exists** |
| 32 | `WEAVE_PK_CGRAM_POST` | `kind` = 32 | cgram (docid posting chain) | **exists** |
| 33 | `WEAVE_PK_PENDING_V10` | `kind` = 33 | segment machinery (pending page, v10 item layout) | **read-only legacy** |
| 34 | `WEAVE_PK_PENDING_V11` | `kind` = 34 | segment machinery (pending page, v11 item layout: + docvalues trailer) | **read-only legacy** (written by v11) |
| 35 | `WEAVE_PK_DOCLIST` | `kind` = 35 | segment machinery (per-bolt document list, §6 "The document list") | **v12, exists** |
| 36 | `WEAVE_PK_PENDING_V12` | `kind` = 36 | segment machinery (pending page, v11 item layout, `doclen == 0` = NULL document) | **v12, the only kind written** |

**The pending chain is the one chain that legitimately mixes page kinds**, because
the item HEADER has grown twice — 12 bytes (v8) → 16 (v9, a `veclen` for the
inserted row's vector) → 20 (v10, a `gramlen` for the raw `gram_ops` text) — and
the three strides are not distinguishable from the bytes. So the page declares its
layout, `weave_insert()` starts a fresh page rather than appending an item of one
layout to a page of another, and the discriminator is per PAGE and not the metapage
version, because `weave_insert()` deliberately does not upcast the metapage. One
index can therefore hold all three at once; `weave_pending_iter_next()` in
`include/weave/am.h` is the only reader of any of them, and `t/019` manufactures a
v8 image to keep the oldest branch executed rather than merely claimed.

Ids 1–15 are **in-memory only**; the disk carries the one-hot bit. Ids ≥ 16 **are**
the byte pattern stored in `kind`, so they are on-disk ABI: never renumber one,
only append. `WEAVE_PK_NKINDS` is one past the last.

`WEAVE_PK_LIVEDOCS` is marked *allocated, never written* because it is: the
livedocs blob goes out through `weave_write_blob()`, which lays it on
`WEAVE_TRGM_DATA` pages. `weave_index_size_detail()` therefore reports zero
livedocs pages and counts tombstone bytes under `trigram_data`. Found by writing
the validator; recorded rather than fixed, because correcting the writer moves
page bytes for a kind that reads fine either way and does not belong in a format
break about something else.

### Why the kind space is no longer a flat bitmap

Prior to v6 `flags` was a pure bitmask and bits 0–9 were spent. The vector channel
wanted four more bits and the fuzzy channel four, plus docvalues and
corpus-trigrams: bits 10–19 of a 16-bit field. **They do not both fit**, so
shipping either channel on the old scheme would have baked in a collision — which
is why `doc/PRODUCTION_READINESS.md` makes this a blocking gate ahead of both
channels.

Two options were on the table. Widening `flags` to `uint32` moves the page opaque
area on **every page of every existing index**, forcing a REINDEX on relations
that contain no new page kind at all. v6 took the other: a **reserved escape bit**
selecting an extended integer kind space, with the kind stored in the previously
unused second opaque word.

The argument, in the order that decided it:

1. **It is the pattern L17 already proved here.** L17 discriminated two
   doclen-sidecar encodings with a flag in the spare high bits of an existing
   count field, **per block**, because an index upgraded from ≤ 0.5.0 keeps its
   old pages while later merges write new ones — a per-index version cannot
   describe a mixed relation (§5). Page kinds have exactly that shape: after an
   upgrade one relation holds v5-written lexical pages and v6-written channel
   pages at the same time, so the discriminator has to be **per page**.

2. **The hard guarantee is the metapage version gate, not the bit layout.**
   `weave_check_meta()` (`src/am/am.c:1695`) rejects any metapage whose
   `version` exceeds the reading build's own `WEAVE_VERSION` before that build
   ever looks at a page's kind bits. A v5 `.so` has `WEAVE_VERSION == 5`; it
   opens a v6 index, sees `meta->version == 6`, and errors out at every entry
   point that calls it (`am.c:6123`, `am.c:6583`, `amscan.c:335`,
   `amcheck.c:684`) — before scanning a single non-meta page. That check does
   not depend on which bits an extended kind sets, so it holds even if a future
   encoding reused low bits by mistake; it is the reason a v5 `.so` never
   reaches a `WEAVE_PK_CHANDESC` page at all.
   The kind encoding below is **defence in depth**, not the primary mechanism:
   it covers the narrower case of two readers that both understand format
   version 6 but disagree about which bits are legacy versus extended. There,
   under the extended encoding every legacy kind bit is zero, so a reader that
   does not know about the escape matches no kind and treats the page as
   absent/unclassified, rather than misreading it. Had the kind integer been
   laid into the **low** bits of `flags` instead, kind 20 (`0b10100`) would
   have read as `POSTING|TRGM` — a wrong answer rather than a refusal.
   `test/hegel/test_pagekind.c` transcribes that first-match rule and checks it
   against every allocated kind, exhaustively over all 2¹⁶ flag words, but it
   proves only that narrower, secondary property — not "a v5 `.so` cannot
   misread a v6 index," which is the version gate's job.

3. **The shipped kinds do not move.** A v6 build keeps writing the legacy bitmap
   for all ten existing kinds, so a lexical page written by v6 is **byte-identical**
   to one written by v5. Only genuinely new kinds use the escape. That is why v6
   needs no page rewrite, and why the existing `flags & WEAVE_DOCLEN`-shaped hot
   paths kept working unchanged.

The cost, stated plainly: **an extended kind is not a bit, so nothing may test one
with a bitwise AND.** `flags & WEAVE_VMETA` compiles and is always false. Every
kind is read through `weave_page_kind_decode()` / `WeavePageGetKind()` /
`WeavePageHasKind()`, and `WEAVE_FREED` — which is a *state*, ORed on top of a
kind and valid under both encodings — is read through `WeavePageIsFreed()`.

That cost is why the v6 change swept every reader rather than only the writers.
The precedent: L17 put a flag in the high bits of `WeaveDoclenBlockHdr.count` and
one validator kept comparing the **raw** count, which silently produced an empty
page directory — and the only symptom was slightly wrong BM25 scores and a
pagination disagreement at row 300. The readers found and converted in v6 were
the four `flags & WEAVE_DOCLEN` sidecar probes in `src/am/am.c`, the two
`WEAVE_FREED` tests in the recycle gate, and — the one that would have repeated
L17 exactly — the bucket loop in `src/am/amsize.c`, whose "first matching bit
wins, and `WEAVE_FREED` must therefore be listed first" contract has no meaning
once the kind is an integer. It now decodes the kind once and tests freed-ness
separately.

## 3. Metapage — v6, EXISTS

Block 0. `WeaveMetaPageData` (`include/weave/am.h`).

| field | type | offset in contents | meaning |
|---|---|---:|---|
| `magic` | `uint32` | 0 | `WEAVE_MAGIC` = 0x42324635 |
| `version` | `uint32` | 4 | 6 today; 3, 4 and 5 still read |
| `ndocs` | `double` | 8 | corpus N: live segments + pending, minus tombstones |
| `sumdoclen` | `double` | 16 | corpus Σ doclen, giving avgdl |
| `nsegments` | `uint32` | 24 | live bolt descriptors |
| `pendinghead` / `pendingtail` | `BlockNumber` | 28 / 32 | pending chain, tail for O(1) append |
| `npending` | `uint32` | 36 | unmerged documents |
| `segs[128]` | `WeaveSegMeta` | 40, stride 56 | the bolt directory |
| `generation` | `uint32` | 7208 | bumped on any directory change; see §7 |

The offsets are given because they are load-bearing: `t/010_format_v6_upgrade.pl`
manufactures a pre-v6 metapage by rewriting exactly the version word and the
per-bolt `chandesc`, and the whole head (`magic` … `npending`) is at identical
offsets in **every** format generation, which is what makes the pending-append
path in `weave_insert()` safe without an upcast.

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

**The versioned reader.** `weave_meta_from_page()` in `src/am/am.c` deserializes
*any* supported version into the current in-memory struct, and every reader goes
through it — casting the page to `WeaveMetaPageData` directly is the 1.5.0 upgrade
bug. It carries three read-structs, `WeaveMetaPageDataV3`, `WeaveMetaPageDataV5`
and the live one, plus `StaticAssertStmt`s on every layout relationship the branch
logic depends on. `weave_meta_upcast_page()` rewrites a pre-v6 metapage into the
current shape in place, under `GenericXLog`, and is called by every one of the six
paths that mutates the directory.

## 4. Bolt descriptor — v6, EXISTS

`WeaveSegMeta`, one per live bolt. 56 bytes, and the offsets matter (see §3):

| field | offset | meaning |
|---|---:|---|
| `dictstart` | 0 | first dictionary page |
| `trgmstart` | 4 | first trigram directory page, or Invalid |
| `livedocs` | 8 | first tombstone blob page, or Invalid (a `WEAVE_TRGM_DATA` page; see §2) |
| — | 12 | padding for the `double`s that follow |
| `ndocs` | 16 | documents including tombstoned |
| `sumdoclen` | 24 | Σ doclen in this bolt |
| `nterms` | 32 | distinct terms |
| `ndeleted` | 36 | tombstoned docs, for merge accounting |
| `livedocslen` | 40 | serialized size of the tombstone blob |
| `dictindexstart` | 44 | sparse block index over dict pages, or Invalid |
| `doclenstart` | 48 | doclen sidecar (v4), or Invalid for a v3 bolt with inline doclen |
| `chandesc` | 52 | **v6**: the bolt's `WEAVE_CHANDESC` page, or Invalid = "lexical only" |

`doclenstart` being per-bolt is what makes dual-read work: a v3 bolt and a v4 bolt
can coexist in one index and each is read according to its own descriptor.
`chandesc` follows that pattern exactly — a pre-v6 bolt and a v6 lexical-only bolt
are indistinguishable to a reader, both saying "lexical only".

**`chandesc` did not change the `segs[]` stride, and §6 predicted that it would.**
It lands at offset 52, inside the four bytes of tail padding the struct already
carried for its `double` members, so `sizeof(WeaveSegMeta)` stays 56 and the offset
of the metapage's `generation` does not move either. Two consequences:

- The version bump is still required. Those four bytes are meaningful now, and an
  older binary would read them as padding and **leak one page per merged bolt**,
  never freeing a descriptor page. Refusing (§8) is correct.
- The versioned reader exists anyway, with a `StaticAssertStmt` on the stride.
  The next field added to `WeaveSegMeta` will not fit the padding, and at that
  point the assert fails the build and the v3-style per-segment expansion loop is
  already the shape to copy.

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

**Doclen sidecar (v4/v5).** One quantized length byte per doc plus a FOR docid
column, in 128-doc blocks. Bought a 4.7× index-size reduction (1421 MB vs
6729 MB on the 2.19 M-article corpus).

The docid column's encoding changed in **v5** and each BLOCK self-describes which
it uses, via the `WEAVE_DOCLEN_ABS` flag in the high bits of
`WeaveDoclenBlockHdr.count` (so always read that field through
`WEAVE_DOCLEN_COUNT()` — it is not a bare count):

| | v4 | v5 |
|---|---|---|
| stored value | gap from the predecessor | **absolute offset from the block's `first_docid`** |
| entry *i*'s docid | prefix sum of gaps 0..*i* | `first_docid + value[i]`, one `weave_for_get()` |
| in-block lookup | unpack all 128 + prefix-sum, then search | **~7-step binary search on the packed column** |
| coded width, 128 docs over ~6,400 docids | ~6 bits | ~13 bits |

The flag is per *block* rather than per index because an index built by ≤ 0.5.0
keeps its v4 pages after an upgrade while later inserts and merges write v5 ones,
so both encodings coexist in one relation. Readers must handle both; the metapage
version gate (`weave_meta_validate`) is what stops an *older* `.so` from meeting a
v5 block, and the flag's placement in `count` means such a reader fails closed
(the block looks over-long and is skipped) rather than misreading offsets as gaps.

Why: the v4 decode was **~72% of a ranked scan** — a single-term scan probes
ascending docids with stride `ndocs/df`, so a 128-docid block covers ~2.6
candidates and was fully unpacked to answer each one
(`bench/RESULTS_SCAN_PROFILE.md`, task **L17**). Task L3 asserted the sidecar
imposed a whole-sidecar decode per scan and was **closed as already satisfied** —
a measured `doclen_sidecar=on/off` A/B is identical in latency at 625 MB vs
859 MB, so the sidecar is a size win whose cost was this cursor.

## 6. Channel descriptors — v6, EXISTS

The problem v6 solves: a bolt must **self-describe which wefts it carries**, so
that an index built without a vector channel costs literally zero vector bytes
rather than empty structures, and so a reader never infers geometry from a GUC
that may have changed since the build.

`WeaveSegMeta.chandesc` names a `WEAVE_CHANDESC` page (§4). Layout, at
`PageGetContents(page)`, from `include/weave/chandesc.h`:

```c
typedef struct WeaveChanDescPageData
{
	uint32		magic;		/* WEAVE_CHANDESC_MAGIC = 0x57434431, "WCD1" */
	uint16		version;	/* WEAVE_CHANDESC_VERSION = 1 */
	uint16		nweft;		/* 1 .. WEAVE_MAX_WEFTS (32) */
	uint32		reserved;	/* must read as zero */
	WeaveChannelDesc weft[nweft];
} WeaveChanDescPageData;

typedef struct WeaveChannelDesc		/* 12 bytes */
{
	uint16		kind;		/* WeaveWeftKind; never 0 */
	uint16		attnum;		/* 1-based index attribute, or 0 */
	uint32		flags;		/* WEAVE_WEFT_F_*; unknown bits are an ERROR */
	BlockNumber root;		/* first page of the weft */
} WeaveChannelDesc;
```

`WeaveWeftKind` is `{ LEXICAL=1, VECTOR=2, FUZZY=3, DOCVALS=4, CGRAM=5, DOCLIST=6 }`.
`LEXICAL`, `FUZZY` (v7), `VECTOR` (v8) and `CGRAM` (Z8) are written; `DOCVALS` is
reserved so the remaining channel cannot collide, which is the same reason the
page-kind ids are allocated in one table (§2).

Structural rules the decoder enforces, each of which exists for a reason:

- **`kind` = 0 is invalid.** A zeroed or torn page must not validate as "one weft
  of kind 0".
- **Descriptors are strictly ascending by `(kind, attnum)`.** This makes duplicate
  detection a single comparison against the predecessor — no O(n²) scan and no
  scratch array in a validator running on hostile bytes — and lets a reader binary
  search once the array grows. A caller that trusted a non-ascending array could
  score one bolt's vector weft twice.
- **`root` must be a real block**, never Invalid and never block 0. A weft with no
  root has no descriptor: the point of v6 is that an absent weft costs zero bytes.
- **No two descriptors may name the same `root`.** §8 item 5 records that a sibling
  project shipped a chain-overlap bug four separate times. The structural defence
  here is that a root is an **explicit `BlockNumber` written by the code that
  allocated the chain** — there is no running offset sum for a term to be omitted
  from — and this check plus `weave_check()`'s relation-wide double-mark catches
  the residual.
- **Unknown `flags` bits and a nonzero `reserved` are errors,** not ignored. A
  reader that ignores a flag it does not understand is doing a best-effort read of
  a format it does not understand.

`weave_chandesc_check()` is backend-independent for the same reason
`include/weave/docvalid.h` is: a validator that only runs inside a backend cannot
be fuzzed. `test/fuzz/fuzz_chandesc.c` runs it against 856,784 well-formed,
truncated, single-field-corrupted and fully random images under ASan+UBSan, sizing
the buffer *exactly* to the declared length so a one-byte overread is a hard
failure, and independently re-verifies the postcondition of every accepted image.
It ships with a planted-bug variant (`FUZZ_NO_ARRAY_GUARD`) that must abort, so a
toothless harness fails instead of passing vacuously.

### Every v6 bolt gets a descriptor page, including a lexical-only one

`chandesc` is `Invalid` on a **pre-v6** bolt and set on every bolt written by v6,
even one carrying nothing but the lexical weft. The alternative — write the page
only once a second channel exists — would leave the writer, the WAL path, the free
path, the validator and `weave_check()`'s reachability rule entirely unexercised
until the vector channel lands, which is the failure mode this gate exists to
prevent. The cost is **one 8 KB page per bolt**, and it buys one thing v4 never
recorded: which index attribute the lexical weft indexes.

### Where §6 as originally specified was wrong or underspecified

- **"Adding a field to `WeaveSegMeta` changes the `segs[]` stride."** It does not,
  for this field: `chandesc` fits the existing tail padding. See §4.
- **"`WeaveMetaPageDataV4` alongside `WeaveMetaPageDataV5`."** Off by one
  generation — the break is v5 → v6, so the read-struct is
  `WeaveMetaPageDataV5` alongside the live v6 struct.
- **"a v4 bolt has `chandesc = InvalidBlockNumber`."** A pre-v6 metapage has
  *padding* there, not `InvalidBlockNumber`. Every historical writer happened to
  zero it, and zero is block 0 — the metapage — so a reader that trusted the bytes
  would treat the metapage as a bolt's descriptor page. The versioned reader
  overwrites the field unconditionally for `version < 6` instead of trusting it.
- **`WeaveChannelDesc.kind` did not say which enum.** The obvious reading collides
  with `WeaveChannelKind` in `include/weave/channel.h`, whose `WEAVE_CH_LEXICAL`
  is **0** and which enumerates *scan strategies* (`WEAVE_CH_VECTOR_SCAN` and
  `WEAVE_CH_VECTOR_GRAPH` are two ways to scan one stored weft; `WEAVE_CH_POSITION`
  is a scan over the lexical weft's fourth column, not a weft at all). Putting a
  scan strategy on disk would have been a mistake, and a 0 value would have made a
  zeroed page valid. v6 introduces a distinct on-disk `WeaveWeftKind`.
- **The page had no header.** §6 specified "an array of `WeaveChannelDesc`" with no
  magic, version or count, which is not something an untrusted-bytes decoder can
  validate. v6 adds `WeaveChanDescPageData`.

### The fuzzy weft — v7, EXISTS

`WEAVE_WK_FUZZY` is the second weft kind a bolt can carry, and it is the first
thing v6's design was actually asked to do: **`WeaveSegMeta` gained no field.**
The trie's root is an ordinary descriptor entry, so a bolt that has no trie —
every pre-v7 bolt, and any bolt whose vocabulary is empty — costs zero bytes for
it, including zero descriptor slots.

Three decisions worth recording next to the format rather than only in
`doc/specs/FUZZY_CHANNEL.md` §3:

- **The chain carries the image and nothing else, so there is no length field
  anywhere.** Not in the descriptor, not in a per-chain header. The image length
  is the sum of the chain pages' payloads, and `weave_surftrie_open()` refuses any
  image whose length disagrees with what its own counts imply — so a torn chain is
  caught with no second source of truth to keep in step. The reader computes the
  length by walking the chain *before* it allocates, which also means the
  allocation is bounded by pages that exist rather than by a count read out of a
  possibly-corrupt header.
- **It is not written with `weave_write_blob()`,** even though §3.3 of the fuzzy
  spec said it would be. That writer hard-codes `WEAVE_PK_TRGM_DATA` (which is
  why the livedocs blob lands on trigram-data pages and `WEAVE_PK_LIVEDOCS` has no
  writer at all, §2), and `weave_read_blob()` validates no page kind: it follows
  `nextblk` and trusts `pd_lower`. `weave_write_surf()` / `weave_read_surf()` in
  `src/am/am.c` are the pair, and the reader checks `WeavePageHasKind(page,
  WEAVE_PK_SURF)` on every page.
- **`weave_free_segment()` is now driven by the descriptor** for every weft that
  is not named by a `WeaveSegMeta` field. A free path written as a hard-coded list
  of `WeaveSegMeta` chains cannot see a weft that lives only in the descriptor,
  and the symptom is a leak of the whole structure on every merge with nothing but
  `weave_check(deep)` to notice. The next weft is freed by that code as written.

Measured cost, on a 200,000-row build of short text (`weave_index_size_detail()`
plus `weave_surf_stats()`, one bolt after a full merge):

| vocabulary | trie image | on disk | share of index | share of dictionary | per term |
|---:|---:|---:|---:|---:|---:|
| 274,244 terms | 1,514,433 B | 186 pages (1,523,712 B) | 6.89 % of 21 MB | 17.1 % | 5.52 B |
| 74,244 terms | 410,061 B | 51 pages (417,792 B) | 5.15 % of 7,928 kB | 17.1 % | 5.52 B |

The per-term figure is the number to carry forward: **~5.5 bytes per vocabulary
term**, and it is flat across a 3.7× change in vocabulary size. The share of the
index is not flat and is not a property of the trie — it falls as the corpus grows
past the point where the vocabulary saturates (Heaps' law), because the postings
keep growing and the trie does not.

### The document list — v12, SPECIFIED 2026-10-05 (doc/GAPS.md G77, G78, G80, G81)

**The problem.** Before v12 a bolt had no enumeration of the documents it holds other
than its posting lists, so a document with no posting was invisible to everything that
asks "which documents are in this bolt?". Three silent wrong answers followed: VACUUM
could not tombstone a zero-term document, so a recycled ctid inherited its docvalue and
vector lane (G80); the NOT universe omitted it (G78); and a row whose lexical column is
NULL was not indexed at all, so a docvalues restriction or a vector ORDER BY answered by
the index dropped it (G77). A fourth defect sat underneath: a bolt whose documents had
no terms at all was not written (G81).

**The structure.** A sixth weft kind, `WEAVE_WK_DOCLIST = 6`, `attnum = 0` (it is not
tied to an index attribute), emitted LAST on the descriptor page because 6 is the
highest kind (§6: strictly ascending `(kind, attnum)`). Its root is the first page of a
chain of `WEAVE_PK_DOCLIST` pages (§2, id 35) that carries one image and nothing else —
the `WEAVE_PK_SURF` / `WEAVE_PK_DOCVALS` chain shape, with the same walker discipline
(kind checked on every page, length = sum of page payloads, measured before allocating).
`WeaveSegMeta` gained no field: it has no free byte, which is why this is a weft.

Image (`include/weave/doclist.h`, backend-independent so it can be fuzzed):

```c
typedef struct WeaveDocListHeader     /* 40 bytes */
{
	uint32		magic;      /* WEAVE_DOCLIST_MAGIC = 0x57444c31, "WDL1" */
	uint16		version;    /* WEAVE_DOCLIST_VERSION = 1; anything else is refused */
	uint16		flags;      /* WEAVE_DOCLIST_F_COMPLETE; unknown bits are an ERROR */
	uint64		ndocs;      /* cardinality of ALL, >= 1 */
	uint64		nnull;      /* cardinality of NULL, <= ndocs */
	uint32		alllen;     /* serialized length of ALL */
	uint32		nulllen;    /* serialized length of NULL; 0 iff nnull == 0 */
	uint32		reserved;   /* must read as zero */
	uint32		reserved2;  /* must read as zero */
} WeaveDocListHeader;
/* then: ALL  = the raw sparsemap buffer (sm_get_data/sm_get_size, the livedocs
 *              blob's encoding) of every docid the bolt holds, at offset 40
 *       NULL = the same for the NULL-document docids, at 40 + ALIGN8(alllen)
 * total image length == 40 + ALIGN8(alllen) + nulllen, exactly. */
```

Docids are the global tid-derived ones (`weave_tid_to_docid()`), the same space the
tombstone sparsemap uses. The validator checks magic, version, flags, reserved words,
the exact total length, that each set is at least a sparsemap header long, that each
set's membership -- counted with the SAME `sm_next_member()` walk the decoder uses,
over a scratch copy because `sm_open()` rewrites an invalid buffer -- equals its
declared count, `nnull <= ndocs`, `NULL ⊆ ALL` (one merge of the two walks), and
`ndocs >= 1`. A bolt with no documents carries no document list.

Two properties of the vendored sparsemap shaped this, both found by
`test/hegel/test_doclist.c` and worth knowing before anyone else builds on it:
`sm_create_from_array()` is a use-after-free once the map grows past its initial
1 KiB (it `sm_free()`s the pre-`realloc` pointer), so the encoder uses
`sm_create()` + `sm_add_many_grow()`; and `sm_cardinality()` can DISAGREE with an
`sm_next_member()` walk on a buffer `sm_validate()` accepts (one flipped byte: 1,973 vs
1,909), so a validator that counted with one and a decoder that iterated with the
other would accept an image the decoder cannot honour.

**`WEAVE_DOCLIST_F_COMPLETE`.** Set when the list was produced by a v12 writer from
the heap (CREATE INDEX) or from pending items whose page kind is `WEAVE_PK_PENDING_V12`:
then ALL is every row the bolt indexes, NULL-document rows included. A merge output is
COMPLETE only if every input was. A flush that folds any pre-v12 pending item writes a
list WITHOUT the flag (the item's NULL-document rows were never written down, so the
list cannot claim them). The flag is what a reader may use to argue "this bolt's
documents are exactly ALL" — G76's padding completeness argument keys off "every live
bolt COMPLETE and every pending page `WEAVE_PK_PENDING_V12`". A list without the flag is
still exact about the documents the bolt holds; it just does not vouch for rows that
were never indexed.

**The docset of a bolt** (one function, `weave_segment_docset()`, used by every reader
below):

- a bolt with a document list: ALL;
- a pre-v12 bolt (no list): postings ∪ docvalues docids ∪ vector warp docids. The union
  matters. With postings alone, a merge of an old bolt would carry a zero-term
  document's docvalue (the docvalues merge carries every non-tombstoned docid) into an
  output whose list does not contain it, and G80 would become permanent in the merged
  bolt.

**Readers.**

- `weave_bulkdelete()` asks the vacuum callback about every docid in the docset
  (closes G80, for pre-v12 bolts too).
- The NOT universe and the fuzzy/regex no-trigram fallback are ALL minus NULL
  (closes G78). A pre-v12 bolt keeps the posting-derived universe and keeps G78 until a
  merge or REINDEX rewrites it.
- `@@@` never matches a NULL document. A NULL document has no postings, so a positive
  query cannot reach it; the universe subtraction above is what keeps it out of `!q`.
- `ndeleted` counts tombstoned docids that are NOT NULL documents, so
  `ndocs - ndeleted` stays the BM25 corpus N.

**Writers.** Every path that writes a bolt writes its list: the build flush, the
pending flush, the oversized-insert bolt, and the merge (output = ∪ (docset_i \
tombstones_i), NULL = ∪ (NULL_i \ tombstones_i)). VACUUM's compaction is a merge and
needs nothing else. The list's root is recorded on the descriptor page, which is written
after every chain and published by the metapage record that adds the bolt, so a crash
can leak a list but cannot publish a bolt without one or with someone else's.

**NULL-document rows (G77, maintainer decision 2026-10-05, option 1).** The build
callback and `weave_insert()` no longer skip a row whose lexical column is NULL. It gets
a docid in ALL and in NULL, its docvalue, and its vector lane (a live lane if its vector
is non-NULL), and no postings. It does not count in `ndocs`, `sumdoclen` or the
metapage N: a NULL document is not a document for IDF. A pending NULL document is an
item with `doclen == 0` (no wdoc bytes) on a `WEAVE_PK_PENDING_V12` page — the v11 item
layout, under a new kind because a v11 `.so` would hand a zero-length document to
`weave_doc_is_valid()`. It counts in `npending` but not in the metapage N.

**G81.** A bolt with documents but no terms is written with ONE EMPTY dictionary page
(`pd_lower` at the contents start), so `dictstart` is valid and the 26 places that read
`dictstart == InvalidBlockNumber` as "consumed slot, skip it" keep meaning only that.
Every dictionary walker already handles a page with no entries.

**What is NOT fixed in place.** An index written before v12 keeps its pre-v12 bolts
until a merge rewrites them. Their G78 behaviour remains until then; G80 is fixed for
them immediately (the union docset); their unindexed NULL-document rows (G77) can only
come back by REINDEX, because no merge can invent rows that were never indexed, and
their merged output is therefore not COMPLETE.

`weave_check()` invariants: `doclist_valid` (every list validates and its root chain is
`WEAVE_PK_DOCLIST` pages), `doclist_covers_postings` (`deep`: every posting docid, every
docvalues docid and every warp docid is in ALL; NULL documents have no posting), and
`doclist_coverage` (informational: bolts with a list, and how many are COMPLETE).
Fuzz target: `test/fuzz/fuzz_doclist.c`. Property test: `test/hegel/test_doclist.c`.

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
- A segment writer that does not hold the maintenance mutex (the oversized INSERT,
  a build) holds the **segment-write lock** (`WEAVE_SEGWRITE_LOCKBLK`, share) from
  its first allocation to its publish record, so VACUUM's reclaim of stranded pages
  can wait it out. Lock order is segment-write lock before maintenance mutex, never
  the reverse. The argument is in §10, "Pages a crash strands between write and
  link".

## 8. Compatibility policy

1. **Versions read: v3 … v12. Version written: v12.** `WEAVE_VERSION` is 12.
   The range is one constant pair in `include/weave/am.h`
   (`WEAVE_VERSION_DOCLEN_INLINE` = 3 is the floor, `WEAVE_VERSION` the ceiling)
   and `weave_check_meta()` is the single gate; `weave_check()` reports the range
   it read so an operator can see it without reading source.

   What each version means to a reader, and what is *per-object* rather than
   per-index:

   | version | what changed | how a reader tells |
   |---|---|---|
   | 3 | inline per-posting doclen | `segs[i].doclenstart == Invalid` |
   | 4 | doclen sidecar, gap-coded docid column | per-**block** `WEAVE_DOCLEN_ABS` clear |
   | 5 | sidecar docid column is absolute offsets | per-**block** `WEAVE_DOCLEN_ABS` set |
   | 6 | per-bolt weft descriptors; extended page-kind space | per-**bolt** `segs[i].chandesc`; per-**page** `WEAVE_PAGE_KIND_EXT` |
   | 7 | the fuzzy weft (SuRF trie over the vocabulary) | per-**bolt**: a `WEAVE_WK_FUZZY` entry on the bolt's descriptor page |
   | 8 | the vector weft (`WEAVE_PK_VMETA` + directory + code strips) | per-**bolt**: a `WEAVE_WK_VECTOR` entry on the descriptor page |
   | 9 | a pending item carries the row's `wvec` | per-**page**: kind `WEAVE_PK_PENDING_V9` |
   | 10 | a pending item carries the row's raw `gram_ops` text, and a merged bolt may carry a cgram weft | per-**page**: kind `WEAVE_PK_PENDING_V10` |
   | 11 | a pending item carries the row's docvalues value | per-**page**: kind `WEAVE_PK_PENDING_V11` |
   | 12 | every bolt carries a DOCUMENT LIST; NULL-document rows are indexed; a pending item may be a NULL document | per-**bolt**: a `WEAVE_WK_DOCLIST` entry on the descriptor page; per-**page**: kind `WEAVE_PK_PENDING_V12` |

   **v8, v9 and v10 changed no metapage struct either, and bumped anyway**, for
   v7's reason and one more. v9 and v10 grew the pending ITEM, so an older `.so`
   would parse the newer page with the shorter stride, hand
   `weave_doc_is_valid()` garbage, and WARN-and-skip documents the metapage has
   already counted into `ndocs` — a silent under-count, not an error. Refusing the
   index is correct.

   **v7 changed no struct, and bumped the version anyway.** A v6 metapage and a v7
   metapage are byte-identical; a v6 `.so` would read a v7 bolt's descriptor page
   without complaint. What it would *not* do is free a weft kind it has never
   heard of, so every merge under that binary would leak the entire trie —
   unreachable, unflagged, and reclaimable by nothing short of a REINDEX. That is
   the same argument item 2 below makes for v5 → v6 ("that binary would read
   `chandesc` as padding and leak one page per merged bolt"), and it reaches the
   same conclusion: refusing is correct. The lesson generalizes — **a new weft kind
   needs a version bump even when it moves no field,** because the discriminator
   that matters is "does the reading build know how to free this?", not "can it
   parse this?".

   Every one of those discriminators is per-object, never per-index, and that is
   deliberate: an index upgraded in place keeps its old objects while later inserts
   and merges write new ones, so **one relation legitimately holds several
   generations at once** and a per-index version cannot describe it. No format
   change since v3 has required a REINDEX, and v6 does not either.

2. **An unknown or inconsistent version is an `ERROR`, never a best-effort read.**
   A best-effort read of a format we do not understand returns wrong answers,
   which is strictly worse than refusing. pg_turbovec's strict-versioning policy
   is the model here. Concretely:

   - `version < 3` or `version > WEAVE_VERSION`, or a wrong `magic`:
     `ERRCODE_INDEX_CORRUPTED` with the supported range and a REINDEX hint
     (`weave_check_meta()`, called on every metapage read).
   - A v6 index met by an older `.so` therefore **refuses**, which is the correct
     outcome and not merely conservative: that binary would read `chandesc` as
     padding and leak one page per merged bolt. A v7 index met by a v6 `.so`
     refuses for the analogous reason: it would leak the whole SuRF trie per
     merged bolt (item 1's table).
   - A pre-v6 metapage that nonetheless names a descriptor page is inconsistent —
     it cannot arise from any writer — and `weave_check()` reports it as a violated
     invariant (`chandesc_version_consistent`) rather than following the pointer.
   - A `WEAVE_CHANDESC` page that fails any structural rule in §6 is an `ERROR`
     with a specific `errdetail` naming which rule; there are fourteen distinct
     codes (`WeaveCdError` in `include/weave/chandesc.h`), because "which of the
     fourteen ways this page is wrong" is the
     difference between a diagnosable corruption report and "index is corrupted".
   - `weave_check()` is the one caller that reports instead of throwing, which is
     why the descriptor reader returns a code and a thin wrapper
     (`weave_chandesc_required()`) does the `ereport`. Catching an `ERROR` without
     a subtransaction to build a report would be the worse trade.

3. On-disk bytes are **not trusted**. Every decoder validates before use, and
   every on-disk structure gets a fuzz target in `test/fuzz/`. A corrupt page must
   produce a clean `ERROR`. For v6: `test/fuzz/fuzz_chandesc.c` (the descriptor
   page) and `test/hegel/test_pagekind.c` (the kind space, exhaustive over all
   2¹⁶ flag words).
4. Additive fields go **after** existing ones when the containing struct is not
   an array element, so offsets do not move (see `generation`, §3). Fields added
   to an array element require a version bump and a versioned reader — even when,
   as with `chandesc`, the field happens to fit existing tail padding and the
   stride does not move (§4).
5. **Chain offsets must be COMPUTED from one place, never hand-summed per call
   site.** pg_turbovec has now hit the same bug **four times**: a running
   chain-offset sum omitted one count field, so a build wrote one chain on top of
   another's data (v2.7.0 fixed the fourth instance, `set_ivf_chains` omitting
   `bq_mean_count`, found by audit rather than a field report). v6 answers this
   structurally rather than by care: a weft's root is an **explicit `BlockNumber`
   written by whichever code allocated the chain**, so there is no running sum for
   a term to be omitted from. The residual risk — two descriptors naming the same
   root — is checked on every descriptor read, and `weave_check(deep => true)`
   marks every reachable block and reports any block reached from two different
   chains (`chains_do_not_overlap`).
6. **Backward compatibility has a test, not a plan.** `t/010_format_v6_upgrade.pl`
   builds a v6 index over 20,000 rows, records its answers, stops the server,
   rewrites the metapage into exactly the bytes a v5 build would have written,
   restarts, and asserts the answers are byte-identical — then upgrades in place by
   inserting and merging and asserts they still are. Before v6 the project's own
   note in `t/009_doclen_sidecar.pl` said the equivalent v3 check was "validated
   out-of-tree in the release qualification (it needs two `.so` builds)", which is
   a gate nobody runs. One `.so` is enough if the old image is *manufactured*
   rather than built, and the v5 → v6 delta is small enough to manufacture exactly.
7. **A weft-internal version is a per-object discriminator, and bumping it does not
   by itself bump `WEAVE_VERSION`.** The vector weft carries its own version on its
   `WEAVE_PK_VMETA` page (`WEAVE_VMETA_VERSION`): 3 since G27's `firstpage`, **4
   since 2026-10-05**, when a code page may carry its block's centroid strip after
   the last lane strip (`doc/specs/VECTOR_CHANNEL.md` §7.1, "v4"). This build reads
   v3 and v4 wefts and writes v4; an upgraded index holds both until a merge rewrites
   the old bolts, and a merge's output is always v4.

   The metapage stays at its version, by item 1's rule -- "does the reading build
   know how to free this?" -- applied to the OLDER binary meeting a v4 weft:

   - **It cannot misread one.** `vec_meta_read()` refuses any `version` but its own
     (an `ERROR`-class refusal, `ERRCODE_INDEX_CORRUPTED`, on scan), so a v3-only
     `.so` never parses a packed page.
   - **It cannot leak one.** A weft it cannot open makes `weave_vec_merge_geom()`
     refuse every merge group containing that bolt, so the bolt is never a merge
     input and its pages are never handed to `weave_vec_free_weft()`. Nothing else
     frees a weft page by page (compaction is a merge; bulkdelete rewrites only the
     livedocs bitmap).
   - **It cannot under-count.** No pending item, metapage field or `segs[]` stride
     changed.

   That is the opposite of v7's situation (an older binary would have *merged* and
   leaked a weft kind it did not know), and the same as the VMETA 2 -> 3 change for
   G27, which also left `WEAVE_VERSION` alone. The cost of not bumping is that the
   older binary fails at query time on a v4 bolt rather than at metapage read: the
   error names the bolt and says the weft is "a format version this build does not
   read". Downgrading a binary over an index that a newer binary has written to is
   not a supported operation in either case.

## 9. Invariants `weave_check()` must verify

One per line, each mechanically testable. `weave_check(regclass, deep boolean)`
returns one row per invariant — `(invariant, ok, detail)` — rather than throwing on
the first violation, because a corruption test needs to assert that a *specific*
injected fault is detected while the others still pass, and an operator wants the
whole list. The exception is the metapage version gate, which throws: with the
version unknown every later invariant would be reading `segs[]` at an offset it
cannot justify.

**Implemented** (`src/am/amcheck.c`), with the row name each reports under:

- `metapage_version_recognized` — magic and version are recognized. Throws if not.
- `nsegments_bound` — `nsegments <= WEAVE_MAX_SEGMENTS`.
- `segment_roots_have_expected_kind` — every `segs[i]` chain is within relation
  bounds, every page on it decodes as the kind the directory implies, and no page
  on a live chain is flagged freed.
- `page_kinds_decodable` — every page decodes to a **known** kind. This is the
  invariant the v6 kind space owes: under the old flat bitmap an unrecognized page
  was one with no bit set, and now it is also one with a reserved bit set, two kind
  bits set, or an extended id outside the allocated range.
- `uninitialized_page_count` — informational: how many pages are
  extended-but-never-initialized. Not corruption on its own (a crash between the
  extend and the `GenericXLog` commit leaves one, which is a normal recoverable
  state), and a single check has no history to distinguish that from a write
  path steadily losing pages — only a *growing* count across repeated checks
  would mean the latter, which is why this is reported rather than asserted.
- `chandesc_reachable` — every bolt that names a descriptor page has one that is in
  bounds, is a `WEAVE_CHANDESC` page, and passes every §6 structural rule.
- `chandesc_roots_agree` — the descriptor's `LEXICAL` weft root equals the bolt's
  `dictstart`. The cross-check between the two places a bolt's geometry is written
  down; when the vector weft lands, its root must equal the `WEAVE_VMETA` page and
  this is where that gets asserted.
- `chandesc_version_consistent` — no pre-v6 metapage names a descriptor page.
- `chandesc_coverage` — informational: how many bolts self-describe. On an index
  upgraded in place this is 0 until the first merge, which is what makes
  "read a pre-v6 index with the new code" a meaningful test.
- `surf_trie_matches_dictionary` — **(fuzzy, task Z3)** the SuRF trie's membership
  is exactly the bolt's dictionary term set. Both directions, and it is the only
  invariant here that compares two independent on-disk structures against each
  other rather than checking one against its own header. See the fuzzy entry under
  "Still owed" below for what makes both directions necessary; the mechanics are a
  merge of two ascending streams (`weave_surftrie_enumerate()`'s lexicographic DFS
  against a dictionary page walk), which settles set equality in one linear pass
  with O(1) memory — materializing either side would be a vocabulary-scale
  allocation inside a validator. A truncated terminal is allowed to cover a
  nonempty run of dictionary terms sharing its bytes, which is the format's
  deliberate one-sided error and not a hole in the check.
- `surf_coverage` — informational: how many bolts carry a fuzzy weft. 0 on an
  index upgraded in place until the first merge rewrites a bolt, the same shape as
  `chandesc_coverage` and for the same reason.
- `pages_reachable_or_freed` (`deep`) — every page is reachable from the metapage,
  or flagged `WEAVE_FREED`. An unreachable unflagged page is a leak. `deep` because
  it walks every chain and every page. Note that nothing short of a REINDEX
  reclaims a page in that state, since it is neither on a chain nor in the FSM.
  **When it fires, `weave_page_info(idx)` is the follow-up**: the invariant row can
  only carry a count and a first offender, and a leaked page's *kind* is what names
  the write path that left it. Both read the same traversal
  (`wvck_mark_reachable()`), deliberately — two walks would be two answers to one
  question, and this invariant is exactly what they would disagree about. A leak is
  the row with `reachable = false`, `freed` not true, `uninitialized = false`.
- `chains_do_not_overlap` (`deep`) — no block is reached from two different chains.
  §8 item 5.

**Still owed — task M6.** `doc/PRODUCTION_READINESS.md` gate 4 previously claimed
weave_check() "covers the inherited lexical ones only"; in fact there was no
`weave_check()` at all before v6, only `weave_check_meta()` on the metapage header.
The following are specified and unimplemented:

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
- Doclen sidecar: for every block, decoding the docid column under the encoding
  its `WEAVE_DOCLEN_ABS` flag declares yields a strictly ascending docid sequence
  whose first element is `first_docid`. Both encodings must yield the IDENTICAL
  sequence for the same input — asserted over 2.8 M random cases by
  `test/hegel/test_doclen_block.c`, because a disagreement produces a wrong
  document length, hence a plausible-but-wrong BM25 ranking rather than an error.
- (vector) The code chain is exactly `nblocks * pages_per_block` pages, where
  `pages_per_block` is the weft geometry's (it depends on `dim`, `bits` and the
  weft version: v4 packs the centroid onto the last lane page when it fits).
  Checked inside `vector_block_stats_match_codes`, because a page that claims no
  valid block is invisible to every per-block read.
- (vector) Every `WeaveVecBlockHdr.smax`, `maxrecnorm`, `minnorm`, `cenrad` equals
  a recomputation from the block's live lanes. See
  `bench/RESULTS_BOUND_PRUNING.md` for why `cenrad` in particular must be
  recomputed against the centroid *as decoded from its stored code*.
- (vector) Graph: out-degree `<= R`; neighbour lists sorted ascending; no edge to a
  warp position `>= nnodes`; no edge to a tombstoned node; entry point live; every
  live node reachable from the entry point. The last is the expensive check and
  the one that actually catches a bad build, so it belongs behind `deep => true`.
- (fuzzy) SuRF trie membership is exactly the bolt's dictionary term set. Both
  directions: a term the trie misses is a dropped row, and a term the trie invents
  is a wasted recheck at best. **Implemented as `surf_trie_matches_dictionary`**
  (see the list above); this entry stays because it is where the reasoning lives.
  The pure half is `weave_surftrie_check()` (`include/weave/surftrie.h`), which
  validates the image structurally and then proves, by a lexicographic DFS, that
  the trie's terminals carry ordinals `0 = ord0 < ord1 < ... < nterms` — the
  machine-checkable form of "this trie is the sorted dictionary". The other half
  is the one `weave_check()` supplies: walk the bolt's dictionary and confirm the
  term *bytes* agree, since the pure validator sees no dictionary. Note that the
  trie is a filter with a deliberate one-sided error (terms longer than
  `WEAVE_SURFTRIE_MAX_DEPTH` are truncated, see
  `doc/specs/FUZZY_CHANNEL.md` sect. 3.2), so the check is "every dictionary term is
  present, and every *exact* trie terminal is a dictionary term" -- a truncated
  terminal is allowed to cover several.

  **Why the image validator is not sufficient, demonstrated rather than argued.**
  `t/013_surf_corruption.pl` bumps the header's `nterms` by one. No section length
  depends on `nterms` (only `nslots`, `nnodes` and `nterminal` do), so the image is
  still exactly as long as its own counts imply, every popcount identity holds,
  every accelerator table still equals a recomputation, and the ordinal-range test
  only got looser — both validation layers accept it. The same test then sets the
  label byte of the *last* slot to `0xFF`, which preserves "labels strictly
  ascending within a node" (`0xFF` is the largest byte and that slot ends its
  node) and changes no bitmap, so that image is structurally perfect too and
  semantically a different vocabulary. Only the comparison against the dictionary
  sees either one.

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

### A durability gap GenericXLog does not close by itself — FIXED 2026-09-12

100 % GenericXLog guarantees that every page change is *described* by a WAL
record. It does not guarantee the record has been **written to a file** when the
statement returns.

`SELECT weave_merge('idx')` writes WAL through GenericXLog but touches no heap and
no catalog, so its transaction never acquires a `TransactionId`. PostgreSQL's
`RecordTransactionCommit()` calls `XLogFlush()` only when the transaction
committed an XID, truncated a relation, or forced a sync commit — so the merge's
records were left in the WAL buffers, in shared memory, written to nothing. A
`pg_ctl stop -m immediate` then lost them.

**Fixed by `ForceSyncCommit()` in `weave_merge()` and `weave_vacuum()`**
(`src/am/amvacuum.c`). `ForceSyncCommit()` is what core uses for the same
situation. `GetCurrentTransactionId()` would also work, by making the commit a
real XID commit, but it burns an XID and moves the wraparound horizon for an
operation that modifies no tuples — a worse trade.

Covered by `t/014_merge_durability.pl`, which runs each maintenance function and
crashes **immediately**, with no XID-acquiring statement in between. Verified
wrong-before/right-after: with the `ForceSyncCommit()` calls removed, that file
fails two assertions; with them, the whole TAP suite passes.

#### Two things this section used to claim that the fix's test disproved

Recorded because both were wrong in the reassuring direction.

1. **"No wrong answers and no corruption — this is lost work, not lost data."**
   Not true in general. With the fix removed, `weave_check()` after recovery
   reports

       105 unreachable page(s) not flagged freed; first is block 1828

   so the rolled-back merge leaves pages **allocated but unreachable** — a leak the
   validator flags as a failed invariant, not a clean earlier state. The original
   claim came from observing a simpler index, where a rolled-back merge happened to
   leave nothing behind; it does not generalize to a merge that had already
   extended the relation.

2. **"`weave_vacuum()` escapes it only because it truncates the relation, which
   sets `nrels > 0`."** Half right, and the half that is wrong matters. The
   truncation *does* survive — that assertion passes even with the fix removed —
   but `weave_check()` still fails after the crash, so escaping the flush condition
   is not the same as being durable. `weave_vacuum()` needed the fix too.

The general lesson is the one this project keeps relearning: a consistency check
that passes on the case you happened to try is not the same as a guarantee, and
"lost work, not lost data" is exactly the kind of reassurance worth re-testing.

#### Why `t/012` no longer needs its WAL anchor

`t/012_surf_crash_recovery.pl` worked around the gap by following each merge with
a trivial INSERT into an unrelated table — a transaction that *does* acquire an XID,
whose commit flushes the WAL stream up to its LSN, including every earlier record.
That anchor is now redundant. It is left in place deliberately: `t/012` exists to
exercise WAL replay of the SuRF chain, and an extra durable commit does not weaken
that, whereas removing it would make `t/012` fail for a durability reason if this
fix ever regressed — duplicating what `t/014` says more directly. A `CHECKPOINT`
would also have worked and is the wrong tool: it puts the pages on disk and leaves
WAL replay, the thing the test exists to exercise, untested.

### Pages a crash strands between write and link — reclaimed by VACUUM (doc/GAPS.md G75, G72) — DESIGNED 2026-10-05

Every multi-page structure is written *before* the one record that links it, and
every replaced structure is freed *after* the record that unlinks it. A crash or
an ERROR inside either window leaves pages that are unreachable from the metapage
and not flagged `WEAVE_FREED`. `t/031` measures it: a mixed pending flush leaks
one page per record, up to 12. Answers stay right, but before this design only
REINDEX got the pages back. The class has four members. All four are reclaimed by
one mechanism:

| leaked page | how it arises | how it is reclaimed |
|---|---|---|
| written, never linked | crash/ERROR before a segment publish (flush, oversized INSERT, merge output, tombstone blob) | **freed** (flagged, stamped, FSM) |
| unlinked, never freed | crash/ERROR between the unlinking record and the last `weave_free_page()` (merge inputs, folded pending pages, old tombstone blob) | **freed** |
| flagged freed, absent from the FSM | the FSM is not WAL-logged; a crash loses recent `RecordFreeIndexPage()` calls | **re-recorded** in the FSM |
| zero page, absent from the FSM | a crash after `P_NEW` extended the file but before the page's first record reached disk | **re-recorded** in the FSM |

**The choice: reclaim in VACUUM, no format change.** In `weave_vacuumcleanup()`
and in `weave_vacuum()`, before the pending flush, a reclaim pass
(`weave_reclaim_unreachable()`) marks every page reachable from the metapage and
frees the rest. It frees a stranded page **without recording it in the FSM**, and
the next VACUUM's pass records it (it is then the "flagged freed, absent from the
FSM" row). Both choices were measured by `t/033` against a never-crashed twin.
A freed page is stamped with an XID that is still running, so nothing can reuse
it before the VACUUM ends. Worse, the live-FSM allocator stops reusing at the
first deferred page and extends for the rest of the allocation. Recording the
page at once therefore made the same VACUUM's flush extend: the excess grew by
about 350 pages per cycle. Running the pass after the flush instead left the
flush facing a stale, crash-restored FSM: about 1,300 pages per cycle.

**Rejected: (b), a "bolt being built" marker that the publish supersedes.** It
needs a new page flag or kind, which is a format change. Every page a writer
creates would carry the mark until the publish, and then something would have to
clear it. That is one more record per page, or a list of pages kept somewhere,
and either one is a second structure that must agree with the chains. It also
covers only the first row of the table above. The free-after-unlink and FSM rows
would still need a reachability walk, so (b) adds a mechanism without removing
one.

**Rejected heuristic: "an unreachable page whose LSN is older than the last
directory change is a leak".** It is unsound with two writers. Oversized INSERTs
W1 and W2 run at the same time. W1 writes its pages, W2 writes and publishes, and
W1 has not yet published. W1's pages are now older than the last directory change
(W2's publish), and they are live.

#### Which writers can run beside the reclaim

The reclaim holds the maintenance mutex `M` (`weave_maintenance_lock()`, a
heavyweight lock on page 0). Every writer, and the lock that keeps it apart from
the reclaim:

| writer | lock it holds | relation to the reclaim |
|---|---|---|
| pending flush, every merge, compaction, bulkdelete's tombstone swap, `weave_merge()`, `weave_vacuum()`, a build's finalize | `M` | excluded by `M` |
| parallel-merge workers | run while their leader holds `M` | excluded, with one exception below |
| CREATE INDEX, REINDEX (serial or parallel) | `AccessExclusiveLock` or `ShareLock` on the table, which conflicts with VACUUM's `ShareUpdateExclusiveLock` | cannot overlap a VACUUM. `weave_vacuum()` holds `AccessExclusiveLock` on the index, which the build's index lock conflicts with |
| CREATE INDEX CONCURRENTLY's build | `ShareUpdateExclusiveLock` on the table, which conflicts with VACUUM's | cannot overlap a VACUUM; its finalize takes `M` anyway |
| **oversized INSERT** (`weave_insert_oversized_as_segment()`) | `RowExclusiveLock` only, no `M` | **runs beside the reclaim.** Writes a whole bolt and then publishes it |
| **pending append** (`weave_insert()`) | `RowExclusiveLock` only, no `M` | **runs beside the reclaim.** A new pending page is written and linked from the old tail (or the metapage) in the SAME record |

So the two writers that can run concurrently are the oversized INSERT, which has a
write-before-link window, and the pending append, which has none. Two mechanisms
make the reclaim safe against them.

**1. The segment-write lock `X` and a barrier.** A heavyweight lock on page
`InvalidBlockNumber` of the index, used for nothing else (`WEAVE_SEGWRITE_LOCKBLK`).
Every segment writer that does not hold `M` takes `X` in `ShareLock` before its
first page allocation and releases it after its publish record. That means the
oversized INSERT, and also `weave_build()` (a CIC build does not hold
`AccessExclusiveLock`, so the allocator's check below needs it). Share mode does
not block other share holders, so concurrent inserters still write in parallel.
Before the reclaim takes `M`, it acquires `X` in `ExclusiveLock` and immediately
releases it. That barrier waits for every segment writer that was already in its
window. Lock order: a writer holds `X` and may then take `M` (a full directory
merges inside `weave_add_segment_with_room()`). The reclaim takes `X` only while
it does NOT hold `M`. So the order is always `X` then `M`, and no deadlock is
possible. A share request queues behind the waiting exclusive request, so inserts
pause for as long as the barrier waits. That is the length of the longest
oversized INSERT already running, the same price GIN's cleanup charges.

**2. The LSN fence `F`.** `F = GetXLogInsertRecPtr()` is read *before* the barrier.
A page is a reclaim candidate only if its `pd_lsn <= F`. A writer that takes `X`
after the barrier is unaffected by the barrier, but every page it writes gets an
LSN greater than `F`, so the reclaim never touches it. The pending append is
covered the same way. Its new page's first record gets an LSN greater than `F`
unless it was written before `F`. If it was written before `F`, it was linked by
that same record, so the walk reaches it.

#### Why the reclaim never frees a page a later publish links

The claim: when the reclaim frees page P, no backend will later link P, and no
reader is about to be handed P through the directory.

1. P is unmarked by a walk over the directory snapshot S, read under `M`. P is
   initialized, not `WEAVE_FREED`, and `pd_lsn(P) <= F`. These conditions are
   re-checked under P's exclusive buffer lock, and P is freed under that same
   lock.
2. P was last written before `F`. So P was not written by any writer that
   started after the barrier.
3. Writers holding `M` are excluded for the whole pass, so P is not part of an
   in-flight flush, merge or swap. Every structure they published is in S.
4. A segment writer holding `X` that was in its window at the barrier has
   published or aborted before the barrier returns. If it published, its bolt is
   in S, because S is read after the barrier. If it aborted, its pages are leaked
   and freeing them is correct.
5. A pending page written before `F` was linked by its own record, so it is
   reachable in S unless a flush has since folded it. A flush holds `M`, so that
   fold is also complete in S, and its cut pages are either still on the chain or
   already leaked.
6. The pending walk stops at S's `pendingtail`. Pages appended after S are past
   the tail, and their LSNs are greater than `F`.
7. The walk must be **complete**. A walk that hits a chain error (a page past
   EOF, a wrong kind, a freed page on a live chain, a cycle), cannot open a
   descriptor or a weft root, or finds a weft kind it does not know marks the map
   **incomplete**. An incomplete map frees nothing, logs, and leaves the decision
   to `weave_check(deep)` and REINDEX. That stops an unreadable chain, or a weft
   added later without a walker arm, from being freed as a leak. The "unknown
   weft kind" arm is what turns a forgotten walker arm into a skipped reclaim
   instead of data loss.
8. A reader holding an older directory snapshot can still read P, if P is a
   merge input whose free was interrupted. The free stamps `pd_prune_xid` like
   every other free, so the allocator's recycle gate (`weave_page_recyclable()`)
   still waits out that reader.

**The race the fence does not close, and why it does not matter.** A parallel
merge whose leader errors drops `M` (`PG_FINALLY`) while its workers may still
be running. A reclaim can then free pages a dying worker writes afterwards. No
publish ever follows, because the leader has abandoned the merge, and the freed
pages are stamped with an XID no older than the worker's transaction. So the
allocator cannot hand them out until the worker is gone. A worker that writes
into an already-freed page can, at worst, overwrite the `WEAVE_FREED` flag and
leak that page again until the next VACUUM.

**Zero pages, and pages already flagged freed, are only re-recorded in the FSM.
The reclaim never writes them.** A zero page below the relation length read
under the relation extension lock is not being extended, because
`weave_new_buffer()` holds that lock until it has the new buffer exclusively
locked. Every caller then keeps that lock until the page's first record. If
`ConditionalLockBuffer()` gets the page and it is still zero, nobody is writing
it. Recording a page in the FSM is safe even if another backend takes it at the
same moment, because of a change to the allocator: **a free-list candidate that
is initialized and not `WEAVE_FREED` is a live page and is dropped from the
candidate set**. It is no longer pushed back into the FSM (`RecordUsedIndexPage`
for the low-free list, no re-record for the live FSM). Before this change a stale
FSM entry for a live page was re-recorded every time it was refused, so it lived
for ever. nbtree and GIN deal with stale FSM entries the same way: refuse the page
and drop it.

**What enforces the two locks.** `weave_new_buffer()` raises
`elog(ERROR)` unless the caller holds `M`, `X`, or `AccessExclusiveLock` on the
index, or is a parallel worker. A parallel worker is a build or merge
participant whose leader holds the right lock. The pending append is the one
writer exempt from this, because it links each page in the record that writes
it, and it allocates through `weave_new_buffer_linked()`, which states that
exemption in its name. A future segment writer that forgets `X` therefore fails
its first allocation in every build, rather than corrupting an index months
later. This is the `weave_assert_merge_serialized()` argument again: the comment
form of a locking rule was already in this tree and was not followed.

**Cost.** One reachability walk over every chain and one pass that reads every
block's buffer, per VACUUM cleanup. That is the same order as GIN's and GiST's
cleanup, which read every page to recycle them. It is measured at 1M rows in
`bench/RESULTS_G75_RECLAIM.md` (owed until that run exists). The pass yields to
`vacuum_delay_point()` per block.

**What it does not cover.**
- A server that has loaded the new `.so` into some backends but not others. A
  backend still running the old library writes oversized segments without `X`.
  After an upgrade, restart the server. Loading pg_weave already needs that for
  `shared_preload_libraries` users, and a mixed-version backend set is not
  supported in general.
- A temporary or unlogged index's LSNs do not advance, so `F` excludes nothing
  there. That is safe: a temporary index has no concurrent writer. An unlogged
  index is refused at build (`weave_reject_unlogged()`).
- ANALYZE-only cleanup (`info->analyze_only`) skips the reclaim, as it skips the
  flush.
