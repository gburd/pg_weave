# Plan: docvals pending/insert slice — make the docvalues gate insert-safe (fixes G52)

Status: proposed 2026-09-26. Prereqs: the int8 build+merge slice (Tasks 1–8, done;
G49 + G51 fixed). This is the "aminsert/flush of docvals" work the int8-slice plan and
`doc/specs/DOCVALS_CHANNEL.md` §9/§11 deferred.

## Why (G52)
The docvalues gate is blind to the pending (un-flushed INSERT) buffer, while the lexical
channel is not. Measured: after one post-build INSERT, `body @@@ 'zebrafish'` finds the
row but `price < 100` returns 0 (heap truth 1), and `@@@ 'alpha' AND price < 100` returns
0 — the gate's empty contribution corrupts the `tidset_and`. A silent wrong answer to a
pushed-down qual, reachable by any ordinary INSERT, asymmetric with the lexical channel
within one index. `int8_docval_ops` (0.24/0.25, unreleased) must not release until fixed
(the G49 bar). Full characterization in `doc/GAPS.md` G52.

## The format decision (follow the V8→V9→V10 precedent exactly)
Pending items have versioned, self-describing layouts: V8 `{tid,doclen}`+doc, V9 adds
`veclen`+wvec, V10 (current `WeavePendingItem`) adds `gramlen`+gram, each with its own page
kind (`WEAVE_PK_PENDING_V9/_V10`). **Add V11:** `WeavePendingItem` gains `uint32 dvlen`
and a trailing `int64 docval`, with page kind `WEAVE_PK_PENDING_V11` and
`WEAVE_VERSION_PENDING_DV = 11`. `dvlen ∈ {0,8}` — 8 when the index has a docvals column
and the value is non-NULL, 0 for an index with no docvals column (mirrors `veclen`/`gramlen`
presence-by-length; no separate flag). v1 docvals is NOT NULL, so a NULL docvals value at
INSERT ERRORs exactly as the build path does (`weave_docvals_accum_add`); `dvlen=0` never
means "NULL docval", it means "no docvals column". Older V10/V9/V8 pending pages stay
readable (an index mid-upgrade with un-flushed V10 items must still flush) — the reader
keys off the page kind, as it does today.

## Tasks (TDD; coordinator compiles + gates; sub-agent worker→reviewer for code)

### Task 1 — V11 pending format (no behaviour change yet)
`WeavePendingItem` + `dvlen`/`docval`; `WEAVE_PK_PENDING_V11` in `pagekind.h` (update the
standalone `test_pagekind` enum list — the eleventh-member guard); `WEAVE_VERSION_PENDING_DV`;
size helper `weave_pending_item_len` for V11; `weave_pending_item_write` takes `int64 docval,
bool has_docval`. StaticAssert the new sizeof. **Test:** a standalone round-trip of a V11
item (write→read back tid/doc/vec/gram/docval) in the pagekind/pending property test; assert
V10/V9/V8 sizes unchanged (no accidental layout shift).

### Task 2 — aminsert writes the docval
`weave_insert` reads the docvals column (via the index's `dvattno`), detoasts nothing (int8
is by-value), and passes `docval,has_docval` to `weave_pending_item_write`; new pending pages
are `WEAVE_PK_PENDING_V11` when the index has a docvals column. **Test:** after INSERT (no
flush) the tail pending page is V11 and its item carries the value (introspection or a
debug assert); an index with no docvals column still writes V10.

### Task 3 — the scan evaluates the docvals gate over pending items (the G52 fix, read side)
The pending-scan path (the one the lexical channel already uses to find un-flushed rows —
find it via `body @@@` reading pending) must, when a docvals restriction scankey is present,
read each pending item's `docval` and apply the same `dvOp`/`dvConst` comparison
`weave_docvals_collect` applies to a segment, emitting the pending row's TID iff it passes.
Wire into both the plain path and the fused gate (`fusepath`/`amscan`). **Test (the G52
repro, now a gate):** post-INSERT no-flush, `price < 100` == heap, `@@@ x AND price < c` ==
heap. Positive control: the pre-fix path returns 0 for the inserted row.

### Task 4 — flush + oversized-insert carry docvals into the segment
`weave_flush_pending` (`ambuild.c:6133`, currently `bs.dvattno = 0`) sets `dvattno` from the
index and collects each pending item's `docval` into the flushed segment's docvals accum
(via `weave_docvals_accum_add_pair`, the G51 pair entry point). `weave_insert_oversized_as_segment`
likewise. **Test:** after INSERT then `weave_merge()`/flush, `price<c` == heap on the flushed
segment (extends `sql/docvals.sql`); combine with the G51 merge case so an insert-then-merge
index is fully correct.

### Task 5 — durability, compat, fuzz
TAP: crash-recovery of a V11 pending page (GenericXLog replay), the `t/010`/`t/019`
manufacture-old-image pattern for a V10→V11 upgrade (old pending pages flush correctly);
`no_data_checksums => 1` on PG18. Fuzz: extend `fuzz_docvals`/the pending fuzz for the V11
item walk (a torn `dvlen` cannot read past the item). Assert `weave_check(deep)` covers a
V11 pending page.

### Task 6 — end-to-end + release gate
G52 marked FIXED; `sql/docvals.sql` post-insert section green; ext bump (0.25→0.26) with the
migration; `DOCVALS_CHANNEL.md` §9/§11 updated (the limitation is lifted, not "extends to").
Rule 12: a delete+insert+merge run at scale on EC2 before the opclass is release-eligible.

## Out of scope (later)
float/int4/int2/date/bool docvals types; NULLs + null bitmap; text dictionary; multi-column
docvals; the zone-map. Each is additive on the v1 store and the V11 pending field.
