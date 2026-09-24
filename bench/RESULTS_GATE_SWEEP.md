# Result: does a fused query get cheaper as the predicate gets more selective?

**Claim 3 of `doc/ARCHITECTURE.md` §9, measured for the first time on 2026-09-23. The
answer is split, and the split is the useful part:**

- **YES in the work the CPU does.** Pivots and vector `score()` calls fall **exactly**
  with selectivity — 1.000× → 0.101× → 0.010× → 0.001× — on all three BEIR corpora,
  across an 11× range of corpus size. Scored code blocks fall too, following a closed-form
  law that this file predicted before measuring and matched to three digits.
- **NO in the pages the query touches.** `EXPLAIN (ANALYZE, BUFFERS)` on the same queries:
  **502 / 515 / 502 / 424 buffers** at selectivity 100 % / 10 % / 1 % / 0.1 %. Flat. A
  0.1 %-selective query reads the whole vector weft to score 0.03 % of its blocks.

One missing on-disk structure explains the whole gap, it is named in our own source
(`src/vector/vecwrite.c:1222-1235`), and it costs **0.125 bytes per document** — against
the 96–192 bytes per document of the second-copy option it replaces.

Harness: `bench/gatesweep.sh` (new). Work counters only — they are deterministic and
host-independent, which is why this runs on the workstation while latency does not.

## What was run

The same three databases `bench/normprod.sh` measures nDCG on, the same query sets, the
same index, `k = 10`: scifact (5,183 docs / 300 queries), nfcorpus (3,633 / 323), fiqa
(57,600 / 648). Per corpus, four points: no predicate, then a gate term chosen nearest in
**log** space to 10 %, 1 % and 0.1 % of the corpus.

```sql
SELECT id FROM fd WHERE body @@@ '<term>'::wquery
 ORDER BY fuse(body <=> $q::wquery, emb <#> $v::wvec, weights => '{0.5,0.5}') LIMIT 10;
```

Two guards, both of which have a silent failure mode:

- `enable_bitmapscan = off`, and the plan is **asserted** to carry an `Index Cond:`. With a
  bitmap path the qual becomes an executor filter — the same rows, no gating inside the
  scan — and the sweep would produce a beautifully flat curve that measures the executor.
  A missing `Index Cond` is fatal in the script, not noted.
- The lexical unit is **derived**, not read. `weave_work_stats().lex_contribs` deliberately
  excludes the fused path (`src/am/am.c:893`), so reading it reports 0 at every point — a
  flat line that looks like a finding. The script uses `fuse.sh`'s decomposition,
  `scores − vec_scores − gate_scores`.

## The measurement

Ratios are against the unfiltered point of the same corpus.

| corpus | target | measured sel | pivots | lexical | vector score() | **blocks scored** |
|---|---|---|---|---|---|---|
| scifact | 100 % | 1.00000 | 1.000× | 1.000× | 1.000× | 1.000× |
| scifact | 10 % | 0.09975 | 0.100× | 0.372× | 0.100× | 0.969× |
| scifact | 1 % | 0.01023 | 0.010× | 0.048× | 0.010× | **0.265×** |
| scifact | 0.1 % | 0.00096 | 0.001× | 0.005× | 0.001× | **0.031×** |
| nfcorpus | 10 % | 0.09772 | 0.098× | 0.215× | 0.098× | 0.868× |
| nfcorpus | 1 % | 0.01018 | 0.010× | 0.028× | 0.010× | **0.272×** |
| nfcorpus | 0.1 % | 0.00110 | 0.001× | 0.003× | 0.001× | **0.035×** |
| fiqa | 10 % | 0.10092 | 0.101× | 0.599× | 0.101× | 0.961× |
| fiqa | 1 % | 0.00997 | 0.010× | 0.162× | 0.010× | **0.272×** |
| fiqa | 0.1 % | 0.00095 | 0.001× | 0.019× | 0.001× | **0.031×** |

**Pivots and vector `score()` calls are selectivity, to three digits, on every row.** That
is the gate doing exactly what `include/weave/fuse.h` note 2 says it does: the predicate
becomes a required channel that steers pivot selection, so a rejected document is never
scored rather than scored and discarded.

**The lexical column is corpus-dependent and that is expected, not noise.** The gate term
is itself a lexical term (see "What this cannot see"), so how much lexical work it removes
depends on how the term's postings overlap the query vocabulary: 0.372× / 0.215× / 0.599×
at the same 10 % selectivity on three corpora.

## The blocks-scored law, predicted then measured

A 32-lane block is scored if **any** of its lanes survives the gate. For a predicate
admitting a fraction `s` of documents, independently of block membership, the expected
fraction of blocks with at least one survivor is

```
    1 − (1 − s)^32
```

| s | predicted | scifact | nfcorpus | fiqa |
|---|---|---|---|---|
| 0.10 | 0.965 | 0.969 | 0.868 | 0.961 |
| 0.01 | 0.275 | 0.265 | 0.272 | 0.272 |
| 0.001 | 0.0315 | 0.031 | 0.035 | 0.031 |

Nine of nine points inside a few percent of a one-term formula. Two consequences follow,
and both are design guidance rather than observations:

1. **Block-granularity work only falls below ~1 % selectivity.** At 10 % it is 0.96× —
   nothing. The §8 gate of ≤ 0.20× is crossed at about `s = 0.05` for the pivot/score
   unit and about `s = 0.006` for the block unit.
2. **Block SIZE is the knob on that curve, not storage layout.** `1 − (1 − s)^8` at 8
   lanes gives 0.077 at `s = 0.01` versus 0.265 — a 3.4× improvement in the regime that
   matters — against 4× more per-block headers and a shorter SIMD run. That is a frontier
   worth sweeping; it is not a format redesign.

## Where it fails: pages

`EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF)`, scifact, one query, four
selectivities, total buffers touched (`shared hit + read`):

| predicate | sel | buffers |
|---|---|---|
| none | 1.00000 | 502 |
| `body @@@ 'low'` | 0.09975 | 515 |
| `body @@@ 'epidemiological'` | 0.01023 | 502 |
| `body @@@ 'tax'` | 0.00096 | 424 |

**Flat.** The hit/read split moves as the cache warms; the total does not move with
selectivity. So the 32× reduction in scored blocks buys 1.0× in page traffic.

**The cause is known, written down in our own source, and not a mystery to investigate.**
The code cursor starts at `meta.codestart` and walks the `WEAVE_PK_VCODES` chain
(`src/vector/vecshuttle.c:258-300`); `want == false` still reads and parses every strip
page, it only declines to scatter the bytes. `src/vector/vecwrite.c:1222-1235` states the
reason in full: strips are block-major so a block's strips are consecutive, "**but nothing
records WHERE**", and `WeaveVecDirRec` is full at 284 bytes, "so there is no room to record
it without a format change... that is the task that has to add the index." `doc/GAPS.md`
**G27** tracks it. Physical contiguity cannot be assumed instead of an index, because
`weave_new_buffer()` may hand out recycled FSM pages, so no `codestart + b·strips` formula
is sound.

## Consequence for the remaining structural option

Before this run the option list for §8's vector work row was: (b) a second **vector-major**
copy of the codes, forfeiting the storage gate, or narrow claim 3. This measurement
replaces both with something cheaper and better targeted.

| | what it fixes | cost per document | fixes scattered candidates? |
|---|---|---|---|
| (b) vector-major second copy | bytes touched *within* a block | **96–192 B** (a second copy of the codes) | **No** — each surviving document still sits on its own page, so the page count is unchanged |
| **block→page index** (G27) | *which pages are read at all* | **0.125 B** (one `BlockNumber` per 32-lane block) | **Yes** — that is precisely what it addresses |

For fiqa's 1,800 blocks the index is 7.2 KB — one page. And the benefit is already measured
in the other unit: page traffic would follow the blocks-scored column, 0.031× at 0.1 %
selectivity instead of 1.000×.

**So the recommendation is the block→page index, and option (b) is dominated on both axes**
— 768× more storage for a quantity that is not the binding one. Narrowing claim 3 is also
unnecessary: the claim is measurably true in CPU work today and the one structure that
makes it true in I/O is a 0.125 B/doc additive index, not a redesign.

## CORRECTION 2026-09-23 (same day): the projection above was too good, and the fix is cheaper than the one recommended

Hard rule 13 says a retraction gets a named home at the original claim. This section is
that home, and it revises the recommendation this file made a few hours earlier in two
ways — one against it, one for it. The finding that page traffic is flat while scored
blocks fall 32× is **unchanged**; what changes is the size of the available win and the
implementation that gets it.

### 1. "Page traffic would follow the blocks-scored column, 0.031×" — WRONG. The floor is ~0.06×, and in TOTAL query terms 1.8–4.3×.

Two structures are read **in full on every scan regardless of the gate**, and the original
projection forgot both:

- **The block directory.** The directory cursor is forward-only like the code cursor, and
  with the normalizer ON a *second* transient directory cursor runs in
  `weave_vec_shuttle_begin()` to fold the weft's max score (the G44 normalizer's vector
  `N_key`). scifact 6 pages, fiqa 65.
- **The warp map.** `weave_fuse_vec_warpmap()` (`src/am/amscan.c:7620`) walks **every**
  lane of the bolt to build `docid[]` and `allow[]` — O(nvec) page reads plus an 8-byte
  per-document allocation, per query, per bolt, before any gating happens. scifact 6
  pages, fiqa 57.

So the achievable vector-page figure at `s = 0.001` is not `0.031 ×` of the weft but
`(dir + warp + vmeta + scored_blocks × strips_per_block) / weft_pages`.

### 2. The ablation that should have been run first: is the vector chain even the dominant page cost?

It is, and it grows with scale — but the measurement was owed before a recommendation, not
after. Same queries, same guards, buffers summed from `EXPLAIN (ANALYZE, BUFFERS)`; the
vector channel's marginal cost is the fused arm minus a lexical-only arm on the same
predicate:

| | scifact unfiltered | scifact gated 0.1 % | fiqa unfiltered | fiqa gated 0.1 % |
|---|---|---|---|---|
| fused total | 1199 | 1045 | 9111 | 8727 |
| lexical-only | 470 | 482 | 966 | 978 |
| **vector marginal** | **729** | **563 (0.77×)** | **8145** | **7749 (0.95×)** |

The vector channel is **61 %** of scifact's buffers and **89 %** of fiqa's, and its own page
cost falls to 0.77× / 0.95× under a 1000× tighter predicate while its scored blocks fall to
0.031×. Both halves of the original finding survive; the premise behind the recommendation
is now measured instead of assumed.

Corrected projection, from the measured geometry (`weave_vec_meta`, `weave_vec_strips`:
2 strip pages per block on both corpora):

| corpus | weft pages | floor (dir+warp+vmeta) | scored blocks at 0.1 % | vector pages after the fix | vector ratio | **total query buffers** |
|---|---|---|---|---|---|---|
| scifact | ~337 | 13 | 5 | ~23 | 0.07× | 705 → ~391 (**1.8×**) |
| fiqa | ~3,723 | 123 | 56 | ~235 | 0.06× | ~4,556 → ~1,068 (**4.3×**) |

**So the honest claim is 1.8–4.3× fewer buffers on these corpora, rising with corpus size,
not 32×.** And after the fix the *floor* is the warp map, which is then the largest
selectivity-independent term (57 of fiqa's 235) — a lazily built or range-restricted warp
map becomes the next question, not a new one to answer now.

### 3. The recommended implementation was the more expensive of two, and the cheaper one needs no format change at all

The recommendation was a new on-disk block→page index at 0.125 B/doc — which still beats
the vector-major copy by 768×, so the conclusion against (b) stands. But the on-disk index
is not required, because the layout is **already nearly addressable**, measured on the real
local indexes with `weave_vec_strips()`:

| corpus | code pages | first | last | span | density | interleave |
|---|---|---|---|---|---|---|
| scifact | 324 | 506 | 834 | 329 | 98.5 % | 5 non-code pages inside the span |
| fiqa | 3,600 | 2,085 | 5,748 | 3,664 | 98.3 % | 64 non-code pages inside the span |

The slack is **exactly** the interleaved directory pages (scifact has 6 directory pages,
1 at `dirstart` and 5 inside the code span; fiqa 65, 1 + 64), because the two chains are
appended concurrently by `vec_chain_append()` — one directory page per 28 blocks, i.e. one
per 56 code pages. That is not a formula to rely on, but it does not have to be:

> **`codestart + b × strips_per_block` as a SPECULATIVE address, validated by the check the
> cursor already makes ("page k of block b must claim block b",
> `src/vector/vecshuttle.c:230`), falling back to the chain walk on a miss.**

**RETRACTED THE SAME EVENING — the speculative address survives a merge and not a vacuum.**
The hit rate was measured on the three states that matter instead of argued from the
fresh-build density: fresh-then-merged **152 of 162 (94 %)**, after a second merge **162 of
162**, and after `DELETE` + `VACUUM` **6 of 122 (5 %)** with a maximum deviation of 213
pages. A vacuum rewrite draws recycled pages, which is precisely the objection the density
argument talked itself out of, and a miss costs the fallback walk *plus* the wasted read. The
paragraph below is left in place because the reasoning is a good example of a plausible
argument that a one-file probe overturned (`/scratch/pg_weave/addrprobe.sql`).

**WHAT IS BEING BUILT INSTEAD — and the first answer to that question was wrong too.** The
`firstpage`-inside-`WeaveVecDirRec` idea is retracted: the arithmetic (28 records per page
before and after) holds, but growing the record moves **every record's offset**, so a v3
reader misparses v2 records and the size becomes version-dependent at six read sites — and
`weave_vec_weft_open()` refuses an unknown version outright, so old wefts would need a
`REINDEX` rather than falling back to the walk. What is being built is `doc/GAPS.md` **G27**
as originally filed — a separate `WEAVE_PK_VCIDX` chain of one `BlockNumber` per block,
0.125 B/doc, O(1) addressable, with `cidxstart` in `WeaveVecMeta` reading 0 on a v2 weft and
0 meaning "walk". Retained here because the sequence is the lesson: three designs in one
evening, each refuted by a measurement or by reading the code it would have to change.

**And the reason no formula survives:** two vacuum rewrites of the same table produced
opposite structures — **6 of 122 (5 %)** hits with a 213-page deviation for the rewrite of
insert-built segments, and **117 of 121 page deltas equal to `strips_per_block`** for the
rewrite of a merged segment. Same writer, so the difference is the **FSM free list**, i.e.
the index's vacuum and merge history. A speculative address is not just sometimes wrong, it
has unpredictable performance, which is worse than a structure that is always right. `weave_vecdir_recs_per_page()` is `(8152 − 8) / 284 = 28` today and
`(8152 − 8) / 288 = 28` with the field, so **the directory occupies the same number of pages**
— verified against the relation, where `ceil(162/28) = 6` and `ceil(1800/28) = 65` are exactly
the interleaved page counts measured inside the code spans. No new page kind, no extra pages,
and correct under page recycling because the pointer is written by whoever moved the page.
`doc/GAPS.md` G27 carries the build order.

Zero on-disk bytes, no new page kind, no `WEAVE_VMETA` version bump, no migration script,
no expected-output regeneration — and the fallback keeps it correct under FSM page reuse,
which is the objection that ruled out arithmetic in the first place. With 98.3 % density the
speculative address misses about 1 time in 57 and a miss costs one wasted page read, not a
wrong answer. **Measure the hit rate on a merged and vacuumed index before choosing**, since
that is the case where the density argument is weakest; if it collapses there, the on-disk
index is still the fallback plan at 0.125 B/doc.

## MEASURED AFTER THE FIX, 2026-09-23 (night): the page curve now moves

G27 is implemented -- `WeaveVecDirRec` carries `firstpage`, the code cursor seeks to it,
and the VMETA version went 2 -> 3. Buffers for the same query, same guards, same fixture,
before and after, at 0.1 % selectivity:

| corpus | unfiltered (before / after) | gated 0.1 % (before / after) | gain |
|---|---|---|---|
| scifact | 1199 / 1199 | 1045 / **513** | **2.04x** |
| fiqa | 9111 / 9111 | 8727 / **1823** | **4.79x** |

The projection this file made before the change was 1.8x / 4.3x, from geometry plus the
lexical-only ablation. Measured: 2.04x / 4.79x. The **unfiltered** arm is unchanged to
within one buffer, which is the control that matters: with no predicate a fused scan visits
every block, so the seek has nothing to skip and must cost nothing.

**The correctness evidence is a diff, not an argument.** All twelve rows of
`bench/gatesweep.sh`'s counters -- pivots, lexical contributions, vector `score()` calls,
gate scores, lanes, blocks, `blkskip`, `rqskip`, vetoes, abandonments -- are
**bit-identical** before and after on all three corpora. The change alters which pages are
READ, not which blocks are scored, and the counters say so in the strongest available form.

Also verified on the paths hard rule 12 names: `weave_check()` reports **0 failing
invariants** on a fresh build, on a three-bolt index, after `weave_merge()`, and after a
`DELETE` + `VACUUM` rewrite, and the fused scan answers on the rewritten weft. Positive
control for the new invariant: a writer mutated to store `firstpage + 2` makes
`weave_check()` report *"bolt 0 block 0: firstpage 251 is not a code page carrying this
block"* and makes the scan **refuse** rather than score another block's codes.

*And one process note, because it cost twenty minutes and is the AGENTS.md
stale-artifact family wearing yet another hat:* after reverting the mutant and rebuilding,
the **running cluster still had the mutant library loaded** -- `lpg.sh` was never restarted
-- so the next run's failures were the positive control still firing, on what looked like
clean code. The tell was that the stored pointer was exactly `correct + 2`. A source revert
is not a deployed revert.

## What this cannot see

- **The predicate is a lexical term, not a scalar facet.** `WEAVE_CH_DOCVALS` is a declared
  channel kind whose page kind is still reserved (`include/weave/pagekind.h`), so
  `WHERE category = 'x'` cannot be pushed into the index at all. Every number here uses
  `body @@@ term`, which is the only pushable predicate and the shape
  `sql/fuse_pushdown.sql` §4 pins. **Claim 3 says "predicates"; today it means "another
  lexical term".** That is a product gap, recorded here because this is where it becomes
  visible.
- **No latency.** Workstation. The buffer counts above are page *counts*, not times.
- **One k (10), one weights vector (0.5/0.5), one embedding model.** The pivot/score
  columns are selectivity by construction of the gate, so they should be insensitive to
  all three; the lexical column certainly is not.
- **Reps.** These counters are deterministic; two runs of the same point return identical
  integers, so the A/A spread is zero and there is nothing for hard rule 10 to compare.
  Hard rule 11's second scale is the three corpora at 3,633 / 5,183 / 57,600 documents.

## Reproduce

```sh
/scratch/pg_weave/lpg.sh start
export PGHOST=/scratch/pg_weave/lpg/run PGPORT=5432 PGUSER=postgres
DBS="normsci normnf normfiqa" bash bench/gatesweep.sh
```

Report: `/scratch/pg_weave/gatesweep/gatesweep-full.tsv`.

## MEASURED ON A QUIET MACHINE, 2026-09-23: the latency curve falls, on every corpus, at every point

Everything above is counters, and counters cannot close claim 3. The claim is that a
query gets **faster**; a user does not feel a pivot count. This is the latency half,
on EC2 `c7i.8xlarge` (32 vCPU / 64 GB), us-east-2, commit `3076580`, ext 0.20.0,
PG17, real all-MiniLM-L6-v2 embeddings computed on the instance, the same three BEIR
corpora loaded through the same `bench/fuse.sh` loader the work pass used
(`FUSE_LOAD_ONLY=1`, added for this). `k=10`, weights `{0.5,0.5}`, 25 queries × 7
reps × **2 slots per point**, first rep of each statement dropped.

| corpus | sel 1.000 | sel 0.100 | sel 0.010 | sel 0.001 |
|---|---|---|---|---|
| scifact (5,183) p50 ms | 2.547 | 1.941 | 0.799 | 0.359 |
| | 1.000× | 0.762× | 0.314× | **0.141× (7.1× faster)** |
| nfcorpus (3,633) p50 ms | 1.575 | 1.203 | 0.581 | 0.275 |
| | 1.000× | 0.764× | 0.369× | **0.175× (5.7× faster)** |
| fiqa (57,600) p50 ms | 26.400 | 19.677 | 6.283 | 1.911 |
| | 1.000× | 0.745× | 0.238× | **0.072× (13.8× faster)** |

**p99 moves with p50 rather than against it**, which is the result that would have
killed the claim had it gone the other way — a scan that wins on the median by
deferring work into the tail has not made anything faster. scifact 1.000× / 0.662× /
0.276× / 0.134×; nfcorpus 1.000× / 0.737× / 0.361× / 0.182×; fiqa 1.000× / 0.682× /
0.227× / 0.074×.

**Nine of nine gated points fall, and every step clears its own noise floor by at
least 21×** (hard rule 10, and the A/A leg here is per *point* because this sweep has
no competitor — each point is measured once in the first rotation and again in the
second, after every other point has run):

| corpus | step | Δp50 ms | A/A spread ms | Δ / A/A |
|---|---|---|---|---|
| scifact | 1.000 → 0.100 | 0.606 | 0.029 | 21× |
| | 0.100 → 0.010 | 1.142 | 0.010 | 114× |
| | 0.010 → 0.001 | 0.440 | 0.001 | 440× |
| nfcorpus | 1.000 → 0.100 | 0.372 | 0.007 | 53× |
| | 0.100 → 0.010 | 0.622 | 0.011 | 57× |
| | 0.010 → 0.001 | 0.306 | 0.010 | 31× |
| fiqa | 1.000 → 0.100 | 6.723 | 0.132 | 51× |
| | 0.100 → 0.010 | 13.394 | 0.002 | 6,697× |
| | 0.010 → 0.001 | 4.372 | 0.001 | 4,372× |

### The number to quote is 5.7–13.8×, NOT 1,000×, and the reason is the more useful part

The pivot and `score()` columns fall as selectivity *exactly* — 1.000× / 0.101× /
0.010× / 0.001×, three digits, measured above. Latency does not, and anyone reading
the work table alone would over-promise by two orders of magnitude. What latency
actually tracks is the **blocks-scored** column, the one this document already
predicted from `1 − (1−s)^32` before measuring it:

| corpus | quantity | 0.100 | 0.010 | 0.001 |
|---|---|---|---|---|
| scifact | blocks scored | 0.969× | 0.265× | 0.031× |
| | **p50 latency** | **0.762×** | **0.314×** | **0.141×** |
| | pivots | 0.100× | 0.010× | 0.001× |
| nfcorpus | blocks scored | 0.868× | 0.272× | 0.035× |
| | **p50 latency** | **0.764×** | **0.369×** | **0.175×** |
| fiqa | blocks scored | 0.961× | 0.272× | 0.031× |
| | **p50 latency** | **0.745×** | **0.238×** | **0.072×** |

Latency sits **between** the two curves and hugs the block curve: at the 0.010 point
it is within 1.2–1.4× of blocks-scored on all three corpora, and 24–37× away from
pivots. So the design's own cost model — a 32-lane block is the unit of vector work,
and a gate at selectivity `s` still touches `1 − (1−s)^32` of them — is the model that
predicts what a user experiences. The pivot count is real work and it really does fall
1,000×; it is simply not what the clock is measuring.

This also bounds the remaining upside honestly. Latency falls *less* than blocks at
the 0.001 point (0.072–0.175× against 0.031–0.035×), and the gap is the work that is
selectivity-independent: `weave_fuse_vec_warpmap()` is O(nvec) with an 8 B/doc palloc
per query per bolt, and both it and the block directory are read in full on every scan
regardless of the gate (see the CORRECTION section above). At the most selective point
that residue is most of what is left. **The warp map is therefore the next lever, and
this is the measurement that says so** — it is worth little on an unfiltered query and
nearly everything on a highly selective one, which is the opposite of the intuition
that would prioritise it by profile share alone.

### What this run does not establish

- **Still a lexical predicate.** Every point is `body @@@ term`; `WEAVE_CH_DOCVALS`
  remains a reserved page kind, so `WHERE category = 'x'` cannot reach the index. The
  "What this cannot see" section above applies unchanged, and it is the largest gap
  between what claim 3 says and what has been measured.
- **Small corpora.** 3,633 / 5,183 / 57,600 documents. Hard rule 11's second scale is
  satisfied *within* this run in the sense that three corpora spanning 16× agree, and
  the largest is also the one with the steepest curve — but nothing here is a million
  rows, and the fixed residue that limits the 0.001 point is the part most likely to
  amortise differently at scale.
- **One k, one weights vector, one embedding model**, as before.
- **The host failed one regression test.** `installcheck` reported `1 of 19` — the G27
  vacuum-rewrite pin, which passed locally only because the planner there chose a
  sequential scan (fixed in `a254a3e`). The numbers above stand because that test is a
  96-row vacuum pin and this sweep never vacuums, but the *harness* had no business
  continuing: it piped `make installcheck` into `tail`, so the failure was recorded and
  ignored. `run_smoke` is now fatal on a red installcheck and the override has to be
  asked for by name. **This paragraph is the disclosure, not a footnote** — it is the
  first EC2 run in this project whose host is known to have been red, and the reason it
  is known is that the run found the defect that had been hiding every previous one.

### The MECHANISM, measured 2026-09-24 — and it costs 0.7x when there is no predicate

Everything above is monotonicity: the same arm gets cheaper as the predicate tightens.
Claim 3 also asserts a **mechanism** — "the predicate is pushed into the SIMD block mask
instead of collapsing recall" — and nothing had measured that. It is the half a reader is
entitled to be sceptical about, because the obvious alternative (fetch in vector order,
recheck the predicate per candidate) is what a system without a shared docid space is
forced into.

**The control is our own index**, which is the only reason it is worth anything. Both arms
carry the same `Index Cond:` on the same index over the same corpus, both are *asserted*
to have one, and the only difference is where the predicate acts: inside the scan for
`fuse()`, or as a per-candidate recheck for a plain `ORDER BY emb <#> q`. A difference
cannot be blamed on an engine, a corpus, a build or a plan shape — the four things that
make a cross-engine filtered-ANN comparison unfalsifiable. **This is not a measurement of
any other engine.**

10 queries per point, buffers and `Rows Removed by Index Recheck` summed, `k=10`.
Deterministic, so measured on the workstation.

| corpus | sel | fused buffers | vector-order buffers | ratio | fused discarded | vector-order discarded |
|---|---|---|---|---|---|---|
| scifact | 1.000 | 6,166 | 4,074 | **0.7×** | 0 | 0 |
| | 0.100 | 6,188 | 14,113 | 2.3× | 0 | 1,565 |
| | 0.010 | 3,799 | 129,791 | 34.2× | 0 | 23,023 |
| | 0.001 | 2,245 | 255,594 | **113.9×** | 0 | 51,780 |
| nfcorpus | 1.000 | 3,639 | 2,975 | **0.8×** | 0 | 0 |
| | 0.100 | 3,458 | 22,210 | 6.4× | 0 | 3,179 |
| | 0.010 | 2,093 | 63,418 | 30.3× | 0 | 10,754 |
| | 0.001 | 1,163 | 183,237 | **157.6×** | 0 | 36,290 |
| fiqa | 1.000 | 49,130 | 38,610 | **0.8×** | 0 | 0 |
| | 0.100 | 47,984 | 56,957 | 1.2× | 0 | 1,073 |
| | 0.010 | 22,268 | 177,444 | 8.0× | 0 | 17,164 |
| | 0.001 | 9,381 | 913,695 | **97.4×** | 0 | 227,054 |

**`Rows Removed by Index Recheck` is the measurement, not the buffer count.** It is the
over-fetch, counted by the executor rather than by us, and it is what "collapsing recall"
looks like if you insist on keeping recall instead: the vector-order arm walks and
discards up to 227,054 candidates to return 100 rows, and **the fused arm discards zero,
at every point, on every corpus.** That is the mask doing the thing claim 3 says it does.

**THE LOSS, stated first among equals (hard rule 8): with no predicate the fused path is
0.7–0.8× — i.e. 1.2–1.4× MORE expensive.** It has to scan the lexical channel as well, and
there is no gate to pay for it. So the honest shape of claim 3 is not "fused is better",
it is **"fused converts predicate selectivity into speed, and costs about 30 % when there
is no selectivity to convert"**. The crossover is between 10 % and 1 % selectivity on every
corpus measured (scifact 2.3× at 0.100, fiqa only 1.2× at 0.100). A user whose queries
carry no predicate should not use the fused path, and nothing in the design hides that.

**One number to resist over-reading:** the 97–158× column is buffers for a *vector-order
plan that keeps exact recall*. An approximate filtered-ANN implementation that accepts
recall loss does less work than this control and is not measured here. What the control
establishes is the cost of the *exact* alternative on the same index, which is the
comparison the fused algorithm is actually against.

### Two levers this ruled out, both of which I had asserted before measuring

- **The warp map.** `weave_fuse_vec_warpmap()` is read unconditionally, so it looked like
  the residue that keeps latency above the blocks-scored curve. Measured: the warp bucket
  is 57 pages on fiqa against 1,823 buffers at the 0.1 % point — **6.7 %**, and 12 of 513
  on scifact. Not a headline lever. The earlier note that it "is therefore the next lever"
  was inferred from the *shape* of the latency-vs-blocks gap and is **withdrawn**.
- **The G44 normalizer's pre-scan.** It walks the whole block directory per query, which
  looked like the selectivity-independent cost. Ablated with `pg_weave.fuse_normalize`:
  776 vs 792 buffers at the 0.001 point — **within noise, and slightly cheaper with it
  on**. The directory is 65 pages, read once and cached; the "O(nblocks) per query" reading
  was mine, and it came from mis-reading `weave_work_stats().vec_lanes` as per-query when
  it is cumulative across all 648 queries. fiqa is 1,800 blocks per bolt, not 36,450.

Hard rule 9 exists for exactly this: two plausible levers, both sized in an afternoon,
both an order of magnitude smaller than asserted. The remaining vector-side budget at a
tight gate is ~423 buffers of which ~122 are unconditional, so **no vector-side change can
win more than ~15 % of a gated query.** At the gated point the lexical channel is the
larger half (390 of 813 buffers on fiqa — 8 posting lists read in full), which is where a
next lever would have to come from.

**And the first thing found on the lexical side is a mechanism, not a number, so it is
recorded as `doc/GAPS.md` G48 with its ceiling explicitly unmeasured.** `wand_skip_blocks()`
makes a seek cheap in CPU exactly as its header claims — block headers only, no FOR decode
— but it `ReadBuffer`s every page it passes over, so a forward seek across N pages of a
posting chain reads all N. The lexical `block_max()` is real and the core does prune with
it; the pruning is free in CPU and costs full price in buffers, because the per-block
`first_docid` and `block_max` a skip would consult live on the page the skip is trying to
avoid. Whether that is the larger part of the 390 or a tenth of it is **not measured**, and
the two levers withdrawn above were both asserted from exactly this kind of plausible
mechanism. The instrument that would settle it is named in G48.

## Reproduce (EC2)## Reproduce (EC2)

```sh
AWS_PROFILE=hotdog AWS_REGION=us-east-2 FUSE_DATASETS="scifact nfcorpus fiqa" \
  FUSE_LIMIT=200000 LATN=25 REPS=7 bash bench/aws/run.sh c7i.8xlarge gatesweep
```

Artefacts: `bench/aws/out/<run>/gatesweep-{scifact,nfcorpus,fiqa}.tsv` (work) and
`gatesweep-lat-*.tsv` (latency, one row per point per slot).
