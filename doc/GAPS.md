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
| **G13** | **ranked retrieval loses to Timescale pg_textsearch at k=10** | **NARROWED by L14 and inverted at k=100.** On a realistic corpus (120 words/doc): k=10 behind 2.35× (rare) / 6.49× (mid) / 4.92× (common), down from 7.6/18.2/7.9×. At **k=100 pg_weave WINS rare 2.46× and ties mid (1.22×) and common (1.08×)** — its k-scaling ratio is 1.04–1.21 against pg_textsearch's 4.74–7.01. `bench/RESULTS_L14_LONG.md` | ≤ pg_textsearch |
| ~~**G12**~~ | ~~boolean NOT~~ | **CLOSED.** `count(*) WHERE 'common & !rare'` 7008 ms → **14.05 ms** (499×) by building the NOT universe lazily. Now **beats GIN's 141 ms by 10×**. | done, and a win |
| ~~**G1**~~ | ~~bare `ORDER BY <=> LIMIT` does not use the index~~ | **CLOSED by L7.** 83 ms → **0.05 ms** (1,662× par4, 7,248× serial). Now beats GIN by 1,615×/7,080× on the same form. | done |
| ~~**G2**~~ | ~~index size~~ | **CLOSED by L8/L10.** The loss was a measurement artifact: 70.7% of the file was freed pages. Live content is **46 MB vs GIN's 81 MB — 1.76× smaller.** | done, and a win |
| **G3** | ranked latency on rare terms (df 25) | 0.05 ms vs 0.03 ms — **1.7×** | ≤ GIN |
| **G4** | ranked latency on mid terms (df 2.5k) | 3.54 ms vs 2.06 ms — **1.7×** | ≤ GIN |
| **G5** | build time | **NARROWED by L12: 495.9 s → 328.0 s (1.51×).** Now **6.96×** behind pg_textsearch (47.1 s) and **1.37×** behind GIN (239.2 s), from 11.0× and 2.10×. Index size unchanged at 625 MB. **No further route identified** — L4 and L11 are withdrawn on upstream evidence. `bench/RESULTS_L12.md` | ≤ GIN |
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

1.2× is the smallest gap and the best understood: merge and vacuum compaction are
effectively single-threaded, and pg_fts measured a full build at 1091 s against
VectorChord's 57 s on the 2.19M corpus. Parallel build exists
(`amcanbuildparallel`); parallel *merge* does not. Task L4.

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

4. **G3/G4**, profile the fixed per-scan cost on EC2 and attribute the 0.05 ms
   before changing anything. New task **L9**.
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

**Standing losses after L12 and L14: G13 (ranked at k=10, 2.35–6.49×) and G5
(build, 6.96× — narrowed from 11.0× and with no further route identified).**

G13's remaining route is L2 (impact-ordered postings). G5 has none, so unless one
appears the honest position is that pg_weave builds more slowly and that is the price
of an index 1.4–1.8× smaller that needs no follow-up maintenance — and the README
should say so rather than implying a fix is pending. G1, G2, G6 and G12 are closed,
and G2 and G12 both turned out to be wins. G1, G2, and G6 are closed, and G2 turned out to
be a win — pg_weave's index is 1.76× *smaller* than GIN's, not 1.9× larger.

Compaction was also shown not to affect ranked latency, which **falsifies the
stated G3/G4 diagnosis**: the build already produces one segment, so per-segment
setup was never the cost. L9 still owes a profile, and the hypothesis it should
test next is the dictionary lookup and cursor construction rather than segment
iteration.

Against the full separate-extension stack it is not yet a comparison: there is no
vector index and no fuzzy channel.

"Better on all dimensions" is achievable against that stack. It is not achievable
today, and the gap list above is what stands between here and there.
