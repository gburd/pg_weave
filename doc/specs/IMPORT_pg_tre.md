# Importing pg_tre's fuzzy/regex/prefix query-compilation subsystem

Status: files imported **and compiling**, channel **not reachable**.  Tasks
Z1/Z2 (pg_weave 0.5.0) vendored TRE, defined the missing GUCs and deadline
helpers, and put every imported file in `OBJS` / `meson.build`.  Nothing in
the access method, the opclasses, or the planner calls any of it yet, and no
SQL-visible function is declared -- that is Z3 onward.  A compiling, linked,
unreachable channel is the intended Z1/Z2 milestone.  See "Wiring TODO"
below for what is done and what remains.

## Source

- Repository: `pg_tre` (Gregory Burd, MIT license).
- Commit imported: `e03d6a833170c9b58b709a8845f30d52685dce69` (2026-08-28),
  short form `e03d6a8` (used in the per-file header banners).
- pg_tre itself vendors two submodules that are **not** touched by this
  import (see "Not imported" below):
  - `vendor/tre` = laurikari/tre @ `d0e0c997336b3210f05b3e1daa7bb5cb9900d274`
    (tag range `v0.8.0-145-gd0e0c99`), 2-clause BSD license (see
    `vendor/tre/LICENSE` in pg_tre).  pg_tre applies
    `patches/tre-progress-hook.patch` on top of this pin to inject two
    weak-symbol hooks (`tre_progress_check`, `tre_compile_progress_check`)
    into `lib/tre-compile.c`, `lib/tre-match-approx.c`,
    `lib/tre-match-backtrack.c`, `lib/tre-match-parallel.c`.
  - `vendor/lime` = the Lime LALR(1) parser generator (a fork/rewrite of
    Lemon), used offline to turn `regex_grammar.y` into
    `regex_grammar.c`/`regex_grammar.h`.  Not vendored here either; if the
    grammar ever needs to change, whoever owns the build must either vendor
    Lime or hand-edit the generated parser (not recommended).

## File mapping

| pg_tre source | pg_weave destination |
|---|---|
| `src/util/surf.c` | `src/query/surf.c` |
| `include/pg_tre/surf.h` | `include/weave/surf.h` |
| `src/query/uleven.c` | `src/query/uleven.c` |
| `include/pg_tre/uleven.h` | `include/weave/uleven.h` |
| `src/query/regex_ast.c` | `src/query/regex_ast.c` |
| `include/pg_tre/regex_ast.h` | `include/weave/regex_ast.h` |
| `src/query/tiling.c` | `src/query/tiling.c` |
| `include/pg_tre/tiling.h` | `include/weave/tiling.h` |
| `src/query/like_translate.c` | `src/query/like_translate.c` |
| `include/pg_tre/like_translate.h` | `include/weave/like_translate.h` |
| `src/util/utf8.c` | `src/util/utf8.c` |
| `include/pg_tre/utf8.h` | `include/weave/utf8.h` |
| `include/pg_tre/popcount.h` | `include/weave/popcount.h` |
| `include/pg_tre/hash.h` | `include/weave/hash.h` |
| `src/util/hash.c` | `src/query/hash.c` (**added in Z1**, see below) |
| `src/util/pattern_cache.c` | `src/query/pattern_cache.c` |
| `include/pg_tre/pattern_cache.h` | `include/weave/pattern_cache.h` |
| `src/query/tre_grammar.y` | `src/query/regex_grammar.y` |
| `src/query/tre_grammar.c` (generated) | `src/query/regex_grammar.c` |
| `src/query/tre_grammar.h` (generated) | `src/query/regex_grammar.h` |
| `src/query/tokens.c` | `src/query/regex_tokens.c` |
| `src/util/tre_match.c` | `src/query/re_match.c` |
| `include/pg_tre/tre_match.h` | `include/weave/re_match.h` |
| `src/query/trgm_similarity.c` | `src/query/trgm_similarity.c` |
| `src/query/extract.c` | `src/query/extract.c` |
| `src/query/parser.c` | `src/query/parser.c` |

**`hash.c` was missing from the original import.** `include/weave/hash.h`
came across but `src/util/hash.c` did not, leaving
`pg_weave_hash_trigram_cp()` declared and undefined.  Nothing noticed,
because no imported file was compiled.  The moment Z1 put them in `OBJS`
the extension still *linked* -- a shared library may carry unresolved
symbols -- and then failed at `CREATE EXTENSION` with

```
could not load library ".../pg_weave.so": undefined symbol: pg_weave_hash_trigram_cp
```

Z1 imported it to `src/query/hash.c` (`src/query/` is where the other two
relocated `src/util/` files went) with the same rename rules.  It is
25 lines of murmurhash32/hash_combine over three codepoints; the hash
values it produces are on-disk-visible once the SuRF/trigram structures
are written in Z3, so it must not be "improved" casually.

`parser.c` and the grammar source were both present in pg_tre and are
imported: `src/query/tre_grammar.y` is the LALR(1) grammar (Lime input,
the true source of truth); `tre_grammar.c`/`tre_grammar.h` are its
committed generated output.  No other `.lemon`/lime-input files exist in
pg_tre besides the one `.y` file.

24 files imported, 7 relocated to a different directory
(`src/util/surf.c` and `src/util/tre_match.c` -> `src/query/`,
`src/util/pattern_cache.c` -> `src/query/`), 3 renamed outright
(`tre_grammar.*` -> `regex_grammar.*`, `tokens.c` -> `regex_tokens.c`,
`tre_match.*` -> `re_match.*`), the rest keep their basename.

## Rename rules applied

Mechanical, in this order, operating on the copied text:

1. Any literal `#include "pg_tre/X.h"` -> `#include "weave/X.h"`, with two
   special cases: `#include "pg_tre/pg_tre.h"` -> `#include "weave/weave.h"`
   and `#include "pg_tre/tre_match.h"` -> `#include "weave/re_match.h"`.
2. `PG_TRE_` -> `WEAVE_` (include guards, `PG_TRE_CP_BITS`/`_MASK`, etc).
3. `TRE_` -> `WEAVE_` for pg_tre's own macros (`TRE_CACHE_SLOTS`,
   `TRE_MAX_PATTERN_LEN`, `TRE_BOUND_LEFT`/`_RIGHT`, `TRE_FUNCS_H`,
   `TRE_CACHE_H`).
4. `pg_tre_` -> `pg_weave_` (all of pg_tre's own C identifiers: GUCs,
   deadline helpers, SQL functions, `surf`/`tile`/`uleven`/`cpstream`/
   `hash`/`like_translate` APIs, the Lime `%name`-generated
   `pg_tre_rx_parse*` family).
5. Bare `pg_tre` (word boundary, e.g. in `pg_tre.compile_timeout_ms` GUC
   names or prose) -> `pg_weave`.
6. `Tre` immediately followed by an uppercase letter -> `Weave` (covers
   both word-initial `TreToken`/`TreParseCtx`/`TreMatchResult`/
   `TreProgressHook`/`TreCacheSlot` *and* mid-identifier `PgTreSurf`/
   `PgTreCpStream` -> `PgWeaveSurf`/`PgWeaveCpStream`; a naive `\bTre`
   regex misses the embedded cases because there is no word boundary
   between `Pg` and `Tre`).
7. Remaining `tre_` prefix -> `weave_` (pg_tre's own lowercase functions:
   `tre_cache_*`, `tre_parse_regex`, `tre_tokenize_next`,
   `tre_compile_pattern`, `tre_free_pattern`, `tre_pattern_num_states`,
   `tre_do_match`, `tre_errmsg`, `tre_set_progress_hook`,
   `tre_set_compile_progress_hook`, and the local variable `tre_err`).

No `weave_re_` fallback prefix was needed: renaming produced zero
collisions with pg_weave's 326 pre-existing `weave_*`/`Weave*`/`WEAVE_*`
symbols (verified programmatically against every pre-existing `.c`/`.h`
file, not just by inspection).

## Exceptions and ambiguities

1. **TRE-upstream API kept verbatim** (EXCEPTION 1). Cross-checked against
   the vendored TRE public header (`vendor/tre/local_includes/tre.h`) and
   internal header (`vendor/tre/lib/tre-internal.h`) in pg_tre, not
   against pg_tre's own headers. Never renamed:
   `tre_regcomp`, `tre_regexec`, `tre_regfree`, `tre_regwcomp`,
   `tre_regwexec`, `tre_regaexec`, `tre_regawexec`, `tre_regncomp`,
   `tre_regnexec`, `tre_reganexec`, `tre_regerror`, `tre_regaparams_default`,
   `regex_t`, `regmatch_t`, `regaparams_t`, `regamatch_t`, `regoff_t`,
   `reg_errcode_t`, and the internal `tre_tnfa_t` (from
   `tre-internal.h`, used in `re_match.c` for `num_states` introspection).
   Also left alone: prose mentions in comments of vendor-internal function
   names `tre_add_tags`, `tre_copy_ast`, `tre_expand_ast` (real symbols in
   `vendor/tre/lib/tre-compile.c`, referenced only descriptively, never
   called from imported code) and the plain word `TRE` used as the
   library's proper name (e.g. "vendored TRE library").  Only in
   `re_match.c`/`re_match.h` and `pattern_cache.c` does any of this
   surface, matching the fact that these are the only two files that
   `#include "tre.h"` / touch `regex_t` and friends.

2. **ABI-crossing weak-symbol hooks kept verbatim** (EXCEPTION 2).
   `tre_progress_check` and `tre_compile_progress_check` are pg_tre's own
   invention (not TRE-upstream) but are injected into the vendored TRE
   sources via `patches/tre-progress-hook.patch` as `__attribute__((weak))`
   externs, called from inside `tre-compile.c` / `tre-match-approx.c` /
   `tre-match-backtrack.c` / `tre-match-parallel.c`.  Renaming them would
   silently break that patch (it references the old names by string, not
   through any header we import).  Comments explaining why were preserved
   and, in `re_match.h`/`re_match.c`, kept adjacent to the two functions
   (see e.g. `include/weave/re_match.h:49-63` and
   `src/query/re_match.c:15-21,49-55`).  Note the *management* API around
   them -- `TreProgressHook`/`tre_set_progress_hook`/
   `tre_set_compile_progress_hook` -- is pg_tre's own plumbing that does
   **not** cross the ABI boundary (only the two weak-symbol functions are
   called from vendored `.c` files), so those *were* renamed to
   `WeaveProgressHook`/`weave_set_progress_hook`/
   `weave_set_compile_progress_hook`.

3. **English prose "Treat" not corrupted** (EXCEPTION 3). Verified every
   occurrence of `[Tt]re` substrings in the 24 files
   (`tree`/`subtree`/`stream`/`streaming`/`cpstream`/`treat(ed/ing)`/
   `textregexeq`) and confirmed the rename regex only fires on `Tre`
   immediately followed by an *uppercase* letter, which none of these
   match. `textregexeq` in particular is a **PostgreSQL builtin function
   name** mentioned in a `like_translate.c` comment, not a pg_tre symbol;
   left untouched.

4. **`PgTreSurf` / `PgTreCpStream`**: the "Tre-followed-by-uppercase" rule
   as literally stated (`\bTre...`) does not fire mid-identifier because
   `g` and `T` are both word characters (no `\b`).  Resolved by dropping
   the leading word-boundary anchor and keeping only the trailing
   uppercase lookahead -> `PgWeaveSurf`, `PgWeaveCpStream`.  No corpus
   collision risk: a capital `T` after `Tre` essentially only occurs in
   deliberate camelCase compounds.

5. **Generated-parser filename drift**: `tre_grammar.c`/`.h` are
   Lime-generated; a blanket `tre_` -> `weave_` pass would have turned
   every *comment* mention of the filenames themselves into
   `weave_grammar.c`/`.h`, which is wrong once the files are physically
   renamed to `regex_grammar.c`/`.h`.  Handled with explicit filename-comment
   fixups (path-qualified and bare forms) applied *before* the identifier
   rename pass, so `parser.c`'s "`Lime-generated parser interface from
   tre_grammar.c`" now correctly reads "`... from regex_grammar.c`", and
   `regex_tokens.c`'s "`Token IDs from tre_grammar.h`" reads "`... from
   regex_grammar.h`". Same treatment for `tre_match.c`/`.h` ->
   `re_match.c`/`.h` and `tokens.c` -> `regex_tokens.c` path/basename
   mentions in top-of-file comments.

6. **Pre-existing stale filename references in pg_tre itself**, corrected
   while renaming rather than faithfully reproduced:
   - `include/pg_tre/tre_match.h` line 2 says `tre_funcs.h` (an old name
     for the same file, never updated after a prior rename in pg_tre) ->
     now correctly says `re_match.h`.
   - `src/util/pattern_cache.c`/`pattern_cache.h` top comments say
     `tre_cache.c`/`tre_cache.h` (ditto, stale) -> now say
     `pattern_cache.c`/`pattern_cache.h`.
   - `include/pg_tre/tre_match.h`'s compile-deadline comment says
     "`tre_compile.c` calls `tre_compile_progress_check()`" but the real
     vendor file is `tre-compile.c` (hyphen, not underscore) -- a
     pre-existing typo in pg_tre.  A blind mechanical rename would have
     turned the underscored typo into `weave_compile.c`, compounding the
     error.  Fixed to read "`vendor/tre's tre-compile.c` calls
     `tre_compile_progress_check()`", correctly naming the *vendor* file
     with its real (unrenamed, hyphenated) name.

7. **`popcount.h` needs no identifier renaming.** It contains zero
   `pg_tre_`/`TRE_`/`Tre` tokens (it's a generic 64-bit popcount fallback,
   ultimately from `github.com/efficient/rankselect`, Apache-2.0, reused
   by pg_tre under an `SPARSEMAP_POPCOUNT_H` guard that was never
   pg_tre-namespaced to begin with).  Only the banner was added; the guard
   name and body are untouched. This file has no comment block above the
   include guard in pg_tre either, so the banner is the file's only
   top-of-file comment now.

## Sparsemap

pg_weave already vendors sparsemap v5.4.0 (`include/weave/sparsemap.h`,
`include/weave/sparsemap_impl.h`, `src/util/sparsemap.c`), which is a
strict superset of pg_tre's vendored v5.1.1 copy
(`include/pg_tre/sparsemap.h`, `src/util/sparsemap.c` in pg_tre). None of
the 24 imported files reference sparsemap at all (`popcount.h` is
pg_tre's *own* copy of a popcount fallback, used independently of
sparsemap's bundled one). pg_tre's sparsemap was **not** imported.

## Vendored TRE (done in Z1)

TRE itself (`laurikari/tre`) is a git submodule in pg_tre at `vendor/tre`,
pinned at Z1 to `d0e0c997336b3210f05b3e1daa7bb5cb9900d274`, under the
2-clause BSD license.  **Z1 copied it in-tree** at that pin, to
`pg_weave/vendor/tre` -- not as a submodule.  Bumped to
`f864ed08a7499865c75b8b59c0cf39a9d59133fe` porting pg_tre `1521662` (see
"Upstream fixes carried in" below and `doc/LICENSING.md`'s TRE section for
the two bugs that made the bump priority 1).  All three prerequisites for
`src/query/re_match.c` are satisfied:

1. Vendored: the eleven `libtre_la_SOURCES` files (with `TRE_APPROX` on)
   plus the `lib/*.h` internal headers and `local_includes/tre.h`, under
   `vendor/tre/LICENSE`.  They are compiled as ordinary translation units
   from `OBJS` (`TRE_OBJS`); there is no autotools sub-build, so the two
   headers `./configure` would have generated are checked in:
   `vendor/tre/local_includes/tre-config.h` and `vendor/tre/config.h`.
2. Patched: `vendor/tre/patches/tre-progress-hook.patch` is checked in
   **already applied**, so `tre_progress_check` and
   `tre_compile_progress_check` exist as weak symbols in
   `lib/tre-compile.c` / `lib/tre-match-{approx,backtrack,parallel}.c`
   and `re_match.c`'s strong definitions win at link time.  Verified by
   installing an always-abort hook and observing both a refused compile
   and `WeaveMatchResult.timed_out`, not just by reading the patch.
3. Include path: `TRE_CPPFLAGS` in the Makefile puts `vendor/tre`,
   `vendor/tre/lib` and `vendor/tre/local_includes` on the command line
   for `re_match.o` and for the vendored TUs **only** -- deliberately not
   globally, so TRE's bare `tre.h` and `config.h` cannot shadow a
   PostgreSQL header anywhere else.

Provenance, the exact pin, the reason for copying instead of
submoduling, and the reason those two symbols keep their `tre_` names are
in `doc/LICENSING.md`.

## Deliberately NOT imported

pg_tre is a full index access method (trigram-based approximate-match
index); only its query-compilation front end (regex parsing, AST,
extraction, tiling, SuRF filter, LIKE lowering, trigram similarity, the
TRE wrapper, and the pattern cache) was imported. The following pg_tre
subsystems were **not** imported and are out of scope for pg_weave, which
has its own segment-based storage engine forked from pg_fts:

- **Index AM** (`src/am/*`, `include/pg_tre/amapi.h`) -- pg_weave has its
  own access method (`src/am/am.c`, `amaux.c`, `amscan.c`,
  `customscan.c`) built around its segment engine, not pg_tre's posting
  layout.
- **Page manager** (`src/pages/*`, `include/pg_tre/page.h`,
  `surf_page.h`, `buffer.h`) -- pg_tre's on-disk page format is specific
  to its own index structure; pg_weave has its own page layer
  (`src/pages/trgm_page.c` and friends) inherited from pg_fts.
- **WAL rmgr** (`src/wal/*`, `include/pg_tre/xlog.h`) -- pg_tre registers
  its own resource manager for index-build/insert WAL records; pg_weave's
  WAL integration is part of its segment engine, unrelated to pg_tre's
  record formats.
- **LSM run catalog** (`include/pg_tre/run_catalog.h`,
  `include/pg_tre/coalesced.h`) -- pg_tre's LSM-style run/compaction
  bookkeeping has no analogue needed here; pg_weave's segment engine
  manages its own run/segment catalog.
- **Pending list** (`include/pg_tre/pending.h`, fast-update machinery) --
  tied to pg_tre's specific index AM insert path.
- Also not imported, and not requested: `bloom.h`/`bloom.c` (pg_tre's
  index-level Bloom filter, distinct from the SuRF filter which *was*
  imported), `upgrade.h`/`upgrade.c`, `free_log.h`, `meta.h`,
  `posting.h`, `upper.h`, `debug.c` (a small AST pretty-printer helper
  used only by pg_tre's own tests), and `trigram.c` (pg_tre's index-side
  trigram extraction entry point, superseded here by pg_weave's own
  `src/query/trgm.c`).

## Wiring TODO

### Upstream fixes carried in, and the half still owed

- **pg_tre `4a9c86c` (SuRF prefilter returned zero rows for case-insensitive
  patterns) — HALF PORTED 2026-09-10.** A `~*` / `ILIKE` scan on an anchored
  prefix could return an EMPTY result set for a pattern with real matches:
  silently, no error, no crash. `always_true` is set for the case-insensitive
  strategies *because* the index stores trigrams case-sensitively, but the
  anchored-prefix key range is derived from raw literal codepoints with no case
  folding, so `name ~* '^GIT'` carried the key for `"GIT"` while the index
  legitimately stored only `"git"`.
  - **Ported:** the extraction guard in `src/query/extract.c` — no key range is
    published once extraction has given up. Latent here (the channel is
    unreachable) and fixed anyway, so the wiring cannot reinherit it.
  - **STILL OWED, a Z7 requirement:** the scan-side prefilter must *also* refuse
    to reject whenever `always_true` is set. Upstream fixed both sides
    deliberately — "a range that exists is a range some future caller may
    consult". Whoever writes the shuttle owes a regression test in the shape of
    pg_tre's `test/sql/surf_prefix.sql`: a case-insensitive anchored pattern must
    return the same rows via the index as via a seq scan.
- **pg_tre `1521662` (vendor TRE `d0e0c997` → `f864ed0`) — PORTED
  2026-09-10.** Upstream's unreleased hardening included a **crash on inputs
  past `INT_MAX`** (`ad26b6d`; position data was `int` throughout in the
  three matcher backends) and a **backref wrong answer** (`2f7dcec`;
  backtracking restarted from the wrong position, so matches at later start
  offsets were missed). Both are demonstrated in `test/hegel/test_tre_bump.c`
  (standalone, linked directly against `vendor/tre` — the fuzzy/regex SQL
  surface is not routed yet, tasks Z3-Z7). The backref fix is shown
  wrong-answer-before/right-answer-after by compiling the *old* pin's
  `tre-match-backtrack.c` alongside the rest of the *new* pin's TRE (the
  bug and its fix are both isolated to that one file) and comparing against
  the same pattern/input compiled with the new file. The `INT_MAX` fix is
  shown by exercising `TRE_MAX_STRING` clamping directly (a real >2GiB
  input is impractical for a test; the clamp constant and the comparison
  that uses it are asserted instead — see the test file for exactly what is
  and is not proven this way). `patches/tre-progress-hook.patch` is pg_tre's
  own rebase onto `f864ed0`, reused verbatim (renamed the usual way) rather
  than re-derived. Full detail in `doc/LICENSING.md`'s TRE section.

### Resolved in Z1/Z2 (pg_weave 0.5.0)

- ~~**GUCs** (referenced, not declared)~~ -- all five are defined in
  **`src/query/fuzzy_guc.c`** and declared in the new
  **`include/weave/regex.h`**, which `include/weave/weave.h` includes so
  the imported files' `#include "weave/weave.h"` resolves unchanged:

  | GUC | C variable | default | range |
  |---|---|---|---|
  | `pg_weave.max_extraction_fanout` | `pg_weave_max_extraction_fanout` | 4096 | 1 .. 65536 |
  | `pg_weave.max_nfa_states` | `pg_weave_max_nfa_states` | 10000 | 32 .. 1000000 |
  | `pg_weave.compile_timeout_ms` | `pg_weave_compile_timeout_ms` | 1000 | 1 .. 600000 |
  | `pg_weave.match_timeout_ms` | `pg_weave_match_timeout_ms` | 1000 | 1 .. 600000 |
  | `pg_weave.similarity_threshold` | `pg_weave_similarity_threshold` | 0.3 | 0.0 .. 1.0 |

  Defaults and ranges are pg_tre's, and `similarity_threshold`'s default
  is pg_trgm's, so a query ported from either behaves the same.
- ~~**Compile-deadline helpers**~~ -- `pg_weave_arm_compile_deadline`,
  `pg_weave_disarm_compile_deadline` and `pg_weave_check_compile_timeout`
  are in `src/query/fuzzy_guc.c`; `pattern_cache.c`'s existing
  `PG_TRY`/`PG_FINALLY` compiles and links against them unmodified.
- ~~**Match-deadline / progress-hook wiring**~~ -- the mirror triad
  `pg_weave_arm_match_deadline` / `pg_weave_disarm_match_deadline` /
  `pg_weave_check_match_timeout(const struct WeaveMatchResult *)` is in
  the same file, installing `weave_set_progress_hook`.  Still **nobody
  calls it**, because nothing calls `weave_do_match()` yet; the channel
  code that does (Z4) must arm around the call and pass the result to
  `pg_weave_check_match_timeout()`.  A timed-out match reported as a
  no-match silently drops rows, which no fixed-output test can catch.
- ~~**`weave/weave.h` additions**~~ -- done via `weave/regex.h`, not by
  growing `weave.h` itself.
- ~~**Vendored TRE**~~ -- see "Vendored TRE (done in Z1)" above.
- ~~**Build wiring**~~ (the *compile* half) -- all fourteen fuzzy TUs
  (`fuzzy_guc`, `surf`, `uleven`, `regex_ast`, `regex_grammar`,
  `regex_tokens`, `parser`, `extract`, `tiling`, `like_translate`,
  `pattern_cache`, `trgm_similarity`, `re_match`, and `src/util/utf8.c`)
  are in the Makefile `OBJS` via `FUZZY_OBJS`, and in `meson.build`.
  They are ordinary translation units. So, since task L1, are `amscan.c`,
  `lev.c` and `trgm_page.c`: the `src/am/am.c` unity build and the
  `make check-unity` target that guarded it are both gone, so there is no
  longer a second kind of source file to be careful about.

### Still open

- **GUC registration is not yet in `_PG_init()`.** The one `_PG_init()`
  pg_weave has lives in `src/am/customscan.c`, which Z1/Z2 could not
  touch.  `pg_weave_init_fuzzy_guc()` is therefore called from a
  module-load constructor in `src/query/fuzzy_guc.c`.  It works, and it
  is idempotent, but it is the wrong mechanism: not the documented
  extension entry point, running before the `PG_MODULE_MAGIC` check, and
  unavailable under MSVC -- so the meson/Windows build registers no fuzzy
  GUCs at all.  The fix is one line in `_PG_init()`:

  ```c
  pg_weave_init_fuzzy_guc();
  ```

  then delete the constructor block.
- **Nothing calls the channel.** No access method, opclass, or planner
  path references `weave_tile_query`, `regex_extract_query`,
  `pg_weave_surf_*`, `weave_cache_lookup*`, `pg_weave_like_to_regex`.
  Because the whole build is LTO'd, the linker dead-strips almost all of
  it out of `pg_weave.so` -- which is the expected observation at this
  milestone, not a build failure.  Z3 onward.
- **No SQL-visible functions.** `trgm_similarity.c`'s
  `pg_weave_trgm_similarity` / `pg_weave_word_similarity` /
  `pg_weave_trgm_sim_op` and friends have their `PG_FUNCTION_INFO_V1`
  records but no `CREATE FUNCTION`; `sql/pg_weave--0.4.0--0.5.0.sql` is
  deliberately DDL-free and says so.
- **Generated-parser toolchain**: `src/query/regex_grammar.c`/`.h` are
  Lime output and are marked `linguist-generated`.  Lime is not vendored
  (decided at Z1, see `doc/LICENSING.md`); changing `regex_grammar.y`
  means building Lime out of tree.  Do not hand-edit the generated
  files.
