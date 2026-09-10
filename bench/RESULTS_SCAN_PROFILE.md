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

### Why this cannot be fixed by caching, and what the actual route is

Worth deriving, because two plausible fixes are already known-dead and a third
looks right until you do the arithmetic.

The sidecar's docid column is **delta-coded** (`weave_for_pack` over gaps), so
finding a docid requires prefix-summing gaps from the block start. The candidate
docids of a single-term scan ascend monotonically, so the current code is
already performing an *optimal sequential sweep*: ~15,000 block loads × 128
entries ≈ 1.9M gap decodes, against a floor of 2M (one per document in the
corpus, since a mid-frequency term's candidates span the whole docid range).
**The sequential decode is not redundant — it is at its floor.** Hence:

- **Caching the whole page instead of the block does not help** (same total
  entries). It was already tried and reverted for a worse reason: it amplified
  scattered rare terms ~200×, ~7.9 ms of a 10.2 ms rare-term scan
  (`src/am/am.c`, `weave_doclen_cursor_load_page` header).
- **Lazy in-block decode barely helps.** With ~2.6 uniformly-placed candidates
  per 128-entry block, the *last* one sits at ~93% of the block, so stopping at
  the requested docid saves ~7%.
- **Resuming the header walk** from the current block instead of the page start
  removes ~610K wasted iterations per query — real, but only ~14% of
  `load_page`'s self time. The other ~86% is the FOR-unpack plus prefix-sum of
  128 entries per block change (~1.9M each, both inlined into `load_page`, which
  is why they appear as its self time).

So the only way to beat 1.9M is to **skip** entries rather than decode them, and
that requires random access into the block. `weave_for_get(buf, i)` already
provides exactly that — FOR packing is fixed-width, so entry *i* is O(1)
addressable — but it cannot be used on a *gap* column, because entry *i*'s docid
is a prefix sum, not a stored value.

**The route is therefore to store the docid column as absolute offsets from the
block's `first_docid` instead of gaps.** Both are FOR-packed by the same codec;
only the values change. That makes the column monotone *and* randomly
addressable, so locating a docid inside a block becomes a **binary search of
~7 `weave_for_get` calls instead of 128 unpacks plus 128 prefix-sum stores**, and
the resident `docid[]`/`byte[]` arrays stop being needed at all — the byte is
read directly at the found index.

| | now (gaps) | absolute offsets |
|---|---:|---:|
| decode work per block change | 128 unpack + 128 prefix-sum | ~7 `weave_for_get` |
| per mid k=10 query | ~1.9M entry decodes | ~105K |
| coded width, 128-doc block spanning ~6,400 docids | ~6 bits (gap ≈ stride) | ~13 bits |
| sidecar size | 4,672 kB | ~9 MB (**+0.7%** of a 625 MB index) |

That is a **segment-format change**, but a narrowly scoped one: it touches only
the doclen sidecar's docid column encoding, and the segment format is already
versioned (v3 inline vs v4 sidecar, with `weave_meta_upcast_page` and a
reader that dispatches on version). It needs a version bump, an upgrade path
that can still read v4 gap-coded sidecars, and an extension of the standalone
FOR/wire property tests — which is why it is a task and not a patch.

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
