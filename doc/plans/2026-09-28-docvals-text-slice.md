# Docvals TEXT (dictionary-encoded) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A writable `text` docvals facet — build, scan, pending INSERT, flush and merge — with
`<, <=, =, >=, >` pushed down as an exact gate. Docvals build-order **step 4**, plus the text
half of step 5 (merge re-dictionary). Multi-column AND-intersection stays deferred.

**Architecture:** The store's per-docid int64 value array holds a **dictionary ordinal**; a new
dictionary region carries the distinct values sorted by the column's collation. So the whole
v2 skeleton (docid array, null bitmap, build/flush/vacuum plumbing) is reused, and text adds
exactly three things: (a) the dictionary region (store v3), (b) scan-time resolution of the
constant to two ordinal boundaries, (c) raw-bytes accumulation on the pending/flush/merge paths
so every writer builds a fresh dictionary. The boundary search — the one place an off-by-one
silently drops or admits a row (spec §7) — is a **pure** function in `docvals.h` taking an
injected comparator, property-tested with `memcmp`; the backend injects `varstr_cmp` under the
column collation.

**Tech Stack:** C (PostgreSQL core style), PGXS/Nix, Hegel-style standalone property tests,
pg_regress, TAP, `test/fuzz/`, `bench/aws/`.

**Spec:** `doc/specs/DOCVALS_CHANNEL.md` (§3 text layout, §5 predicate→gate, §6 NULLs, §7
contract, §9 build/flush/merge, §11 step 4/5).

## Design decisions (settled; do not re-derive)

1. **Boundaries.** For constant `k` over a dictionary `D[0..n)` strictly ascending under the
   collation: `lo = #{i : D[i] < k}`, `hi = #{i : D[i] <= k}` (so `hi - lo ∈ {0,1}`). Then
   `LT: ord < lo`, `LE: ord < hi`, `EQ: lo <= ord < hi`, `GE: ord >= lo`, `GT: ord >= hi`.
   NULL docids are skipped first (§6), exactly as `weave_dv_eval_int8`.
2. **Store v3.** `WeaveDocvalsHeader` grows 28 → 32 bytes with `uint32 dict_off` placed in
   what is today alignment padding (`values_off` is already `MAXALIGN(28) == 32`), so v1/v2
   images are byte-identical and still validate. `dict_off` is read **only** when
   `version == 3`; v3 requires `typid_kind == 2` (TEXT) and `dict_off != 0`; v1/v2 require
   `typid_kind == 1`. The writer zeroes the 4 former-padding bytes on every store it writes.
3. **Dictionary region** at `dict_off = MAXALIGN(end of docids, or end of null bitmap if
   present)`: `uint32 ndict`, `uint32 offs[ndict + 1]` (`offs[0] == 0`, non-decreasing,
   `offs[ndict] == blob length`), then the packed entry bytes (raw text payload, no varlena
   header). Entry `i` is `blob[offs[i] .. offs[i+1])`. The validator checks structure and that
   **every non-NULL ordinal is in `[0, ndict)`**; it cannot check collation order (not pure) —
   `weave_check` deep verifies strict ascending order under the collation (T5).
4. **Deterministic collations only.** A non-deterministic collation makes byte-distinct values
   compare equal, so "distinct values" and "equality == ordinal compare" are both wrong. CREATE
   INDEX **ERRORs** on a `text_docval_ops` column whose collation is non-deterministic.
5. **Pending text encoding.** A v11 pending item's docval trailer on a TEXT docvals index is
   `0x01` followed by the raw bytes (`dvlen = 1 + len`), so the empty string (`dvlen == 1`) can
   never collide with the NULL encoding (`dvlen == 0`). The reader distinguishes int8 vs text by
   the **index layout** (column type), never by the item. An int8 index keeps `dvlen ∈ {0, 8}`.
6. **No WEAVE_VERSION bump.** Store versioning is self-describing (the v2 precedent); an old
   `.so` meeting a v3 store fails closed (unknown version). Extension **0.26.0 → 0.27.0** adds
   `text_docval_ops` (varchar columns use it via binary coercion, like btree's `text_ops`).
7. **Collation drift** (a libc/ICU upgrade reordering strings) invalidates a text docvals
   dictionary exactly as it invalidates a btree; the remedy is REINDEX, documented not solved.

## Global Constraints

- Pure C, PostgreSQL core style (`doc/CONVENTIONS.md`); explanations next to the code.
- 100% GenericXLog (hard rule 2); the store is written by the existing `weave_docvals_write`
  chain writer — no new page kind.
- On-disk bytes are not trusted: every new field is validated in `docvals.h`, fuzzed with a
  planted-bug variant (CONVENTIONS 2); unknown version → ERROR (CONVENTIONS 3).
- `check-alloc`: dictionary blobs, offset arrays and accumulated raw bytes are corpus-scale →
  `WEAVE_ALLOC_MAYBE_HUGE` / huge repalloc; a doubling `cap` needs the huge-safe path.
  A dictionary whose blob or offsets would exceed `UINT32_MAX` bytes ERRORs at write time.
- `check-pdlower`, `check-rename`, `check-ascii` stay green (the new SQL file is ASCII).
- Expected output regenerated only from a FULL installcheck, proven additive (hard rule 3).
- A new TAP file goes into `flake.nix` `PROVE_TESTS` in the same commit.
- Hard rule 12: flush/merge/vacuum are touched → T8 EC2 10M run is part of done.
- Agents do not build; the coordinator compiles and runs every gate.

## Review Focus

1. **Empty string vs NULL** — `''` must be a real dictionary entry matching `= ''`, `<= 'a'`,
   and never be confused with NULL in pending (dvlen) or the store. Tests: T1 (prop_text draws
   empty strings), T4 regression row `''` before and after flush.
2. **Constant absent from the dictionary** (between entries, below all, above all) — `lo == hi`,
   `=` emits nothing, ranges split correctly. T1 prop_text draws constants not in the set and
   the extremes; T3 regression includes `< ''`, `> 'zzzz'`, a between-value constant.
3. **Collation ≠ byte order** — under a linguistic default collation `'B' < 'a'` may be false.
   T3 regression runs the same predicates on a `COLLATE "C"` column and a default-collation
   column, each index==heap.
4. **Two segments with different dictionaries** — ordinal 3 means different strings per bolt; a
   scan must resolve per segment and a merge must re-dictionary, never concatenate ordinals.
   T4 (pending + flushed + built bolts coexisting) and T5 (merge) regression, index==heap.
5. **Non-deterministic collation** — must ERROR at CREATE INDEX, not silently build. T7 TAP
   (skips with a message when the server has no ICU).

---

### Task 1: Pure core — store v3, dictionary, boundary search, ordinal evaluator

**Files:**
- Modify: `include/weave/docvals.h`
- Test: `test/hegel/test_docvals.c` (new `prop_text`, `prop_text_reject`; registered in `main`)

**Interfaces (Produces):**
- Header: `uint32 dict_off` after `docids_off`; `_Static_assert(sizeof == 32)`; macros
  `WEAVE_DV_KIND_INT8 1`, `WEAVE_DV_KIND_TEXT 2`.
- `const char *weave_docvals_validate(const void *img, size_t len)` — extended per decisions 2–3.
- `uint32_t weave_docvals_ndict(const void *img)` — 0 for v1/v2.
- `const unsigned char *weave_docvals_dict_entry(const void *img, uint32_t ord, uint32_t *len)`.
- `typedef int (*WeaveDvCmp)(void *ctx, const void *a, uint32_t alen, const void *b, uint32_t blen);`
- `uint32_t weave_dv_dict_lower_bound(const void *img, const void *key, uint32_t klen, WeaveDvCmp cmp, void *ctx)` → `lo`.
- `uint32_t weave_dv_dict_upper_bound(...)` same signature → `hi`.
- `int weave_dv_eval_ord(const void *img, WeaveDvStrat op, uint32_t lo, uint32_t hi, uint64_t *out, uint32_t outcap)` — decision 1; same output contract as `weave_dv_eval_int8`.
- Test helpers (under `WEAVE_DOCVALS_TEST_HELPERS`): `size_t weave_docvals_store_len_text(uint32_t ndocs, int has_nulls, uint32_t ndict, size_t blob)`, `void weave_docvals_build_text(void *buf, const int64_t *ords, const uint64_t *docids, const uint8_t *nullbits, uint32_t ndocs, const uint32_t *offs, uint32_t ndict, const unsigned char *blob)`.

- [ ] **Step 1: Write `prop_text`.** Per trial: draw 0..MAXN docids (existing `draw_docids`),
  a random value set of short byte strings over a small alphabet (include `""`, shared
  prefixes, lengths 0..6), ~25% NULLs; build the dictionary by distinct+`memcmp`-sort, ords by
  lookup; build the store with the helper; assert `weave_docvals_validate == NULL`. Draw a
  constant (50% an existing value, 50% random incl. `""` and above/below all). For each of the
  5 ops assert: `eval_ord(op, lower_bound(k), upper_bound(k))` output == reference
  `{docid : !null && memcmp_order(value, k) satisfies op}` (same order, same count). Also assert
  `hi - lo ∈ {0,1}` and `lo <= hi <= ndict`.
- [ ] **Step 2: Write `prop_text_reject`.** Mutate one field of a valid v3 image and assert the
  validator refuses: `dict_off` misaligned / past end, `ndict` too large for len, `offs`
  decreasing, `offs[0] != 0`, `offs[ndict]` past end, an ordinal `>= ndict` on a non-NULL
  docid, `version 3` with `typid_kind 1`, `version 2` with `typid_kind 2`. Also every
  truncation length `0..len-1` of a valid v3 image is refused (the `prop_trunc` pattern).
- [ ] **Step 3: Run; confirm the new props fail** (functions missing → compile error is the
  expected failure). Coordinator: `make check-standalone`.
- [ ] **Step 4: Implement** the header change, validator, accessors, bounds (standard
  half-open binary search; `lower_bound` uses `cmp(D[mid], k) < 0`, `upper_bound` uses `<= 0`),
  `eval_ord`, helpers. Existing props (`prop_pred`, `prop_nulls`, `prop_trunc`, `prop_reject`)
  must still pass unchanged — v1/v2 images are byte-identical.
- [ ] **Step 5: Teeth (positive control).** Change `upper_bound` to `< 0` locally → `prop_text`
  must FAIL; revert. Record the failing seed/message in the commit body.
- [ ] **Step 6: Gate.** `make check-standalone` exits 0, prints the test_docvals check count;
  `gcc -O2 -fsanitize=address,undefined -I include test/hegel/test_docvals.c` run clean.
- [ ] **Step 7: Commit** `docvals: store v3 with a collation-sorted text dictionary (pure core)`.

### Task 2: Build path — opclass, type detection, text accumulator + writer

**Files:**
- Create: `sql/pg_weave--0.26.0--0.27.0.sql`; Modify: `pg_weave.control` (0.27.0), `Makefile`
  (DATA), `sql/pg_weave--*.sql` base script if the project keeps a full install script (follow
  the 0.25→0.26 precedent exactly).
- Modify: `src/am/am.c` (`weave_opfamily_kinds[]` row `text_docval_ops → WEAVE_WK_DOCVALS`),
  `include/weave/docvals.h` (`WEAVE_DV_T_TEXT` in `WeaveDvType`), `include/weave/am.h`
  (`WeaveDocvalsAccum` text fields), `src/pages/docvals_page.c`, `src/am/ambuild.c` (build
  callback / `weave_build_dvtype`).

**Interfaces:**
- Consumes: T1 layout + `weave_docvals_build_text` semantics (the backend writer must produce
  byte-identical layout).
- Produces:
  - `WEAVE_DV_T_TEXT`; `weave_dv_type_for_oid(TEXTOID | VARCHAROID) → WEAVE_DV_T_TEXT`.
  - `WeaveDocvalsAccum` gains `Oid collation`, `char *tbytes; Size tlen, tcap; uint64 *toff;
    uint32 *tlenv` (per-row start offset into `tbytes` -- uint64 because the arena is
    corpus-scale and may pass 4 GB -- and a parallel per-row byte length, both in lockstep with
    the existing docid/value/isnull arrays; row i's bytes are
    `tbytes[toff[i] .. toff[i] + tlenv[i])`, and a NULL row has `tlenv` 0 and is told from `''`
    only by `isnull`).
  - `void weave_docvals_accum_init(WeaveDocvalsAccum *acc, MemoryContext ctx, bool active, WeaveDvType dvtype, Oid collation)` (new param; update all callers).
  - `void weave_docvals_accum_add_text(WeaveDocvalsAccum *acc, uint64 docid, const char *p, uint32 len, bool isnull)`; `weave_docvals_accum_add` dispatches to it for TEXT (detoast with `PG_DETOAST_DATUM_PACKED`, `VARDATA_ANY`/`VARSIZE_ANY_EXHDR`).
  - `weave_docvals_write_weft` for TEXT: sort rows by docid (carry isnull + byte span), then
    sort distinct values by `varstr_cmp(.., collation)` with a bytewise tie-break removing
    exact duplicates, assign ordinals, write a v3 store via `docvals_serialize` extended with
    `(offs, ndict, blob)`.
  - Build-time check: collation of the docvals column is deterministic
    (`get_collation_isdeterministic`), else `ereport(ERROR, errcode(FEATURE_NOT_SUPPORTED),
    errmsg("text docvalues require a deterministic collation"))`.

- [ ] **Step 1: Write the failing regression** (append §10 to `sql/docvals.sql`): table
  `dvt(id int, body text, cat text COLLATE "C")`, ~2000 rows with ~40 distinct `cat` values
  incl. `''` and 5% NULL; `CREATE INDEX … USING weave (body wdoc_lex_ops, cat text_docval_ops)`;
  `SELECT weave_check('dvt_idx', true)` shows the docvals chain reachable. (No pushdown asserted
  yet — T3.)
- [ ] **Step 2: Implement** SQL + opclass row + type + accumulator + writer + callback wiring.
- [ ] **Step 3: Gate.** Coordinator: `nix build .#pg17`; lpg: create the index, `weave_check`
  deep clean, `weave_page_info` shows docvalues pages; a throwaway SQL function or DEBUG1
  `elog` in the loader proves the loaded store validates with `version 3, ndict 40` (positive
  control that the text path — not the int8 path — produced it; remove before commit).
  installcheck pg17 green (expected updated only for the 0.26→0.27 NOTICE bump, proven
  non-semantic, and the new §10 lines, proven additive).
- [ ] **Step 4: Commit** `docvals: text_docval_ops builds a dictionary-encoded store (ext 0.27.0)`.

### Task 3: Scan — text constant, per-segment boundary resolution, pushdown

**Files:** `src/am/amscan.c` (scan opaque, `weave_rescan` dispatch ~2380–2460,
`weave_docvals_collect` ~4663), `src/am/fusepath.c` (borrow accepts a `text_docval_ops` clause),
`sql/docvals.sql` §10, `expected/docvals.out`.

**Interfaces:**
- Scan opaque gains `bool dvText; text *dvKey; Oid dvColl;` (key copied into scan memory
  context). `weave_docvals_collect` takes the text key + collation when `dvText`.
- Per segment: load store; if `version == 3` → `lo/hi` via T1 bounds with a backend comparator
  `weave_dv_varstr_cmp(ctx = &collation, …)` wrapping `varstr_cmp`, then `weave_dv_eval_ord`;
  a v1/v2 (int8) store on a text column is impossible by construction → `elog(ERROR)`.

- [ ] **Step 1: Failing regression.** In §10, under `enable_seqscan=off, enable_bitmapscan=off`,
  for each op in `< <= = >= >` and constants {an existing value, `''`, a between-value string,
  a below-all, an above-all}: `EXPLAIN (costs off)` shows `Index Cond` on `cat` (one EXPLAIN per
  op), and `dvs_agree`-style assertion `index count == seqscan count` → `t`. Repeat on a second
  column `cat2 text` with the database default collation. Add a `varchar(20)` column variant
  (one op suffices) and a fused query `WHERE cat < 'm' ORDER BY fuse(...)` (if the file's
  existing fused pattern exists — reuse it) asserting agreement.
- [ ] **Step 2: Implement** rescan capture, collect path, fusepath borrow.
- [ ] **Step 3: Mutation (site-running check).** Swap `lo`/`hi` in the `LE` arm locally → §10
  must go red on pg17; revert. Record in the commit body.
- [ ] **Step 4: Gate.** installcheck pg17 + pg18 green; expected regenerated from the FULL list,
  old file a byte-identical prefix.
- [ ] **Step 5: Commit** `docvals: push text comparisons down as an exact dictionary gate`.

### Task 4: Pending INSERT, pending scan, flush

**Files:** `src/am/ambuild.c` (`weave_insert` docval ~5983, item build ~5834, flush PRODUCER 4
~6337, oversized-insert path), `include/weave/am.h` (`WeavePendingRec`), the pending reader that
fills `rec.hasdv/dvslot/docval` (grep `dvslot =`), `src/am/amscan.c` pending loop ~4728.

**Interfaces:**
- `WeavePendingRec` gains `const char *dvbytes; uint32 dvbyteslen;` — set for a TEXT layout
  when `dvlen >= 1` (first byte must be `0x01`, else ERROR corrupt), `hasdv = true`. `dvlen == 0`
  → NULL (`dvslot && !hasdv`) unchanged. For an int8 layout the reader still requires
  `dvlen ∈ {0, 8}`.
- Pending scan for text: `weave_dv_varstr_cmp(key, rec.dvbytes)` then the op against 0.
- Flush: `weave_docvals_accum_add_text(&bs.dv, docid, rec.dvbytes, rec.dvbyteslen, false)` /
  NULL arm → `add_text(..., true)`; the flush accum is initialised TEXT with the column collation.
- Oversized insert (bypasses pending) uses the build accumulator → already TEXT-aware via T2.

- [ ] **Step 1: Failing regression** (§10 continued): after build, INSERT rows with new values
  (one below all, one between, `''`, NULL, one equal to an existing value); index==heap for all
  5 ops **before** flush; force flush (the existing `weave_flush`/vacuum idiom used in §9);
  index==heap again; `weave_check` deep clean.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Gate.** installcheck pg17+pg18; `make check-alloc check-pdlower`.
- [ ] **Step 4: Commit** `docvals: text INSERT is answerable before and after flush`.

### Task 5: Merge re-dictionary + amcheck order verification

**Files:** `src/am/ambuild.c` (`weave_docvals_merge_append` ~3377, merge accum init sites incl.
~4200 — make every merge site that carries docvals init TEXT with the collation), `src/am/amcheck.c`
(docvals arm ~1384), `sql/docvals.sql` §10.

**Interfaces:**
- `weave_docvals_merge_append` for a v3 input: for each surviving (non-tombstoned) dense index,
  `add_text(docid, dict_entry(ord), isnull)`; the writer then builds the union dictionary. A v3
  input merged into an int8 accumulator (or vice versa) → `elog(ERROR)` (impossible by
  construction).

> **Pulled forward into Task 2 (2026-09-28):** the `weave_docvals_merge_append` text mode and
> the merge accumulator-init sites (TEXT + collation). Reason: the end of a multi-segment
> CREATE INDEX collapses its segments through the merge (`weave_build_finalize` ->
> `weave_merge_segments`), so an interim "don't carry text through the merge" guard silently
> dropped the whole docvalues weft of any text index built as more than one segment. Task 5
> keeps: the amcheck dictionary-order verification, the merge regression (Step 1), the shape
> guard (Step 2), and the copy-the-ordinal mutation (Step 4).

- `weave_check` deep: for a v3 store, `varstr_cmp(D[i], D[i+1]) < 0` for all i under the
  column collation; report "docvalues dictionary is not strictly ascending" otherwise.

- [x] **Step 1: Failing regression:** produce ≥3 bolts with overlapping and disjoint value sets
  (build, then two INSERT+flush rounds with values chosen so the same string has different
  ordinals in different bolts), DELETE some rows, VACUUM so the merge runs (§9's idiom), assert
  `nsegments` dropped (merge actually ran — positive control) and index==heap for all 5 ops
  and the Review-Focus constants; `weave_check` deep clean.
- [x] **Step 2: Shape guard (hard rule 16).** The merged output depends on segment shape
  (which strings live in which bolt). Run the §10 merge assertion for two shapes: all-new
  values in the inserted bolts vs. all-shared values — both must agree with heap.
- [x] **Step 3: Implement. Step 4: Gate** installcheck pg17+pg18; mutation: make merge copy the
  input ordinal instead of the entry bytes → §10 must go red; revert.
- [x] **Step 5: Commit** `docvals: merge re-dictionaries text stores`.

> **Done 2026-09-29.** `wvck_docvals()` (src/am/amcheck.c) emits
> `docvals_dictionary_ascending` under `weave_check(deep)`; `sql/docvals.sql` sect. 15 gained
> the merged phase (4 bolts -> 1) and sect. 16 the two-shape guard (all-new / all-shared). The
> mutation (merge feeds the input ordinal's 8 bytes instead of the entry) turned the merged phase
> 13/45 red and the shapes 22/55 and 18/55 red; the positive control for the invariant (writer
> sorts descending) made it report `f` on every text index in the file. Caveat: every text store
> the invariant is exercised on in installcheck is effectively byte-ordered unless the test
> database's default collation is linguistic, so a check that compared bytes instead of the
> column collation would pass there too.

### Task 6: Fuzz the v3 dictionary region

**Files:** `test/fuzz/fuzz_docvals.c`.

- [x] Add v3 seed images (with/without null bitmap, `ndict` 0/1/many, empty entries) and
  mutations targeting `dict_off`, `ndict`, `offs[]`, and ordinals. After a successful validate,
  call every reader (`ndict`, `dict_entry` for all ords, both bounds with a memcmp comparator,
  `eval_ord` all ops) under ASan/UBSan. Planted bug (`-DPLANT_BUG_DICT`): drop the
  "ordinal < ndict" check → must abort. Coordinator runs the clean corpus locally with the
  project's standalone sanitizer build; `check-fuzz` relies on CI (local clang lacks libc
  headers). Commit `docvals: fuzz the v3 dictionary region`.

> **Done 2026-09-29.** Sections 6-9 of `test/fuzz/fuzz_docvals.c`; the teeth are a compile-time
> removal in the real validator (`WEAVE_DV_PLANT_NO_ORD_GUARD`), not a transcription, and
> `test/fuzz/run.sh` builds and requires the abort (`fuzz_docvals_noord`). Local run under gcc
> ASan+UBSan: 687,947 cases clean, both docvals teeth builds abort at their intended assertion.

### Task 7: TAP — crash recovery, torn write, concurrency, non-deterministic collation

**Files:** `t/023_docvals_text_recovery.pl`, `t/024_docvals_text_corruption.pl`,
`t/025_docvals_text_concurrent.pl`, `flake.nix` PROVE_TESTS. Model each on `t/020`–`t/022`.

- [ ] 023: build + INSERT text rows, `immediate` stop without checkpoint, restart, index==heap
  for all ops + EXPLAIN Index-Scan positive control.
- [ ] 024 (`no_data_checksums => 1`): corrupt `dict_off`, an `offs[]` entry, and an ordinal on
  disk → query ERRORs "corrupt docvalues store", postmaster alive.
- [ ] 025: concurrent INSERTers of text values + a reader asserting a fixed anchor predicate's
  count is monotone and == heap at the end; reads > 0 asserted.
- [ ] Non-deterministic collation: if `CREATE COLLATION … (provider = icu, deterministic =
  false)` succeeds, CREATE INDEX with `text_docval_ops` on it must fail with the T2 message;
  else `note` the skip and assert the skip reason is "ICU unavailable" (in 023 or its own test).
- [ ] Gate: tap-pg17 + tap-pg18 green, new files' markers present in the log. Commit.

### Task 8: Docs, scale run, closure

**Files:** `doc/specs/DOCVALS_CHANNEL.md` (§3 text row, §9 text flush/merge DONE, §10 store v3,
§11 step 4 DONE + step 5 text half DONE), `include/weave/docvals.h` file header, `doc/PHASES.md`,
`doc/PRODUCTION_READINESS.md`, `bench/aws/docvals_scale.sql` (+ text facet `tcat`, e.g. 1000
distinct values Zipf-ish + 5% NULL, its own index since one docvals column per index), 
`bench/RESULTS_DOCVALS_SCALE.md` (TEXT section).

- [ ] Add the text facet to the 10M workload with the same self-checking `dvs_assert_agree`
  across build / DELETE+VACUUM / merge / pending / flush, ON_ERROR_STOP fatal.
- [ ] EC2 run via `bench/aws/run.sh` docvals job on `hotdog` (us-east-2), commit first (source
  arrives by `git archive HEAD`); `disagreements=0` every phase; record sizes and the tooth
  (`tcat < k` == `tcat IS NOT NULL AND tcat < k`). Sweep only `pgweave-*` resources after.
- [ ] Docs + memory; CI full matrix green on the final commit.
