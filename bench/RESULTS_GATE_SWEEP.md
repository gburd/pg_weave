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
