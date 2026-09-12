# P0: the merge's tombstone test was O(terms × chunks)

**Severity:** VACUUM on a delete-heavy index runs without ever finishing. Not a
wrong answer — the merge's output was always correct — but at production scale the
operation does not complete, which for an operator is indistinguishable from a
hang.

**Inherited from pg_fts**, fixed upstream in `670d150` (release 1.6.1,
2026-09-10). pg_weave carried the pre-fix shape because the fork predated it.
Fixed here in `src/am/ambuild.c` (`merge_source_open`,
`weave_merge_segments_streaming`) with the secondary cursor-reset in
`src/am/amvacuum.c`.

## Mechanism

`weave_merge_segments_streaming()` iterates **terms** in sorted order, and each
term's postings ascend from a low docid. So the sequence of docids tested against
a source's tombstone sparsemap is **not globally monotonic** — it resets at every
term boundary.

Both `sm_contains_cached()` (an 8-way MRU cache) and `sm_contains()` (a forward
cursor) assume near-monotonic access. A reset sends either one back to an
`O(chunks)` head-walk. Per posting that cost is invisible; the merge pays it once
per (term, posting), so the total is `O(terms × chunks)`.

The fix decodes the sparsemap **once per source** into a dense bitmap, making each
test an `O(1)` bit read and the per-source cost `O(tombstones + chunks)`.

Two details that are easy to get wrong, both recorded next to the code:

- The bitmap must be sized by the **largest tombstoned docid**, not by the
  segment's live-doc count. A docid is `heap_block * MaxHeapTuplesPerPage + offset`
  — a sparse *global* address unrelated to how many docs a segment holds. Sizing by
  ndocs silently drops most tombstones, and a dropped tombstone **resurrects a
  deleted row**: a wrong answer, not a slow one.
- The allocation uses `WEAVE_ALLOC_MAYBE_HUGE`, not `palloc0`. The bitmap is ~36
  bytes per heap block, so `MaxAllocSize` corresponds to a heap of roughly 236 GB
  — large, but not hypothetical, and this is the allocation class behind four real
  pg_fts crashes.

## Why no CI test can catch this

This is the most important thing on this page.

Upstream added `t/010_vacuum_delete_heavy.pl`, **reverted its own fix**, ran the
new test against the broken code, and the test **passed** (`98b1354`). At ~60k docs
with ~20k tombstones the sparsemap's chunk chain stays short — sparsemap only
allocates chunks where bits exist — so the `O(terms × chunks)` term never dominates
inside a timeout.

Exposure requires **two things at once**:

1. a long chunk chain — a sparse docid space with hundreds of thousands of
   tombstones, and
2. millions of term boundaries — a high distinct-term count.

Upstream measured the shape that kills it: 2.19M docs with 312k tombstones is
~1,068 chunks against ~7.4M terms. That corpus costs ~500 s to build before the
VACUUM even starts, which is why a wall-clock-bounded TAP test at CI scale is
**structurally incapable** of exposing the defect.

pg_weave's `t/008_vacuum_reclaim.pl` never issues a `DELETE` before `VACUUM`, so it
does not even reach the code path. Adding a CI-scale delete-heavy test would create
the *appearance* of coverage without the substance — which is worse than a
documented gap, and is the same failure mode as a queued CI job reading like a
passing one.

## pg_weave's own A/B — status: PENDING

Owed: an out-of-CI scale reproduction on EC2, A/B against the same binary with and
without the fix, on the shape above. A hang cannot be waited out, so the
measurement is a **scaling curve**, not a single number: VACUUM wall-clock at
increasing term counts and tombstone counts, showing superlinear growth before the
fix and near-linear growth after.

Until that curve exists, the justification for this change is upstream's
measurement plus the mechanism above, and this section says so rather than implying
we measured something we did not.
