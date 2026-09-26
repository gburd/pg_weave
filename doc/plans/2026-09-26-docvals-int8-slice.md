# Docvals channel — int8 vertical slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.
>
> **THIS PROJECT'S EXECUTION MODEL:** worker agents have **no compiler**. Every "build / run gate" step is performed by the **coordinator**, which is also the between-task review point. A worker's deliverable is the edit; the coordinator compiles (`nix build`), runs the gate, and only then checks the box. Never mark a build/gate step done without the coordinator's captured exit status (AGENTS.md "a result needs evidence the specific thing you meant to run, ran").

**Goal:** Ship the scalar/docvalues channel for a single `int8` facet column end-to-end — on-disk per-docid store, planner pushdown, and a gate that feeds the existing fused-scan seek — so that `WHERE price <op> const ORDER BY fuse(...)` gets *faster* as the facet tightens instead of slower.

**Architecture:** A docvals column becomes one weft per bolt (`WEAVE_WK_DOCVALS`, page `WEAVE_PK_DOCVALS`), a dense `int8` array keyed by segment-local docid behind a versioned header. At scan start the pushed comparison predicate is evaluated over that array into a sorted docid set, handed to the **existing** `weave_gate_shuttle_from_tidset(WEAVE_CH_DOCVALS, …)`; the fused core already seeks through it and skips vector work. No change to the fused core or the gate cursor.

**Tech Stack:** C (PostgreSQL core conventions), PGXS/nix build, GenericXLog, Hegel property tests, `installcheck`/TAP gates.

**Spec:** `doc/specs/DOCVALS_CHANNEL.md` (read it; this plan implements §§3–9, build-order step 1 of §11).

## Global Constraints

- **100 % GenericXLog** — no raw `XLogInsert`/`log_newpage`/`smgrwrite` (hard rule 2).
- **On-disk bytes are not trusted** — every field validated by a pure reader; a corrupt store `ERROR`s, never crashes, never returns a wrong gate set (CONVENTIONS §2).
- **The (C5) correctness contract** — the emitted gate set equals *exactly* the docids whose stored value satisfies the predicate, which equals the heap's answer. No false negatives (hard rule 1: a channel without its bound/exactness property test is not merged).
- **Reloption/GUC rule** — nothing session-dependent changes on-disk bytes.
- **PostgreSQL license, pure C, no new dependency; no code from `~/src/zvec`** (hard rule 6).
- **v1 scope this plan:** one `int8` column, operators `< <= = >= >`, `NOT NULL`, single segment (no merge, no NULLs, no text — those are later plans per spec §11). Extension bump `0.23.0 → 0.24.0`.
- Build/gate incantations (coordinator): `nix build .#pg17 -L`; `nix build .#checks.x86_64-linux.installcheck-pg17 -L`; `installcheck-pg18`; `tap-pg17`. Standalone C: `gcc -O2 -I include -o /scratch/pg_weave/<t> test/hegel/<t>.c <srcs> -lm`.

---

### Task 1: Backend-independent store, validator, and predicate evaluation

The crux and the only place novel correctness lives. Header-only inline functions like `include/weave/for.h`, so the same code is testable with `gcc` and no backend (CONVENTIONS exemplar). Fixed-width `int8` array behind a versioned header with reserved space for the deferred zone-map (spec §3).

**Files:**
- Create: `include/weave/docvals.h`
- Test: `test/hegel/test_docvals.c`

**Interfaces:**
- Produces (consumed by Tasks 2, 6):
  - `typedef struct WeaveDocvalsHeader { uint32 magic; uint16 version; uint16 typid_kind; uint32 ndocs; uint32 null_off; uint32 zonemap_off; uint32 values_off; uint32 reserved; } WeaveDocvalsHeader;` — `magic=0x57445631` ("WDV1"), `version=1`, `typid_kind=1` for int8, `null_off=0`/`zonemap_off=0` in this slice, `values_off=MAXALIGN(sizeof header)`.
  - `typedef enum { WEAVE_DV_LT=1, WEAVE_DV_LE=2, WEAVE_DV_EQ=3, WEAVE_DV_GE=4, WEAVE_DV_GT=5 } WeaveDvStrat;` — btree strategy numbering (spec §4).
  - `const char *weave_docvals_validate(const void *img, size_t len);` — returns NULL if the image is a structurally valid v1 int8 store of its stated `ndocs`, else a static reason string.
  - `static inline int64 weave_docvals_int8(const void *img, uint32 docid);` — value at docid (caller guarantees `docid < ndocs`; asserts).
  - `int weave_dv_eval_int8(const void *img, WeaveDvStrat op, int64 c, uint32 *out, uint32 outcap);` — writes ascending docids satisfying `value(docid) op c` into `out` (cap `outcap`, = ndocs), returns count. Pure, no allocation.

- [ ] **Step 1: Write the failing property test**

`test/hegel/test_docvals.c` — build a store from a random `int64[]`, then assert `weave_dv_eval_int8` emits exactly the docids a straight-line reference loop emits, for every operator and random constants (including values present/absent and INT64_MIN/MAX), plus `weave_docvals_validate` accepts it and rejects a truncated / bad-magic / wrong-ndocs image. Use the Hegel harness style of `test/hegel/test_for.c` (read it for the assert/iteration macros).

```c
/* core assertion, per (op, c) over N random docs */
static void check_one(const int64 *v, uint32 n, WeaveDvStrat op, int64 c) {
    uint8_t *img = build_store(v, n);            /* header + MAXALIGN + n*8 bytes */
    assert(weave_docvals_validate(img, store_len(n)) == NULL);
    uint32 got[MAXN]; int ng = weave_dv_eval_int8(img, op, c, got, n);
    uint32 exp[MAXN]; int ne = 0;
    for (uint32 d = 0; d < n; d++) {
        int64 x = v[d]; int hit =
            (op==WEAVE_DV_LT && x<c) || (op==WEAVE_DV_LE && x<=c) ||
            (op==WEAVE_DV_EQ && x==c) || (op==WEAVE_DV_GE && x>=c) ||
            (op==WEAVE_DV_GT && x>c);
        if (hit) exp[ne++] = d;
    }
    assert(ng == ne);
    for (int i = 0; i < ne; i++) assert(got[i] == exp[i]);   /* ascending, exact */
    free(img);
}
```

- [ ] **Step 2: Run it to verify it fails to compile** (coordinator)

Run: `gcc -O2 -I include -o /scratch/pg_weave/tdv test/hegel/test_docvals.c -lm`
Expected: FAIL — `include/weave/docvals.h` does not exist / functions undefined.

- [ ] **Step 3: Write `include/weave/docvals.h`** — the struct, enum, and the three functions above as `static inline`, plus `build_store`/`store_len` test helpers guarded by `#ifdef WEAVE_DOCVALS_TEST_HELPERS` (so the test builds a store without the backend page writer). `weave_dv_eval_int8` is a single pass; `weave_docvals_validate` checks `len >= values_off + (size_t)ndocs*8`, magic, version, and that offsets are `MAXALIGN`ed and non-overlapping.

- [ ] **Step 4: Compile and run the test** (coordinator)

Run: `gcc -O2 -DWEAVE_DOCVALS_TEST_HELPERS -I include -o /scratch/pg_weave/tdv test/hegel/test_docvals.c -lm && /scratch/pg_weave/tdv`
Expected: prints its own total (`N checks, 0 failures`) and exits 0. (AGENTS.md eleventh member: the test must print its own total, not be judged by a pipe.)

- [ ] **Step 5: Wire into `make check-standalone`** — add `test_docvals` to the target's suite list the way `test_pagekind`/`test_lexbound` are (each redirects to a log and checks its own exit status — do not use `| tail`). Add a positive control: a planted-bug leg (`build_store` writing one wrong value) that must make the suite exit non-zero, proving the gate can fail (AGENTS.md eleventh member).

- [ ] **Step 6: Commit**

```bash
git add include/weave/docvals.h test/hegel/test_docvals.c Makefile
git commit -m "docvals: backend-independent int8 store, validator, predicate eval + property test"
```

---

### Task 2: Page writer/reader and fuzz target (`WEAVE_PK_DOCVALS`)

Persist the Task 1 image on a GenericXLog page chain and read it back as a contiguous image. The vector weft (`src/vector/vecwrite.c`) is the exemplar for "write value pages, record the root last"; `src/pages/trgm_page.c` is the exemplar for a blob-over-page-chain reader with a validated `pd_lower`.

**Files:**
- Create: `src/pages/docvals_page.c`
- Modify: `include/weave/am.h` (declare the two functions, with the rationale-comment style that file requires)
- Create: `test/fuzz/fuzz_docvals.c`; Modify: `Makefile` (OBJS + fuzz list), `flake.nix` if fuzz is gated there

**Interfaces:**
- Produces (consumed by Tasks 4, 6):
  - `BlockNumber weave_docvals_write(Relation index, GenericXLogState *st, const int64 *vals, uint32 n);` — lays the header + value array across `WEAVE_PK_DOCVALS` pages using `weave_new_buffer`/GenericXLog exactly as `vecwrite.c` does, returns the root block. **`check-alloc`: any `cap`/size from `n` uses the huge-safe variant** (n is corpus-scale).
  - `const void *weave_docvals_load(Relation index, BlockNumber root, MemoryContext cxt, uint32 *ndocs_out);` — reassembles the contiguous image into `cxt`, runs `weave_docvals_validate` and `ERROR`s on a non-NULL reason. Read `pd_lower` only via `weave_page_entry_end()` (check-pdlower).

- [ ] **Step 1: Write the failing test** — extend `test/hegel/test_docvals.c` (or a new `sql/`-level check in Task 7) is not enough here; write `test/fuzz/fuzz_docvals.c` that feeds random bytes to `weave_docvals_validate` and asserts it never reads out of `len` and either returns NULL or a reason (never crashes), plus a **planted-bug** build (`#ifdef PLANT` corrupts an offset) that must abort — the pattern `fuzz_chandesc.c` uses.
- [ ] **Step 2: Run to verify it fails** (coordinator) — `gcc`-compile the fuzz target against `docvals.h`; expect undefined `weave_docvals_load`/link error until Step 3, and the planted-bug leg to NOT yet abort.
- [ ] **Step 3: Implement** `src/pages/docvals_page.c` (`weave_docvals_write`, `weave_docvals_load`) following `vecwrite.c`'s chain-write and `trgm_page.c`'s validated blob read. Declare both in `include/weave/am.h`.
- [ ] **Step 4: Build + fuzz** (coordinator) — `nix build .#pg17 -L` (must exit 0; capture status, `make clean` implied by nix); run `fuzz_docvals` (≥100k cases, prints its own total) and the planted-bug leg (must abort). Wire `fuzz_docvals` into the fuzz list.
- [ ] **Step 5: Commit**
```bash
git add src/pages/docvals_page.c include/weave/am.h test/fuzz/fuzz_docvals.c Makefile
git commit -m "docvals: WEAVE_PK_DOCVALS page writer/reader + fuzz target"
```

---

### Task 3: `int8_docval_ops` opclass and extension bump 0.24.0

Make a column join the docvals channel via its opclass, matching `wvec_weave_ops` (spec §4). No scan behavior yet — this task's deliverable is that `CREATE INDEX … USING weave (body wdoc_lex_ops, emb wvec_ip_ops, price int8_docval_ops)` succeeds and the layout resolves the docvals attnum.

**Files:**
- Create: `sql/pg_weave--0.23.0--0.24.0.sql`; Modify: `pg_weave.control` (`default_version=0.24.0`), `Makefile` (DATA)
- Modify: `src/am/am.c` — add `{"int8_docval_ops", WEAVE_WK_DOCVALS}` to `weave_opfamily_kinds[]` (~4066) and a `WEAVE_WK_DOCVALS` arm to the layout resolver (~4180, mirroring the `WEAVE_WK_VECTOR` arm) setting a new `layout.dvattno`
- Modify: `include/weave/am.h` — add `AttrNumber dvattno;` to `WeaveIndexLayout` with a rationale comment

**Interfaces:**
- Produces (Tasks 4,5,6): `layout.dvattno` (0 when the index has no docvals column); opclass `int8_docval_ops` with `OPERATOR 1 <(int8,int8) … 5 >(int8,int8)` and `FUNCTION 1 btint8cmp` in a new opfamily.

- [ ] **Step 1: Write the failing test** — add to a scratch `sql` snippet (promoted in Task 7): `CREATE INDEX t_w ON t USING weave (body wdoc_lex_ops, emb wvec_ip_ops, price int8_docval_ops);` and `SELECT weave_index_stats('t_w');` — expected to fail today with "operator class int8_docval_ops does not exist".
- [ ] **Step 2: Run to verify it fails** (coordinator) — against the local `lpg` cluster.
- [ ] **Step 3: Implement** — the upgrade SQL creates the opfamily/opclass (DROP+CREATE not needed, it is new); register in `weave_opfamily_kinds[]`; extend the layout resolver + `WeaveIndexLayout`. Bump control + Makefile DATA.
- [ ] **Step 4: Build + create index** (coordinator) — `nix build .#pg17`; on `lpg`, `ALTER EXTENSION pg_weave UPDATE TO '0.24.0'`; the `CREATE INDEX` above succeeds and `weave_index_stats` returns.
- [ ] **Step 5: Commit**
```bash
git add sql/pg_weave--0.23.0--0.24.0.sql pg_weave.control Makefile src/am/am.c include/weave/am.h
git commit -m "docvals: int8_docval_ops opclass, layout.dvattno, extension 0.24.0"
```

---

### Task 4: Collect values at build time and attach the weft

Populate the store during index build and record its root in the chandesc, in ascending `(kind,attnum)` order (spec §9). Mirror the vector accumulator (`weave_vec_accum_add`) and `weave_attach_chandesc`.

**Files:**
- Modify: `src/am/ambuild.c` — a `WeaveDocvalsAccum` in `WeaveBuildState`; collect in `weave_build_callback` (~786, alongside the vec/cgram `values[...]` reads); finalize writes the store via `weave_docvals_write`
- Modify: `src/am/am.c` — `weave_chandesc_for_segment`/`weave_attach_chandesc` (~2644) grow a `dvroot`/`dvattno`; `weave_free_segment` (~3799) gains a `WEAVE_WK_DOCVALS` free arm; `weave_check` chandesc-reachability already covers it
- Modify: `include/weave/am.h` — `weave_attach_chandesc` signature

**Interfaces:**
- Consumes: `weave_docvals_write` (T2), `layout.dvattno` (T3).
- Produces (T6): a bolt whose `chandesc` has a `WEAVE_WK_DOCVALS` weft rooting a valid store; `weave_docvals_root_for_segment(seg, &attnum)` helper (mirror `vecwrite.c`'s root lookup ~1007).

- [ ] **Step 1: Write the failing test** — a TAP or `sql` check: build a 2,000-row index with an int8 `price`, then `SELECT (weave_check('t_w')).*` asserts `chandesc_reachable` true and a docvals weft is present; and a new `weave_docvals_dump('t_w', segno)` debug SRF (or reuse `weave_page_info`) shows `WEAVE_PK_DOCVALS` pages. Expected to fail: no weft written.
- [ ] **Step 2: Run to verify it fails** (coordinator).
- [ ] **Step 3: Implement** — accumulator + callback collection + finalize write + chandesc attach in `(kind,attnum)` order (WEAVE_WK_DOCVALS=4 between FUZZY=3 and CGRAM=5 — the ordering `am.c:2575` warns about) + free arm.
- [ ] **Step 4: Build + gates** (coordinator) — `nix build .#pg17`; `installcheck-pg17` green; `weave_check` reports the weft reachable; `check-alloc`/`check-pdlower` clean.
- [ ] **Step 5: Commit**
```bash
git add src/am/ambuild.c src/am/am.c include/weave/am.h
git commit -m "docvals: write the int8 store at build and attach the weft in chandesc order"
```

---

### Task 5: Planner pushdown — the docvals qual becomes an Index Cond

Recognize `dvcol <op> const` and route it into the index as a gate, not an executor Filter (spec §8). Threat: a missing index condition is silent and yields a flat curve that measures the executor — so the deliverable is asserted on the plan shape.

**Files:**
- Modify: `src/am/customscan.c` (the qual-matching path ~200) and/or the `amrestrpos`/`amgetbitmap` index-qual plumbing so a `WEAVE_STRAT_*`-numbered docvals operator on `layout.dvattno` is accepted as an index clause.

**Interfaces:**
- Consumes: `layout.dvattno` (T3). Produces (T6): the scan receives the docvals qual as an `IndexScan` `Index Cond` with strategy + constant reachable in `weave_rescan`/scan-key setup.

- [ ] **Step 1: Write the failing test** — `EXPLAIN (COSTS OFF)` of `SELECT id FROM t WHERE price < 100 ORDER BY fuse(body <=> …, emb <#> …) LIMIT 10` with `enable_seqscan`/`enable_bitmapscan` off, asserting the plan carries `Index Cond: (price < 100)`. Expected fail: today it is `Filter:` (confirmed for `id % N` in RESULTS_DOCVALS_PRIZE.md).
- [ ] **Step 2: Run to verify it fails** (coordinator).
- [ ] **Step 3: Implement** the pushdown recognition. (No gate execution yet — Task 6 consumes the scan key.)
- [ ] **Step 4: Build + assert plan** (coordinator) — plan shows `Index Cond` on the docvals qual.
- [ ] **Step 5: Commit**
```bash
git add src/am/customscan.c
git commit -m "docvals: push a comparison on a docvals column into the index as a gate cond"
```

---

### Task 6: Build the gate set at scan start and feed the existing gate shuttle

The payoff. At scan start, evaluate the pushed predicate over the loaded store into a sorted docid set and hand it to `weave_gate_shuttle_from_tidset(WEAVE_CH_DOCVALS, …)` — the fused core already seeks through it (spec §2, §5). Multiple docvals quals AND by intersection (kept minimal here: support one, structure for many).

**Files:**
- Modify: `src/am/amscan.c` — near the `so->plainInit`/`plainTids` materialization (~2783, ~8244): when a docvals scan key is present, `weave_docvals_load` the store for the bolt, `weave_dv_eval_int8` into a docid array, convert into the gate's key space exactly as the lexical `plainTids` path does, and pass to `weave_gate_shuttle_from_tidset` (the call at 8272 already exists — feed it the docvals set instead of only the query TIDs).

**Interfaces:**
- Consumes: `weave_docvals_load` + `weave_dv_eval_int8` (T1/T2), the scan key (T5), `weave_gate_shuttle_from_tidset` (existing).
- Produces: correct rows for `WHERE price <op> c ORDER BY fuse(...)`.

- [ ] **Step 1: Write the failing test** — `sql`: results of `… WHERE price < 100 ORDER BY fuse(...) LIMIT 10` equal the same query answered with `enable_indexscan` off / seq scan (identical rows, identical order). Expected fail: gate not built, rows differ or the qual is ignored.
- [ ] **Step 2: Run to verify it fails** (coordinator).
- [ ] **Step 3: Implement** the gate-set materialization + shuttle feed.
- [ ] **Step 4: Build + gates** (coordinator) — `nix build`; the index-vs-seq agreement holds; `installcheck-pg17`/`pg18` green.
- [ ] **Step 5: Commit**
```bash
git add src/am/amscan.c
git commit -m "docvals: materialize the gate set from the store and drive the fused seek"
```

---

### Task 7: Regression test — index vs heap agreement (the (C5) SQL half)

The spec §7 SQL property, pinned. Also regenerates expected output for the 0.24.0 NOTICE cascade (hard rule 3: prove the diff non-semantic).

**Files:**
- Create: `sql/docvals.sql`, `expected/docvals.out`; Modify: `Makefile` (REGRESS), regenerate the version-NOTICE files.

- [ ] **Step 1: Write `sql/docvals.sql`** — build an index with an int8 facet; for a range of constants (dense → selective), assert the gated fused query's row set == the seq-scan answer; assert the plan carries `Index Cond` (threat 2); include a `count(*) WHERE price = k` cross-check; DROP at end.
- [ ] **Step 2: Generate expected** (coordinator) — `nix build .#checks.x86_64-linux.installcheck-pg17 --keep-failed`; copy `results/docvals.out`; for every OTHER changed `.out`, prove the diff is only the `version "0.24.0" is already installed` NOTICE (hard rule 3) before copying.
- [ ] **Step 3: Run to verify green** (coordinator) — `installcheck-pg17` and `installcheck-pg18` exit 0, 0 `not ok`.
- [ ] **Step 4: Commit**
```bash
git add sql/docvals.sql expected/*.out Makefile
git commit -m "docvals: regression test for index-vs-heap agreement; regen 0.24.0 expected"
```

---

### Task 8: End-to-end proof — re-run the prize spike on a real docvals column

The slice's gate (spec §11 step 1): the scalar arm must now *fall* with selectivity like the lexical arm, capturing the prize `RESULTS_DOCVALS_PRIZE.md` measured against the un-pushable filter.

**Files:**
- Modify: `bench/RESULTS_DOCVALS_PRIZE.md` (add a "MEASURED WITH THE CHANNEL" section); a throwaway harness in `/scratch/pg_weave/`.

- [ ] **Step 1:** On `lpg`, add an int8 facet to `normfiqa`/`normsci` **without churning the vector weft** — build a fresh index that includes `price int8_docval_ops` where `price` is a precomputed column (add the column, then `REINDEX`, so the warp map is rebuilt in docid order — the churn lesson from the sizing spike).
- [ ] **Step 2:** Run the two arms from the sizing spike, but arm S now uses `WHERE price <op> const` (pushable) instead of `id % N` (filter). Capture `vec_blocks` across selectivity 0.1/0.01/0.001. (Coordinator; work counters are deterministic/host-independent — no EC2 needed.)
- [ ] **Step 3: Assert the prize is captured** — arm S `vec_blocks` now *falls* with selectivity (matching arm L within the selectivity-match tolerance), not grows. If it does not, the gate is not pruning — stop and diagnose (reach for the ablation: is the Index Cond present? is the gate set non-empty and sorted?).
- [ ] **Step 4:** Record the measured before/after in `RESULTS_DOCVALS_PRIZE.md` (losses as prominently as wins — if it captures less than the projected 82.8×, say by how much and why).
- [ ] **Step 5: Commit**
```bash
git add bench/RESULTS_DOCVALS_PRIZE.md
git commit -m "docvals: int8 slice captures the prize end-to-end (measured)"
```

---

## What this slice deliberately does NOT do (later plans, spec §11)

float/int4/int2/date/bool types; NULLs + null bitmap; text dictionary encoding + collation; **`aminsert`/flush of docvals values for post-build rows** (so a docvals gate on an un-flushed row is the G29 limitation until then — a later plan writes the value into the pending buffer alongside the vec/gram values, spec §9); merge (re-dictionary, concatenation); multiple docvals columns; `IS NULL` pushdown; the zone-map. Each is additive on the v1 format. Steps touching merge/vacuum are provisional until a 1M scale run (hard rule 12).
