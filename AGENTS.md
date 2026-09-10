# AGENTS.md

Orientation for coding agents working on pg_weave. Read this, then
`doc/ARCHITECTURE.md`, then the spec for whatever you are about to touch.

## What this is

One PostgreSQL index access method (`weave`) carrying several retrieval channels
over a shared document-id space: BM25 lexical, quantized-vector ANN, fuzzy,
regex, prefix, and scalar facets. The point of fusing them is that all channels
in a segment share one dense docid, so a bound derived in one channel can skip
work in another — which is how a selective `WHERE` clause makes a vector query
faster instead of collapsing its recall.

**The product, stated so priorities follow from it:** pg_weave is a replacement for
the *combination* of a BM25 index and a vector-similarity index — one all-in-one
search index in place of the pgvector + pg_fts/pg_textsearch stack. Two channels
are the product; the lexical half is real and competitive today and the vector half
cannot yet index a column, which is why phase V is the long pole and phase Z
(fuzzy/regex/prefix) sequences after 1.0. If a task does not move BM25, vector, or
the fusion of the two closer to working, it is not on the critical path.
See `doc/PRODUCTION_READINESS.md` "The route from here".

The thesis is `doc/ARCHITECTURE.md` §3. The novel algorithm is
`doc/specs/FUSED_TOPK.md`. The four claims the project is allowed to make, and
the list of dimensions where it loses, are `doc/ARCHITECTURE.md` §§8–9.

## Reading order for a new agent

1. `doc/ARCHITECTURE.md` — model, vocabulary (warp/weft/shuttle/bolt), provenance
2. `include/weave/channel.h` — the shuttle contract. Contracts (C1)–(C6) are
   **correctness** requirements, not style
3. `doc/PHASES.md` — every task, its spec, and its gate
4. `doc/specs/SEGMENT_FORMAT.md` — what is actually on disk
5. The spec for your subsystem: `FUSED_TOPK.md`, `VECTOR_CHANNEL.md`,
   `FUZZY_CHANNEL.md`
6. `doc/CONVENTIONS.md` — PostgreSQL core C style as this project applies it
7. `doc/TESTING.md` — five layers, and why the property layer is mandatory

## Build and test, verbatim

The nix devShell has a broken PATH entry: the clang-wrapper store path is on
`PATH` without its `/bin` suffix, so a plain `make` inside `nix develop` fails
with `clang: No such file or directory`. The working incantation:

```sh
nix develop --command bash -c \
  'export PATH=$(ls -d /nix/store/*clang-wrapper*/bin | head -1):$PATH; make -s'
```

The reliable full-check path does not need that workaround:

```sh
nix build .#pg17 -L                                   # build
nix build .#checks.x86_64-linux.installcheck-pg17 -L  # regression + isolation
nix build .#checks.x86_64-linux.installcheck-pg18 -L
nix build .#checks.x86_64-linux.tap-pg17 -L           # TAP
```

**To read a regression diff from a failed nix check** you must keep the build
directory, because the derivation is discarded on failure:

```sh
nix build .#checks.x86_64-linux.installcheck-pg17 --keep-failed
# note the printed "keeping build directory" path, then:
find <that path> -name regression.diffs
```

Without `--keep-failed` you get a failure with no diff and no way to see what
changed. This cost real time to discover; do not rediscover it.

Standalone codec tests, no backend needed:

```sh
gcc -O2 -I include -o /scratch/tq test/hegel/test_quantize.c \
    src/vector/quantize.c src/vector/pack.c -lm && /scratch/tq
gcc -O2 -I include -o /scratch/bp bench/bound_pruning.c \
    src/vector/quantize.c src/vector/pack.c -lm && /scratch/bp 1 && /scratch/bp 0
```

## Scratch space, and where parallel worktrees go

**Everything that is not the repository goes in `/scratch`.** Build outputs,
compiled standalone tests, throwaway clusters, corpora, profiles, and git
worktrees. Do not create sibling directories next to the checkout: `~/ws` is the
maintainer's workspace root holding a hundred unrelated projects, and a
`~/ws/wt-something` is indistinguishable from one of them a week later.

When several agents work at once they need **separate worktrees**, because a
shared tree means concurrent `make` runs writing the same object files:

```sh
git worktree add /scratch/pgw-<task> -b wt/<task> main
# ... work, commit in the worktree, do not push ...
git worktree remove /scratch/pgw-<task>
```

`/scratch` is a **different filesystem** from `$HOME`, so `git worktree move`
into it fails with "Invalid cross-device link". Create it in the right place the
first time; relocating means `worktree remove` + `worktree add`, which is only
safe once every change is committed.

Two quirks a fresh worktree has that the main checkout does not, both discovered
the hard way:

- `make` tries to re-run bison on the Lime grammar, because fresh checkout mtimes
  make `src/query/regex_grammar.c` look stale. `touch src/query/regex_grammar.c`.
- `nix build` sees only what git has indexed, so `git add` your new files before
  building or the flake builds without them.

## Lint targets and what each protects against

| target | protects against |
|---|---|
| `make check-rename` | pg_fts-era identifiers surviving the fork. The fork was mechanical; a missed `ftsdoc` compiles fine and confuses everyone afterwards |
| `make check-unity` | someone "fixing" the `src/am/am.c` unity build and getting duplicate symbols |
| `make check-ascii` | a non-ASCII byte in install SQL, which makes `CREATE EXTENSION` **fail** on a LATIN1 or EUC_JP server |
| `make check-alloc` | a `palloc` sized from a corpus- or vocabulary-scale quantity without the huge-safe variant. This is the exact class behind four real crashes in pg_fts 0.3.4 / 1.0.1 / 1.0.2 / 1.0.3 |

## Hard rules

**1. Channel contracts (C1)–(C6) in `include/weave/channel.h` are correctness.**
`block_max()` must be a true upper bound on `score()` for every position in the
block. A bound that is 1 % too low silently drops rows, and **no
fixed-expected-output regression test can catch it** — the answers are still
plausible, just missing. Every channel needs a property test asserting
`bound ≥ score` on random input. A channel without that test is not merged.

**2. 100 % `GenericXLog`.** No raw `XLogInsert`, no `log_newpage`, no
`smgrwrite`, no custom rmgr. pg_tre used a custom rmgr (140) and pg_weave
deliberately does not inherit it: GenericXLog is less efficient and it is the
thing that makes crash-safety auditable and the extension `trusted`.

**3. Never edit `expected/*.out` to make a test pass without first proving the
diff is non-semantic.** The technique, used for real during the fork: read both
files, normalize each line by collapsing runs of spaces and runs of dashes, and
assert the normalized lines are identical. That proved all 90 diffs were psql
column padding tracking shorter identifier names — zero semantic differences —
*before* the expected output was regenerated. If the normalized comparison shows
even one real difference, you have a bug, not a formatting change.

**4. `BM25` is a real algorithm name; do not rename it.** `WeaveSegMeta` is our
struct and gets the weave prefix, but "BM25 ranking", `BM25F`, `bm25+`, and
`BM25L_DELTA` are Robertson & Spärck Jones's, and renaming them is a
documentation bug. `ci/fork-rename.sh` anchors on `\b` and on `BM25<Uppercase>`
for exactly this reason. The first pass got it wrong and produced "Weave ranking"
and `WeaveF`.

**5. `src/am/am.c` is a unity build.** It `#include`s `amscan.c`,
`../query/lev.c`, and `../pages/trgm_page.c`. Do not add those to `OBJS`.
`make check-unity` guards it. Splitting it properly is task **L1**.

**6. Do not copy code from `~/src/zvec`.** Alibaba, Apache-2.0. Ideas only.
Apache-2.0's patent grant and NOTICE requirements are incompatible with a clean
PostgreSQL-licensed release, and contrib-track eligibility is the reason that
matters. See `doc/LICENSING.md`.

**7. Do not start Phase F before the L and V gates pass.** The fused scorer is the
interesting part and the temptation is strong. A fused scorer debugged against a
half-working vector channel costs more time than both, because every wrong answer
has two possible causes and you will chase the wrong one.

*Amended 2026-09-10:* this rule used to read "L, Z, and V". **Z was removed from the
precondition, not from the project.** The product is a replacement for the
combination of a BM25 index and a vector-similarity index, so fuzzy/regex/prefix is
a third channel on a two-channel product and it sequences after 1.0
(`doc/PRODUCTION_READINESS.md`). The rule's *reason* is unchanged and still binding
for V. Nothing about F is channel-count dependent: pivot selection, the
essential/non-essential partition, and F5's property test are all agnostic, so Z
arriving later costs F nothing.

**8. Record losses as prominently as wins.** The house standard is
`pg_turbovec/docs/PARITY_GAPS.md`, which retracted its own headline performance
claim when a contention-controlled benchmark overturned it — and explained that
the original number came from a scalar-fallback bug producing fast-but-wrong
results. Verify correctness *before* recording a latency. A benchmark of a broken
fast path is worse than no benchmark.

**9. Measure the thing the design rests on, before building on it.** See
`bench/RESULTS_BOUND_PRUNING.md`: the vector block bound as originally specified
prunes 0.0 % of blocks and the fix additionally requires a docid ordering
constraint nobody had written down. Both facts were invisible to correctness
tests. That measurement took an afternoon and would otherwise have surfaced
months in.

## Where things are

| subsystem | code | spec |
|---|---|---|
| access method, scan, custom scan | `src/am/` | `doc/specs/SEGMENT_FORMAT.md` |
| lexical channel: parse, analyze, rank, match | `src/query/` | forked; see `doc/ARCHITECTURE.md` §4 |
| FOR codec | `include/weave/for.h` | — |
| page readers/writers | `src/pages/` | `doc/specs/SEGMENT_FORMAT.md` |
| fuzzy/regex/prefix (imported, unwired) | `src/query/{surf,uleven,regex_ast,tiling,like_translate}.c` | `doc/specs/FUZZY_CHANNEL.md`, `doc/specs/IMPORT_pg_tre.md` |
| vector codec (implemented) | `src/vector/{quantize,pack}.c` | `doc/specs/VECTOR_CHANNEL.md` |
| vector storage, kernels, graph (stubs) | `src/vector/{vector,kernels,graph}.c` | `doc/specs/VECTOR_CHANNEL.md` |
| fused scorer (unimplemented) | `src/am/fuse.c` | `doc/specs/FUSED_TOPK.md` |
| channel contract | `include/weave/channel.h` | — |
| property tests | `test/hegel/` | `doc/TESTING.md` |
| fuzz targets | `test/fuzz/` | `doc/TESTING.md` |
| benchmarks and recorded results | `bench/` | `.agent/skills/weave-bench/` |

## How to pick up work

1. Open `doc/PHASES.md`. Find the lowest-numbered task whose prerequisites are
   met and which is not done. Respect the phase ordering rule (§ "Ordering
   principle") — it exists for a reason stated there.
2. Read that task's spec document, in full, before writing code.
3. Write the test that expresses the gate *first*. For a channel, that is the
   bound property test.
4. Implement.
5. Run the gate. If it fails, and the gate is a performance number, record the
   number you actually got in `bench/RESULTS_*.md` rather than deleting the gate.
6. Update `doc/PHASES.md` status and, if you learned something non-obvious, add it
   to the relevant spec next to the thing it constrains — not in a separate
   "notes" file where nobody will see it.

## Skills

`.agent/skills/` has focused guides. Load the one that matches what you are
doing:

- `weave-channel` — implementing a new retrieval channel
- `weave-pg-style` — PostgreSQL core C conventions as applied here
- `weave-index-am` — access-method callbacks, locking, WAL, MVCC, CIC
- `weave-testing` — the five test layers and when each applies
- `weave-bench` — benchmarking credibly, including the EC2 workflow
