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

pg_weave's product definition is **a singular text index**: BM25, vector similarity,
fuzzy, approximate regex, prefix, and n-gram, over one docid space, in one
`CREATE INDEX`. Read the table against that: two of the six retrieval kinds work
today (BM25, prefix), one has a tested codec that no on-disk format calls yet
(vector), and three are imported code that nothing routes (fuzzy, approximate regex,
n-gram).

| capability | state | evidence |
|---|---|---|
| BM25 lexical search, boolean, phrase, NEAR, prefix | **works** | forked from pg_fts 1.5.8, current with upstream 1.6.0; 6 regression + 2 isolation + 105 TAP green on PG 17 and 18 |
| Index-native `count(*)` | **works** | inherited; measured ~200× faster than tsvector+GIN at 2 M docs (`bench/RESULTS_LEXICAL.md`) |
| Ranked top-k latency | **works, competitive at k=100** | after L14/L15/L17: rare k=10 1.51 ms (1.20× pg_textsearch), k=100 **wins** rare 3.96× and mid 1.27×; common k=10 still 4.75× behind (task L2). `bench/RESULTS_L17.md` |
| Build time | **at GIN parity** | 192.5 s vs GIN 202.7 s, pg_textsearch 49.2 s (3.91× behind). `bench/RESULTS_L15.md` |
| Index size | **best of the three** | 626 MB vs pg_textsearch 873 MB, tsvector+GIN 1120 MB |
| Crash recovery, replication, MVCC, CIC/REINDEX | **works** | inherited; `t/001`–`t/009` |
| Vacuum, tombstones, tiered merge | **works** | Explicit `weave_vacuum()` reclaims **2289 → 264 pages** after deleting 90% of 120k rows, in one pass (`t/008`). Plain `VACUUM`/autovacuum also reclaims: **1093 → 94 pages**, then stable across four more cycles, with 73 low-bias page reuses (`t/015`). **The row said "partial — autovacuum does NOT reclaim tombstoned space" for one day and that was wrong:** it was measured on a cluster with `autovacuum = off` and no other activity, so nothing advanced the transaction-id horizon that every page's recyclability is gated on. A real, narrower defect was found and fixed the same day (G18): with a *stalled* horizon and the free-page trigger firing, the relocation pass reused nothing and ratcheted the index up +73 pages per cycle without bound; it now skips a pass that cannot pack. Until 2026-09-14 nothing reclaimed tombstoned space at all and the only recovery was `REINDEX` (G14) |
| Multi-channel on-disk substrate: per-bolt channel descriptors, extended page-kind space, versioned metapage reader | **works** | phase X (format v6); `t/010` upgrades a 20,000-row v5 index and proves byte-identical answers; `t/011` proves a corrupted descriptor `ERROR`s cleanly; 852,070 exhaustive kind-space checks |
| `weave_check()` invariant verification | **partial** | 11 invariants, incl. chandesc reachability and page-kind classification. Before phase X only `weave_check_meta()` existed. ~14 of `SEGMENT_FORMAT.md` §9 still owed |
| Vector quantizer: rotation, codebook, encode/decode, packing | **works** | 17,741 + 1,909,440 property checks, 0 failures (tasks V2–V5) |
| Vector block scoring kernels | **partial** | scalar oracle + `lut-wide` + `lut-avx2`, verified `memcmp`-identical over 308,278 differential checks; dispatch resolved once at `_PG_init`. AVX-512, NEON and the approximate families are unmet and tabulated as unmet (`VECTOR_CHANNEL.md` §8) |
| `wvec` type: I/O, typmod, casts, 4 distance operators, arithmetic, btree | **works** | task V1; `sql/wvec.sql`, green on PG 17 and 18 |
| Quantizer reachable from SQL (`weave_quantize_roundtrip`) | **works** | lets reconstruction error be measured on a real corpus before the index exists |
| Vector *indexing* (the AM accepting a `wvec` column) | **does not exist** | tasks V7–V9. **This is the gap between pg_weave and its own product definition** |
| Vector storage pages, IVF, ANN | **does not exist** | tasks V7, V9, V13, V14 |
| Fused-threshold top-k | **works, and 2 of 5 §8 gate rows pass** | F1/F2/F5–F8 done; the flagship `fuse(body <=> q, emb <#> v)` is answered by the index. `bench/RESULTS_FUSE.md`: recall vs exhaustive 1.000 (raw objective only, G46) and nDCG@10 **above** RRF on three BEIR corpora (1.053× / 1.010× / 1.114×) with the per-key normalizer — while **p50 (0.710× / 0.827× / 1.172× vs ≤ 0.50×), p99 (0.710× / 0.612× / 1.000× vs ≤ 0.70×) and the `score()`-call ratio (0.571× / 0.875× / 0.513× vs ≤ 0.20×) all FAIL**. **Measured on EC2 2026-09-22 (night) for the shipping scorer: p99 used to PASS at 0.609× / 0.560× / 0.633× on the raw sum, so the normalizer TRADED p99 FOR nDCG — and on fiqa the fused arm is now SLOWER than the RRF control it replaces** (p50 1.172×; the normalizer costs +19 % / +3 % / +104 % of p50, fiqa's doubled). Cause is the pivot walk, not the pre-scan pass: fiqa pivots 5.3×, vector lanes +4.6 %. nfcorpus also loses recall@100 and MRR@10 to RRF where nDCG wins. **DECIDED 2026-09-22: `pg_weave.fuse_normalize` STAYS ON BY DEFAULT** (a user chooses a ranking, not a scan strategy; off, the objective loses to RRF on all three corpora, and the GUC is `PGC_USERSET` so the old trade is available per query or per session). The work row is also **restated per channel** the same day — lexical BM25 contributions 0.203× / 0.380× / **0.052×** (**met on fiqa**), vector **code blocks read 1.000× everywhere** (failed), pivots per query reported and not gated. **The single blocker is the size of the vector channel's candidate set** |
| pgvector / tsvector compatibility | **does not exist** | specified only; M1–M3, and 1.0 requirements rather than polish |
| Fuzzy / approximate regex / n-gram channel | **compiles, unreachable** | Z1/Z2 done (TRE vendored at `f864ed0` and current, GUCs wired); no routing (Z3–Z7). **Four of the six named product capabilities are here**, so this is on the 1.0 path |
| Prefix search (`term*`) | **works via the lexical dictionary walk** | inherited; measured 3.8–7.7× faster than tsvector+GIN (`bench/RESULTS_LEXICAL.md`). Z4 re-routes it through SuRF, which must not regress it |
| Unanchored cross-token substring (n-gram / `cgram`) | **does not exist** | task Z8, **now required** rather than opt-in. Its cost ships with it: with `cgram` on, pg_weave is not smaller than `pg_trgm` |

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
   **STATUS 2026-09-22: it exists and F5 is MET** (1,140,000 trials / 37,765,994 checks,
   0 failures), and the real-corpus correctness gate is 299 of 299 comparable queries
   exact against an exhaustive per-channel oracle. What is *not* met is the §8
   performance/quality gate, now **3 of 5 rows** (recall, p99, nDCG) with **2 failing**:
   p50 latency (0.582–0.795× against ≤ 0.50×) and the channel `score()`-call ratio
   (0.541–0.903× against ≤ 0.20×). Those two rows, plus the unmeasured cost of the
   normalizer's pre-scan pass, are what a fused 1.0 claim is blocked on — no longer the
   ranking. `bench/RESULTS_FUSE.md`.
   **CORRECTED 2026-09-22 (night) by the EC2 re-run of the SHIPPING scorer — the gate is
   2 of 5, not 3 of 5, and the missing row is p99.** Measured with the normalizer on:
   p99 **0.710× / 0.612× / 1.000×** against ≤ 0.70× (**FAIL**; it passed at 0.609× / 0.560× /
   0.633× on the raw sum), p50 **0.710× / 0.827× / 1.172×** (**FAIL**, and on fiqa **slower
   than the RRF control**), `score()` **0.571× / 0.875× / 0.513×** (**FAIL**). The normalizer
   that won the nDCG row costs **+19 % / +3 % / +104 %** of p50 — so **the change traded p99
   for nDCG**, and the "unmeasured pre-scan pass" was the wrong suspect: the cost is the pivot
   walk (fiqa pivots 5.3×, vector lanes +4.6 %). **What a fused 1.0 claim is blocked on is now
   three rows with one cause** — the size of the vector channel's candidate set — ~~**plus an
   open maintainer decision on whether `pg_weave.fuse_normalize` stays on by default**~~
   **[DECIDED 2026-09-22: IT STAYS ON. A user chooses a ranking, not a scan strategy; with the
   normalizer off the fused objective loses to a plain RRF control on all three corpora
   measured (0.982× / 0.924× / 0.687×), and a fused scan that ranks worse than the control it
   replaces has no reason to exist. The price is named rather than absorbed — p99 PASS → FAIL,
   p50 worse on every corpus, fiqa slower than the control — and `PGC_USERSET` means the old
   trade is available per query or per session, which is why this is a default and not a fork
   in the design. The latency regression is charged to the blocker above, not accepted as
   permanent. §8's work row was restated per channel the same day and the vector half still
   fails at 1.000× in blocks, so the gate stays at 2 of 5]**, since
   its two settings each fail a different gate row. `bench/RESULTS_FUSE.md` (fourth and fifth
   measurements), `doc/GAPS.md` G44.
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
18. ~~A page recycler, so merge does not leave space only REINDEX reclaims.~~
    **DELIVERED 2026-09-14 (L18 + L19).** `weave_vacuum()` reclaims tombstoned
    space (2289 → 264 pages after a 90% delete) and plain `VACUUM`/autovacuum does
    too (1093 → 94, then stable). A pass that cannot reuse the pages it frees is
    now skipped rather than run, which removes a pre-existing unbounded ratchet
    (G18). The follow-on WAL-batching task was **withdrawn**: the upstream
    measurement it rested on was retracted, and the real rate is 0.005 ms/page
    (G17).
19. Predicate locks, hence SSI support.
20. `EXPLAIN` output that shows per-channel work, so a slow query is diagnosable.

## Known permanent limitations

These will not be fixed and belong in any evaluation:

- **No index-only scans.** The index is non-covering by design.
- **Exact recall × sublinear latency × minimal storage: pick two.**
- **Unanchored cross-token substring search costs `pg_trgm`-scale storage.** This
  used to be phrased as a limitation avoided by keeping the corpus-trigram channel
  opt-in. Since `n-gram` is a named product capability (task Z8), the honest form is:
  the capability ships, and an index with `cgram` on **is not smaller than
  `pg_trgm`**. The reloption remains so an index that does not need the capability
  does not pay for it.
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

**The product is a singular text index.** Settled 2026-09-10: BM25, vector
similarity, fuzzy, approximate regex, prefix, and n-gram, over one docid space, in
one `CREATE INDEX`. All six ship, so phases L, Z and V are all on the path to 1.0
and none is optional.

*This section was rewritten twice on the same day — once to drop Z from the path
when the scope was read as "BM25 + vector", then back when it was restated as all
six. Both are in `git log`. The lesson recorded in `AGENTS.md` hard rule 7 is that a
hard rule was weakened on an inference about scope rather than a question about it.*

**Also closed 2026-09-22, and it is the most consequential correctness finding this project
has recorded: `doc/GAPS.md` G43.** The fused path returned a **plausible, correctly-ordered,
wrong** top-k on BEIR scifact — six of ten rows wrong, every row defensible in isolation,
found the first time a pg_weave index was built over a real text+vector corpus. The cause was
not in the fused core, not in the vector channel, and not in any bound: `wand_skip_blocks()`
in the lexical posting cursor decides a block lies below the seek target by reading **the next
block's header**, which for a term's FINAL block belongs to another term, so the cursor
declared itself exhausted with its last block never decoded. A term with df 211 in blocks of
128 + 83 silently lost 83 postings.

Four things follow, and they are readiness statements rather than a bug report:

1. **The fused scan is the first caller that exercises the posting cursor's seek contract at
   full generality.** Instrumented, the plain ranked path reaches that inference **zero**
   times on the same corpus and queries. So passing single-channel tests means a channel has
   been tested against one caller, and every other channel should be assumed to be in the
   same position until a fused scan has driven it.
2. **No fixed-expected-output test can catch this class**, and none did — hard rule 1's
   argument arriving from page layout rather than from a bound's arithmetic. What caught it
   was a benchmark harness with a correctness gate in front of it, against an exhaustive
   per-channel oracle. That gate is now the cheapest real-corpus assurance this project has.
3. **Two claims were retracted in place** (hard rule 13): the vector-ceiling hypothesis, and
   "the (C2) check ran and did not fire" — the latter because the GUC had been defined inside
   `#ifdef WEAVE_TEST_HOOKS` and did not exist in any shipped build, while `SHOW` echoed back
   `on` from a placeholder. An absent GUC is indistinguishable from one that is off.
4. **The §8 benchmark table is now blocked only on EC2 time**, not on correctness. Gate:
   25 of 25 judged scifact queries, 0 mismatches.

**And the run happened the same day, with a result that does not flatter the design
(`bench/RESULTS_FUSE.md`).** Three BEIR corpora, real MiniLM embeddings, EC2, an RRF
control over the same single index. **Two of five §8 rows fail, so Phase F is not
claimable:**

- **nDCG@10 is WORSE than RRF on all three datasets** — 0.982×, 0.924×, and **0.687×** on
  fiqa — and recall@100 is worse too, so it is not a top-10 cut artefact. The fused scan
  is *exact* (recall-vs-exhaustive 1.000, 299 of 299 comparable queries), so this is the
  objective and not the scan: it sums **raw** BM25 (~10–20) against a **raw** quantized
  inner product (~[−1,1]), a 33× scale mismatch, so `{0.5,0.5}` weights are effectively
  lexical-only while RRF is scale-free by construction. `doc/GAPS.md` **G44**.
  **Consequence for the four claims: claim 2 is UNSUPPORTED, not retracted.** Its
  mechanism demonstrably works; a user choosing between this and RRF is choosing a
  ranking, and right now the ranking is worse. Claim 2 must not be quoted as measured.
- **The `score()`-call ratio fails (0.54–0.90× against a 0.20× gate), and the cause was
  already on disk.** The lexical side clears the gate unaided on the largest corpus
  (0.149×); the vector side is 0.956–0.991× of the control, and it is 71–87 % of all calls.
  `bench/RESULTS_BOUND_PRUNING.md` measured that bound pruning 0.0 % long before this run
  and G43 wrote down the generalization — *a bound computed but not acted on is a latent
  defect*. Hard rule 9 was satisfied in letter and missed in spirit: the measurement
  existed, the inference did not. **This is the most reusable lesson of the week — taking
  a measurement is not the same as drawing its consequence.**
  **CORRECTION 2026-09-22 (evening), reasoning not result:** this bullet cited
  `vec_blocks_bound_skipped = 0 on every dataset` as the evidence. That counter is
  **structurally zero in any fused scan** — it needs the vector shuttle's own threshold
  floor, set only by `weave_vec_shuttle_set_threshold()`, whose sole caller is the
  `weave_vec_scan()` SRF driver — so it could not have fired regardless of the bound's
  quality, and the tree said so in a comment nobody propagated. The failing ratio is a
  direct measurement and is unchanged; the bound's uselessness rests instead on 0.00–0.01 %
  of blocks pruned at an **oracle** θ (`bench/RESULTS_CODE_SCAN.md`) and B2 ≈ 1.0 by
  construction. `blkskip` is the informative counter but sums a combined bound over all
  channels, so it cannot attribute a skip to the vector channel.
  `bench/RESULTS_FUSE.md`, `doc/GAPS.md` **G46**.

What cleared: the recall row (§8's most valuable), and a real latency win — p99
0.56–0.63× of RRF, with an A/A leg putting the within-arm spread at 0.001–0.013 ms
against between-arm deltas **170–714× larger**. That A/A leg did not exist until this run
and is why the latency numbers are admissible at all under hard rule 10.

**AND THE nDCG ROW WAS FIXED AND RE-MEASURED THE SAME DAY, so the §8 gate is now 3 of 5
rows passing (recall, p99, nDCG) and 2 of 5 failing (p50, `score()` calls).** [**CORRECTED
2026-09-22 (night): 2 of 5 — p99 FAILS for the shipping scorer. Dated block below.**] G44's
fix is
a **per-key** ceiling normalizer — every channel of a `fuse()` key carries weight
`w_key / N_key` where `N_key` is the key's pre-scan ceiling — taken **query-global** rather
than per bolt, because the fused pass merges per-bolt top-k lists by score and a per-bolt
normalizer would make the answer depend on the segment layout and change silently after
INSERT, VACUUM and merge. New GUC `pg_weave.fuse_normalize`, default on; `off` restores the
raw sum, the arm every earlier figure was measured on. Measured **in the product**, not
offline (`bench/normprod.sh`: three arms that are the same statement differing only in the
GUC, plus the RRF control, through `bench/ndcg.py`; no EC2, because nDCG is deterministic
and host-independent):

| dataset | raw sum | RRF control | ceiling-normalized | norm ÷ RRF |
|---|---|---|---|---|
| scifact (300 q) | 0.6720 | 0.6846 | **0.7212** | **1.053×** |
| nfcorpus (323 q) | 0.3161 | 0.3422 | **0.3455** | **1.010×** |
| fiqa (648 q) | 0.2393 | 0.3482 | **0.3878** | **1.114×** |

The `raw` arm reproduces the recorded EC2 numbers to four decimals as its positive control,
and recall@100 improves too (scifact 0.8892 → 0.9683, fiqa 0.5141 → 0.7079), so it is not a
top-10 reshuffle.

**The loss in that run, stated with the win: nfcorpus is not a clean victory.** There the
normalized arm loses **recall@100 (0.3206 vs 0.3251)** and **MRR@10 (0.5441 vs 0.5514)** to
RRF while winning nDCG@10 — the gate row — by 1.0 %. One metric ahead, two behind.

**What this does and does not do to claim 2.** `ARCHITECTURE.md` §9 claim 2 is
*fused-threshold top-k rather than over-fetch-plus-RRF*: its **ranking** half is now
supported on three public datasets (with the nfcorpus caveat), and its **work-reduction**
half is still **UNSUPPORTED** — the `score()`-call row fails at 0.648× / 0.903× / 0.541×
against a 0.20× gate because the vector block bound prunes nothing and the vector channel
sets 71–87 % of all fused `score()` calls. Quote the ranking, not the work saved.

**And one new caveat that is unmeasured rather than failed:** the normalizer adds a pre-scan
pass — one LUT build and one directory fold per bolt per vector key — with **no latency
figure taken since the change**. [**MEASURED 2026-09-22 (night): +19 % / +3 % / +104 % of p50,
and the pre-scan pass is not where it lives — the pivot walk is. This caveat is now a FAILED
ROW, not an unmeasured one. Dated block below.**] The directory is kilobytes against a code weft of
megabytes, so the cost is expected to be small, but the p50/p99 rows in
`bench/RESULTS_FUSE.md` now describe a scorer that no longer ships. They are marked **stale
in place, not retracted** (hard rule 13), and an EC2 re-run of `bench/fuse.sh` is owed before
any fused latency figure is quoted again.

**So what blocks a 1.0 fused claim, precisely:** the p50 row, the `score()`-call row (i.e.
vector candidate reduction — `bench/RESULTS_BOUND_PRUNING.md`, Phase V V13/V14/V15/V18), and
the unmeasured cost of the normalizer's pre-scan pass. Ranking quality is no longer on that
list. [**CORRECTED 2026-09-22 (night): the p99 row joins the list, the normalizer's cost is no
longer unmeasured (+19 % / +3 % / +104 % of p50) and it was never the pre-scan pass. Dated
block below.**]

**REVISED 2026-09-22 (evening): the `score()`-call row does not need a better bound, it
needs a RESTATED UNIT or a LAYOUT CHANGE.** Work counters for the shipping scorer
(`bench/normprod.sh`; `bench/RESULTS_FUSE.md`, third measurement, with the `raw` arm
reproducing the EC2 figures as its positive control) show the normalizer *improving* the
gated row on all three corpora (0.648 → 0.571, 0.903 → 0.875, 0.541 → 0.513) and the
lexical side sharply (fiqa 0.149× → 0.052×), while the **vector side goes to exactly
1.000×** and stays there across seven weight ratios from 0.0625 to 4. The observed cause is
two-part: a dense channel whose weighted ceiling (0.4947 of a 1.0 partition ceiling) sits
above θ (0.1106) forces the pivot to visit **every** document, and in `WEAVE_PACK_LANE`
reading one lane touches every byte of its block, so scoring 1 lane costs the same memory
traffic as scoring 32 (`bench/RESULTS_CODE_SCAN.md:330,417`). So the blocker list above is
refined rather than replaced: **this row needs either (a) §8's row restated in blocks or
bytes rather than lanes, or (b) a second vector-major copy of the codes — forfeiting the
storage gate — or (c) a cluster-ordered weft, which contradicts the strictly-ascending-docid
requirement the fused vector channel depends on** (`include/weave/vecdocmap.h:35,105,122`).
All three are maintainer decisions, ~~presented and not taken~~ **— (a) was TAKEN 2026-09-22 as
a MEASUREMENT decision and did not rescue the row (in blocks the vector ratio is 1.000×,
exactly as in lanes); (c) was TAKEN 2026-09-23 and WITHDRAWN THE SAME DAY BY MEASUREMENT;
(b) is now the only one left, and it is the single blocker. Dated
block below.** `doc/GAPS.md` **G46**,
`doc/specs/FUSED_TOPK.md` **sect. 8d**.

**(c) IS DEAD, MEASURED 2026-09-23 BEFORE IT WAS BUILT.** `bench/code_scan.c` already carried
`order=clustered|natural` with a real k-means, so decision (c) cost one TSV→`.fvecs`
converter and an hour instead of a page-format version bump, a 4-byte/doc docid→lane
indirection, a merge path rewritten from merge-sort to re-clustering, and a reopened F8. On
the three BEIR corpora the nDCG row uses: **0.00 % of blocks pruned and 100.00 % of lanes
scored in BOTH orderings, and 0.00 % at an ORACLE threshold**, which is the ceiling over every
possible block ordering. Clustering is not failing to happen — the clustered arm's mean block
radius really is smaller (1.0436 → 0.9992 on scifact) — it is failing to matter, because the
bound needs a **2.12–3.40×** smaller radius and gets 4 %. And the knob has no better setting:
at `lists > n/32` one 32-lane block spans several clusters and the radius returns to the
natural-order value. `bench/RESULTS_CLUSTER_ORDER.md`; V13 is withdrawn in `doc/PHASES.md`.

**DECISION REVIEW, 2026-09-23, asked for explicitly and answered against the measurements
rather than against the reasoning that produced them.** Three decisions were taken that day.

- **(c) cluster-ordered weft + reopen F8 — WRONG, and the refuting evidence was already in
  this tree.** `doc/PHASES.md` V13 had recorded the same experiment's 0.00 % since
  2026-09-13; `FUSED_TOPK.md` §8d carried (c) as live anyway, and the decision was taken
  from §8d. Cost: one hour, no code, and two results the older entry did not have (the
  oracle-theta ceiling, and `lists` being non-monotone). The withdrawal is now **complete**
  rather than partial: cluster ordering had three possible payoffs — tighter bounds
  (measured dead), cluster probing (ruled out by V9's 4-bit measurement), and candidate
  contiguity (dead by construction, because a fused scan's candidate set is defined by the
  gate, not by vector similarity). Recorded as a stale-option-list failure in `AGENTS.md`.
- **Upstream reports as committed docs — correct**, and cheap to revisit since they are
  unpushed.
- **Deferring the EC2 re-run — correct, and it looks better in hindsight than when it was
  taken.** The two measurements that landed afterwards (cluster ordering, the gate sweep)
  both changed what an EC2 run should measure; a paid run that morning would have measured
  the pre-correction hypothesis, and the gated latency curve claim 3 needs was not yet in
  any harness.

**And the recommendation that replaced (c) needed correcting too, which is the more useful
half of this review.** "Page traffic would follow the blocks-scored curve (0.031×)" was
wrong: the block directory and the warp map are read in full on every scan regardless of the
gate, so the real projection is **1.8× / 4.3× fewer total query buffers** (0.06–0.07× of the
vector weft), measured from geometry and confirmed by a lexical-only ablation that puts the
vector channel at 61 % of scifact's buffers and 89 % of fiqa's. The *direction* survives —
addressing, not layout, and the vector-major copy stays dominated — but the on-disk index is
probably unnecessary: the code chain measures **98.3–98.5 % dense**, so a speculative
`codestart + b × strips_per_block` validated by the cursor's existing check, with a fallback
to the chain walk, gets the same win for zero format change. Hit rate on a merged and
vacuumed index is the measurement that decides it.

**MEASURED ON EC2 2026-09-22 (night), run `pgweave-20260922-224507`: THE OWED LATENCY RE-RUN
HAPPENED, AND IT IS A LOSS — §8 IS 2 OF 5, THE p99 ROW WENT FROM PASS TO FAIL, AND ON fiqa THE
FUSED ARM IS SLOWER THAN THE RRF CONTROL IT EXISTS TO REPLACE.** `c7i.8xlarge` (32 vCPU),
us-east-2, PG17, extension 0.19.0, commit b0bd1b7, real `all-MiniLM-L6-v2` embeddings computed
on the instance, the same three BEIR corpora, RRF `k'=100`/`k=60` control over the same index,
50 queries × 7 reps alternated per query with an A/A leg; instance terminated and verified, no
orphaned volumes, keys or security groups.

| row | gate | scifact | nfcorpus | fiqa | on the raw sum | |
|---|---|---|---|---|---|---|
| recall vs exhaustive | 1.000 | 1.000 | 1.000 | 1.000 | — | **PASS** (raw objective only — G46) |
| nDCG@10 | ≥ RRF | 1.053× | 1.010× | 1.114× | 0.982× / 0.924× / 0.687× | **MET** |
| p99 latency | ≤ 0.70× | 0.710× | 0.612× | **1.000×** | 0.609× / 0.560× / 0.633× | **FAIL on two of three — WAS PASS** |
| p50 latency | ≤ 0.50× | 0.710× | 0.827× | **1.172×** | 0.582× / 0.795× / 0.578× | **FAIL**, fiqa slower than the control |
| ~~`score()` calls~~ | ≤ 0.20× | 0.571× | 0.875× | 0.513× | 0.648× / 0.903× / 0.541× | **FAIL — row SUPERSEDED 2026-09-22 by the per-channel restatement below; the unit changed, the verdict did not** |

**The gate was 2 of 5 before the normalizer (recall, p99) and it is 2 of 5 after it (recall,
nDCG): the change TRADED p99 FOR nDCG.** The normalizer's own p50 cost is **+19 % / +3 % /
+104 %** — fiqa's p50 doubled, 10.889 → 22.163 ms, the same statement one GUC apart — and the
cost is **not** the pre-scan pass this document has been calling unmeasured: fiqa's pivots rise
5.3× (7,081,750 → 37,306,460) while the vector channel's lane count moves 4.6 %, so the bill is
the **pivot walk** the dense channel's ceiling above θ forces. Hard rule 10 is satisfied:
|fused − `fused_aa`| p50 = 0.014 / 0.003 / 0.035 ms against between-arm deltas 30×–320× larger.
Quality and correctness reproduced the local figures exactly, which is a cross-harness control.

**Two consequences for this document.** The three failing rows have **one** cause and therefore
one blocker — the size of the vector channel's candidate set, i.e. the (a)/(b)/(c) choice above,
of which (a) cannot help p50 or p99 at all. And a **new open maintainer decision** belongs on
the list: **should `pg_weave.fuse_normalize` stay ON by default?** On, the ranking beats RRF on
three corpora and the scan is slower than RRF on the largest; off, the scan is fast and the
ranking loses to RRF on all three, which is the state that made claim 2 unsupported to begin
with. The GUC is `PGC_USERSET`, so a user can already choose per query, and this is the first
knob in the project whose two settings each fail a different gate row. ~~**Presented, not taken.**~~
`bench/RESULTS_FUSE.md` (fourth measurement), `doc/GAPS.md` **G44**,
`doc/specs/FUSED_TOPK.md` **sect. 8b** and **8d**.

**BOTH DECISIONS TAKEN 2026-09-22, AND THE GATE DOES NOT MOVE: IT IS STILL 2 OF 5. The list of
open maintainer decisions for the fused scan is now EMPTY, and the blocker list is ONE ITEM
LONG.**

1. **`pg_weave.fuse_normalize` STAYS ON BY DEFAULT.** A user chooses a **ranking**, not a scan
   strategy: with the normalizer off the fused objective loses to a plain RRF control on all
   three corpora measured (0.982× / 0.924× / 0.687×), and a fused scan that ranks worse than
   the two-query control it replaces has no reason to exist; with it on the ranking beats RRF
   everywhere measured (1.053× / 1.010× / 1.114×). **The price is recorded and not hidden:**
   p99 **PASS → FAIL** (0.609× / 0.560× / 0.633× → 0.710× / 0.612× / 1.000×), p50 0.582× /
   0.795× / 0.578× → 0.710× / 0.827× / 1.172×, and on fiqa the fused arm is slower than the
   control. Because the GUC is **`PGC_USERSET`**, a deployment that wants the old trade can
   have it per query or per session — **which is why this is a default and not a fork in the
   design** — and the latency regression is **not accepted as permanent**: it is charged to the
   one open blocker, the size of the vector channel's candidate set.
2. **§8's work row is RESTATED per channel, in each channel's own unit:** lexical **BM25
   contributions** (gate ≤ 0.20×), vector **CODE BLOCKS READ** against the control's own code
   scan (gate ≤ 0.20×), and **pivots per query reported but NOT gated**, because the RRF control
   has no pivot loop and there is nothing to take a ratio against. Two measurements forced it: a
   one-lane read in `WEAVE_PACK_LANE` touches every byte of its block, so **a lane is not a unit
   of cost and a block is** (measured in the V15/V16 work,
   `bench/RESULTS_CODE_SCAN.md:330,417`); and the lane-based row said normalization made the
   fused arm *cheaper* on all three datasets (0.648→0.571, 0.903→0.875, 0.541→0.513) while the
   clock said fiqa's p50 **doubled** — a work row that cannot predict the latency row is
   measuring the wrong thing, and the **pivot count** is what tracks the clock (fiqa pivots
   5.3×, p50 2.0×).

| row | gate | scifact | nfcorpus | fiqa | |
|---|---|---|---|---|---|
| lexical work: BM25 contributions | ≤ 0.20× | 0.203× | 0.380× | **0.052×** | **MET on fiqa**, missed on scifact (just over) and nfcorpus |
| vector work: **code blocks read** | ≤ 0.20× | 1.000× | 1.000× | 1.000× | **FAIL everywhere** |
| pivots per query | *reported, not gated* | 5,183 | 3,627 | 57,572 | corpora of 5,183 / 3,633 / 57,600 documents — **a full pass over the docid space** |

**Restating the row does not rescue it:** in blocks the vector ratio is **1.000×, exactly as it
was in lanes**, so decision 2 buys **honesty about the unit, not a pass**. What it buys
operationally is that the two halves fail separately: the lexical half is **met on fiqa** and the
vector half is missed everywhere.

**And the reason the vector half cannot be fixed cheaply is now arithmetic rather than a
suspicion.** Incremental abandonment fires **constantly** — on fiqa **36,709,890 abandonments
over 37,306,460 pivots, 0.98 per pivot** — which is what produced the lexical improvement (2.8×
fewer BM25 contributions). It cannot help the vector channel because the fused core sums scored
channels in **descending weighted ceiling** order and the normalized vector channel carries
**0.4947** against **0.0110** per lexical channel, so it is summed **first** and scored before
any abandonment test can run. **Reversing that order is dead on arithmetic, not on effort:** the
test would be `s_lex + w_vec * block_max_vec <= theta`, and `w_vec * block_max` is ~0.49 against
a theta of ~0.11; it would still be dead at k = 10. A per-block vector bound carries no
information on L2-normalized data either ((B1), (B2), (B3) all ≈ 1.0 unless a block is coherent
in direction). **So a fused 1.0 claim is blocked on exactly one thing: a smaller vector
candidate set, by (b) or (c).** `bench/RESULTS_FUSE.md` (fifth measurement), `doc/GAPS.md`
**G44**, `doc/specs/FUSED_TOPK.md` **sect. 8** and **8d**.

**43 of 77 tasks are done** (`doc/PHASES.md`), phase X included, with 4 partials (V6,
Z5, Z8, Z9) and 6 withdrawn (L2, L21, V9, V10, V13, **F4**). By phase: X 4/4, L 16/20,
Z 7/9, V 9/20, P 2/4, **F 6/9**, M 0/6, R 0/5. **Five rows were added on 2026-09-21
and every one of them was created by work that ran, not by planning:** **F8** and **F9**,
because F2.2's pushdown found that the channels do not share a position space (lexical and
gate shuttles publish docids, the vector shuttle publishes segment-local lane indices, `<@>`
publishes dictionary positions) so it decided the **docid space** and offered no path for the
other two. **F8 is now DONE (2026-09-22) and the flagship `fuse(body <=> q, emb <-> v)`
query is answered by the index** -- and it fixed a live wrong answer on the way (`doc/GAPS.md`
**G41**: `fuse()` had no score-recovery function for `<->`, `<#>` or `<@>`, so a raw DISTANCE
entered the sum as a score and the FARTHEST vector ranked first; the defect was printed in a
checked-in expected file and reviewed three times, because both arms were wrong in the same
direction and every assertion compared the two arms). Also **V16**, **V17**, **V18** from a read-only review of Alibaba's zvec
(ideas only, hard rule 6). V16 is the one that matters soonest: cosine is refused today for
want of a stored maximum true norm, and pg_weave already stores a per-lane `(scale, norm)`
pair, so cosine may be a **storage** decision rather than an unsupportable metric. V17 is
**claim 3's missing mechanism** -- switch plan strategy on predicate selectivity -- and claim
3 has been unmeasured since F4's withdrawal.

**Also closed 2026-09-21: G34 and G35**, the two cgram gaps, with the closure proved by a
**pre-fix control run** rather than by a green suite (0/2/0 before, 1/1/1 after); format
v10. Hard rule 12 debt is recorded: that work touches merge, so a run at scale is owed
before it counts as durable. **Phase F grew by two rows on 2026-09-21,
both found by reading the code before writing any, and both are now DONE the same day:**
**F6**, a BM25 term shuttle (the lexical channel had none — ranked lexical is a WAND over
`WandCursor`, and the fused core consumes only shuttles), and **F7**, an `ORDER BY`
operator for the vector channel (`wvec_weave_ops` was `STORAGE` only, so a vector query
could not reach `amrescan` at all). So the fused scan now has two channels to fuse, which
it did not when the session started, and F2.2 is what connects them.

**F7's first cut was a wrong answer and the corrected test measures how wrong.** It
registered `<=>`, which is cosine, while the scan core serves only IP and L2 and refuses
cosine (`include/weave/vecscan.h:53`) — so the index answered an **l2 ordering under a
cosine operator**, and the file's own "quantizer divergence" section was measuring that:
24 of 25 positions differing, overlap 19 of 25. With `<->` and `<#>` registered instead
— the operators whose semantics the weft actually computes — the same section measures
**4 of 25 and overlap 25 of 25**. An `ORDER BY` operator an index serves has to be the
ordering the index computes; the remaining debt is one operator family per metric
(`VECTOR_CHANNEL.md` §8b), because a `metric` reloption is invisible to the planner and
can drift from the weft under `ALTER INDEX ... SET`.

**The lexical channel was re-measured on 2026-09-21 at two scales with a pg_fts arm for
the first time, and it changes what this project may claim.** Against tsvector + GIN,
pg_weave is now ahead on everything except three rows that are behind by **10–30 µs**
(ranked rare k=10/k=100 and `count(*)` AND) — including **index size, 1.73–1.79× ahead**,
and **build time, 1.03–1.46× ahead**, the two dimensions Phase L used to fail on
outright. **The Phase L gate was RESTATED IN ABSOLUTE TERMS on 2026-09-21 (maintainer
decision) and its GIN half is now MET:** no row behind by more than 0.05 ms, none behind
at all where either arm exceeds 0.10 ms, size and build not behind. The old phrasing was
"zero measured losses", which a 10 µs difference cannot satisfy and a 0.01 ms reporting
resolution cannot measure. **The three rows are still losses and are still recorded as
such** (G3, hard rule 8) — the gate changed, the code did not. **The third-party half of
the gate (P3: pg_search, pg_textsearch, VectorChord) is still owed.**

**The arm that matters more is pg_fts, because pg_weave is a fork of it and the two had
never been compared.** Earned: the bare `ORDER BY` index path (L7 — upstream v1.8.3 still
seq-scans it; **3,705× at 1M, 11,853× at 4M**), mid-band ranked latency (7.0× / 5.0×,
L14 + L17), common-band ranked (1.86× / 1.61×), and **index size 3.5× / 3.0× smaller
than the fork** (L8/L12/L17/L18). **Inherited, as exact ties at both scales:** `count(*)`
pushdown and prefix `count(*)` — so the 133–144× `count(*)` margin over GIN is pg_fts's
custom scan renamed, and `doc/ARCHITECTURE.md` §9's four claims correctly exclude it.
**One loss to upstream, visible only at scale:** build, 1.08× slower at 4M. **And one bug
found in both forks and fixed only here**, the same day: `count(*)`'s visibility gate was
O(heap pages) and selectivity-independent, making it up to **40× slower than the ordinary
path** below ~df 9,000 — `doc/GAPS.md` G38, now closed, measured at 0.003–0.005 ms flat
where it had been 0.278–0.291 ms. pg_fts has the identical loop at
`pg_fts_am_scan.c:4402`, so **an upstream bug report is owed** (the third: pg_fts's gate
premise and harness artifact, pg_tre's `uleven.c` OOB read, and now this). Details and
two acknowledged confounds in `bench/RESULTS_LEXICAL.md`; new gap **G38** is a 2.2×
`count(*)` slowdown since 2026-09-07 that the fork reproduces to 0.01 ms, which is how
it is known not to come from pg_weave's divergence.

**Phase F started on 2026-09-20 under a SCOPED WAIVER of hard rule 7, and the waiver's
terms are part of the status. The waiver was WIDENED TO F2 AND F3 on 2026-09-21.** L's
GIN half now passes under the restated absolute gate; **Z and V still do not pass** — Z's
own gate artifact `bench/RESULTS_FUZZY.md` was never written, V8's GIST-960d latency gate
is unrun — and
the waiver is recorded at `doc/PHASES.md` "Phase F" with what was checked and why
bounded top-k is nonetheless the one lever every unmet Z/V latency gate needs. **F1 and
F5 are done; F2 is in progress and F3 is sequenced behind it** (F3 projects a scan only
F2's pushdown can start, which is a structural dependency and not a waiver term),
**F4 is withdrawn** because the vector proximity graph it integrated was
withdrawn in Phase V, and the §8 gate's blocker moved: `bench/lexical.sh` **has** been
re-run, so its RRF control no longer has a stale lexical arm, and `bench/RESULTS_FUSE.md`
is now blocked by F2 alone — plus the nDCG rows on two public datasets, which this
project has never run at all.

**And one blocker nobody had counted, found on 2026-09-22 by starting the harness:** the
§8 gate's decisive row is the `score()`-call ratio, and **no query could read that number**
— the core has counted since F1 into structs that die with the scan's memory context.
`weave_fuse_stats()` (extension **0.18.0**) carries them out, with a `block_max()` count
beside them so the ratio cannot be passed by trading score calls for bound calls. It is
worth recording as a pattern rather than a fix: this is the third time a gate on this
project turned out to be blocked on an instrument rather than on the feature it names
(the allocator counters in 0.8.0 and the channel-mechanism counters in 0.9.0 were the
other two), and each time the instrument settled something the same afternoon. Here it
was that the fused scan calls `block_max()` 1.7× more often than `score()` — a cost the
§8 table has no row for. `doc/specs/FUSED_TOPK.md` §8a.

What F1 bought beyond code is five falsifications of the algorithm's own spec, all
recorded in `FUSED_TOPK.md` §3a rather than quietly fixed, and one of them is a
correction to a **correctness contract**: (C5)'s promise that a boolean channel needs
"no special case" is false — a gate past the pivot is not in the contributing set, so
its -INF is never summed and the predicate is silently not applied. Boolean channels are
conjunctive, `channel.h` says so now, and a channel declares `required` on its shuttle
rather than having it inferred from `kind` — because Z9's `<@>` channel is **scored** and
labels itself `WEAVE_CH_FUZZY`, so the inference would have vetoed every row it ranks.
F5's gate is met at 1,140,000 trials and 37,765,994 checks with 11 of 13 mutants killed,
and its positive control measures how silent a loose bound is: a bound 1 % too low
changes the answer in **1.30 %** of trials, one 10 % too low in **13.08 %**. **ALL NINE PHASE-Z TASKS NOW HAVE AN IMPLEMENTATION as of 2026-09-20**, and two of them are
PARTIAL for reasons that are measurements rather than missing code: Z8's `cgram` channel is
2.0-2.4x slower than `pg_trgm` and 1.67x its size (`bench/RESULTS_CGRAM.md`), with a
cgram-bearing bolt not yet mergeable (`G34`) and post-build inserts unaccelerated (`G35`);
Z9's `<@>` bound is sound at 25.4M property checks and prunes 0.0 % of dictionary pages for
the ordinary query shape (`G33`). Both are recorded where a reader meets the feature, per hard
rule 8. **Z4, Z6 and Z7 closed on 2026-09-20** -- Z4 by measuring the prefix-via-trie route and declining it (a 10-term prefix is 1-2 ms through the dictionary range walk; one uncached trie consult is 12 ms; a resident one cannot skip the walk); the regex route's
gate was measured for the record (`bench/RESULTS_FUZZY_REGEX.md`) and the boolean-gate
shuttle exists with its property test. The same run measured Z5's gate: `k=1` meets it,
`k=2` does not (208-293 ms against 200), and that miss is recorded in the Z5 row rather than
re-aimed -- it is fanout, and the lever is Phase F's bounded top-k. **The count did not move on 2026-09-19 although two
days of work landed**, which is the accounting working rather than failing: Z4 part 2
(the resident trie) and Z5's swap of the byte automaton for the character-exact `uleven`
core are both real and both gated on measurements that have not been taken -- Z4 part 3's
prefix-routing number and Z5's 1M-row `k=1`/`k=2` latency gate. What did move is
correctness: `G30` and `G31` in `doc/GAPS.md` are two wrong answers the fuzzy channel
returned before that swap, one of them on pure ASCII input, and both were invisible while
the index and the heap were wrong in the same direction. Z6 (2026-09-20) is the same
shape again: the regex leaf is now answered exactly from the dictionary in tens of
milliseconds where it took ~5 s, the indicative read passes the gate with margin, and the
row stays PARTIAL until `bench/aws/run.sh` says so for the record -- while `G32`, a false
negative the old literal-run extractor produced for `\d`, is closed by deleting it. **Four of the six retrieval kinds are CHANNELS** -- BM25 lexical, quantized-vector ANN
(V8, 2026-09-18), and as of Z7 on 2026-09-20 fuzzy and regex -- in the sense defined above:
a shuttle with a real bound (for a predicate, +/-INF is exactly tight; `include/weave/gate.h`).
The gate shuttle is key-space agnostic and is not yet driven by any query: **as of F1 a fused
scorer exists to drive it, but nothing builds one, because that is F2's planner pushdown and
the waiver keeps F2 closed.** Prefix returns correct rows through the dictionary range walk
and could use the same shuttle the day a route hands it a key set; n-gram (`cgram`, Z8) has
neither a route nor a shuttle.

*Z4 is PARTIAL as of 2026-09-19, and the interesting part is why.* Its stated shape --
route prefix through SuRF instead of the dictionary walk -- is a strict superset of the
work it was meant to replace: a trie hit yields a vocabulary RANK, not a posting
address, so the dictionary range scan still has to run; that scan is already sparse-index
seeked and stops at the first key past the prefix; and `weave_surf_load()` reads the
whole trie image per call with no cache. The half of its gate that asked for `EXPLAIN` to
name the channel is not implementable either -- there is no AM-level EXPLAIN callback on
PostgreSQL 17 or 18. What shipped instead is the thing both halves actually needed:
`weave_channel_stats()`, thirteen backend-local counters naming which MECHANISM served
each query leaf, readable from SQL for every plan shape. Still owed: a resident trie, and
then the measurement that either routes prefix through it or declines the route with a
number.

*And the counters immediately corrected this document.* The first draft asserted that
five of them were structurally zero "because four of the six retrieval kinds answer
nothing" -- and two of those zeros were false. `term*` is served by a dictionary range
walk, `term~k` by a Levenshtein automaton walked over the sorted dictionary
(`weave_fuzzy_terms()`, exact, no recheck), and `/re/` plus any over-long fuzzy term by
the trigram funnel with an exact recheck. **Prefix, fuzzy and regex return correct rows
today and have for some time.** The zeros were an artifact of the counters not existing
yet, read as evidence that nothing ran, which is the precise failure the counters were
added to prevent.

So the sentence below needs its terms defined, and this is the definition the rest of the
document uses: a retrieval kind **answers a query** when it returns correct rows, and it
**is a channel** when it has a shuttle implementing `include/weave/channel.h` with a real
bound, which is what lets it join a fused top-k. By the first measure five of six answer
today (all but n-gram). By the second, two of six are channels -- BM25 lexical and
quantized-vector ANN -- and that is the measure Phase F depends on, so it is the one the
count below tracks. Three of the thirteen counters are structurally zero on the second
measure and those are the zeros now asserted: `prefix_surf`, `fuzzy_surf`, `regex_surf`.

*The count did not move on 2026-09-19, and that is the honest reading.* V7's second
half closed **G23** -- a row inserted after the build kept no vector, so it was present
in lexical answers and absent from vector ones, which is claim 1 being false in
practice rather than a missing optimization -- and closed **G26** with it. Neither is a
task; both are defects in a task already marked done. What the day bought is that the
vector channel now covers rows the index acquired after its build, on both write paths
(pending buffer and oversized insert), at format v9. What it did not buy is a channel:
four of six still answer nothing, and **G29** is the residue -- the vector scan does
not read the pending buffer, so the asymmetry survives in the window between an
`INSERT` and the next flush.

What V8 does *not* deliver is as load-bearing as what it does. It is the first
implementation of the shuttle contract in `include/weave/channel.h`, which found five
defects in that contract -- two before any code was written, three by executing it --
and the most serious was an L2-domain bound reported next to an IP-domain score, which
would have made (C2) *meaningless* rather than violated. It also made the block bound's
case **weaker**: a block's lane strips precede its centroid strip on disk, so the bound
cannot decide anything in a single forward pass, which means it saves the kernel call
and nothing else. And its second gate is **unmet and unattempted** -- "a selective mask
makes the scan measurably faster" needs a corpus, and the only one wired up is 300
synthetic rows whose warp order is nearly spatial by construction. Measuring latency
there would be a benchmark of an unrepresentative case, which hard rule 8 rates as
worse than no benchmark. That measurement is the vector channel's next milestone.

Three costs are stated rather than hidden: the vector half of a merge is not streaming
and holds `O(nvec * codebytes)` resident (~497 MB per million 960-dimensional vectors,
`doc/GAPS.md` G25); a row inserted after the build has no vector in any segment until a
rebuild (G23); and the codes chain has no block→page index, so a block skipped on the
allowlist still costs its page reads, which constrains claim 3 for this channel to
*less scoring* rather than *less I/O* (G27).

The ordering below is forced by three things: hard rule 7 (F waits for L, Z **and** V),
every new on-disk structure owing the adversity gates (7–11) before it counts, and hard
rule 9 — which is why one *measurement* from phase V jumps ahead of both channels. The
page-kind exhaustion that used to force the ordering is closed.

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

### Stage 3 — the V9 recall de-risk — DONE 2026-09-10, and it found something

This was scheduled out of phase order as an afternoon's measurement, on hard rule 9's
logic that a design assumption should be checked before months of work rest on it.
It was worth it. `bench/RESULTS_IVF_RECALL.md`.

**The finding.** Measured at **full probe** — every cluster probed, so probe-miss
error is exactly zero and only quantization error remains — compressed-domain-only
recall@10 tops out at:

| corpus | 2 bits | 3 bits | 4 bits |
|---|---:|---:|---:|
| GloVe 6B, 200-d, 200k vectors | 0.7345 | 0.8515 | 0.9205 |
| TEXMEX GIST-1M, 960-d, 100k vectors | 0.6130 | 0.7880 | 0.8780 |

Full probe is the ceiling over every `nprobe`, so **`recall@10 >= 0.99` is
unreachable in the compressed-domain-only configuration at any probe count, at any
bit width tested, on two real corpora at native dimensionality.** A rerank window of
100 over full-precision vectors closed the gap on both.

**What it refuted, and how.** `include/weave/quantize.h` and
`doc/specs/VECTOR_CHANNEL.md` §2 both claimed the unbiased compressed-domain estimator
"removes the need for a float32 rerank pass at moderate k". §2 went further and said
that if property test P6 ever failed, the claim was wrong and the rerank sidecar
"stops being optional". **P6 passes and the claim was false anyway** — unbiasedness
constrains the *mean* signed error, while recall@10 depends on the *ranking* under
per-vector error, and an unbiased estimator with nonzero variance still permutes a
top-10 list. P6 was a correct test of a property that was never sufficient. All three
places are corrected; §2.1 now derives it.

**What it costs the plan.**

1. **V10 is reshaped and `WEAVE_PK_VRERANK` is WITHDRAWN** (2026-09-13). The rerank
   is required, but it reads full precision from the **heap**, not from a stored
   sidecar. The id stays reserved so it is neither reused nor revived from a stale
   comment.
2. **The recall gate and the storage gate are NOT jointly unsatisfiable — that
   reading rested on an unmeasured denominator.** A full-coverage float32 sidecar
   does cost `4 * dim` = 4,096 B/vector at 1024-d, which is why the sidecar is
   withdrawn; but with the rerank coming from the heap the index is codes only, and
   pgvector HNSW measures **8,056 B/vector** (m=16, ef_construction=64, 999,990 ×
   960-d, `bench/RESULTS_PHASE_V_COLD.md`) rather than the ~5,700 B/vector the
   `0.15×` budget had been priced from. The ratified shape is **4 bits + a top-25
   heap rerank: recall@10 0.9920 at n=1M, 0.064× HNSW.** Both gates met on one
   corpus. The three-resolution framing is resolved and closed.
3. **TQ+ affine calibration is not the fix.** It moved recall the wrong way at 3 of 4
   measured points.
4. `doc/ARCHITECTURE.md` §8 states recall and storage as measured and **latency as
   unmeasured**: V7/V8 do not exist, so no pg_weave vector query has been timed end
   to end. The code scan — 512 MB of codes at 1M × 4 bits — is now the deciding half.
5. **The gate itself was restated** (2026-09-13), because `p50 ≤ 2× pgvector HNSW at
   recall@10 ≥ 0.99` named an operating point the comparator does not have: pgvector
   HNSW tops out at **0.9760** on this corpus. Latency is now compared iso-recall at
   `R* = min(0.99, the comparator's best)`, warm and cold both, with the comparator
   tuned by an `m` × `ef_construction` sweep and every prewarm verified.

**What it does not show.** The width sweep is 200k × 200-d and 100k × 960-d; the
window, the HNSW baseline and the cold latencies are **1M × 960-d**, still not the
gate's second corpus at 1024-d. k=10 cosine only. The quantization ceiling is the
robust half — taken at full probe, so no partition quality can raise it. The
probe-miss half of the same run is *pessimistically* biased by a deliberately crude
harness k-means and must not be quoted as a ceiling.

Three specific limits on the 2026-09-12 EC2 run, all recorded in
`bench/RESULTS_PHASE_V_COLD.md`: the HNSW recall ceiling of 0.9760 is for **m=16,
ef_construction=64** and modest `m` is a known weakness at 960-d, so the sweep that
would confirm or move it has not been run; every **warm** arm after the first loop
iteration is invalid because `pg_prewarm` was never installed; and all
reads-per-candidate figures from that run are invalid because planner catalog reads
were summed into the execution count. The cold p50s and the recall table are the
parts that stand.

Compare the cost of learning this now against the counterfactual: V7, V8 and V9 built
on disk, all correct, and then a recall gate that no configuration reaches. That is
the second time hard rule 9 has paid for itself, after
`bench/RESULTS_BOUND_PRUNING.md`.

### Stage 4 — Z: fuzzy, approximate regex, prefix routing, n-gram (months)

Four of the six named capabilities, and the cheaper of the two remaining channels:
the code is imported and compiling, TRE is current at `f864ed0`, and phase X already
landed the kind space and per-bolt descriptors that Z3's new page kind needs.

- Z3 (SuRF trie over the **bolt vocabulary**, not the corpus — that substitution is
  the whole reason this channel can exist at a reasonable size) → Z4/Z5/Z6 routing →
  Z7 shuttle.
- **Z7 owes the other half of pg_tre `4a9c86c`**: the prefilter must refuse to reject
  when `always_true` is set, with a case-insensitive-anchored-pattern regression test.
  The extraction half is ported.
- **Z8 (`cgram`) is now required, not opt-in**, because `n-gram` is a named product
  capability. Its cost ships with it and `bench/RESULTS_CGRAM.md` must state it as
  plainly as any win: with `cgram` on, pg_weave is not smaller than `pg_trgm`.
- Z9 (`<@>` edit-distance KNN) needs a real bound, which means a bound property test
  (gate 3), not a plausible-looking heuristic.
- Z4 must **not regress prefix**, which already works and already beats tsvector+GIN
  by 3.8–7.7×. A re-route that loses that is not an improvement.

**Exit gate:** bound property test (gate 3), fuzz target (gate 8), crash +
replication TAP (gate 7), concurrency test matching `t/005`'s 58,049 reads (gate 11)
— for the new weft specifically. Plus the Phase Z gate in `doc/PHASES.md`: beat
pg_tre on every row of its own perf table, and stay within 3× of `pg_trgm`'s index
size with `cgram` off.

### Stage 5 — V: the vector channel on disk (many months)

The long pole. V1–V5 are done — quantizer, `wvec` type, 32-lane packing — at
17,741 + 1,909,440 property checks. V6 is **partial**: the scalar scoring oracle
exists and three paths (`scalar`, `lut-wide`, `lut-avx2`) are verified `memcmp`-
identical to it over 308,278 differential checks, dispatch resolved once at
`_PG_init`. Everything that touches disk remains.

- V7 `WEAVE_PK_VCODES`/`WEAVE_PK_VMETA` pages (kind ids reserved by X1 — read them
  with `WeavePageHasKind()`, never a bitwise AND, and **zero the block buffer before
  packing** or the page image is nondeterministic); V8 code-scan shuttle; **V9 IVF**,
  not Vamana, and now informed by Stage 3's measurement rather than assuming it.
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

**Exit gate:** the four adversity gates (7, 8, 9, 11) for the new weft, plus a
matched-recall comparison against pgvector HNSW — recall held equal *and demonstrated
reachable by both*, then latency and size compared.

### Stage 6 — F, the actual thesis (months)

Only after L, Z and V gate (hard rule 7, restored to its original form on 2026-09-10
after a same-day amendment was reverted). F1–F4, and F5's property test: fused top-k
identical to brute force over 10⁶ generated cases. Until F5 passes, the central claim
of the project is a document.

With six retrieval kinds rather than two, F is worth *more* and costs the same: the
loop, pivot selection, the essential/non-essential partition and F5's property test
are all channel-count agnostic, while the value of fusing rises with the number of
channels a query can combine.

**Where this stage actually stands, 2026-09-22.** F1, F2, F5, F6, F7 and F8 are done, F4 is
withdrawn, F3 and F9 remain, and F5's property gate is met — so the central claim is code
rather than a document. The §8 gate is **2 of 5** (**corrected 2026-09-22 night**, it read
3 of 5 for one day): recall and — since the per-key normalizer — nDCG pass; **p50**, **p99**
and the **`score()`-call ratio** fail, p99 having passed before the normalizer and failed
after it, and fiqa's fused p50 now being **1.172× the RRF control**. The remaining work in
this stage is therefore not the scorer's structure but **vector candidate reduction** — Phase
V's V13/V14/V15/V18, already measured at 0.00 % block pruning
(`bench/RESULTS_BOUND_PRUNING.md`) — which is now the single blocker for **three** gate rows
rather than one; the EC2 re-run that used to be owed here has happened
(`bench/RESULTS_FUSE.md`, fourth measurement) and is what moved the p99 row. **Open and
awaiting the maintainer: whether `pg_weave.fuse_normalize` stays on by default**, since on it
fails the latency rows and off it fails the ranking one.

### Stage 7 — M, P, R: shippable (months)

**M is where the product definition gets cashed.** "A singular text index replacing
pgvector + a BM25 index + pg_trgm" is a compatibility claim before it is a
performance claim, so M1/M2 (pgvector surface and coexistence), M3
(`tsvector`/`tsquery` casts and a `@@`-compatible operator) and **M4 (pg_trgm
surface: `%`, `similarity()`, `word_similarity()` over `cgram`)** are all 1.0
requirements rather than polish. M4 returns to the 1.0 path with Z8. M5's in-place
index swap and M6's `weave_check()` health reporting stay in.

Then **P4 cost-model calibration** — with six channels and `amcanorderbyop`, an
uncalibrated cost model does not merely mis-cost one path, it picks the wrong
*channel* — P3 the reproducible competitive matrix including losses, then R1–R5:
DocBook docs, examples, PGXN, managed-service readiness, contrib submission.

### Cross-cutting, continuous — not a stage

ASan/UBSan on the full suite (gate 10), a fuzz target per on-disk structure
(gate 8), `weave_check()` covering all ~20 `SEGMENT_FORMAT.md` §9 invariants
(gate 4 — `weave_check()` now exists and covers 11; before phase X there was only
`weave_check_meta()`), and documented resource behaviour (gate 14 — pg_tre shipped
without it and a user hit a temp-disk wall the docs did not predict). **CI: corrected 2026-09-10.** This used to say "a Codeberg CI runner is still not
registered, so `.forgejo/workflows/ci.yml` has never run", and it gated everything
above. Half of that is true and the wrong half mattered: the Forgejo runner does not
exist, but **Codeberg push-mirrors to `github.com/gburd/pg_weave` where Actions is
enabled**, so gates have been running since 2026-09-08 and nobody was reading them.
What they showed: `standalone` green, `sanitize` failing on a workflow bug, and the
`test` matrix **queued for 21+ hours** across four pushes because it asked for a
runner label GitHub does not have. A queued job is worse than a failing one, because
the absence of a signal reads exactly like success.

`.github/workflows/ci.yml` is therefore **the gate that is watched**, and it now also
runs the full TAP suite (the Forgejo container job can only manage t/003 and t/004).
The Forgejo file is kept in sync for the day a runner is registered. Registering one
is still worth doing, but it no longer gates anything.

## A lexical-only 1.0 is a worse idea than it was this morning

The staged plan above remains 18–30 months. Dropping or adding Z does not change
that, because Z was never the long pole. V is.

pg_weave's lexical channel is **already better than the alternatives on four
measured axes** — index size (626 MB vs 873/1120), build time (at GIN parity),
`count(*)` (~200×), keyless `ORDER BY` (index path where GIN seq-scans) — and near
parity on ranked latency, winning outright at k=100. That is a shippable thing,
reachable in weeks rather than years.

But with the product defined as **six retrieval kinds in one index**, a lexical-only
release ships one and a half of them (BM25, plus prefix), and it competes on a
channel we inherited rather than on the thing the architecture argues for. It is a
good **0.x** and a bad **1.0**. Version numbers are cheap; the fused thesis is not.

The genuinely useful intermediate is different: **L + Z is four of the six** (BM25,
fuzzy, approximate regex, prefix, and with Z8 n-gram makes five), needs no new
codec, and would already be a single index replacing `pg_fts` + `pg_trgm` + `pg_tre`.
That is a defensible 0.x milestone with a real claim, and unlike a lexical-only
release it is on the way to 1.0 rather than beside it.

This is a maintainer decision, not a technical one, and it should be made on
purpose.

## What to do instead, today

| need | use |
|---|---|
| BM25 text search in Postgres | **pg_fts** (same code, real release history) or Timescale's pg_textsearch |
| Vector search in Postgres | **pgvector** (HNSW), or VectorChord |
| Fuzzy / substring / regex | **pg_trgm**; add **pg_tre** only for k≥1 edit distance at modest scale |
| Hybrid at scale, managed, willing to leave Postgres | **turbopuffer** or ParadeDB — see `doc/COMPETITIVE.md` |
| Storage-optimal exact vector recall where an O(n) scan fits | pg_turbovec |

## How this document gets updated

Every entry above moves from blocking to done **only** when its gate command
passes and the output is recorded. Not when the code is written. Not when it looks
right. If you are reading this and the gate list is unchanged but the README has
grown confident, the README is wrong.
