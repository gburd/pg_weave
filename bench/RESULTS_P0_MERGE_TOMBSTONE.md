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

## pg_weave's own A/B — ATTEMPTED 2026-09-12, DID NOT REPRODUCE, and why

Run `pgweave-20260912-145106` on EC2 (`c7i.8xlarge`, us-east-2) A/B'd the fix
against a reverse-applied patch of exactly this commit, at n = 250k / 750k / 2M.
**It did not reproduce the pathology, and the reason is that it drove the wrong
code path.** Recording the failed attempt because a benchmark that measured the
wrong thing is worth more as a documented dead end than as a silently discarded
run.

The corpus was right by the last attempt. At n = 2M:

| | |
|---|---|
| distinct documents | 2,000,000 |
| distinct terms | 4,908,673 |
| tombstones after `DELETE ... WHERE id % 7 = 0` | 285,714 |
| index size | 400 MB |

That is close to the shape upstream measured as fatal (2.19M docs, 312k
tombstones, ~7.4M terms). So the corpus generation is not the problem.

**The problem is that plain `VACUUM` never calls
`weave_merge_segments_streaming()`.** That function is reached from
`weave_merge_selected()` and `weave_merge_group_to_seg()`
(`src/am/ambuild.c:2771`, `:2745`), which are reached from the *user-callable*
`weave_merge(regclass)` and `weave_vacuum(regclass)` (`src/am/amvacuum.c:910`) —
not from `amvacuumcleanup`. Compounding it, **`nsegments = 1` is enforced by
insert-time tiered merge**, so an index built by a single `CREATE INDEX` has
exactly one segment and a merge has nothing to merge even when it is called.

The measurement said so, and it is worth seeing how:

| n | fixed | before |
|---|---|---|
| 250,000 | 1.0728 s | — |
| 750,000 | **2.789516900 s** | **2.789527626 s** |
| 2,000,000 | 6.3173 s | no result (see below) |

Two independent VACUUMs of 2.79 s agreeing to **10 microseconds** — a relative
difference of 4e-6 — are not two runs of different code. They are the same work
done twice, which is exactly what you get when the only difference between the
binaries lies on a path neither run enters. The `ccur` hoist in `weave_bulkdelete`
*is* on the VACUUM path, and its effect at this scale is evidently below noise.

The 2M `before` arm produced no number at all: the driver process died mid-arm, so
the ~20 minutes it had been running when that was noticed **cannot be attributed**
to the VACUUM rather than to a dropped ssh session, and it is not evidence of
anything. The instance was terminated by hand.

### What the correct reproducer requires

1. **Several segments, each carrying tombstones** — so the merge has multiple
   sources whose tombstone maps must be probed. Build the index, then `INSERT` in
   batches so insert-time tiered merge leaves more than one segment behind.
2. **`DELETE` spread across the whole docid space**, so each source's sparsemap has
   a long chunk chain rather than a few dense chunks.
3. **An explicit `SELECT weave_merge('p0doc_weave')`** — or `weave_vacuum()` — as
   the timed operation. Not `VACUUM`.
4. The corpus assertions the job now makes (distinct documents ≥ n/2, term count
   and tombstone count reported) plus one more it does not yet make: **assert the
   index holds more than one segment before timing the merge**, since a
   single-segment index makes the whole measurement vacuous.

Until that exists, the justification for this change remains upstream's
measurement plus the mechanism above — which is what the previous section says,
and it has not been upgraded by this attempt.

