# Benchmark & measurement craft

The hard rules in `AGENTS.md` (9–16) say *what* must hold for a number to count.
This file is the *how*: the harness traps that have made this project — and the
sibling projects it learns from — publish wrong numbers, and the concrete guard
against each. Most entries were paid for in real EC2 time or real retractions.

Every trap below has a worked example in the tree or in a sibling project's logs.
When a benchmark result contradicts one of these, the harness is the first suspect.

## Latency harnesses

A single bad harness turned a "we lose ~490×" scoreboard into "flat WINS at high
recall" once corrected (sibling project, 2026-09-25). Three independent harness
bugs stacked:

1. **Inline the query argument as a literal; never put it in an `ORDER BY`
   subquery.** `ORDER BY v <=> (SELECT v FROM q WHERE id=$n)` adds ~90 ms of
   InitPlan/materialize overhead *outside* the index scan, on every arm equally, so
   it inflates the absolute number and flatters whichever arm is slower. A real
   client sends a bound parameter — measure that.
2. **One `psql -f` session per arm, with the warm-up query discarded.** A fresh
   `psql` per query pays the per-backend cold-cache reload (~460 ms in that case)
   every single time, which swamps the thing you are trying to measure. Run all
   queries of an arm in one session after a thrown-away warm-up.
3. **Time the top-level `EXPLAIN (ANALYZE)` Execution Time, not the Index-Scan
   node.** The node time excludes the reorder-queue recheck / rerank that pg_weave
   does in-AM, so timing the node under-reports our own work and is not comparable
   across engines. The whole-query time is the honest, engine-agnostic number.

Corollary: a "seq-fallback detector" must be scoped to a seq scan on the *corpus*
table — an InitPlan seqscan on a 100-row query-set table is harmless and must not
disqualify an arm.

## Memory measurements

The sibling project's Z6 saga (six sessions) produced two rules the hard way:

- **Report `Private_Dirty`, not `ps` RSS.** `ps`/`/proc/<pid>/status` RSS counts
  the shared-memory segment, so every figure is inflated by `shared_buffers` (a
  15.26 GiB RSS was 13.30 GiB private at `shared_buffers=2GB`). Read
  `/proc/<pid>/smaps_rollup` `Private_Dirty`.
- **A memory attribution is a delta between two markers, never a cumulative
  reading.** A whole session's "85 % of peak is in `train_kmeans`" conclusion was
  RETRACTED because the stage tracer reported *cumulative* process private memory,
  not per-stage growth — the memory was already resident before that stage ran.
  Emit a baseline marker at phase entry and attribute the *difference*.

For pg_weave this lands on the build path (`ambuild.c`) and the alloc counters: a
build-memory claim needs a private-memory delta bracketed by markers, at a fixed
`maintenance_work_mem`, on a corpus large enough to matter (see G50).

## The benchmark substrate is EBS, and that changes what "faster" means

Our EC2 runs (`bench/aws/`) use EBS gp3, which is **throughput-capped**, as is
managed-Postgres storage generally. Two consequences:

- **Naive prefetch hurts on a capped device — MEASURED, REJECTED by the sibling
  project 2026-09-26.** A fixed-window (64-page) `PrefetchBuffer` readahead ahead of
  a sequential `ReadBufferExtended` made a true-cold scan **1.73× slower** (6209 ms
  → 10757 ms) because the speculative reads contend with the real sequential reads
  in the capped queue, on top of the readahead the OS/EBS layer already does. On
  uncapped local NVMe it might help — but we do not benchmark there. Do not add an
  I/O-overlap trick and cite a local-NVMe win; measure it on the EBS substrate we
  actually ship numbers from. This bears directly on G48 (the I/O-skip gap): the
  right tool is PostgreSQL's `read_stream` (PG17+, AIO-backed on PG18+), which
  *adapts* prefetch distance to the device and coalesces reads, not a fixed window.
- A cold number and a warm number are different measurements; label which. A
  connection pool creating/destroying backends is the *cold-backend* reality (each
  new backend re-pays per-backend cache install); a long-lived connection is warm.

## Profiling a build under a packaged/systemd PostgreSQL — what does NOT work

Recorded so the next EC2 profiling attempt does not re-burn the hours the sibling
project did:

- **`LD_PRELOAD` malloc interposers are stripped** by Debian's `pg_ctlcluster`
  wrapper (rejected in `environment`, removed from the systemd unit). And glibc
  routes large allocations through `mmap`, so a `malloc`-only hook misses them
  anyway — you would have to interpose `mmap` too.
- **`heaptrack --pid` fails to attach under the `postgres` uid** even with
  `ptrace_scope=0` and `gdb` installed (it needs a uid-matched FIFO).
- **systemd sanitizes env vars.** A trace-enabling env var only reaches backends if
  the postmaster is started via `pg_ctl` **directly** with `env`, not through the
  distro wrapper.
- **What DID work: instrument the project's own trace hooks** with a
  `/proc/self/smaps_rollup` `Private_Dirty` read at phase boundaries. Attribution
  then comes from your own timeline, in-process, no attach.

## Reach for a standalone harness before EC2

The sibling project settled a question that had cost four EC2 sessions **in minutes,
locally, at zero cost** by reproducing a pure-code component (`train_kmeans`, which
had no PG dependencies) outside PostgreSQL. pg_weave already banks this via
`make check-standalone` / `check-alloc`. Before provisioning a burner, ask whether
the thing under test is pure code (vector kernels, `uleven`, BM25 scoring, docvals
eval, packing) — if so, a standalone harness answers it for free, and EC2 is only
for what genuinely needs the buffer manager, WAL, a real planner, or a quiet host
for latency.

## Sweep-driver hygiene

- **Guard "is the build still running?" on `pg_stat_activity`, not `pgrep`.**
  `pgrep -f "CREATE INDEX"` matches the sweep script's *own* command line, so a
  parallelism sweep waited forever and burned ~2 h (sibling project). Query the
  server for the activity, do not grep the process table.
- One run per arm is not a result (hard rule 10): re-run an arm against itself and
  know the within-arm spread before believing a between-arm delta.
