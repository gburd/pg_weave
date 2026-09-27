# Docvals NULL support + durability gates — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add NULL handling to the docvals channel (store format v2 with a null bitmap, "emit iff not-null AND comparison true"), and discharge the adversity gates the weft owes (crash-recovery TAP, torn-write, concurrency) — docvals build-order **step 3**.

**Architecture:** The int64 store and single-comparison evaluator are unchanged; a new optional null bitmap region (one bit/docid, 1 = NULL) is appended after the docids array and the evaluator skips a docid whose null bit is set. The store's own `version` field goes 1→2 and `null_off` carries the bitmap offset (0 = no NULLs), so old v1 stores keep reading (X1 non-breaking). Pending NULLs reuse the v11 item's `dvlen == 0` slot — unreachable today because NULL currently ERRORs — so no new pending page kind.

**Tech Stack:** C (PostgreSQL core style), PGXS/Nix build, Hegel property tests, pg_regress, TAP (`t/*.pl`), `test/fuzz/`.

**Spec:** `doc/specs/DOCVALS_CHANNEL.md` (§3 format, §6 NULL semantics, §7 contract, §9 build/flush/merge/vacuum, §11 build order step 3).

## Global Constraints

- Pure C, PostgreSQL core C style (`doc/CONVENTIONS.md`, `.agent/skills/weave-pg-style`).
- 100% GenericXLog for any page write (hard rule 2). No raw XLogInsert/log_newpage/smgrwrite/custom rmgr.
- On-disk bytes are not trusted: every new field/region is validated by the pure `docvals.h` validator, with a planted-bug fuzz target (CONVENTIONS decision 2, hard rule 1).
- An unknown store version is an ERROR (CONVENTIONS decision 3): validator accepts version 1 and 2 only.
- `check-alloc`: any corpus-scale allocation uses the huge-safe allocator; the null bitmap size is `ceil(ndocs/8)`, ndocs is corpus-scale.
- `check-pdlower`: read `pd_lower` only via `weave_page_entry_end()`.
- `WEAVE_DV_MAXALIGN` (8) governs region offsets; header stays 28 bytes.
- Never regenerate `expected/*.out` without proving the diff non-semantic (hard rule 3); generate expected from a FULL `make installcheck`, never `REGRESS=<one>`.
- A new TAP file must be added to `flake.nix`'s `PROVE_TESTS` in the same commit.
- rule 12: because this touches flush/merge/vacuum, "local green" is not done — an EC2 scale run is a task here (T8).

## Review Focus

- **A NULL that satisfies the comparison arithmetically** (e.g. NULL encoded to some int64 that happens to be `< c`): must be excluded by the null bitmap *before* the comparison, never emitted. Owned by T1 (evaluator) + property test.
- **A v1 store on disk after the upgrade** (built by 0.26.0): must still validate and answer identically (null_off must be 0, version 1 accepted). Owned by T1 validator + T4 regression against a v1-built index.
- **A torn/corrupt null bitmap** (null_off past end, bitmap truncated, null_off set but version 1): must ERROR cleanly, never crash or wrong-answer. Owned by T1 validator + T5 fuzz.
- **A pending NULL row visible before flush** (`dvlen == 0` on a docvals-bearing index): the gate must exclude it live and the flush must fold it into the bitmap so the answer is identical before and after flush (the G52 hazard, for NULLs). Owned by T3 + T4.
- **A merge whose inputs mix null-bearing and null-free (or absent) docvals wefts**: the merged bitmap must be correct over the union of docids, tombstoned docids dropped. Owned by T3 (merge_append) + T8 scale run.

---

### Task 1: Store format v2 + null-aware evaluator (pure, standalone)

**Files:**
- Modify: `include/weave/docvals.h` (header, validator, evaluator, accessors, test helpers)
- Test: `test/hegel/test_docvals.c`

**Interfaces:**
- Consumes: existing `WeaveDocvalsHeader` (28 bytes), `weave_docvals_validate`, `weave_dv_eval_int8`, `weave_docvals_int8`/`_docid`, `WEAVE_DV_MAXALIGN`, the `#ifdef WEAVE_DOCVALS_TEST_HELPERS` builders.
- Produces:
  - `static inline int weave_docvals_isnull(const void *img, uint32_t idx)` — 1 if dense idx is NULL, else 0; returns 0 when `null_off == 0`.
  - `weave_dv_eval_int8` semantics: a docid with the null bit set is never emitted, for every `WeaveDvStrat`.
  - Test helper `weave_docvals_build_nulls(void *buf, const int64_t *vals, const uint64_t *docids, const uint8_t *nullbits /* NULL => no bitmap */, uint32_t ndocs)` and `weave_docvals_store_len_nulls(uint32_t ndocs, int has_nulls)`.

- [ ] **Step 1: Write the failing test** — in `test_docvals.c`, add `prop_nulls`: over random (value, docid, isnull) triples and a random `(op, c)`, assert the set emitted by `weave_dv_eval_int8` over a store built with the null bitmap equals the reference set `{ docid[i] : !isnull[i] && cmp(vals[i], op, c) }`, for all five ops and every `WeaveDvType`. Add validator-teeth cases: `null_off` past image end, bitmap one byte short, `null_off != 0` with `version == 1` — each must return non-NULL (a reason string). Add coverage counters and assert both branches (some-null and no-null stores) and every op are exercised, printing an `N checks, 0 failures` total.

- [ ] **Step 2: Run test to verify it fails** — `gcc -O2 -I include -DWEAVE_DOCVALS_TEST_HELPERS -o /scratch/pg_weave/td test/hegel/test_docvals.c -lm && /scratch/pg_weave/td`. Expected: FAIL (either no `weave_docvals_build_nulls`, or nulls emitted).

- [ ] **Step 3: Implement in `include/weave/docvals.h`.** Header: `version` writers set to 2; `null_off` = `WEAVE_DV_MAXALIGN(docids_off + ndocs*8)` when a bitmap is present else 0; bitmap length `(ndocs + 7) / 8`. Validator: accept `version` 1 or 2; if `version == 1` require `null_off == 0`; if `null_off != 0` require it equals the aligned post-docids offset and `null_off + ceil(ndocs/8) <= len` (all in uint64, overflow-safe). `weave_docvals_isnull`: `null_off == 0 ? 0 : (bitmap[idx>>3] >> (idx&7)) & 1`. Evaluator: `if (weave_docvals_isnull(img, i)) continue;` before the switch. Test helpers as above; `_len_nulls` adds the aligned bitmap when `has_nulls`.

- [ ] **Step 4: Run test to verify it passes** — same command; Expected: PASS, prints the `checks, 0 failures` total, both coverage branches nonzero.

- [ ] **Step 5: Verify teeth** — flip the evaluator's null skip to `if (0)` in a scratch copy, rerun, confirm `prop_nulls` FAILS; revert. (positive control, per the eleventh member.)

- [ ] **Step 6: Commit** — `git add include/weave/docvals.h test/hegel/test_docvals.c && git commit -m "docvals: v2 store null bitmap + null-aware evaluator"`

### Task 2: Writer + accumulator null tracking

**Files:**
- Modify: `include/weave/am.h` (`WeaveDocvalsAccum`, `weave_docvals_accum_add` doc)
- Modify: `src/pages/docvals_page.c` (`weave_docvals_accum_init/reset/add`, `weave_docvals_write_weft`, the header writer at line ~110)

**Interfaces:**
- Consumes: T1's header layout + `weave_docvals_store_len_nulls` shape (same byte layout the writer must produce); existing `DocvalsPair`, `weave_docvals_accum_add_pair`.
- Produces:
  - `WeaveDocvalsAccum` gains `uint8 *isnull` (n bits packed OR n bytes — choose n bytes for simple append, packed at write time) and `uint32 nulls` (count).
  - `weave_docvals_accum_add(acc, tid, value, isnull)` records a NULL (does **not** ERROR); a NULL still appends a (docid, placeholder-0) pair so the docid stays in the dense space, and marks the null.
  - `weave_docvals_write_weft` emits a v2 store, writing the bitmap iff `acc->nulls > 0`.

- [ ] **Step 1: Write the failing test** — extend `test_docvals.c` (or a small backend-free harness) only if the writer is reachable standalone; otherwise the gate for this task is the T4 regression. Add a `StaticAssertDecl` in `docvals_page.c` that its locally-computed bitmap offset equals `WEAVE_DV_MAXALIGN(docids_off + ndocs*8)` (mirror-check against the header, the existing MAXALIGN-agreement pattern).

- [ ] **Step 2: Run the build to verify the current ERROR path** — confirm `weave_docvals_accum_add` still ereports on NULL before the change (read the code; this is the state T1 leaves).

- [ ] **Step 3: Implement.** `am.h`: add `isnull`/`nulls` to the struct with a comment; update the `weave_docvals_accum_add` contract comment (NULL is recorded, no longer refused). `docvals_page.c`: `accum_init` allocates nothing extra (lazy like docid/value); `accum_add` records null via a huge-safe-grown `isnull` array parallel to docid/value, appends the pair with value 0 when null; `accum_reset` NULLs the new pointer; `write_weft` sorts the triple by docid (extend `DocvalsPair` with an `isnull` field or carry a parallel index permutation), packs the bitmap when `nulls>0`, sets `version=2`, `null_off` accordingly; the standalone header writer sets `version=2`.

- [ ] **Step 4: Build** — `nix build .#pg17 -L >/dev/null 2>&1; echo $?` Expected: 0. Run `make check-alloc check-pdlower`.

- [ ] **Step 5: Commit** — `git add include/weave/am.h src/pages/docvals_page.c && git commit -m "docvals: writer + accumulator carry a null bitmap"`

### Task 3: Build / insert / flush / merge / scan null carriage

**Files:**
- Modify: `src/am/ambuild.c` (build callback ~937, pending insert ~5823-5848 + ~5969, flush, `weave_docvals_merge_append` ~3376)
- Modify: `src/am/amscan.c` (`weave_docvals_collect` ~4663 and the pending docval read path)
- Modify: `include/weave/am.h` (WeavePendingItem `dvlen` comment: `dvlen==0` on a docvals-bearing index now means NULL)

**Interfaces:**
- Consumes: T2's `weave_docvals_accum_add(...,isnull)`; T1's `weave_docvals_isnull`.
- Produces: build/insert/flush/merge all preserve null-ness; the gate excludes NULL docids everywhere (segment stores, pending items, merged stores).

- [ ] **Step 1: Write the failing test** — this task's gate is the T4 regression (index==heap with NULLs, before/after flush, after merge). Write T4's SQL first (it belongs to T4 but is the failing test for T3+T4 together); run it against the current tree to confirm it FAILS (build ERRORs on the NULL insert today).

- [ ] **Step 2: Run to verify it fails** — `bash /scratch/pg_weave/lpg.sh reset && bash /scratch/pg_weave/lpg.sh psql -f sql/docvals.sql` (or the nix installcheck): Expected FAIL — `weave docvalues column must not contain NULL`.

- [ ] **Step 3: Implement.** Build callback: pass `isnull[dvidx]` through (already does; T2 makes it non-fatal). Pending insert (`weave_insert_*`): when the index has a docvals column and the value isnull, write the item with `dvlen == 0` (no trailer); update the am.h comment. Pending scan / `weave_docvals_collect` pending leg: treat a docvals-bearing index's `dvlen == 0` item as NULL → excluded from every comparison gate. Flush (`weave_flush_pending`): a `dvlen == 0` pending docval on a docvals-bearing index folds into the segment as a NULL (accum_add with isnull=true). `weave_docvals_merge_append`: read each input store's bitmap via `weave_docvals_isnull` and carry null-ness into the merge accumulator (extend the pair-append to a null-aware append, or call `accum_add_pair` + a new `accum_mark_null`). `weave_docvals_collect` over segment stores already reads via the evaluator, which now skips nulls (T1) — verify it uses the evaluator and not a hand-rolled loop.

- [ ] **Step 4: Run T4 SQL to verify it passes** — Expected: index rows == heap rows for every qual, before and after flush, after merge.

- [ ] **Step 5: Commit** — `git add src/am/ambuild.c src/am/amscan.c include/weave/am.h && git commit -m "docvals: carry NULLs through build, insert, flush, and merge"`

### Task 4: Regression — index == heap with NULLs

**Files:**
- Modify: `sql/docvals.sql` (new §9), `expected/docvals.out`

**Interfaces:** Consumes T3's behavior.

- [ ] **Step 1: Write the test** — `sql/docvals.sql` §9: a table with a **nullable** facet column (mix of NULLs and values), `CREATE INDEX ... USING weave (... price int8_docval_ops)`, `SET enable_seqscan=off; SET enable_bitmapscan=off;`. For each of `< <= = >= >`: assert the weave-gated count/rowset equals the same qual under a seq scan (the `EXCEPT`-both-ways oracle already used in §8). Assert an `Index Cond` is present on the comparison (plan check, §8 threat 2). Assert `IS NULL` / `IS NOT NULL` fall back to a Filter (no Index Cond) and still return the heap-correct rows. Repeat the comparison oracle **after** a post-build `INSERT` of NULL and non-NULL rows (pending path) and **after** `VACUUM`/merge.

- [ ] **Step 2: Run to verify it fails** — on the pre-T3 tree (or note T3 already made it pass; if executing in order, this ran red in T3 step 2).

- [ ] **Step 3: Generate expected from a FULL run** — `nix build .#checks.x86_64-linux.installcheck-pg17 --keep-failed`; from the kept build dir copy `results/docvals.out` to `expected/docvals.out` **only after** proving the diff non-semantic (hard rule 3: normalize spaces/dashes, assert lines identical modulo padding). Never generate solo with `REGRESS=docvals`.

- [ ] **Step 4: Run both majors** — `nix build .#checks.x86_64-linux.installcheck-pg17 -L; echo $?` and `-pg18`. Expected: 0 / 0.

- [ ] **Step 5: Commit** — `git add sql/docvals.sql expected/docvals.out && git commit -m "docvals: regression — nullable facet index==heap, IS NULL falls back"`

### Task 5: Fuzz the null bitmap

**Files:**
- Modify: `test/fuzz/fuzz_docvals.c`

**Interfaces:** Consumes T1's validator + `weave_docvals_isnull`.

- [ ] **Step 1: Write the test** — extend the corpus to v2 stores with a null bitmap; mutate: `null_off` past end, `null_off` mid-values, bitmap truncated by 1 byte, `version==1` with `null_off!=0`, trailing bits beyond `ndocs` set. Assert the validator returns a reason (never accepts) for each malformed image, and that for a *valid* image `weave_docvals_isnull` reads only within `[null_off, null_off+ceil(ndocs/8))`. Keep the planted-bug variant that must abort.

- [ ] **Step 2: Run to verify teeth** — build the planted-bug variant, confirm it aborts; build the clean variant, confirm 0 failures. Command: the recipe already in `make check-standalone` for `fuzz_docvals` (`$(CHECK_CC) ... test/fuzz/fuzz_docvals.c -lm`).

- [ ] **Step 3: Run `make check-standalone`** — Expected: `== ALL STANDALONE CHECKS PASSED ==`, and `make check-fuzz` (clang ASan+UBSan) rc 0 where clang is available; else note it runs in CI.

- [ ] **Step 4: Commit** — `git add test/fuzz/fuzz_docvals.c && git commit -m "docvals: fuzz the v2 null bitmap region"`

### Task 6: Durability gates — crash-recovery, torn-write, concurrency (TAP)

**Files:**
- Create: `t/0NN_docvals_null_recovery.pl`, `t/0NN_docvals_null_tornwrite.pl`, `t/0NN_docvals_null_concurrent.pl` (pick the next free numbers)
- Modify: `flake.nix` (`PROVE_TESTS` list — same commit)

**Interfaces:** Consumes the shipped NULL behavior (T3).

- [ ] **Step 1: Write the tests.**
  - *Recovery:* build a nullable-facet docvals index, record the gate answer for each op; `-m immediate` stop, restart; assert answers survive and equal the heap. Init with `no_data_checksums => 1` only if it rewrites pages (recovery does not; the torn-write one does).
  - *Torn-write:* manufacture a torn docvals page (the `t/010`/`t/019` pattern), assert the reader ERRORs cleanly (validator), postmaster survives, no wrong answer. `init(..., no_data_checksums => 1)` (PG18 checksums-on, per AGENTS.md).
  - *Concurrency:* one session runs repeated docvals gate scans while another INSERTs rows including NULLs and non-NULLs; assert every scan's result is a consistent snapshot equal to a heap seq scan at that point (no torn/missing rows). Make the inserters genuinely concurrent (hard rule 11 / G28: assert they overlapped, e.g. via `pg_stat_activity` or a barrier — do not let them serialize).

- [ ] **Step 2: Wire into PROVE_TESTS** — add all three files; run `nix build .#checks.x86_64-linux.tap-pg17 -L; echo $?`. Expected: 0, and grep the TAP output for each new file's own `ok` lines (evidence it RAN, per the "suite running is not the site running" members).

- [ ] **Step 3: Run pg18** — `nix build .#checks.x86_64-linux.tap-pg18 -L; echo $?`. Expected: 0.

- [ ] **Step 4: Commit** — `git add t/0NN_*.pl flake.nix && git commit -m "docvals: crash-recovery, torn-write, and concurrency TAP for NULLs"`

### Task 7: Docs + version bump + gap closure

**Files:**
- Modify: `doc/specs/DOCVALS_CHANNEL.md` (§3/§6/§11 step 3 → DONE; note the `dvlen==0`-means-NULL pending encoding and its backward-safety), `doc/GAPS.md`, `doc/PHASES.md` (step-3 task status), `doc/PRODUCTION_READINESS.md` (gates 7–11 for docvals), `include/weave/docvals.h` file-header comment (lift "int8-only, no nulls"), extension version if any SQL changed (none expected → no bump; confirm).

- [ ] **Step 1: Update docs** — mark step 3 DONE with the commit refs and the T8 scale result; record the format-v2 decision and the pending-NULL encoding next to the code (CONVENTIONS "explanations go next to the thing"). If no SQL/opclass changed, state explicitly that no extension version bump is needed and why (the store's own `version` field self-describes; X1 old-`.so` gate unaffected).

- [ ] **Step 2: Commit** — `git add doc/ include/weave/docvals.h && git commit -m "docvals: document NULL support (step 3) and close the gap"`

### Task 8: EC2 scale run (hard rule 12 — merge/flush/vacuum touched)

**Files:**
- Modify: `bench/aws/docvals_scale.sql` (add a **nullable** facet, e.g. `nprice int8` ~10% NULL), `bench/RESULTS_DOCVALS_SCALE.md`

**Interfaces:** Consumes the shipped behavior; uses the existing `docvals` job in `bench/aws/run.sh`.

- [ ] **Step 1: Extend the workload** — add a nullable facet; the self-checking `disagreements=0` oracle must hold for the nullable column through: build, post-build INSERT (incl. NULLs) via pending, 40%-delete VACUUM (tombstone-drop rewrite), flush+merge. Keep it fatal on any disagreement.

- [ ] **Step 2: Run on EC2** — `AWS_PROFILE=hotdog` region `us-east-2`; launch via `bench/aws/run.sh` job `docvals`; detach with `setsid nohup … & disown`; pull artifacts incrementally (hard rule 14). Only ever touch `pgweave-*` resources.

- [ ] **Step 3: Record + verify** — assert the run's own `disagreements=0` marker for the nullable facet at 10M in every phase; write the numbers into `RESULTS_DOCVALS_SCALE.md` (losses as prominently as wins, hard rule 8). Sweep all EC2 instances/keys/SGs at the end; confirm zero pgweave strays.

- [ ] **Step 4: Commit** — `git add bench/aws/docvals_scale.sql bench/RESULTS_DOCVALS_SCALE.md && git commit -m "docvals: nullable facet validated at 10M on EC2"`
