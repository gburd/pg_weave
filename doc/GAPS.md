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
| ~~**G3**~~ | ~~ranked latency on rare terms (df 25)~~ | **CLOSED 2026-09-21 BY THE RESTATED PHASE L GATE, NOT BY A FIX — the loss is accepted, not repaired.** 0.04 ms vs 0.03 (1M) and 0.05 vs 0.03 (4M): still **behind**, by 10–30 µs, reproducing in the same direction at both scales. The gate now reads "no row behind by more than 0.05 ms, none behind above 0.10 ms" (`doc/PHASES.md`, maintainer decision), which this clears. The loss stays written here and in `bench/RESULTS_LEXICAL.md` under hard rule 8; `count(*)` AND (0.04 vs 0.02) is the same shape and the same verdict | accepted at ≤ 0.05 ms |
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

### G3, G4 — rare and mid ranked latency — **G4 (mid) CLOSED BY MEASUREMENT 2026-09-21; G3 (rare) CLOSED 2026-09-21 BY THE RESTATED GATE AT 10–30 µs, WHICH IS NOT THE SAME THING**

**G4 is closed.** The mid band is no longer a loss: at 1M it is **0.50 ms against
GIN's 2.27 ms** and at 4M **1.02 against 3.96** — a 3.7–4.5× *win*, from a 1.7× loss
(`bench/RESULTS_LEXICAL.md`, 2026-09-21, two scales). Nothing in this gap's own
"candidate fixes" list did it; L14 (incremental WAND growth) and L17 (the doclen
sidecar's absolute-offset docid column) did, and the fork-vs-fork arm attributes it:
pg_fts v1.8.3 measures 3.49 ms and 5.07 ms on the same table, so the win is earned
rather than inherited.

**G3 is closed by decision, not by a fix, and the distinction is the whole content of
this update.** Ranked rare k=10 is 0.04 ms against GIN's 0.03 at 1M and 0.05 against
0.03 at 4M: a loss of **10–30 µs**, reproducing in the same direction at both scales,
with p50 equal to p99 for both arms and a reporting resolution of 0.01 ms.
`count(*)` AND behaves the same way (0.04 vs 0.02). The loss is real. What changed on
2026-09-21 is the **gate**: it was phrased "zero measured losses", which a 10 µs
difference can never satisfy and which a 0.01 ms reporting resolution cannot even
measure, so it was restated in absolute terms — no row behind by more than 0.05 ms, no
row behind at all above 0.10 ms (maintainer decision, `doc/PHASES.md`). Under that gate
these rows pass; under any honest reading pg_weave is still slower on them, and hard
rule 8 keeps them written down here and in `bench/RESULTS_LEXICAL.md`. **The hypothesis
below (fixed per-scan setup) is no longer supported by its own evidence**: it rested on
the df 25 vs df 2,503 ratio being 70× for a 100× document ratio, and that ratio is now
0.04 → 0.50 ms, i.e. 12.5× for 100× — which says the per-document work got much cheaper
while the fixed cost did not move. Anything spent on G3 from here is spent on 20 µs,
which is why nothing more will be.

*Original text follows.* **Absolute magnitudes are small** — 0.02 ms and 1.5 ms — but
they are losses, and they are the queries a search application runs most.

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

### G5 — build time — **REFRAMED 2026-09-21: the GIN loss is gone, a pg_fts loss appeared at scale**

Against GIN, build is now a **win**: 10.9 s against 11.2 s at 1M and 56.8 s against
82.8 s at 4M (`bench/RESULTS_LEXICAL.md`). The 1.2×-then-2.5× loss this gap was opened
for is closed by L12.

Against **pg_fts**, measured for the first time on 2026-09-21, build is a **tie at 1M
(10.9 s each) and 1.08× slower at 4M (56.8 s against 52.7 s)**. Small, and it only
shows at scale, which is the interesting part: a constant-factor difference would show
at both. The mechanism is not established, and L8's vacate+pack pass is the obvious
suspect precisely because it is the thing pg_weave added and L12 made cheap rather than
free. Not chased; recorded so the direction is on the record per hard rule 8.

The paragraph below is about a different corpus (`synth-2m-long`) and a different
comparator (pg_textsearch) and still stands as the profile attribution.

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

### G27 — the codes chain has no block→page index, so a skipped block still costs its page reads — **FIXED 2026-09-23; hard rule 12's scale run DONE 2026-09-24, and it found a detection gap in the invariant**

> **SCALE RUN, 2026-09-24** (`bench/RESULTS_VECMERGE_SCALE.md`). Three runs at 1M × 960-d
> on EC2, four merges and up to six vacuum cycles each. `weave_check(deep)` clean at
> every stage of every run, the live-lane digest byte-identical across every merge, and
> the whole weft byte-stable across all six vacuum cycles.
>
> **But the first two runs could not say that, and the reason is worth keeping.** The
> mutation control never fired — twice for harness reasons, and the third time because
> `firstpage + 2` is *not a detectable mutation at this geometry*: `vector_codes` 75,000
> pages over 25,000 blocks is exactly three strip pages per block at 960 dimensions, and
> all three carry the same `blockno`. The invariant as originally written checked
> page-kind plus `blockno == b`, so it passed a pointer naming the same block's third
> strip — while a scan starting there follows `nextblk` into block b+1 and **refuses**.
> An offline checker that calls an index healthy when its queries error is worse than no
> checker.
>
> Tightened to require the block's **first lane strip** (`WeaveVecStripHdr.j0 == 0` and
> not the centroid flag), which at three strips per block takes the undetectable wrong
> values from two per block to none. True positive demonstrated on EC2:
> `firstpage 1832 is not this block's first lane strip`. The control now asks both
> questions — does the checker catch it, does the scan refuse it — because checking only
> the first is what made a detection gap look like a blind invariant.
>
> **Also found, and not G27's:** a reproducible ~1.53× swing in NON-vector pages across
> vacuum cycles, bit-identical across two runs, period-2, tied to the relocation pass.
> Every vector bucket is constant to the page. Tracked separately; it is the G18 shape
> and L19 was supposed to have closed it.

**FIXED, and by the design the third attempt arrived at: one `weave_uint32 firstpage` in
`WeaveVecDirRec`** (284 -> 288 bytes, records per directory page unchanged at 28, so zero
extra pages), `WEAVE_VMETA_VERSION` 2 -> 3 with v2 refused as v1 already was, a validated
O(1) seek in the code cursor, and a `weave_check()` invariant.

*Measured on the shipping scan,* buffers for a fused query at 0.1 % selectivity:
**scifact 1045 -> 513 (2.04x)**, **fiqa 8727 -> 1823 (4.79x)**, against a pre-registered
projection of 1.8x / 4.3x. The unfiltered arm is unchanged to within one buffer, which is
the control: with no predicate every block is visited, so the seek must cost nothing.

*Answer-preserving, by diff rather than by argument:* all twelve rows of
`bench/gatesweep.sh`'s work counters are **bit-identical** before and after on all three
corpora -- pivots, lexical contributions, vector `score()`, gate scores, lanes, blocks,
`blkskip`, `rqskip`, vetoes, abandonments.

*Checked on the paths that matter:* `weave_check()` reports 0 failing invariants on a fresh
build, a three-bolt index, after `weave_merge()`, and after a `DELETE` + `VACUUM` rewrite,
and the fused scan answers on the rewritten weft. Positive control: a writer mutated to
store `firstpage + 2` makes `weave_check()` say *"bolt 0 block 0: firstpage 251 is not a code
page carrying this block"* and makes the scan **refuse** rather than score another block's
codes. `make check-standalone`, `installcheck-pg17`, `installcheck-pg18` and `tap-pg17` all
pass; `test/hegel/test_vecpage.c`'s record-size pin was updated from 284 to 288 with the
records-per-page assertion left beside it, because that pair is what proves the field was
free.

**Owed:** hard rule 12's scale run -- this touches merge and vacuum, and local green is not
evidence for those. `bench/RESULTS_GATE_SWEEP.md` carries the numbers and the three refuted
designs.

*Original entry follows.*

**2026-09-23: this gap is now the recommended fix for something much bigger than it was
filed as, and it got there by measurement rather than argument.** `bench/gatesweep.sh`
measured claim 3 for the first time: with the predicate tightened from 100 % to 0.1 %,
pivots and vector `score()` calls fall **exactly** with selectivity and scored code blocks
follow `1 − (1 − s)^32` (0.969× / 0.265× / 0.031×, nine of nine points near the formula on
three corpora) — while `EXPLAIN (ANALYZE, BUFFERS)` reports **502 / 515 / 502 / 424**
buffers, flat. The 32× reduction in scored blocks buys 1.0× in pages, and THIS gap is why.

That makes the block→page index the alternative to the vector-major second copy that was
the only remaining option for `FUSED_TOPK.md` §8's vector work row:

| | fixes | cost per doc | helps a scattered candidate set? |
|---|---|---|---|
| vector-major second copy | bytes touched within a block | **96–192 B** | **no** — each survivor is on its own page |
| **block→page index (this gap)** | which pages are read at all | **0.125 B** (one `BlockNumber` per 32-lane block) | **yes** |

7.2 KB — one page — for fiqa's 1,800 blocks. See `bench/RESULTS_GATE_SWEEP.md`.

**MEASURED VERDICT AND CHOSEN DESIGN, 2026-09-23 (evening). The speculative address is
REFUTED and the third option is better than both of the first two.**

*The speculative `codestart + b × strips_per_block` is dead.* `/scratch/pg_weave/addrprobe.sql`
built the three states that matter from real 384-d vectors and measured the hit rate of the
guess (interleave-corrected) against the truth from `weave_vec_strips()`:

| state | blocks | naive hit | interleaved hit | max deviation |
|---|---|---|---|---|
| fresh build, then merged | 162 | 28 | **152 (94 %)** | 22 pages |
| after `DELETE` + `VACUUM` (tombstone rewrite) | 122 | 6 | **6 (5 %)** | **213 pages** |
| after a second merge | 162 | 28 | **162 (100 %)** | 0 |

A vacuum rewrite draws recycled pages and destroys addressability — 5 %, which is worse than
useless: a validated guess that misses pays the fallback walk *plus* the wasted read, and a
long-lived index accumulates exactly these segments. So the earlier "no format change needed"
correction is itself **retracted**; the density argument held only for the states that had
never been vacuumed.

*And the on-disk index chain this gap was filed as is not the cheapest fix either.* Put the
pointer in the directory record that every scan already addresses in O(1):

> **`WeaveVecDirRec` gains one `weave_uint32 firstpage`, the block's first strip page.**

The arithmetic is why this is the answer, and it is exact rather than approximate.
`weave_vecdir_recs_per_page()` is `(usable − sizeof(WeaveVecDirHdr)) / sizeof(WeaveVecDirRec)`
with `usable = WEAVE_VECPAGE_PAYLOAD = 8152`, so it is `8144 / 284 = 28` today and
`8144 / 288 = 28` with the extra field. **The directory occupies the same number of pages
before and after** — confirmed against the relation, since `ceil(162/28) = 6` and
`ceil(1800/28) = 65` are exactly the interleaved-page counts measured inside the code spans.

| option | new page kind? | extra pages | survives a vacuum rewrite? |
|---|---|---|---|
| vector-major second copy | no | **+96–192 B/doc** | n/a — does not address page count |
| separate block→page index chain (as filed) | **yes** | +0.125 B/doc | yes |
| speculative address + validation | no | 0 | **NO — 5 % hit rate** |
| **`firstpage` in `WeaveVecDirRec`** | **no** | **0** | **yes** |

Implementation shape, in the order it has to be built:

1. `include/weave/vecpage.h`: the field, and a `WEAVE_VMETA_VERSION` bump so a reader can
   tell a populated `firstpage` from a zero one. **Old wefts keep the chain walk** — the
   doclen-sidecar precedent (`am.h`, v4): a self-describing decoder means no `REINDEX`.
2. `src/vector/vecwrite.c`: populate it in the build/merge strip appender, which already knows
   each page's number as it appends, and **in every path that moves a strip page** — the same
   discipline the five bound fields already carry, and the same hazard: a stale `firstpage`
   would send a scan at another block's codes, which is a wrong answer rather than an error.
3. `src/vector/vecshuttle.c`: `code_cur_seek()`, validated by the check the cursor already
   makes ("page k of block b must claim block b", `:230`), falling back to the walk on any
   mismatch or on an old weft.
4. `weave_check()`: every block's `firstpage` names a `WEAVE_PK_VCODES` page claiming that
   block. This is the invariant that makes step 2's hazard detectable.
5. A property test that seeks every block in random order and compares against the chain
   walk, and — because hard rule 12 applies, this touches merge and vacuum — a scale run.

Expected win, corrected and measured rather than projected: **1.8× (scifact) / 4.3× (fiqa)
fewer total query buffers at 0.1 % selectivity**, rising with corpus size; the vector weft
drops to 0.06–0.07 % of its pages, after which the **warp map** (`weave_fuse_vec_warpmap()`,
O(nvec) unconditionally) is the dominant selectivity-independent term.

**THIRD AND FINAL CORRECTION, same evening: an explicit per-block pointer is MANDATORY, and
it cannot live in `WeaveVecDirRec` as cheaply as the arithmetic above suggested.**

*Why no formula can work.* Two vacuum rewrites of the same table produced opposite structures.
The rewrite of the insert-built segments hit **6 of 122 (5 %)** with a maximum deviation of 213
pages; a later rewrite of a *merged* segment came out locally contiguous — **117 of 121 page
deltas equal to `strips_per_block`**, the other 4 being the interleaved directory pages. Both
are rewrites through the same single writer (`weave_vec_write_weft()`), so the difference is
not in the code: it is which pages the **FSM free list** happened to hold, which is a function
of the index's vacuum and merge history. A speculative address is therefore not merely
sometimes wrong, it has **unpredictable performance** — 5 % or 97 % depending on history —
which is worse for a planner and for a benchmark than a structure that is always right.

*Why the pointer cannot simply be added to the directory record.* Growing `WeaveVecDirRec`
from 284 to 288 bytes keeps records-per-page at 28 (that arithmetic stands), but it moves
**every record's offset**: record `i` sits at `i × sizeof(rec)`, so a v3 reader misparses every
v2 record after the first. Supporting both would make the record size version-dependent at
every read site (`weave_vecdir_read`, the shuttle's directory cursor, `vecstats`,
`weave_check()`, two SRFs). And `weave_vec_weft_open()` **refuses an unknown version outright**
(`src/vector/vecwrite.c:917`), so the "old wefts keep the chain walk, no `REINDEX`" sentence
written above is **false as the code stands** — found by reading the gate rather than assuming
it, and it is the third thing this entry got wrong before any code was written.

*The hardened design, which is G27 as originally filed plus a migration story:*

1. **New chain `WEAVE_PK_VCIDX`**: a dense array of `BlockNumber`, entry `b` = block `b`'s
   first strip page. O(1) addressable exactly like the directory (entry `b` on page
   `b / (payload / 4)`), **0.125 bytes per document**, 7.2 KB for fiqa's 1,800 blocks. No
   record-offset change anywhere, so no version-dependent parsing.
2. **`WeaveVecMeta` gains `cidxstart`**, version 2 → 3, and the version gate is relaxed to
   accept **both**: on a v2 weft the field reads as **0**, which is `WEAVE_METAPAGE_BLKNO` and
   can never be a valid chain root, so 0 is a sound "absent → walk the chain" sentinel. This
   depends on VMETA pages being zero-filled past the struct (`weave_init_page` plus a fresh
   page); **assert that on a real v2 weft before relying on it**, because if it does not hold
   the fallback is a `REINDEX` requirement and that changes the release note.
3. **Writer**: collect each block's first strip page during the existing block loop (a
   huge-safe `nblocks × 4` transient array) and emit the chain after it. One writer serves
   build, merge *and* the vacuum rewrite, which is why this is one change and not three.
4. **Reader**: `code_cur_seek()` in `src/vector/vecshuttle.c`, validated by the check the
   cursor already makes, falling back to the walk on mismatch or on a v2 weft.
5. **`weave_check()`**: every entry names a `WEAVE_PK_VCODES` page claiming that block — the
   invariant that makes a stale pointer (a wrong answer, not an error) detectable.
6. **Gates**: a property test seeking every block in random order against the chain walk; and
   because this touches merge and vacuum, hard rule 12's scale run before it counts as done.

**CORRECTED the same day, twice, and both corrections matter to whoever implements this.**

*The ratio was overclaimed.* Page traffic cannot follow the blocks-scored column to 0.031×,
because two structures are read in full on every scan regardless of the gate: the block
**directory** (forward-only cursor, and with the normalizer ON a second transient cursor
folds the weft max score) and the **warp map** (`weave_fuse_vec_warpmap()`,
`src/am/amscan.c:7620`, walks every lane to build `docid[]`/`allow[]`). Measured geometry
(`weave_vec_meta`, `weave_vec_strips`: 2 strip pages per block) gives the real projection at
`s = 0.001`: vector pages 337 → ~23 on scifact and ~3,723 → ~235 on fiqa, i.e. **0.06–0.07×
of the weft**, and in total query buffers **1.8× / 4.3×** — rising with corpus size. An
ablation against a lexical-only arm confirms the premise the recommendation rested on: the
vector channel is **61 %** of scifact's buffers and **89 %** of fiqa's.

*And the on-disk index may not be needed at all.* Measured with `weave_vec_strips()` on the
real local indexes, the code chain is **98.3–98.5 % dense** (scifact 324 pages in a 329-page
span, fiqa 3,600 in 3,664), and the slack is **exactly** the interleaved directory pages —
one per 28 blocks, because `vec_chain_append()` appends both chains concurrently. So
`codestart + b × strips_per_block` works as a **speculative** address, validated by the check
the cursor already performs ("page k of block b must claim block b",
`src/vector/vecshuttle.c:230`), with a fallback to the chain walk on a miss. That is zero
on-disk bytes, no page kind, no `WEAVE_VMETA` bump, no migration, no expected-output churn,
and it stays correct under FSM page reuse — which was the objection that ruled out
arithmetic. A miss costs one wasted page read, roughly 1 in 57. **Measure the hit rate on a
MERGED and VACUUMED index before choosing**, because that is where the density argument is
weakest; the 0.125 B/doc on-disk index remains the fallback design.

*Original entry follows.*

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

**Now visible through a plan, not only through an SRF (task F7, 2026-09-21).** F7 gave
the vector channel its `ORDER BY` members — `<->` (metric `l2`) and `<#>` (metric `ip`),
one per metric the scan core serves, and deliberately not `<=>` — so the gap is
reachable from ordinary SQL:
`sql/vecorderby.sql` section (7) asserts `vec_rows_before_flush` = table rows − 1 and
`pending_row_in_vec_answer_before_flush` = 0, with an `EXPLAIN` proving the count came
from the index scan rather than from a Seq Scan (the count is the discriminator, so a
sort would have reported the opposite and looked like a pass). That makes the window a
user-visible missing ROW in a `LIMIT`-less vector query, which is a stronger statement
of the same gap than a lane count was.

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

### G32 — the regex funnel's literal-run extractor read `\d` as the letter d: a FALSE NEGATIVE with the weft present — **FOUND AND CLOSED 2026-09-20**

`weave_regex_trigrams()` (`src/query/trgm.c`, now deleted) scanned a pattern for runs of
literal characters to extract "required" trigrams, and its escape handling was "the next
char is a literal". Core's ARE gives a backslash-letter a meaning in many cases (`\d \w
\s \y \m \M \A \Z` and their complements), so `/ab\dcd/` was read as requiring the run
`abdcd`, whose trigrams no term containing `ab5cd` has. Measured on an index built
`WITH (trigrams = on)`: the index arm returned **no rows**, the heap predicate and core's
`~` returned the row. `/\yabc\y/` happened to survive only because the funnel UNIONed
the run's trigrams and `abc` was among them.

**Why it stayed hidden**: the weft is off by default, so the extractor ran in one
regression test; and the funnel's union was so permissive that most wrong runs still
admitted the right term by accident. A test that compares the index against the heap
would have caught it the first time anyone wrote `\d` — nobody had.

**Closed by** Z6: the extractor is gone. Narrowing now comes from pg_tre's regex AST, and
is refused outright — the whole dictionary is walked instead — whenever the raw pattern
contains any construct on which core's ARE and pg_tre's tokenizer could disagree about a
literal run. The route that consumes the candidates is exact (core's engine over each
term), so a narrowing error can only ever be a false negative, which is why the rule is
"refuse when unsure" and not "recheck later". `sql/regexdict.sql` pins `\d`, `\y`,
`[[:digit:]]` and `(?i)` three-way against core's `~`, and the mutation leg that removes
both defences (the whitelist and the tokenizer's own refusal of `\d`) reproduces this gap
exactly.

### G33 — the `<@>` block bound is sound and prunes **0.0 %** of dictionary pages for the ordinary query shape — **OPEN 2026-09-20, found by measuring Z9's own gate**

`doc/specs/FUZZY_CHANNEL.md` §5 specified two lower bounds on edit distance (length
deficit, trigram deficit) for the `<@>` KNN shuttle and required the tightness to be
measured before Z9 could be called done. It was measured (`bench/edist_bound.c`,
`bench/RESULTS_EDIST_BOUND.md`): on a 254,000-term vocabulary in 1,004 dictionary pages,
over 12 patterns x k in {1,2,3}, the **median** fraction of pages skipped without computing
a single distance is **0.0 %** and the median fraction of terms whose exact Levenshtein
distance is computed is **100.00 %**. 22 of 36 rows prune nothing at all.

**The mechanism is the ordering, not the tightness.** A dictionary page is a run of
lexicographically adjacent terms, and byte order has no relationship to term LENGTH or to
distinct-trigram COUNT -- so every page carries a near-full spread of both, its min/max
statistics sit near the vocabulary's global min/max, and both deficits collapse to ~0.
Pruning only appears when the pattern is extreme in length relative to the vocabulary
(89-99 % for a 1-2 character pattern) or when an exact match exists and sets theta to 0
(32.7 %). For the case the feature exists to serve -- a misspelled word, several edits from
anything -- it is exactly zero.

This is **the same finding as `bench/RESULTS_BOUND_PRUNING.md`**, one channel over: a
provably-correct bound that prunes nothing because the block's contents are unrelated to
the quantity being bounded, and the fix there was an ordering constraint nobody had
written down. Here the analogous constraint would be clustering the dictionary by term
length, and the second arm of the measurement shows it is **not** sufficient either: the
achievable-ceiling column rises to ~40 % for length-extreme patterns and stays ~0.1 % for
the ordinary one. It is also not available -- the lexical channel's point lookups, prefix
scans and sparse block index all require byte order.

**What is NOT wrong.** (C2) holds: `test/hegel/test_edist.c` asserts bound <= true distance
at 25.4 M positions with zero violations, and `bench/edist_bound.c` re-asserts it on every
page of every query. Correctness parity with seq-scan `levenshtein()` holds for 12 patterns
and for every row of the table (`sql/edist.sql`). The mutation leg that makes `block_max()`
always +INF changes **no answer**, which is the direct proof that the scan does not depend
on the bound for correctness -- only for speed it is not currently getting.

**Open question for the coordinator**, stated rather than decided: the candidates are (a)
accept it, because `<@>` is still far better than a Seq Scan computing min-over-terms
Levenshtein per ROW -- the pass computes it per vocabulary TERM, which by Heaps' law is
~sqrt(corpus); (b) store per-page min/max length in a page header and pay a format change
for I/O the current shape still performs; (c) index the dictionary by length as a secondary
structure, which is a new weft; (d) replace the bound with the universal-Levenshtein
automaton's own dead-prefix skip (`include/weave/uleven.h`), which prunes on the PREFIX
rather than on statistics and is known to work -- it is what Z5's `term~k` route already
uses. (d) is the only one of the four that needs no new bytes on disk and is not a bound at
all, which would mean `<@>` stops being a shuttle in the (C1)-(C6) sense and becomes a walk
with a cutoff.


### G34 — a cgram-bearing bolt cannot be merged — **FOUND AND CLOSED 2026-09-21; the diagnosis below was half wrong, which is the interesting part**

A merge reads its inputs' dictionaries and streams terms out of them. The cgram weft's input
is the raw column **text**, which the index does not store, so a merge cannot reconstruct the
trigram vocabulary of the output bolt. `weave_seg_mergeable()` refuses a group containing such
a bolt and `weave_merge_selected()` refuses again at its own chokepoint, so the failure mode is
"this bolt is never compacted", not a wrong answer.

**What it costs, and the honest part is that the cost is UNMEASURED.** The build bolt keeps its
cgram weft forever; bolts created by later inserts carry no cgram weft (see `G35`) and merge
among themselves normally, so the segment-directory cap is not approached by this alone. What
is not measured is read amplification on an index whose largest bolt can never be folded into a
later one.

**The fix is known and is not large.** A cgram weft is dictionary + postings in the lexical
shape, so `MergeSource` parameterized on `(dictstart, dictindexstart)` merges two cgram wefts
the same way it merges two lexical ones — the trigram keys are 4-byte big-endian and therefore
sort by `memcmp` exactly as terms do. It was left undone deliberately: Z8's gate is parity and
size, and a merge path written without a test that can see a mis-merged trigram posting list is
how a silent wrong answer gets shipped.

**WHAT WAS ACTUALLY WRONG WITH THAT DIAGNOSIS.** "A merge cannot reconstruct the trigram
vocabulary" is false, and the sentence right after it says why without noticing: a merge does
not need the column's TEXT, it needs the (trigram, docid) PAIRS, and an input bolt's cgram weft
is *precisely a container of those pairs*. The premise that made the refusal look structural
was the assumption that a weft must be rebuilt from its original input. So the fix is not the
k-way merge proposed above; it is 150 lines that read the pairs back
(`weave_cgram_merge_append()` in `src/am/ambuild.c`), drop the tombstoned docids using the same
dense bitmap the lexical merge uses, and hand them to `weave_build_cgram_weft()` — **the same
writer** the build and the flush use, which is the rule `VECTOR_CHANNEL.md` §7.3 states as "one
function writes a weft". A second writer for one format is how a merged weft starts disagreeing
with a built one.

**And it had to close with `G35`, not after it.** `G35`'s fix makes every pending flush produce
a cgram-bearing bolt. With the refusal still in place those bolts would be unmergeable
*forever*, so the segment directory would grow by one per flush until
`weave_add_segment_with_room()` ran out of retries and raised — converting a latency gap into a
**failed INSERT**. The mitigation this gap's own text relied on ("bolts created by later inserts
carry no cgram weft and merge among themselves") was `G35` being open.

**The rules that replaced the refusal**, all in `src/am/ambuild.c`:

- `weave_seg_mergeable()` takes a `cgramok` flag with the same meaning `vecok` has: what a merge
  can produce is a property of the SET of inputs. `weave_segs_cgram_agree()` computes it — every
  live bolt carries a weft, or none does.
- A **mixed** directory keeps exactly the old behaviour: the cgram-bearing bolts are excluded
  from candidate lists and everything else still compacts. An output weft covering only some of
  its bolt's documents is a false negative, so a mixed group is refused rather than merged with a
  partial weft. A mixed directory is already unaccelerated (`weave_cgram_collect()` uses no weft
  unless every live bolt has one), so the exclusion costs nothing a query can see.
- Both chokepoints (`weave_merge_selected()`, `weave_merge_group_to_seg()`) re-check, because the
  parallel merge's worker groups are formed elsewhere and "should always pass" is what G24 was
  made of.

**What is NOT fixed, and is the follow-up.** The re-accumulation is **not streaming**: the pair
array is 16 bytes per pair of the whole merged group, ~930 B per document at the 58.1
pairs/document measured in `FUZZY_CHANNEL.md` §6, so compacting a 1M-document index wants
~930 MB on VACUUM's cleanup path. It is bounded by `WEAVE_CGRAM_MAX_PAIRS` and **degrades
safely** — past the cap the accumulator sets `toobig`, the writer omits the weft with a WARNING,
and the merged bolt simply has none, which is the state every merged bolt was in before this
change. The streaming version is the k-way merge the original text proposed, and it is an
optimization; doing it *instead* of this would have meant a second writer for the format, which
is the trade that was rejected. **Unmeasured at scale** — `bench/RESULTS_CGRAM.md` §5 item 1
carries the same note.

**What can see a mis-merged posting list**, which is the objection the refusal existed to
answer: `sql/cgram.sql`'s parity sweeps, which now run against a *merged* weft. A merge that
loses a (trigram, docid) posting makes the pattern's AND exclude a document `LIKE` keeps, and
the symmetric difference then names the ids. Duplication is absorbed by `tidset_sort_uniq()` and
is harmless. The file also asserts `segments_after_merge`, which reads **2** before this change
and **1** after.

### G35 — a post-build INSERT is not indexed by the cgram channel — **FOUND AND CLOSED 2026-09-21**

`WeavePendingItem` carries the row's `wdoc` and (since `G23`) its `wvec`. It does not carry the
raw text of a `gram_ops` column, so an inserted row contributes no trigrams and is found only by
the route's fallback — correct answer, no acceleration. This is `G23` one channel over, and the
same two options apply: carry the text on the pending item (bytes, and the text is already in the
heap), or teach the flush to re-read the heap tuple. `sql/cgram.sql` asserts the correct answer
across INSERT, DELETE and VACUUM so that closing this shows up as a latency change and not as a
correctness change.

**THE COST WAS UNDERSTATED ABOVE, and the correction is the reason this was worth doing before
anything else in the channel.** It is not "that row is not accelerated". `weave_cgram_collect()`
requires **every live bolt** to carry a weft before it will use *any* of them — it must, or the
narrowed candidate set is not a superset of the answer — so ONE weft-less bolt makes every `@~`
and `@~*` query in the index fall back to a sequential heap pass. A single post-build INSERT
therefore de-accelerated the whole channel until the next REINDEX. It was never a wrong answer
(the fallback is exact, which is why `sql/cgram.sql`'s row sets passed throughout), and it was
never only one row either.

**The fix, and where the contribution is computed.** `WeavePendingItem` gains a `gramlen` word
and the raw, detoasted text after the wvec; `weave_flush_pending()` then runs
`weave_cgram_accum_add()` — the *same* producer the build callback runs — over that text. So the
trigrams are computed **at flush time, from the stored bytes**, and not at insert time. Two
reasons, the first `G23`'s and the second stronger than `G23`'s:

- **Size.** A value contributes one 16-byte pair per byte of text, so storing the pairs would
  make a pending item ~16× the size of the text it came from. The cheap thing to store is the
  input.
- **The pairs depend on the EXTRACTOR, which belongs to the weft and not to the insert** — the
  ASCII fold, the gram width, the key encoding. `G23` stored the vector raw because a code baked
  at insert time freezes the `bits` reloption that an `ALTER INDEX` can change between the insert
  and the flush; the same argument here has a worse failure mode, because a weft whose dictionary
  mixes keys from two extractors makes a pattern require a key the matching document was never
  indexed under — a dropped row, not a refused merge.

**The layout is discriminated by PAGE KIND** (`WEAVE_PK_PENDING_V10` = 33), exactly as v8 → v9
was and for the identical reason: the strides are not distinguishable from the bytes, and
`weave_insert()` does not upcast the metapage, so one index can hold all three layouts at once.
`WEAVE_VERSION` is 10; an older `.so` refuses the index rather than parsing a v10 page with the
v9 stride. `weave_pending_iter_next()` has a branch per layout, `t/019` exercises the v8 one by
manufacturing the old image, and a pre-v10 item in the chain switches the producer off for the
**whole** bolt (`gramcomplete`) — a weft over the recoverable subset would be incomplete, and
absent is safe while incomplete is not.

**The oversized-INSERT path needed the same fix and is asserted separately**, which is what
`sql/pendingvec.sql` learned for the vector half: a document too large for a pending page becomes
its own one-document bolt, and a fix to the buffer alone would have looked correct everywhere
else while leaving that bolt weft-less — which, by the paragraph above, de-accelerates the entire
index.

**The number that discriminates**, because almost nothing else does: `sql/cgram.sql`'s
`served_after_flush` (`cgram_scan` after `weave_merge()` has folded the pending buffer into a
bolt) reads **0** before the fix and **1** after. What does *not* discriminate, and is asserted
next to it so it cannot be mistaken for it: the row sets (identical either way — that is what
"absent is safe" means) and `served_while_pending` (1 either way — a pending TID is a candidate
unconditionally).

### G36 — the fused core is document-at-a-time: `score_block()` exists, is faster, and is unused — **OPEN 2026-09-20, deliberately**

`include/weave/channel.h` defines an optional `score_block()` slot for channels that can
produce a whole block's worth of candidates more cheaply than one `seek()` per position —
"the vector code scan does 32 lanes in a few SIMD instructions" — and says **"When present,
the scorer prefers it."** As of task F1 the scorer does not. `src/am/fuse.c` is
document-at-a-time only, so V8's `score_block()` is implemented, verified, and called by
nothing.

Why it was left: using it turns the loop inside out. The block becomes the unit of work,
the pivot has to be recomputed against a range rather than a position, and the
essential/non-essential partition has to be evaluated per range. Doing that before the
document-at-a-time loop was proven against brute force would have meant debugging two
algorithms at once, with the wrong-answer modes of each available to explain any
disagreement. That is the trade AGENTS.md hard rule 7 is about, made inside one task
instead of across a phase.

**The cost is unmeasured and must not be guessed.** The honest statement of what is known:
V8's kernels score 32 lanes per call, and the fused loop currently calls `score()` once per
surviving position. Whether that matters depends on how many positions survive the block
prune, which at 10^6 synthetic trials is 32.2 M block skips against 79.4 M pivots — a
synthetic ratio that says nothing about a real corpus. Measure it when F2 makes a real
query reachable; do not quote a factor before then.

Related and smaller: `channel.h` also documents the `nwarp`/`allow` bitmap-extent
obligation that `score_block()` carries, and no caller exercises it, so that contract is
asserted in V8's own property test and nowhere else.

### G37 — MaxScore's *seek* saving is not taken: every scored channel is advanced at every pivot — **OPEN 2026-09-20**

`src/am/fuse.c` advances **every** scored channel to the pivot, essential or not. That is
required for correctness — a non-essential channel that is not advanced silently drops its
contribution from every score, which is `FUSED_TOPK.md` §3a correction 3 and cost a
debugging round when F5 found it — but it is more work than a textbook MaxScore does. What
is preserved is the saving that matters for *candidates*: a non-essential channel never
generates a pivot, so the scan never visits a document only it matches, and incremental
abandonment can stop before its `score()` runs. What is given up is the seek itself.

A lazier variant is possible: defer a non-essential channel's seek until the abandonment
loop actually reaches it, and take its bound from its ceiling rather than its block until
then. That is strictly looser, so it prunes less, so it is a trade and not an improvement —
and the number that decides it (seeks avoided against blocks no longer skipped) does not
exist. **Unmeasured, and not attempted.** Recorded so that the current shape reads as a
choice rather than an oversight, and so that anyone profiling a fused scan and finding seek
cost dominant knows the lever exists.

### G38 — `count(*)`'s fast path cost O(heap pages) and was a 40× pessimization below ~df 9,000 — **FOUND AND CLOSED 2026-09-21, the same day it was opened**

**It was never a regression.** G38 was opened on an unexplained 2.2× slowdown between two
benchmark runs. The answer is that `weave_count_dictdf_fastpath()`'s gate (4) proved
whole-heap visibility by calling `VM_ALL_VISIBLE()` once per **heap block**, so the path
was O(heap pages) with a ~3.2 ns constant and **completely independent of selectivity**.
The three figures that looked like a regression are three heap sizes on one straight line:
0.43 ms at 1,076 MB, 0.95 ms at 2,357 MB, 3.81 ms at 9,238 MB.

**Measured inside ONE run, where only the term changed** (1M docs, 87,486-page heap,
c7i.2xlarge, `/scratch/pg_weave/g38.sh`, two passes agreeing to the last digit):

| query | df | before | after | general path (fast path defeated) |
|---|---:|---:|---:|---:|
| `count(*)` rare | 25 | 0.278 ms | **0.004 ms** | 0.007 ms |
| `count(*)` mid | 2,505 | 0.291 ms | **0.004 ms** | — |
| `count(*)` common | 196,785 | 0.278 ms | **0.003 ms** | 6.0 ms |
| `count(*)` no-match | 0 | 0.280 ms | **0.005 ms** | — |

Flat from df 0 to df 196,785, with `EXPLAIN (ANALYZE, BUFFERS)` reporting `shared hit=8` —
so the 0.278 ms was never I/O, it was 87,486 function calls. **A term that does not exist
in the corpus cost the same as one matching 196,785 documents.**

**Why that is a defect and not a trade.** The ordinary path answers df 25 in 0.007 ms, so
below roughly **df 9,000** on that heap the "fast" path was up to **40× slower than the
code it exists to avoid** — and the crossover moved with heap size rather than with
anything a user could see or tune.

**The fix, two parts, both in `src/am/amscan.c`:**

1. **A zero df needs no visibility proof.** Gate (3) already establishes `npending == 0`,
   so if no segment's dictionary holds the term the answer is 0 whatever the VM says.
   Returns before gate (4) runs at all.
2. **Gate (4) uses `visibilitymap_count()`**, which reads VM *pages* and popcounts, making
   the gate O(heap_pages / 32672) buffer reads instead of O(heap_pages) calls.

**The one hazard the swap introduces is checked, not argued away.**
`visibilitymap_count()` counts bits over the whole map, including any belonging to blocks
past the end of the relation, so a count that merely *equals* `nblocks` could in principle
be real pages plus stale bits — and believing it produces a **wrong count**, not a slow
one. It is unreachable (`visibilitymap_truncate()` runs inside `RelationTruncate()`'s
critical section and `XLOG_SMGR_TRUNCATE` covers heap, VM and FSM together; `TRUNCATE
TABLE` makes a new relfilenode with no VM), but `doc/CONVENTIONS.md` rule 2 exists to
distrust exactly that reasoning — so **under `USE_ASSERT_CHECKING` the authoritative
per-block scan runs and must `Assert` agreement**. A cassert build re-derives the gate
across the whole regression suite. Same arrangement as the score()-returns-distance
convention.

**The `count(*)` rows in `bench/RESULTS_LEXICAL.md` are pre-fix** (0.95 ms at 1M, 3.81 ms
at 4M) and will fall by two orders of magnitude on the next run. They are left as measured
rather than edited, with a pointer: a benchmark file records what was run.

*Original entry, kept because the hypothesis it reached was wrong in an instructive way —
it blamed the environment and the harness, and the answer was in the code all along:*

### G38 (original) — `count(*)` on a common term measured 0.43 ms on 2026-09-07 and 0.95 ms on 2026-09-21, on a corpus with FEWER matches

`bench/RESULTS_LEXICAL.md`: the common-term `count(*)` pushdown was 0.43 ms at 1M docs
with df 197,552 and is 0.95 ms at 1M docs with df 179,772 — 2.2× slower on 18 %
*fewer* matching documents, same instance type, same CPU model (Xeon 8375C @ 2.90 GHz),
same PostgreSQL major.

**What rules a cause in or out.** `pg_fts v1.8.3` measures **0.94 ms on the same table
in the same run**, and the two forks agree to 0.01 ms at both scales (3.81 vs 3.81 at
4M). pg_fts does not have L14, L17 or L18, so if any of those had regressed the count
path, pg_weave would be the slower of the two. It is not. Whatever moved is either in
the code both forks still share, or it is the environment.

**The candidate, and why it is not yet an answer.** The table is 2.2× wider than in the
2026-09-07 run — it now carries an `ftsdoc` column alongside `wdoc` and `tsvector` — so
a heap-touching plan would pay more per row. But `WeaveCount` is a custom scan that is
supposed to answer from the index alone, and "supposed to" is not a measurement. Either
it touches the heap, in which case the custom scan is not doing what its own
documentation says, or it does not, in which case the 2.2× is unexplained and the
number to distrust is one of the two.

**Cheapest next step**, and it needs no new corpus: run `count(*)` common on the same
seeded corpus with and without the extra analyzed columns and compare `EXPLAIN (ANALYZE,
BUFFERS)` heap-block counts. If `shared hit` is proportional to the table width, the
custom scan reads the heap. That is a 10-minute experiment and it should precede any
optimization, per hard rule 9.

### G39 — a query-less scan of an indexed table fails on PostgreSQL 18 when the seq scan is disabled — **OPEN 2026-09-21, found by F7's test file, and pre-existing**

`SELECT count(*) FROM t` where `t` carries a weave index, under `SET enable_seqscan =
off`, fails on PostgreSQL 18 with *"a weave index scan requires a query"*. It succeeds on
17. Nothing about it is new: the guard and its reasoning predate F7
(`weave_costestimate`, `src/am/am.c`), and F7's regression file is simply the first test
that asked for the shape.

**The mechanism, in three facts that are each individually correct.** (1) The AM cannot
serve a scan with no restriction and no ordering clause: `weave_build_callback` and
`weave_insert` skip NULLs, so a row whose indexed column is NULL has no entry and a full
scan would **undercount** — a wrong answer, not a slow one. (2) `amoptionalkey` must stay
true, because that is what lets the keyless *ordering* path exist at all (task L7, the
3,705x-at-1M win), and it also lets the planner consider an Index Only Scan for an
unqualified aggregate, since `count(*)` needs no columns and `check_index_only()`
therefore succeeds regardless of `amcanreturn`. (3) So the path is priced at 1e12 and
refused at runtime as a backstop. **PostgreSQL 18 turns the backstop into a user-visible
failure**: it compares disabled-node counts *before* costs, so a disabled seq scan loses
to a path costed at 1e12.

**Nothing here is fixable by tuning the cost**, which is the useful part: on 18 the price
is not what decides. The candidates are (a) serve the scan by falling back to a heap pass
when the AM knows it cannot enumerate — a whole mechanism for a query nobody should send
to this index, (b) get per-path veto from core, which does not exist, or (c) leave it and
document, which is what this row does. The practical cost is confined to sessions that
disable the seq scan, which is a debugging setting; the regression files that need it keep
`enable_seqscan = on` around their query-less counts and say why at the call site.

### G34, G35 — **BOTH CLOSED 2026-09-21, and the closure was proved by a pre-fix control run**

G35 (a post-build INSERT is not indexed by the cgram channel) and G34 (a cgram-bearing
bolt cannot be merged) are closed together, because G35 alone would have been a P0: every
flush would add an unmergeable bolt until `weave_add_segment_with_room()` ERRORs, i.e. a
failed INSERT.

`WeavePendingItem` carries the row's **raw gram text** (format **v10**, new page kind
`WEAVE_PK_PENDING_V10`; v9 and v8 layouts kept with their own stride helpers, and an
unrecognized kind now yields **nothing** rather than guessing the oldest stride). The
trigrams are computed **at flush time**, through the same `weave_cgram_accum_add()` the
build callback uses: pairs are ~16 bytes per byte of input text and they depend on the
extractor (fold, gram width, key encoding), which belongs to the weft being written — the
same argument V7 used for storing vectors raw, and here a frozen extractor would produce a
weft mixing two key vocabularies, which is a false negative rather than a refused merge.
A pre-v10 pending item switches the producer off for the whole bolt: absent is safe,
incomplete is not. `weave_cgram_merge_append()` reads an input bolt's weft back as pairs,
drops tombstoned docids with the lexical merge's dense bitmap, and feeds the existing
writer; `weave_seg_mergeable()` gains a `cgramok` term, and a group where only some live
bolts carry a weft keeps the old refusal.

**The recorded diagnosis for G34 was wrong** and that is worth keeping: it said a merge
cannot reconstruct the vocabulary. A merge needs the *pairs*, not the text, and the input
weft holds them.

**The proof, because "the test passes" is not it.** Every row-set assertion in
`sql/cgram.sql` passes with both gaps open — that is what "absent is safe" means, since a
weft-less bolt just falls back to a heap pass and returns the same rows more slowly. A
**pre-fix control run** (build `5ed01e6`, the new `sql/cgram.sql` against the old code,
PG17, EC2 dev host) measured `served_after_flush` **0**, `segments_after_merge` **2**, and
`served_after_oversized` **0**, against **1**, **1**, **1** after. `served_while_pending`
was *claimed* in the file not to discriminate and the control measured **0 -> 1** there
too; the wrong claim is left in place at the call site, because "this number does not
discriminate" is exactly the kind of assertion that has to be measured rather than
reasoned, and it had been reasoned.

**Two follow-ups, recorded rather than hidden.** (1) The cgram half of a merge is **not
streaming**: ~930 B/document resident, bounded by `WEAVE_CGRAM_MAX_PAIRS`, past which the
weft is omitted — the same shape as G25 for the vector half. (2) **Hard rule 12 debt:**
this touches merge, so local green is not evidence; a run at scale through `bench/aws/` is
owed before the closure is treated as durable.

### G40 — no regression test exercises a post-build, un-VACUUMed, HOT-churned heap — **OPEN 2026-09-21, found by the pg_tre 4.0.x review**

pg_tre 4.0.2 fixed `amgettuple` returning HOT-successor TIDs, and its post-mortem says the
blind spot that hid the bug for three rounds was a test suite that churned the heap only
*before* the index existed. pg_weave has the same blind spot: `sql/cgram.sql:80` runs its
UPDATE churn **before** `CREATE INDEX` at `:81`, and `:97` sets only `enable_seqscan = off`
with no arm that forces `amgettuple` specifically. pg_weave's own code is **not** defective
here — the HOT-root fix is present at `src/am/amscan.c:4014-4054` and every other TID path
inverts `weave_docid_to_tid()` from root TIDs recorded at build/insert time
(`amscan.c:6395-6405`), so it is structurally immune — but the *coverage* that would catch a
regression does not exist. What is owed: an arm that UPDATEs after the index is built,
does not VACUUM, and reads through a plan that must use `amgettuple`.

### G41 — `fuse()` ranked every DISTANCE-spelled channel BACKWARDS, and the defect was printed in a checked-in expected file — **FOUND AND CLOSED 2026-09-22 by F8**

`fuse()` sums **scores**, higher better, and negates once at the end so that ascending
is best-first (`FUSED_TOPK.md` §7a). A channel argument spelled as a **distance** has
to be recovered into a score first, which is what the planner support function does by
wrapping each recognized `OpExpr` in a recovery call (`src/am/fusepath.c`,
`weave_fuse_recover()`).

0.14.0 shipped that recovery for two operators — the lexical `<=>` and the cosine
`wvec <=> wvec` — and stopped, because at that point no vector or fuzzy channel could
be fused and the point of the functions was the rewrite. The three operators left
without one were `<->`, `<#>` and `<@>`. For each of them:

    ORDER BY fuse(body <=> 'alpha', emb <-> '[1,0,0,1]')

parsed, ran, and returned the **farthest** vectors first. Same for `<#>`, and same for
`<@>`, which ranked the worst spelling match first.

**Why no test caught it, and this is the part worth keeping.** Two independent reasons,
each sufficient:

1. **Both arms were wrong in the same direction.** The fallback computed the inverted
   sum; the pushdown, once F8 built it, would have inverted it identically. Every
   assertion this project writes about `fuse()` compares the two arms *against each
   other* — `sql/fuse_fallback.sql` §5, `sql/fuse_pushdown.sql` §2 — so no amount of
   that testing could see it. §7a had already arrived at the fix from another
   direction: assert against an **oracle** built from the index's own per-channel
   scores.
2. **It was VISIBLE, in a file three reviews read.**
   `expected/fuse_pushdown.out` contained, checked in:

       Sort Key: (fuse(weave_lexscore((body <=> '''alpha'''::wquery)),
                       (emb <-> '[1,0,0,1]'::wvec), '{1,1}'::real[]))

   One argument wrapped in its recovery function and the other bare. That asymmetry
   *is* the bug, on the page. A missing recovery function looks exactly like a
   recovery function that is not needed.

**The fix**, extension 0.17.0: `weave_l2score(d) = -d*d`, `weave_ipscore(d) = -d`,
`weave_edistscore(d) = -d`, all STRICT and total on every float8 for the reason their
two 0.14.0 siblings are. `-d*d` and not `-d` for l2 because the domain belongs to the
**channel**, not the operator: the weft scores l2 as `-||q-v||^2`, so `-d` would rank a
lone vector channel identically while weighting it differently from the index at every
distance except 1 — and a fused sum is arithmetic, not a ranking.

`weave_edistscore()` ships even though `<@>` cannot be fused until F9. The two halves
of this gap are independent: the pushdown needs a document-space shuttle, the FALLBACK
needs only the recovery function, and fixing two of three channels would be an
arbitrary place to stop.

**What the closure is asserted by:** `sql/fuse_pushdown.sql` §2b — the flagship
two-channel query's rows equal an exact oracle built from `weave_search()` plus
`weave_vec_scan()`, and a separate arm reports the *direction* on its own, because an
oracle agreement could still hide it if the oracle were built the same wrong way. On
the 40-row corpus the top 10 are ids 1..10 (the nearest vectors) where the lexical
channel pulls the other way (tf of `alpha` is `g`, so large `g` scores higher) — so an
inverted vector channel moves the answer.

**The generalization, which is not about `fuse()`:** an argument list where some
elements are transformed and others are not is a shape whose bug is invisible in
review, because the untransformed ones look like a deliberate exception. Wherever this
project maps a set of operators through a table, the test has to be that the set is
COMPLETE, not that each member it contains is right.

### G42 — `make check-standalone` could not fail: sixteen of eighteen property suites were invoked through a pipe, and one of them had been aborting for releases — **FOUND AND FIXED 2026-09-22**

Found while running the local gates for the fused-scorer counters (0.18.0). `make
check-standalone` printed `== ALL STANDALONE CHECKS PASSED ==` and exited 0 while
`test_pagekind` aborted on an assertion, with the abort message plainly visible in the
output three lines above the success banner.

**The mechanism is one character.** The recipe ran each suite as

```
$$tmp/pk | tail -1;
```

and a pipeline's exit status is the LAST command's. `tail` always succeeds, so `set -e`
had nothing to see. Sixteen of the eighteen suites were invoked that way; only
`test_kernels` and `test_lexbound` redirected to a log and checked their own status,
which is now what all eighteen do. The two spellings had sat side by side in the same
recipe for months.

**What it was hiding.** `test_pagekind`'s check 7 asserts
`all_kinds[NKINDS - 1] == WEAVE_PK_NKINDS - 1` — the one thing that notices when a page
kind is allocated and not added to the test's enumeration. Five had been:
`WEAVE_PK_PENDING_V9` (29), `WEAVE_PK_CGRAM_DICT` (30), `WEAVE_PK_CGRAM_DICTINDEX` (31),
`WEAVE_PK_CGRAM_POST` (32) and `WEAVE_PK_PENDING_V10` (33). So the assertion designed to
catch exactly this was *working*, and was silenced by the harness. With the five added,
the suite passes 852,112 exhaustive checks over all 2^16 flag words. The on-disk
consequence was nil — the encode/decode bijection holds for those ids, they simply went
unchecked — and that is the point: **the cost of this gap is not a bug it let through,
it is that the gate's green was uninformative for every release in between.**

**The fix is verified by a positive control, not by the suite passing.** The stale
`test_pagekind` was compiled from `git show HEAD:` and run through the new pattern: it
exits 1. Under the old pattern the identical binary produced a pass. A gate fix whose
only evidence is that the gate now passes is the failure this gap is about.

**A SECOND suite was also failing, and only CI could show it.** The first push with the
fixed gate went red on the `standalone` leg: `run_tre_bump.sh backref` dies with
`fatal: invalid object name 'aae7a35'`. That leg rebuilds the vendored TRE library from a
fixed historical commit to prove the backref fix changes behaviour on our own copy, and
`actions/checkout` clones at depth 1, so the object is not there. It had been failing on
**every CI run since the leg was added** and nothing said so. Locally it passes, because a
developer checkout has the history — so the two suites this gap was hiding fail in
opposite environments, and neither the local run nor the CI run alone would have found
both. The fix is `fetch-depth: 0` on that job, with a comment on it saying why, since a
shallow clone is exactly the sort of thing someone optimizes back in.

**Why it belongs in the record rather than in a quiet commit.** AGENTS.md already carries
this exact family — members eight and nine are `psql -f t.sql | head -90` killing the
process under test, and `make 2>&1 | grep error; test -f pg_weave.so && echo OK` reporting
on a stale artifact. This is the same mistake in the gate that fronts **every property
test in the project**, i.e. the gate behind hard rule 1. Every "N million checks, 0
failures" figure this project has published came through it. Those figures are not
retracted — the suites print their own totals and a suite that ran and reported is still
evidence — but a suite that ABORTED would have been reported identically, and nothing
distinguished the two until now.

### G43 — a fused scan returns a plausible wrong top-k on a real corpus, because a posting cursor reports itself exhausted one block early — **FIXED 2026-09-22 (same day). The vector channel was innocent; two hypotheses recorded below are RETRACTED in place.**

**ROOT CAUSE, found by measurement and not by the reasoning further down.**
`wand_skip_blocks()` in `src/am/amscan.c` advances a posting cursor over whole
128-posting blocks reading block HEADERS only, and concludes a block lies entirely
below the seek target when **the next block's `first_docid <= target`**. That inference
holds only while the next block belongs to the same term. Posting lists share pages, so
the header after a term's **final** block belongs to another term and its `first_docid`
is an unrelated — typically small — number. Read as this term's continuation it
"proves" the final block is below almost any target, so the block is skipped, `nread`
reaches `df`, and the cursor reports **EXHAUSTED with its last block never decoded**.

The fix is one condition: a block that holds the term's last posting is never
prove-skipped, it is decoded. `nread + count < df` — strictly less, because
`nread + count == df` is exactly the case with no trustworthy header behind it.

**The evidence, in the order it actually arrived**, because every step of it was
measurement and the three intermediate hypotheses were all wrong:

1. Weighting the vector channel down to `1e-6` left the wrong answer **unchanged**,
   which exonerated the vector channel completely and killed the abandonment
   hypothesis below.
2. Single-term queries were wrong too, so it was not OR accumulation. The terms that
   were wrong (`properties` df 211, `show` df 1012) were the ones with more than one
   posting block; the terms that were right (`dimensional` 72, `inductive` 2,
   `biomaterials` 1) all fit in one.
3. The returned top-10 was exactly the exhaustive top-10 **restricted to a docid
   prefix**, which looked like early termination — and was not. New instrumentation
   (`WeaveFuseState.stop`, reported per bolt under `pg_weave.fuse_check_bounds`) said
   `stop = EXHAUSTED` with `ceiling = 6.64 > theta = 2.28`: the global ceiling test had
   never fired. The four loop exits are indistinguishable from the outside, and hours
   went into that difference; they are now named and reported.
4. Per-channel reporting then made it trivial: `chan 0 kind=lexical cur=4294967295
   nseek=129 nscore=128` — the lexical channel had scored 128 of its 211 postings and
   parked on the end sentinel. Varying `k` over 128/512/2048 held `nscore` at exactly
   128 every time, so the 128 was the **posting block size**, not the heap.
5. A probe at the skip site printed the mechanism verbatim:
   `blk count=83 nread=128 df=211 nbfirst=1167 target=184499 islast=1 skip=1`.
   A foreign `first_docid` of 1167 "proving" that postings at docid 194098+ were below
   target 184499.

**Severity, stated precisely.** Every row returned was plausible and correctly ordered;
the answer was a correct top-k of the subset that survived. Six of ten rows were wrong
on the probed scifact query. **No fixed-expected-output test can catch this** and none
did — which is hard rule 1's whole argument, arriving from the direction of a page
layout rather than an arithmetic bound.

**Why it needed a FUSED scan to surface, which is the reachability lesson.** The skip
requires a seek target that jumps past the end of the cursor's current block while
postings remain, and only a second channel driving the pivot produces one. Instrumented,
the plain ranked path reached the header inference **zero** times on the same corpus and
the same queries, while the fused path hit it on the first query. So the defect predates
F8 and was unreachable before it — the same shape as the latency note below, and the
reason the single-channel oracle stayed trustworthy throughout (it is what proved the
fix).

**Regression coverage** is `sql/orderby.sql`'s final section, and it needs four
conditions at once — a term spanning more than one block, sorting early enough in the
dictionary that another term's blocks follow its last one, sparse relative to a second
dense term so that term's pivots land beyond its block boundaries, and varying document
lengths so a top-k oracle is well defined at all. Each was observed to hide the bug when
dropped; the first three fixtures tried reproduced nothing. Positive control: with the
guard reverted the assertion reports 128 of 130 documents, missing exactly the two
postings in the skipped final block.

**~~Still owed, and not closed by this fix:~~ CLOSED 2026-09-23.** The abandonment audit added
here (`WEAVE_FUSE_C2_ABANDON` / `fuse_audit_abandon()`) had not fired on anything, so it had
no positive control and its silence meant nothing — the twelfth-member lesson applied to
this commit's own instrument. It has now fired: **P9 in `test/hegel/test_fuse_props.c`**, a
hand-built two-channel fixture, three legs over one set of channels.

Why a generator mode could not do it, which is also why a million random trials never
reached the audit while the *prune* fired 590,910 times in 62,000 of them: the audit needs
four conditions simultaneously — a finite theta (heap full), the sabotaged channel sorting
**last** by weighted ceiling so it lands in the unscored suffix, the leading channel's block
max **above** its score at the pivot so the block prune does not take the document first,
and the sabotaged channel's true contribution large enough to carry the document back over
theta. The fixture: A at weight 1.0 scoring 1.0/0.8/0.8/0.8 over one block, B at weight 0.5
scoring 0.0/1.0/1.0/1.0 with `block_max` scaled to 0.01, k = 1.

- **bound 100× too low, `check_bounds` on** → `WEAVE_FUSE_C2_ABANDON`, naming B.
- **honest bound, `check_bounds` on** → OK, answer warp 1 at 1.3.
- **bound 100× too low, `check_bounds` off** → OK, answer **warp 0 at 1.0**. Hard rule 1 in
  one fixture: same channels, same scores, no error, a plausible answer, best document gone.

Two mutants, because a passing gate is not a positive control. Stubbing the audit call out
(the pre-fixture state) makes leg 1 return OK and P9 fails 2 checks. Disabling the
abandonment **prune** instead makes leg 1 return `C2_VIOLATION` (6) rather than OK — with
nothing abandoned, B is scored and the *per-score* check catches the same bound. That is the
sharpest available statement of why P7 never covered this: the two checks partition the
class by whether the channel was scored, and **which one fires is decided by a prune, not by
the bound**. The `test_vecbound.c` item recorded below turned out not to exist; it is
retracted in place.

---

*Original entry, kept per hard rule 13. Its diagnosis was wrong; the measurements it
records were all correct and are what the eventual root cause had to be consistent
with.*

### G43 (original) — the F8 fused vector path returns a different top-k than two non-fused paths that agree with each other, on a real corpus — **was OPEN 2026-09-22, found by the section 8 benchmark harness on its first real dataset**

Found while smoke-testing `bench/fuse.sh` against BEIR scifact (5,183 documents, 384-d
vectors, `metric = 'ip'`, PG17.11, extension 0.19.0). This is the first time any
pg_weave index has been built over a real text+vector corpus at more than 24,000 rows,
and it took one afternoon to find something 18,899,792 property checks did not.

**The observation.** For one query, three arms over the same index and the same query
vector, top-10:

```
weave_vec_scan() SRF        : 238 1478 1554 2730 2983 3217 3284 3401 3751 4956
ORDER BY emb <#> v          : 238 1478 1554 2730 2983 3217 3284 3401 3751 4956
fuse(body<=>q, emb<#>v)     : 238  880 1478 2730 2983 3217 3284 3401 3751 4956
```

Two independent non-fused paths agree exactly. The fused path substitutes **880**
(lane 894, score 0.22096) for **1554** (lane 1568, score 0.22553) — it admits a
lower-scoring document and drops a higher one. Stable across `LIMIT 10`, `20` and
`50`, so it is not the widening ladder truncating.

**What has been ruled OUT, each by measurement:**

- *The score-recovery plumbing.* `emb <#> v` returns −0.10451, `weave_ipscore()`
  recovers +0.10451, and `weave_vec_scan()` reports 0.10300 for the same document —
  the quantizer's 1.4 % error and nothing else. Not a second G41.
- *The lexical side.* The single-channel `ORDER BY body <=> q` top-10 is **identical**
  to an exhaustive `weave_search()` oracle on both probed queries.
- *Block pruning.* `weave_fuse_stats()` on the failing query: `blkskip = 0`,
  `pivots = 5183` (every document considered), and `weave_work_stats()` reports
  `vec_blocks_bound_skipped = 0`. No block was skipped by any bound.
  — **HALF OF THAT CITATION IS A TAUTOLOGY; CORRECTED 2026-09-22, see "the counter six
  documents cited" in G44 below.** `vec_blocks_bound_skipped` increments only on
  `WEAVE_VSCAN_SKIP_BOUND` (`src/vector/vecscan.c:290-292`), which needs the vector
  shuttle's own floor — set by `weave_vec_shuttle_set_threshold()`, whose sole caller is
  the `weave_vec_scan()` SRF driver. In a fused scan the floor stays at its `-INFINITY`
  init, so the counter is **structurally zero** and reports nothing about this query. The
  ruling-out still holds, because `blkskip = 0` says it and that counter *can* move.
- *The ladder and the merge-race retry.* `passes = 1`, `runs = 1`.
- *Tombstones.* `livedrop = 0` on a freshly built index.
- *A float32 tie at the cut.* The two documents' oracle scores are 5.42375 and
  5.37364 — a 0.9 % gap, not a rounding artifact.

**What is left, and it is the leading hypothesis rather than a diagnosis.**
— **RETRACTED 2026-09-22. The vector channel was not involved at all.** Weighting it
down to `1e-6` left the wrong answer bit-for-bit unchanged, which no vector-ceiling
defect can survive. The correlation recorded in this paragraph is real and was a
coincidence of this corpus: the dropped documents lived in a posting block the LEXICAL
cursor had skipped, and their vector scores happened to be the higher ones. **A
correlation observed in "every disagreement" when there are two disagreements is one
observation, not a pattern** — and it pointed at the channel that was working. What
made it seductive is that it explained the *direction* of the error; what should have
killed it in ten minutes is the weight sweep, which is the cheapest possible test of
"is this channel involved at all". Reach for the experiment that REMOVES a component
before the one that explains the symptom.

The only
prune that fired is **incremental abandonment**: 4,564 of 5,183 documents were
abandoned mid-sum on `s + csuffix[j+1] <= theta`. That test is sound only if each
`csuffix` entry is a true upper bound on the remaining channels' scores at that
document. And **in every disagreement observed, the DROPPED document had the HIGHER
vector score** — 0.103 vs 0.026 in the balanced-weight case, 0.22553 vs 0.22096 in the
vector-dominated one. A vector ceiling that is too low produces exactly that: the
document whose realized vector score would have saved it is abandoned before the
vector channel is ever scored.

**Why this could stay latent until F8, which is the part worth generalizing.** The
vector block bound **prunes 0.0 % of blocks** on the single-channel path — that is
`bench/RESULTS_BOUND_PRUNING.md`'s headline finding, ~~reproduced here as
`vec_blocks_bound_skipped = 0`~~ **[NOT REPRODUCED — CORRECTED 2026-09-22. That counter
cannot move in a fused scan (mechanism in G44 below), so the zero was structural rather
than measured. The single-channel figure stands on `bench/RESULTS_BOUND_PRUNING.md` and
`bench/RESULTS_CODE_SCAN.md:43-44` — 0.00–0.01 % even at oracle theta — and the fused
scan's own equivalent is `blkskip`, which was also 0 on this query.]**. A bound that never
prunes is a bound whose soundness is
never load-bearing, so a too-low one is unobservable. The fused scorer uses the same
bound *differently* — as a ceiling inside a suffix sum that decides abandonment — and
that use IS load-bearing. **F8 did not introduce the defect; it made a latent one
reachable.** Any bound this project computes but does not act on is in the same
position.

**And the property test has a hole that hard rule 1 names exactly.**
— **RETRACTED 2026-09-22. There is no hole; this was a grep artifact.**
`test/hegel/test_vecbound.c` tests **both** bound functions explicitly and by name:
assertion **B1** compares `weave_block_bound_ip()` against an independently computed
exact reconstructed inner product, and **B3** compares `weave_block_bound_l2()` against
an independently computed exact L2 similarity, in that sign convention, over every live
lane of every generated block. The file contains **zero** occurrences of the word
"metric" and never mentions `WeaveMetric` — which is what was searched for, and which is
why a file with full coverage read as a file with none.

**The lesson is the same one already learned about limited greps, from the other
direction: absence of a VOCABULARY is not absence of COVERAGE.** A test names the
functions it calls, not the concept the caller uses to choose between them. Grep for the
symbol under test, and when the answer is "no coverage at all", read the file before
believing it — especially when the conclusion creates work.

*What the original paragraph said, and it was wrong:*
`test/hegel/test_vecbound.c` is the (C2) test for this bound and **contains no mention
of a metric anywhere**, while `include/weave/quantize.h` has two bound functions —
`weave_block_bound_ip()` (line 535) and `weave_block_bound_l2()` (551, which is built
on top of the ip one). So one metric is tested implicitly and the other is not tested
at all, on a code path where l2's correctness does not imply ip's. Rule 1 says a
channel without a (C1)+(C2) property test is not merged; this is the subtler version,
a test that covers a channel but not a *configuration* of it.

**The (C2) check was made reachable and RAN, and the result is a refinement rather than
an answer.** — **RETRACTED 2026-09-22: IT NEVER RAN.** The
`DefineCustomBoolVariable("pg_weave.fuse_check_bounds", ...)` call was placed inside the
`#ifdef WEAVE_TEST_HOOKS` block in `src/am/customscan.c`, and nothing in the Makefile,
meson or flake defines that macro — so the GUC **did not exist in any build anyone
runs**. `SET pg_weave.fuse_check_bounds = on` was then accepted as a *placeholder*
custom GUC and `SHOW` echoed back `on`, so the check read as enabled while
`st.check_bounds` stayed 0 for the whole run. The reasoning below about what the check
is blind to is still correct and the audit it asks for was built; what is retracted is
the evidence, because there was none. **An absent GUC is indistinguishable from a GUC
that is off** unless you look in `pg_settings` — where a placeholder has no
`short_desc`, and where the library must be LOADED in that session or the view is empty
either way and tells you nothing. This is the same family as G42 one day earlier: a
diagnostic needs a positive control exactly as much as a gate does, because until it has
fired once, its silence is not evidence of anything.

*What the original paragraph said:* `st.check_bounds` was wired to `USE_ASSERT_CHECKING` alone, so the check
that names the offending channel sat behind a PostgreSQL rebuild on the one machine
where the bug was in hand. It is now also a GUC — `pg_weave.fuse_check_bounds`, off by
default, `PGC_USERSET` — because a correctness check reachable only by recompiling the
server is a check nobody runs at the moment they need it.

With it **on**, the reproducer raises nothing and still returns the wrong row.

**And that is why it does not clear the bound: the check is structurally blind to the
prune that is dropping rows.** `check_bounds` compares `score()` against `cbound` for
channels the scorer actually SCORES. Incremental abandonment means the remaining
channels are never scored — that is what abandoning is — so a `cbound` too low for a
channel whose score is never computed has nothing to be compared against. The one prune
that fired on this query is the one prune this check cannot audit. The two facts fit
together rather than contradicting: a too-low vector `cbound` would both cause the
abandonment and escape the assertion.

**So the decisive step is a check that does not exist yet: verify the PRUNE, not the
bound.** When abandonment fires, compute the remaining channels' scores anyway and
assert that `s + actual_remaining <= theta` really held. That is strictly stronger than
the present assertion; it is sound only as a diagnostic mode, since it defeats the prune
it audits, so it belongs behind the same GUC; and it would convert this gap into a named
channel in one run. `test/hegel/test_fuse_props.c` should grow the same property against
the synthetic channels, where it is cheap — P4 already proves the fused answer equals
the reference, but **a reference built from the same too-low bound agrees with it**,
which is how a synthetic suite misses this class entirely. That is the sharpest lesson
here: `weave_fuse_reference()` shares the channels' bounds with the scorer, so it can
only catch scorer bugs, never bound bugs.

Then extend `test_vecbound.c` over both metrics before touching anything.

**Consequence for the benchmark, recorded because it is the reason this was found.**
— **SUPERSEDED 2026-09-22: the gate is now GREEN and `bench/RESULTS_FUSE.md` is
unblocked.** `bench/fuse.sh`'s correctness gate passes 25 of 25 judged scifact queries
against the exhaustive per-channel oracle, with 0 mismatches, and the harness now runs
end to end through its plan assertion, quality pass, work counters and latency pass. No
EC2 run had been spent on the broken state, which is the outcome hard rule 8 exists to
produce. The paragraph below stands as written for the period it described.

`bench/RESULTS_FUSE.md` is **not** being produced from this state. Hard rule 8: verify
correctness before recording a latency, and a benchmark of a broken fast path is worse
than no benchmark. The harness stays, its correctness gate stays red, and no EC2 run
was spent. The gate that caught this is `bench/fuse.sh`'s per-query comparison against
an exhaustive per-channel oracle — which is only an oracle because
`sql/fuse_pushdown.sql` section 2b had already worked out that the `fuse()` fallback is
not one (`FUSED_TOPK.md` section 7a (1)).

### G44 — the fused scorer's linear sum of RAW channel scores is a worse ranking function than RRF, and the gap grows with how much the dense channel matters — **FIXED 2026-09-22, the same day it was opened: the per-key ceiling normalizer is IN THE PRODUCT, on by default, and measured there. `FUSED_TOPK.md` §8b's nDCG@10 row is now MET — AND THE FIX HAS A MEASURED PRICE, RECORDED 2026-09-22 (night): it costs +19 % / +3 % / +104 % of p50, and §8b's p99 row goes from PASS to FAIL, so the gate is 2 of 5 either way and the change TRADED p99 FOR nDCG. Its p50 and `score()`-call rows also still FAIL. DECIDED 2026-09-22: the normalizer STAYS ON BY DEFAULT and the work row is RESTATED PER CHANNEL — nothing in this entry awaits the maintainer any more, and neither decision moves the gate off 2 of 5.** Found by the first real-corpus §8 run; the original measurement, the study that produced the fix and the two things that study got wrong are all left below, annotated in place.

`bench/RESULTS_FUSE.md`. nDCG@10, fused vs RRF `k'=100`, same index, same
embeddings, nothing differing but the scorer:

| dataset | docs | fused | RRF | ratio |
|---|---|---|---|---|
| scifact | 5,183 | 0.6720 | 0.6846 | 0.982× |
| nfcorpus | 3,633 | 0.3161 | 0.3422 | 0.924× |
| fiqa | 57,600 | 0.2393 | 0.3482 | **0.687×** |

recall@100 is worse too (0.934×, 0.894×, 0.742×), so this is not a top-10 cut artefact.

**It is not a correctness defect and the recall row proves it:** the fused scan returns
its own top-k *exactly*, 1.000 against an exhaustive per-channel oracle on all three
datasets. The objective is what loses.

**Mechanism, and it is arithmetic rather than subtle.** `weights => '{0.5,0.5}'` sums
**raw, unnormalized** channel scores. BM25 is unbounded and reaches ~10–20 on these
corpora; a quantized inner product lives in ~[−1, 1]. Measured on scifact: max BM25
≈ 10.0 against max vector score ≈ 0.305, a **33× scale mismatch**. At equal weights the
vector term cannot reorder the BM25 ranking by more than a nudge, so the fused arm is
**effectively lexical-only** — and it is competing against a control that is scale-free
by construction, because RRF ranks on reciprocal *rank* and never sees a channel's units.

**The evidence that this is the mechanism rather than a story that fits:** the deficit
tracks how much the dense channel should contribute. fiqa is where dense retrieval
carries the most signal in this set and is where the fused arm loses 31 %; scifact, where
BM25 alone is strong, is within 2 %. A per-dataset trend across a 16× size range is what
makes the diagnosis testable rather than decorative.

**Why this is the most consequential gap in the project right now.** `ARCHITECTURE.md`
§9 claim 2 is fused-threshold top-k *instead of* over-fetch-plus-RRF. Being faster at
computing a worse objective does not support that claim — a user is choosing a ranking,
not a scan strategy. **The claim is not retracted, because the mechanism it names works
(see the latency and lexical-pruning rows); it is UNSUPPORTED until the objective is
competitive.** Do not quote claim 2 as measured.

**AMENDED 2026-09-22 by the implementation at the end of this entry, and the amendment does
not license the claim.** The objective *is* now competitive — ceiling-normalized nDCG@10
beats RRF on 3 of 3 corpora in the product — so the reason claim 2 is unsupported has
changed rather than gone: it is now the p50 row (0.795× on nfcorpus against a 0.50× gate)
and the `score()`-call row (0.903× against 0.20×). A user choosing a ranking now has a
reason to choose this one; the claim that the scan strategy is *also* the cheaper one is
still unmeasured for the shipping default, because the normalizer's own cost has not been
timed. Still do not quote claim 2 as measured.

**What it needs, and what it does not.** Not scorer work: per-channel score
normalization or calibration *before* the sum. Options, none measured:
  - normalize each channel to a comparable range from statistics the index already keeps
    (the lexical channel knows its term-wide ceiling; the vector channel knows its
    per-block max), which is attractive because (C2) survives any *monotone positive*
    rescaling — `w·bound ≥ w·score` needs only `w > 0`;
  - fit weights per dataset, which is honest only if reported as fitted and is not a
    product answer;
  - a rank-based fused objective, which would keep RRF's scale-freeness but gives up the
    threshold algebra §2 is built on, since a rank is not known until the scan ends.
The first is the only one that preserves the design. **It is also the one that makes
V17's selectivity switch and claim 3 meaningful**, since neither matters if the ranking
is not competitive.

**MEASURED 2026-09-22, same day: NORMALIZATION IS THE ANSWER, and it overturns the
result rather than merely closing it.** Before writing any C — hard rule 9 — the
candidate objectives were scored offline from the index's **own** per-channel scores
(`weave_search()` and `weave_vec_scan()`, exhaustive), through the same run-file and
`bench/ndcg.py` path the real arms use, so no scheme can win here by being measured
differently. nDCG@10:

| dataset | raw sum (today) | RRF (control) | **maxn** | mmn | maxn ÷ RRF |
|---|---|---|---|---|---|
| scifact | 0.6720 | 0.6846 | **0.7182** | 0.7189 | **1.049×** |
| nfcorpus | 0.3161 | 0.3422 | **0.3444** | 0.3208 | **1.006×** |
| fiqa | 0.2393 | 0.3482 | **0.3556** | 0.3348 | **1.021×** |

`maxn` divides each **key** by its realized per-query maximum; `mmn` is per-key min-max.
**The raw sum loses to RRF on 3 of 3; `maxn` beats it on 3 of 3.** MRR@10 moves the same
way and recall@100 is at parity, so the gain is concentrated where nDCG@10 measures it.

**Positive control for the study itself:** the `raw` and `rrf` arms reproduce the recorded
EC2 numbers to four decimals on all three datasets (0.6720/0.6846, 0.3161/0.3422,
0.2393/0.3482). A study that could not reproduce the thing it claims to improve would be
measuring something else. This also means the study needs no EC2: nDCG is deterministic
and host-independent, and **only latency needs a quiet machine** — which is why this cost
nothing.

**`mmn` IS REJECTED, and the reason is a semantic one worth keeping.** It wins on scifact
and loses on the other two (0.937×, 0.962×). Min-max shifts each channel's floor to the
corpus minimum, which destroys BM25's "an absent term contributes exactly **0**" — every
non-matching document gets lifted off the floor. Dividing by the max keeps 0 as 0. So the
scheme that respects the channel's own semantics is the one that generalizes, and the
textbook choice is the one that does not.

**THE NORMALIZER IS PER KEY, NOT PER CHANNEL, and getting this wrong would damage BM25.**
A lexical `fuse()` argument expands to **one channel per query term**. Normalizing each
channel by its own ceiling would rescale terms relative to each other — i.e. it would
partially undo idf weighting, which is the thing BM25 is for. All channels arising from
one `fuse()` argument must therefore share one normalizer. The algebra survives either
way (each channel's effective weight is still a positive constant, so (C2), the suffix
sums and the MaxScore partition are untouched), which is exactly why this would have been
easy to get wrong and hard to notice.

**WHAT IS NOT YET ESTABLISHED, and it is the whole implementation risk.**
**[ANSWERED 2026-09-22 by the product measurement at the end of this entry: the median
1.37× relative misweighting costs nothing the gate can see. The ceiling-normalized arm, in
the product, beats RRF on nDCG@10 on 3 of 3 corpora and beats the realized-max `maxn`
study figure on 2 of 3. The paragraph stays as written because the risk was real when it
was written, and because the proxy-versus-product comparison at the end of the entry is
only readable against it.]** `maxn` uses the
**realized** maximum, which a single-pass threshold scan cannot know before it starts. The
obvious pre-scan substitute is the key's **ceiling** (the sum of its channels' `maxscore`),
and measured against the realized maximum over 25 scifact queries:

| | median | range |
|---|---|---|
| lexical ceiling ÷ realized max | 2.07× | 1.34–4.55× |
| vector ceiling ÷ realized max | 1.52× | 1.20–2.86× |
| **ratio of the two** (what distorts the ranking) | **1.37×** | 0.88–2.09× |

Equal looseness cancels — it is a common factor — so only the **ratio** matters. Ceiling
normalization would therefore leave a median **1.37×** relative misweighting against the
raw sum's **33×**: a 24× improvement, not an exact fix. Whether 1.37× costs nDCG is
measurable and unmeasured.

Two facts make this tractable rather than a dead end:

  1. **The vector ceiling is a CONSTANT 1.0107** across all 25 queries, because the
     vectors and the query are L2-normalized and the metric is ip, so the ceiling is ~1.0
     while the realized max is the best cosine (0.35–0.85 here). All of the distortion is
     the lexical ceiling, whose looseness comes from assuming every term hits its max tf
     in the shortest document *simultaneously* — an assumption that gets worse as term
     count grows, which is why fiqa's 8-term queries are the loose end of the range.
  2. **The normalizer does NOT have to be an upper bound.** `include/weave/fuse.h` note 3
     requires only that a weight be **positive and finite**; nothing in §2's algebra needs
     a normalized score to be ≤ 1, and the ceiling still bounds the normalized score
     correctly because it is divided by the same constant. So the normalizer may be a
     *statistical* estimate of the realized max — for the lexical key, the contribution at
     max tf and **average** doclen rather than at |D| → 0 — which is far closer to the
     realized maximum and is still known before the scan. This is the design freedom that
     makes a one-pass fix plausible, and it was not obvious: every other constant in this
     design is a bound, so the reflex is to reach for one here too.

**MEASURED, and the cheap one-pass scheme is the ANSWER — it beats RRF on 3 of 3 and
beats the realized-max scheme on 2 of 3.** `bench/normsweep.sh` sweeps the key-vs-key
weight ratio after max-normalization, evaluating every ratio from ONE materialized pair of
exhaustive scans per query (7 ratios for the cost of 1; on fiqa the naive shape would have
taken three hours). nDCG@10:

| lex:vec ratio | scifact | nfcorpus | fiqa |
|---|---|---|---|
| 0.25 | 0.6986 | 0.3333 | **0.4026** |
| 0.50 | **0.7194** | 0.3449 | 0.3965 |
| **0.73 — the ceiling proxy** | **0.7133** | **0.3489** | **0.3763** |
| 1.00 — `maxn` | 0.7182 | 0.3444 | 0.3556 |
| 1.37 | 0.7190 | 0.3431 | 0.3347 |
| 2.00 | 0.7118 | 0.3368 | 0.3160 |
| 4.00 | 0.6998 | 0.3264 | 0.2807 |
| *RRF control* | *0.6846* | *0.3422* | *0.3482* |

**THE CEILING PROXY IS RATIO 0.73, NOT 1.37, AND GETTING THAT DIRECTION WRONG INVERTS THE
CONCLUSION.** The lexical ceiling is looser than the vector ceiling (2.07× vs 1.52×), so
dividing each key by its own ceiling shrinks the *lexical* side more, and the effective
ratio moves **below** 1: `1/(2.07/1.52) = 0.73`. The first reading of this table took the
proxy to be 1.37 — where fiqa scores 0.961× RRF, i.e. a loss — and would have rejected the
implementable scheme on the strength of an arithmetic slip. A looseness ratio is a
divisor, not a multiplier.

At ratio 0.73 — what dividing each key by its pre-scan ceiling actually does:

| dataset | RRF | raw sum (today) | **ceiling-normalized** | vs RRF |
|---|---|---|---|---|
| scifact | 0.6846 | 0.6720 | **0.7133** | **1.042×** |
| nfcorpus | 0.3422 | 0.3161 | **0.3489** | **1.020×** |
| fiqa | 0.3482 | 0.2393 | **0.3763** | **1.081×** |

So the fix needs **no second pass, no new statistics and no new on-disk state**: it divides
each key by a constant the scan *already computes* for the MaxScore partition. On nfcorpus
0.73 is the best point in the whole sweep, and on fiqa it beats `maxn` outright.

**Why that is luck as much as design, and it must not be written up as though it were
not.** The ceiling's looseness happens to push the ratio toward *more vector weight*, and
more vector weight is what all three of these corpora want (their best points are at 0.25,
0.50 and 0.73 — every one below equal). On a corpus that wanted more *lexical* weight the
same looseness would push the wrong way. The scheme is validated **empirically on three
datasets**, not derived.

**AND THE THIRD DATASET OVERTURNED THE SECOND'S CONCLUSION, which is hard rule 11 arriving
in person.** scifact swings 3.0 % across the whole 16× ratio range and nfcorpus 6.9 %, so
after two datasets the honest-looking summary was "nDCG is flat, the `weights` knob is
forgiving, any reasonable ratio beats RRF". **fiqa swings 43.4 %**, is monotone decreasing
across the entire range, and drops below RRF at ratio 1.37. That conclusion would have been
published off two datasets and been wrong. *The knob is forgiving on some corpora and sharp
on others, and which one you have is not knowable from the ranking alone.*

**Still unmeasured, and stated rather than glossed:** fiqa's optimum is at or below the
lowest ratio swept (0.25, at 1.156× RRF) and the curve is *still falling* at that edge, so
the sweep does not contain fiqa's best point. Extending the range would say how much is
being left on the table by an equal-weight default, and would probably say the default
should not be equal. That is a tuning question, and tuning is dataset-specific; what
matters for the gate is that the equal-weight, ceiling-normalized default beats RRF
everywhere measured.

**Implementation, now fully specified by measurement.** Per `fuse()` KEY (not per channel),
divide by the sum of that key's channels' `maxscore`. Each channel's effective weight
becomes `w_key / N_key`, a positive finite constant, so (C2), the suffix sums and the
partition are untouched — and each key's ceiling then equals exactly its weight, which
makes `suffix[0] = Σ w_key` and the weights directly interpretable as relative influence
for the first time.

**[IMPLEMENTED 2026-09-22 exactly as this paragraph specifies, plus ONE CONSTRAINT it does
not mention and which turned out to be the day's real finding: the normalizer has to be one
constant for the whole QUERY, not one per bolt. `doc/specs/FUSED_TOPK.md` §8d is the
specification as built; the narrative is directly below.]**

**Latent because every prior test used one channel or a fixture.** A single-channel
ranking has no scale to mismatch, and `sql/fuse_pushdown.sql`'s fixtures assert the
scorer computes *what it says it computes*, which it does. No fixed-output test can see
"the objective is worse than a different objective" — that needs a labelled corpus, and
this project had never run one until today.

---

**IMPLEMENTED AND MEASURED IN THE PRODUCT, 2026-09-22.** The specification of what shipped
— the rule, the two `N_key` derivations, the guard, the GUC and the per-bolt constraint —
is `doc/specs/FUSED_TOPK.md` §8d and is not repeated here. What is here is the narrative:
the finding the study did not contain, the test and its positive controls, the product
measurement, the loss, and how well the study predicted the product.

Briefly, so this entry reads on its own: every channel of a `fuse()` key now carries
effective weight `w_key / N_key`, where `N_key` is the key's pre-scan ceiling — for a
lexical key the sum of `weave_bm25_term_bound()` over its terms at each term's max tf
**over all segments**, accumulated in the `src/am/amscan.c` loop that already reads every
segment's dictionary entry for the global idf, so at zero extra I/O; for a vector key the
maximum over bolts of `weave_vec_weft_maxscore()` (`src/vector/vecshuttle.c`), which is
bound (B2) over one directory pass plus `weave_vec_scan_maxscore()` for the metric's
domain. `w/N` is positive and finite, which is all `include/weave/fuse.h` note 3 asks, so
nothing in the algebra moved. `pg_weave.fuse_normalize` (`PGC_USERSET`, default **on**,
defined outside any `#ifdef` — AGENTS.md's twelfth member) turns it off and restores the
raw sum.

**THE FINDING THAT WAS NOT IN THE SPEC, AND IT IS THE MOST IMPORTANT PART OF THIS ENTRY:
the normalizer has to be ONE CONSTANT FOR THE WHOLE QUERY, NOT ONE PER BOLT.**
`weave_fuse_pass()` runs one bounded top-k **per bolt** and merges the per-bolt lists **by
score** — `src/am/amscan.c` sorts the accumulated rows and truncates to the pass width,
under a comment asserting that merging exact per-bolt top-k lists yields an exact global
top-k. That merge is exact only while every bolt scored against the *same objective*.
`WeaveShuttle.maxscore` is per bolt, so the obvious implementation — read the ceiling off
the shuttle you just opened — would rank each bolt against a different objective and make
the answer **a function of the segment layout**: it would change after an INSERT, after
VACUUM and after a merge, with no error anywhere and plausible output every time. That is
why the maximum is taken before the first bolt is scanned, and why
`weave_vec_weft_maxscore()` exists at all instead of a read of `sh->maxscore`. Note also
that the studies above measured a query-global normalizer — they normalized in SQL over the
whole corpus — so a per-bolt implementation would not even have been the thing that was
measured. This is the same shape as G43 and as the whole "plausible wrong answer" family:
the defect has no error message and no wrong-looking output, and its trigger is a
maintenance operation rather than a query.

**THE TEST, AND ITS POSITIVE CONTROL.** `sql/fuse_degenerate.sql` section (6): 300
documents with `'alpha'` at a tf unique per document plus `'beta'` in all of them, index
built; then a second, VACUUM-flushed batch of 300 with the same `'alpha'` ladder and **no
`'beta'`** — a run of two segments is below the tiered auto-merge threshold, so both
persist. The query is `fuse(d <=> 'alpha beta', d <=> 'gamma', weights '{1,1}') LIMIT 10`,
taken before and after `weave_merge()` and compared as an id **set**. A mutant build whose
normalizer used each bolt's own ceilings returns `{1..9,308}` before the merge and
`{1..9,11}` after it, so the assertion reads **f**; the shipping build reads **t**, twice
in a row. Expected output regenerated and verified line by line.

**THREE EARLIER FIXTURES COULD NOT FAIL, and that is the lesson worth keeping.** (a) A
two-key query over an 80-document index and (b) a 4,000-row index built under
`maintenance_work_mem = '1MB'` both had exactly **one** segment — `nseg = 1` — so "per
bolt" and "per query" were the same thing and the mutant passed. The test now asserts
`weave_index_nsegments() > 1` before it asserts anything else, so that can never be silent
again. (c) A genuine three-bolt fixture whose bolts differed in max tf (2, 8, 1) **also
passed under the mutant**, and the reason is BM25 itself: the term bound at tf=1 and at
tf=8 differs by about **25 %, not 8×**, because of tf saturation. What separates a
per-bolt normalizer from a per-query one is **a query term present in one bolt and absent
from another** — then the key's ceiling is a sum over a *different number of terms* in each
bolt and the scale moves by a factor rather than a percent. Generalization, and it is not
specific to normalizers: **when you design a fixture to discriminate two implementations,
ask which quantity actually differs between them and by how much.** Three of four fixtures
here were sensitive to the wrong quantity, and two of the three were additionally testing a
one-bolt index.

**A SECOND POSITIVE CONTROL, UNPLANNED, AND IT HAD BEEN RED IN A CHECKED-IN EXPECTED FILE
SINCE THE FILE WAS WRITTEN.** `sql/fuse_degenerate.sql`'s own control assertion
`weights_change_the_answer` was recorded as **f**, contradicting the comment directly above
it — "The weighting has to MATTER, or every assertion above would pass against a scorer
that ignored the weights entirely". The mechanism, measured: with the raw sum, `'zeta'`
(2 of 40 documents, so a large idf) dominated so thoroughly that `0.01 × zeta` still
outranked `0.99 × alpha`. The weights were being applied correctly and **could not reach
the ranking**. With the normalizer the 0.99/0.01 arm returns `{34,38}` — the two highest-tf
`'alpha'` documents — and the control passes. A new section (5a) pins the raw behaviour
under `pg_weave.fuse_normalize = off`, so both objectives are now tested rather than one.
This is the eleventh and twelfth members' lesson arriving from a third direction: the file
recorded its own control failing, in a file that was reviewed and committed, and nobody
read the value against the sentence above it.

**MEASURED IN THE PRODUCT, not in a study.** `bench/normprod.sh`: quality only, **no
EC2** — nDCG is deterministic and host-independent, and only latency needs a quiet machine.
Three arms that are **the same statement** differing only in the GUC, plus the RRF control,
all scored through `bench/ndcg.py`. 100 % of the corpora are the real MiniLM BEIR sets from
the 2026-09-22 EC2 run, restored into local databases. nDCG@10:

| dataset | raw sum (normalizer off) | RRF control | ceiling-normalized | norm ÷ RRF |
|---|---|---|---|---|
| scifact (300 q) | 0.6720 | 0.6846 | **0.7212** | **1.053×** |
| nfcorpus (323 q) | 0.3161 | 0.3422 | **0.3455** | **1.010×** |
| fiqa (648 q) | 0.2393 | 0.3482 | **0.3878** | **1.114×** |

**Positive control of the harness, and it did NOT pass the first time.** The raw arm
reproduces the recorded EC2 nDCG to four decimals on all three (0.6720 / 0.3161 / 0.2393)
and the RRF arm reproduces 0.6846 / 0.3422 / 0.3482. On the first run scifact's RRF read
**0.6834** instead of 0.6846, because `normprod.sh`'s copy of the RRF generator defaulted
`RRFK=100` where `bench/fuse.sh` uses 60. A 0.18 % error, caught immediately by the
control, that would otherwise have quietly re-baselined the comparison in the fix's favour
— and note that it was in the *control* arm, which is the arm nobody inspects when the
headline number looks good.

**recall@100 moves the same way, so this is not a top-10 reshuffle:** scifact 0.8892 →
0.9683 and fiqa 0.5141 → 0.7079, against RRF's 0.9517 and 0.6932.

**THE LOSS, recorded as prominently as the wins (hard rule 8): ON NFCORPUS THE NORMALIZED
ARM LOSES TWO OF THREE METRICS TO RRF.** recall@100 **0.3206 vs 0.3251** and MRR@10
**0.5441 vs 0.5514**, while winning nDCG@10 0.3455 vs 0.3422. The gate row is nDCG, so the
row is met — but the win on that dataset is **1.0 %** and it comes with two regressions.
This must not be presented as a clean sweep: it is two clear wins and one draw that the
gate scores as a win.

**THE STUDY'S PREDICTION AGAINST THE PRODUCT'S MEASUREMENT.** The ceiling proxy (ratio
0.73, derived above) predicted 0.7133 / 0.3489 / 0.3763. The product measures 0.7212 /
0.3455 / 0.3878: **direction right on 3 of 3, magnitude within 1–3 %**, and on nfcorpus the
product is slightly **worse** than the proxy predicted (0.3455 vs 0.3489). A proxy that
gets the direction right on three datasets and the magnitude to a few percent is a good
proxy. It is not the measurement (hard rule 11), and the product is now the measurement —
which is also why the sweep numbers above stay marked as a study rather than being quietly
upgraded.

**WHAT IS STILL OPEN, and none of it is closed by this work.**

  - **The p50 latency row still FAILS** (gate ≤ 0.50×; measured 0.582× / 0.795× / 0.578×)
    and **the `score()`-call row still FAILS** (gate ≤ 0.20×; measured 0.648× / 0.903× /
    0.541×). The mechanism is unchanged and is not the objective: the vector block bound
    prunes nothing (~~`vec_blocks_bound_skipped = 0` on all three datasets~~ **[CITATION
    CORRECTED 2026-09-22: structurally zero in a fused scan; the evidence is
    `bench/RESULTS_CODE_SCAN.md:43-44` and (B2) ≈ 1.0 by construction — see the dated
    work-counter block below]**) and 71–87 % of
    all fused `score()` calls are the vector channel, so the vector side sets the ratio no
    matter how well the lexical side prunes. Nothing about normalization touches that.
    — **AMENDED 2026-09-22 by the work counters below: "nothing about normalization touches
    that" is wrong in both directions.** The normalized arm's totals are 0.571× / 0.875× /
    0.513× (better on all three, still failed), its lexical side is 0.203× / 0.380× /
    0.052×, and its vector side is exactly **1.000×** with `blkskip` collapsed. The row
    fails either way; with the normalizer on it fails for a *different* reason, which is
    the dense-channel ceiling property in `FUSED_TOPK.md` §8d.
    — **RE-MEASURED ON EC2 2026-09-22 (night): the p50 figures in this bullet are the raw
    arm's. The shipping arm is 0.710× / 0.827× / 1.172× — worse on every corpus, and on fiqa
    slower than the control — and the p99 row, which passed here, now FAILS at 0.710× /
    0.612× / 1.000×.** Dated block at the end of this entry.
    — **RESTATED 2026-09-22 (maintainer decision): the `score()`-call row no longer exists in
    this unit.** It is now lexical **BM25 contributions** (0.203× / 0.380× / 0.052×, gate
    ≤ 0.20×, **MET on fiqa**) plus vector **code blocks read** (**1.000×** everywhere, gate
    missed) plus pivots per query, reported and not gated. The verdict is unchanged — in blocks
    the vector ratio is exactly what it was in lanes — so this bullet stays open on the vector
    half alone. Dated block at the end of this entry.
  - **The normalizer's own cost is UNMEASURED.** One LUT build and one directory pass per
    bolt per vector key, ahead of the first bolt. Kilobytes of directory against megabytes
    of codes is a reason to expect it to be small, not a measurement; no latency figure has
    been taken since the change, so `FUSED_TOPK.md` §8's p50/p99 rows describe the
    pre-normalizer build and need an EC2 re-run before they are quoted for the default.
    — **CLOSED BY MEASUREMENT 2026-09-22 (night), AND THE GUESS IN IT IS CORRECTED: the cost
    is +19 % / +3 % / +104 % of p50, and it is NOT the pre-scan pass.** See the dated EC2 block
    at the end of this entry. "Kilobytes against megabytes" was a sound argument about the
    pass and an unsound one about the total, because the pass is not the component that got
    slower — the **pivot walk** is.
  - **fiqa's optimum ratio is still at or below the lowest ratio swept**, so the
    equal-weight default is probably not optimal there even now. Unchanged by this work.
    — **CLOSED BY MEASUREMENT 2026-09-22, and OVERTURNED: in the product fiqa PEAKS at
    r = 0.5** (nDCG@10 0.4056) and falls off below it — 0.3968 at 0.25, 0.3817 at 0.125,
    0.3766 at 0.0625. The original sentence stays because its axis was a different axis and
    both readings are right on their own: the `bench/normsweep.sh` study table above applied
    its ratio *after* realized-max normalization, the product divides by the **ceiling**, and
    the ceiling proxy is 0.73 in study units — so product r = 0.5 is study r ≈ 0.37, and the
    study's 0.4026 at study-r 0.25 sits between the product's 0.3968 at 0.25 and 0.4056 at
    0.5. On that mapping the two agree and neither curve is still falling at its low end.
    Sweep and reconciliation in the dated block below.

**Local gates green on the change:** `installcheck` pg17 and pg18, `tap-pg17`,
`make check-standalone`, and the four lint targets. Per hard rule 12 that is not evidence
at scale — and the thing that needs scale here is exactly the thing not re-measured, which
is latency.

---

**MEASURED 2026-09-22, LATER THE SAME DAY: THE WORK COUNTERS. Normalization improved the
gated total on all three corpora and the `score()` row still FAILS by 2.6–4.4×, and the
reason is now a property of the code page layout rather than a tuning question.**
`bench/normprod.sh`, locally, no EC2: a count of `score()` calls is **deterministic**,
which is the same argument that bought the nDCG arms their local run. Real MiniLM BEIR
corpora (scifact 5,183 docs / 300 queries, nfcorpus 3,633 / 323, fiqa 57,600 / 648), fused
arm over the RRF control, gate ≤ 0.20×:

| dataset | arm | lexical | vector | total | fused `blkskip` |
|---|---|---|---|---|---|
| scifact | raw | 0.353× | 0.991× | 0.648× | 97,028 |
| scifact | **norm** | **0.203×** | **1.000×** | **0.571×** | 0 |
| nfcorpus | raw | 0.581× | 0.985× | 0.903× | 4,339 |
| nfcorpus | **norm** | **0.380×** | **1.000×** | **0.875×** | 88 |
| fiqa | raw | 0.149× | 0.956× | 0.541× | 3,444,538 |
| fiqa | **norm** | **0.052×** | **1.000×** | **0.513×** | 6,732 |

**Positive control:** the `raw` rows reproduce the recorded EC2 work numbers exactly on all
three corpora (0.353 / 0.991 / 0.648, 0.581 / 0.985 / 0.903, 0.149 / 0.956 / 0.541), so the
local harness is counting what the paid run counted.

**The win and the loss, side by side because they are the same change.** The lexical side
improved a lot — fiqa **0.149× → 0.052×**, one nineteenth of the WAND control's BM25
contributions — and the gated total improved on all three. **The other two columns went the
wrong way: the vector side became exactly 1.000×, and range skipping collapsed** (fiqa
3,444,538 → 6,732 skipped ranges, scifact 97,028 → 0, nfcorpus 4,339 → 88). The gate is
missed on all three: the vector channel is **74–87 %** of the fused total, so it sets the
ratio no matter that the lexical half now clears the gate on fiqa by a factor of four.

**THE COUNTER SIX DOCUMENTS CITED AS EVIDENCE IS A TAUTOLOGY IN A FUSED SCAN. This is a
correction of REASONING, not a reversal of a result.** `vec_blocks_bound_skipped` increments
only on `WEAVE_VSCAN_SKIP_BOUND` (`src/vector/vecscan.c:290-292`), which fires only when the
vector shuttle's own floor is above a block's bound — and that floor is set by
`weave_vec_shuttle_set_threshold()` (`src/vector/vecshuttle.c:1078`), whose **sole caller is
the `weave_vec_scan()` SRF driver** (`vecshuttle.c:1336`). A fused scan never calls it, the
field stays at its `-INFINITY` init (`vecshuttle.c:887`), and `bound <= threshold` is false
for every finite bound. **The zero is structural; it says nothing about bound quality.**

The counter that *can* move is the fused core's own `blkskip` (`weave_fuse_stats()`;
`src/am/fuse.c:618`, `ub <= st->theta`, incremented at `:644`). It compares the **sum** of
every contributing channel's weighted block bound (`ub += b` at `:611`) against theta, so it
proves range skipping happens and **cannot attribute it to the vector channel**. Nothing in
the tree can, today — which is why the table above prints `blkskip` and the tautology is
gone from it.

**The conclusion survives on evidence that was always the better evidence.** The vector
block bound does not prune usefully: `bench/RESULTS_CODE_SCAN.md:43-44` measures
**0.00–0.01 %** of blocks skipped even at *oracle* theta (`:55` confirms it at n = 200k), and
(B2) = `max‖recon‖ · ‖q‖` is **≈ 1.0 by construction** on L2-normalized data with
`metric = 'ip'` — it is the domain's own maximum, so it cannot get under a realized score.
What changes is which line you cite.

**And none of this needed measuring: `src/vector/vecshuttle.c:609-614` and `:44-46` already
said it, in comments, before any of the six documents was written** — "`threshold` is
-INFINITY unless a driver that owns a top-k sets it … so this only ever fires for the SRF
below", and "a fused-loop driver passes -INFINITY as the threshold anyway". Nobody
propagated it. A fact recorded where the code is and nowhere else is a fact the documents
will contradict at their leisure; `doc/CONVENTIONS.md`'s deliberate three-place redundancy
exists for exactly this, and was not applied here.

**WHY THE VECTOR SIDE IS 1.000×, observed rather than argued.** One scifact query under the
per-bolt diagnostic NOTICE (`FUSED_TOPK.md` §8c), five lexical channels and one vector
channel:

| | normalizer **on** | normalizer off |
|---|---|---|
| weights | `w_lex` = 0.0109506 each (0.5 / `N_lex` ≈ 45.7 over five terms), `w_vec` = 0.494712 (0.5 / 1.01069) | 0.5 everywhere |
| partition ceiling | **1.0 exactly** — the sum of the weights | 23.3351 |
| theta | 0.110627 | 2.28239 |
| essential / non-essential split | 4 of 6 | — |
| pivots | **5,183 — every document** | 1,450 (28 %) |
| vector `nscore` | **5,183** | 1,052 |

**The general property, and it is the durable half of the finding (`FUSED_TOPK.md` §8d): A
DENSE CHANNEL WHOSE WEIGHTED CEILING SITS ABOVE THETA FORCES THE PIVOT TO VISIT EVERY
DOCUMENT.** Normalization puts the vector key's weighted ceiling at 0.494712 and the whole
partition's at 1.0, which only a document that maxes both keys *at once* could reach. Real
documents score 0.11–0.35 of it, so theta settles at 0.110627, never climbs past 0.49, the
vector channel is never non-essential, and MaxScore has nothing to exclude. With the
normalizer off the 33× scale mismatch this entry opened on put theta at 2.28239 against a
1.01069 vector ceiling: the vector channel went non-essential, 72 % of documents were never
pivoted, and its `cur` ended at **300024** rather than the end sentinel — abandoned, unread.
**That is the ranking defect and the cheap scan being the same state.** The nDCG win and the
work loss are one mechanism, not two findings.

**THE OBVIOUS FIX IS DEAD ON MEASUREMENT: the vector channel is 1.000× AT EVERY WEIGHT
RATIO.** A seven-point lex:vec sweep (weights summing to 1, normalizer on), nDCG@10 and work
reported together so a quality-for-work trade is visible rather than inferred. scifact:

| lex:vec | nDCG@10 | lexical | total | `blkskip` |
|---|---|---|---|---|
| 0.0625 | 0.6679 | 0.153× | 0.544× | 0 |
| 0.125 | 0.6815 | 0.157× | 0.546× | 0 |
| 0.25 | 0.6976 | 0.164× | 0.550× | 0 |
| 0.5 | 0.7138 | 0.178× | 0.558× | 0 |
| 1 — the shipping default | **0.7212** | 0.203× | 0.571× | 0 |
| 2 | 0.7207 | 0.238× | 0.590× | 4,114 |
| 4 | 0.7112 | 0.269× | 0.607× | 4,114 |

nfcorpus:

| lex:vec | nDCG@10 | lexical | total | `blkskip` |
|---|---|---|---|---|
| 0.0625 | 0.3260 | 0.249× | 0.849× | 0 |
| 0.125 | 0.3373 | 0.262× | 0.852× | 0 |
| 0.25 | 0.3488 | 0.285× | 0.856× | 0 |
| 0.5 | **0.3535** | 0.325× | 0.864× | 0 |
| 1 — the shipping default | 0.3455 | 0.380× | 0.875× | 88 |
| 2 | 0.3401 | 0.434× | 0.886× | 601 |
| 4 | 0.3314 | 0.477× | 0.893× | 7,452 |

fiqa:

| lex:vec | nDCG@10 | lexical | total | `blkskip` |
|---|---|---|---|---|
| 0.0625 | 0.3766 | 0.026× | 0.499× | 0 |
| 0.125 | 0.3817 | 0.028× | 0.500× | 0 |
| 0.25 | 0.3968 | 0.031× | 0.502× | 0 |
| 0.5 | **0.4056** | 0.038× | 0.505× | 0 |
| 1 — the shipping default | 0.3878 | 0.052× | 0.513× | 6,732 |
| 2 | 0.3474 | 0.073× | 0.523× | 332,823 |
| 4 | 0.3085 | 0.095× | 0.533× | 4,096,432 |

**The vector column is 1.000× at all 21 points** (0.997–0.999× at three of them) and is
omitted from the tables for that reason. Weighting the vector key *down* is worse than
useless: at r = 4 it carries 0.2 of the weight instead of 0.5, fiqa's range skipping goes
6,732 → **4,096,432**, the vector channel's lane count does not move, and nDCG@10 falls on
**every** corpus (0.7112 / 0.3314 / 0.3085). More range skips and identical vector work is
the signature of skips the vector channel is not paying for.

**The mechanism, and this project had already measured it for an unrelated reason**
(`bench/RESULTS_CODE_SCAN.md:330,417`): in `WEAVE_PACK_LANE`, coordinate *j* of lane *s* is
one nibble at byte `j*16 + s/2` (`include/weave/vecpage.h:18`), so **reading one lane touches
every byte of the block** — scoring 1 lane costs the same memory traffic as scoring 32. A
probe anywhere in a block scores the whole block; a non-essential channel is still probed at
every candidate; candidates are scattered across docid space. The V15 verdict moved three
times on this same fact, expressed as a harness bug.

**CONSEQUENCE: `FUSED_TOPK.md` §8's `score()` row (≤ 0.20×) is NOT REACHABLE for the vector
channel by tuning the objective or tightening the bound.** Three structural options, each
with its cost, ~~**none chosen here — this is a maintainer decision and no task id is invented
for it**~~ — **option 1 was TAKEN 2026-09-22 as a MEASUREMENT decision and 2 and 3 remain
open; the decision does not claim the row, because in blocks the vector ratio is still
1.000×. Dated block at the end of this entry** (the durable form, with the code anchors, is
`FUSED_TOPK.md` §8d):

  1. **Restate the row in the unit the layout has** — blocks or bytes of code read, not
     lanes. No code. Honest only if the restated gate is written down *before* it is
     measured against.
  2. **A second, vector-major copy of the codes**, so one lane can be scored without its 31
     neighbours. Forfeits the storage gate, and `include/weave/vecpage.h:24-26` refuses
     `WEAVE_PACK_VECMAJOR` on the coordinate-split page layout, so it is a new on-disk shape.
  3. **Cluster-order the weft** so candidates are contiguous. This **contradicts the
     strictly-ascending-docid requirement the fused vector channel depends on** —
     `include/weave/vecdocmap.h:35` derives (C2) from it, `:105` states it as the adapter's
     invariant, `:122` is the (C1) lower-bound search that needs it, `:167` is the `init()`
     refusal that enforces it. **V13 and F8 are therefore not independent**, a conflict
     nothing in the tree had recorded before today.

**THE SWEEP ALSO OVERTURNS AN OPEN ITEM OF THIS ENTRY — the last bullet of the open-items
list above — and the axes are the reason it looked open.** The item said fiqa's optimum is at or below the lowest ratio swept
and still falling. In the product fiqa **peaks at r = 0.5** (0.4056) and falls at 0.25
(0.3968), 0.125 (0.3817) and 0.0625 (0.3766). The two measurements are not in conflict: the
study swept its ratio *after* realized-max normalization, the product divides by the
**ceiling**, and the ceiling proxy is 0.73 in study units — so **product r = 0.5 is study
r ≈ 0.37**, and the study's 0.4026 at study-r 0.25 lands between the product's 0.3968 and
0.4056. Marked CLOSED BY MEASUREMENT in place, with the original sentence left as written,
because the thing that was wrong was the axis and not the number.

**RECORDED AND DELIBERATELY NOT ACTED ON: every corpus has an INTERIOR optimum, and r = 0.5
beats the shipping equal-weight default on 2 of 3** — nfcorpus 0.3535 vs 0.3455, fiqa 0.4056
vs 0.3878 — while **losing scifact by 1.0 %** (0.7138 vs 0.7212). **The default is not being
changed.** Hard rule 11: this is one harness at one scale on three corpora, the scifact loss
is real, and "tune the default to the mean of three BEIR sets" is fitting, which is honest
only if reported as fitted. It is a maintainer decision, and the numbers are here so that it
can be made from measurement rather than from taste.

**MEASURED ON EC2 2026-09-22 (night): THE NORMALIZER'S COST IS +19 % / +3 % / +104 % OF p50,
THE §8 p99 ROW GOES FROM PASS TO FAIL, AND ON fiqa THE FUSED ARM IS NOW SLOWER THAN THE RRF
CONTROL IT EXISTS TO REPLACE. This entry's "FIXED" verdict stands on nDCG and is now paid for
in latency.** Run `pgweave-20260922-224507`: `c7i.8xlarge` (32 vCPU), us-east-2, PostgreSQL 17,
extension **0.19.0**, commit **b0bd1b7**, real `all-MiniLM-L6-v2` embeddings computed on the
instance, the same three BEIR corpora, an RRF `k'=100`/`k=60` control over the **same** index,
`LATN=50` × `REPS=7` with arms alternated per query and an A/A leg. Instance terminated and
verified; no orphaned volumes, keys or security groups. This is the re-run the open item above
was waiting for, and it is a **loss**.

| row | gate | scifact | nfcorpus | fiqa | the same row on the raw sum | |
|---|---|---|---|---|---|---|
| p99 fused ÷ RRF | ≤ 0.70× | 0.710× | 0.612× | **1.000×** | 0.609× / 0.560× / 0.633× | **FAIL on two of three — was PASS** |
| p50 fused ÷ RRF | ≤ 0.50× | 0.710× | 0.827× | **1.172×** | 0.582× / 0.795× / 0.578× | **FAIL**, and fiqa is **slower than the control** |

Absolute p50 / p99 in ms — fused (normalizer on) | RRF | `fused_aa` | `fused_raw`:
scifact 2.557 / 3.296 | 3.601 / 4.645 | 2.571 / 3.277 | 2.144 / 2.939;
nfcorpus 1.575 / 1.843 | 1.905 / 3.012 | 1.578 / 1.875 | 1.533 / 1.691;
fiqa 22.163 / 27.918 | 18.915 / 27.905 | 22.198 / 27.900 | 10.889 / 16.845.

**The p99 movement is caused by this change, not by staleness.** The two arms are the same
statement one GUC apart on one index in one run. **The differences are real** (hard rule 10):
|fused − `fused_aa`| at p50 is **0.014 / 0.003 / 0.035 ms**, and the between-arm deltas are
**30× to 320×** that within-arm spread.

**THE CAUSE IS THE PIVOT WALK, NOT THE PRE-SCAN PASS — and the open item above guessed the
pass.** fiqa work counters, normalizer on vs off: **pivots 37,306,460 vs 7,081,750 (5.3×)**,
lexical contributions 2,065,310 vs 5,884,038 (down 2.8×), **vector lanes 37,324,800 vs
35,670,912 (up 4.6 %)**, `blkskip` 6,732 vs 3,444,538, `fuse_scores_total` 39,365,038 vs
7,011,737 (5.6×). The kernel barely moved; the loop did. The normalized scan visits every
document because the dense channel's weighted ceiling sits above θ — the property this entry
already records and `FUSED_TOPK.md` §8d states generally — and one pivot costs one `seek()`
plus one `block_max()` **per contributing channel** whether or not it ends in a `score()`.
**The generalizable mistake: the cost was attributed to the component the change ADDED rather
than to the component whose INPUT the change altered.** Same shape as this entry's earlier
counter correction, one level up: a plausible mechanism, never asked to predict a number.

**A GATE-DESIGN FINDING, and it is the part that outlives this run: §8's work row and §8's
latency row moved in OPPOSITE directions.** The work row counts `score()` calls by channel and
says normalization made things **better** (0.648 → 0.571, 0.903 → 0.875, 0.541 → 0.513 of the
control). The clock says fiqa got **2.0× slower**. A work-count gate that excludes the pivot
walk cannot predict the latency row it stands in for. **Recommendation, recorded in
`FUSED_TOPK.md` §8 as well: if the gate keeps a work row it should count PIVOTS alongside
`score()` calls.**

**What reproduced exactly, which is itself a cross-harness positive control.** nDCG@10
fused / raw / RRF = 0.7212 / 0.6720 / 0.6846, 0.3455 / 0.3161 / 0.3422, 0.3878 / 0.2393 /
0.3482; recall@100 = 0.9683 / 0.8892 / 0.9517, 0.3206 / 0.2908 / 0.3251, 0.7079 / 0.5141 /
0.6932 — the local `bench/normprod.sh` figures to four decimals, on a different host through a
different harness, nfcorpus's recall@100 loss included. Correctness gate (normalizer **off**,
per **G46**): scifact 100 of 100 compared / 0 mismatched, nfcorpus 99 of 100 compared / 1
skipped for a tied oracle / 0 mismatched, fiqa 100 of 100 compared / 0 mismatched; the `fuse()`
fallback differed on 100 / 85 / 100 queries, which is `FUSED_TOPK.md` §7a's documented
divergence and not a defect. Index build 6.6 MB / 0.8 s, 4.9 MB / 0.3 s, 45.4 MB / 6.2 s.

**GATE STATE: 2 of 5.** recall **PASS** (raw objective — G46), nDCG@10 **MET**, p50 / p99 /
`score()` **FAIL**. Before the normalizer it was also 2 of 5 (recall, p99). **THIS ENTRY'S FIX
TRADED p99 FOR nDCG**, and that sentence is the honest summary of G44 as a whole.

**THE DECISION IT FORCES, ~~PRESENTED AND NOT TAKEN — AWAITING THE MAINTAINER~~ — DECIDED
2026-09-22: `pg_weave.fuse_normalize` STAYS ON BY DEFAULT. The presentation below stays as
written because it is the table the decision was made from; the reasoning is in the dated
block at the end of this entry. Should
`pg_weave.fuse_normalize` stay ON by default?** **On:** the fused ranking beats RRF on all
three corpora and is slower than RRF on the largest one. **Off:** it is fast (p99 0.609× /
0.560× / 0.633×, the row passes) and ranks worse than RRF on all three — the state that made
`ARCHITECTURE.md` §9 claim 2 unsupported in the first place. **Neither:** make the vector
channel's candidate set smaller, which `FUSED_TOPK.md` §8d shows needs a restated unit, a
second vector-major copy of the codes, or a cluster-ordered weft — and which is now the
**single blocker for three of the five rows** rather than one. Two facts that belong with the
choice: the GUC is **`PGC_USERSET`**, so a user can already choose per query; and this is the
**first knob in the project whose two settings each fail a different gate row**, which is why
no default is being changed here. Full numbers: `bench/RESULTS_FUSE.md`, fourth measurement.

**BOTH MAINTAINER DECISIONS TAKEN 2026-09-22, AND THE WORK ROW RE-MEASURED IN THE NEW UNITS.
Neither decision moves the gate: it is still 2 of 5.** Local, deterministic, no EC2 —
`bench/normprod.sh` with `PHASES=work`. The harness computes these ratios itself now, in both
`bench/normprod.sh` and `bench/fuse.sh`, and **the two agree to the digit on nfcorpus**, which
is the cross-harness control on the new arithmetic.

| dataset | arm | lexical BM25 contribs | vector code blocks | pivots/query | abandon | `blkskip` |
|---|---|---|---|---|---|---|
| scifact | normalized | 367,940 = **0.203×** | 48,600 = **1.000×** | 5,183 | 1,373,329 | 0 |
| scifact | raw | 639,196 = 0.353× | 48,156 = 0.991× | 1,814 | 265,092 | 97,028 |
| scifact | rrf control | 1,810,229 = 1.000× | 48,600 = 1.000× | — | — | — |
| nfcorpus | normalized | 112,237 = **0.380×** | 36,821 = **1.000×** | 3,627 | 985,711 | 88 |
| nfcorpus | raw | 171,450 = 0.581× | 36,235 = 0.984× | 2,556 | 637,194 | 4,339 |
| nfcorpus | rrf control | 295,197 = 1.000× | 36,822 = 1.000× | — | — | — |
| fiqa | normalized | 2,065,310 = **0.052×** | 1,166,400 = **1.000×** | 57,572 | 36,709,890 | 6,732 |
| fiqa | raw | 5,884,038 = 0.149× | 1,114,716 = 0.956× | 10,929 | 3,048,945 | 3,444,538 |
| fiqa | rrf control | 39,532,352 = 1.000× | 1,166,400 = 1.000× | — | — | — |

**DECISION 1 — `pg_weave.fuse_normalize` STAYS ON BY DEFAULT.** A user chooses a **RANKING**,
not a scan strategy. Off, the fused objective loses to a plain RRF control on all three corpora
measured (0.982× / 0.924× / 0.687×), and a fused scan that ranks worse than the two-query
control it replaces has no reason to exist; on, the ranking beats RRF everywhere measured
(1.053× / 1.010× / 1.114×). **The price is recorded and not hidden:** p99 from **PASS**
(0.609× / 0.560× / 0.633×) to **FAIL** (0.710× / 0.612× / 1.000×), p50 from 0.582× / 0.795× /
0.578× to 0.710× / 0.827× / 1.172×, and on fiqa the fused arm is slower than the control.
Because the GUC is **`PGC_USERSET`** a deployment that wants the old trade can have it per
query or per session, **which is why this is a default and not a fork in the design**. And the
latency regression is **not accepted as permanent**: it is charged to the one open blocker, the
**size of the vector channel's candidate set**.

**DECISION 2 — §8's work row is RESTATED per channel, each channel in its own unit, plus a
diagnostic.** Lexical work: **BM25 contributions**, fused vs the WAND control, gate ≤ 0.20×.
Vector work: **CODE BLOCKS READ**, fused vs the control's own code scan, gate ≤ 0.20×. Pivots
per query: **reported, not gated**, because the RRF control has no pivot loop and there is
nothing to take a ratio against. Two measurements forced it: (a) in `WEAVE_PACK_LANE` a
one-lane read touches every byte of its block, so **a lane is not a unit of cost and a block
is** — measured during the V15/V16 work (`bench/RESULTS_CODE_SCAN.md:330,417`); (b) the
lane-based row said normalization made the fused arm **cheaper** on all three datasets
(0.648→0.571, 0.903→0.875, 0.541→0.513) while the clock said fiqa's p50 **doubled**, and a work
row that cannot predict the latency row is measuring the wrong thing. The pivot count is what
tracks the clock (fiqa pivots 5.3×, p50 2.0×).

**Restating the row does NOT rescue it.** In blocks the vector ratio is **1.000×, exactly as it
was in lanes**, so the gate is still failed; the restatement buys **honesty about the unit, not
a pass**. What it does buy is separable halves: the **lexical** half is **MET on fiqa**
(0.052×) and missed on scifact (0.203×, just over) and nfcorpus (0.380×), while the **vector**
half is missed **everywhere at 1.000×**. And the normalized arm's pivot count is **exactly the
corpus size per query** — 5,183 / 3,627 / 57,572 against corpora of 5,183 / 3,633 / 57,600
documents — i.e. **the fused scan is a full pass over the docid space**, which is the plainest
possible statement of the dense-channel problem this entry has been circling.

**A FACT THAT DESERVES ITS OWN PARAGRAPH: incremental abandonment is firing CONSTANTLY, not
rarely.** On fiqa, **36,709,890 abandonments over 37,306,460 pivots — 0.98 per pivot.** That is
what produced the lexical improvement (2.8× fewer BM25 contributions). **It cannot help the
vector channel, and the reason is arithmetic rather than implementation:** the fused core sorts
its scored channels by **descending weighted ceiling** (`src/am/fuse.c:163-165`) and abandons on
`s + csuffix[j+1] <= theta` (`:716`), so after normalization the vector channel — weight
**0.4947** against **0.0110** for each lexical channel — is summed **FIRST** and its score is
computed **before any abandonment test can run**.

**Reversing the order to put the expensive channel last was considered and is DEAD ON
ARITHMETIC, not on effort.** The test that would skip it is
`s_lex + w_vec * block_max_vec <= theta`, and `w_vec * block_max` is **~0.49** while **theta is
~0.11**, so it can **never** fire. It would remain dead at **k = 10** instead of the ladder's
k = 128, because theta would still be well under 0.49. Recorded so nobody spends a week on it.

**And it follows from the same arithmetic that a per-block vector bound carries no
information** on L2-normalized data — (B1), (B2) and (B3) are all ≈ 1.0 unless a block is
coherent in direction — so **every route to a smaller vector candidate set runs through the
three structural options** above: restated unit, a vector-major second copy, or a
cluster-ordered weft that conflicts with F8's ascending-docid requirement. **Decision 2 takes
the first of those as a MEASUREMENT decision only; it explicitly does not claim the row.**

**So G44's standing summary, after both decisions:** the gate is **2 of 5**, the single blocker
is the **size of the vector channel's candidate set**, and nothing in this entry is awaiting the
maintainer any more. `bench/RESULTS_FUSE.md` (fifth measurement),
`doc/specs/FUSED_TOPK.md` §8 and §8d.

### G45 — `prepdata.py`'s MS MARCO source returns HTTP 404; the dataset §8 names by name cannot be fetched — **FIXED 2026-09-23** (the fixture builds again; the nDCG run on it is still owed)

**~~OPEN 2026-09-22.~~** `bench/prepdata.py` fetched MS MARCO passage from
`https://msmarco.z22.web.core.windows.net/msmarcoranking/`, and
`queries.dev.small.tsv` returned **404**. The run died there after completing all
three BEIR datasets, so the loss was the dataset, not the run (artefacts are pulled back
per dataset — hard rule 14 — so nothing measured was lost).

**The source was not gone; one file was.** Probing the whole set is what closed this, and
it took one minute: `qrels.dev.small.tsv` (200, 143,300 bytes), `collection.tar.gz` (200,
1,035,009,698 bytes), `collectionandqueries.tar.gz` (200) and `queries.tar.gz` (200,
18,882,551 bytes) all still answer at the same base. Only the bare `queries.dev.small.tsv`
is gone. The conclusion recorded on 2026-09-22 — "the hosting moved" — was wrong, and the
diagnosis that mattered was *enumerating the siblings of the thing that failed* rather
than reasoning about why it failed.

**The fix reconstructs dev.small rather than substituting a different corpus.**
`queries.tar.gz` carries `queries.dev.tsv`, the FULL 101,093-query dev set, and
"dev.small" is *by definition* the subset of dev whose qids appear in
`qrels.dev.small.tsv`. So filtering reproduces the missing file exactly instead of
approximating it, and the BEIR-msmarco route the original entry proposed — with its trap
of a plain `--limit` truncation silently discarding ground truth — is not needed at all.
`build_msmarco_sub()` keeps its qrels-preserving reservoir sample untouched.

**Verified end to end, not by HTTP status:** 6,980 unique qids in the qrels, 6,980 rows
matched in `queries.dev.tsv` — which is the published size of dev.small — and a full
`--dataset msmarco-sub --limit 20000 --embed hash` run exits 0 with
`nqrels 7437, nqrels_dropped 0, nqueries 6980, unjudged_queries_dropped 0`. Nothing
judged was dropped, which is the property the subsample exists to have.

**Positive control, and it took three attempts because the harness defended itself
twice.** The new fatal assertion is "every qrels qid has text" — a judged query with no
text would hand an arm a query it cannot answer and average zeros into the nDCG row, the
same class of failure as dropping a judged passage. To make it fire:

1. Appending a bogus qid row to the cached qrels and pointing `--out` at a *different*
   directory tested nothing: a different `--out` means a different `_cache`, so the file
   was downloaded fresh. Exit 0.
2. Appending it to the cache the run actually used still tested nothing: `fetch()`
   compares the cached size against the remote `Content-Length` and **re-downloaded the
   pristine file**, repairing the tamper. Exit 0. That size check was written to catch
   truncated downloads and it catches sabotage by the same mechanism.
3. Substituting an existing qid with a **byte-length-preserving** one that does not occur
   in `queries.dev.tsv` (`300674` → `999999`, file size identical at 143,300) fires it:
   `exit=1`, `1 of 6980 qrels query ids have no text in queries.dev.tsv (e.g. 999999);
   dev.small cannot be reconstructed from this source`.

The first two attempts are the interesting part, because both LOOKED like a passing
positive-control run and both had examined nothing — the eleventh-member shape arriving
from the direction of a cache.

**What is still owed:** an nDCG measurement *on* this dataset. `bench/fuse.sh` refuses
hash embeddings unless `FUSE_ALLOW_HASH=1` (deliberately: every recorded number is
`all-MiniLM-L6-v2`), and `sentence-transformers` is not installed on the local host. So
§8's dataset list is no longer *blocked*, it is unmeasured — a distinction hard rule 11
cares about. It belongs to the next run that has the embedding model.

**Process note, unchanged and now paid for twice:** a URL in a benchmark harness is a
dependency with no version and no test. This one worked when it was written and rotted
silently; the first evidence was a 404 on a paid instance. A harness that downloads
anything should fetch the smallest file first and fail fast, which is what happened here
by luck of ordering rather than design.

### G46 — `bench/fuse.sh`'s exhaustive oracle cannot express the shipping objective, so the correctness gate runs with `pg_weave.fuse_normalize = off` — **OPEN 2026-09-22**

The gate compares the fused pushdown against `0.5*lex + 0.5*vec` computed from
`weave_search()` and `weave_vec_scan()`. Since 2026-09-22 the shipping scorer divides each
`fuse()` **key** by its own pre-scan ceiling (`doc/specs/FUSED_TOPK.md` §8d), and **that
objective is not expressible in SQL**: the vector key's normalizer is reachable (max over
segments of `weave_vec_scan_stats().maxscore`, the same (B2) fold the scan uses), but the
lexical key's needs **each term's max tf**, and nothing in `sql/` exposes it —
`weave_index_df` and `weave_index_stats` are what there is
(`sql/pg_weave--0.1.0.sql:252-262`), and they give df, ndocs, avgdl and nterms.

So the gate now sets `pg_weave.fuse_normalize = off` and says so in its banner
(`bench/fuse.sh:320-334`). **What it still proves is worth keeping and is narrower than it
reads:** the scan is *exact* — pivot selection, the MaxScore partition, incremental
abandonment, summation — **for a given set of weight constants**. What it does not check is
the constants, which is the half that changed. Those are covered elsewhere and neither cover
is exhaustive: `sql/fuse_degenerate.sql` (6) pins bolt-count independence of the normalizer,
and the nDCG arms in G44 pin its direction against RRF on three corpora.

**The risk, named because it is not the missing coverage itself:** a gate that tests a
**non-default configuration** is one configuration change away from testing nothing. The
day `pg_weave.fuse_normalize` is removed, renamed, or made `PGC_POSTMASTER`, this gate either
errors out or silently starts exercising the default while comparing against the raw
objective — and the second of those reports mismatches that are not defects, which is how a
red gate gets waived. It is green today and it is green about the arm nobody ships.

**The fix, either form of which is an extension version bump.** Expose max tf per query term
(the scan already accumulates it in the `src/am/amscan.c` loop that reads every segment's
dictionary entry for the global idf, so it is a return value, not new I/O), or expose the
per-key normalizer directly as an accessor so the oracle divides by the same constant the
scan does. The second is less surface and more coupling: it makes the oracle agree with the
scan **by construction**, which is exactly what an oracle must not do if the constant itself
can be wrong. Prefer max tf, and let the oracle recompute the normalizer from it.


### G47 — with a vector weft, `weave_vacuum_compact()` has no fixed point: every other VACUUM rewrites the live segment, extends the relation, truncates nothing, and achieves no net change — **OPEN 2026-09-24**

**The function's own header states the contract this violates:** *"Converge a bloated
index to its size floor in ONE call, stably (repeated calls do not oscillate) and NEVER
returning larger than we started."* Repeated calls oscillate forever, and the call that
starts at 2,577 pages returns 4,039 — 1.57× larger.

**Measured, and it reproduces at three scales.** 20k × 96-d locally:
`2577 → 4039 → 2577 → 4039 → 2577 → 4039`. 2,000 × 96-d, the smallest fixture found:
`438 → 280 → 424 → 280 → 424`. 1M × 960-d on EC2: `190091, 185234, 283924, 185234` —
**bit-identical across two independent runs** (`bench/RESULTS_VECMERGE_SCALE.md`).
Period 2, peak/trough 1.53–1.57×.

**Attributed by ablation, not by argument.** A lexical-only index over the *same*
documents with the *same* history (build, two insert+merge cycles, 10 % delete) converges:
`442 → 373 → 211 → 211 → 211 → 211`, 31 free pages. Put a vector weft in and it never
converges. In the failing case the weft is 1,166 of the 1,346 live pages.

**Mechanism, from the allocator counters rather than from reading the source**
(`weave_alloc_stats()`, the instrument L19 added for exactly this discrimination):

| cycle | lowfree_reuse | lowfree_defer | fsm_defer | extend | result |
|---|---|---|---|---|---|
| shrink | **1346** | 0 | 0 | 0 | 2,577 |
| grow | 1230 | **1346** | 116 | **1462** | 4,039 |

Three exact equalities carry the diagnosis: `extend` (1462) is the page swing;
`lowfree_reuse` on a grow cycle (1230) is *every* free page below the live data; and
`lowfree_defer` on a grow cycle (1346) is the **next** cycle's `lowfree_reuse`, to the
page, at every scale measured. So: `weave_vacuum_compact()`'s **vacate phase frees the
pages its pack phase needs**, a page freed by the current transaction is never
recyclable within it (`weave_free_page` stamps `ReadNextTransactionId()`,
`weave_page_recyclable` asks `GlobalVisCheckRemovableXid()`), and the pack therefore
extends by the shortfall. The next VACUUM finds those pages recyclable, packs into them,
and truncates back down. Then the cycle repeats.

**Two structures make it invisible to the existing guards:**

- **`weave_index_is_compacted()` term (1)** counts free pages below the highest live
  block and fires above `max(nblocks/50, 8)` — 1,230 against a threshold of 51. But
  those 1,230 are holes the *previous* pass created, and the next pass cannot fill them
  either: it needs `live` (1,346) destinations and has 1,230. A hole you cannot fill is
  not a reason to run a pass.
- **The "never return larger than we started" backstop** only walks down a *contiguous
  free tail*. What grew the file is **live data** relocated above `startblocks`, so it
  breaks on the first live page and truncates nothing. Confirmed: `freetail = 0` in
  **both** steady states, so `weave_truncate_free_tail()` can never help, and neither
  state is front-packed. The floor is 1,347 pages; the two states sit at 1.91× and 3.00×.

**This is not G18 and L19 did not cause it.** G18 was *unbounded* growth with
`lowfree_reuse = 0` — the free list never consulted. Here reuse is maximal every cycle
and the growth is bounded. Same family, different defect. L19's probe
(`weave_any_free_page_recyclable()`) does not stop it, because pages *are* recyclable —
just not enough of them, and not the ones the pass is about to free.

**Cost.** Not bloat — bounded — but pure waste: ~1,346 page relocations, 1,462
extensions, and GenericXLog WAL for all of it, on every other VACUUM, forever, for no
net change. At 1M × 960-d that is ~94,000 pages (≈770 MB) of churn per cycle and a
1.53× file-size swing a user can see.

**Pinned by** `t/015_alloc_outcomes.pl`'s `vector weft present` arm, as a **TODO** block
(so it records the shape today and turns into a loud "unexpectedly succeeded" when
fixed) plus a non-TODO `no_ratchet` assertion, because the oscillation being *bounded*
is the half that keeps this a waste defect rather than a bloat defect. `t/015` could not
have caught it before: its `no_ratchet()` takes the max of cycles 2..N against cycle 1,
so for a period-2 swing its verdict is decided by the parity of the cycle the loop
starts on. The new `converges()` assertion is the property that was missing — and the
three pre-existing arms pass it, which is why a tighter threshold on them was not the
answer.

**THE OPTIONS WERE MEASURED THE DAY AFTER THEY WERE WRITTEN, AND THE LIST BELOW IS THE
CORRECTED ONE.** `bench/RESULTS_G47_VACATE.md` has the matrix: four arms
({vacate on, vacate off} × {`VACUUM`, `weave_vacuum()`}), six cycles, two reps each,
bit-identical, `weave_check(deep)` clean and the live-lane digest constant throughout.
The original three options are kept below with their verdicts rather than deleted
(hard rule 13), because two of the three were wrong in instructive ways.

**What the measurement changes, first, because it reframes the whole entry: G47 is TWO
defects and only one of them is fixable.**

- **the waste** — the vacate phase contributes **1,346 of the 1,461 extends (92.1 %)**
  and the entire visible file-size swing, and buys the share-lock caller *nothing*:
  both arms sit at the **same trough, 2,578 pages**. Fixable now (option 4 below).
- **the non-convergence** — plain `VACUUM` sits at 1.91× the floor forever. **Not
  fixable without giving up the recycle gate**, because reaching the floor requires
  recycling pages freed by the same transaction, and under a share lock that hands a
  concurrent scan a page it is still reading — the field-reported crash
  `weave_page_recyclable()` exists to prevent. The floor stays reachable only under
  AccessExclusiveLock (`weave_vacuum()`, `REINDEX`), which is how it works today.

1. ~~**Reuse freed pages immediately when holding `AccessExclusiveLock`.**~~
   **ALREADY IMPLEMENTED — there was nothing to build.** `weave_page_recyclable()`
   already bypasses the gate under AEL (`src/am/am.c`, "safe to bypass ONLY when no
   concurrent scan can exist"), and the matrix shows the consequence: `weave_vacuum()`
   converges to the **exact floor (1,347 pages) in ONE call** and then does literally
   nothing for five more cycles — 0 extends, 0 reuses. *This option was stale the day
   it was written, in the entry whose own AGENTS.md rule is "an option list is a cache
   and it goes stale". Grepping the source for the option's own status row would have
   caught it; the option list was written from the comment at `amvacuum.c:511` instead.*
2. **Under a share lock, skip the pass when it must extend.** Still available, still the
   "skip forever" trap (`amvacuum.c:500-509`) — but **its prize shrank by 92 %**. It was
   worth 1,461 extends per cycle when it was written; with option 4 in place it is worth
   115.
3. ~~**Teach the pack phase to place the new weft genuinely front-packed.**~~
   **REFUTED, on two independent grounds, and it never needed to be built to be
   settled.** (a) *Arithmetic:* from the trough the pass needs **1,346 destinations and
   has 1,231** (`DEMAND` vs `BUDGET`, both read off the free space map with the
   allocator's own `BLCKSZ/2` criterion), so any single write-before-free pass **must**
   extend ≥ 115 pages regardless of placement policy. (b) *Measured:* the pack phase
   with the vacate ablated lands on exactly `2,578 + 115 = 2,693` — it **is** that
   bound, so it is already as front-packed as write-before-free permits. The
   destination set is fixed by what was already free, not by how the pass chooses among
   it. There is no placement lever here.
4. **NEW, and it is the one the measurement supports: make the vacate phase conditional
   on the lock actually held.** Run it under AccessExclusiveLock, where it reaches the
   floor in one call; skip it under a share lock, where it provably cannot help. The
   predicate is already in the file — the L19 probe skip at `amvacuum.c:510` uses
   `CheckRelationLockedByMe(index, AccessExclusiveLock, true)` for the same reason.
   **It is not option 2** (which skips the whole pass; this skips only phase 1 and still
   runs the pack) and **it changes no policy**: plain `VACUUM` reclaims to exactly the
   2,578 pages it reaches today.

   | | today | option 4 |
   |---|---|---|
   | `weave_vacuum()` (AEL) | 1,347 = floor, one call | unchanged |
   | `VACUUM` trough / peak | 2,578 / 4,039 | 2,578 / **2,693** |
   | extends per grow cycle | 1,461 | **115** |
   | swing a user sees | 1.568× | **1.045×** |

**And the ablation refutes the lazy reading of it too:** *deleting* the vacate phase is
wrong. With it ablated, the AEL path stalls at 2,693 — 2.00× the floor — and keeps
paying 115 extends every cycle forever. The vacate phase is load-bearing exactly where
its freed pages are recyclable in-transaction, and dead weight exactly where they are
not. That is why the fix is a condition and not a deletion.

**One observation for the guard rather than the fix.** `novacate` + `weave_vacuum()`
holds a **stable 2,693 pages while extending 115 pages every single cycle, forever**. A
stable size with permanent work is invisible to `weave_index_is_compacted()`, whose term
(1) sees 1,346 free pages below live against a threshold of 8 and concludes there is
work to do. A hole the pass cannot fill is not a reason to run the pass — the same
blindness recorded above, wearing the other arm's clothes.

**A finding about fixtures that came out of pinning the start states**, recorded because
it invalidates the obvious way to run this comparison: the same logical history (build,
4 × insert+merge, 10 % delete) landed on **2,904 pages three times and 3,755 the
fourth**, because a merge reuses a freed page only when the horizon has moved past its
freeing xid. **An index's size after a history is a function of the transaction horizon
as well as of the history.** Adding the post-delete `VACUUM` to the fixture makes it
deterministic (2,578 on three consecutive rebuilds) — and that vacuum belongs there
anyway, since it is the L19 cycle that is not a steady-state datum.

**Instrument, and it stays in the tree:** `pg_weave.vacuum_vacate` (bool, default `on`
= shipped behaviour, `PGC_USERSET`) ablates phase 1. It is a diagnostic, not a tuning
knob, and it is deliberately **outside** the `WEAVE_TEST_HOOKS` block — an absent GUC is
indistinguishable from one that is off. Its positive control is that it moved `extend`
from 1,461 to 115; until a diagnostic has fired once, its silence is not evidence. It
stays because an A/B that cannot reproduce its own baseline is not an A/B (hard rule 10).

**Hard rule 12's run is DONE — 2026-09-24, run `pgweave-20260924-200357`, 1M × 960-d**
(`bench/RESULTS_VECMERGE_SCALE.md` run 4). Option 4 holds at 50× the scale, and two
results are stronger than a reproduction:

- **peak/trough 1.53× → 1.03×**; the grow cycle went from **+98,690 pages (~771 MB)** to
  **+4,049 (~32 MB)**; the previously-expensive vacuum cycles went from 1,097–1,219 s to
  **566 s (1.94×)** and their alternation — the symptom that first identified the
  relocation pass — is gone (0.6 % spread). `lowfree_defer = 0` on every cycle.
- **The DEMAND/BUDGET law predicted each grow cycle to the page**: cycle 2 reported a
  shortfall of 4,049 and cycle 3 extended exactly 4,049 (same for 4 → 5). That is the law
  this entry used to refute option 3, now holding at a second scale (hard rule 11).
- **Cycles 1 and 2 are bit-identical to the two pre-fix runs** (190,091 and 185,234), so
  the delta is attributable: the cycles where the fix does nothing are unchanged.
- **The floor leg quantifies what the share-lock caller gives up**: `weave_vacuum()` took
  the settled index from 185,234 to **94,642 pages — exactly the floor** (94,641 live +
  metapage) — in ONE call, and a second call moved it not at all with **zero
  allocations**. The two callers differ by **1.96×, about 708 MB**.

`weave_check(deep)` clean with the weft invariant present at every stage including the new
`ael` one, recall@10 0.8500 → 0.8500 (differential), live-lane digest constant, 0 deferred
failures, and **the mutation control fired on that host** — so the clean results are
informative rather than silent.

**The non-convergence half remains OPEN and is probably unfixable.** 1.03× is smaller than
1.53×; it is not a fixed point. `t/015`'s G47 TODO arm stays red by design, and it is now
mechanism-based so it cannot go quiet the way the size-based version did.

---

## THE MAINTAINER CALL, 2026-09-25, and it rejects every option on the list

Taken on the measurements below rather than on the option list, because the option list was
wrong twice (option 1 was already implemented; option 3 was refutable by arithmetic).

**First, what option 4 did NOT fix, which is bigger than the swing it did fix.** At 1M
every cycle reuses **94,641 pages — the entire live segment** — and takes **~566 s**:

| cycle | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| `lowfree_reuse` | 94641 | 94641 | 90592 | 94641 | 90592 | 94641 |
| `extend` | 0 | 0 | 4049 | 0 | 4049 | 0 |
| seconds | 708.8 | 564.6 | 566.5 | 566.3 | 569.6 | 590.0 |

So a plain `VACUUM` rewrites a ~740 MB segment and writes GenericXLog for all of it, on
**every** cycle, forever, to move the file between two sizes 2.2 % apart. On a quiet
database autovacuum does this indefinitely. **Doing nothing is therefore not an available
disposition** — not because of the space, which is bounded, but because an index that
rewrites itself forever has no precedent in core and would be a review objection against
claim 4 (`doc/ARCHITECTURE.md` §9), which is the claim the whole contrib-track goal rests
on.

**Option 2 is REFUTED by our own data, and the refutation is the useful part.** Its stated
justification — "when the recyclable free space below the live data is less than the live
size, the rewrite provably cannot truncate and can only grow the file" — is false. Measured
at 20k: from **2,904 pages with budget 1,410 against demand 1,493** (budget *below* demand,
exactly where option 2 skips) the pass reclaimed to **2,578, an 11.2 % shrink**. A pass can
truncate a freed *tail* without ever reaching the floor. Option 2 would have skipped that
reclaim, which is the "skip forever" failure it was already warned about arriving by a
route nobody had noticed.

**The direction that survives: let the pass PREDICT its own result and decline work it has
computed to be useless.** Which blocks a low-bias pack will use is not a mystery — it is
the free space map, which `weave_index_is_compacted()` already walks:

```
LIVE = blocks the FSM shows in use;  FREE = blocks it shows free
|FREE| >= |LIVE|  ->  post-pass size = (the |LIVE|-th lowest FREE block) + 1
|FREE| <  |LIVE|  ->  post-pass size = nblocks + (|LIVE| - |FREE|)
```

Validated by `/scratch/pg_weave/g47pred.sh`, which predicts, then runs the real `VACUUM`,
then compares — **6 of 6 states exact, error +0, on both branches**, and it also predicts
the two 1M states to within one page (the metapage) from data taken before the formula
existed. Under this guard the 1M steady state becomes **185,234 held with zero work** in
place of a 566 s rewrite per cycle.

**BUT THE GUARD FAILED ITS OWN CONTROL AND IS THEREFORE NOT ADOPTED YET.** Run from the
2,904 state — the one that refuted option 2 — the predictor said 2,987 and the pass
delivered **2,578: an error of −409 pages, in the dangerous direction** (it predicted growth
where there was a shrink, so a guard built on it would have skipped exactly the reclaim
option 2 was rejected for). The mechanism is understood: that pass **drops tombstones**, so
the live page count falls 1,493 → 1,346 *during* the pass, and the formula assumes live is
constant. The predictor is exact precisely when the rewrite will not change the live size,
i.e. when there are no tombstones to drop — and `weave_vacuum_compact()`'s own comment
states that invariant ("the rewrite writes a segment with `ndeleted = 0`").

**So the call is:**

1. **Reject options 1, 2 and 3** — implemented already, refuted by measurement, and refuted
   by arithmetic respectively.
2. **Adopt the predictive guard in principle**, as a fourth term in
   `weave_index_is_compacted()` conditioned on `ndeleted = 0`, which is the condition under
   which it is exact. Not "skip the pass" — *the predicate that decides whether the index is
   at its achievable floor learns that a rewrite it has computed cannot shrink is not work
   worth doing.* That is the same shape as L18's fix, where the predicate learned what a
   tombstone is.
3. **Blocked on ONE measurement, and deliberately not built before it.** `ndeleted` is not
   visible from SQL — `weave_index_stats().ndocs` already has it subtracted — so the
   precondition the guard depends on **cannot currently be asserted in a test**, and a guard
   whose precondition is untestable is the twelfth-member trap with a page count instead of a
   GUC. The instrument is one column on an existing stats function (another `DROP` + `CREATE`
   and a bump to 0.22.0). Then re-run both controls: the 2,904 state must **run**, the
   2,578/2,693 states must **skip**.
4. **Until then the oscillation is documented behaviour, not a bug being ignored**, and
   `weave_vacuum()` is the documented route to the floor: 185,234 → **94,642 = exactly the
   floor** in one call, then zero allocations, worth **1.96× / ~708 MB** at 1M.

**UNBLOCKED AND IMPLEMENTED, 2026-09-25 — term (4) is in, and G47's waste half is closed.**

The blocker was that the guard's precondition (no tombstones about to be dropped) had to be
assertable. Two findings, the first a loss:

- **`ndeleted` exposed to SQL does NOT serve that purpose**, which is the opposite of what
  this entry predicted an hour earlier. `weave_index_stats()` gained the column (extension
  0.22.0) and it reads **0 at every point SQL can sample**: `ambulkdelete` sets it and the
  compaction pass in the *same* `VACUUM` clears it, so between vacuums it is always zero.
  The column is kept for the reason it is independently worth having — `pg_weave.vacuum_
  tombstone_frac` is documented as tunable and nothing could see either side of its
  comparison — but it did not unblock anything.
- **The quantity IS visible at the point the decision is taken**, which a probe in the
  predicate showed rather than an argument: `elog` at the decision point read
  `tombfrac=0.100000` in the 2,904 state and `tombfrac=0.000000` in every settled state.
  The guard runs inside `amvacuumcleanup`, after `ambulkdelete`, which is exactly where the
  discriminator exists. So the condition is `weave_tombstone_frac(index) == 0` — **not** the
  GUC's 0.2 threshold, which a 10 % delete does not cross.

**Measured, both controls, at 20k:**

| control | requirement | result |
|---|---|---|
| 2,904 start (the state that refuted option 2) | the pass must **RUN** | 2,904 → 2,578 (alloc 1,348), then 2,578 with **alloc 0** |
| settled 2,578 / 2,693 | the pass must **SKIP** | **2578 ×6, extends 0 ×6 — a fixed point with zero work** |
| `weave_vacuum()` (AEL) | unchanged, still the floor | 1,347, one call, then zero |
| vacate ablated under AEL | still discriminates | 2,693 with 115 extends/cycle |

**And the matrix caught a regression the single arm could not.** Term (4)'s prediction models
a **pack-only** pass — what a share-lock caller does now that the vacate is AEL-only. Under
`AccessExclusiveLock` the pass is vacate+pack and reaches the *floor*, so the first version of
the term told `weave_vacuum()` its index was already compacted and **the floor became
unreachable: 2,578 instead of 1,347, a 1.91× regression in the one path that had been
working.** Six cycles of the share-lock arm looked perfect while that was true. The term is
therefore restricted to share-lock callers, with the same `CheckRelationLockedByMe()`
predicate option 4 uses. *Run the whole matrix on a change to this predicate, not the arm the
change is about.*

`t/015`'s G47 arm is no longer a TODO: it asserts that a settled weft index does **zero
allocation work** on the next plain `VACUUM`, and the series is now
297 → 280 → 280 → 280 → 280 → 280.

**Still owed:** the 1M `vecmerge` run (hard rule 12 — this is the vacuum path), and the
`PRODUCTION_READINESS.md` limitation stays as written, because the *floor* is still
AEL-only. What changed is that a plain `VACUUM` now settles there for free instead of
rewriting the segment forever.

**Why (4) is defensible rather than a climbdown, and this is the north-star argument.**
PostgreSQL already ships this exact two-tier model: plain `VACUUM` reclaims what it can in
place and never returns a heap to its floor; `VACUUM FULL` and `REINDEX` reach the floor
under `AccessExclusiveLock`. After option 4 our behaviour has the same shape as core's, and
a committer recognises it. What has no precedent in core — and what option 4 removed — is an
index whose plain `VACUUM` grows the file 1.5× and hands the space back on the next run. The
remaining gap between 1.96× and the floor is a *documentation* obligation
(`doc/PRODUCTION_READINESS.md` owes "run `weave_vacuum()` to reach the floor"), not a
correctness one.


### G48 — a lexical seek skips the DECODE but reads every PAGE it passes over, so the channel that dominates a gated query has no way to skip I/O — **OPEN 2026-09-24, ceiling UNMEASURED**

**Where this came from.** `bench/RESULTS_GATE_SWEEP.md` sized two vector-side levers and
withdrew both, leaving the arithmetic that the vector channel cannot win more than ~15 %
of a gated query: at the tight gate the lexical channel is the larger half, 390 of 813
buffers on fiqa. So the next lever has to be lexical, and the first question is what the
lexical channel's floor actually is.

**What is established, by reading the code rather than by measuring it.**
`wand_skip_blocks()` (`src/am/amscan.c`) exists precisely to make a seek cheap, and its
header says so: it advances "past whole 128-blocks whose docids are all < target, reading
only block HEADERS (no FOR decode) … what lets a seek over a high-df term skip hundreds
of thousands of postings without decoding." That is true and it is the right design for
CPU. But the loop that implements it does `ReadBuffer(c->index, c->curblk)` once per page
and follows `nextblk` to the next, so **a forward seek across N pages of a posting chain
reads all N buffers.** It skips the decode, not the I/O.

Consequences worth stating separately, because they have different fixes:

- **The fused threshold cannot turn into avoided reads on the lexical side.** The shuttle
  has a real `block_max()` (`src/query/lexshuttle.c`) and the core does seek past blocks
  the bound rules out — so the pruning *works*, and every page it prunes over is read
  anyway. The pruning is free in CPU and costs full price in buffers.
- **This is the structure BlockMax-WAND normally has and we do not**: per-block
  `first_docid` and `block_max` held OUTSIDE the chain, so a block can be rejected
  without touching its page. We store both, in the block header, on the page you must
  read to see them.
- The chain's start is not the problem — the dictionary gives the first page directly. It
  is the traversal that is linear in pages.

**WHAT IS NOT MEASURED, AND NOTHING SHOULD BE BUILT UNTIL IT IS.** The number that
decides whether this is a lever or a footnote is the split of those 390 buffers into
*pages read only to skip over* versus *pages read to decode a block that scored*. If the
first is 10 % of the total the whole idea is a footnote; if it is 80 % it is the largest
remaining lever in the project. **I do not know which, and the shape of the code is not
evidence for either** — this is the same error as the warp map and the normalizer pre-scan,
both of which were asserted as levers from a plausible mechanism and came back at 6.7 %
and within-noise. A recommendation is a claim (AGENTS.md).

**The instrument that would settle it**, which does not exist today: the lexical
counterpart of `weave_vecwork_blocks` / `weave_vecwork_blk_bound`. Two counters, one site
each — pages read inside `wand_skip_blocks()` (skip-only traffic) and pages read inside
`wand_load_block()` (decode traffic) — surfaced through `weave_work_stats()`. Note that
adding columns there means a version bump and a DROP + CREATE in the upgrade script: a
function whose whole result is `OUT` parameters has those parameters as its return type,
so a column cannot be added in place (the 0.10→0.11 and 0.12→0.13 scripts both say so).
`weave_lex_contribs` cannot substitute: it deliberately counts the single-channel WAND
path only, so it is zero for exactly the fused queries this is about.

Also unmeasured and cheaper to get wrong: whether the same pages are being re-read across
the several terms of a query (posting lists share pages), in which case shared_buffers
absorbs most of the cost and the EXPLAIN BUFFERS figure already reflects that. The
measurement must therefore be buffer *reads* as the executor counts them, not page visits
as a counter counts them, or it will overstate the lever.

---

## MEASURED 2026-09-25 — the instrument exists, and the answer is 19–50 % of page VISITS

`weave_work_stats()` gained `lex_pages_skip` and `lex_pages_load` (extension 0.21.0), the
lexical counterpart of `vec_blocks` / `vec_blocks_bound_skipped`. `bench/gatesweep.sh`
records both per point. Three BEIR corpora, 40 queries per point, **A/A repeat on fiqa
bit-identical**:

| corpus (docs) | sel 1.0 | 0.1 | 0.01 | 0.001 |
|---|---|---|---|---|
| fiqa (57,600) | **49.5 %** | 49.5 % | 49.4 % | 42.3 % |
| scifact | 45.5 % | 45.5 % | 44.5 % | 32.8 % |
| nfcorpus | 34.9 % | 34.7 % | 34.7 % | 19.4 % |

(share = `lex_pages_skip / (lex_pages_skip + lex_pages_load)`; fiqa at 0.1 % is
17,159 skip against 17,478 load over 40 queries.)

**So the lever is real and it is not a footnote** — between a fifth and a half of the
lexical channel's page traffic is spent proving blocks irrelevant. Two structures in the
numbers matter more than the headline:

- **It scales with corpus size** (nfcorpus 35 % → scifact 45 % → fiqa 49.5 %), which is
  what the mechanism predicts: skipping needs terms whose posting lists span enough
  128-blocks to leave whole ones behind. At Wikipedia scale it would be higher, so the
  small-corpus numbers are a floor rather than an estimate.
- **It FALLS as the gate tightens** (49.5 → 42.3 on fiqa, 34.9 → 19.4 on nfcorpus), and
  the absolute traffic falls 2.8×. A tighter predicate does less lexical work *and* a
  smaller fraction of what remains is skip-only, so this lever is worth least exactly at
  the operating point claim 3 is about. It is a bigger win for the unfiltered query — the
  case that currently *loses* 0.7–0.8× (`bench/RESULTS_GATE_SWEEP.md`).

**WHAT IS STILL NOT MEASURED, and it is the half that decides the size of the prize.**
These are page **visits**, not I/Os. fiqa at the gated point is (17,159 + 17,478)/40 ≈
**866 visits per query**, while the lexical channel's share of that query's buffers is
**390** — so most visits are repeat visits to pages already in shared_buffers, exactly the
overstatement this entry warned about before the instrument existed. A visit costs a pin, a
share lock and a spinlock; removing it is real work saved, but it is **not** 49.5 % of the
I/O. Sizing the I/O half needs `EXPLAIN (ANALYZE, BUFFERS)` split by channel, or a
`shared_blks_read`-level counter, and until that exists the honest claim is: **up to half
of the lexical channel's buffer-access traffic is removable in principle; how much of it is
disk is unknown.**

**One negative result worth keeping.** The skip path is **unreachable from the regression
fixture**: at 4,000 dense rows, seven query shapes (conjunctions of common with rare, two
rare terms at opposite ends of the docid space, ranked disjunctions at k = 1, 2, 3, 10, and
an `ORDER BY` with a `WHERE`) all streamed the posting lists through `wand_load_block` and
**none** called `wand_skip_blocks` at all. `sql/chanstats.sql` therefore asserts
`lex_pages_load > 0` and records `lex_pages_skip = 0` with the reason, rather than
asserting a control it cannot satisfy. The first shape tried there was worse than useless
and is documented in place: `count(*) ... WHERE d @@@ 'common & needle'` returns the right
answer with **both** counters at zero, because that path never opens a cursor — had the
control been written as "the counters are non-negative" the instrument would have shipped
wired to a branch nothing in the suite reaches.
