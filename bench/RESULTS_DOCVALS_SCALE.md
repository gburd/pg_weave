# RESULTS: docvals correctness AT SCALE (10M) — the hard-rule-12 gate for G51 + G52

**Date:** 2026-09-27
**Harness:** `bench/aws/run.sh m7i.4xlarge docvals` (job `run_docvals`), workload
`bench/aws/docvals_scale.sql` (correctness, fatal) + `bench/aws/docvals_prize.sql`
(prize, best-effort). Reproduce:

```sh
AWS_PROFILE=hotdog bench/aws/run.sh m7i.4xlarge docvals
```

**What this discharges.** Hard rule 12: "when a release touches tombstones, merge or
vacuum, local green is not evidence." G51 (a segment MERGE must carry each input's
docvals weft, including the delete-heavy tombstone-drop path) and G52 (a post-build
INSERT is answerable via the pending buffer, and stays answerable once the flush folds
it into a segment) were both local-green only. This is the at-scale run.

## Setup

| | |
|---|---|
| Host | EC2 `m7i.4xlarge` (16 vCPU, 64 GB), us-east-2, on-demand |
| PostgreSQL | 17.11 (Ubuntu package), data dir on gp3 EBS |
| pg_weave | commit `8ef4dfc` (sparsemap 5.6.0 + G51/G52 fixes), built + regression + isolation + 19 TAP files (283 tests) green on the host before any number was taken |
| build settings | `maintenance_work_mem=4GB`, `work_mem=256MB`, `max_parallel_maintenance_workers=4` (recorded per METHODOLOGY; weave's own 32 MB flush budget is hardcoded and independent, so the build is multi-segment + merge regardless) |
| corpus | 10,000,000 rows. `body='common doc '||(id%5)||' t'||(id%100000)||' freq'||(id%100)`, `d=to_wdoc(body)`, `price=((hashint8(id)%1000)+1000)%1000` — a **scattered, docid-uncorrelated** int8 facet (1000 distinct values, `price<100`≈10%), the worst case for docid-contiguity pruning and the same choice Task 8 made |
| index | `weave (d, price int8_docval_ops)` — **no vector column** (see "what this does not tell us") |

## Correctness: index == heap at every phase

The workload is self-checking: each `dvs_assert_agree(pred)` builds the index-forced
answer (`enable_seqscan/bitmapscan=off`) and the seqscan answer as TID sets and RAISEs
on any set difference, so `ON_ERROR_STOP` makes a wrong answer at scale FATAL. Every
assertion below reported **`disagreements=0`**. Counts are deterministic (host-
independent); only the timings are indicative on a shared burner.

| phase | operation | assertions (all `disagreements=0`) | wall |
|---|---|---|---|
| 1 build | 10M CTAS; `CREATE INDEX` (internal multi-segment → 1) | — | CTAS 14.6 s, index 32.8 s |
| 2 after build (G51) | gate == heap on the built (merged) segment | `price<100`=998,991; `<10`=99,698; `<1`=10,067; `=500`=10,003; `>=990`=100,118; `@@@'freq7' AND price<100`=10,046 | ~2.3 s/10% arm |
| 3 delete 40% + VACUUM | `DELETE WHERE id%5<2`; VACUUM → tombstone-drop rewrite | `price<100`=599,976; `<10`=59,921; `<1`=6,074; `@@@∩`=10,046 | VACUUM 10.2 s |
| 4 explicit merge | `weave_merge()` | `price<100`=599,976; `<10`=59,921; `@@@∩`=10,046 | ms (already 1 seg) |
| 5 insert 200k **pending** (G52) | `INSERT` 200k new rows (pending buffer) | `price<10`=**63,891** (the pending rows are seen — pre-fix they were dropped); `@@@'freq7' AND price<100`=10,433 | insert 1.4 s |
| 6 flush + merge | VACUUM (flush pending → segment) + `weave_merge()` | `price<10`=61,891; `price<100`=619,952; `@@@∩`=10,251 | flush/VACUUM 7.2 s |

Final: `nsegments=1`, index **453 MB**, heap **2111 MB**.

## The prize, re-confirmed at a second scale (rule 11)

`bench/aws/docvals_prize.sql` on a **separate 1M-row vector-bearing table** (`dvsv`,
`weave(d, emb wvec(4), price int8_docval_ops)`), fused top-k
`ORDER BY fuse(d <=> 'common', emb <#> q, weights='{0.5,0.5}') LIMIT 10` narrowed by
`price < K`. Plan confirmed `Index Cond: (price < 100)`. PRIMARY METRIC
`weave_work_stats().vec_blocks` (deterministic):

| facet selectivity | vec_lanes | **vec_blocks** |
|---|---|---|
| 0.10 (`price<100`) | 4,813,125 | **150,890** |
| 0.01 (`price<10`)  | 1,356,500 | **42,525** |
| 0.001 (`price<1`)  | 160,450   | **5,030** |

`vec_blocks` **falls monotonically ~30×** as the facet tightens 100× — the vector scan
does *less* work as the predicate gets more selective. This is claim 3
(`ARCHITECTURE.md` §9) at 1M (~17× Task 8's fiqa 57.6k), so the Task 8 result
(`bench/RESULTS_DOCVALS_PRIZE.md`) is no longer single-scale.

## What the numbers mean

- **G51 holds at scale.** The gate is exact (`disagreements=0`) after the build's
  internal multi-segment merge, after an explicit `weave_merge()`, and — the important
  one — after a **40%-delete VACUUM** that rewrites the segment dropping tombstoned
  docids while carrying the survivors' docvals. Before the G51 fix this returned zero.
- **G52 holds at scale.** 200k rows inserted post-build are found via the pending
  buffer (`price<10` rose 59,921 → 63,891, exactly the new low-price rows), the
  `@@@ AND price` conjunction over pending is exact, and after the flush folds them into
  a segment the gate stays exact.
- Both are now backed by an at-scale run, not just local green. **Rule 12 discharged for
  `int8_docval_ops`.**

### Type slice 2 (float8) validated at 10M (added 2026-09-27, ext 0.26.0)

The same 10M run carries a second index on a **float8** facet (`fprice`, a scattered
hash rank / 7.0) — `weave (d, fprice float8_docval_ops)` — asserted alongside the int8
gate at every phase. The float8 encode (the monotonic IEEE-754→int64 transform,
`weave_dv_encode_f8`) is the type slice's whole correctness risk, so proving it under a
real 10M merge/delete/pending/flush — not just the 500-row regression — is the point:

| phase | float8 assertion | `disagreements` |
|---|---|---|
| after build | `fprice < 10.0` = 699,066; `fprice < 4.5::float4` (cross-type) = 319,008 | 0 |
| delete 40% + VACUUM | `fprice < 10.0` = 420,098 | 0 |
| flush + merge | `fprice < 10.0` = 447,943 | 0 |

So float8 (including the cross-type float4 constant) holds `index == heap` through the
identical merge/pending/vacuum pipeline int8 uses. The other four types (int4/int2/date/
bool) share that pipeline and encode by a plain widening whose order-preservation is not
in question; the regression (`sql/docvals.sql` §8) covers them, and the float8 result is
the at-scale evidence for the slice.

### NULLs validated at 10M (added 2026-09-27, store format v2)

The same 10M run carries a third index on a **NULLABLE** int8 facet (`nprice`, ~10 %
NULL — a hash rank ending in 0 — else the same scattered 0..999) — `weave (d, nprice
int8_docval_ops)` — asserted alongside the non-null gates at every phase. Shape confirmed
before the build: `pct_null = 10.01 %` of 10,000,000 rows. A NULL satisfies no comparison
(SQL three-valued logic), so the seqscan oracle excludes it; `disagreements = 0` therefore
proves the **v2 null bitmap** excludes it too, through the same merge/vacuum/pending
pipeline hard rule 12 governs. The load-bearing check is that `nprice < 100` and
`nprice IS NOT NULL AND nprice < 100` return the **same** set:

| phase | nullable assertion | `disagreements` |
|---|---|---|
| after build | `nprice < 100` = 899,173 **==** `nprice IS NOT NULL AND nprice < 100` = 899,173; `nprice >= 990` = 90,058 | 0 |
| delete 40% + VACUUM | `nprice < 100` = 540,224 | 0 |
| explicit merge | `nprice < 100` = 540,224 | 0 |
| +200k INSERT (pending) | `nprice < 10` = 57,428 | 0 |
| flush + merge | `nprice < 10` = 55,627; `nprice < 100` = 558,297 | 0 |

The build-phase equality (899,173 == 899,173) is the tooth: a NULL stores a placeholder
value the bitmap masks, so a dropped or mis-carried bitmap would let those ~1 M NULLs
answer the comparison and diverge the two sets. They coincide at 10M across build, the
tombstone-drop VACUUM, an explicit merge, the pending buffer, and the flush — the paths a
freshly-built index does not exercise. This is the at-scale (rule 12) half of the NULL
slice (`doc/plans/2026-09-27-docvals-nulls-slice.md`); the property test, the
`sql/docvals.sql` §9 regression, the `fuzz_docvals` v2 teeth, and the `t/020`–`t/022`
crash-recovery / torn-write / concurrency TAP are the local half. Index 456 MB / heap
2295 MB; `nsegments` stayed 1 (the build merges internally). Instance c7i.4xlarge
(Xeon Platinum 8488C, 30 GB), commit `5635689`.


## What this does NOT tell us (and one thing it surfaced)

- **No vector column in the 10M correctness run**, on purpose: a first attempt with
  `emb wvec(4)` spent >61 min single-threaded in `weave_vec_block_read` (the vector-weft
  build/merge at 10M) — an unrelated bottleneck that never reached the docvals
  assertions. The vector channel's build/merge cost at 10M is untested here and is its
  own question. The prize table above therefore comes from a 1M table, not 10M.
- **`nsegments` stayed 1** at every observable phase: the build merges internally to one
  segment, so the multi-segment merge is exercised *inside* `CREATE INDEX` and by the
  phase-6 flush (→2→1), not as a persistent multi-segment directory. A variant that
  holds several segments (the `fuse_degenerate` INSERT+VACUUM-below-threshold fixture)
  would exercise a merge of >2 inputs and is worth adding.
- **Latencies are indicative only** (shared burner). The wall-clock column is for order
  of magnitude, not comparison.
- **SURFACED — a lexical-channel memory blowup, NOT docvals (doc/GAPS.md G53).** The
  first workload used `@@@ 'common'`, a term matching ~100% of the corpus, and the
  backend was OOM-killed (signal 9) at **60 GB anon-rss**. Isolated on the kept
  instance: `@@@` memory scales **super-linearly with df** — a 20%-df term (2.04M
  matches) peaks at **4.6 GB**, a ~100%-df term (6.2M) exceeds 60 GB. This is
  pre-existing in the lexical collect (nothing to do with docvals — `@@@ 'common'` alone
  crashes) and would bite real corpora with common terms. The docvals workload now uses
  `freq7` (df≈1%); the lexical issue is filed separately as G53.
