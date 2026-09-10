---
name: weave-index-am
description: Use when working on the pg_weave access method itself — amroutine callbacks, buffer locking order, GenericXLog, MVCC and snapshot rules, CREATE/REINDEX CONCURRENTLY, vacuum and tombstones, page recycling, cost estimation, or ORDER BY pushdown. Load this before touching src/am/.
---

# Working on the weave index access method

`src/am/` is the AM, split by task L1 into four translation units along the
seams the access method already had:

| file | owns |
|---|---|
| `am.c` | the `amhandler`, reloptions, cost estimation, page allocation and recycling, page kinds, the metapage and its versioned reader, channel descriptors, the segment directory, the posting decoder, and the doclen-sidecar read cursor |
| `ambuild.c` | `ambuild`/`aminsert`, the segment writers (postings, doclen sidecar, dictionary), the streaming k-way merge and the size-tiered merge policy, parallel build and parallel merge |
| `amvacuum.c` | `ambulkdelete`, `amvacuumcleanup`, the vacate/pack/truncate compaction, and `weave_merge()`/`weave_vacuum()` |
| `amscan.c` | `ambeginscan`..`amendscan`, boolean/phrase set algebra, the fuzzy and regex walks, and the block-max WAND / MaxScore top-k |

`src/query/lev.c` (Levenshtein automaton) and `src/pages/trgm_page.c` (trigram
page layout) are ordinary translation units too.

**The interface between them is `include/weave/am.h`,** which for every
declaration says which file defines it, which files consume it, and why it is not
`static`. Read that section before adding a cross-file call: if the symbol you
want is not there, the question is whether your code is in the right file, not
whether to add an `extern`.

## Capability flags, and the reasons

`amcanorderbyop = true` is the interesting one: it lets the planner push
`ORDER BY doc <=> query LIMIT k` into the index scan with no Sort node. That is
what makes ranked retrieval fast, and it is the mechanism `fuse()` will extend
(`doc/specs/FUSED_TOPK.md` §7).

Deliberately false:

| flag | why |
|---|---|
| `amcanorder` | not a naturally ordered index like btree |
| `amcanreturn` | non-covering: postings store analyzed terms, not the original value. No index-only scans, probably permanently |
| `amcanparallel` | parallel *build* yes (`amcanbuildparallel`), parallel *scan* not yet |
| `ampredlocks` | no SSI support |
| `amcaninclude` | — |

## WAL: GenericXLog, exclusively

Every page mutation goes through `GenericXLogStart` /
`GenericXLogRegisterBuffer` / `GenericXLogFinish`. There are **zero** raw
`XLogInsert`, `log_newpage`, or `smgrwrite` calls and it stays that way.

pg_tre used a custom rmgr (140). pg_weave deliberately did not inherit it.
GenericXLog writes more WAL; in exchange, crash safety is auditable by
inspection, replication works without a rmgr on the standby, and the extension
can be `trusted`. That trade is the reason to prefer pg_weave over an AGPL
alternative, so it is not negotiable for a speedup.

Treat the window between `GenericXLogStart` and `GenericXLogFinish` as a critical
section: no data-sized `palloc`, no `ereport`, no additional lock acquisition.

## Locking order

- Readers: `AccessShareLock` on the relation, `BUFFER_LOCK_SHARE` on pages.
- Opportunistic in-scan maintenance (pending flush, merge): `RowExclusiveLock`
  plus an advisory lock so two backends do not merge concurrently.
- `weave_merge()`, on-demand `weave_vacuum()`, CIC finalize:
  `AccessExclusiveLock`.
- Autovacuum cleanup: `ShareUpdateExclusiveLock`, which does not block readers.

Always metapage before segment pages, and lower block number before higher within
a chain. Violating that produces a deadlock the isolation tests will find
eventually, on someone else's machine.

## The page-recycle safety gate

`weave_page_recyclable()` may bypass the deletion-XID safety check **only** when
the caller provably holds `AccessExclusiveLock`, verified with
`CheckRelationLockedByMe()`.

This exists because of a real SEGV found by AddressSanitizer: a concurrent reader
held a pointer into a page that `weave_vacuum()` had freed and reused. If you
touch page recycling, run the isolation tests under ASan, not just normally.

## MVCC and the pending list

`aminsert` always routes new tuples to the **pending write buffer**, never
directly into a segment. Two consequences worth internalizing:

1. It is what makes `CREATE INDEX CONCURRENTLY` and `REINDEX CONCURRENTLY`
   correct: in-flight rows land somewhere searchable without a rebuild.
2. Ranked (`<=>`) scans do **not** see unflushed pending docs; `@@@` and
   `weave_count()` do. That asymmetry is documented, not a bug — and it is why the
   vector channel can avoid incremental graph maintenance entirely
   (`include/weave/graph.h`, "Maintenance"), which is the failure mode
   pg_turbovec's graph kind hit with an O(n) rewrite per insert.

## Concurrent merge and the generation counter

The metapage carries a `generation` counter, bumped on every segment-directory
change. A scan snapshots it and re-checks before trusting results; if it moved, a
concurrent merge may have freed and recycled pages the scan read through a stale
descriptor, so the scan restarts from a fresh snapshot.

Any new code that caches a segment descriptor across page reads must participate
in this. Forgetting to is a use-after-free that only appears under load.

## Vacuum

`ambulkdelete` sets tombstone bits in the per-segment livedocs sparsemap and does
not touch postings — that is what makes it MVCC-correct and cheap. Physical
removal happens during a size-tiered merge.

`weave_vacuum()` does vacate + pack + truncate to reclaim physical space, up to
`WEAVE_VACUUM_MAX_PASSES` (6, a backstop; one pass suffices in the common
single-segment case). Plain merge leaves unreferenced blocks that only REINDEX
reclaims — no page recycler yet, tracked as debt in `doc/PHASES.md`.

If you add a channel, its pages must be reachable from the segment descriptor so
merge and vacuum can find and free them. A weft that leaks pages passes every
functional test.

## Cost estimation

`weave_costestimate()` is a real cost model, not a stub, and it has to be:
`amcanorderbyop` means the planner is choosing between an index `ORDER BY` and a
Sort, and a wrong cost makes it choose a seq scan on a 20 M-row table.

Phase P task P4 is calibrating it against measured latencies. Until then, if you
change the scan's asymptotics, revisit the cost function **in the same commit** —
otherwise the plan silently stops being chosen and the resulting benchmark
regression looks like a scan regression.

## Custom scan

`src/am/customscan.c` intercepts `count(*)` and `EXISTS` over `@@@` and rewrites
them into an index-native count, bypassing per-tuple executor overhead. Measured
3.8 ms against pg_search's 16.31 ms on a 2.19 M-doc corpus. It is installed from
`_PG_init` and must degrade to the normal path whenever the pattern does not match
exactly — a custom scan that fires on a query it cannot answer correctly is far
worse than one that never fires.
