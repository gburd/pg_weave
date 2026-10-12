# Sibling-project currency

pg_weave forks or draws ideas from four sibling projects (see `doc/ARCHITECTURE.md`
§4 and `doc/LICENSING.md`). Their reviews have repeatedly caught bugs we share, so
hard rules 8, 10, 11 and 12 cite them. This file records when we last checked them
against our tree and what we took or confirmed, so the check is dated and not
re-derived from memory.

## 2026-10-12 check (local checkouts in `~/ws`, `~/src/zvec`)

| project | our doc pin | their latest tag then | gap |
|---|---|---|---|
| pg_fts | 1.5.8 | **1.12.0** | large |
| pg_tre | 3.2.1 | **4.3.0** (HEAD WIP 4.3) | large |
| pg_turbovec | 2.1.0 | **2.12.0** | large |
| zvec (ideas only, Apache-2.0, never copy) | — | 0.7.0+ | — |

Cross-checked the sibling release notes between our pins and their latest against
our GAPS/history and current source. Findings:

- **Already covered, equal or better in pg_weave** (no action):
  - pg_fts 1.7.0/1.7.1 pd_lower-at-every-read-site: our `make check-pdlower` lint
    predates it and we fixed four stragglers (AGENTS.md).
  - pg_fts 1.6.1/1.7.0 "VACUUM can never complete / permanently unvacuumable at
    scale": our L22/L24/G73 family and the `check-alloc` idiom widening.
  - pg_fts 1.6.0 "unverifiable phrase is FALSE, not a silent AND": our phrase /
    exact-gap / tsquery-cast work (G93 and the phrase tasks).
  - pg_fts 0.3.4/1.0.x corpus-scale `palloc` crashes: `check-alloc`.
  - pg_tre 4.0.2 "Index Scan dropped HOT-updated rows": closed as **G40**
    (2026-10-12) with both-major HOT-successor mutation evidence; pg_weave is
    structurally immune (HOT-root TIDs), the gap was coverage.
  - pg_tre 4.2.1 "INSERT self-deadlock on its own pending-list tail
    (LWLock:BufferContent, immune to cancel)": pg_weave's allocator already uses
    `ConditionalLockBuffer` on every FSM candidate
    (`src/am/am.c` weave_new_buffer_internal ~1005, ~1065), the same nbtree-style
    fix pg_tre had to add. Structurally immune.
  - pg_turbovec 2.2.2 "check() did not validate the scales/calibration chain
    (scan-fatal blind spot)": our `weave_check` walks all four vector chains and
    sets `incomplete` when a calibration chain is present (`amcheck.c:1786-1792`).

- **Worth watching / possible future adoption** (ideas, not bugs we share):
  - pg_fts 1.9.0/1.10.0/1.12.0 ranked-path performance (shared doclen arrays,
    in-memory dictionary-index lookups on the ranked path). Our ranked path is
    forked from 1.5.8; these are candidate speedups if ranked latency is revisited.
  - pg_tre 4.0.0 "no custom resource manager, no shared_preload_libraries" —
    pg_weave already took this stance from the start (GenericXLog only, trusted).
    Confirms the architecture choice; nothing to adopt.
  - pg_tre 4.3.0 "every server encoding; classes and (?i) follow core regex" —
    compare against our regex/fuzzy work (G88/G91/G92) if that area reopens.
  - pg_turbovec 2.6.0-2.8.x 1-bit sign BQ + IVF+BQ composition, 2.10.x IVF cell
    layout stability across INSERTs and ~3.5x CREATE INDEX peak-memory cut —
    relevant to the vector channel's own open items (V-phase), ideas only, and
    pg_turbovec is Rust so this is design inspiration, not code.

No code was copied. zvec remains ideas-only (Apache-2.0, incompatible with the
contrib-track PostgreSQL licence; see `doc/LICENSING.md`).

**Next check:** when the retirement ledger (Tasks 5-8) lands, or sooner if a
sibling ships a VACUUM/merge/recovery P0, since that is the area pg_weave is
actively changing.
