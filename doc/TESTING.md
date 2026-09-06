# Test strategy

The operative guide is `.agent/skills/weave-testing/SKILL.md`, which has the
commands, the layer table, and the technique for proving an expected-output diff
is non-semantic. This file records the strategy behind it.

## Five layers, and what each structurally cannot catch

| layer | catches | blind to |
|---|---|---|
| `pg_regress` (`sql/`, `expected/`) | SQL surface, planner shape, error text, formatting | concurrency, crashes, anything corpus-dependent |
| isolation (`test/isolation/`) | lock order, merge-vs-scan races, CIC correctness | single-backend logic errors |
| TAP (`t/`) | crash recovery, replication, corruption, encodings | fine-grained algorithmic errors |
| property (`test/hegel/`) | invariants over generated input | anything needing a live backend |
| fuzz (`test/fuzz/`) | crashes and UB on adversarial bytes | logic that is wrong but not crashy |

## Why the property layer is mandatory rather than nice-to-have

This is the one strategic point in this document.

A `block_max()` that is slightly too low causes the fused scorer to prune a
document that belonged in the top-k. The query returns plausible search results,
just missing some. There is no error, no warning, and no crash.

**A fixed-expected-output regression test cannot catch this.** It compares against
one recorded answer on one corpus. Whether the bug is visible depends entirely on
the data, so the test passes on the corpus you have and the bug ships.

The only test shape that catches it is: generate random input, compute the exact
answer, assert the bound dominates it, repeat many times. Hence the rule in
`AGENTS.md`: every channel needs a bound property test, and a channel without one
is not merged.

The same argument applies to codecs. `include/weave/for.h` says so at the top of
the file and was written backend-independent specifically to make it possible;
`include/weave/quantize.h` follows the pattern. `test/hegel/test_quantize.c`
currently runs **17,741 checks with 0 failures** across 7 dimensions and 3 bit
widths, linking `src/vector/quantize.c` and `src/vector/pack.c` with no PostgreSQL
at all.

If an algorithmic core cannot be linked into a plain `gcc` invocation, that is a
reason to restructure it, not a reason to skip the test.

## Two bugs the property test found immediately

Both in code that looked obviously correct, and neither would have been caught by
any other layer:

1. **Infinite loop in the rotation's Fisher-Yates shuffle.** The rejection
   threshold `2^32 - (2^32 mod bound)` was computed in `uint32`, where it
   truncates to 0 whenever `bound` divides `2^32` — which every power-of-two
   dimension triggers on the first step. The function simply never returned.
2. **Exponential "adaptive" quadrature.** The Lloyd-Max codebook solver's adaptive
   Simpson recursion had a depth cap of 40 with a tolerance tight enough to
   resolve a spiked density: 2^40 evaluations, so the solve never completed.

Both surfaced within a minute of running a standalone test. Neither is the kind of
bug a code reviewer catches by reading.

## What is not a test

A benchmark is not a test. `bench/RESULTS_BOUND_PRUNING.md` measures whether a
bound prunes; that is a design question, not a correctness question. But the
harness that answers it also asserts bound soundness on every block of every
query, so it doubles as a large randomized correctness run. Prefer that shape:
when a benchmark can cheaply assert correctness too, make it.

Conversely: **never record a latency without first verifying correctness on the
same build.** `AGENTS.md` rule 8 and the pg_turbovec retraction it cites.

## Coverage expectations by phase

`doc/PHASES.md` gates are the definition. Summarized:

- A new channel: bound property test (C1 + C2), a `weave_check()` invariant, and a
  differential test against a reference implementation.
- A new on-disk structure: a fuzz target, a `weave_check()` invariant, and a
  crash-recovery TAP case.
- A new SIMD path: bit-exact equivalence with the scalar reference, run in CI on an
  appropriate runner or under emulation.
- A new query form: a `pg_regress` case, plus a degenerate case proving it reduces
  to the existing single-channel path byte-identically.
