# Build-out phases

This is the execution contract. Every task has an id, a spec, an owner file, and
a **gate** — a mechanically checkable condition that must hold before the task is
considered done. A task without a passing gate is not done, regardless of how
much code exists.

Read `doc/CONVENTIONS.md` before writing code and `doc/TESTING.md` before
claiming a gate.

## Two tasks are withdrawn on upstream evidence

**L4 (parallel merge) and L11 (parallel scan) are withdrawn**, not deferred. Both
were listed here as routes to G5 and G11 and both were quoted to the maintainer as
candidates. pg_fts has since measured L4 (1.45× slower, 19% bloatier) and
re-analysed L11 (already built and reverted, with an Amdahl ceiling that cannot
close the gap). Carrying disproven work in a plan is worse than having a shorter
plan, because it makes the remaining gap look addressable when it is not.

The consequence for **G5 (build time, 11.0× behind pg_textsearch)** is that **L12 is
now the only remaining route.** If L12 does not deliver, the honest position is that
pg_weave builds more slowly than its competitors and that is the price of a
compacted, smaller index — and the README should say so rather than implying a fix
is pending.

The consequence for **G11 (no parallel scan)** is that it is no longer a gap to
close but a permanent characteristic, and it belongs in
`doc/ARCHITECTURE.md` §8's list of things pg_weave loses.

**Process change:** upstream is reviewed on every release, not on request. Both of
these had been measured upstream before I quoted them as open work, and the phrase
correctness bug ported in `c0d65a7` was live and default-on here for days.

## Ordering principle

**The product decides the order.** pg_weave is a **singular text index**: BM25,
vector similarity, fuzzy, approximate regex, prefix, and n-gram over one docid
space, in one `CREATE INDEX`. All six ship. So phases L, Z and V are all on the
path to 1.0 and the only open question is their sequence.

*This paragraph was rewritten twice on 2026-09-10 -- first to drop Z from the path
when the product was read as "BM25 + vector", then back when the scope was restated
as all six. Both rewrites are in `git log`. Recorded because a plan that changes
under you is worth less than one that says why it changed.*

Channels land **one at a time, fully**, in the order of decreasing certainty: the
lexical channel is field-tested, the fuzzy channel is imported code needing wiring,
the vector channel is new code implementing a published algorithm, and the fused
scorer is the only genuinely novel piece. Building the novel piece last means it is
built against working channels instead of simultaneous unknowns.

**The sequence, and the one deliberate exception to certainty ordering.** Z before
V, because Z is months where V is many months, its code is already imported and
compiling, TRE is current at `f864ed0`, and phase X just landed the kind space and
per-bolt descriptors that Z3's new page kind needs. But one piece of V jumps the
queue: **V9's recall assumption gets measured before either channel is built.**
Hard rule 9 exists for exactly this -- pg_turbovec measured IVF's probe count
imposing a recall ceiling no rerank window moves, which may put Phase V's
`recall@10 >= 0.99` gate out of reach *by design*. That question costs an afternoon
with a standalone program now and costs months if V7/V8/V9 are built first. So:

```
  L tail + V9 recall de-risk  ->  Z3-Z9  ->  V7-V14  ->  F
```

The de-risk is a *measurement*, not a channel, and it does not violate the
one-channel-at-a-time rule.

Corollary: **do not start F1 before L, Z, and V are green.** The temptation will
be strong because F is the interesting part. Resist it. A fused scorer debugged
against a half-working channel will consume more time than all of them.

Second corollary, and the reason phase X exists: **the format substrate both
remaining channels share had to land before either of them.** The page-kind space
was a `uint16` bitmap with bits 0-9 spent, and the vector and fuzzy channels each
wanted four more — they do not both fit, so whichever channel shipped first would
have baked in a collision the other could only resolve with a REINDEX.

```
  phase 0  DONE   repository, build, lexical channel forked, all tests green
  phase L         lexical channel: pay down the inherited debt
  phase X  DONE   format substrate (v6): kind space + per-bolt weft descriptors
  phase Z         fuzzy / approx-regex / prefix / n-gram: wire the pg_tre import
  phase V         vector channel: new C code -- the long pole
  phase F         fused-threshold top-k: the novel part
  phase M         migration/compatibility surface
  phase P         performance: the numbers that justify the claims
  phase R         1.0: docs, packaging, PGXN, contrib submission
```

---

## Phase 0 — foundation (DONE)

| id | task | gate | status |
|---|---|---|---|
| 0.1 | Fork pg_fts 1.5.8, rename to weave namespace | `make check-rename` clean; `ci/fork-rename.sh` reproduces the tree | done |
| 0.2 | Relayout to `src/` + `include/weave/` | `make` clean, zero warnings under PostgreSQL's warning set | done |
| 0.3 | Build infra: PGXS, meson, nix, PGXN META | `nix flake check` green | done |
| 0.4 | Verify inherited test suite | 3 regression + 2 isolation + 61 TAP green on PG17 **and** PG18 | done |
| 0.5 | Import pg_tre fuzzy sources, unwired | `doc/specs/IMPORT_pg_tre.md` exists with the wiring TODO | done |

---

## Phase L — lexical channel debt

The forked channel works and is fast on rare and mid-frequency terms. It has
three measured weaknesses, all documented in `pg_fts/bench/`, and they are the
cheapest wins available anywhere in this project.

| id | task | spec | gate |
|---|---|---|---|
| ~~**L1**~~ | **DONE 2026-09-10.** Split the 7,260-line `src/am/am.c` unity build into `am.c` 2,337 (AM core, page/segment/metapage machinery) / `ambuild.c` 3,895 (build, insert, segment writers, merge) / `amvacuum.c` 988 (bulkdelete, cleanup, compaction) / `amscan.c` 5,339, with `src/query/lev.c` and `src/pages/trgm_page.c` promoted to ordinary TUs in `OBJS` and `meson.build`. 37 symbols lost `static`, all declared in `include/weave/am.h` with per-declaration rationale. | **MET, with mechanical evidence:** `make check-unity` deleted and AGENTS.md rule 5 retired; every TU compiles standalone with zero warnings; regression **byte-identical** (zero `expected/*.out` changes, pg17 6+2, pg18 6+2, TAP 105). Object-code comparison of the LTO'd `.so`: 422 sized symbols both sides with 415 identical in size, no linkage-class or import change, 301 of 310 `.text` functions byte-identical after normalizing `__LINE__`; the 9 that differ all call a symbol that crossed a TU boundary and differ only in register allocation, stack slots or inlining. A byte-identical `.so` is unachievable since `__FILE__`/`__LINE__` must change, so per-function equivalence is the strongest available claim |
| L2 | **Common-term ranked latency — ROOT CAUSE CORRECTED 2026-09-10, DEMOTED BELOW L17.** The original entry asserted "decode-bound posting scan on high-df terms" and prescribed impact-ordered posting blocks (an on-disk format change) on the strength of that sentence, with no profile. `bench/RESULTS_SCAN_PROFILE.md` measured it: posting decode, BM25 and WAND together are **~28%** of a ranked scan; **~72% is the doclen cursor** (now L17). Impact ordering attacks the 28%. It is **not withdrawn**, and after L17 it is RE-AIMED: L17 fixed the sparse-term cost (rare 1.87×, mid 1.66×) but left `common` flat at 14.9 ms, because at df 1.74M of 2M the candidate stride is ~1.15 docids and the scan genuinely reads 1.74M postings — which is exactly what impact ordering and earlier block-max termination attack. **L2 is now the route for the common band specifically** (4.75× behind pg_textsearch, the worst remaining ratio). Its stated baseline (74.7 ms at 2.19M) predates L7/L12/L13/L14 and is stale (now 15.35 ms common k=10). | `doc/specs/IMPACT_ORDERING.md` (write it) | common-term (df > 5%) ranked k=10 p50 ≤ 8 ms; rare/mid must not regress by >10%. Re-derive the gate after L17 |
| ~~L3~~ | ~~**Doclen sidecar per-scan tax.**~~ **CLOSED 2026-09-10 — the gap does not exist in this codebase.** L3 inherited the claim that the v4 sidecar "decodes the whole segment sidecar per scan, a fixed ~18 ms" and prescribed a page-directory random-access cursor. pg_weave already has that cursor and does no whole-sidecar decode. Measured A/B (`bench/RESULTS_SCAN_PROFILE.md`): `doclen_sidecar=on` vs `off` is identical within noise in all three bands, at **625 MB vs 859 MB** — the sidecar is a 234 MB win at no latency cost. The equality is two costs cancelling (sidecar: cheap postings + cursor lookups; inline: 37% more posting data + free doclen), which is *why* making the cursor cheaper (L17) is a strict win rather than a wash. | `bench/RESULTS_SCAN_PROFILE.md` | closed |
| ~~L4~~ | ~~**Parallel merge.**~~ **WITHDRAWN 2026-09-08 — measured upstream and it is a loss.** pg_fts `fa4c15e`: on 2.19M docs an 8-segment 7,185 MB index merges in **230.6 s serial vs 333.5 s parallel (1.45× SLOWER)** and the parallel path emits a **19% LARGER** index (10,229 vs 8,606 MB). W=1 costs the same as W=3, so it is a fixed penalty for taking the path, not a scaling curve. Suspected cause: per-worker output streams pack pages independently. Also a trap worth knowing: at `max_parallel_maintenance_workers=8` the workers register, start and exit within ~2 ms so the merge silently runs SERIAL — the "fast" runs were the serial path. | — | withdrawn |
| L5 | Positions default review. Phrase is 8500 ms with positions unbuilt vs pg_search's 24.84 ms. Decide and document whether `positions=on` becomes the default; measure the size cost on the same corpus. | — | a recorded decision in `bench/RESULTS_POSITIONS.md` with both numbers, and the default set accordingly |
| L6 | Storage AIO: use `read_stream` for posting-page prefetch. Pointer-chained pages currently defeat readahead. | — | measurable p50 improvement on cold-cache common-term scan; no regression warm |
| ~~**L7**~~ | **DONE 2026-09-06.** **Keyless ordering scan.** `ORDER BY d <=> q LIMIT k` with no `WHERE` must generate an index path. Today it silently falls back to Seq Scan + top-N Sort: measured 83 ms par4 / 362 ms serial vs **0.05 ms** for the supported form — a 7,000× cliff on the first query any user writes, because pgvector taught them that shape. `bench/RESULTS_LEXICAL.md` §Footgun, `doc/GAPS.md` G1. | `sql/orderby.sql` | **MET:** bare form produces `Index Scan ... Order By`; p50 **0.05 ms == the qualified form**; `@@@`-parity, boolean-structure, score-equality, and LIMIT-overrun assertions all hold |
| ~~**L8**~~ | **DONE 2026-09-07.** **Deterministic index size.** A fresh `CREATE INDEX` measures 156 MB and the same index after `weave_merge` + `weave_vacuum` measures 115 MB — a 35% swing on whether an optional maintenance step ran. `doc/GAPS.md` G6. | — | **MET:** as-built 46 MB == compacted 46 MB, swing 0.0%; `weave_merge`/`weave_vacuum` both return false on a fresh build; `bench/lexical.sh` now aborts if either fails |
| ~~**L9**~~ | **DONE 2026-09-10 (measurement task; it produced L17).** **Attribute the fixed per-scan cost.** `bench/scan_profile.sh` + `bench/RESULTS_SCAN_PROFILE.md`: **~72% of a ranked mid k=10 scan is the doclen sidecar cursor** (`weave_doclen_cursor_load_page` 64.6% self, `_lookup` 7.3%), of which only 1.9% is buffer lookup — the pages are resident, so it is locate+decode, not I/O. `EXPLAIN` shows **11,702 buffer hits to return 10 rows** (df 39,683 ⇒ one page touch per 3.4 candidates, against the cursor's documented intent of ~1 per 128). Posting decode + BM25 + WAND are the other ~28%. | `bench/RESULTS_SCAN_PROFILE.md` | **MET** — the fixed cost is attributed by caller, and the profile chose L17 over L2 |
| ~~**L10**~~ | **DONE 2026-09-07.** **`weave_index_size_detail()`** — bytes per structure (dictionary, block index, postings, positions, doclen sidecar, livedocs, trigram) so the 1.7× size gap against GIN is attributed rather than guessed. `doc/GAPS.md` G2. | — | **MET:** function exists and sums to `pg_relation_size`; breakdown recorded. It also found that 70.7% of a fresh index was freed pages, which closed G2 as a *win* |
| ~~**L11**~~ | ~~**Parallel scan.**~~ **WITHDRAWN 2026-09-08 — already built, measured and deliberately reverted upstream.** pg_fts `a513d13`: a complete parallel ranked CustomScan was built, verified byte-exact, measured and reverted (`bench/NOTE_PARALLEL_RANKED.md` opens with "built, measured, reverted"). Amdahl p=0.88 gives W=8 a best case of 8.3 ms (realistically ~11.8) against pg_search's 2.12 ms — about 4.4× of a ~17× gap for 8 CPUs, and the shipped `max_parallel_workers_per_gather=2` default would give users ~20 ms. Two further blockers surfaced on re-analysis: `nsegments=1` is ENFORCED by insert-time tiered merge and autovacuum compaction, so per-segment parallelism divides by one; and workers refused to launch from inside `ExecCustomScan` on EC2 (0 workers, silent serial fallback). | — | withdrawn |
| ~~**L12**~~ | **DONE 2026-09-08.** **Low-bias end-of-build merge.** L8's vacate+pack pass roughly doubles build write I/O, taking build from 12.6 s to 29.2 s against GIN's 11.7 s. Make the end-of-build merge allocate output pages from the low free region so the result is front-packed and a plain tail truncation suffices, removing the relocation pass. Viable because at end of build the freed inputs are ~70% of the file and the live output ~30%, so the low free region exceeds the live segment; `weave_vacuum_compact`'s two-phase relocation exists for the harder general case where it does not. `doc/GAPS.md` G5. | `bench/RESULTS_L12.md` | **PARTIALLY MET: 495.9 s → 328.0 s (1.51×) with size unchanged.** The literal target (≤15 s) was never achievable — it assumed the vacate was the whole build cost, when it is one of several (analysis, sort, posting construction, WAL) and only its compaction half is removed. Gap to pg_textsearch 11.0× → 6.96×; to GIN 2.10× → 1.37× |
| ~~**L15**~~ | **DONE 2026-09-10.** **Cut the build's hash cost.** `bench/RESULTS_BUILD_PROFILE.md` attributed 37.5% of build time to `hash_search_with_hash_value`. Three steps, each measured: (1) length-aware `TermKey` (`HASH_FUNCTION`/`HASH_COMPARE`/`HASH_KEYCOPY`) — **3.9%**, and the hash's share *rose*, proving the cost was pointer chasing, not hashing; (2) the streaming merge no longer builds a per-term dynahash and probes it once per posting for its single entry — **19.5%**; (3) the doclen sidecar collector, a `uint64`-keyed dynahash `HASH_ENTER`'d ~240M times by both the writer and the merge and never named by the profile, replaced by a per-heap-block radix map (no hash, no qsort, ~1 byte/tuple) — **29%**. No hash remains on the per-posting path. `simplehash.h` step withdrawn as moot. | `bench/RESULTS_L15.md` | **MET: 353.8/356.5 s → 194.2 s (1.82×) like-for-like; plain run 328.0 → 192.5 s (1.70×).** Size unchanged 625 MB; ranked latency ±4% mixed-sign. Gap to pg_textsearch 6.96× → 3.91×; to GIN 1.37× → 0.95× (single run; GIN swung 15% on its own) |
| ~~**L17**~~ | **DONE 2026-09-10.** **Doclen sidecar docid column: gaps → absolute offsets from the block's `first_docid` (format v5).** Same FOR codec, different values, so the column is monotone AND fixed-width addressable: an in-block lookup is a `weave_for_get` binary search over the packed bytes instead of unpacking 128 entries and prefix-summing them to use ~2.6. Each BLOCK self-describes via `WEAVE_DOCLEN_ABS` in the high bits of `WeaveDoclenBlockHdr.count`, since a ≤0.5.0 index keeps v4 pages after upgrade while later merges write v5. | `bench/RESULTS_L17.md`, `doc/specs/SEGMENT_FORMAT.md` §5 | **LATENCY MET, ONE GATE MISSED.** mid k=10 **10.259 → 6.191 ms** (≤8 ✅), rare **2.815 → 1.508** (≤2.2 ✅), common 15.351 → 14.919 (no regression ✅), size 625 → **626 MB** (≤630 ✅). Cursor 71.9% → **45.2%** of the scan. **Buffer hits 11,702 → 11,924 against a <3,000 target — MISSED, and the gate was mis-specified:** hits track block *changes*, which L17 deliberately did not alter (it changed the cost *of* a change). v4-page read path untested at page level (codec-level only, 2.84M checks) — recorded as a limitation. **Both stated follow-ups shipped 2026-09-10** (pure read-path, `WEAVE_VERSION` stays 5): (1) the 8-step pre-bisect walk in `weave_doclen_cursor_lookup` is now gated by `WEAVE_DOCLEN_WALK_WINDOW` — one O(1) `weave_for_get` read decides whether the walk can reach the target before committing to it, instead of always paying the full window; instrumented on a 100k-doc scratch corpus (mid term df≈2500): **~12.0 → ~6.55 `weave_for_get` calls/lookup**, next to a plain bisect's ~7. (2) `weave_doclen_cursor_load_page` now copies the WHOLE sidecar page, not one block, into `WeaveDoclenResident`, and `weave_doclen_cursor_lookup` relocates within the resident page in memory (`weave_doclen_cursor_relocate`) before falling back to a fresh `ReadBuffer`; same-corpus warm buffer hits for the profiled query **767 → 90**. Result rows verified byte-identical before/after on rare/mid/common bands; `test/hegel/test_doclen_block.c` (2,839,534 checks) and the full PG17/PG18 regression + isolation + TAP suites stay green. Corpus scale here (100k docs) is smaller than `RESULTS_L17.md`'s 2M-doc benchmark, so these are raw instrumented counts, not a re-run of the latency table. |
| ~~**L13**~~ | **WAND initial-k.** DONE 2026-09-07: made `pg_weave.wand_initial_k` a GUC, swept 4..200 against `LIMIT` 10 and 100 across three df bands, set the default from the frontier (32, was 100). `LIMIT 10` 2.3× faster on rare; `LIMIT 100` 1.67–1.96× slower. `bench/RESULTS_WAND_K.md`. | `bench/RESULTS_WAND_K.md` | **MET** |
| ~~**L14**~~ | **DONE 2026-09-08.** **Incremental WAND growth.** The k frontier is non-monotonic because each ×4 growth recomputes the whole pass from scratch: from k=16 a `LIMIT 100` query runs three complete passes (16, 64, 256), from k=32 two, from k=100 one. So the cost of a deep page is dominated by how many times the scan is redone, and the initial k is the wrong knob. Keep the accumulated heap and per-term cursor state across a growth so a recompute extends the previous pass. This should make a low initial k strictly better at every `LIMIT` instead of a trade, and is the cheaper of the two routes at G13. | `bench/RESULTS_L14_LONG.md` | **MET in substance:** the scan stopped discarding 3/4 of every over-fetched pass, so k-scaling went from 1.00 (flat for a bad reason — a LIMIT 10 paid for k=100) to 1.04–1.21 (flat for a good one — a LIMIT 100 is served by one pass). pg_weave now WINS k=100 rare by 2.46× and ties mid/common. The literal gate wording (`k=8` matching `k=100`) is moot: k ≤ 16 are indistinguishable because the over-fetch floor is Max(4k,64)=64, so the knob has no effect there |

**Phase L gate:** `bench/lexical.sh` re-run and recorded with **zero measured
losses** against tsvector + GIN on latency, p99, and index size — the six gaps
G1–G6 in `doc/GAPS.md` all closed. Then the same against pg_search,
pg_textsearch, and VectorChord (task P3).

**Status 2026-09-06:** measured for the first time (`bench/RESULTS_LEXICAL.md`).
Winning by 8.2–19× on common-term ranked, 595× on `count(*)`, and 3.8–7.7× on
prefix. Losing by 1.7× on rare and mid ranked, 1.7–1.9× on index size, 1.2× on
build time, and carrying one silent 7,000× cliff (L7). L1–L6 unstarted; L7 is now
the highest priority in the project.

---

## Phase X — format substrate: the v6 break (DONE)

`doc/PRODUCTION_READINESS.md` blocking gate 5, and its Stage 2. Two coupled
problems, fixed in one break because either alone would have forced a second one.
Spec: `doc/specs/SEGMENT_FORMAT.md` §§2, 4, 6, 8, 9.

| id | task | spec | gate |
|---|---|---|---|
| ~~**X1**~~ | **DONE 2026-09-10 (0.6.0).** **The page-kind space stops being a flat bitmap.** `flags` bit 15 is a reserved escape selecting an extended integer kind space held in the second opaque word (was `unused`); the ten shipped kinds keep their one-hot bits, so a v6-written lexical page is byte-identical to a v5-written one and no page is rewritten. Chosen over widening `flags` to `uint32`, which moves the opaque area on every page of every existing index and would force a REINDEX on relations containing no new kind at all. Follows L17's precedent: a per-*object* self-describing discriminator in spare bits of an existing field, because after an upgrade one relation holds both generations and a per-index version cannot describe it. | `SEGMENT_FORMAT.md` §2 | **MET.** The primary guarantee is `weave_check_meta()`'s version gate (`am.c:1695`): a v5 `.so` sees `meta->version == 6` and errors out before it ever reaches a page kind, regardless of bit layout. `test/hegel/test_pagekind.c` proves a narrower, secondary property as defence in depth: 852,070 checks, exhaustive over all 2^16 flag words — encode/decode bijection, the shipped kinds' byte patterns unchanged, and that a v5 reader's first-match rule finds no kind bit on any extended page (so it would refuse rather than mistake kind 20 for `POSTING|TRGM` even absent the version gate) |
| ~~**X2**~~ | **DONE 2026-09-10 (0.6.0).** **Per-bolt weft descriptors.** `WeaveSegMeta.chandesc` names a `WEAVE_CHANDESC` page holding a header plus a `(kind, attnum)`-ascending array of `WeaveChannelDesc`. A bolt self-describes what it carries, so an index built without a vector column stores no vector structures and a reader never infers a weft's geometry from a GUC that may have changed since the build. Every v6 bolt gets one — including a lexical-only bolt — because a descriptor page that no shipped code writes is a substrate whose writer, WAL path, free path and validator are all untested until the vector channel lands, which is the failure mode this gate exists to prevent. | `SEGMENT_FORMAT.md` §6 | **MET.** `sql/chandesc.sql`: one descriptor page per live bolt across build, insert, merge, vacuum and REINDEX; zero unclassified pages; identical answers index-vs-seqscan. Cost **one 8 KB page per bolt** (raw measurement, not a ratio) |
| ~~**X3**~~ | **DONE 2026-09-10 (0.6.0).** **Versioned metapage reader + `weave_check()`.** `weave_meta_from_page()` deserializes v3/v4/v5/v6 into the current struct behind `StaticAssertStmt`s on every layout relationship the branching depends on. `weave_check(regclass, deep)` is new — there was none before, only `weave_check_meta()` on the metapage header — and reports one row per invariant so a corruption test can assert a *specific* fault is caught. Eleven invariants implemented, including `page_kinds_decodable`, `chandesc_reachable`, `chandesc_roots_agree`, `chandesc_version_consistent`, `pages_reachable_or_freed` and `chains_do_not_overlap`. | `SEGMENT_FORMAT.md` §§8-9 | **MET.** Fuzz: `test/fuzz/fuzz_chandesc.c`, 856,784 images under ASan+UBSan with an exact-sized buffer, plus a planted-bug variant that must abort. **A finding**: `WEAVE_LIVEDOCS` is allocated in the table and in the header but no writer sets it — the livedocs blob goes out on `WEAVE_TRGM_DATA` pages, so `weave_index_size_detail()` reports zero livedocs pages. Recorded in §2, not fixed here |
| ~~**X4**~~ | **DONE 2026-09-10 (0.6.0).** **The backward-compatibility test the project owed.** `t/010_format_v6_upgrade.pl` builds a v6 index over 20,000 rows, records six counts + a ranked top-20 + twenty distance values, stops the server, rewrites the metapage into exactly the bytes a v5 build would have written, restarts, and asserts the answers are **byte-identical** — then upgrades in place by inserting 3,000 rows and merging, and asserts they still are. `t/009_doclen_sidecar.pl` had said the equivalent v3 check was "validated out-of-tree in the release qualification (it needs two `.so` builds)", which is a gate nobody runs. Closes `PRODUCTION_READINESS.md` gate 6's "upgrade over an index containing data". | `SEGMENT_FORMAT.md` §8 item 6 | **MET.** 32 assertions, green on PG17. Also proves the leak detector has teeth: the orphaned descriptor pages the downgrade creates are *reported* as unreachable, and the only cure is REINDEX |

**Phase X gate:** regression + isolation green on PG17 and PG18, TAP green, and a
pre-v6 index readable with identical results. **MET.** Not in scope and
deliberately not done: writing any vector or fuzzy page (their kinds are reserved,
not implemented), the `pg_upgrade` test (gate 15), and the remaining `weave_check()`
invariants (task M6).

---

## Phase Z — fuzzy / approximate-regex / prefix / n-gram channel

**Four of the product's six named retrieval kinds live here**, which makes this
phase load-bearing rather than an add-on. It was briefly moved post-1.0 on
2026-09-10 and moved back the same day when the scope was restated; see the
ordering principle above.

**Z8 is no longer opt-in.** It used to read "opt-in and can slip". `n-gram` is a
named product capability, so the corpus character-trigram channel ships, and the
cost it was hedged behind ships with it: **with `cgram` on, pg_weave is not smaller
than `pg_trgm`.** That moves from a limitation we dodge by defaulting off, to a
stated cost of the product, and `bench/RESULTS_CGRAM.md` has to record it as
plainly as any win (hard rule 8). The reloption stays -- an index that does not need
unanchored substring search should not pay for it -- but the *default-on* decision
is now the product's, not an optimization's.

One thing that made deferring Z look cheap is still true and still worth knowing:
TRE is current (`f864ed0`), carrying an `INT_MAX` crash fix and a backref
wrong-answer fix with regression coverage in `test/hegel/`, and both bugs were
**latent** here because the channel is unrouted. An unrouted channel cannot return a
wrong answer. Routing it is what makes those fixes matter, which is the work below.

The code is imported (`doc/specs/IMPORT_pg_tre.md`). The work is wiring it to the
**vocabulary** funnel rather than pg_tre's corpus-level trigram index — that
substitution is the whole reason this channel can exist at a reasonable size.
See `doc/ARCHITECTURE.md` §7 and `doc/specs/FUZZY_CHANNEL.md`.

| id | task | gate |
|---|---|---|
| ~~**Z1**~~ | **DONE 2026-09-09 (0.5.0).** Vendor TRE (laurikari/tre, BSD-2) as a submodule or in-tree copy; record the pinned commit and license in `doc/LICENSING.md`; get `src/query/re_match.c` compiling | `make` clean with the fuzzy sources in OBJS |
| ~~**Z2**~~ | **DONE 2026-09-09 (0.5.0).** Wire the missing GUCs and deadline helpers listed in `doc/specs/IMPORT_pg_tre.md` "Wiring TODO" | all imported TUs compile; GUCs visible in `pg_settings` |
| **Z3** | **PURE CORE DONE 2026-09-10.** SuRF trie over the **bolt vocabulary** (dictionary terms), not the corpus: `include/weave/surftrie.h` + `src/query/surftrie.c` are the backend-independent builder/reader/validator (LOUDS-Sparse, page kind `WEAVE_PK_SURF` id 21 as reserved by X1), format v1 recorded in `doc/specs/FUZZY_CHANNEL.md` §3.3. A separate `terminal` bitmap replaces SuRF's reserved `0xFF` terminator label, because on a non-UTF-8 server a term can legitimately contain `0xFF` and that collision is a wrong answer in the false-negative direction. | **MET for the core:** `test/hegel/test_surf.c` 2,843,941 checks, 0 failures, clean under ASan+UBSan, asserting trie membership == dictionary membership in **both** directions plus prefix-enumeration exactness and the one-sided (false-positive-only) contract for over-long terms; `test/fuzz/fuzz_surftrie.c` 278,387 cases with two planted-bug builds that abort. 20 of 21 mutations caught; the 21st is provably redundant (derived at `src/query/surftrie.c:1136`, not deleted). **AM WIRING DONE 2026-09-11.** Writer at flush and merge, `WEAVE_WK_FUZZY` descriptor, `surf_trie_matches_dictionary` invariant, `t/012` crash recovery, `t/013` corruption, ~5.52 B/term.

**All three coordinator escalations resolved 2026-09-12:**

**(a) `WEAVE_VERSION` 7 / extension 0.7.0 — RATIFIED.** The evidence was already in hand rather than needing new work: `sql/pg_weave--0.6.0--0.7.0.sql` exists, CI's upgrade-path leg chains `0.1.0` → current on every push and is green, `t/010` carries `WEAVE_VERSION_CUR => 7` and notes explicitly that hard-coding 6 would have turned a correct bump into a test failure, and `src/am/am.c:1066` fail-closes on a version outside `[WEAVE_VERSION_DOCLEN_INLINE, WEAVE_VERSION]` per `doc/CONVENTIONS.md` decision 3. Nothing to do but say so.

**(b) `weave_merge()`/`weave_vacuum()` WAL durability — FIXED**, `ForceSyncCommit()` plus `t/014_merge_durability.pl`, verified wrong-before/right-after. It also disproved two reassuring claims in `doc/specs/SEGMENT_FORMAT.md` §10 — the gap is not "lost work, not lost data" (`weave_check()` reports unreachable pages after recovery) and `weave_vacuum()` did not escape it via `nrels > 0`. Both corrected there.

**(c) SuRF reloption-gating — YES, gate it, DEFAULT ON.** Three reasons, and one observation that made the decision cheap. (i) `doc/ARCHITECTURE.md` §8.4 already commits to per-channel opt-in as the mitigation for the operational-simplicity loss — "unused machinery is absent from the index rather than merely idle" — so a structure costing ~5.52 B/term must be declinable. (ii) It changes bytes on disk, so `doc/CONVENTIONS.md` decision 1 makes it a **reloption, not a GUC**, with the value recorded in the segment. (iii) The default is **on**, because the product statement is that all six retrieval kinds are present in one `CREATE INDEX`; a capability defaulted off is not a capability the product has. Precedent and naming come from the three existing bool reloptions in `src/am/am.c` — `positions`, `trigrams` (already a per-channel gate, default off) and `doclen_sidecar` (default on) — so this one is `surf`, naming the structure as they do. **The observation:** Z4 needs no new fallback path for the trie being absent, because the fallback *is* the current dictionary walk that Z4 exists to replace. Implementing the reloption is Z4's prerequisite, not a separate task |
| Z4 | **Inherits the case-folding precondition** in `doc/specs/FUZZY_CHANNEL.md` §3.2: the trie is built over ANALYZED terms, so a case-sensitive probe cannot be rejected by it — the dictionary has already discarded the distinction. Upstream shipped the inverse of this bug for two releases and returned zero rows silently. Route prefix (`term*`) through SuRF instead of the current dictionary walk | prefix p50 ≤ today's, and `EXPLAIN` shows the surf channel |
| **Z5** | **PURE CORE DONE 2026-09-11.** `include/weave/uleven.h` is the backend-independent exact-neighbourhood core: query term + edit budget *k* + any ascending-byte-order vocabulary iterator (`WeaveUlevVocab`: `next`, optional `skip`) → exactly the terms within distance *k*. Exact in both directions (unlike SuRF's one-sided error), edit unit is the **character** not the byte (`WEAVE_ULEVEN_UTF8` / `_BYTE`), Levenshtein not Damerau — all three for agreement with `contrib/fuzzystrmatch`'s `levenshtein()`, which is Z9's differential oracle. Design in `doc/specs/FUZZY_CHANNEL.md` §3.6. `test/hegel/test_uleven.c`: 7,445,258 checks, 0 failures, in `make check-standalone`; six injected mutations each confirmed to fail it. **The remaining half is routing**: nothing calls this from the AM yet, `term~k` still runs the old path, and the gate below is a latency gate that therefore has not been measured. Z5 is *not* done. | index-accelerated `k=1` and `k=2` on a 1M-row corpus, p50 ≤ 200 ms (pg_tre measured 5.5 s and 7.3 s) |
| Z6 | **Inherits the same case-folding precondition as Z4** (`FUZZY_CHANNEL.md` §3.2). Route regex (`/re/`) through regex AST → trigram tiling → vocabulary candidates. **Before trusting a tile for pruning, read `src/query/uleven.c`'s header:** a `k≥2` tile always falls back to `always_true` at the `max_out` `tiling.c` passes, so any measurement that appears to show `k≥2` tile pruning is measuring something else | character-class regex `E-[0-9]{4}` p50 ≤ 100 ms (pg_tre measured 1.5 s) |
| Z7 | Implement the fuzzy/regex shuttle per `include/weave/channel.h` (C5 boolean gate) | property test in `test/hegel/test_bounds.c` covers it |
| Z8 | **REQUIRED (was opt-in; `n-gram` is a named product capability).** Corpus-level character-trigram channel `cgram` for unanchored cross-token substring (`LIKE '%tion refu%'`) | correctness parity with `pg_trgm` on a randomized pattern suite; size honestly recorded in `bench/RESULTS_CGRAM.md` |
| Z9 | `<@>` edit-distance KNN ordering as a shuttle with a real bound | correctness parity with a seq-scan `levenshtein()` reference |

**Phase Z gate:** on the 1M-row corpus from `pg_tre/doc/perf.md`, pg_weave beats
pg_tre on every row of that table *and* is within 3× of pg_trgm's index size
with `cgram` off. Recorded in `bench/RESULTS_FUZZY.md`.

---

## Phase V — vector channel

New C code. Spec: `doc/specs/VECTOR_CHANNEL.md`. Reference implementation to
read but **not** to port line-by-line: `~/src/turbovec` (Rust, MIT).

| id | task | gate |
|---|---|---|
| ~~**V1**~~ | **DONE 2026-09-06 (0.2.0).** `wvec` type: I/O, typmod, casts, `<->` `<#>` `<=>` `<+>`, arithmetic, btree opclass, codec introspection | **DONE** — `sql/wvec.sql`, 4th regression test, green on PG 17 and 18. Found and fixed a silently-accepted trailing comma in the parser. |
| ~~**V2**~~ | **DONE.** Deterministic rotation: ChaCha8-seeded global permutation + sign flips + per-block normalized Walsh-Hadamard, K=2 rounds | **bit-identical output across x86-64 and aarch64 and across thread counts.** This is a hard gate; a fixture hash committed in `test/hegel/rotation_fixture.h` must match on both arches in CI |
| ~~**V3**~~ | **DONE.** Lloyd–Max codebook for Beta((d-1)/2,(d-1)/2), memoized by `(bits, dim)` | codebook values match a committed fixture to 1e-9; solve time ≤ 100 ms |
| ~~**V4**~~ | **DONE.** Encode: normalize, rotate, optional TQ+ affine calibration, quantize, bit-pack, store per-vector renormalization scale | round-trip property test; the compressed-domain inner-product estimator is unbiased within a stated tolerance |
| ~~**V5**~~ | **DONE.** 32-lane packing layout: `WEAVE_PACK_LANE` (coordinate-major) and `WEAVE_PACK_VECMAJOR` (vector-major, for int8-dot kernels), plus the O(1) `weave_pack_move_lane` swap-remove vacuum needs. x86 `perm0` lane interleaving is a kernel-internal register relabelling (task V6), not a third on-disk variant — see the note in `include/weave/quantize.h` above `WeavePackLayout` and `doc/specs/VECTOR_CHANNEL.md` §8 for why. | `test/hegel/test_pack.c`: pack/unpack round-trip, per-lane isolation, `move_lane`/`zero_lane` O(1) swap-remove, guard-byte bounds check — **1,909,440 checks, 0 failures**, also clean under `-fsanitize=address,undefined` |
| **V6** | **PARTIAL 2026-09-10.** Block-scoring kernels with runtime dispatch. Implemented and verified bit-identical to the scalar oracle on x86-64: `scalar` (the oracle; reaches codes only through the pack API, so it is layout-agnostic), `lut-wide` (portable wide-word float-LUT gather), `lut-avx2` (`vpsrlvd` + `vpgatherdps` + `vcvtps2pd`). Dispatch resolves once in `weave_vec_kernels_init()` from `_PG_init`, honours the new `pg_weave.vec_kernel` GUC (`auto`/`scalar`/`lut`/`dot`), and `weave_vec_kernel_name()` reports whichever path was resolved. | `test/hegel/test_kernels.c`: **308,278 checks, 0 failures**, clean under `-fsanitize=address,undefined`, and mutation-tested — a swapped AVX2 lane half, a wrong shift, an ignored `allow` mask, a 1-byte over-read, a dropped `allow` bounds check (ASan heap-buffer-overflow), a defaulted-instead-of-rejected pack layout, and a query LUT built without the rotation (which K1 cannot see and K5 catches at 10^5× tolerance) are each caught. Scores are checked against the *definition* in `doc/specs/VECTOR_CHANNEL.md` §2 — ⟨q, reconstruct(code)⟩ from real `weave_encode`/`weave_decode` output — not only against a transcription of the LUT loop. **What is NOT done, and why:** (a) no SSE2 and no baseline-NEON path — held to exactness, scoring is *gather*-bound, and neither ISA has a gather or a variable shift, so an exact kernel there is `lut-wide` plus register shuffling; (b) no AVX-512BW/VNNI and no NEON SDOT — this host is x86-64 without AVX-512 and there is no aarch64 runner or emulator wired up here, and an ISA path nobody ran is the failure AGENTS.md rule 8 exists to prevent, so none was written; (c) **no nibble-split byte-LUT and no int8-dot kernel at all** — both families quantize the query table to 8 bits, so they cannot be "identical to the scalar path" as this gate says, and they owe a recall budget that does not exist yet (see `doc/specs/VECTOR_CHANNEL.md` §8); (d) `bench/kernels.c` is still owed, so which of the three verified paths is fastest on a given host is **unmeasured** — `auto` picks the widest ISA by convention, not by measurement; (e) the fast paths skip masked lanes at **8-lane granularity** where the oracle skips per lane, so a scattered filter saves them less than it saves the oracle — stated in `src/vector/kernels.c` and `doc/specs/VECTOR_CHANNEL.md` §9, and unresolvable without (d) since the output is identical either way. The full V6 gate as originally written (seven ISAs, both strategies) remains **unmet and is tabulated as unmet** in `doc/specs/VECTOR_CHANNEL.md` §8. |
| V7 | New page kinds `WEAVE_PK_VCODES`/`WEAVE_PK_VMETA` (ids 18/17, reserved by X1 in the extended kind space -- read them with `WeavePageHasKind()`, never a bitwise AND); codes live in the bolt under GenericXLog. **On-disk determinism:** `weave_block_codebytes()` allocates 0-28 slack bytes per block beyond the tight `4*dim*bits` requirement (see the note above it in `include/weave/quantize.h`), and no pack function ever writes them, so V7 **must zero the block buffer before encoding** or two indexes holding identical vectors get different bytes on disk. | crash-recovery TAP test extended to a vector index |
| V8 | Code-scan shuttle: `score_block` over 32 lanes, `allow`-mask block short-circuit, and the block bound from `doc/specs/FUSED_TOPK.md` §2. Passes the segment's pack layout and the allowlist's `nwarp` into `score_block()` — both are parameters of that prototype, neither is assumed (`doc/specs/VECTOR_CHANNEL.md` §§8, 9) | (C1)+(C2) property test; a selective mask makes the scan measurably *faster* |
| V9 | **DEMOTED 2026-09-12 to "conditional on our own flat-vs-IVF measurement", from required.** pg_turbovec v2.8.0–2.8.3 measured at 1M rows that **flat brute-force beats IVF at every recall target at 2-bit and 4-bit**; IVF wins only at **1-bit**, and only up to ~0.95 (47 % faster at R@10 ≥ 0.90, 38 % at ≥ 0.95). The mechanism it gives is that wider, less lossy codes need a narrower exact-rerank window, so the full flat scan is already cheap — not that probe-miss error dominates. pg_weave's own sweep now puts it at **3 bits plus an exact top-100 rerank**, i.e. squarely in the regime where IVF measured *worse* than flat, and pg_weave would additionally be clustering over **already-quantized** codes, a lossier geometry than pg_turbovec's and one nobody has measured. Before building V9, measure flat-vs-IVF on pg_weave's own codebook; if it reproduces, withdraw V9 and let V8's flat 32-lane scan plus the block bound carry the channel. **Knock-on: V13 loses its justification** — it was written as "IVF satisfies this requirement inherently", and without IVF the warp ordering needs a build-time clustering step of its own, which is what the withdrawn graph plan also needed. The block bound prunes 99.6 % of blocks with a coherent warp and 0.0 % with a random one, so the clustering does not become optional just because the probing does. Original entry follows. **IVF coarse quantizer** over the quantized codes: k-means over a sample, per-cluster centroids, `nprobe` probing, cluster-aligned code blocks (which also satisfies V13). **NOT a proximity graph** — withdrawn after pg_turbovec deprecated its graph kind in v2.5.0, having measured that at R@10 ≥ 0.98 on GIST-10M/960-d IVF reached 28.4 ms while the graph could not reach 0.98 at **any** latency (ceiling 0.873 at 181 ms) and built 57–90× slower. **A later release (v2.7.4) additionally measured that IVF's probe count sets a hard recall ceiling a wider rerank window cannot break — 0.846/0.906/0.954/0.978/0.984 at probes 8/16/32/64/128 at `lists=512` — a mechanism, not a BQ-specific artifact, so it applies to this design's IVF too.** `doc/specs/VECTOR_CHANNEL.md` §8a. | recall@10 ≥ 0.99 on 1M × 1024-d, **backed by a probes-vs-recall-vs-p50 sweep on pg_weave's own codebook and corpus geometry, not a single (probes, recall, p50) triple picked to clear the bar**; p50 within 2× of pgvector HNSW measured at the same recall target **on the same corpus** (confirm HNSW can itself reach 0.99 there before using it as the comparator — pg_turbovec's own data shows a corpus where HNSW topped out at 0.983); storage ≤ 0.15× pgvector HNSW; **plus a recall floor in the gate for any partitioned build** — pg_turbovec's shard/thread coupling cost R@10 0.920 → 0.605 while a build-time-only test called it a 60× speedup |
| V10 | **PROMOTED to a 0.99 prerequisite 2026-09-10, was "optional".** `recall=exact` path: graph off, full code scan, and the full-precision rerank sidecar `WEAVE_PK_VRERANK`. `bench/RESULTS_IVF_RECALL.md` measured compressed-domain-only recall@10 at **full probe** — probe-miss error zero, so this is the ceiling over every `nprobe` — topping out at 0.9205 (GloVe-200d) and 0.8780 (GIST-960d) at 4 bits, against a 0.99 gate. A rerank window of 100 closed the gap on both corpora. So the sidecar is not an extra for an exactness mode; it is how the ordinary gate is met. `doc/specs/VECTOR_CHANNEL.md` §2.1. | recall@10 == 1.000 for `recall=exact`, **and** the rerank window needed for `recall@10 >= 0.99` recorded per bit width and corpus |
| V11 | Journal-checksum-style incremental commit for the vector wefts (alternating header slots, delta digest) — adapted to PostgreSQL's WAL rather than replacing it | torn-write injection TAP test detects and recovers |
| V12 | ColBERT-style multivector late interaction as a distinct channel kind | MaxSim correctness against a reference implementation |
| V13 | **RE-JUSTIFICATION REQUIRED 2026-09-12** — this row says "IVF satisfies this requirement inherently", and V9 (IVF) is now demoted, so the clustering has to be provided by something. It does not become optional: the block bound prunes 99.6 % of blocks with a coherent warp and **0.0 %** with a random one, so warp coherence is load-bearing regardless of whether anything probes clusters at query time. What changes is that it needs a build-time k-means of its own rather than inheriting one — the same requirement the withdrawn proximity-graph plan had. Original entry follows. **Warp ordering by cluster.** Assign warp positions in the order the IVF build's k-means clustering produces (§8a of doc/specs/VECTOR_CHANNEL.md; IVF satisfies this requirement inherently, where the withdrawn graph plan needed it as a separate constraint), so each 32-lane code block is spatially coherent. Not an optimization: `bench/RESULTS_BOUND_PRUNING.md` measures the block bound pruning 99.6% of blocks with a coherent warp and **0.0%** with a random one. | `bench/bound_pruning.c` reports ≥ 90% blocks pruned at k=10 on the shipped corpora; a heap-order build is rejected by the gate |
| V14 | Per-block centroid (stored as a quantized code) + radius in `WeaveVecBlockHdr`, maintained across insert, vacuum lane-zero, and merge | `weave_check()` recomputes both and compares; a randomized soundness run of `bench/bound_pruning.c` finds no (C2) violation |

**Status:** V1–V5 done. V2–V5 have working scalar implementations in
`src/vector/quantize.c` and `src/vector/pack.c` with 17,741 + 1,909,440
property checks passing, but no on-disk format and no committed
cross-architecture fixture, so V2–V4 are not yet gate-complete in the on-disk
sense (V5's gate is a standalone codec property test and does not need one:
the packing layout is architecture-independent by design, see the header note
above `WeavePackLayout`). V6 is partial: three verified scoring paths and a
resolved dispatch table (`src/vector/kernels.c`, `src/vector/kernel_ops.c`, 308,278 differential
checks),
no verified vector ISA outside x86-64 AVX2, no approximate kernel family, and
no per-host A/B. V7–V14 not started.

**Phase V gate — ANSWERED BY MEASUREMENT 2026-09-12. Resolution 1 is refuted; a
maintainer decision is needed on the shape that replaces it.** The gate was: on
1M × 1024-d Cohere-wiki, all three of `recall@10 ≥ 0.99`, `p50 ≤ 2× pgvector HNSW`,
`size ≤ 0.15× pgvector HNSW` simultaneously.

`bench/RESULTS_BITWIDTH_SWEEP.md` swept widths 2..8 at full probe on both corpora,
which is what resolution 1 ("keep both claims, find a rerank representation cheaper
than float32") required. The result:

- **No supported width reaches 0.99 on GIST-960d.** GloVe needs **8 bits**
  (0.9950); GIST tops out at 0.9860 at 8 bits with decaying increments.
  `recall@10 ≥ 0.99` is **unreachable with a single code width** — measured, not
  extrapolated. (This bullet used to add that 8 bits is "0.18× HNSW against a 0.15×
  budget, so the width that clears 0.99 on the easier corpus already misses the
  storage claim". That half is **withdrawn**: it divided by an estimated
  denominator. Measured, 8 bits is 0.127× and fits — see the HNSW denominator
  bullet below. The single-width conclusion stands on recall alone.)
- **But codes plus an exact float32 rerank of a top-*w* window, read from the
  heap, satisfies both.** The rerank has to be full precision (§2.1.1's rule: a
  *b*-bit rerank cannot beat the *b*-bit ceiling), and a stored float32 sidecar
  costs `4 * dim` = 4,096 B/vector, worse than HNSW — so the rerank source is the
  **heap**, where the original vector already is and the index pays nothing for it.
- **Both *b* and *w* are now measured at the gate's own corpus size, and the answer
  is not the narrowest width.** `bench/RESULTS_PHASE_V_COLD.md`, two `r7i.2xlarge`,
  one engine per host. Windows at **n = 1M** on GIST-960d, index bytes at 1024-d,
  against a **measured** HNSW denominator of 8,056 B/vector:

  | bits | window @ 0.99 (n=1M) | index B/vec | × HNSW | SIMD kernel |
  |---|---|---:|---:|---|
  | 3 | 50 | 384 | 0.048 | yes |
  | **4** | **25** | **512** | **0.064** | **yes** |
  | 5 | 20 | 640 | 0.079 | no |
  | 8 | — (0.9860 ceiling) | 1024 | 0.127 | no |

- **RECOMMENDED SHAPE: 4 bits + exact rerank of a top-25 window.** recall@10
  **0.9920** at n = 1M; index **0.064× HNSW**; widest width that keeps the SIMD
  code-scan kernel.
- **The window grows with n, by ~+25% per decade, uniformly across widths** (40→50,
  20→25, 15→20). The flagged risk was real and is now bounded. Two controls make it
  attributable to n: the same binary reproduced the n=100k row exactly on the same
  host, and the n=1M 3-bit row reproduced **bit-identically on a second machine
  with `lists=1` instead of `lists=1024`**, confirming the full-probe column is
  partition-independent. `lists=1 probes=1` is now the cheap way to measure a
  ceiling — it skips k-means entirely.
- **THE HNSW DENOMINATOR WAS A GUESS AND IT WAS 30% LOW.** §2.1.1 priced the 0.15×
  budget from "on the order of 5,700 B per vector (unmeasured — measure before
  quoting it)" and it was quoted repeatedly. Measured at m=16, ef_construction=64
  on 999,990 × 960-d: **8,056 B/vector**, 7,683 MB, built in 277 s. **0.15× is
  ~1,208 B/vector, not ~855.** This overturns the sweep's argument that 8 bits at
  0.18× "already misses the storage claim" — it is **0.127×** and fits. The
  single-width conclusion survives on **recall** (8 bits tops out at 0.9860), not
  on storage.
- **THE BASELINE HAS NO OPERATING POINT AT THE RECALL THE GATE NAMES.** pgvector
  HNSW recall@10 against an exact sequential scan: 0.4400 / 0.7200 / 0.8560 /
  0.9160 / 0.9600 / **0.9760** at ef = 10 / 40 / 100 / 200 / 400 / 800. It never
  reaches 0.99, independently reproducing what `VECTOR_CHANNEL.md` §8a imported
  from pg_turbovec. **So `p50 ≤ 2× pgvector HNSW at recall@10 ≥ 0.99` has nothing
  to be 2× of, and needs restating rather than passing or failing.** Caveat that
  limits it: m=16 is modest for 960-d and pgvector's guidance is to raise it, so
  this is a ceiling for *these build parameters*. An m / ef_construction sweep is
  the next measurement, and it could move the number materially.
- **Cold, the rerank is not what decides the gate.** A 20-candidate window is
  **86 ms** p50 cold at n = 1M (~100 ms at 25), against **6.2 s** for HNSW at
  ef = 400 and 11.5 s at ef = 800. Mechanism as predicted: HNSW's traversal is
  dependent random I/O; a rerank window's TIDs are all known before the first
  fetch. Warm, the two arms that were verifiably warm (zero reads) are 0.598 ms for
  a 10-candidate rerank and 3.548 ms for HNSW at ef = 10.
- **The local page-count prediction was wrong by 7×, and its stated basis was
  false.** `bench/RESULTS_RERANK_IO.md` predicted ~48 reads / ~12 ms for a
  20-candidate window and called page counts "device-independent" because they
  follow from layout. Measured 86 ms. At 250k rows with a 32 MB pool the toast
  *index* stays resident and descents are nearly free; at 1M rows genuinely cold,
  every descent pays full depth. Page counts depend on cache state and scale.
- **WHAT IS STILL UNMEASURED IS NOW THE CODE SCAN, AND IT IS THE DECIDING HALF.**
  V7 and V8 are not implemented, so no pg_weave vector query exists to time; what
  was measured is the rerank component in isolation. At 4 bits, 1M codes is 512 MB
  to read and score. Nothing here licenses a claim that this channel beats
  pgvector — only that the heap rerank, which reopened this gate, is not what will
  close it.

- Compressed-domain-only recall@10 at **full probe** is 0.9225 (GloVe-200d) and
  0.8680 (GIST-960d) at 4 bits. Full probe means zero probe-miss error, so that
  is the ceiling over every `nprobe`, and **0.99 is unreachable at 4 bits without a
  full-precision rerank** (§2.1 of `doc/specs/VECTOR_CHANNEL.md`).
- A full-coverage float32 rerank sidecar costs `4 * dim` bytes per vector — 4,096 at
  1024-d — which is roughly what pgvector HNSW spends on the vector it stores. The
  `size ≤ 0.15×` figure assumed the codes were the whole index.

So `recall@10 ≥ 0.99` **and** `size ≤ 0.15× pgvector HNSW` may be jointly
unsatisfiable, and that is a maintainer decision rather than an engineering task.
**MAINTAINER DECISION 2026-09-11: resolution 1 — keep both claims and find a
rerank representation cheaper than float32.** The other two remain recorded below
as what we fall back to if the measurement below refuses.

That decision is not yet an engineering task, because arithmetic on the gate's own
corpus narrows it to one shape and rules out the obvious candidate. At
1M × 1024-d, pgvector HNSW spends `4 · 1024` = 4,096 B on the vector plus its graph
links; at `m = 16` the total is on the order of 5,700 B per vector (**and that
baseline has never been measured on this corpus — measure it before quoting the
ratio**). A `0.15×` budget is therefore about **855 B per vector, which at 1024-d is
6.68 bits per coordinate for everything the index stores.** Consequences:

- **A rerank representation cannot lift recall above its own compressed-domain
  ceiling.** Reranking a top-*W* window with a *b*-bit representation yields the
  *b*-bit ranking of that window, so the minimum viable *b* is the smallest one
  whose full-probe compressed-domain recall@10 reaches 0.99 — and `§2.1` of
  `doc/specs/VECTOR_CHANNEL.md` has now measured *b* = 2..8 at
  0.7345/0.8515/0.9225/0.9570/0.9750/0.9860/0.9950 (GloVe-200d) and
  0.6130/0.7880/0.8680/0.9200/0.9660/0.9780/0.9860 (GIST-960d). This is
  why "rerank with 4-bit codes" is not a candidate: 4 bits *is* the 0.9225 ceiling.
- **"4-bit codes plus an 8-bit sidecar" is refuted without a benchmark.** An
  8-bit-per-coordinate sidecar is 1,024 B by itself — 0.18× HNSW before the scan
  codes exist — and with 4-bit codes the total is 1,536 B, **0.27×**. Arithmetic,
  not measurement, kills it.
- **So the surviving shape is a single code width used for both the scan and the
  final ranking, with `b ≤ 6`,** not codes plus a sidecar. That is a design
  simplification the budget forces rather than a preference.

**The one measurement that now decides the phase:** the smallest *b* whose
full-probe compressed-domain recall@10 reaches 0.99 on both corpora, swept at
*b* = 5, 6, 7, 8. If that *b* ≤ 6, both claims hold and V10's sidecar collapses
into a wider code. If it is 7 or 8, the storage claim fails on arithmetic and we
fall back to resolution 2 or 3 below. Extrapolating the measured points suggests
GloVe-200d needs ~7 and GIST-960d may not reach 0.99 even at 8 — **extrapolation is
not measurement, and this project's rule is to measure the thing the design rests
on, so the sweep is the next V task.** It needs codec work first:
`include/weave/quantize.h` validates the Lloyd-Max solver only for 2..4 bits, and
`bench/ivf_recall.c` accepts only `bits=2,3,4`.

The two resolutions not chosen, kept because the measurement may force one:

2. **Keep the storage claim, lower the recall claim.** "0.92 recall at ~0.12× the
   storage" is a real product position and a defensible one. It is not the position
   `doc/ARCHITECTURE.md` §8 currently implies.
3. **Keep the recall claim, drop the storage claim.** pg_weave then competes with
   pgvector on latency and integration rather than size.

Until one is chosen, the gate is: **all three measured and recorded on the real
corpus, with a probes-vs-recall-vs-p50 sweep and the rerank sidecar counted in the
size**, and whichever of the three is not met stated plainly. Recorded in
`bench/RESULTS_VECTOR.md`, which **must include a probes-vs-recall-vs-p50 sweep for
V9's IVF, not a single passing configuration** — both pg_turbovec's v2.7.4 and our
own `bench/RESULTS_IVF_RECALL.md` measured a fixed `nprobe` imposing a recall ceiling
no rerank-window widening breaks, so "some setting clears 0.99" is not evidence that
a *fast* setting does.

Two further warnings about the gate, both from measurements taken after it was
written:

- **The comparator may not clear its own bar.** On pg_turbovec's 500k × 1024-d
  corpus pgvector HNSW never reached 0.99 at all, so `p50 ≤ 2× pgvector HNSW` at
  matched recall is undefined there. Confirm HNSW reaches the target on *our* corpus
  before using it as the comparator.
- **Our own probe-miss numbers are pessimistically biased and must not be quoted as
  the ceiling.** `bench/ivf_recall.c` uses a deliberately crude Lloyd k-means with
  random init and 12–32 sample rows per centroid; a better partition can only raise
  those recalls. The *quantization* half of that measurement is the robust half,
  because it is taken at full probe where the partition cannot matter.

This is also the gate pg_turbovec **failed** (490× slower); the difference is V9. If
V9 does not land — or lands and cannot clear 0.99 within the latency and storage
budget — the vector channel is storage-optimal and latency-poor and the README must
say exactly that.

---

## Phase F — fused-threshold top-k

Spec: `doc/specs/FUSED_TOPK.md`. Do not start until L, Z, V gates pass.

| id | task | gate |
|---|---|---|
| F1 | `include/weave/fuse.h` + `src/am/fuse.c`: the loop from FUSED_TOPK §3, pivot selection, essential/non-essential partition, block prune, incremental abandonment | degenerate cases in `sql/fuse_degenerate.sql` byte-identical to the single-channel paths |
| F2 | `fuse()` planner support: recognize it, push it into the index `ORDER BY`, and provide an executable fallback so the query never fails | `sql/fuse_fallback.sql` |
| F3 | `score()` / `score_parts()` projection | values match a reference recomputation |
| F4 | Two-phase graph integration per FUSED_TOPK §6 | filter pushdown demonstrably steers traversal: recall with a 1%-selective predicate ≥ recall without it |
| F5 | Property test `test/hegel/test_fuse_props.c`: for random channel bounds/scores, fused top-k == brute-force top-k | passes 10^6 generated cases |

**Phase F gate:** every row of the table in `doc/specs/FUSED_TOPK.md` §8,
including the `score()`-call ratio. If the call ratio is not ≤ 0.2× RRF, the
bounds are too loose — fix them before anything else, and record the negative
result.

---

## Phase M — migration and compatibility

Performance moves nobody. Compatibility does. This phase is not optional and it
is not "polish".

| id | task | gate |
|---|---|---|
| M1 | pgvector surface: `vector`/`halfvec`/`sparsevec`/`bitvec` types or coexisting casts, `<->` `<=>` `<#>`, `vector_dims`, aggregates | pgvector's own regression suite, adapted, passes |
| M2 | Decide and document the `vector`-type collision with pgvector: coexist via casts, or conflict. **Coexist.** | `CREATE EXTENSION vector; CREATE EXTENSION pg_weave;` both succeed in one database, with working casts |
| M3 | `tsvector`/`tsquery` casts and a `@@`-compatible operator so existing GIN queries run unchanged | a corpus of real `to_tsquery` queries returns identical row sets |
| M4 | pg_trgm surface: `%`, `similarity()`, `word_similarity()` over the `cgram` channel | parity on a randomized suite |
| M5 | `weave_migrate_from_pgvector(regclass)` / `..._from_gin(regclass)` that swap the index in place | TAP test: build a pgvector index, migrate, verify identical query results |
| M6 | `weave_check(regclass)` returning actionable per-channel health, and `weave_index_degraded()` | corruption TAP test detects each injected fault |

**Phase M gate:** a documented, tested path from a live pgvector + tsvector +
pg_trgm schema to pg_weave with **zero application query changes**.

---

## Phase P — performance and honesty

| id | task | gate |
|---|---|---|
| ~~**P1**~~ | **DONE.** `bench/` harness: corpus loaders (Wikipedia 20231101.en, MS MARCO, Cohere-wiki, BEIR subset), latency driver, nDCG scorer | one command reproduces every recorded result |
| ~~**P2**~~ | **DONE.** EC2 harness (`bench/aws/`) using the burner profile (`lava` as of 2026-09-11; `bene` before it, and `bene` expired mid-session with its credentials already dead when the switch was made — which is the argument for the harness never hardcoding an account id): launch, tune, measure, terminate | `bench/aws/run.sh` produces a `RESULTS_*.md` and terminates the instance even on failure |
| P3 | The competitive matrix: pg_weave vs pgvector(HNSW,IVFFlat), pg_search, pg_textsearch, VectorChord, tsvector+GIN, pg_trgm, pg_tre, pg_turbovec | `bench/RESULTS_MATRIX.md`, with every loss stated as plainly as every win |
| P4 | Cost model calibration against measured latencies so the planner picks the right channel | planner chooses the faster plan on ≥90% of a query workload |

**Phase P gate:** `bench/RESULTS_MATRIX.md` exists, is reproducible, and
contains a "where we lose" section. `pg_turbovec/docs/PARITY_GAPS.md` is the
house style: it retracted its own headline claim when a controlled benchmark
overturned it, and that is the standard.

---

## Phase R — 1.0

| id | task | gate |
|---|---|---|
| R1 | DocBook reference docs (`doc/pg_weave.sgml`) matching PostgreSQL's own doc conventions | renders; every SQL-visible object documented |
| R2 | Examples in `examples/`: hybrid search, RAG retrieval, log search, code search, faceted catalog | each runs against a fresh database via one script |
| R3 | PGXN release; `META.json` validated; `make dist` reproducible from a tag | uploaded, installs from PGXN |
| R4 | Managed-service readiness: `trusted`, no `shared_preload_libraries` requirement, no filesystem access outside the relation | a document a cloud vendor can act on |
| R5 | contrib-track submission: `-hackers` design proposal with the argument from `doc/ARCHITECTURE.md` §3 | thread exists; feedback triaged |

---

## Running tally of inherited debt

Carried from pg_fts and pg_tre, not yet scheduled above. Do not let this list
silently grow.

- No index-only scans (non-covering by design; probably permanent).
- No parallel *scan*, and no parallel vacuum (parallel build only).
- Ranked results over fuzzy/prefix/regex expansions are a correct but possibly
  incomplete subset; exhaustive retrieval requires `@@@`. Phase Z should either
  fix this or document it as permanent.
- Merge/vacuum leaves unreferenced blocks reclaimed only by REINDEX; no page
  recycler.
- No predicate locks, so no SSI support.
- Segment cap of 128 per index (`WEAVE_MAX_SEGMENTS`), a metapage-size limit.
- `tsquery` → `wquery` migration is a helper plus a cast, not transparent (M3).
