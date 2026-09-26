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

**The product, stated so priorities follow from it:** pg_weave is **a singular text
index** carrying BM25, vector similarity, fuzzy, approximate regex, prefix, and
n-gram retrieval over one docid space. Not "BM25 plus a vector index" -- all six, one
index, one `CREATE INDEX`. That means every one of phases L, Z and V is on the path
to 1.0, and none of them is optional.

Two consequences that are easy to get wrong:

1. **Z8 (`cgram`, the corpus n-gram channel) is required, not opt-in.** It used to be
   described as opt-in and slippable. "n-gram" is now a named product capability, so
   it ships -- and its known cost ships with it: with `cgram` on, pg_weave is **not
   smaller than `pg_trgm`**. That is a stated cost of the product now, not a
   limitation we avoid by defaulting it off.
2. **Do not re-derive the scope from a single sentence in an issue or a commit.** It
   was narrowed to "BM25 + vector" and re-widened to all six within one session on
   2026-09-10, and the plan documents were rewritten both times. If a request seems
   to change the product's scope, ask before rewriting the roadmap.

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
8. `bench/METHODOLOGY.md` — before you write a benchmark: the harness traps that
   make numbers wrong, and the guard against each

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

**Two traps that manufacture false evidence about a check, both hit for real:**

- **`nix build` caches successful checks.** A plain `nix build .#checks...` on an
  unchanged tree returns the cached success without running anything. To actually
  re-run a test -- to probe a flaky one, say -- you need `--rebuild`.
- **`--rebuild` refuses when there is no valid prior output**, exiting 1 with "some
  outputs are not valid, so checking is not possible" *without running the test*, and
  **`nix log <drv>` then hands you the most recent HISTORICAL log for that
  derivation** -- not the run you just attempted. Combining the two reports an old
  failure as a new one, repeatedly. This produced a six-times-counted "reproduction"
  of a bug from a single old run (see G21 in `doc/GAPS.md`).
- **Editing the tree while a `--rebuild` loop is running silences the loop.** Any
  edit -- a comma in a doc -- changes the flake source, so the derivation changes, so
  there is no valid prior output, so every later iteration hits the refusal above and
  *runs nothing* while finishing in seconds. Hit for real on 2026-09-15: 19 of 20
  legs in a G21 hunt tested nothing, and the only reason it was noticed is that the
  loop asserted the test's own marker rather than its exit status. Either leave the
  tree alone for the duration, or have the loop re-seed with a plain `nix build` when
  it sees the refusal.

So: **a test result needs evidence the test RAN**, not just an exit status -- grep the
output for the test's own markers. And capture the status of the build itself, never
through a pipe:

```sh
nix build ... 2>&1 | tail -5; echo "$?"   # WRONG: that is tail's status, always 0
nix build ... >/dev/null 2>&1; echo "$?"  # right
```

Three verification errors in two days came from this family. A gate that reports a
state it never checked is the failure mode `make check-alloc` was widened for.

**And one level down: the suite running is not the SITE running.** On 2026-09-16 a
mutation that reintroduced the pre-V7 `values[0]` bug in the scan-side recheck passed
the whole regression suite twice. First because `weave_recheck_exact()` is only called
for query shapes the posting lists over-generate, so the test's plain two-term AND
never reached it; then, once the test used a phrase, because **the planner answered it
with a bitmap heap scan whose executor recheck re-evaluates `@@@` itself** -- the right
answer by a path that does not touch the mutated code. Only `weave_count()` and
`weave_search()`, which enter the scan machinery directly, exercise it. Writing SQL
that reaches a specific C function is not the same as writing SQL that returns the
right answer, and a mutation run is the only thing that tells the two apart.

**Sixth member, and it was in the mutation harness itself: a BUILD error dressed as a
test result.** A V7 mutation leg substituted a call to a function that does not exist,
so the derivation failed to compile and the harness -- which treats "the check did not
succeed" as "the mutation was caught" -- reported a pass. The mutation was never run at
all, and the compiling equivalent turned out to **survive**. So a mutation harness needs
the same discipline as any other gate: assert that the mutant BUILT, not merely that the
check failed. The generalization that now covers all nine: **a result needs evidence that
the specific thing you meant to run, ran.**

*And the bug that leg was hiding is worth knowing on its own,* because no amount of
regression SQL would have found it: the vector weft's docid ordering looked untested
because a sequential scan visits pages in ascending order, so the build callback's
order already IS docid order and sorting is the identity. It stops being the identity
only when a **synchronized scan starts mid-relation**, which needs a relation larger
than `NBuffers/4` -- unreachable from `installcheck`, where `shared_buffers` cannot be
set. It takes a TAP test with its own cluster (`t/017_vector_syncscan.pl`). A guard whose
input is identical to its output under every condition your harness can produce is not
tested by that harness.

**`flake.nix`'s `PROVE_TESTS` is an explicit list, and a TAP file that is not named in
it runs nowhere while the suite reports success.** Both new V7 TAP files hit this. Add
the file to the list in the same commit that adds the file.

**Eighth member: a pipe that KILLS the process under test.** `psql -f t.sql | head -90`
sends SIGPIPE to psql at line 90, so the file never reaches its own `DROP TABLE` --
and the next run silently inherited the table and reported 134 lanes where 67 was
correct (2026-09-19, G23). Redirect the output of anything being measured to a file
and read the file; never truncate its stdout.

**Ninth member: a stale artifact makes a failed build look like a pass.**
`make 2>&1 | grep error; test -f pg_weave.so && echo OK` printed OK while the build
had ERRORED -- the `.so` was left over from the previous build. Same day, same
session, and it is the first member wearing a different hat: `| tail`, `| head` and
`test -f` all report on something other than the build. Take `make`'s own exit status,
and `make clean` first.

**An access method may only return HOT-CHAIN ROOT TIDs, and returning a physical one fails
SILENTLY.** Z8's fallback heap pass returned the TID it had just read; a HOT-updated row's
physical TID resolves to no visible tuple, so the plan read `Bitmap Index Scan rows=2` above
`Bitmap Heap Scan rows=0` with no error anywhere. `heap_get_root_tuples()` per page is the fix.
Nothing about this is specific to that channel: any AM path that manufactures TIDs outside the
build callback has the same hazard, and the symptom is a row count, not a message.

**TENTH MEMBER, and it presents as a backend CRASH in a test that has nothing to do with
your change.** On the 2026-09-21 dev image, PGXS's bitcode step runs `llvm-lto` from
PostgreSQL 17's LLVM 19 over bitcode the image's **clang 21** produced, and it aborts:
`sudo make install` exits 134 *after* having written some of
`/usr/lib/postgresql/17/lib/bitcode/pg_weave/`. The `.so` is therefore never replaced, and
PG17's JIT now finds mismatched bitcode to inline and **kills the backend** —
`error: Unknown attribute kind (102) (Producer: 'LLVM21.1.8' Reader: 'LLVM 19.1.1')` in the
server log, `server closed the connection unexpectedly` in the client, and the postmaster
survives so later tests still run and still pass. The crash appeared in `sql/weave.sql` on
the one query complex enough to exceed `jit_above_cost`, three hours after the install that
caused it.

Three things follow:

- **Always `with_llvm=no` on this image.** `/scratch/pg_weave/gates.sh` already does, with
  a comment saying why; a *manual* `sudo make install` without it is what poisoned the
  host, and `with_llvm=no` on a later install does not remove what the earlier one left.
- **`SET jit = off` is the one-line triage** that separates "my change broke this" from
  "the toolchain did". It is not the fix and it is not proof of innocence — it only shows
  JIT is necessary for the crash.
- **`rm -rf $libdir/bitcode/pg_weave*`** is the repair. Check for it before believing any
  crash on a host where a manual install has ever run.

The generalization the other nine already make, arriving by a new route: the failure was
reported by a test that was not near the change, on the major that was not first, hours
after the cause. An install whose exit status is not checked is a landmine with a delay
fuse.

**ELEVENTH MEMBER, and it is in the gate that fronts every property test in the
project.** `make check-standalone` invoked sixteen of its eighteen suites as
`$tmp/pk | tail -1`. A pipeline's exit status is the LAST command's, so `set -e` saw
`tail` succeed no matter what the test did — and on 2026-09-22 `test_pagekind` was found
to have been **aborting on an assertion** while the target printed
`== ALL STANDALONE CHECKS PASSED ==` and exited 0, with the abort message visible three
lines above the banner. Two suites in the same recipe (`test_kernels`, `test_lexbound`)
had always redirected to a log and checked their own status; all eighteen do now.

What it hid is instructive on its own: the assertion that fired is the one designed to
notice a page kind added to the enum and not to the test's list, and **five had been**
(`WEAVE_PK_PENDING_V9`, the three `WEAVE_PK_CGRAM_*` kinds, `WEAVE_PK_PENDING_V10`). The
guard worked; the harness silenced it. `doc/GAPS.md` **G42**.

Two rules follow, and the second is the one that generalizes:

- **A gate fix needs a positive control, not a passing gate.** "The suite passes now" is
  compatible with the gate still being unable to fail. The stale `test_pagekind` was
  recompiled from `git show HEAD:` and run through the new pattern to show it exits 1.
- **Every "N million checks, 0 failures" figure this project publishes came through that
  target.** Those are not retracted — a suite that ran and printed its own total is still
  evidence — but a suite that *aborted* would have been reported identically, and until
  now nothing distinguished the two. When a harness reports on something other than the
  thing under test, the damage is not the bugs it let through; it is that every green it
  ever printed becomes uninformative in retrospect.

**TWELFTH MEMBER, one day after the eleventh, and it is the same shape wearing a
GUC.** A diagnostic GUC added specifically to investigate `doc/GAPS.md` G43 —
`pg_weave.fuse_check_bounds`, whose own comment says "AVAILABLE IN A RELEASE BUILD, and
it is diagnostics rather than a test hook" — was placed **inside the
`#ifdef WEAVE_TEST_HOOKS` block**, which nothing in the Makefile, meson or flake defines.
So the GUC did not exist in any build anyone runs. `SET pg_weave.fuse_check_bounds = on`
was then accepted as a **placeholder** custom GUC, `SHOW` echoed back `on`, the check
never executed, and "the check ran and did not fire" was recorded in the gap entry and in
project memory as evidence that a bound was sound. It had examined nothing.

- **An absent GUC is indistinguishable from a GUC that is OFF.** `SHOW` cannot tell you;
  it happily reports placeholder values. `pg_settings` can — a placeholder has no
  `short_desc` — but **only in a session where the library is actually LOADED**, and a
  fresh `psql -c "select ... from pg_settings"` loads nothing, so the view is empty and
  answers a question you did not ask. Touch an index first, then look.
- **A diagnostic needs a positive control exactly as much as a gate does.** Until it has
  fired once, its silence is not evidence of anything. The eleventh member said this
  about a test target; it is equally true of an assertion, a GUC, a counter and a
  `WARNING`. When the abandonment audit built for G43 also stayed silent, that silence
  was correctly treated as uninformative — and the instrument that did find the bug was
  a per-channel `elog` proven to fire before it was trusted.

*And the diagnostic sequence that eventually worked is worth copying,* because three
plausible hypotheses each explained the symptom and all three were wrong. What
discriminated was never analysis; it was **the experiment that REMOVES a component
rather than the one that explains the behaviour**. Weighting the suspected channel down
to `1e-6` and finding the wrong answer bit-for-bit unchanged took two minutes and
exonerated it completely. Reach for the ablation first.

**A gate that reports FAIL and nothing else costs a round trip**, which on a remote
build host is minutes. Print the compiler's own error lines on a build failure and the
install log's tail on an install failure. Two round trips were burned on
"FAIL pg17: build" before the gate script was taught to say why.

**On a host with both majors packaged, DERIVE the port; never assume it.** PG18 took
5432 and PG17 got 5433 on the 2026-09-19 EC2 host, and a run that assumed the opposite
sent PG17's `installcheck` at the PG18 server: all nine regression tests red with an
EMPTY `regression.diffs`. Nine failures that were one wrong port. Read the port from
`pg_lsclusters` and then assert `show server_version_num` matches the major just built
for. (Also: installing `postgresql-17` and `postgresql-18` in one apt transaction
created only the 18 cluster.)

**An expected file generated with `REGRESS=<one-file>` does not match the full suite.**
`CREATE EXTENSION IF NOT EXISTS pg_weave` emits `extension "pg_weave" already exists,
skipping` in the full run -- an earlier file created it -- and nothing in a solo run.
Both new regression files this week (`pendingvec`, `chanstats`) were generated solo and
failed their first full-suite run on exactly that line. Generate the expected output
from a FULL `make installcheck`, not from `REGRESS=<name>`.

**PostgreSQL 18 turns data checksums ON by default and 17 does not.** Any TAP test
that rewrites page bytes behind the server's back -- the manufacture-the-old-image
pattern in `t/010` and `t/019` -- passes on 17 and dies on 18 with "invalid page in
block N" unless it inits with `no_data_checksums => 1`. Upstream added that option for
exactly this class of test; PG17's `init()` ignores the unknown key.

Standalone codec tests, no backend needed:

```sh
gcc -O2 -I include -o /scratch/tq test/hegel/test_quantize.c \
    src/vector/quantize.c src/vector/pack.c -lm && /scratch/tq
gcc -O2 -I include -o /scratch/bp bench/bound_pruning.c \
    src/vector/quantize.c src/vector/pack.c -lm && /scratch/bp 1 && /scratch/bp 0
```

## Scratch space, and where parallel worktrees go

**Everything that is not the repository goes under `/scratch/pg_weave/`.** Build
outputs, compiled standalone tests, throwaway clusters, corpora, profiles, harness
scripts, per-host env files, and git worktrees. Two placements are both wrong and
both were corrected on 2026-09-20: **not** a sibling directory next to the checkout
(`~/ws` is the maintainer's workspace root holding a hundred unrelated projects, so
`~/ws/wt-something` is indistinguishable from one of them a week later), and **not**
`/scratch` top level either (`/scratch` is shared with every other project, and this
project's debris reached ~17 GB before anyone noticed). Be tidy: delete what is no
longer needed, in the session that stops needing it.

When several agents work at once they need **separate worktrees**, because a
shared tree means concurrent `make` runs writing the same object files:

```sh
git worktree add /scratch/pg_weave/<task> -b wt/<task> main
# ... work, commit in the worktree, do not push ...
git worktree remove /scratch/pg_weave/<task>
```

`/scratch` is a **different filesystem** from `$HOME`, so `git worktree move`
into it fails with "Invalid cross-device link" — but a move *within* `/scratch`
works, which is how the 2026-09-20 relocation was done. Create it in the right
place the first time; a cross-device relocation means `worktree remove` +
`worktree add`, which is only safe once every change is committed.

The bash harness refuses `rm -rf <dir>`. Use `find <dir> -depth -delete`.

Two quirks a fresh worktree has that the main checkout does not, both discovered
the hard way:

- ~~`make` tries to re-run bison on the Lime grammar~~ **Fixed.** The Makefile now
  cancels PGXS's implicit `%.c: %.y` rule for `src/query/regex_grammar.c`, so
  `touch src/query/regex_grammar.c` is no longer needed. The same nanosecond-level
  mtime race is what made CI's `sanitize` leg fail while the other three legs
  passed on the identical commit; see the comment in `Makefile`.
- `nix build` sees only what git has indexed, so `git add` your new files before
  building or the flake builds without them.

## Lint targets and what each protects against

| target | protects against |
|---|---|
| `make check-rename` | pg_fts-era identifiers surviving the fork. The fork was mechanical; a missed `ftsdoc` compiles fine and confuses everyone afterwards |
| `make check-ascii` | a non-ASCII byte in install SQL, which makes `CREATE EXTENSION` **fail** on a LATIN1 or EUC_JP server |
| `make check-alloc` | a `palloc` sized from a corpus- or vocabulary-scale quantity without the huge-safe variant. This is the exact class behind four real crashes in pg_fts 0.3.4 / 1.0.1 / 1.0.2 / 1.0.3. **Widened 2026-09-14, and the reason it needed widening is the more useful lesson:** it matched an enumerated allowlist of size-variable *names*, which grew one name at a time, so the codebase's dominant allocation pattern — a doubling `cap` variable — was invisible to it. 43 such sites existed; eight were genuinely corpus-scale and four were reachable from VACUUM/merge, where a throw makes the index permanently unvacuumable. It now matches the **idiom** (any identifier containing `cap` in a size expression) and deliberately over-matches: a bounded one costs one `alloc-ok:` annotation, a missed one costs an unvacuumable index. A lint that reports safety it has not checked is worse than no lint |
| `make check-pdlower` | reading `pd_lower` anywhere but `weave_page_entry_end()` in `include/weave/am.h`. That value comes off disk on a page held under a share lock, so forming `page + pd_lower` is UB before any dereference. **Added 2026-09-16 after the upstream review**: pg_fts 1.7.0 fixed one such site (an impossible merge allocation that left the index permanently unvacuumable), 1.7.1 found eight siblings and concluded "I should have grepped the siblings then". This repo had the helper *first* and still had four stragglers — one of them a genuine out-of-bounds **read**: `src/pages/trgm_page.c` computed `avail = pd_lower - contents_offset` as an unsigned `Size`, so a torn page underflowed it to a huge value, and the `Min()` against the remaining blob length then clamped it to a length spanning the whole page *chain*. It could not overrun the destination buffer, which is exactly why it read safe. The lint is the grep, made permanent |
| ~~`make check-unity`~~ | **DELETED by task L1.** It guarded the `src/am/am.c` unity build against someone adding the three `#include`d `.c` files to `OBJS` and getting duplicate symbols. L1 split the file, so all three are ordinary translation units in `OBJS` and the failure mode the target existed to catch is structurally gone -- there is no `#include` of a `.c` file left to conflict with. Keeping the target would have required inverting it, and an inverted guard asserts the absence of a mistake nobody is positioned to make |

**AN OPTION LIST IS A CACHE, AND IT GOES STALE — 2026-09-23, and it cost a maintainer
decision.** `doc/specs/FUSED_TOPK.md` §8d listed three structural options for the vector
work row, one of which (cluster-order the weft) `doc/PHASES.md` **V13** had recorded as
*already refuted* since 2026-09-13 — same experiment, 0.00 % on real corpora, natural order
measuring the same 0.00 %. The option list was never updated, the decision was taken from
the option list, and an hour of measurement was spent re-deriving a conclusion the tree
already held. Two rules follow, and the second is the one that generalizes past option
lists:

- **Before presenting options for a decision, grep each option for its own status row.**
  A task id (V13 here) is where a refutation lands; a prose option list in a spec is where
  it does not.
- **When two documents in this tree disagree about whether something is settled, the one
  with the task id and the gate is the newer claim** — status is maintained per task, and
  narrative sections are written once and re-read forever.

*The same review also caught a recommendation of mine that had not been measured*: "page
traffic would follow the blocks-scored curve" ignored two structures that are read in full
on every scan (the block directory and the warp map), so the real win is 1.8–4.3× rather
than 32×, and the on-disk index it proposed is probably unnecessary because the chain
measures 98.3 % dense. A recommendation is a claim, and hard rule 9 applies to it as much
as to a design.

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

**5. ~~`src/am/am.c` is a unity build.~~ RETIRED by task L1 (done).** It used to
`#include` `amscan.c`, `../query/lev.c` and `../pages/trgm_page.c`, so those three
had to be kept out of `OBJS` or their symbols were compiled and linked twice;
`make check-unity` guarded both halves of that and has been **deleted** with the
target.

The AM is now `src/am/am.c` (AM core, page/segment/metapage machinery),
`src/am/ambuild.c` (build, insert, segment writers, merge), `src/am/amvacuum.c`
(bulkdelete, cleanup, compaction, maintenance SQL) and `src/am/amscan.c` (scan),
with `src/query/lev.c` and `src/pages/trgm_page.c` as ordinary translation units.
The interface between them is `include/weave/am.h`, which says of every
declaration why it is there. **Add new AM code to the file that owns the seam**,
and if a symbol has to become non-`static` to be reached, declare it there with a
reason rather than putting an `extern` at the top of a `.c` file.

*Why the rule is gone rather than reworded:* the hazard it named was a textual
`#include` of a `.c` file colliding with a compiled object of the same file.
There is no such `#include` left, so there is nothing to guard. The rule that
replaced it is the one above about where code goes.

**6. Do not copy code from `~/src/zvec`.** Alibaba, Apache-2.0. Ideas only.
Apache-2.0's patent grant and NOTICE requirements are incompatible with a clean
PostgreSQL-licensed release, and contrib-track eligibility is the reason that
matters. See `doc/LICENSING.md`.

**7. Do not start Phase F before the L, Z, and V gates pass.** **SCOPED WAIVER IN
FORCE since 2026-09-20, WIDENED 2026-09-21 — read `doc/PHASES.md` "Phase F" before acting
on this rule.** F1 and F5 are done; **F2 and F3 are now licensed too**, because the L gate
was restated in absolute terms and its GIN half passes, which discharges the "a gate that
cannot be evaluated" objection F2 was held on. **F4 is withdrawn.** Z and V still do not
pass; the waiver states which, why bounded top-k is nonetheless the one lever every
unmet Z/V latency gate needs, and what the mitigation for rule 7's actual hazard is.
The rule below is the reasoning the waiver had to answer, so it stays as written.

The fused scorer is
the interesting part and the temptation is strong. A fused scorer debugged against a
half-working channel costs more time than all of them, because every wrong answer
has several possible causes and you will chase the wrong one.

*History, because the amendment and its reversal are both instructive:* on
2026-09-10 this was amended to "L and V" when the product was briefly understood as
BM25 + vector, and **reverted the same day** when the scope was restated as all six
retrieval kinds. The rule is back to its original form. The lesson is not about the
rule, it is about the process: a hard rule was weakened on an inference about scope
rather than a question about scope.

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

**10. Re-run an arm against itself before believing a difference between arms.**
Adopted 2026-09-18 from the sibling project's review, where an 8.5 % "win" turned
out to be a single baseline outlier. This rule indicts our own
`bench/RESULTS_G20_MERGE_GATE.md`, which is **one run per arm**;
`bench/RESULTS_G20_SNAPSHOT_ALLOC.md` is two and is the new house minimum. A
between-arm delta smaller than the within-arm spread is not a result, and the
within-arm spread is unknown until you measure it twice.

**11. A number is provisional until it reproduces at a second scale**, and a
projected ratio is not a measurement — report what was measured, or write
"unmeasured". Corollary, learned the hard way twice in one week: **a harness can
make a number up.** `t/007`'s four "concurrent" inserters were serial, so every
segment-count figure this project published came through a test that was not
testing concurrency (G28); and `bench/code_scan.c`'s stage 2 scored a whole
32-lane block per survivor, which moved the V15 verdict three times. Before
quoting a figure, ask what the harness would have to be doing for it to be wrong,
and check that.

**12. When a release touches tombstones, merge or vacuum, "local green" is not
evidence.** The sibling project shipped the same P0 through a green local gate
twice. Those paths need a run at scale — which is what `bench/aws/` and the
`weave-bench` skill exist for — before the work counts as done.

**13. Retractions get a named home, not a quiet edit.** Hard rule 8 says record
losses as prominently as wins; this says *where*. A claim this project published
and has since disproved gets a marked **RETRACTED** or **SUPERSEDED** note left in
place at the point of the original claim, with the date and the mechanism —
`bench/RESULTS_CODE_SCAN.md`'s threading note and `VECTOR_CHANNEL.md` §8a are the
worked examples. Deleting a wrong claim destroys the evidence that the process
works.

**14. No AWS account id, VPC id, security-group id, AMI id or key name in the
repository.** The harness takes a profile name and derives everything else; the
sibling project needed `git-filter-repo` to undo the alternative. Related, and it
has already paid for itself elsewhere: **pull benchmark artefacts incrementally,
never in one final scp** — a burner expiring mid-run cost them an instance and
zero data, because the data was already on disk.

**15. A swept-parameter boundary is not a capability wall.** Adopted 2026-09-26
from the sibling project's IVF review: a sweep capped `probes` at 128 of 1024 and
reported the arm as "unreachable at R@10 ≥ 0.98"; extending the grid to 256/512
reached 0.987 then 1.000 — it was grid-limited, never a wall. Before writing that a
channel "cannot" reach a target, either extend the grid until the curve flattens or
label the number **grid-limited** with the cap that produced it. This indicts our
own owed docvals-prize probe (`bench/RESULTS_DOCVALS_PRIZE.md`, G49 note): the 2k-row
`vec_scores`-rising observation is a LIMIT-driven widening confound, not evidence of
a ceiling, and must not be reported as one.

**16. A determinism / bit-identity guard must vary the axis the algorithm actually
depends on, not a proxy.** Adopted 2026-09-26: the sibling project's
`rotate_corpus_bit_identical_across_pool_sizes` varied the *thread count* at a fixed
shape and passed, so a "mathematically equivalent" blocked-rotation optimization
looked safe — but the underlying GEMM's tiling (and thus its float reduction order)
depends on the *row count*, so blocking would have silently changed on-disk bytes.
Only a guard varying the shape (`m=1` vs `m=300`) caught it, and only because it was
written **before** the optimization was trusted. For pg_weave this sharpens hard
rule 1 and every merge/build/repack "same bytes" claim: identify what the output
byte-depends on (segment shape, docid order, chunk boundaries, tail padding — not
just worker count) and make the guard sweep *that* axis, first.

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

See .agent-steering-domains.md for domain-specific steering (local).
