# Coding conventions

pg_weave matches **PostgreSQL core C style**, including the parts you may
disagree with, because the project targets contrib and matching core is a
submission requirement rather than a preference.

The operative reference is `.agent/skills/weave-pg-style/SKILL.md`. It is not
duplicated here; read it. This file records only the project-level decisions that
sit above formatting.

## The exemplars

When in doubt, copy these files rather than reasoning from first principles:

| for | read |
|---|---|
| a header with a real design rationale | `include/weave/am.h` |
| a backend-independent, property-testable core | `include/weave/for.h` |
| an interface with a stated correctness contract | `include/weave/channel.h` |
| a scalar reference implementation with determinism requirements | `src/vector/quantize.c` |

## Where explanations go

**Next to the thing they constrain.** Not in a separate notes file, not in a
commit message, not in a wiki.

The reason is concrete. `bench/RESULTS_BOUND_PRUNING.md` established that the
vector channel's block bound only prunes if the docid space is spatially ordered.
That fact lives in three places: the bound's own comment in
`include/weave/quantize.h`, the spec section that derives it
(`doc/specs/VECTOR_CHANNEL.md` §6), and a gated task (`doc/PHASES.md` V13). Someone
reading only the header still finds out. Someone who reads only the spec still
finds out. That redundancy is intentional and worth its cost.

Corollary: a comment that says *what* the code does is usually noise, and a
comment that says *why it is this way and not the obvious way* is usually the most
valuable thing in the file.

## Things that are decisions, not style

1. **Reloption, not GUC, for anything that changes bytes on disk.** A GUC that
   changes the format produces an index whose contents depend on session state.
   Query-time knobs are GUCs; build-time geometry is a reloption, and the *value
   used* is recorded in the segment so a reader never has to guess.
2. **On-disk bytes are not trusted.** Every decoder validates. Every on-disk
   structure gets a fuzz target. A corrupt page produces a clean `ERROR`, never a
   crash and never a wrong answer.
3. **An unknown format version is an `ERROR`.** Best-effort reads of formats we do
   not understand return wrong answers, which is worse than refusing.
4. **No new dependency outside PostgreSQL/BSD/MIT/ISC.** See `doc/LICENSING.md`.
5. **Generated files are checked in with their source and regeneration command,**
   and marked `linguist-generated` in `.gitattributes`.

## Commit messages

Subject line under 72 characters, imperative, naming the subsystem. Body explains
*why* and records any measurement that justified the change. If a commit changes a
performance characteristic, the number goes in the body or in a
`bench/RESULTS_*.md` the body references.

The fork commit (`git log` first entry) is the house example: it records the
namespace mapping, the reason the first rename attempt was wrong, the technique
used to prove 90 expected-output diffs were non-semantic, and the exact test
results on both PostgreSQL majors.
