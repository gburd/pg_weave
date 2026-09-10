# Result: the ranked-scan profile (G13 / L9)

Date: 2026-09-10. `bench/scan_profile.sh` via
`DO_PROFILE=scan bench/compete/orchestrate.sh synth-2m-long weave`,
corpus `synth-2m-long` (2,000,000 docs, 120.1 words/doc), `r6id.4xlarge`,
PostgreSQL 17.6 from source, index 625 MB, one segment, fully warm
(every EXPLAIN below is 100% `shared hit`, zero reads).

## Why this was taken before writing any code

`doc/PHASES.md` **L2** asserts G13's root cause is a "decode-bound posting scan
on high-df terms" and prescribes **impact-ordered posting blocks** — a change to
the on-disk segment format. That sentence has never been checked against a
profile. `bench/RESULTS_BUILD_PROFILE.md` is the precedent for the cost of not
checking: it identified the right hot symbol and the wrong cause, twice, and L15
spent two of its three steps discovering that.

The latency shape also constrained the answer in advance. pg_weave's k=10 and
k=100 differ by <10% in every band, while pg_textsearch's k=100 is 5–7× its
k=10, and the WAND `initial_k` sweep shows `initial_k` is not the floor (mid
k=10 is 9.82 ms at 4 vs 10.31 ms at 32). So the cost had to be **proportional to
df and independent of k** — i.e. paid per *candidate*, not per *result*.

## The measurement

### Self time, ranked mid k=10, 4,000 reps in one backend

```
64.55%  weave_doclen_cursor_load_page   <- weave_doclen_cursor_lookup
                                           <- weave_topk_candidates_range
 9.19%  weave_topk_candidates_range
 7.31%  weave_doclen_cursor_lookup      <- weave_topk_candidates_range
 2.20%  hash_search_with_hash_value     <- BufTableLookup <- ReadBufferExtended
                                           <- weave_doclen_cursor_load_page (1.91%)
```

**~72% of a ranked scan is the doclen sidecar cursor**, and only 1.9% of that is
buffer lookup — the pages are resident, so this is **decode and locate cost, not
I/O**. Posting decode, BM25 scoring and WAND bookkeeping together are the
remaining ~28%.

### `EXPLAIN (ANALYZE, BUFFERS)`, mid k=10

```
Limit (actual time=12.071..12.297 rows=10 loops=1)
  Buffers: shared hit=11702
  ->  Index Scan using docs_weave (actual time=12.070..12.294 rows=10)
        Buffers: shared hit=11702
Execution Time: 12.330 ms
```

**11,702 buffer hits to return 10 rows.** The mid term's df is 39,683, so that is
one page touch per **3.4 candidates** — against the cursor's own documented
intent of "~1 page read per ~128 scored docids" (`src/am/am.c`, doclen reader
header). A 37× miss.

## The mechanism

`weave_doclen_cursor_lookup()` keeps **one 128-docid block** resident, not one
page. On any lookup outside the resident block it calls
`weave_doclen_cursor_load_page()`, which:

1. `ReadBuffer` + `LockBuffer` the page **again**, then
2. **re-walks that page's block headers from the start of the page** to find the
   block containing the docid (a page holds tens of blocks), then
3. FOR-unpacks 128 gaps and prefix-sums them to materialise the block.

A single-term ranked scan probes candidate docids in ascending order with a
stride of `ndocs/df`. For the mid band that stride is ~50 docids, so a 128-docid
block spans only ~2.6 candidates — a block change, and therefore a full re-pin
plus header re-walk plus 128-entry decode, **every 2.6 lookups**. That is
~15,000 block loads per query, and it is exactly the k-independent,
df-proportional cost the latency table demanded.

The design intent in the code is right; the implementation re-derives its
position from scratch on every block change instead of advancing.

## The A/B that proves the sidecar is not the problem — the *cursor* is

`doclen_sidecar=off` falls back to the v3 inline doclen column: same ranking,
no sidecar lookups at all.

| band, k=10 | sidecar=on (default) | sidecar=off (v3 inline) |
|---|---:|---:|
| rare | 9.64 | 9.69 |
| mid | 17.13 | 17.23 |
| common | 23.03 | 22.96 |
| **index size** | **625 MB** | **859 MB** |

(These numbers include ~7 ms of `psql` process startup per query — the arms are
measured identically so the *difference* is what is being read, not the
absolute. Server-side latencies are in the compete SUMMARY.)

Identical within noise, at a 37% size penalty for `off`. That equality is *not*
"the sidecar is free" — it is two costs cancelling, and the cancellation is the
most useful thing in this profile:

- **sidecar=on:** posting blocks carry 3 FOR columns (625 MB), so posting decode
  is cheap — but every candidate needs a cursor lookup into delta-coded sidecar
  pages, which is the 72% above.
- **sidecar=off (v3):** doclen is a 4th FOR column in the posting block, so it
  arrives *free* with the posting decode and there is no lookup at all — but the
  index is 859 MB and there is 37% more posting data to decode.

The two effects are within noise of each other on this corpus. Conclusions:

1. **L3 is already satisfied and is closed.** L3 claims the v4 sidecar "decodes
   the whole segment sidecar per scan, a fixed ~18 ms" and prescribes a
   page-directory cursor. pg_weave already has that cursor, there is no
   whole-sidecar decode, and the sidecar saves 234 MB at no latency cost. The
   gap L3 describes does not exist in this codebase.
2. **The sidecar's win is size, and it is paid for in cursor work.** Making the
   cursor cheaper is therefore a *strict* improvement — it keeps the 234 MB and
   removes the cost that currently offsets it. That is a better target than
   either L2 or L3.

### Why decoding cannot simply be cached away

Worth stating so the fix is not over-promised. The sidecar is delta-coded, so
finding a docid requires prefix-summing gaps. The cursor already avoids the
obvious waste: it walks *block headers only* (which carry `first_docid`) to find
the covering block and FOR-unpacks just that one block. With mid's ~50-docid
stride, ~2.6 of a block's 128 entries are ever used, and per query that is
~15,000 block decodes ≈ 1.95M entry decodes — for a corpus with 2M documents.
So the scan decodes roughly *one entry per document in the corpus* regardless.
Decoding the whole page instead of the block does not help: same total entries.

What *is* pure waste, and what L17 removes:

- the **header re-walk from the start of the page** on every block change
  (~40 header steps × ~15,000 loads ≈ 610K wasted iterations per query),
- the **`ReadBuffer`/`LockBuffer`/`UnlockReleaseBuffer` per block change**
  (~15,000 pin cycles per query; the 11,702 buffer hits in the EXPLAIN above),
- decoding a block **past** the docid actually requested.

So L17 is expected to remove a substantial fraction of the 72%, not all of it.
Gate accordingly: mid k=10 ≤ 8 ms (from 10.26), rare ≤ 2.2 ms (from 2.82), with
index size unchanged at 625 MB.

## Consequence for L2

**L2's stated root cause is wrong and its prescription is not justified by any
measurement.** Impact-ordered posting blocks change the on-disk format to let
block-max WAND terminate earlier — which attacks posting decode, i.e. ~28% of
the scan, not the 72%. L2 is **not withdrawn** (earlier WAND termination is still
worth having, and would compound with this fix by reducing the candidate count
itself), but it is **demoted below** the cursor fix and its "root cause" sentence
is corrected.

The new task is **L17**: make the doclen cursor advance instead of re-deriving.
Keep the page pinned while walking forward within it, resume the block-header
walk from the current position rather than the page start, and only fall back to
the directory binary search on a genuine backward seek or page change. No
on-disk format change.

## What this does not tell us

- **One corpus, one shape.** Synthetic 120-word docs with a uniform term
  distribution; the candidate stride that makes a 128-docid block cover 2.6
  candidates is a function of df/ndocs, so a real corpus's clustering will
  change the constant (probably favourably — clustered docids share blocks).
- **Single-term queries only.** The cursor's block cache is shared across a
  query's terms, and the code comments claim a multi-term win from that. A
  boolean or phrase query may thrash it differently, better or worse.
- **`common_prefix` (763 ms) and `count_not` (73 ms) are unprofiled** and are the
  two worst absolute pg_weave numbers in the compete table. Neither is in G13.
