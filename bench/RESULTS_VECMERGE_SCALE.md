# Result: the vector weft's merge and vacuum paths, at scale (hard rule 12)

`bench/vecmerge.sh`, driven by `bench/aws/run.sh`'s `vecmerge` job.

## Why this file exists at all, which is the least comfortable part of it

AGENTS.md hard rule 12 says a release touching tombstones, merge or vacuum needs a run
at scale, because the sibling project shipped the same P0 through a green local gate
twice. An audit on 2026-09-23, made while looking for somewhere to run G27's gate,
found there was nowhere:

- no job in `bench/aws/run.sh` built a **vector-carrying weave index at scale** —
  `run_fuse` is the only vector index in the harness and its BEIR corpora are a few
  thousand documents; `run_p0merge` is lexical-only; every other vector job is a
  standalone C binary with no index in it;
- **no job called `weave_check()` at all.**

So every vector merge and vacuum change this project has shipped was gated by
`installcheck` at 300 rows. Hard rule 12 has not been *unenforced* for the vector
channel; it has been **unenforceable**. That is the finding this document leads with,
because it is older and larger than anything measured below.

## Run 1 — 2026-09-23, commit `3076580`, EC2 `c7i.8xlarge`, PG17

1M × 960-d GIST (999,990 rows after 10 zero-norm drops), built on 800,000 rows, then
four insert+merge cycles of 50,000, then a 10 % delete and three vacuum cycles with
the xid horizon advanced between them.

### What held

| assertion | result |
|---|---|
| `weave_check(deep)` after build, 4 merges, 3 vacuums | **clean, 8 of 8**, with `vector_block_stats_match_codes` present in every one |
| every merge rewrote the directory | blocks 25,000 → 26,563 → 28,125 → 29,688 → **31,250** |
| live-lane digest across every merge | **byte-identical** (`1720120588581082`), so nothing re-encoded and no lane moved onto another document |
| the weft across all three vacuum cycles | **byte-stable**: `vector_codes` 84,375, `vector_dir` 1,005, `vector_meta` 1, `vector_warp` 884, blocks 28,125, digest unchanged |

Build 591.9 s; merges 418.5 s each; vacuums 1230.8 / 545.9 / 1079.6 s. Each deep check
is ~5 minutes at 25–31k blocks, because it recomputes every directory record from the
codes stored beside it and then opens the page each record names.

### What failed: the index grew 1.49× across vacuum cycles, and it is NOT the weft

`relpages` 190,091 → 185,234 → **283,924** over cycles 1→2→3. Split against the
vector buckets, which is the measurement that answers the only question that matters
here:

| cycle | total pages | vector pages | non-vector pages |
|---|---|---|---|
| vacuum1 | 190,091 | 86,265 | 103,826 |
| vacuum2 | 185,234 | 86,265 | 98,969 |
| vacuum3 | 283,924 | 86,265 | **197,659** |

**Every vector bucket is constant to the page.** The growth is entirely in the lexical
half. So G27 is exonerated — by its own data, not by argument — and there is a separate
page-accounting problem in the non-vector path that only a scale run could show. It has
the shape of `doc/GAPS.md` **G18**, which L19 was supposed to have closed.

### But "unbounded growth" is NOT established, and the instrument was wrong

The same script at 20k × 96-d produces **2,577 → 4,039 → 2,577 → 4,039**: a period-2
**oscillation**, peak/trough 1.567×, reproducible. A `cycle1 vs cycle3` ratchet cannot
tell a trend from a swing, because its verdict depends on the parity of the cycle the
relocation pass happens to fire on. Run 1's verdict is therefore **provisional** in
exactly the way hard rule 11 means.

The vacuum legs now run **four** cycles and compare **matching parity** (3 against 1, 4
against 2), and the peak-to-trough ratio is reported either way — a transient 1.57× is a
real cost to a user even when it is bounded, and it is a *different defect* from a
ratchet. Run 2 exists to tell them apart.

### Two harness defects, both found by the failure rather than by review

Both are the same shape — **an assertion decided what else got measured**:

1. `fail()` exited where it stood, so the page ratchet took the **answer check** down
   with it.
2. Worse: `run_vecmerge` died on the clean arm's status **before the mutation
   control**, so the control never ran. The control is the only thing that makes those
   eight clean results mean anything (AGENTS.md's twelfth member: until a check has
   fired once, its silence is not evidence). **The strongest claim available from run 1
   is "the invariant reported clean eight times at 1M rows", not "the invariant
   held"** — and the difference is precisely what the control settles.

Fixed in `6e9be57`: `defer()` records and carries on with a non-zero exit at the end;
the control runs whatever the clean arm did, and both statuses are reported so "the
invariant works and something else is wrong" is distinguishable from "the invariant is
blind".

### One more disclosure

Run 1's host **failed one regression test** (`vecindex`, 1 of 19) and the harness
carried on, because `run_smoke` piped `make installcheck` into `tail` and read the
pipeline's status. That is fixed in `a254a3e` — a red `installcheck` is now fatal and
the override must be asked for by name. The red test was the G27 vacuum pin itself,
which passed locally only because the planner there chose a sequential scan; it says
nothing about page accounting, but the harness had no business deciding that.

## Reproduce

```sh
AWS_PROFILE=hotdog AWS_REGION=us-east-2 \
  NROWS=1000000 BATCH=50000 NBATCH=4 DELFRAC=10 VACCYC=4 \
  bash bench/aws/run.sh c7i.8xlarge vecmerge
```

Artefacts: `bench/aws/out/<run>/vecmerge.tsv` (one row per stage per metric),
`check.<stage>.tsv` (the full `weave_check` output per stage), `mutant_check.log` (the
control).
