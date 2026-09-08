# Build-out phases

This is the execution contract. Every task has an id, a spec, an owner file, and
a **gate** — a mechanically checkable condition that must hold before the task is
considered done. A task without a passing gate is not done, regardless of how
much code exists.

Read `doc/CONVENTIONS.md` before writing code and `doc/TESTING.md` before
claiming a gate.

## Ordering principle

Channels land **one at a time, fully**, in the order of decreasing certainty:
the lexical channel is already field-tested, the fuzzy channel is imported code
needing wiring, the vector channel is new code implementing a published
algorithm, and the fused scorer is the only genuinely novel piece. Building the
novel piece last means it is built against three working channels instead of
three simultaneous unknowns.

Corollary: **do not start F1 before L, Z, and V are green.** The temptation will
be strong because F is the interesting part. Resist it. A fused scorer debugged
against a half-working vector channel will consume more time than both.

```
  phase 0  DONE   repository, build, lexical channel forked, all tests green
  phase L         lexical channel: pay down the inherited debt
  phase Z         fuzzy/regex/prefix channel: wire the pg_tre import
  phase V         vector channel: new C code
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
| L1 | Split the `src/am/am.c` unity build into `am.c` / `amscan.c` / `ambuild.c` / `amvacuum.c` with real prototypes in `include/weave/am.h` | — | `make check-unity` deleted; each TU compiles standalone; zero new warnings; regression byte-identical |
| L2 | **Common-term ranked latency.** 74.7 ms at 2.19M docs vs pg_search's 2.27 ms. Root cause is decode-bound posting scan on high-df terms. Implement impact-ordered posting blocks (sort within block by descending tf) so block-max WAND can terminate on the first block. | `doc/specs/IMPACT_ORDERING.md` (write it) | common-term (df > 5% of corpus) ranked k=10 p50 ≤ 8 ms on the 2.19M Wikipedia corpus; rare/mid must not regress by >10% |
| L3 | **Doclen sidecar per-scan tax.** The v4 sidecar buys 4.7× index size (1421 MB vs 6729 MB) but decodes the whole segment sidecar per scan, a fixed ~18 ms. Replace with a page-directory random-access cursor; the 1.5.8 fork has a partial version. | `doc/specs/DOCLEN_SIDECAR.md` (write it) | rare-term ranked k=10 p50 ≤ 2.5 ms **with** `doclen_sidecar=on`; index size within 10% of the sidecar-on size today |
| L4 | **Parallel merge.** Build is 1091 s vs vchord's 57 s, dominated by single-threaded merge + vacuum compaction. | — | full build+merge+vacuum of the 2.19M corpus ≤ 300 s on a 16-vCPU host |
| L5 | Positions default review. Phrase is 8500 ms with positions unbuilt vs pg_search's 24.84 ms. Decide and document whether `positions=on` becomes the default; measure the size cost on the same corpus. | — | a recorded decision in `bench/RESULTS_POSITIONS.md` with both numbers, and the default set accordingly |
| L6 | Storage AIO: use `read_stream` for posting-page prefetch. Pointer-chained pages currently defeat readahead. | — | measurable p50 improvement on cold-cache common-term scan; no regression warm |
| ~~**L7**~~ | **DONE 2026-09-06.** **Keyless ordering scan.** `ORDER BY d <=> q LIMIT k` with no `WHERE` must generate an index path. Today it silently falls back to Seq Scan + top-N Sort: measured 83 ms par4 / 362 ms serial vs **0.05 ms** for the supported form — a 7,000× cliff on the first query any user writes, because pgvector taught them that shape. `bench/RESULTS_LEXICAL.md` §Footgun, `doc/GAPS.md` G1. | `sql/orderby.sql` | **MET:** bare form produces `Index Scan ... Order By`; p50 **0.05 ms == the qualified form**; `@@@`-parity, boolean-structure, score-equality, and LIMIT-overrun assertions all hold |
| ~~**L8**~~ | **DONE 2026-09-07.** **Deterministic index size.** A fresh `CREATE INDEX` measures 156 MB and the same index after `weave_merge` + `weave_vacuum` measures 115 MB — a 35% swing on whether an optional maintenance step ran. `doc/GAPS.md` G6. | — | **MET:** as-built 46 MB == compacted 46 MB, swing 0.0%; `weave_merge`/`weave_vacuum` both return false on a fresh build; `bench/lexical.sh` now aborts if either fails |
| **L9** | **Attribute the fixed per-scan cost.** Rare-term ranked is 0.05 ms vs GIN's 0.03 ms and mid-term is 3.54 vs 2.06 — a 70× latency ratio across a 100× document ratio, consistent with a small fixed setup cost plus linear per-document work. Profile with `perf` on EC2 and attribute it **before** changing code. `doc/GAPS.md` G3/G4. | — | a recorded profile in `bench/RESULTS_SCAN_PROFILE.md` naming where the fixed cost goes |
| ~~**L10**~~ | **DONE 2026-09-07.** **`weave_index_size_detail()`** — bytes per structure (dictionary, block index, postings, positions, doclen sidecar, livedocs, trigram) so the 1.7× size gap against GIN is attributed rather than guessed. `doc/GAPS.md` G2. | — | **MET:** function exists and sums to `pg_relation_size`; breakdown recorded. It also found that 70.7% of a fresh index was freed pages, which closed G2 as a *win* |
| **L11** | **Parallel scan** (`amcanparallel`). GIN's common-term advantage halves when parallelism is removed, i.e. GIN currently benefits from parallelism pg_weave cannot use. `doc/GAPS.md` G11. | — | parallel ranked scan correct under `t/005`-style concurrency; measurable p50 improvement on common-term |
| **L12** | **Low-bias end-of-build merge.** L8's vacate+pack pass roughly doubles build write I/O, taking build from 12.6 s to 29.2 s against GIN's 11.7 s. Make the end-of-build merge allocate output pages from the low free region so the result is front-packed and a plain tail truncation suffices, removing the relocation pass. Viable because at end of build the freed inputs are ~70% of the file and the live output ~30%, so the low free region exceeds the live segment; `weave_vacuum_compact`'s two-phase relocation exists for the harder general case where it does not. `doc/GAPS.md` G5. | — | build ≤ 15 s on the 1M corpus with as-built size still 46 MB |
| **L13** | **WAND initial-k.** DONE 2026-09-07: made `pg_weave.wand_initial_k` a GUC, swept 4..200 against `LIMIT` 10 and 100 across three df bands, set the default from the frontier (32, was 100). `LIMIT 10` 2.3× faster on rare; `LIMIT 100` 1.67–1.96× slower. `bench/RESULTS_WAND_K.md`. | `bench/RESULTS_WAND_K.md` | **MET** |
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

## Phase Z — fuzzy / regex / prefix channel

The code is imported (`doc/specs/IMPORT_pg_tre.md`). The work is wiring it to the
**vocabulary** funnel rather than pg_tre's corpus-level trigram index — that
substitution is the whole reason this channel can exist at a reasonable size.
See `doc/ARCHITECTURE.md` §7 and `doc/specs/FUZZY_CHANNEL.md`.

| id | task | gate |
|---|---|---|
| Z1 | Vendor TRE (laurikari/tre, BSD-2) as a submodule or in-tree copy; record the pinned commit and license in `doc/LICENSING.md`; get `src/query/re_match.c` compiling | `make` clean with the fuzzy sources in OBJS |
| Z2 | Wire the missing GUCs and deadline helpers listed in `doc/specs/IMPORT_pg_tre.md` "Wiring TODO" | all imported TUs compile; GUCs visible in `pg_settings` |
| Z3 | Build the SuRF trie over the **bolt vocabulary** (dictionary terms), not the corpus. New page kind `WEAVE_SURF`. | `weave_check()` validates the trie; a property test asserts trie membership == dictionary membership |
| Z4 | Route prefix (`term*`) through SuRF instead of the current dictionary walk | prefix p50 ≤ today's, and `EXPLAIN` shows the surf channel |
| Z5 | Route fuzzy (`term~k`) through universal-Levenshtein neighbourhood expansion over the vocabulary trigram map | index-accelerated `k=1` and `k=2` on a 1M-row corpus, p50 ≤ 200 ms (pg_tre measured 5.5 s and 7.3 s) |
| Z6 | Route regex (`/re/`) through regex AST → trigram tiling → vocabulary candidates | character-class regex `E-[0-9]{4}` p50 ≤ 100 ms (pg_tre measured 1.5 s) |
| Z7 | Implement the fuzzy/regex shuttle per `include/weave/channel.h` (C5 boolean gate) | property test in `test/hegel/test_bounds.c` covers it |
| Z8 | **Opt-in** corpus-level character-trigram channel `cgram` for unanchored cross-token substring (`LIKE '%tion refu%'`) | correctness parity with `pg_trgm` on a randomized pattern suite; size honestly recorded in `bench/RESULTS_CGRAM.md` |
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
| V1 | `wvec` type: I/O, typmod, casts, `<->` `<#>` `<=>` `<+>`, arithmetic, btree opclass, codec introspection | **DONE** — `sql/wvec.sql`, 4th regression test, green on PG 17 and 18. Found and fixed a silently-accepted trailing comma in the parser. |
| V2 | Deterministic rotation: ChaCha8-seeded global permutation + sign flips + per-block normalized Walsh-Hadamard, K=2 rounds | **bit-identical output across x86-64 and aarch64 and across thread counts.** This is a hard gate; a fixture hash committed in `test/hegel/rotation_fixture.h` must match on both arches in CI |
| V3 | Lloyd–Max codebook for Beta((d-1)/2,(d-1)/2), memoized by `(bits, dim)` | codebook values match a committed fixture to 1e-9; solve time ≤ 100 ms |
| V4 | Encode: normalize, rotate, optional TQ+ affine calibration, quantize, bit-pack, store per-vector renormalization scale | round-trip property test; the compressed-domain inner-product estimator is unbiased within a stated tolerance |
| V5 | 32-lane packing layout; x86 `perm0`-interleaved, ARM sequential, plus the vector-major layout for int8-dot kernels | `test/hegel/test_pack.c`: pack/unpack round-trip, `move_lane`/`zero_lane` O(1) swap-remove |
| V6 | SIMD kernels with runtime dispatch: scalar, SSE2, AVX2, AVX-512BW, AVX-512 VNNI, NEON, NEON SDOT. Nibble-split byte-LUT **and** int8-dot strategies. | every ISA path produces results identical to the scalar path on a randomized suite (`test/hegel/test_kernels.c`); CI runs the AVX-512 path under an emulator or an appropriate runner |
| V7 | New page kinds `WEAVE_VCODES`, `WEAVE_VMETA`; codes live in the bolt under GenericXLog | crash-recovery TAP test extended to a vector index |
| V8 | Code-scan shuttle: `score_block` over 32 lanes, `allow`-mask block short-circuit, and the block bound from `doc/specs/FUSED_TOPK.md` §2 | (C1)+(C2) property test; a selective mask makes the scan measurably *faster* |
| V9 | **IVF coarse quantizer** over the quantized codes: k-means over a sample, per-cluster centroids, `nprobe` probing, cluster-aligned code blocks (which also satisfies V13). **NOT a proximity graph** — withdrawn after pg_turbovec deprecated its graph kind in v2.5.0, having measured that at R@10 ≥ 0.98 on GIST-10M/960-d IVF reached 28.4 ms while the graph could not reach 0.98 at **any** latency (ceiling 0.873 at 181 ms) and built 57–90× slower. `doc/specs/VECTOR_CHANNEL.md` §8a. | recall@10 ≥ 0.99 on 1M × 1024-d; p50 within 2× of pgvector HNSW; storage ≤ 0.15× pgvector HNSW; **plus a recall floor in the gate for any partitioned build** — pg_turbovec's shard/thread coupling cost R@10 0.920 → 0.605 while a build-time-only test called it a 60× speedup |
| V10 | `recall=exact` path: graph off, full code scan, optional full-precision rerank sidecar | recall@10 == 1.000 |
| V11 | Journal-checksum-style incremental commit for the vector wefts (alternating header slots, delta digest) — adapted to PostgreSQL's WAL rather than replacing it | torn-write injection TAP test detects and recovers |
| V12 | ColBERT-style multivector late interaction as a distinct channel kind | MaxSim correctness against a reference implementation |
| V13 | **Warp ordering by cluster.** Assign warp positions in the order the IVF build's k-means clustering produces (§8a of doc/specs/VECTOR_CHANNEL.md; IVF satisfies this requirement inherently, where the withdrawn graph plan needed it as a separate constraint), so each 32-lane code block is spatially coherent. Not an optimization: `bench/RESULTS_BOUND_PRUNING.md` measures the block bound pruning 99.6% of blocks with a coherent warp and **0.0%** with a random one. | `bench/bound_pruning.c` reports ≥ 90% blocks pruned at k=10 on the shipped corpora; a heap-order build is rejected by the gate |
| V14 | Per-block centroid (stored as a quantized code) + radius in `WeaveVecBlockHdr`, maintained across insert, vacuum lane-zero, and merge | `weave_check()` recomputes both and compares; a randomized soundness run of `bench/bound_pruning.c` finds no (C2) violation |

**Status:** V1 done. V2–V5 have working scalar implementations in
`src/vector/quantize.c` and `src/vector/pack.c` with 17,741 property checks
passing, but no on-disk format and no committed cross-architecture fixture, so
they are not gate-complete. V6–V14 not started.

**Phase V gate:** on 1M × 1024-d Cohere-wiki, all three of
`recall@10 ≥ 0.99`, `p50 ≤ 2× pgvector HNSW`, `size ≤ 0.15× pgvector HNSW`
simultaneously. Recorded in `bench/RESULTS_VECTOR.md`. Note this is the gate
pg_turbovec **failed** (490× slower); the difference is V9. If V9 does not land,
the vector channel is storage-optimal and latency-poor and the README must say
exactly that.

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
| P1 | `bench/` harness: corpus loaders (Wikipedia 20231101.en, MS MARCO, Cohere-wiki, BEIR subset), latency driver, nDCG scorer | one command reproduces every recorded result |
| P2 | EC2 harness (`bench/aws/`) using the `bene` profile: launch, tune, measure, terminate | `bench/aws/run.sh` produces a `RESULTS_*.md` and terminates the instance even on failure |
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
