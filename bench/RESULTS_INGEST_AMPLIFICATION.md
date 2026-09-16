# Bulk-ingest write amplification: measured, and it is ours too

**Date:** 2026-09-14. **Gap:** G20 (open). **Host:** this workstation, PG 17,
`autovacuum = off`, `maintenance_work_mem = 256MB`, scratch cluster.

Page counts are deterministic and contention-independent, so a loaded workstation
is a valid host for this measurement. No latency is reported here for that reason.

## The question

The sibling project measured 23-30 index pages extended per document at ~1,660
terms per document, with page reuse at ~0.3% — a 12-15x write amplification whose
cause it had misattributed three times before the counters settled it. Our merge
policy differs (leveled, merges only an over-capacity level, no-ops otherwise), and
the previous session recorded that difference as a reason to expect we were better
placed. "Better placed" is not "measured". This measures it.

## Method

`docs(id, body)` with a `weave` index on `to_wdoc('simple', body)`. Each document
is 1,660 terms drawn from a shared 50,000-term vocabulary, so the vocabulary is
realistic rather than one private term set per document, and every document exceeds
one pending page. Batches of 1,000, no `VACUUM` and no `weave_merge()` in between.

**One session throughout.** The allocator counters are backend-local; a read from a
different backend reports that backend's zeros. **One transaction per batch**, as a
top-level `INSERT` rather than a loop inside a `DO` block — see the artifact below.

## Result

| arm | pages | `nseg` | `fsm_reuse` | `fsm_defer` | `extend` | pages/doc |
|---|---:|---:|---:|---:|---:|---:|
| batch 3 (3,000 docs) | 254,856 | 8 | 76 | 18,924 | 96,415 | 96 |
| batch 4 (4,000 docs) | 371,888 | 8 | 19 | 18,981 | 117,032 | **117** |
| +200 docs, one txn each | 385,295 | 7 | 2,793 | 1,007 | 13,407 | 67 |
| after one `weave_vacuum()` | **1,624** | 1 | — | — | — | — |

Six batches (6,000 documents) reach **4,293 MB**. 4,200 documents reach 385,295
pages and one `weave_vacuum()` returns them to 1,624 — **a factor of 237**.

Page reuse is **0.02-0.08%** in the batch arms (worse than the sibling project's
0.3%) and **17.2%** when every document is its own transaction.

## Mechanism, discriminated rather than inferred

`fsm_defer` ~19,000 per batch against `fsm_reuse` of 19-76 is the counters saying:
the free list is consulted, ~19,000 candidates are found, and essentially every one
is rejected by `weave_page_recyclable()`. The inserting transaction freed those
pages itself, through its own merge, so `GlobalVisCheckRemovableXid()` cannot clear
them while it runs. **In-transaction reuse is impossible by construction**, and the
gate is correct — it is the gate whose absence caused a real crash.

The trigger is `weave_insert_oversized_as_segment()` calling
`weave_merge_segments()` after every document. At 1,660 terms every document takes
that path, mints a one-document segment, and rewrites a level-0 run.

This is the outcome pattern G19 was built to distinguish — `extend` high **with
`defer` high** means the recycle gate is the constraint, not a free list that was
never consulted. It read correctly on the first attempt that printed the right
column.

## Three things this is not

- **Not a segment-count runaway.** `nsegments` stayed 7-8 in every arm. The sibling
  project's field report (8 -> 128 segments, then unable to merge or VACUUM) does
  not reproduce here; our leveled compactor holds the count. The cap is not at risk
  and raising it would fix nothing.
- **Not G18.** That was the vacuum-path relocation ratchet, and it required a
  stalled xid horizon. This is the insert path with the horizon advancing.
- **Not fixed by shortening transactions.** One document per transaction improves
  reuse by an order of magnitude, to 17.2%, and is still overwhelmingly extension.

## My own artifact, which did not change the answer and easily could have

The first run put all six batches inside one `DO` block. A `DO` block is one
transaction, so the horizon could not advance between batches and every free page
was trivially unrecyclable — the same class of artifact as `t/008`'s idle-cluster
reclaim reading and `t/015`'s reason for burning xids explicitly. Re-running with
one transaction per batch moved reuse from 0.00% to 0.02-0.08%. The conclusion
survived, which is luck, not method: had the true reuse been 60% the first run would
have reported a defect that did not exist.

The first run also selected `reuse` and `extend` and omitted `defer` — dropping
exactly the column that discriminates the two candidate causes.

## Not fixed here

The sibling project's mitigation gates the insert-time merge on a fan-out's worth of
small runs waiting, and it calls that a mitigation rather than a fix because the freed
pages still cannot pass the XID gate inside the inserting transaction. The real fix
moves the merge out of the inserting transaction — a design change, not a point edit.
G20 stays open with a measured mechanism.

**Retracted 2026-09-16, and the retraction is the point.** This section originally
went on to predict that "our compactor already no-ops when no level is over capacity,
so that gate buys us less than it bought them." Both halves are false.
`weave_merge_segments()` also compacts when `nsegments > WEAVE_MERGE_THRESHOLD` with
no level over capacity, so it does not no-op below the fan-out threshold; and when the
gate was actually built and A/B'd on this same corpus shape, 6,000 documents went from
550,896 pages to 353,181. The prediction was read off the code without running the
arm — in a file whose entire purpose is that reading merge policy off the code had
already been wrong three times. Numbers and commands:
`bench/RESULTS_G20_MERGE_GATE.md`.
