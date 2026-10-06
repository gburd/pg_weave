# Result: what the three sibling projects learned since the fork, and what pg_weave took from them (2026-10-06)

pg_weave forked its lexical engine from **pg_fts 1.5.8**, imported its fuzzy/regex code from
**pg_tre**, and reimplemented its vector codec from **turbovec**, the library behind
**pg_turbovec**. All four have kept moving. This file is the review of everything since the
fork that could matter here. Each item says what was found, whether pg_weave has the same
defect or opportunity, the evidence, and what was done.

Hard rule 6 still applies. pg_turbovec is Apache-2.0, so ideas only. turbovec is MIT and
pg_tre MIT, same author, relicensed. pg_fts is PostgreSQL-licensed.

## Summary

| source | finding | pg_weave had it? | done |
|---|---|---|---|
| pg_fts 1.9.0 | IDF goes **negative** after deletes; WAND returns the worst documents | **yes**, reproduced on main | **fixed, G83** |
| pg_fts 1.8.4 | a corrupt tombstone sparsemap opens as **empty**, silently resurrecting every deleted row | **yes**, four unguarded sites | **fixed, G85** |
| pg_tre 4.2.0 | `pg_mb2wchar_with_len` overruns a single `pg_wchar` (stack overwrite, backend abort) | **yes**, latent (not exposed in SQL) | **fixed, G84** |
| pg_fts 1.9.0 | WAND seek skipped a term's **last** posting block | already fixed here as **G43**, before pg_fts | nothing |
| pg_fts 1.8.3 | bulk-ingest growth: the merge allocated extend-only; a mutex-less merger deadlocked | already here (`weave_assert_merge_serialized`, snapshot allocation) | nothing |
| sparsemap 5.8.1 | the two defects pg_weave reported upstream (G82) | vendored 5.8.0 | **re-vendored 5.8.1** |
| turbovec 1.1.0 | **staged 4-bit "planes" search**, the bulk of pg_turbovec 2.11.0's win | different codec | **measured: does not transfer as-is** (below) |
| pg_turbovec 2.11.0 | after the kernel got 3-6.5x faster, the **per-candidate heap recheck** is the bottleneck | yes, V10's heap rerank | recorded as the lever (below) |
| pg_fts 1.9.x | LIMIT passed to the ordering scan; dense exhaustive scoring for high-df single terms; lazy phrase gate; galloping phrase intersection | partly | recorded as candidates, not built (below) |

## 1. Correctness fixes taken (all three on main, merged `c9ba491`)

### G83: negative IDF after deletes (pg_fts 1.9.0)

`N` is the live corpus (tombstones subtracted) and `df` is the dictionary df (tombstoned
postings still counted until a merge). For a term in nearly every document, deleting a
fraction makes `df > N`, the unclamped Lucene IDF goes negative, every contribution flips
sign, and block-max WAND's bounds (C2) prune the best documents. Reproduced on pg_weave
main with pg_fts's shape: 6,000 rows with `com` in every one, 1 in 7 deleted.
`ORDER BY d <=> 'com' LIMIT 3` returned the heap's worst distance, and `weave_search`
scored −0.1337. Fixed with one clamped `weave_index_idf()` in `include/weave/bm25bound.h`,
used at all four index-side sites. Pinned by `sql/idf_deletes.sql`. **Positive control:**
three of its checks read `f` on the pre-fix build and `t` with the fix.

### G85: corrupt sparsemap opens empty (pg_fts 1.8.4)

Since sparsemap 5.6.0, `sm_open()` replaces a buffer that fails validation with an empty
map. For tombstones, empty means "nothing deleted". `weave_sm_open_checked()` refuses a
blob that does not reopen at its stored length. **Positive control
(`pgweave-20261006-050829-9795`):** with the guard compiled out (distinct `.so` md5), the
`t/003` scan over a corrupted tombstone blob returned **4000**, the resurrected answer.
With the guard it errors with `corrupt tombstone bitmap`.

### G84: `pg_wchar` overrun (pg_tre 4.2.0)

Latent here, because the similarity functions are not exposed in SQL yet (M4). Fixed the
same way pg_tre did.

### sparsemap 5.8.1

Upstream fixed both defects pg_weave reported on 2026-10-05 (`9e72ee4`), and pg_weave has
re-vendored it. Both reproducers in the upstream report now pass, and the wire format is
verified against four prior releases.

## 2. turbovec's staged search: where pg_turbovec 2.11.0's win came from, and why it does not transfer as-is

### What it is

turbovec 1.1.0 (`6c40f46`, hill-climb log `benchmarks/hillclimb/LOG_search.md`, round 2)
made 4-bit search **1.87x** faster over 32 cells: arm/x86, 1 or 8 threads, batch or single
query, k = 10–100. Its first hypothesis, H1, delivered **x1.72** of that. A search:

1. **scans only the sign bit** of every coordinate, a quarter of the code bytes, for a
   shortlist of `max(256, 20k)`;
2. ranks the shortlist on the sign plus the top low plane, keeping `max(96, 6k)`;
3. ranks those on all three low planes with a linear level model
   (`alpha*sgn + sum beta_j*rho_j`, fitted by weighted least squares), keeping
   `max(32, 1.5k)`;
4. rescores those exactly with the kernel's own integer dot product, so returned scores
   are bit-identical.

The ids are approximate. 99.92–100% of queries return the whole-index scan's ids on
OpenAI-1536/3072 and mpnet-768.

pg_turbovec adopted it as 2.11.0. End to end it is **1.14–1.17x** at 4 bits, against a
**3–6.5x** kernel speed-up. The rest of a query is about 4.5 ms fixed plus **~48 µs per
candidate** of heap fetch and exact recheck (pg_turbovec
`benches/results/tv111_arm_20261005/FINDINGS.md` §2): "for flat-index queries the scan is
no longer the bottleneck; the recheck is."

### Why it rests on one number, and that number was measured here first (hard rule 9)

The whole design rests on how short the sign-stage shortlist can be. turbovec's own probe
(P1) measured it: **about 104 at k=10 out of 100K on OpenAI-1536, 180 on mpnet-768** for
99.9% of queries. pg_weave's codec is different (its own rotation, Lloyd–Max codebook and
per-lane scale), so `bench/plane_probe.c` re-asks P1 on our own 4-bit codes. GIST-1M at
960-d and at a 384-d prefix, 1,000 held-out queries, no index involved. The control arm
scores with the exact levels and must need exactly k; it does at k = 1, 10 and 100 on a
synthetic check.

**Shortlist needed for the estimate's top-L to hold the exact 4-bit top-k**, n = 200k
(`pgweave-20261006-052912-fab3`):

| first stage | bytes read | dim | k=10 p50 | p99 | **p99.9** | k=100 p99.9 |
|---|---|---|---:|---:|---:|---:|
| sign bit | 1/4 | 960 | 248 | 3,018 | **3,997** | 16,437 |
| sign bit | 1/4 | 384 | 1,497 | 13,912 | **20,389** | 59,210 |
| top 2 bits | 1/2 | 960 | 362 | 4,344 | 8,480 | 22,865 |
| top 2 bits | 1/2 | 384 | 2,094 | 24,603 | 36,257 | 71,983 |
| all 4, linear model | 1 | 960 | 20 | 74 | **97** | 864 |
| all 4, linear model | 1 | 384 | 33 | 239 | **427** | 2,210 |

**n = 1M, the second scale** (hard rule 11). 384-d is from the same run
(`pgweave-20261006-052912-fab3`). The 960-d arm was lost to a host reboot and re-run
(`pgweave-20261006-130721-1dd7`) with the probe parallelized over queries. Its output is
byte-identical to the serial build at 1, 3 and 8 threads, and the re-run's 960-d / 200k arm
reproduced the original serial run exactly. The control arm needs exactly k in every row:

| first stage | dim | k=10 p50 | p99 | **p99.9** | as % of n | k=100 p99.9 |
|---|---|---:|---:|---:|---:|---:|
| sign bit | 960 | 481 | 8,069 | **19,823** | 2.0 % | 48,471 |
| sign bit | 384 | 4,008 | 51,442 | **83,351** | 8.3 % | 206,262 |
| top 2 bits | 960 | 874 | 16,812 | 43,381 | 4.3 % | 89,535 |
| top 2 bits | 384 | 6,041 | 88,301 | 110,311 | 11.0 % | 247,346 |
| all 4, linear | 960 | 23 | 129 | **214** | 0.02 % | 1,336 |
| all 4, linear | 384 | 43 | 378 | **723** | 0.07 % | 3,630 |

**It reproduces as a fraction of the corpus, not as a count.** The sign stage needs 2.0 %
of the corpus at 960-d at both scales (3,997 / 200k, 19,823 / 1M) and 8–10 % at 384-d.
A shortlist that grows linearly with n is a full scan of a fixed fraction, which is the
opposite of what a first stage is for. turbovec's 104 at k=10 out of 100K is 0.1 %, which
is 20x (960-d) to 80x (384-d) below ours.

### Reading

- **The sign stage does not discriminate on our codes.** At 960-d it needs about 4,000
  candidates per query for 99.9% recall of the exact top-10, against turbovec's 104 on
  OpenAI-1536. That is **38x** larger, 2% of a 200k corpus. At 384-d it needs 20,389, 10%
  of the corpus, where turbovec needed 180 on mpnet-768. A first stage that must pass a
  tenth of the corpus to the next stage saves almost nothing.
- **The top-2-bit stage is WORSE than the sign stage**, at both dims and every k. The
  two-feature linear fit on (sign, top low bit) is a poor model of a 16-level Lloyd–Max
  codebook (the four-feature model's worst-level error is 17% of the outermost level).
  So the intermediate pass turbovec relies on is not just weak here, it is
  counterproductive.
- **The full linear model is good:** 97 at 960-d and 427 at 384-d for k=10. But it reads
  all four bits, so it is a cheaper *arithmetic* on the same bytes, not a cheaper *read*.
  That is V16's byte-LUT kernel's territory, which already exists.
- **Why the difference, a hypothesis not yet tested:** turbovec's sign plane is the sign
  of a coordinate after its rotation on real embeddings with strong per-coordinate
  structure. GIST is a 960-d image descriptor, rotated by pg_weave's Hadamard rotation.
  The codebook shapes differ too: ours puts 8 of 16 levels within ±0.033 of zero at 384-d,
  so the sign of a near-zero coordinate carries almost no information. It would also
  depend on corpus. turbovec's own README says the shortlist "cannot separate what has no
  structure" on isotropic random vectors, where only 4–7% of queries match.

### What this decides for V18

**V18 (bit-plane progressive refinement) is NOT promoted.** Measured at two scales (200k and 1M) and two dimensionalities, on one corpus (GIST); the result reproduces as a fraction of n. Still owed, and the only thing that could reopen it: a real text-embedding corpus. Its gate, "the
survival fraction measured before anything is built", is now measured, and it fails at
both dimensionalities on GIST. Two things would reopen it, both owed and cheap:
- the same probe on a **real text-embedding corpus** (MiniLM or OpenAI), which is
  turbovec's regime and pg_weave's product; GIST may simply be the wrong corpus;
- a **sign-stage estimate weighted per coordinate by |q_j|**, which turbovec's seeded
  collector effectively does and this probe does not.

### What does transfer, now

**The bottleneck after a faster kernel is the recheck.** pg_turbovec measured about 48 µs
per candidate for the heap fetch, vector deserialize and exact recheck. pg_weave's V10
heap rerank of a top-25 window is the same shape: about 86 ms cold for 20 candidates in
`RESULTS_PHASE_V_COLD.md`, 0.598 ms warm. So the lever for pg_weave's §8 vector latency
row is **a smaller rerank window at equal recall**, or a cheaper rerank, not a faster code
scan. That is consistent with what V15 found: its gain shrank to 1.06x once recall
needed the bigger window.

## 3. pg_fts 1.9.x lexical performance: recorded as candidates, not built

Each needs its own measurement on pg_weave's tree before it is built. pg_weave's lexical
path has diverged (fused scorer, G27 page index, normalizer), so none of these numbers
transfer:

| pg_fts change | what it did there | pg_weave status |
|---|---|---|
| (A) LIMIT passed to the ordering scan; exact-k WAND when the heap is all-visible | common k10 45→31 ms, OR3 13.3→5.7 ms | **Measured here, applies: G87. FIXED for the lexical route.** LIMIT 10 work halves (3,613 -> 1,799 on a 3-term OR); latency on EC2 drops 2.0x / 1.4x for a single term at 200k / 1M, and only 0.6-3 % on ORs, whose cost on this corpus is page decoding. Fused and vector routes owed. |
| `fts_current_distance()`: reuse the scan's distance instead of recomputing `<=>` per returned row | 28% of rare-term CPU | **Measured here, applies: G86.** LIMIT 400 calls `weave_distance` 450 times, and a two-channel `fuse()` 800 times plus 800 `weave_lexscore`, each a detoast and a fresh BM25. |
| (C2) dense exhaustive scoring for a high-df single term | exact top-k with no WAND overhead | **Candidate**, gated on df |
| lazy phrase gate for ranked phrase queries | "united states" 110.6→35.4 ms on 2.2M Wikipedia | pg_weave builds the phrase match set first. **Candidate** |
| galloping forward probe in the positional phrase intersection | 183→146 ms | pg_weave gallops in `tidset_and`, not in `weave_phrase_eval_seg`. **Candidate** |

## Reproduce

```sh
# the plane probe (no index; a corpus and the codec only)
gcc -O3 -march=native -std=gnu99 -I include -o plane_probe bench/plane_probe.c \
    src/vector/quantize.c src/vector/pack.c -lm
./plane_probe gist_base.fvecs gist_query.fvecs 960 200000 1000 [usedim]
# EC2: SCRIPT=<job that fetches GIST and runs the four arms> bench/aws/run.sh c7i.8xlarge script
```
