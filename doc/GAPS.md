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

**STILL OPEN, deliberately: the autovacuum path does not reclaim tombstones.**
`weave_vacuumcleanup()` has an independent trigger for the same rewrite with the
identical blind spot (it fires only when free pages exceed 25% of the file). It is
left alone because `weave_vacuum()` takes `AccessExclusiveLock`, which is what
licenses `weave_page_recyclable()` to bypass `GlobalVisCheckRemovableXid` and reuse
pages inside the same call — that is why vacate+pack converges in one pass there.
Under autovacuum's `ShareUpdateExclusiveLock` a concurrent scan can exist, the gate
must stand, phase 2 cannot pack into the pages phase 1 just freed, and the rewrite
would **extend**. That is exactly how pg_fts produced its 35 → 52 → 69 MB. So
tombstone reclaim currently **requires an explicit `weave_vacuum()`**, and `t/008`
has an arm that pins the limitation by measurement (264 → 267 → 267 across two
plain `VACUUM`s: no growth, no reclaim) rather than leaving it an assumption.
Closing it needs a rewrite that is correct under a share lock, which is its own
task and its own measurement.

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

### G17 — `weave_free_page()` emits one WAL record per page — **OPEN, identified 2026-09-14**

`weave_free_page()` (`src/am/am.c`) wraps each page in its own
`GenericXLogStart`/`Finish`, and it is called in a loop from `weave_free_chain()`
and `weave_free_segment()`. Freeing a segment therefore emits one full-page delta
per page. Upstream measured the same shape at 3.8 GB / ~489k pages: its vacuum ran
**113+ minutes without finishing** (gdb showed progress, not a hang).

This is now **more** likely to be reached, not less: **L18 makes `weave_vacuum()`
actually rewrite tombstone-heavy segments**, so the free path runs on real
workloads where it previously did nothing. Our reclaim test frees ~2000 pages and
is fast; a multi-GB index has not been measured here.

A fix means batching pages into one `GenericXLog` record, which is bounded by
`MAX_GENERIC_XLOG_PAGES` (4) — so the best available win is **~4×, not the order of
magnitude the page count suggests**. Upstream deliberately did not rush it into a
correctness release, and neither should we: it needs its own measurement on a large
index, and that measurement should establish whether 4× is enough to matter before
any code is written.

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

Against the full separate-extension stack it is not yet a comparison: there is no
vector index and no fuzzy channel.

"Better on all dimensions" is achievable against that stack. It is not achievable
today, and the gap list above is what stands between here and there.
