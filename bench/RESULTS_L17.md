# Result: L17 — absolute-offset doclen sidecar (v5)

Date: 2026-09-10. `bench/compete`, corpus `synth-2m-long` (2,000,000 docs,
120.1 words/doc), `r6id.4xlarge`, PostgreSQL 17.6 from source, N=200 after 10
warmup, one segment, fully warm. Corpus identity gate PASS across both engines.

## The change

`bench/RESULTS_SCAN_PROFILE.md` measured ~72% of a ranked scan in the doclen
sidecar cursor. The sidecar's docid column was **gap-coded**, so answering one
lookup meant FOR-unpacking a whole 128-entry block and prefix-summing it — to use
~2.6 of those entries, because a single-term scan probes ascending docids with
stride `ndocs/df`.

v5 stores each docid as an **absolute offset from the block's `first_docid`**
instead of a gap. Same codec, same column, different values — but the column
becomes monotone *and* fixed-width addressable, so `weave_for_get()` reads any
entry in O(1) and an in-block lookup is a **binary search over the packed bytes**
with no decode at all. Each block self-describes its encoding via the
`WEAVE_DOCLEN_ABS` flag in the high bits of `WeaveDoclenBlockHdr.count`, because
an index built by ≤ 0.5.0 keeps v4 pages after upgrade while later merges write
v5 ones.

## Result

| band | before (v4) | after (v5) | change |
|---|---:|---:|---|
| ranked_rare_k10 | 2.815 | **1.508** | **1.87× faster** |
| ranked_rare_k100 | 3.446 | **2.203** | 1.56× faster |
| ranked_mid_k10 | 10.259 | **6.191** | **1.66× faster** |
| ranked_mid_k100 | 11.178 | **7.201** | 1.55× faster |
| ranked_common_k10 | 15.351 | 14.919 | 1.03× (flat) |
| ranked_common_k100 | 16.267 | 15.870 | flat |
| index size | 625 MB | **626 MB** | +0.16% |

Scan profile, same query (ranked mid k=10), before → after:

| | before | after |
|---|---:|---:|
| doclen cursor total | **71.9%** | **45.2%** |
| ├ `weave_doclen_cursor_load_page` | 64.6% | 13.0% |
| └ `weave_doclen_cursor_lookup` | 7.3% | 32.1% |
| `weave_topk_candidates_range` | 9.2% | 18.1% |

### Why `common` barely moved, and why that is the expected result

The win scales with **candidate stride**, which is `ndocs/df`. A common term
(df 1,741,439 of 2,000,000) has a stride of ~1.15 docids, so a 128-docid block
already covered ~111 candidates and the decode was already amortised across them.
Rare and mid terms are sparse — stride ~200 and ~50 — so each block was decoded
in full to serve 0.6 and 2.6 candidates respectively. L17 removes exactly that
amplification and nothing else, so a flat `common` is the prediction, not a
disappointment.

This also re-aims **L2**. The remaining `common` gap is not the cursor: at
stride 1.15 the scan is genuinely reading 1.74M postings, which is the
posting-decode/candidate-count cost impact-ordered blocks and earlier block-max
termination attack. L2 is now pointed at the one band where it applies.

## Against pg_textsearch (same run, same corpus)

| query | pg_textsearch | pg_weave | before L17 | now |
|---|---:|---:|---|---|
| ranked_rare_k10 | 1.252 | 1.508 | 2.25× behind | **1.20× behind** |
| ranked_mid_k10 | 1.624 | 6.191 | 6.31× behind | **3.81× behind** |
| ranked_common_k10 | 3.142 | 14.919 | 4.86× behind | 4.75× behind |
| ranked_rare_k100 | 8.726 | **2.203** | 2.46× win | **3.96× win** |
| ranked_mid_k100 | 9.123 | **7.201** | tie | **1.27× win** |
| ranked_common_k100 | 14.836 | 15.870 | tie | 1.07× behind |

G13's rare band is now near parity and the k=100 wins widened. Common k=10
remains the worst ratio and is L2's target.

## The sidecar A/B inverted, which was the point

`doclen_sidecar=on` vs `off` (v3 inline doclen), per-query including ~7 ms of
`psql` startup in both arms, so read the difference and not the absolute:

| band, k=10 | on (v5) | off (inline) | before L17 (on = v4) |
|---|---:|---:|---:|
| rare | **7.60** | 7.88 | 9.64 (vs 9.69 off) |
| mid | **12.34** | 12.41 | 17.13 (vs 17.23 off) |
| common | **21.38** | 21.92 | 23.03 (vs 22.96 off) |
| size | **626 MB** | 859 MB | 625 MB |

Before L17 the two arms were identical within noise — two costs cancelling
(sidecar: cheap 3-column postings + expensive cursor; inline: free doclen +
37% more posting bytes). L17 removed the cursor cost, so the sidecar is now
faster than inline in **all three bands** as well as 233 MB smaller. That is the
strict win `RESULTS_SCAN_PROFILE.md` predicted from the cancellation.

## Gate: latency MET, buffer-hit target MISSED

| gate | target | actual | |
|---|---|---:|---|
| mid k=10 p50 | ≤ 8 ms | **6.191** | ✅ |
| rare k=10 p50 | ≤ 2.2 ms | **1.508** | ✅ |
| common k=10 | no regression | 14.919 (from 15.351) | ✅ |
| index size | ≤ 630 MB | **626 MB** | ✅ |
| buffer hits, ranked mid k=10 | < 3,000 | **11,924** (from 11,702) | ❌ |
| v4 index still reads after upgrade | correct results | see limitations | ⚠️ untested at page level |

**The buffer-hit gate was mis-specified and I should not have written it.** Buffer
hits track *block changes*, and L17 did not change how often the cursor changes
block — it changed what happens when it does (a few-hundred-byte `memcpy` instead
of 128 unpacks plus 128 prefix-sum stores). One `ReadBuffer`/`UnlockReleaseBuffer`
per block change remains, so the count is unchanged at 11,924. The number that
matters moved 1.66×; the gate metric I chose was measuring the mechanism I left
alone.

## What remains, and it is now cheap

The profile after L17 says the cursor is *still* the largest single cost at 45.2%,
now concentrated in `_lookup` (32.1%) rather than `load_page` (13.0%). Two
follow-ups, both consequences of measurements in this run:

1. **The 8-step linear walk before the bisect is counterproductive at sparse
   stride.** ~32% self time for ~39,683 lookups is ~50 ns each, about 15
   `weave_for_get` calls — consistent with the walk running its full 8 probes and
   *failing* (at stride 50 the next candidate is usually in a different block),
   then paying 7 more to bisect. The walk was tuned for consecutive docids, i.e.
   common terms. Bisect immediately when the hint's value already exceeds the
   target.
2. **Copy the whole page instead of the block.** Previously rejected because it
   meant decoding ~4,000 entries to use ~2.6 (~200× amplification, and it was
   tried and reverted). **v5 does not decode**, so a page copy is an 8 KB `memcpy`
   that then serves every block on that page: buffer hits and header re-walks
   both fall from ~15,000 to ~584 per query, at the same total memcpy volume.
   The objection that killed this idea for v4 does not apply to v5.

## Limitations, stated rather than smoothed over

- **No test reads a real on-disk v4 sidecar page.** The dual reader is covered at
  the codec level — `test/hegel/test_doclen_block.c`, 2,839,534 checks, asserts
  that gap and absolute encodings decode to identical docid sequences and that
  binary search finds every present docid and rejects every absent one including
  interior holes — and the metapage version gate stops an older `.so` from
  meeting a v5 block. But the v5 writer can no longer *produce* a v4 page, so
  nothing exercises the v4 branch against real bytes. Closing this needs either a
  test-only encoding GUC or a committed binary fixture; `WEAVE_TEST_HOOKS` exists
  for this purpose but is currently dormant (nothing builds with it and no TAP
  test uses its existing hook), so adding a second dormant hook would give false
  comfort rather than coverage.
- **One corpus, one shape.** Uniform synthetic term distribution. Real corpora
  cluster docids, which raises the candidates-per-block ratio and therefore
  *shrinks* L17's win — the effect measured here is closer to a best case than a
  typical one.
- **`count_not` 76.7 ms and `count_prefix`** remain the worst absolute pg_weave
  numbers and are untouched by this work.
