# Gap analysis: what pg_weave must beat, where it loses, and why

Written 2026-09-06 from measurement, not from inherited notes.
Source data: `bench/RESULTS_LEXICAL.md`, `bench/RESULTS_BOUND_PRUNING.md`,
`bench/aws/RESULTS_EC2_SMOKE.md`.

## 1. The target, stated precisely

The goal is to be **demonstrably better than the separate-extension stack** —
pgvector + (pg_fts or pg_textsearch) + (pg_trgm or pg_tre) — on features,
correctness, latency, p99, and index size. That stack is free, finished, and
already installed, which makes it the only competitor that matters for adoption.

Two dimensions are deliberately **not** targeted, because pursuing them would
require abandoning the thesis that makes pg_weave worth building:

- **Beating turbopuffer at scale and storage cost.** They run 1T+ documents over
  3 PB with S3 as the system of record. pg_weave is an index on a Postgres table
  with a 128-segment metapage cap. Reaching a trillion documents means becoming a
  separate system, at which point the transactional-consistency argument — the
  entire reason to prefer pg_weave — evaporates. `doc/COMPETITIVE.md` keeps that
  boundary honest.
- **Beating GIN on index size for unanchored cross-token substring search.**
  GIN-over-corpus-trigrams is close to optimal for that job. See
  `doc/specs/FUZZY_CHANNEL.md` §6.

Everything else is in scope and every loss below is treated as a defect.

## 2. Measured losses against tsvector + GIN

From `bench/RESULTS_LEXICAL.md`, 1M documents, r6id.4xlarge, PostgreSQL 17.

| # | gap | measured | target |
|---|---|---|---|
| **G13** | ranked latency at k=10 | **NARROWED by L17 2026-09-10 (`bench/RESULTS_L17.md`).** rare **2.82 → 1.51 ms (1.20× behind** pg_textsearch, from 2.25×), mid **10.26 → 6.19 (3.81× behind**, from 6.31×), common 15.35 → 14.92 (4.75× behind, from 4.86×). At k=100 pg_weave now WINS rare by 3.96× and mid by 1.27×. Cause was measured, not assumed: the doclen sidecar cursor was 72% of a ranked scan because its gap-coded docid column had to be unpacked and prefix-summed (128 entries) to serve ~2.6 candidates. v5 stores absolute offsets from each block's `first_docid`, making the column randomly addressable, so a lookup is a `weave_for_get` binary search — cursor now 45.2%, index +0.16%. The win scales with candidate stride, so `common` (stride ~1.15) is flat by construction and is now L2's target. | ≤ 2× pg_textsearch at k=10 — **met for rare**, open for mid/common |
| ~~**G12**~~ | ~~boolean NOT~~ | **CLOSED.** `count(*) WHERE 'common & !rare'` 7008 ms → **14.05 ms** (499×) by building the NOT universe lazily. Now **beats GIN's 141 ms by 10×**. | done, and a win |
| ~~**G1**~~ | ~~bare `ORDER BY <=> LIMIT` does not use the index~~ | **CLOSED by L7.** 83 ms → **0.05 ms** (1,662× par4, 7,248× serial). Now beats GIN by 1,615×/7,080× on the same form. | done |
| ~~**G2**~~ | ~~index size~~ | **CLOSED by L8/L10.** The loss was a measurement artifact: 70.7% of the file was freed pages. Live content is **46 MB vs GIN's 81 MB — 1.76× smaller.** | done, and a win |
| **G3** | ranked latency on rare terms (df 25) | 0.05 ms vs 0.03 ms — **1.7×** | ≤ GIN |
| **G4** | ranked latency on mid terms (df 2.5k) | 3.54 ms vs 2.06 ms — **1.7×** | ≤ GIN |
| **G5** | build time | **NARROWED AGAIN by L15: 328.0 s → 192.5 s (1.70×; 1.82× under identical `DO_PROFILE` conditions).** Now **3.91×** behind pg_textsearch (49.2 s) and **0.95×** of GIN (202.7 s) — a tie-to-win on GIN pending a reproducing run, since GIN itself swung 15% between runs on unchanged code. Index size unchanged at 625 MB, ranked latency within ±4% both directions. The profile's "37.5% dynahash" was right about the symbol and wrong about the cause twice: hashing/`memcmp` of the 64-byte key was worth 3.9%; the merge's per-term hash (probed once per posting into a one-entry table) was worth 19.5%; and the largest single cost was a hash the profile never named — the doclen sidecar collector, a `uint64`-keyed dynahash `HASH_ENTER`'d once per posting (~240M) in both the writer and the merge. Replaced by a per-heap-block radix map. No hash remains on the per-posting path. `bench/RESULTS_L15.md` | ≤ GIN — **met in one run, not yet reproduced** |
| ~~**G6**~~ | ~~index size is non-deterministic~~ | **CLOSED by L8.** as-built 46 MB, compacted 46 MB, swing **0.0%**; `weave_merge`/`weave_vacuum` both return false on a fresh build. | done |

Where pg_weave already wins, and by how much, so the wins are not lost in the
list of gaps: ranked common-term **8.2× (par4) / 19× (serial)**, `count(*)`
**595×**, prefix `count(*)` **3.8× / 7.7×**, plus BM25/BM25F, phrase, NEAR,
fuzzy, and regex which GIN does not have at all.

## 3. Diagnosis

### G1 — the bare `ORDER BY` form generates no index path — **CLOSED**

**Resolved 2026-09-06 by task L7.** Root cause was `amoptionalkey = false`;
setting it true was the entire fix. Confirmed by measurement: 83.09 → 0.05 ms
par4, 362.38 → 0.05 ms serial, with no other measurement moving. The hazard it
introduced (Index Only Scan over a NULL-skipping index for an unqualified
`count(*)`) is contained by a prohibitive cost plus a runtime rejection. Original
diagnosis retained below.

**Root cause.** The AM's ordering path (`weave_gettuple`) is only reached when the
planner picks an `Index Scan ... Order By`, and that currently requires *both* a
stored `wdoc` column *and* a `WHERE d @@@ q` restriction clause alongside the
`ORDER BY d <=> q`. With no restriction clause the scan would have zero scan keys,
and the AM does not support a keyless ordering scan — so no index path is
generated and the planner falls back to Seq Scan + top-N Sort.

pg_weave inherited this from pg_fts, where it is documented in a regression-test
comment as a constraint rather than a bug. That is the wrong call. pgvector
supports exactly this shape (`ORDER BY embedding <=> $1 LIMIT 10`, no `WHERE`),
so it is the shape every user writes first, and the failure is silent.

**Fix.** Support a scan with zero restriction keys and one order-by key: the AM
must be able to rank the entire corpus by `<=>` and return the top rows in order.
Mechanically this means `weave_beginscan`/`weave_rescan` accepting `nkeys == 0`
with `norderbys == 1`, and the candidate-generation path deriving its term set
from the order-by argument instead of from a restriction key. The block-max WAND
machinery already ranks by the query's terms; what is missing is the entry point
that does it without a matching `@@@` key.

**Risk.** The ranked path over fuzzy/prefix/regex expansions is documented as a
correct-but-possibly-incomplete subset. A keyless ordering scan must not silently
widen that gap, so it needs the same `@@@`-parity assertion the existing
regression tests apply.

### G2, G6 — index size

**Root cause, partly known.** The v4 doclen sidecar cut index size 4.7× on the
Wikipedia corpus (1421 MB vs 6729 MB) so the storage design is not naive. The
remaining 1.7× against GIN is not yet attributed, and attributing it is the first
task, not guessing:

- GIN stores no term frequencies, no document lengths, and no positions by
  default. pg_weave stores tf per posting and a quantized doclen byte per document
  because BM25 needs them. Some of the 1.7× is the price of ranking correctly and
  is not recoverable.
- Some of it is FOR bit-packing being less dense than GIN's varbyte + posting-tree
  compression on low-cardinality lists.
- Some of it is per-segment fixed overhead (dictionary, sparse block index,
  livedocs, doclen sidecar) multiplied by the segment count.

**G6 is separable and immediate.** The 35 % swing is `weave_merge()` /
`weave_vacuum()` not having run. Two things follow: the harness must stop
tolerating failure of those calls, and more importantly a freshly built index
should not need a manual compaction step to reach its natural size. Autovacuum's
cleanup path does fold pending documents in, but `CREATE INDEX` followed by a
query should not be 35 % larger than the same index after manual maintenance.

**Fix order.** Measure the size breakdown per structure first (a
`weave_index_size_detail()` function), then attack the largest attributable
component. Publishing a size number that swings 35 % on operator behaviour is
worse than publishing the larger number.

### G3, G4 — rare and mid ranked latency

**Absolute magnitudes are small** — 0.02 ms and 1.5 ms — but they are losses, and
they are the queries a search application runs most.

**Likely root cause, not yet confirmed.** pg_weave does strictly more work per
matching document than GIN: it decodes tf, looks up a quantized doclen, and
evaluates the BM25 saturation function, where GIN's bitmap scan just collects
TIDs and `ts_rank` runs afterwards on the heap tuple. For a 25-document match that
per-document work is irrelevant; the 1.7× is therefore probably **fixed per-scan
setup** — segment iteration, dictionary lookup per segment, cursor construction,
livedocs deserialization — amortized over very few documents.

**That hypothesis is testable and must be tested before any optimization**: the
same query at df 25 and df 2,503 costs 0.05 ms and 3.54 ms, a 70× ratio for a
100× document ratio, which is consistent with a small fixed cost plus linear
per-document work. Profile with `perf` on the EC2 host and attribute the 0.05 ms
before touching code.

**Candidate fixes, in order of expected value:** reduce per-segment setup by
keeping the index compacted to one segment (which also fixes G2/G6); cache the
per-segment dictionary lookup across scans in the relcache the way the doclen
page directory already is; and skip livedocs deserialization when a segment has
zero tombstones.

### G5 — build time

After L12 and L15, 192.5 s against pg_textsearch's 49.2 s and GIN's 202.7 s on
`synth-2m-long`. What remains is not a hash: the post-L15 profile's top three are
the merge's k-way term comparison (17.4% self), `weave_write_postings` FOR packing
(14.7%), and the scan-side term hash in `add_posting` (14.3%, ~28 s — the only
dynahash left; `simplehash.h` might recover a third of it and is not the next
thing). Parallel merge (L4) and parallel scan (L11) are withdrawn on upstream
evidence. `bench/RESULTS_L15.md`.

### G14 — a single-segment index never reclaimed deleted space — **CLOSED by L18 2026-09-14**

**Renumbered from G12, which was already taken** by the boolean-NOT gap in the
table above. The collision shipped for a day; two different gaps under one id in
one file is how a closed gap gets cited as evidence for an open one.

Delete 90% of a 120,000-row corpus, `VACUUM`, then `weave_vacuum()` twice, and the
index went **2289 → 2296 pages**: it grew by 7 and reclaimed nothing.
`weave_vacuum()` returned **false** both times and `weave_index_nsegments()` was
**1**. Since insert-time tiered merge drives every index toward exactly one
segment, **the steady state of a weave index was the state that could not
reclaim**, and the only recovery was `REINDEX`.

**Fixed:** 2289 → **264 pages**, true on the first call and false on the second.
264/2289 is 11.5% of the file for 10% of the rows — nearly proportional, so the
reclaim is essentially complete. Converges in one pass, because the rewrite emits
a segment with `ndeleted = 0`.

**THE MECHANISM RECORDED HERE WAS WRONG, and that is the useful part.** This entry
used to say compaction was "implemented as a *merge* of segments, and a merge of
one segment is a no-op guarded by an `nsegments > 1` precondition". All three
clauses were false. `weave_merge_selected()` has no such precondition,
`weave_compact_to_one()` already calls it with `nsel >= 1`, and
`weave_merge_segments_streaming()` already drops tombstoned postings per source.
**The rewrite machinery was complete and correct the whole time; nothing ever
asked it to run.**

The real cause: every term in `weave_index_is_compacted()` — the floor guard
`weave_vacuum()` consults before deciding a rewrite would be waste — is a
statement about **free space** or segment count, and **a tombstone is neither**. It
is a live, allocated, fully-packed page holding a posting no scan can see. A
segment that is 90% deleted is perfectly front-packed with nothing to coalesce, so
the guard called it compacted. The whole fix is a third term teaching that
predicate what a tombstone is, gated by `pg_weave.vacuum_tombstone_frac`
(default 0.2, **not measured** — the frontier has not been swept and the right
value depends on segment size).

Had the diagnosis in this file been trusted, the work would have been a redesign
of the merge instead of eight lines. **A wrong mechanism in a gap entry is more
expensive than no mechanism**, because it is specific enough to act on.

**Why no test caught it.** `t/008_vacuum_reclaim.pl` existed for exactly this
requirement — its header says "must SHRINK a bloated index, not grow it" — but it
ran `weave_vacuum()` on a **freshly built index with no deletes**, where after
L8/L12 the file is already compact and the call correctly does nothing. Its
assertions were `v1 <= built` and `v2 <= v1 + 1`, both satisfied by doing nothing.
It could catch a vacuum that *grew* the index, which is the bug it was written for,
and could not catch one that never shrinks.

The delete-then-reclaim arm that found this came from reviewing **pg_fts `2605d00`**,
which hit the growth half of this bug class (35 → 52 → 69 MB across cleanups with
no rows added), fixed it by skipping a compaction pass whose free space is not yet
reusable, and then had its own new test catch that fix degrading into never
reclaiming (18 → 22 MB) because a stale free-space map overstates the live size.
The lesson transferred even though the root cause did not: **a no-growth assertion
cannot see a no-reclaim regression, and a fix for one produces the other.**

**RETRACTED 2026-09-14, the day after it was written: "the autovacuum path does
not reclaim tombstones."** That claim was measured on a test cluster with
`autovacuum = off` and no other activity, so **nothing consumed transaction ids**.
`weave_free_page()` stamps `ReadNextTransactionId()` on a freed page and
`weave_page_recyclable()` asks `GlobalVisCheckRemovableXid()`, so with a stalled
horizon no page ever becomes recyclable and no reclaim is possible — an artifact of
the harness, not a property of the code. Advance the horizon and plain `VACUUM`
reclaims: **1093 → 94 pages** on the first cycle, then 94 for the next four, with
73 low-bias page reuses. `t/008`'s "264 → 267 → 267, no reclaim" arm was reading
the same artifact.

The general error is one this file keeps recording: **a measurement taken in
conditions the production system does not have is not a measurement of the
production system.** An idle cluster does not advance its xid horizon, and every
visibility-gated mechanism in PostgreSQL depends on that horizon moving.

**What was actually wrong was narrower and pre-existing — see G18.**

### G15 — eight dictionary page walks had no bounds check — **CLOSED 2026-09-14**

Eight loops stepped over variable-length `WeaveDictEntry` records taking their end
bound from a raw `pd_lower` and their stride from an untrusted on-page
`de->termlen`. On a recycled or corrupt page — possible because these pages are
read under only `BUFFER_LOCK_SHARE` — the walk leaves the page. Ranked by
consequence:

1. `merge_source_load_page()` (`src/am/ambuild.c`) — **the counting pass SIZES two
   allocations and bounds a second pass that WRITES.** This shape has been hit in
   the field at scale as `invalid memory alloc request size 3406063183` (~142M
   entries where an 8 kB page holds a few hundred), and because the function sits
   under the streaming merge it killed every merge, every autovacuum cleanup and
   every explicit vacuum: **the index could never be vacuumed or reclaimed again.**
   Provenance: pg_fts 1.7.0, investigating a 2.87M-doc email-body index.
2. `weave_segment_docids()` (`src/am/amvacuum.c`) — runs on **every** VACUUM and CIC
   validate, and feeds a garbage `de->df` to `weave_decode_term()`.
3. `weave_dict_seek()` (`src/am/amscan.c`) — the hottest term lookup in the AM; a
   garbage `ie->blk` is then read as if it were a dictionary page.
4. `weave_free_segment()`'s two walks (`src/am/am.c`) — a garbage `de->firstposting`
   goes to `weave_free_chain()`, which would mark an arbitrary block chain free.
   **Freeing live pages from a corrupt read is the worst outcome in that file.**
5. `weave_anomalous_docs()`'s two passes (`src/am/amscan.c`) — SQL-reachable by any
   user.
6. `trgm_page.c`'s dictionary and trigram walks.

**Do not port a fix for this as "make the allocation huge-safe".** Upstream treated
it as two findings and did both, but its 2.5 GB request came *from* the unvalidated
walk — routing a garbage-derived size to `MemoryContextAllocHuge` turns a clean
error into a 2.5 GB allocation and a garbage walk, which is worse. Validation and
huge-safety fix different bugs. Our doclen loader was already guarded, so its size
is honest and corpus-scale, and *that* one wanted huge-safety (see G16).

**Fixed** by `weave_page_entry_end()` (validates `pd_lower` as an integer before a
pointer is formed from it — `page + pd_lower` for an out-of-range value is UB *at
formation*, so a guard written as the pointer comparison `ptr < (char *) page +
pd_lower` is itself the bug it means to prevent) and by promoting
`weave_dict_entry_fits()` from `static` in `amscan.c` to `include/weave/am.h`. That
helper was already correct and already used at seven `amscan.c` sites; **it was
`static`, so the four other files that needed it each walked unguarded instead.**
A guard only one translation unit can reach is how eight walks went without one.

`test/fuzz/fuzz_dictwalk.c` pins both hazards with planted-bug builds that must
abort. A third planted bug was written and **exited 0**: it removed the second
walk's `n < cap` bound on the rationale that the two passes might see different
bytes, and that rationale is false — the share lock is held across both passes. The
bound stays as belt-and-braces and is documented as not load-bearing.

**`src/am/amcheck.c` is deliberately excluded** from the `pd_lower` clamp: it
reports corruption for a living, and clamping there would hide what it exists to
find. Recorded here so the inconsistency is a decision rather than an oversight.

### G16 — the allocation lint reported safety it had not checked — **CLOSED 2026-09-14**

`ci/check-alloc.sh` had said "no unguarded corpus/vocabulary-scale allocations" on
every commit for months. It matched an enumerated allowlist of size-variable
**names** — `df`, `sumtf`, `ndocs`, `nposts` and eighteen more — and that list grew
one name at a time as each was hit (`parena_cap` and `ocap` are in it because
someone hit exactly those two). That is the same find-one-function-at-a-time mode
the lint was written to replace, and it left the codebase's **dominant** allocation
pattern invisible: the doubling capacity variable.

A sweep for that idiom found **43 sites the lint could not see.** Eight are
genuinely corpus-scale. **Four are reachable from VACUUM or merge**, which is the
part that matters — a throw on a query path loses one query, a throw inside
`weave_bulkdelete` or the streaming merge means every vacuum fails and the index can
never be reclaimed again:

- the doclen resident array (every docid in a segment, ~134M to overflow), reached
  from `merge_source_open()`;
- `weave_bulkdelete()`'s `carry` and `newdead` tombstone arrays;
- the dict-page bookkeeping trio in the dictionary writer, reached from
  `weave_vacuumcleanup()` via the streaming merge;
- `dict_spill_next()`'s term buffer, where **`tcap` was an `int` and the doubling
  itself was the bug**: a term length is bounded by the document (1 GB), so
  `tcap * 2` exceeds `INT_MAX`, signed-overflows, and a negative int reaching
  `palloc` becomes an enormous `Size`.

Also `doc.c`'s per-document parse arrays, where the "bounded by one document"
defence does **not** hold: the 8-byte pointer array overflows at ~134M terms while
the minimal ~6-byte-per-token encoding lets a single 1 GB literal carry ~179M
tokens, so a maximal literal overflows the array before reaching the document
ceiling `doc.c` already enforces.

The lint now matches the **idiom** rather than a list of names, and deliberately
over-matches: a bounded `cap` costs one `alloc-ok:` annotation, a missed one costs
an unvacuumable index. Nine genuinely bounded sites carry that annotation with the
bound stated, so the next reader need not re-derive it. Teeth verified by planting a
plain `palloc(mycap * ...)` and confirming it fails — **because a lint that goes
green on its first run is precisely what the old one did.**

### ~~G17~~ — one WAL record per freed page — **WITHDRAWN 2026-09-14, the evidence was retracted upstream**

Recorded earlier on 2026-09-14 citing an upstream measurement of ~14 ms/page (a
3.8 GB index, ~489k pages, a vacuum running 113+ minutes without finishing).
**Upstream retracted that number the same day.** Re-measured there: 8,686,917 pages
freed in 46 seconds = **0.005 ms/page, ~2,800× cheaper** than published, confirmed
by a second run (7,912,288 pages in 33 s). There is no per-page WAL problem and no
batching is needed.

The original number came from a stack sample taken on an index that had **already
hit the allocation bug repeatedly**, so the backend was working on a damaged state.
`gdb` showed the process inside the page-free function, and that was turned into
"this function is the bottleneck".

**The lesson is why this entry is kept rather than deleted: a stack sample gives a
LOCATION, not a RATE.** To claim a cost you need pages per second, not a frame. The
second-order error is ours: G17 was written here, and task L21 into
`doc/PHASES.md`, on inherited evidence never measured against our own code. Both
are withdrawn. If the free path ever looks slow here, the first step is a rate.

### G18 — a relocation pass under a share lock ratcheted the index upward — **CLOSED by L19 2026-09-14**

Pre-existing, and found only because the allocator counters (G19) made the branch
visible. Once `weave_vacuumcleanup()`'s free-page trigger fires — which a bulk
ingest guarantees, since every tiered merge frees its inputs — the vacate+pack
relocation runs under `ShareUpdateExclusiveLock`, can reuse **nothing**, and
extends. The pages it just freed keep the trigger satisfied, so it repeats:

| cycle | pages | `lowfree_reuse` | `lowfree_defer` | `extend` |
|---|---|---|---|---|
| start | 1022 | | | |
| 1 | 1166 | 0 | 1001 | 144 |
| 2 | 1239 | 0 | 1092 | 73 |
| 3 | 1312 | 0 | 1165 | 73 |
| 4 | 1385 | 0 | 1238 | 73 |
| 5 | 1458 | 0 | 1311 | 73 |

`lowfree_reuse = 0` with `lowfree_defer` = every candidate is the whole diagnosis.
`weave_page_recyclable()` bypasses `GlobalVisCheckRemovableXid()` only under
`AccessExclusiveLock`; under a share lock a concurrent scan may still hold a
directory snapshot referencing those pages, so the gate must stand. Worse, the
rejected-candidate list grows every cycle, so the scan gets slower as the file
gets bigger.

**Attributed, not assumed:** the identical ratchet appears with L18's tombstone
term disabled (`vacuum_tombstone_frac = 1.0`), so L18 did not cause it. It also
requires a **stalled** xid horizon; with the horizon advancing the same sequence
reclaims (1093 → 94) and is stable. So the trigger condition is "free pages > 25%
of the file **and** an idle database" — narrow, but real: a read-mostly system with
periodic deletes and a scheduled `VACUUM`.

This is the shape the sibling project has open as a ~210× transient bloat with the
cause listed as unknown. The cause here is the recycle gate plus a self-sustaining
trigger.

**Fixed** the way that project fixed the growth half: `weave_vacuum_compact()`
probes, when it does not hold `AccessExclusiveLock`, whether any currently-free
page is recyclable, and if none is, truncates any free tail and stops. A pass that
cannot pack can only extend, so doing nothing is strictly better.

| | before | after |
|---|---|---|
| stalled horizon | 1022 → 1166 → 1239 → 1312 → 1385 → **1458** | 1022 → 1093 → 1093 → 1093 → 1093 → **1093** |
| advancing horizon | — | 1093 → **94**, then flat (11.6×) |

**The obvious way to get this wrong is to skip forever**, which is exactly how that
project's own fix degraded (18 → 22 MB, never reclaiming, because a stale
free-space map made the pass look unnecessary every time). Two defences: the probe
asks about **recyclability**, a property of the freeing xid which advances on its
own, rather than about free space; and `t/015` **requires a shrink** on the
horizon-advancing arm, so degrading into permanent skipping fails a test rather
than silently stopping work. The probe is bounded to 256 candidates; the bound can
only cause a false negative (skip a pass that would have worked), which
self-corrects next cycle — the safe direction, since a false positive starts a
relocation that can only grow the file.

### G19 — nothing could say which allocation path ran — **CLOSED 2026-09-14**

Not a bug in the index; a bug in what could be known about it. Every page pg_weave
allocates comes from the compaction low-bias list, the free space map, or a
relation extension, and until now nothing reported which. **Three separate
investigations in this lineage stalled on exactly that, and all three acted on a
guess:**

1. L18 asserted the recycle gate was blocking reuse under a share lock. Measured:
   `defer = 0`. The gate was never consulted, because the trigger never fired.
2. L18 asserted autovacuum could not reclaim tombstoned space. Measured on a
   cluster that could not advance its xid horizon; false in production conditions.
3. The sibling project published a per-page WAL cost as the reason freeing a
   segment was slow, then retracted it — G17 above.

`weave_alloc_stats()` reports seven counters: `lowfree_reuse`, `lowfree_defer`,
`lowfree_contended`, `fsm_reuse`, `fsm_defer`, `fsm_contended`, `extend`. The
discrimination this buys is the one reasoning kept failing at: **`extend` high with
`defer` high means the recycle gate is the constraint; `extend` high with `defer`
zero means the free list was never consulted at all** — a different bug with a
different fix.

Always compiled in (one increment per `ReadBuffer`) and read from SQL rather than
logged, both deliberate: a build flag means the numbers only exist in a binary
nobody is running, and `log_min_messages = warning` silences `elog(LOG)`, which
cost the sibling project a whole measurement run.

### G20 — bulk ingest of term-rich documents inflates the index ~237x — **mechanism corrected and addressed 2026-09-18; measured 2026-09-14, mitigated 2026-09-16**

Found by taking the sibling project's field shape (~1,660 terms per document,
thousand-document batches, no maintenance in between) and running it against us
instead of assuming our merge policy made us immune. It does not.

**Measured**, 4-bit irrelevant here, PG 17, `autovacuum = off`, one session
throughout, `bench/RESULTS_INGEST_AMPLIFICATION.md`:

| arm | `fsm_reuse` | `fsm_defer` | `extend` | pages per doc |
|---|---:|---:|---:|---:|
| 1,000 docs per transaction, batch 3 | 76 | 18,924 | 96,415 | 96 |
| 1,000 docs per transaction, batch 4 | 19 | 18,981 | 117,032 | **117** |
| one doc per transaction, 200 docs | 2,793 | 1,007 | 13,407 | 67 |

6,000 documents reach **4,293 MB**; 4,200 documents reach 385,295 pages and one
`weave_vacuum()` returns them to **1,624 pages — a factor of 237**. A document's
1,660 postings do not fill a single page, so 117 pages extended per document is
write amplification of order **100x**, against the sibling project's measured
12-15x on the same corpus shape.

**Mechanism, discriminated by the counters rather than reasoned about.**
`fsm_defer` sits at ~19,000 per batch with `fsm_reuse` at 19-76: the free list *is*
consulted, ~19,000 candidates are found, and essentially every one is **rejected**
by `weave_page_recyclable()`. The inserting transaction freed those pages itself
via its own merge, so `GlobalVisCheckRemovableXid()` cannot clear them while that
transaction runs. Reuse is **0.02-0.08%**, not the sibling project's 0.3%.

**That reading was half the picture, and the missing half is the one that mattered.**
The counters above are the *segment writer's* allocations. The merge's own output
allocations appear in none of the three reuse columns, because
`weave_merge_segments()` ran the whole loop `extend_only`: it consulted no free list
at all, neither for pages it had just freed (the hazard extend-only guards) nor for
pages freed by earlier, committed calls (safe, and the vast majority — at 1,660
terms/doc the merge is the dominant writer). The entry originally concluded that
"in-transaction reuse is impossible by construction" and that "the real fix moves
the merge out of the inserting transaction — a design change, not a point edit".
The first clause is true of pages freed inside the current transaction and the
recycle gate must keep refusing those. The conclusion was wrong: the sibling
project falsified the horizon explanation by measurement — a fresh transaction per
row moved its pages-per-document figure by only **1.4x** (49.2 bulk vs 34.3
single-txn, both ~70-100x their compacted size) — and then closed its equivalent
gap with a point edit to the allocator.

The trigger is `weave_insert_oversized_as_segment()` calling
`weave_merge_segments()` after **every** document. Any document whose analyzed
`wdoc` exceeds one pending page takes that path, and at 1,660 terms every document
does, so each insert mints a one-document segment and immediately rewrites a
level-0 run.

**Three things this is NOT, each checked:**

- Not a segment-count runaway. `nsegments` stayed at 7-8 in every arm, including
  the one-row-per-transaction arm. Our leveled compactor is doing its job; the
  sibling project's field report (8 -> 128 segments, then unable to merge or
  VACUUM) does not reproduce here. The cap is not what is at risk.
- Not G18. G18 was the vacuum-path relocation ratchet and needed a stalled xid
  horizon. This is the insert path, and it happens with the horizon advancing.
- Not fixed by shortening transactions. One transaction per document raises reuse
  to 17.2%, an order of magnitude better and still overwhelmingly extension.

**My own artifact, recorded because it is the same one that has now bitten this
lineage four times.** The first run wrapped all six batches in a single `DO` block,
which is one transaction, so the horizon could not advance between batches and
every candidate was trivially unrecyclable. Re-running with one transaction per
batch changed reuse from 0.00% to 0.02-0.08% — that is, the artifact was real and
did **not** materially change the answer, which is luck rather than method. I also
selected the `reuse` and `extend` columns and omitted `defer` on the first pass,
which is precisely the discrimination G19 exists to provide.

**Mitigated 2026-09-16 (the rewrite-rate half); still open at that date.** The
sibling project (pg_fts 1.7.2, commit
c41e319) gates the insert-time merge on there being a fan-out's worth of small runs
waiting, and calls that a mitigation rather than a fix because the freed pages still
cannot pass the XID gate inside the inserting transaction. That gate is now ported:
`weave_small_runs_worth_merging()` in `src/am/ambuild.c` counts smallest-level runs
under a share lock and defers the merge until there are `WEAVE_MERGE_FANOUT` of them,
so a rewrite is amortised over a fan-out's worth of documents instead of paid per
document. Measured A/B, raw numbers and the exact commands in
`bench/RESULTS_G20_MERGE_GATE.md`: 6,000 documents at 1,660 terms each go from
550,896 to 353,181 pages (4,303 MB to 2,759 MB) — **-35.9 %, one run per arm** — and
the page count after one `weave_vacuum()` is **2,029 in both arms**, so the gate
changes the scratch space the ingest burns, not the index it leaves behind. Under one
transaction per document the reduction is larger (271,083 -> 127,248 pages, -53.1 %),
and the two arms must not be compared: arm A's freed pages are refused by the recycle
gate (`fsm_defer` ~113,000) while arm B's xid horizon advances between commits
(`fsm_defer` 0). **The gate did not improve page reuse and could not** -- arm B's
`fsm_reuse` is 75,848 in *both* arms and only `extend` moved. It reduces the amount of
work, which is the whole of the claim.

**What this entry previously claimed, and why it was wrong.** It said "our compactor
already no-ops when no level is over capacity, so that particular gate buys us less
than it bought them". Both halves are wrong. `weave_merge_segments()` has a *second*
trigger — no level over capacity but `meta.nsegments > WEAVE_MERGE_THRESHOLD`
compacts the lowest level with two or more runs — so it does not no-op below the
fan-out threshold, and the gate is therefore not behaviour-neutral. The measurement
that shows it: max live segments over 4,000 single-row transactions moved from 8 to
15. And it did not buy us less: the before/after page counts above are on the same
corpus shape the sibling project measured. The prediction was a plausible inference
from the code, made without running the arm, and it was wrong in both direction and
magnitude. (The same secondary trigger exists upstream, so the upstream rationale was
also incomplete; upstream's measured max of 15 is the same evidence read as a
success.)

**The safety property this puts at risk, and its measurement.** The eager merge exists
because a field deployment went 8 -> 128 segments in ~1h and could then neither merge
nor VACUUM. Deferring the merge lets the directory sit higher between merges, so the
worst case for segment minting was measured directly — one oversized row per
transaction, 4,000 transactions, `weave_index_nsegments()` sampled after every commit:
**max 15 live segments** against `WEAVE_MAX_SEGMENTS` of 128 (max 8 without the gate).
`t/007_segment_cap.pl`'s 4 concurrent inserters, a different write pattern entirely,
independently peak at 15 as well.
The bound is structural, not incidental: level 0 holds at most `WEAVE_MERGE_FANOUT - 1`
runs before the gate opens, and a merge invocation runs its convergence loop until no
level qualifies, so the count is bounded by ~`WEAVE_MERGE_THRESHOLD +
WEAVE_MERGE_FANOUT`. `t/007_segment_cap.pl` now asserts `nsegments <= 64` as well as
`<= 128`, following the same reasoning upstream used to tighten its own: 128 is the
hard cap, and an assertion that only fires there fails for the first time when the
index is already unrecoverable.

**Addressed 2026-09-18 by the snapshot allocator, and the mechanism this entry
named is the one that was fixed.** `weave_merge_segments()` and `weave_merge_all()`
now allocate in SNAPSHOT mode (`weave_alloc_snapshot_enter()`,
`include/weave/am.h`): the free list is gathered **once at scope entry, before the
call frees anything**, the loop hands out only from that snapshot and then extends,
and it never re-consults the live FSM. A page freed by merge N cannot reach merge
N+1 — it is not in the snapshot — so the recycle-race guard extend-only
over-approximated is now exact, while pages freed by earlier calls are reused.
Raw before/after page counts, one measurement per arm per run and no arithmetic,
are in `bench/RESULTS_G20_SNAPSHOT_ALLOC.md`; the arithmetic and any claim belong
to whoever reads that file.

Two things it does **not** fix, recorded so the entry is not read as closed:

- The single-large-statement shape. Inside one statement the freeing xid *is* the
  horizon, so no page freed during it can pass the recycle gate before it ends, and
  no allocation policy changes that. `weave_vacuum()` after a bulk load remains the
  guidance for that one shape.
- `fsm_defer` for the *segment writer's* allocations in a long transaction. The
  gate still refuses those, correctly.

Two prerequisites had to land first, and did, as separate commits: every merger is
now serialized under the maintenance mutex and that is enforced by
`weave_assert_merge_serialized()`, and the parallel merge's commit now frees only
sources it confirmed present. Without the first, two mergers reusing pages draw
from one free list and hand out the same block — which is how the sibling project
turned this fix into a deadlock and then had to find the two unserialized sites.

Corrected as part of the original measurement: the comment on
`weave_insert_oversized_as_segment()` said oversized documents are "Rare, so building
a whole segment per such document is acceptable", 50 lines above another comment in
the same function saying "EVERY insert lands here and mints a segment" for the common
case of a body index. Both cannot be true, and the measurement says the second one is.

### G21 - `t/014` reported a leaked page after double crash recovery, once - **OPEN, NOT currently reproducible, mechanism unknown 2026-09-15**

`t/014_merge_durability.pl` test 9 failed once with `weave_check()` reporting **"1
unreachable page(s) not flagged freed; first is block 2883"** after the second
crash-recovery cycle. Test 7, which asserts the relation size survived that crash, passed
in the same run - so the file was the right length and one page inside it was orphaned.

**Reproducer**: none. It was seen in the full nix TAP leg,

    nix build .#checks.x86_64-linux.tap-pg17 --rebuild -L

and has not been seen since -- see the table below before spending time on it.
`--rebuild` is required for any attempt: a plain `nix build` returns a cached success
without re-running. See the warning further down before trusting any result from it.

**The rate is not known, and the "1 run in 8-10" this entry used to claim is withdrawn.**
That figure came out of the same contaminated series the correction below describes -- the
one in which six of eight runs never executed the test. What survives the correction is
that the failure was *observed*, not how often. Runs since, every one carrying evidence
that `t/014` actually ran:

| series | runs | failures |
|---|---:|---:|
| full `tap-pg17` leg, `--rebuild --keep-failed` | 16 | 0 |
| the full `t/0*.pl` list, same order, outside nix | 20 | 0 |
| `t/014` alone, driven by `prove` against the same binaries the leg builds | 100 | 0 |

The three lines say different things and the differences are the finding.

- **The file alone is not a reproducer.** At one failure in nine, 100 clean isolated runs
  have probability 8e-6.
- **Neither is the file list, sandbox or no sandbox.** Combining the two full-sequence
  series -- 36 runs, 0 failures -- puts P(all clean | one in nine) at 1.4 %, and the
  95 % upper bound on the per-run rate at **8 %**. So the rate is not 1 in 9; it may
  well be 1 in 50 or 1 in 100, which no affordable loop distinguishes.
- The nix sandbox is not the trigger either: a sequence outside it costs 2 m 55 s
  against the leg's 2 m 58 s, so the two series are the same experiment minus the
  sandbox, and both are clean.

**Disposition: not hunted further until it recurs.** There is no reproducer, and
continuing to buy 3-minute lottery tickets at a rate bounded above by 8 % is worse value
than the open work in `doc/PHASES.md`. What replaces the hunt is the instrument below:
the next occurrence -- in CI, on a workstation, anywhere -- prints what the page is
instead of just that it exists. That is the whole reason to build the reading before
chasing the bug.

**What is established:**

- It reproduces on the current committed tree, with `t/014` definitely executed, and
  reports the same block number every time it appears: 2883.
- **Block 2883 is the single page `weave_vacuum()` appends.** Reproduced by hand: after
  the first crash the relation is exactly 2883 pages (blocks 0..2882), and
  `weave_vacuum()` extends it to 2884. So the orphan is whatever that one appended page
  is, and in the passing case the same page ends up reachable.
- It is **not caught by CI**, which is green and does run this file (the TAP leg is
  `make installcheck REGRESS= ISOLATION=` with `TAP_TESTS = 1`).
- It is **not** reproducible by any deliberate sequence I could construct by hand: a deep
  check after `weave_merge()`, after `weave_vacuum()`, after either followed by an
  immediate crash, and after crashing *during* `weave_vacuum()` at eight different offsets
  from 0.05 s to 0.8 s, are all CLEAN. `weave_vacuum()` completes in under 50 ms on this
  corpus, so a sleep-based crash cannot land inside it.
- Synthetic load (12 spinners, load average 14.6) does **not** make it appear.
- Killing the postmaster **during a plain `VACUUM`** - which is what invokes
  `weave_vacuumcleanup()`, the autovacuum path the hypothesis below implicates - is
  CLEAN at eight offsets from 0.02 s to 0.5 s. So either the window is narrower than a
  sleep can hit, or the cleanup path is not where the page is lost. Recorded so the
  next attempt does not repeat it.

**What is NOT established, and is the whole diagnosis:** what kind of page block 2883 is,
and why it is sometimes linked and sometimes not.

**The instrument for reading it now exists** (was "next step 1"): `weave_page_info(idx,
blkno)` in `src/am/amcheck.c` reports per page its kind, raw header flags and kind id,
freed/uninitialized/reachable state, `nextblk`, LSN and free bytes. The leak query is

    SELECT * FROM weave_page_info('md_weave')
     WHERE NOT reachable AND coalesce(freed, false) = false AND NOT uninitialized;

and `t/014` now runs exactly that after each recovery and `diag`s the rows, with the
control file's redo and checkpoint LSNs beside them. An intermittent failure has to print
its evidence at the moment it happens; there is no going back for it afterwards. So the
next occurrence -- in CI or on a workstation -- reports what the page is without anyone
having to catch it.

`reachable` comes from the same traversal the leak invariant uses
(`wvck_mark_reachable()`), split out of `wvck_reachable()` for that purpose: two
independent walks would be two answers to one question, and this invariant is precisely
what they would disagree about.

What is left, in order:

1. **Wait for it.** The instrument reports the page's kind the next time it happens; CI
   runs this file on every push.
2. If it recurs and a reproducer appears with it, run `t/014` with `autovacuum = off` to
   test the hypothesis below by elimination. Note that this is a test of a hypothesis,
   not a reproducer, so it is worth nothing until step 1 supplies one.

Decoding a page header by hand out of a `--keep-failed` data directory (the old step 2) is
no longer the plan: the instrument prints the same reading in SQL, and 16 legs run under
`--keep-failed` since it landed have not tripped.

One mechanism is worth writing down because it is the only one consistent with a *physical*
log: **GenericXLog replay has no undo.** A transaction killed part-way has the records it
already emitted replayed and the rest not, so a page can be initialised, WAL-logged, and
linked nowhere. `weave_vacuum()` had committed before the crash in this test, and
`ForceSyncCommit()` flushes its records, so its own work should be all-or-nothing - but
`t/014` leaves **autovacuum ON**, and an autovacuum worker killed inside
`weave_vacuumcleanup()` would produce exactly this. That is a hypothesis with the right
shape and no evidence yet; it predicts the failure should vanish with `autovacuum = off`,
which is a cheap test and has not been run.

**A CORRECTION, because the first version of this entry was wrong in a way that matters
more than the bug.** It claimed 8 of 8 reproductions including 3 on a clean worktree, and
concluded the failure was deterministic and pre-existing. **Six of those eight runs never
executed the test.** `nix build --rebuild` **refuses** when the derivation has no valid
prior output - it exits 1 with "some outputs are not valid, so checking is not possible" -
and I read that exit 1 as a test failure. I then ran `nix log <drv>` to get the detail,
which returns the **most recent historical log for that derivation**, not the log of the
run I had just attempted. So the same old failure was reported back to me six times and I
counted it six times.

Two rules follow, and they are the reusable part of this entry:

1. **`nix log` is historical.** It is not evidence about the run you just made.
2. **A test result needs proof the test RAN**, not just an exit status. Grep the output for
   the test's own markers. Every run counted here now carries that evidence.

This is the third verification error in two days from the same family - the other two being
`$?` after a pipeline reading `tail`'s status, and a `grep -q` used as a pass/fail test.
All three reported a state that had not been checked.

### G22 — an unvalidated `pd_lower` in the trigram blob reader was an out-of-bounds read — **CLOSED 2026-09-16**

Found by the standing per-session upstream review, not by a test. pg_fts 1.7.1's
release note says it audited the siblings of the one `page + pd_lower` its 1.7.0
had fixed, found **eight** more, routed them all through one validating helper,
and recorded "I should have grepped the siblings then". pg_weave already had that
helper — `weave_page_entry_end()` in `include/weave/am.h`, which validates in the
integer domain and treats an implausible value as an empty page — and 20-odd call
sites went through it. Four did not.

Three of the four were benign (write paths on a page held exclusively, or an
integer comparison that was already bounded). The fourth was real:

```c
avail = ((PageHeader) page)->pd_lower -
    ((char *) PageGetContents(page) - (char *) page);
avail = Min(avail, len - off);
memcpy(buf + off, PageGetContents(page), avail);
```

`src/pages/trgm_page.c`, reading a sparsemap blob spread over a page chain under
`BUFFER_LOCK_SHARE`. `avail` is a `Size`, so a torn or recycled page reporting
`pd_lower` **below** the contents offset underflows it to ~2^64, and the `Min()`
then clamps it not to this page but to the caller's remaining blob length — which
spans the whole chain. The `memcpy` reads past the page into adjacent shared
buffers. **It cannot overrun `buf`, which is exactly why it read as safe**: the
destination is bounded, the source is not.

Fixed by routing all four through the helper, and the audit is now a lint —
`make check-pdlower`, in both CI workflows — that fails on any read of `pd_lower`
outside `include/weave/am.h`. Making the grep permanent is the point: the class
has now recurred in two codebases, and in both the first fix was one instance.

Not reachable from the regression or TAP suites, which never produce a torn page.

**The open work is now done (2026-09-16).** The guard's integer half was extracted
to `include/weave/pagebound.h` — `weave_page_entry_end_off(blcksz, contents_off,
lower)` plus the available-bytes and blob-chunk functions derived from it — with no
PostgreSQL dependency, following the `include/weave/chandesc.h` and
`include/weave/docvalid.h` precedent. `weave_page_entry_end()` and the new
`weave_page_blob_chunk()` in `include/weave/am.h` are thin wrappers: they read
`pd_lower` (still the only place in the AM permitted to, per `make check-pdlower`)
and turn the returned **offset** into a pointer. The offset return type is
load-bearing rather than stylistic — the hazard is UB *at pointer formation*, so
the guard has to finish before a pointer exists.

`test/fuzz/fuzz_pagebound.c` then drives that arithmetic over 868,560 cases
(`make check-fuzz` under ASan+UBSan, and again without sanitizers under `make
check-standalone`), including a transcription of `weave_read_blob()`'s chain loop
whose pages are `malloc`'d at exactly BLCKSZ so ASan's redzone sits immediately
past the last readable byte. Two planted-bug builds — compile-time removals in the
real header, not a weakened copy in the test — prove it has teeth:
`WEAVE_PAGEBOUND_PLANT_RAW_SUB` restores the expression above verbatim and
`WEAVE_PAGEBOUND_PLANT_NO_LOW_GUARD` keeps only the `lower > blcksz` half of the
bound (which is how the bug actually gets written). Both abort with
`AddressSanitizer: heap-buffer-overflow ... READ of size 49597 ... 0 bytes after
8192-byte region`, and both also trip the target's independent integer
postconditions (`avail == end - low`, `end >= low`) with the chain pass removed —
so the target catches the class by two independent mechanisms, not just because a
sanitizer happened to be on.

### G23 — a row inserted after the build has no vector, in any segment — **CLOSED 2026-09-19 by V7's second half**

A `WeavePendingItem` carries the tid and the `wdoc` and nothing else, so when the
pending buffer flushes into a bolt, or when an oversized `INSERT` writes a bolt of
its own, the row's `wvec` is not available at that point at all
(`src/am/ambuild.c:4200`, `src/am/ambuild.c:4484`). Those bolts therefore carry **no
vector weft**, and the rows in them are absent from vector answers rather than
present with a wrong vector — which is the safer of the two, and the reason the
writer does not instead emit a weft of dead lanes: a weft claims to cover the
documents it spans, and one that covers them with nothing is a silent recall loss
that looks like a working index.

Closing it means carrying the vector through the pending buffer, which changes the
pending item format. It was held until the merge producer (`VECTOR_CHANNEL.md` §7.3)
landed, because both changes touch the same code and the merge producer is the one V8
was blocked on. **The merge producer landed 2026-09-17, so this is now the next
vector-side gap** — and closing it also closes **G26**, because an `INSERT` after an
`ALTER INDEX ... SET (bits = ...)` is exactly what makes two bolts disagree about
their code width and makes the merge's geometry guard reachable.

Until then a vector-bearing index must be built, not incrementally inserted into, for
its vector channel to be complete — stated in the spec and here rather than
discovered by a user whose recall degrades with every `INSERT`. Note what a merge
does with such a bolt now: it carries the weft-bearing input's lanes forward and
gives every document from the weftless input a **dead lane**, so the merged bolt's
warp space still covers every document and the rows that were inserted are absent
from vector answers rather than present with somebody else's vector.

**CLOSED 2026-09-19.** `WeavePendingItem` grew a `veclen` word and the row's `wvec`
is now stored verbatim after the `wdoc`; `weave_flush_pending()` and
`weave_insert_oversized_as_segment()` both run producer 1 over it, so a flushed bolt
carries a real weft. `sql/pendingvec.sql` is the proof and `t/019_pending_v8_upgrade.pl`
covers the format transition.

**THE VECTOR IS STORED RAW, NOT PRE-QUANTIZED**, at ~8× the bytes (3,848 against 480
at 960-d/4-bit). Quantizing at insert time would bake the `bits` reloption into the
pending buffer, and an `ALTER INDEX ... SET (bits = ...)` before the flush would then
leave a pending code of the wrong width — re-widening it means reconstructing the
vector, which recomputes the `(scale, norm)` pair from a reconstruction, the one
thing `VECTOR_CHANNEL.md` forbids. Storing it raw means a flush runs producer 1
exactly as a build does: **one quantization path, not two.**

**THE FORMAT TRANSITION IS BY PAGE KIND, NOT BY VERSION WORD.** The header grew from
12 to 16 bytes, which moved the `wdoc` and changed the item stride, and the two
strides are not distinguishable from the bytes. `weave_insert()` deliberately does
not upcast the metapage (only a directory change does), so one index can hold pages
of both layouts at once — the discriminator has to be per page. Hence
`WEAVE_PK_PENDING_V9`, `WEAVE_VERSION` 9, and `WEAVE_PK_PENDING` becoming a read-only
legacy format.

**A LANE COUNT CANNOT SEE THIS BUG, which is worth recording.** Run against the
pre-fix build, `sql/pendingvec.sql` still reports 67 lanes after the flush: the merge
already gave every document from the weftless input a dead lane, so the *count* was
always right. What discriminates is `dead_lanes` (3 before, 1 after — only the row
whose vector really was NULL) and retrievability (before: the top-1 for the inserted
row's own vector was an unrelated built row at warp 53; after: the inserted row at
warp 64). A test that counted lanes would have passed on the broken code.

**THE FIRST VERSION OF THE FIX WAS WRONG AND THE SUITE CAUGHT IT.** It kept
`WEAVE_PK_PENDING` for indexes with no vector column — to avoid churning their
page-kind census — while writing NEW-layout items onto those pages, so the reader
parsed them with the old stride and three regression files filled with "skipping
malformed pending document". The kind names the **layout**, not the payload.

### G24 — the vector weft's free path was unreachable, therefore untested — **CLOSED 2026-09-17 by merge producer 2**

**Closed.** Producer 2 (`VECTOR_CHANNEL.md` §7.3) removed the exclusion that made the
free path unreachable, so `weave_free_segment()`'s `WEAVE_WK_VECTOR` arm now runs on
every merge of a vector-bearing bolt. `sql/vecindex.sql` merges and then runs
`weave_check(deep)`; the `free-omits-strips` mutation that survived the whole V7 suite
is **caught by `installcheck-pg17`** (`pages_reachable_or_freed`: 5 unreachable
pages), and so are the sibling legs for the directory chain and the new warp-map
chain. The enumeration below is kept because the *diagnosis* — an unreachable
statement rather than a missing assertion — is the reusable part, and because the
warp-map chain added by the same commit is a fourth thing this function must free.

*As it stood:* `weave_vec_free_weft()` (`src/vector/vecwrite.c`) frees a weft's
chains, and **no execution reached it.** A mutation deleting its `meta.codestart` line
survived the whole suite; the diagnosis is not a missing assertion but an
unreachable statement, and the enumeration is short enough to state completely:

| candidate path | reaches it? | why |
|---|---|---|
| `weave_merge_selected()` → `weave_free_segment()` | no | refuses at its `weave_seg_has_vector()` gate before allocating a page (sect. 7.3's interim rule) |
| `weave_merge_all()` / `weave_merge_all_parallel()` group selectors | no | filter vector-bearing bolts out of the candidate lists, so no group containing one is ever formed |
| `weave_vacuum()` / VACUUM cleanup → `weave_vacuum_compact()` → `weave_compact_to_one()` | no | compaction *is* a merge; it selects every live bolt and is refused for the same reason. This is the cost sect. 7.3 already states: a vector index does not compact |
| `ambulkdelete()` | no | tombstones live in the livedocs bitmap; it frees only the previous bitmap chain |
| tombstone-driven single-bolt rewrite (task L18) | no | not implemented, and when it is, it is a merge of one bolt and inherits the same gate |
| `REINDEX`, `VACUUM FULL` | no | build into a new relfilenode; the old one is unlinked whole, not freed page by page |
| `DROP INDEX` | no | unlinks the relation without reading a page of it |

Verified rather than only argued: a build whose `weave_vec_free_weft()` begins with
`elog(ERROR)` passes `installcheck-pg17` and `tap-pg17` (14 files, 207 tests)
unchanged.

**The function is kept, not deleted, and it is deliberately not reached by a
test-only door.** The exact condition that makes it reachable is sect. 7.3's merge
producer 2: the moment the `weave_seg_has_vector()` exclusion at
`src/am/ambuild.c:3015` comes out, every merge frees its inputs' wefts, and a free
path that forgot the strip chain leaks every code page of every merged bolt —
thousands per merge, reclaimable by nothing short of a `REINDEX`. So the mutation
that survives here is a hole that **the merge producer's commit must close**, and
that commit's gate is: a merge of two vector-bearing bolts, then
`weave_check(deep)` reporting zero unreachable pages, with the `meta.codestart`
mutation proven to fail it. Recorded here because an untested line that nobody
wrote down is indistinguishable from a tested one six months later.

### Checked and NOT a gap: the merge's move is byte-exact, but not for the reason the spec gave

`VECTOR_CHANNEL.md` §7.3 forbade the merge from re-encoding on the grounds that
quantization error would compound with an index's merge history. **It does not.**
A code is a fixed point of decode-then-encode — dequantizing yields exactly the
codebook levels and re-quantizing those returns the same levels — measured at 0 of
2,100 round trips over `bits` 2–8 at 64-d and 768-d, and 4 of 9,800 (one byte each,
at a decision boundary) over dim 4–1536. The mutation that substitutes a
decode-and-re-encode for the merge's move is therefore an **equivalent mutant** and
survives the whole suite; the mutation that additionally takes the *re-encoded scale*
is caught, because a reconstruction's norm is not its original's. §7.3 now states
that, `test/hegel/test_quantize.c` asserts it, and the rule is unchanged — carrying
bytes is still cheaper, and idempotency stops holding the moment two segments differ
in width or calibration. Recorded here because a rule with a wrong reason attached is
a rule the next person discards when they disprove the reason.

### G25 — the vector half of a merge is not streaming: O(nvec · codebytes) resident — **OPEN, quantified 2026-09-17**

The lexical merge is deliberately streaming: `weave_merge_segments_streaming()` is
bounded to **one term's postings** at a time, because merging K segments by buffering
all their postings is how a full compaction of a large index OOMs the server. Merge
producer 2 does not match that discipline. It appends every surviving lane to
`WeaveVecAccum` and only then calls `weave_vec_write_weft()`, so peak resident memory
for the vector half is

    nvec · (codebytes + 8 docid + 8 scale/norm + 1 live)  +  nvec · 8 (transient sort)

| n (documents merged) | 960-d, 4 bits | 1536-d, 4 bits | 128-d, 4 bits |
|---:|---:|---:|---:|
| 100 k | 50 MB | 79 MB | 8 MB |
| **1 M** | **497 MB** | 785 MB | 82 MB |
| 10 M | 4.97 GB | 7.85 GB | 820 MB |

**The threshold at which this starts to matter is about 1 M documents at 960-d**:
below that the merge's peak is comparable to a default `maintenance_work_mem` and to
what the lexical term hash already reaches during a build; above it, a full
compaction of one index can exceed the host's RAM — and the merge is reachable from
autovacuum, so nobody chose the moment. It is also **charged to the merge's own
memory context and not to `maintenance_work_mem`**, so an operator who lowered that
setting to bound maintenance memory has not bounded this.

Why it shipped this way: the alternative is a second writer. The lanes must come out
in output-docid order, and each input weft is already sorted by docid, so the
streaming shape is a k-way merge of sorted runs feeding blocks that are written as
they fill — which means the block writer, the statistics and the strip plan all have
to work incrementally instead of over a filled accumulator. That is a real change to
`weave_vec_write_weft()`, and §7.3's rule is that ONE function writes a weft; doing
it in the same commit as producer 2 would have meant landing the move and a rewritten
writer together, with no way to tell which one a failure came from.

The fix, when it is done: give the writer a pull-based lane source (`next_lane(void
*arg, uint8 **code, float *scale, float *norm, uint64 *docid)`), have producer 1 pull
from the accumulator and producer 2 pull from a heap of per-input cursors, and keep
32 lanes resident instead of `nvec`. The docid ordering is what makes this possible
and it is already guaranteed per input (`vector_warp_map_ascending`).

### G26 — the merge's geometry-mismatch skip is unreachable, therefore untested — **CLOSED 2026-09-19 by G23, exactly as predicted**

`weave_vec_merge_geom()` refuses a merge whose input wefts disagree on
`(dim, bits, layout, metric)`, because re-quantizing on a merge is forbidden
(`VECTOR_CHANNEL.md` §7.3). **No sequence of SQL reaches the disagreement**, and the
enumeration is short:

| how the wefts could differ | reachable? | why |
|---|---|---|
| `bits` (a reloption; `ALTER INDEX ... SET (bits = 2)` is legal any time) | no | only a *build* writes a weft, and one build reads the reloption once. The pending-flush path writes no weft at all (G23), so no bolt can carry the new width until a `REINDEX` — which rewrites *every* bolt at it |
| `dim` (a `wvec` column with no typmod may hold several) | no, in practice | `weave_vec_accum_add()` throws on a dim change *within* a segment, so two bolts of different dims need a flush boundary landing exactly on the dim change. Not arrangeable from a test, and a parallel build makes it *less* likely, not more: workers see interleaved blocks, so both dims land in one participant and it throws |
| `layout` | no | `WEAVE_PACK_LANE` is the only storable layout and the accumulator hard-codes it |
| `metric` | no | written as `WEAVE_METRIC_L2` by both producers until V8 decides whether the metric is an opclass or a reloption |

So the mutation that deletes the skip **survives**, and it is recorded here rather
than deleted, for the reason G24 gives: an untested branch nobody wrote down is
indistinguishable from a tested one six months later. It is not closed with a
test-only door either.

**What closes it is G23.** The moment a pending flush carries vectors, an `INSERT`
after an `ALTER INDEX ... SET (bits = ...)` writes a second weft at the second width
and the mismatch is three statements away — at which point this guard is the only
thing between that index and a merge that either throws inside VACUUM's cleanup or
re-quantizes half its corpus. The guard is implemented now precisely because the
change that makes it reachable is one someone will make without reading §7.3.

**CLOSED 2026-09-19, and the prediction held exactly.** G23 landed, and the three
statements are the last block of `sql/pendingvec.sql`: `ALTER INDEX pv_idx SET
(bits = 2)`, an `INSERT`, a `weave_merge()`. `weave_vec_meta()` then reports two
wefts of different `bits` in one index, the merge declines to combine them rather
than mixing widths, the index keeps answering, and every `weave_check()` invariant
holds. The first row of the table above is now false for the stated reason -- the
pending-flush path writes a weft, at the CURRENT reloption, which is correct for a
brand-new segment built from raw vectors and is exactly what a merge must not do.

### Checked and NOT a gap: the doclen sidecar has the same posting-derived hole, and it is harmless

Merge producer 2 found that a weft could not map a lane back to a row, because the
derivation it relied on -- "warp *i* is the *i*-th smallest docid in the bolt, and the
bolt's docids are all in its lexical weft" -- is false: a document reaches the lexical
weft only if it has at least one **posting**, and a non-NULL `wdoc` whose text is empty
or entirely stopwords has none. One such document shifts every later warp's derived
docid by one, and nothing counts wrong. That is why `WEAVE_PK_VWARP` exists.

**The doclen sidecar is built from the same posting stream and therefore has the same
gap -- and there it does not matter.** The sidecar is consulted only for *candidates*,
and a candidate arrives from a posting list. A document with no postings is never a
candidate, so its absent entry is never looked up; its `doclen` would be 0 in any case,
and it still counts toward the corpus `N` that BM25 needs, because `ndocs` is
incremented in the build callback rather than derived from the postings.

Recorded because the structural similarity is alarming at a glance and someone will
notice it again. The question to ask of any docid-keyed structure is not "is it built
from postings" but **"is it ever addressed for a document that has none"** -- the
sidecar is not, and the vector weft is, on every row it returns.
### Checked and NOT a gap: HOT-successor TIDs in `amgettuple`

pg_tre 4.0.2 fixed a silent under-return: its always-true scan path collected TIDs
from `heap_getnext()`, which returns the **current** tuple version, and after any
HOT update that version is a `HEAP_ONLY` successor rather than the chain root.
`index_fetch_heap` bails immediately on a heap-only TID, so every HOT-updated row
was handed over as unreachable and silently discarded — 169 of 185 rows on the
reporter's production heap.

**pg_weave is immune, and the reason is structural rather than lucky.** Our only
heap scan is `table_index_build_scan()` in `ambuild.c`, which routes through
`heapam_index_build_range_scan()` — core code that already translates a heap-only
tuple to its chain root via `heap_get_root_tuples()`. Every other TID we return
comes out of the index's own posting lists, and those store roots. The NOT-universe
path (`weave_universe_bounded()`) is built from posting lists, not from the heap, so
it inherits the same property.

Recorded rather than left unstated: "we don't have that bug" is only worth
anything with the mechanism attached, and the next person to add a heap-reading
path needs to know this is the constraint they are working under.

**Re-argued 2026-09-18, because the argument above was borrowing a premise.** It was
written against pg_tre's diagnosis, which located the bug in an `amgettuple` path and
treated `amgetbitmap` as immune. That second half does not transfer: our
`amgetbitmap` calls `tbm_add_tuples()`, and a TID bitmap is *lossy-capable* and
resolved by the executor through `heap_hot_search_buffer()`, so tuple-level TIDs
handed to it are chased to their successors rather than dropped. Inheriting
"amgetbitmap was immune" from a project whose bitmap path differs from ours is not
an argument about our code.

The immunity therefore rests on **two** properties, and both are checkable:

1. *Production* — every TID we emit is a chain root. The only heap scan in the tree is
   `table_index_build_scan()`, and posting lists store roots.
2. *Consumption* — the only way we resolve a TID to a tuple is
   `table_index_fetch_tuple()`, which performs the HOT walk itself. We never open a
   buffer and read a line pointer directly.

Property 2 is the one the borrowed argument skipped, and it is what makes the
conclusion independent of which of `amgettuple` / `amgetbitmap` the planner picks. If
someone adds a path that reads a heap page directly, property 2 is what breaks, and
no test will say so.

### G27 — the codes chain has no block→page index, so a skipped block still costs its page reads — **OPEN 2026-09-17, found by V8**

`weave_vec_block_read()` locates block `b` by walking the **entire** `WEAVE_PK_VCODES`
chain from `codestart` and matching `blockno` on each strip. Two separate costs come
out of that, and only the first was already written down (`src/vector/vecwrite.c`
flags it in a comment):

1. **Random access is O(pages in the weft).** A rerank window of 25 blocks costs 25
   full chain walks. V8 sidesteps this rather than fixing it: the shuttle uses a
   forward-only sequential cursor, so a full scan is O(pages). V10's rerank window
   and any in-place vacuum lane update cannot sidestep it.
2. **A skipped block cannot skip its I/O.** This is the part V8 found. Even with a
   sequential cursor, the chain must be read to find the next link, so the
   allowlist short-circuit saves the strip scatter and the kernel call and saves
   **nothing** on `ReadBuffer`. Claim 3 for this channel therefore has to be stated
   as *less scoring*, not *less I/O*, and the measured figure it rests on has to be
   a CPU figure.

Both are closed by the same thing: a per-block page pointer, or an extent-based
directory that gives `blockno → BlockNumber`. There is a natural place for it — the
directory record already exists per block and has room — but adding a field to
`WeaveVecDirRec` is a **format change** (`WEAVE_VERSION`, `amcheck`, the strip
invariants, the fuzz target), and V8 is not a format change. Sizing, so the decision
is not re-litigated from scratch: 4 bytes × `nblocks` = 122 kB at n = 1M, 960-d,
which is 0.02 % of the 512 MB of codes.

Not yet a *correctness* gap, which is why it is here rather than blocking V8: every
answer is right, the constant is wrong, and one claim's wording is constrained by it.

### G28 — under genuine concurrency the segment directory runs AT its cap, and the margin is a retry loop — **OPEN 2026-09-18, and it falsified a number this file published**

`t/007_segment_cap.pl`'s four "concurrent" inserters **were serial.** `finish($h) for @ins`
pumps one `IPC::Run` handle to completion while the others sit in `ClientRead` on empty
stdin — the exact harness trap the sibling project hit and documented, already present in
our copy of the test. Everything this file said about peak segment counts came through
that harness.

With the pumping fixed, the peak is **119–126 against a cap of 128**, not 15. Bisected
across five configurations, so the attribution is not a guess:

| tree | peak / resting |
|---|---|
| pre-sprint `main` | 113 / 6 and 123 / 7 |
| + merge serialization | 119 / 7 |
| + parallel-merge claim discipline | 120 / 6 |
| + snapshot allocator | 119 / 7 |

End page counts move by less than 0.4 % across all five, so this is **pre-existing and not
caused by the 2026-09-18 sprint**. Two consequences:

1. **G20's "four concurrent inserters independently peak at 15" is an artifact**, and so is
   the comfort that `peak <= 64` provided. The assertion has moved to the *resting* count
   (which is 1); the peak is now bounded by the hard cap, with the diagnostic as the
   measurement rather than a threshold nobody had justified.
2. **The sibling project's "max 15 against a cap of 128" is likely the same artifact**,
   since we inherited the test's shape from them. That belongs in the upstream report.

Phase B of the rewritten test — a `VACUUM` every 0.2 s against the inserters under a 300 s
deadline — has reached **128 of 128 with no cap error**. That is not an outage:
`weave_add_segment_with_room()` merges to make room and its retry loop is doing exactly its
job. But the margin is the retry loop rather than the design, and a retry loop that exhausts
`WEAVE_MAX_SEGMENTS` passes throws, which surfaces as a failed `INSERT`.

Candidate lever, unmeasured and therefore not implemented: make the insert-time merge
**block** rather than skip above some fraction of the cap, so pressure becomes back-pressure
instead of directory growth. That is a latency-for-safety trade and it needs its own
measurement before anyone picks a fraction (hard rule 9).

### Checked and NOT a gap, then made into one by our own change: the recyclability liveness gate

Worth recording as a pair, because the sequence is the lesson.

`weave_page_recyclable()` returns `true` for a page with no `WEAVE_FREED` flag — the same
shape as a bug the sibling project fixed in 1.8.3, where a live page was handed out as merge
output and the merge self-deadlocked on its own buffer. I checked it on 2026-09-18 and
concluded **not a gap**, with a mechanism: the only writer of "free" into the FSM is
`weave_free_page()`, which sets the flag first; both allocator paths clear the FSM entry on
handout (`RecordUsedIndexPage`, and `GetFreeIndexPage` removes it by construction);
`RelationTruncate` truncates the FSM; and every acquisition uses `ConditionalLockBuffer`, so
a backend cannot be handed a page it already holds — it takes the contended branch instead of
deadlocking.

**That reasoning was correct, and the snapshot allocator invalidated it the same day.** A
snapshot entry can be taken and filled by another backend between the snapshot and the
handout, and `GetPageWithFreeSpace()` can hand one block to two backends: the snapshot is
itself a new staleness window, which is exactly the premise whose absence the "not a gap"
verdict rested on. The liveness gate now runs **before** the AccessExclusiveLock bypass.

The generalizable part: **a "not a gap" verdict is only valid against the code that was there
when it was made,** and it should name the property it depends on so the change that breaks
the property is forced to notice. This one depended on "nothing advertises a page as free
except the free path", and the fix in the very next commit added something that did.

## 4. Gaps against the rest of the stack

These are absences rather than regressions, and they are larger than everything in
§2.

| # | gap | state |
|---|---|---|
| **G7** | **No vector index.** The AM does not accept a `wvec` column. | The quantizer works and is property-tested; `wvec` exists as a type. Storage, kernels, and the Vamana graph do not. Tasks V7–V14. Until this lands there is no pgvector comparison to make. |
| **G8** | **No fuzzy/regex channel.** | Sources imported from pg_tre, not compiled. Tasks Z1–Z9. |
| **G9** | **No fused top-k.** | The headline differentiator. Specified, unimplemented. Phase F. |
| **G10** | **No cost-model calibration.** | Directly caused G1's discovery being delayed and will cause more: with `amcanorderbyop` the planner is choosing between an index ordering scan and a Sort, and a wrong cost silently loses the index. Task P4. |
| ~~**G11**~~ | ~~No parallel scan~~ | **NOT A GAP — a permanent characteristic.** pg_fts built a complete parallel ranked CustomScan, verified it byte-exact, measured it and reverted it (`a513d13`). Amdahl p=0.88 caps W=8 at 8.3 ms best case against pg_search's 2.12 ms, `nsegments=1` is enforced by tiered merge so per-segment parallelism divides by one, and workers refused to launch from `ExecCustomScan` on EC2. Moved to `doc/ARCHITECTURE.md` §8. |

## 5. Plan, in dependency order

The ordering is driven by three rules: fix silent-wrongness before slowness; fix
things that make measurement honest before things that make numbers better; and
do not start the novel work until the channels it composes actually exist.

**Now — correctness and honesty of measurement**

1. **G1**, keyless ordering scan. Highest priority: it is a silent 7,000× cliff on
   the first query a user writes. New task **L7**.
2. **G6**, deterministic index size. The harness must fail if `weave_merge` /
   `weave_vacuum` fail, and a fresh `CREATE INDEX` should reach its natural size
   without manual compaction. New task **L8**.
3. **G10**, cost-model calibration against the measured latencies now in
   `bench/RESULTS_LEXICAL.md`. Task **P4**. Without this, every later optimization
   is invisible because the planner may not choose the path.

**Next — close the measured losses**

4. ~~**G3/G4**, profile the fixed per-scan cost on EC2 and attribute the 0.05 ms
   before changing anything. New task **L9**.~~ **DONE 2026-09-10** —
   `bench/RESULTS_SCAN_PROFILE.md`. The answer was the doclen cursor (~72%), not
   dictionary lookup or cursor construction. Route is L17.
5. **G2**, `weave_index_size_detail()` then attack the largest attributable
   component. New task **L10**.
6. **G5/G11**, parallel merge (L4) and parallel scan (L11).

**Then — the absent capabilities, in this order**

7. **G7**, the vector channel through V9, because that is what makes a pgvector
   comparison exist. Note V13: warp ordering by cluster is a gate, not an
   optimization — the block bound prunes 99.6 % with a coherent docid order and
   0.0 % with a heap order.
8. **G8**, the fuzzy channel through Z9.
9. **G9**, the fused scorer, only once 7 and 8 are green.

**Throughout**

10. Re-run `bench/lexical.sh` after every change and update
    `bench/RESULTS_LEXICAL.md`. A performance change with no recorded before/after
    is not done.
11. Add pg_search, pg_textsearch, and VectorChord to the harness (task P3). GIN is
    the floor, not the competition.

## 6. Honest position statement

Today, against tsvector + GIN on a 1M-document corpus, pg_weave wins
overwhelmingly on common-term ranking (8.2–19×), `count(*)` (595×), prefix
counting (3.8–7.7×), and — since L7 — the bare `ORDER BY` form (1,615–7,080×). It
wins on features outright. It loses by 1.7× on rare and mid ranked latency and by
1.7–1.9× on index size.

**Standing losses after L12, L14, L15 and L17: G13 (ranked at k=10, now 1.20–4.75×) and G5
(build, 3.91× behind pg_textsearch — narrowed from 11.0× → 6.96× → 3.91×; now at
parity with GIN in one run).**

G13 now has a *measured* root cause rather than an asserted one, and it was not the
asserted one. `bench/RESULTS_SCAN_PROFILE.md`: ~72% of a ranked scan is the doclen
sidecar cursor re-deriving its position — it keeps one 128-docid block resident and
re-pins the page and re-walks its block headers from the start on every block change,
which for a mid-frequency term is every ~2.6 candidate documents. L2's on-disk impact
ordering attacks the other ~28%. Route is L17. This is the second time in this project
that a hot symbol's *cause* was mis-stated before anyone profiled (see L15), and the
second time the fix turned out to be cheaper than the prescribed one.

G13's route was **L17**, which delivered on the sparse bands (rare 1.87×, mid
1.66×) and left `common` flat by construction — the win scales with candidate
stride, and a term matching 87% of the corpus has none to exploit. **L2 now owns
the common band**, where the scan really is reading 1.74M postings. **G5's route was L15, and
it over-delivered**: the profile predicted a ceiling of ~205 s from eliminating the
term hash; the build reached 192.5 s because the larger cost was a second,
unnamed per-posting hash (the doclen collector). The profile's estimate was
wrong in the useful direction, but it was wrong, and for a reason worth keeping:
it attributed by symbol, not by caller, and so counted one hash where there were
two. G1, G2, G6 and G12 are closed, and G2 and G12 both turned out to be wins —
pg_weave's index is 1.76× *smaller* than GIN's, not 1.9× larger.

Compaction was also shown not to affect ranked latency, which **falsifies the
stated G3/G4 diagnosis**: the build already produces one segment, so per-segment
setup was never the cost. **L9's profile has since been taken
(`bench/RESULTS_SCAN_PROFILE.md`) and the hypothesis recorded here — dictionary
lookup and cursor construction — was also wrong.** It is the doclen sidecar
cursor: ~72% of a ranked mid k=10 scan, re-pinning the page and re-walking its
block headers on every 128-docid block change. Recorded rather than deleted
because it is the third hypothesis in this file that a profile overturned.

### G29 — the vector channel does not scan the pending buffer, so an inserted row is absent from vector answers until a flush — **OPEN 2026-09-19, found by closing G23**

G23 is closed at the SEGMENT: a flush now folds the pending vectors into a real weft.
The window before that flush is a second, much smaller version of the same
asymmetry. The lexical channel matches pending documents from the pending page
itself (`weave_collect_matches()` walks `meta.pendinghead`), so an inserted row is
searchable immediately. The vector channel cannot: the shuttle is per-bolt
(`weave_vec_shuttle_*`, three lockstep cursors over VDIR/VCODES/VWARP), and a pending
document is in no bolt. So between the `INSERT` and the next flush the row is in
lexical answers and not in vector ones.

**Why this is much smaller than G23 was.** The window is bounded by the flush
cadence, and flushes are driven by VACUUM cleanup and by the insert-time tiered
compaction, not by anything the user has to remember. G23's window was *forever*.

**What closing it takes, and why it is not done here.** The vector is on the page, so
quantizing it at scan time is possible — the codebook and rotation are pure functions
of `(dim, bits)` — but it needs a scoring path that is not a bolt cursor, and its
results have to enter the same top-k as the bolts'. That is the fused scorer's job
(Phase F), and building a second, parallel top-k merge before F exists is how two
scorers that disagree get written. Held for F, recorded here, and asserted in
`sql/pendingvec.sql` (`lanes_before_flush`) so that closing it shows up as a diff
rather than as nothing.

**Note the asymmetry is not new to the vector channel.** Per-term `df` in the
dictionary is also not updated until a merge, which is documented as matching GIN
fastupdate's staleness. The difference is that stale `df` perturbs a *score* while
this omits a *row*.

Against the full separate-extension stack it is not yet a comparison: there is no
vector index and no fuzzy channel.

"Better on all dimensions" is achievable against that stack. It is not achievable
today, and the gap list above is what stands between here and there.

### G30 — the heap-side fuzzy predicate returned FALSE NEGATIVES, on pure ASCII, and Z5 turned that into a cross-plan disagreement — **FOUND AND CLOSED 2026-09-19**

`weave_doc_has_fuzzy()` (`src/query/doc.c`) is the heap-side evaluation of `term~k`
and the recheck above a lossy bitmap scan. It computed an exact CHARACTER distance
with core's `varstr_levenshtein_less_equal()` and then gated that computation behind
two pre-filters stated in BYTES, both with the wrong bound:

1. **The trigram pigeonhole bound was `nqtrg > k`.** One edit destroys the three
   trigrams that overlap its position, not one, so the sound bound is `nqtrg > 3k`.
   Measured: `to_wdoc('simple','abxd zzz') @@@ 'abcd~1'` returned **false** while
   `levenshtein('abcd','abxd') = 1`. Same for `abcde` / `abxde`. **No multi-byte
   character is involved** — this was a wrong answer on ASCII input.
2. **The length filter compared BYTE lengths against a CHARACTER budget.**
   `abs(candlen - termlen) > k` skips a five-character candidate that differs from a
   five-character query by one three-byte character.

The same misstated constant was in the index-side funnel: `weave_trgm_candidates()`
was called with a flat `min_trigrams = 3` regardless of `k`, so a query with exactly
three distinct trigrams was funnelled at `k = 1` and one substitution could destroy
all three. That route is the inexact one, so its heap recheck can only remove rows,
never restore them.

**Why it survived this long: both paths were wrong in the same direction.** Before
Z5 the index used the byte-wise automaton in `include/weave/lev.h`, so
`'naive~1'` missed the U+00EF spelling from the index too — index and heap agreed by
both being wrong, and every test in the tree compares one plan against another plan.
Z5 made the index exact in characters, which is what turned a latent wrong answer
into a visible disagreement. **The lesson is the one `AGENTS.md` already carries in
another form: two implementations agreeing is not evidence when a third, independent
oracle is available and was never asked.** `contrib/fuzzystrmatch`'s `levenshtein()`
was that oracle all along.

**Closed by** stating each pre-filter's precondition instead of its bound: the length
filter counts characters (`pg_mbstrlen_with_len`), and the trigram filter acts only
when `nqtrg > 3k` **and** both strings are ASCII (byte trigrams say nothing about
character edits otherwise — `naive` and the U+00EF spelling share not one byte
trigram and are one character apart). The funnel's minimum is now `3k+1` for fuzzy;
regex keeps 3, because its trigrams come from the pattern AST and there is no `k` to
budget for. `sql/fuzzyuleven.sql` asserts index / heap / `levenshtein()` agreement on
every case, and five mutation legs — each of the two bounds, the edit unit, the
funnel minimum, and `~0` — are each killed by it.

### G31 — `term~0` silently meant `term~1` — **FOUND AND CLOSED 2026-09-19**

`src/query/parse.c` lexed `~k` as `Max(k, 1)`, so a zero edit budget returned every
term at distance 1 as well. It is the only wrong answer this parser produces on its
own, and it is in the false-POSITIVE direction, which is the direction a recheck
cannot repair either (there is nothing to recheck against — the query itself has been
widened). `~0` now normalizes to the plain term: the same rows by definition, reached
through the exact-term route rather than an automaton with an empty budget. A bare
`term~` still means 2, which is a default rather than a value the user wrote. Visible
in `'naive~0'::wquery` printing as `'naive'`, asserted in `sql/fuzzyuleven.sql`.
