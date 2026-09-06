---
name: weave-pg-style
description: Use when writing or reviewing any C code in pg_weave — file banners, naming, indentation, error reporting, memory contexts, and the huge-allocation rule. Load this before writing C, and when a reviewer flags style. Matches PostgreSQL core conventions, including the parts you disagree with.
---

# PostgreSQL core C conventions, as pg_weave applies them

pg_weave targets contrib. Matching core's style is therefore a submission
requirement, not a preference. Read `include/weave/am.h` and
`include/weave/for.h` and match them.

## File banner

Every `.c` and `.h` file:

```c
/*-------------------------------------------------------------------------
 *
 * filename.c
 *		One line saying what this file is for.
 *
 * Longer prose explaining the design, the invariants, and anything a reader
 * would otherwise have to reverse-engineer.  This is where the reasons go.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/subsys/filename.c
 *
 *-------------------------------------------------------------------------
 */
```

Imported files additionally name their origin, upstream license, and commit — see
`doc/LICENSING.md` rule 1.

## Formatting

- **Tabs**, width 4. Not spaces. `.clang-format` matches core's `pgindent`.
- Comments wrap at 78 columns.
- Function definitions put the return type on its own line:
  ```c
  static WeaveWarp
  weave_something(WeaveShuttle *s)
  ```
- Braces on their own lines, always.
- Declarations at the top of a block. `-Wdeclaration-after-statement` is on and
  the build is warning-clean; keep it that way. The one exception is the vendored
  sparsemap, which has an explicit per-object override in the `Makefile`.

## Naming

| kind | convention | example |
|---|---|---|
| typedef | `Weave` + CamelCase | `WeaveSegMeta`, `WeaveShuttleOps` |
| macro / constant | `WEAVE_` + SHOUTY | `WEAVE_MAX_SEGMENTS`, `WEAVE_VCODES` |
| function | `weave_` + snake_case | `weave_flush_pending()` |
| SQL-callable | `weave_` or the type name | `weave_bm25f()`, `wvec_dims()` |
| struct field | snake_case, terse | `dictstart`, `sumdoclen` |

`BM25` stays `BM25` wherever it names the scoring function rather than one of our
structs. See `AGENTS.md` rule 4.

## Error reporting

```c
ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("bits must be between 2 and 4"),
         errhint("Use 4 for best recall, 2 for smallest index.")));
```

The message style guide, which reviewers do enforce:

- `errmsg` starts lowercase, has **no trailing period**, and states a fact rather
  than issuing a command.
- `errhint` and `errdetail` are complete sentences **with** periods.
- Always pick a real `errcode`. `elog(ERROR, ...)` is for can't-happen internal
  conditions only — it produces `XX000`, which no client can handle.
- Do not interpolate an identifier without `%s`, and do not concatenate
  translatable strings.

## Memory

- `palloc` / `pfree`, never `malloc` / `free`, in backend code. The exception is
  the backend-independent cores (`include/weave/{for,quantize}.h`), which take
  allocator function pointers precisely so the standalone tests can pass `malloc`
  — see the design-intent note at the top of `for.h`.
- Long-lived state goes in an explicitly created `MemoryContext` with a name that
  reads usefully in a memory-context dump.
- Anything allocated inside a loop over documents or terms belongs in a
  per-iteration context that is reset, not a `pfree` per allocation.
- `PG_TRY` / `PG_CATCH` only to release a non-memory resource (a buffer pin, a
  lock, a file). Never to continue after an error.

## The huge-allocation rule

`make check-alloc` fails the build if a `palloc` or `repalloc` is sized from a
corpus- or vocabulary-scale quantity without the huge-safe variant.

This is not theoretical. It is the exact class of bug behind four separate crashes
in pg_fts — 0.3.4, 1.0.1, 1.0.2, and 1.0.3. The pattern: an allocation sized
`nterms * sizeof(x)` is fine on the test corpus, exceeds `MaxAllocSize` (1 GB) on
a real one, and `palloc` then throws — sometimes inside a `GenericXLog` window
where throwing is not safe.

If you are sizing an allocation from anything that scales with the data, use the
huge-safe wrapper and ask whether the algorithm should be streaming instead.

## Interrupts

Any loop that can run longer than a few milliseconds needs
`CHECK_FOR_INTERRUPTS()`. A build over a large corpus that cannot be cancelled is
a production incident.

## What not to do

- No `//` comments.
- No VLAs. `-Werror=vla` is on.
- No `assert()`. Use `Assert()`, which compiles out in a non-assert build.
- No new GUC that changes bytes on disk — that is a reloption. A GUC that changes
  the format means an index whose contents depend on session state.
