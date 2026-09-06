# Result: EC2 smoke run — clean-host build, full test suite, bound sweep

Date: 2026-09-06. Harness: `bench/aws/run.sh`. Reproduce with

```sh
bench/aws/run.sh c7i.4xlarge all
```

Purpose: prove the build and test suite work on a **clean host** rather than only
inside the Nix devShell that produced them, and reproduce
`bench/RESULTS_BOUND_PRUNING.md` on different hardware.

## Setup

| | |
|---|---|
| instance | `c7i.4xlarge`, 16 vCPU, 30 GiB, 80 GiB gp3 |
| CPU | Intel Xeon Platinum 8488C (Sapphire Rapids), `avx512_fp16` present |
| region | us-east-2 |
| OS | Ubuntu 24.04.4 LTS, AMI resolved from SSM |
| PostgreSQL | 17, from the PGDG apt repository |
| compiler | gcc (Ubuntu default), plus clang installed |
| source | `git archive` of HEAD — only committed state is measured |
| commit | `8298a7a` |

Ubuntu 24.04 ships PostgreSQL 16, so the PGDG repository is required, not
optional. The harness adds it.

## Build

Clean build against a stock `postgresql-server-dev-17`, producing `pg_weave.so`.
No Nix, no vendored toolchain, no `-I` gymnastics beyond the one
`PG_CPPFLAGS = -I$(srcdir)/include` in the `Makefile`.

All four lint gates pass on the clean host:

| gate | result |
|---|---|
| `check-ascii` | PASS |
| `check-alloc` | PASS |
| `check-unity` | PASS |
| `check-rename` | PASS |

## Tests

| suite | result |
|---|---|
| regression (`weave`, `unicode_fold`, `idx_scan_stats`) | **3/3 pass** |
| isolation (`weave_concurrency`, `weave_cic`) | **2/2 pass** |
| TAP | **9 files, 81 tests, all pass** |
| standalone codec property test | **17,741 checks, 0 failures** |

TAP detail worth keeping, because these are the tests that justify calling the
segment engine field-tested:

```
t/001_crash_recovery.pl      ok
t/002_replication.pl         ok
t/003_corruption.pl          ok   # corrupted 15 posting page(s), 74 pending page(s)
t/004_encodings.pl           ok
t/005_concurrency.pl         ok   # reads=58049 wrong=0 / reads=58037 wrong=0
t/006_concurrent_extend.pl   ok
t/007_segment_cap.pl         ok   # final segments = 8 (hard cap 128)
t/008_vacuum_reclaim.pl      ok   # 4688 pages -> 2199 after weave_vacuum
t/009_doclen_sidecar.pl      ok
```

`t/005` is the one to note: two concurrent readers performed 58,049 and 58,037
reads against an index being modified, with **zero wrong results**.

## Determinism, incidentally confirmed

The codec property test produced **byte-identical** output on two different
microarchitectures:

| host | CPU | tightest bound/blockmax, 4-bit d=256 |
|---|---|---|
| local | Intel Core Ultra 7 258V (Lunar Lake) | 219.41x |
| EC2 | Intel Xeon Platinum 8488C (Sapphire Rapids) | 219.41x |

All four reported figures matched to the last printed digit (77.67 / 92.20 /
219.41 / 506.14). That is not the formal cross-architecture fixture gate — task
**V2** requires x86-64 *and* aarch64 with a committed hash — but it is real
evidence that the no-FMA, fixed-reduction-order rotation in
`src/vector/quantize.c` does what `include/weave/quantize.h` claims. Two different
x86 microarchitectures with different vector units agreeing exactly is the
property the wire format depends on.

## Bound pruning, reproduced

Identical to the local measurement, compiled `-march=native` on Sapphire Rapids:

| bound | coherent warp | random warp |
|---|---:|---:|
| (B1) per-coordinate LUT max | 0.0 % | 0.0 % |
| (B2) Cauchy–Schwarz | 0.2 % | 0.0 % |
| (B3) centroid + radius | **99.6 %** | 0.0 % |

`score()` call ratio: 0.004 coherent, 1.000 random. The conclusion in
`bench/RESULTS_BOUND_PRUNING.md` holds on real hardware and is not an artifact of
the local machine or compiler.

## Cost and hygiene

Three instances were launched during this session (two failed on harness bugs,
one succeeded). **All three terminated**, verified by state query, not assumed:

```
i-095c876c138815b45  c7i.4xlarge  terminated
i-074e74e1e11de3f13  c7i.4xlarge  terminated
i-09108b30fbb074930  c7i.4xlarge  terminated
```

Each run's security group and key pair were deleted too. Total spend for a
`c7i.4xlarge` run of this shape: roughly 12 minutes wall clock, well under a
dollar.

## Two harness bugs this run found

Both worth recording because both produced misleading symptoms:

1. **`ssh never came up`**, which looks like a networking fault. VPC, IGW, route
   table, public-IP assignment, and the `/32` ingress rule were all correct and
   port 22 was serving a banner. The cause was client-side: a `Host *` block in
   `~/.ssh/config` setting `ControlMaster auto` with a shared `ControlPath`, an
   explicit `IdentityFile`, and `ConnectTimeout 5`. The global identity is offered
   ahead of the launch key, so authentication fails before the launch key is ever
   tried. Fixed with `-F /dev/null -o IdentitiesOnly=yes`.
2. **`make: command not found`**, a symptom three steps from its cause. The
   `apt-get install` had failed because `postgresql-17` is not in Ubuntu 24.04's
   default repositories, and the output was redirected to `/dev/null` behind a
   `;` rather than an `&&`. Fixed by adding PGDG and by running provisioning under
   `set -e` with a checked assertion on the installed `pg_config` version.

The harness now captures `get-console-output` before dying on SSH failure, so the
next person sees what the instance thought was happening.

## What this does not tell us

- **No performance number here beyond the bound sweep.** This is a smoke run: it
  proves the thing builds and passes on a clean host. The competitive matrix
  (`bench/RESULTS_MATRIX.md`, task P3) and the lexical 5-way re-run are still
  owed, and both need a corpus load this run did not do.
- **x86-64 only.** The determinism observation above needs an aarch64 run
  (`c7g`/`c8g`) to satisfy task V2, and the ARM NEON kernels do not exist yet.
- **One instance type.** Nothing here says anything about NVMe behaviour, memory
  bandwidth sensitivity, or how any of this scales; `r6id.4xlarge` and
  `i4i.8xlarge` are the types the source projects used for those questions.
