# Result: L15 — the build's hash cost

Date: 2026-09-09/10. Harness `bench/compete`, corpus `synth-2m-long` (2,000,000
docs, 120.1 words/doc), `r6id.4xlarge`, PostgreSQL 17.6 from source,
`DO_PROFILE=1` on every run so the numbers are like-for-like (see "Method").

## The target

`bench/RESULTS_BUILD_PROFILE.md` attributed **37.5% of CREATE INDEX** to
`hash_search_with_hash_value` and named the build's `TermKey` — a fixed 64-byte
`HASH_BLOBS` key hashed and `memcmp`'d in full on every term occurrence — as the
cause. The task was written as two steps: a length-aware key first, `simplehash.h`
second if justified. Gate: **build ≤ 260 s** with size unchanged at 625 MB and no
ranked-latency regression.

## What was actually found

The profile's *symbol* was right and its *explanation* was wrong, twice over.

| step | change | `timed_index` | phase A (default) | `hash_search` self |
|---|---|---:|---:|---:|
| baseline (2 runs) | — | 353.8 s / 356.5 s | 349.6 s | 37.5% |
| 1 | length-aware `TermKey` (`HASH_FUNCTION`/`HASH_COMPARE`/`HASH_KEYCOPY`) | 340.9 s | 344.9 s | **38.5%** |
| 2 | merge bypasses the term hash entirely | 274.6 s | 274.2 s | **46.8%** |
| 3 | doclen collector: dynahash → per-heap-block radix map | **194.2 s** | **194.5 s** | **14.3%** (scan-side term hash only) |

**Gate: build ≤ 260 s — MET, at 194.2 s (1.82× faster than the 353.8/356.5 s
baseline under identical conditions).**

**Step 1** cut hashing and comparison to the real term length (~10 bytes instead
of 64) and moved the needle **3.9%**. The share of time in `hash_search` did not
fall — it rose. So the cost was never the hashing or the `memcmp`; those live in
separate frames. It is dynahash's own bucket-chain pointer chasing, i.e. cache
misses on a table larger than L2.

**Step 2** removed the merge's term hash outright. The streaming merge gathers
*one term per pass* and was creating a fresh dynahash (child memory context,
1024-bucket directory) per term, then probing it once per posting of that term —
a lookup whose answer was always "the single entry this table can hold". That
was 19% of the build. Removing it gave **−19.5%** (340.9 → 274.6 s). But
`hash_search` *still* accounted for 46.8% of the remainder, 29.5% of it under
the merge — which now contained no term hash at all.

The remaining hash was the **doclen sidecar collector**: a `uint64`-keyed
dynahash mapping docid → quantized length, `HASH_ENTER`'d **once per posting**
(~240M times) by both the segment writer and the merge, holding ~2M entries,
grown from a 65,536-entry hint. It is `docid`-keyed, and
`docid = heapblock × MaxHeapTuplesPerPage + offset`, so it is dense per heap
block. **Step 3** replaces it with a two-level array (heap block → growable
per-block byte leaf): O(1) lookup with no hashing, ~1 byte per live tuple, and
in-order iteration for free, which also deletes the `qsort` of 2M entries the
writer used to do.

## Method

`DO_PROFILE=1` runs `bench/build_profile.sh` *before* the harness's own
`timed_index`: three unprofiled `CREATE INDEX` runs (A default, B positions=on,
C trigrams=on) plus one 60 s `perf record` sample. The `timed_index` number
therefore comes after four prior builds on the same heap and reads ~8% higher
than in a plain run (328.0 s vs 353.8/356.5 s for identical code). The cause of
that ordering effect is not isolated. All rows in the table above are
`DO_PROFILE=1` runs and are comparable with each other; **none is comparable with
the 328.0 s in `RESULTS_L12.md`** without a plain run, which is recorded at the
end of this file.

Index size was verified unchanged after every step: postings 78,662 pages
(615 MB), dictionary 785, doclen_sidecar 584, total 625 MB. The sidecar page
count being identical after step 3 is also the check that it stores the same
docid set (see "Correctness").

## Plain run, three engines (the number comparable with `RESULTS_L12.md`)

`compete-20260910-010740`, no profiler, N=200 after 10 warmup, corpus identity
gate PASS across all three hosts.

| | L12 run (2026-09-08) | this run | change |
|---|---:|---:|---|
| pg_weave build | 328.0 s | **192.5 s** | **1.70× faster** |
| pg_textsearch build | 47.1 s | 49.2 s | +4.5% (noise) |
| tsvector+GIN build | 239.2 s | 202.7 s | −15% (noise — same code) |
| gap vs pg_textsearch | 6.96× | **3.91×** | narrowed |
| gap vs GIN | 1.37× | **0.95×** | pg_weave now builds faster than GIN in this run |
| index size | 625 MB | 625 MB | unchanged |

GIN's 15% run-to-run swing on identical code is the honest error bar on a single
build-time sample per run. pg_weave's −41% is well outside it; the GIN
comparison is a tie-to-win, not a clean win, until a run reproduces it.

Ranked latency, p50 ms, pg_weave, L12 run → this run (cross-run, so read as
noise unless large and one-directional):

| band | k=10 | k=100 |
|---|---|---|
| rare | 2.921 → 2.815 (−3.6%) | 3.543 → 3.446 (−2.7%) |
| mid | 10.478 → 10.259 (−2.1%) | 11.076 → 11.178 (+0.9%) |
| common | 15.206 → 15.351 (+1.0%) | 15.828 → 16.267 (+2.8%) |

Mixed sign, all ≤ 4%: no ranked regression. Two non-ranked rows moved more:
`count_not` 66.2 → 73.0 ms (**+10%**) and `count_or2` unchanged. L15 touches
only CREATE INDEX — the postings and sidecar page counts are identical and no
scan-path code changed — so the `count_not` delta has no mechanism in this
change; it is recorded here so the next run can confirm or retire it rather
than being averaged away.

All four pg_weave correctness gates passed on the plain run, including
`topk_parity_vs_bm25_oracle`, which depends on the sidecar's doclens through
BM25 length normalisation.

## Correctness

Every step passed the full suite on PG 17.11 and PG 18 (5 regression, 2
isolation, 61 TAP) plus `check-alloc`, `check-unity`, `check-rename`. The
regression suite's ranked outputs depend on the sidecar's doclens through BM25
length normalisation, so an incorrect step-3 sidecar would change `expected/`
output; it did not. The plain three-engine run's `topk_parity_vs_bm25_oracle`
gate passed as well.

One deliberate semantic note on step 3: a doc whose quantized length byte is 0
(doclen 0) is no longer written to the sidecar. `weave_doclen_lookup()` returns
0 for an absent docid, so no reader can distinguish the two; the sidecar can only
get smaller. Such a doc has no postings that could reach the collector anyway.

## Post-measurement change

After the runs above, the collector's leaf indexing was changed from
`bytes[offset - 1]` to `bytes[docid % WEAVE_OFFSET_FACTOR]`. The docid format
sets `WEAVE_OFFSET_FACTOR = MaxHeapTuplesPerPage`, so an offset equal to the
factor aliases to `(block + 1, 0)` — pre-existing in the format and harmless in
`weave_docid_to_tid`, but under the `- 1` indexing it would have been a
one-byte out-of-bounds write instead of an alias. The committed code mirrors
`weave_docid_to_tid`'s split exactly and is total over any `uint64`. The
difference is one subtraction per posting; the measured numbers stand, but the
measured binary is not byte-identical to the committed one.

## What this changes about the profile's conclusion

`RESULTS_BUILD_PROFILE.md` §"Where the remaining time can go" proposed
`simplehash.h` as step 2. That is withdrawn: after step 3 there is no hash on
the per-posting path at all. The scan-side term hash (`add_posting` under
`weave_build_callback`) is the only dynahash left, now 14.3% of a 194 s build
(~28 s). Converting it to `simplehash.h` might recover a third of that; it is
not the next thing to do.

The profile's own ceiling estimate — "eliminating all of the hash cost would take
the build to about 205 s" — was reached and passed (194 s) because the estimate
counted only the term hash's 37.5% and did not know the doclen collector
existed. Two lessons for the next profile: attribute by *caller* as well as by
symbol, and treat any per-posting `hash_search` as suspect regardless of what
it is keyed on.

The post-L15 profile top three are `weave_merge_segments_streaming` 17.4% (self),
`weave_write_postings` 14.7%, `hash_search_with_hash_value` 14.3%. The merge's
self time is the k-way term comparison and cursor advance; the writer's is FOR
packing. Those are the next targets if G5 is pursued further, and neither is a
hash.
