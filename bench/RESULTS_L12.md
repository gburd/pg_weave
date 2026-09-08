# Result: L12 — skipping the vacate phase at end of build

Date: 2026-09-08. Harness `bench/compete`, corpus `synth-2m-long` (2,000,000 docs,
120.1 words/doc), `r6id.4xlarge`, PostgreSQL 17.6 from source, N=200 after 10
warmup. Corpus identity gate PASS across three engines.

## The target

G5 (build time) became the project's largest gap after L14 narrowed G13, and L12 was
its **only remaining route**: L4 (parallel merge) and L11 (parallel scan) were both
withdrawn on upstream evidence — measured at 1.45× slower / 19% bloatier, and
already-built-and-reverted respectively (`doc/PHASES.md`).

## Result

| | before L12 | after L12 | change |
|---|---:|---:|---|
| build time | 495.9 s | **328.0 s** | **1.51× faster** |
| index size | 625 MB | **625 MB** | unchanged |
| gap vs pg_textsearch (47.1 s) | 11.0× | **6.96×** | narrowed |
| gap vs tsvector+GIN (239.2 s) | 2.10× | **1.37×** | narrowed |

Ranked latency did not regress — every band improved marginally, which is
measurement noise rather than an effect:

| band | k=10 before → after | k=100 before → after |
|---|---|---|
| rare | 2.938 → 2.921 | 3.567 → 3.543 |
| mid | 10.561 → 10.478 | 11.167 → 11.076 |
| common | 15.415 → 15.206 | 16.052 → 15.828 |

## Why it works

`weave_vacuum_compact` does a two-phase relocation. Phase 1 **vacates**: rewrites
the segment extend-only onto fresh high blocks so everything below is freed. Phase 2
**packs**: rewrites it low-bias into that now-large low region. Then truncate.

Phase 1 exists for the hard case its own header describes — the live segment sitting
high with a low free region *smaller* than it, where a plain low-bias rewrite fills
the low space and then extends, straddling the file with an un-truncatable live tail.

**At end of build that case does not arise.** The merge writes its output before
freeing its inputs (write-before-free, required for crash safety), so a freshly built
index is roughly 70% freed pages below 30% live — measured at 110 MB freed against
46 MB live on a 1M-document build (`bench/RESULTS_LEXICAL.md`). The low region
already exceeds the live segment by better than 2×, so a single low-bias pass fits
at the front and phase 1 is an entire extra rewrite of the segment for nothing.

`weave_low_free_fits_live()` counts mostly-free against live blocks below the highest
live block and skips the vacate when free ≥ live. Deliberately conservative: it must
never claim one pass suffices when it does not, or the index is left un-truncatable
and the convergence loop burns a pass discovering that.

The 1.51× rather than a clean 2× is expected — the vacate is one of several costs in
a build (analysis, sort, posting construction, WAL) and only the compaction half of
it is removed.

## The size guarantee is intact

This is the property that mattered most, because L8 bought it at the cost of the very
build time L12 is now recovering. Two independent confirmations:

- `t/008_vacuum_reclaim.pl` still reports **"index pages after build: 2199"**, the
  identical floor L8 established.
- The competitive run reports **625 MB as-built**, unchanged, and still 1.40× smaller
  than pg_textsearch's 873 MB and 1.79× smaller than GIN's 1120 MB.

## What is left of G5

**6.96× behind pg_textsearch** on build. No further route is identified. The honest
position, unless one appears: pg_weave builds more slowly than its competitors, and
that is the price of a compacted index that is 1.4–1.8× smaller and needs no
follow-up maintenance step. That trade should be stated in the README rather than
left looking like a pending fix.

Remaining candidates, none costed:
- The build's own sort and posting-construction phases have never been profiled.
  L9 owes a profile and has only ever been aimed at *scan* cost.
- pg_textsearch builds in 47 s for a 873 MB index. Understanding *how* is a
  prerequisite for claiming the gap is closable at all.

## What this does not tell us

Synthetic text at 120 words/doc, not Wikipedia at 485. Single build, not a
distribution — build time was measured once per engine, so the 1.51× carries no
confidence interval, unlike every latency number in this harness.
