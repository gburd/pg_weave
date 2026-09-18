# Owed upstream — reports and patches to send

Tracked here because `make check-rename` bans upstream version citations in source
comments, so provenance and debts live in docs. Delete an entry when it is sent.

## To pg_fts — the insert-time merge gate's stated rationale is false, and a test is lying

Scope narrowed 2026-09-18 after reading 1.7.2..1.8.3. Two of the three things we were
going to report have been superseded by their own work; what is left is sharper.

**1. The gate's rationale contradicts the code 500 lines above it.** The comment on
`bm25_pending_segments_worth_merging()` says that below the threshold
`bm25_merge_segments()` "finds no level over capacity and is a no-op that we can skip
without changing behaviour". But `bm25_merge_segments()` has a **second trigger** —
`nsegments > BM25_MERGE_THRESHOLD` compacts the lowest level with >= 2 runs — so the
skip is not behaviour-neutral. We ported the gate, hit the same thing, and measured it:
peak live segments moved 8 -> 15 against a cap of 128.

**Their own 1.7.2 CHANGELOG contains the disproof.** It records "max 15 segments against
the cap of 128" as a success and tightens `t/007` from <= 128 to <= 64. That number *is*
the behaviour change the rationale says cannot happen. So the tightening is not
belt-and-braces; it is the only guard on the property the gate put at risk, and the
reasoning should say so.

**2. And that measurement is probably an artifact — this is the part they cannot get
from their own tree.** Our `t/007`, inherited from theirs, pumps `IPC::Run` handles with
`finish($h) for @ins`, which runs the "concurrent" inserters **serially**: each handle is
pumped to completion while the others sit in `ClientRead` on empty stdin. With the
pumping fixed we see a peak of **119-126**, not 15, bisected across five trees and
present before any of our changes (our G28). If their harness has the same shape — and
the idiom is theirs — then "max 15" is measuring one inserter at a time, and `<= 64` is
resting on it. They have already documented the pump-starvation trap in their own 1.8.3
test work, which is how we knew to look.

**3. A second, independent A/B of the gate**, same corpus shape, two arms: -35.9 % pages
with one transaction per 1,000-document batch, -53.1 % with one per document, identical
post-vacuum page count in both arms — so the gate moves scratch space, not resident data.

**Do NOT send** anything premised on "in-transaction reuse is impossible by
construction". They retracted that themselves (measured at 1.4x, not "several times") and
we were the ones still carrying it; we have adopted their correction instead.

**Also worth telling them, as a thank-you rather than a bug:** their
`BM25_ALLOC_SNAPSHOT` is a partial fix, not a general one. We ported it and measured
127,248 -> 5,066 pages with one transaction per document and **353,181 -> 352,027 —
nothing — with one transaction per 1,000-document batch**, because inside one transaction
the xid horizon cannot advance, so the snapshot fills with pages that are not recyclable
yet and the search for a usable one costs ~195 deferred probes. Unbounded probing reaches
179,418 pages but costs 48 % wall clock. Details in
`bench/RESULTS_G20_SNAPSHOT_ALLOC.md`.

## To pg_tre — a `uleven.c` out-of-bounds read, still present at 4.0.2

We fixed it on import; their `src/query/uleven.c` still has the unguarded loop. Ours is
`src/query/uleven.c` with the guard. Send the guard, and note that we are a
PostgreSQL-licensed fork of their MIT code so the flow back is one-directional by
licence, not by preference.

**Also worth telling them:** pg_tre 4.0.0 removing the custom resource manager and
converting all 22 WAL sites to generic WAL converged on the choice we made at fork time
for `trusted` eligibility. Their `6227d7c` (first pending page left `pending_head`
un-WAL-logged) is the bug class we audited for and did not find, and one thing they may
want: `blbuildempty()`'s post-PG-16 form is **not** a safe template for an init fork,
because it relies on `BM_PERMANENT` (a checkpoint) rather than WAL, so a crash before the
next checkpoint loses the metapage and a standby gets nothing. We hit that while fixing
our own `ambuildempty` and ended up refusing unlogged indexes instead.

## Owed BY us, to ourselves, not upstream

- The second half of pg_tre `4a9c86c`: the scan-side prefilter must not reject when
  `always_true`. Still owed, as a Z7 gate.
