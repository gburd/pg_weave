---
name: weave-testing
description: Use when adding or debugging tests in pg_weave, or deciding which test layer a new behaviour belongs in. Covers pg_regress, isolation, TAP, property-based (hegel), and libFuzzer, plus the technique for proving an expected-output diff is non-semantic. Load this before editing anything under test/, t/, sql/, or expected/.
---

# Testing pg_weave

Five layers. Each catches a class the others structurally cannot.

| layer | catches | cannot catch | run with | lives in |
|---|---|---|---|---|
| pg_regress | SQL surface, planner shape, error messages, output formatting | anything needing concurrency, crashes, or a different corpus | `make installcheck` | `sql/`, `expected/` |
| isolation | lock ordering, concurrent merge vs scan, CIC correctness | single-backend logic errors | `make installcheck` | `test/isolation/` |
| TAP | crash recovery, replication, corruption, encodings, cluster-level behaviour | fine-grained algorithmic errors | `nix build .#checks.x86_64-linux.tap-pg17` | `t/` |
| property (hegel) | **invariants over generated input** — bound soundness, codec round-trips, monotonicity | anything needing a live backend | plain `gcc` + run | `test/hegel/` |
| libFuzzer | crashes and UB on adversarial bytes in a decoder | logic that is wrong but not crashy | `test/fuzz/run.sh` | `test/fuzz/` |

## Why the property layer is mandatory

A `block_max()` that is slightly too low silently drops rows from the top-k. The
query returns plausible search results, just missing some. A fixed-expected-output
regression test **cannot** catch this: it compares against one recorded answer on
one corpus, and the bug's visibility depends entirely on the data.

So every channel needs a property test asserting `bound ≥ score` over randomized
input. This is a hard rule in `AGENTS.md`, enforced at review.
`test/hegel/test_quantize.c` is the worked example — 17,741 checks across 7
dimensions and 3 bit widths, asserting contracts (C1) and (C2) from
`include/weave/channel.h` plus codec round-trips and estimator unbiasedness.

## Making a core property-testable

The trick is a design decision, not a testing one: keep the algorithmic core
**backend-independent** and have it take allocator function pointers, so the
backend passes `palloc`/`pfree` and the test passes `malloc`/`free`.

`include/weave/for.h` states this intent explicitly at the top and is the original
example; `include/weave/quantize.h` follows it. The result is that the codec — the
part most likely to be subtly wrong — links into a plain `gcc` invocation with no
PostgreSQL at all:

```sh
gcc -O2 -I include -o /tmp/tq test/hegel/test_quantize.c \
    src/vector/quantize.c src/vector/pack.c -lm && /tmp/tq
```

If a core cannot be tested this way, that is a reason to restructure it, not a
reason to skip the test.

## Proving an expected-output diff is non-semantic

You will be tempted to regenerate `expected/*.out` to make a test pass. Rule 3 in
`AGENTS.md`: prove the diff is non-semantic first. The technique, used for real
during the pg_fts fork:

```python
import re
for t in tests:
    e = open(f"expected/{t}.out").read().splitlines()
    r = open(f"results/{t}.out").read().splitlines()
    assert len(e) == len(r), "line count differs -- REAL difference"
    for a, b in zip(e, r):
        if a == b:
            continue
        na = re.sub(r'-{2,}', '-', re.sub(r' +', ' ', a)).strip()
        nb = re.sub(r'-{2,}', '-', re.sub(r' +', ' ', b)).strip()
        assert na == nb, f"SEMANTIC difference:\n  {a!r}\n  {b!r}"
```

Collapsing runs of spaces and runs of dashes normalizes away psql's column padding
and header rules. During the fork this proved all 90 diffs were padding tracking
shorter identifier names — **zero** semantic differences — and only then was the
expected output regenerated. If it reports even one real difference, you have a
bug, not a formatting change.

## Reading a regression diff from a nix check

The derivation is discarded on failure, so keep it:

```sh
nix build .#checks.x86_64-linux.installcheck-pg17 --keep-failed
find <printed build directory> -name regression.diffs
```

Isolation diffs land under `output_iso/regression.diffs` in the same tree.

## Adding a regression test

- `REGRESS` in the `Makefile` is a list of names, which pg_regress runs
  **serially**. `sql/weave.sql` creates the extension; the others assume it
  exists. A test that must run standalone should create the extension itself.
- Keep EXPLAIN output out of expected files unless the plan *is* the thing under
  test — and then pin it with `SET enable_seqscan` and friends rather than hoping.
- Negative tests are good, and the errors they produce appear in the server log
  during a check run. That is expected output, not a failure.

## Isolation tests

`pg_isolation_regress` hardcodes `<inputdir>/specs/<name>.spec`, so the `Makefile`
has a `specs` target that mirrors `test/isolation/*.spec` into a generated
top-level `specs/` directory. That directory is gitignored — it is generated, not
authored. Edit the files under `test/isolation/`.

## Fuzzing

Targets live in `test/fuzz/` and cover the decoders: the FOR codec, the block
header parser, the docvalid checker. Any new on-disk structure needs a fuzz
target, because the on-disk bytes are **not trusted** — a corrupt page must produce
a clean `ERROR`, never a crash and never a wrong answer.

## Before claiming a gate

`doc/PHASES.md` gates are mechanical. Run the actual command and paste the actual
number. If a performance gate fails, record the number you got in
`bench/RESULTS_*.md` — do not weaken the gate. See `AGENTS.md` rule 8.
