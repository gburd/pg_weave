# Licensing and provenance

pg_weave is distributed under the **PostgreSQL License** (see `LICENSE`). That
choice is not aesthetic. It is the license that makes contrib-track submission
possible, and contrib-track eligibility is one of the four things pg_weave claims
(`doc/ARCHITECTURE.md` §9). Every provenance decision below follows from wanting
to keep that claim true.

## Why the license is the strategic question

The extension pg_weave most directly competes with, ParadeDB's `pg_search`, is
AGPL-3.0 and embeds Tantivy — a storage engine that lives outside PostgreSQL's
buffer manager, WAL, and MVCC. Both facts cap its ceiling: AGPL is a hard blocker
inside many companies and disqualifying for core inclusion, and a bolt-on engine
cannot be audited for crash-safety the way an index AM using `GenericXLog` can.

pg_weave gives up performance headroom to keep both properties. If a contribution
would compromise either — an Apache-2.0 dependency, a custom rmgr, a vendored
engine with its own storage — the answer is no, even if it is faster.

## Per-subsystem provenance

| subsystem | files | origin | license | how incorporated |
|---|---|---|---|---|
| Segment engine, metapage, tiered merge, WAL, vacuum, MVCC, CIC, custom scan | `src/am/*`, `src/pages/trgm_page.c` | pg_fts 1.5.8 | **PostgreSQL** | forked wholesale |
| Lexical channel: FOR codec, dictionary, block-max WAND, BM25/BM25F, positions, phrase/NEAR | `include/weave/{for,am,weave,docvalid}.h`, `src/query/{parse,doc,rank,analyze,tsanalyze,match}.c` | pg_fts 1.5.8 | **PostgreSQL** | forked wholesale |
| Vocabulary trigram map, bounded Levenshtein automaton | `src/query/{trgm,lev}.c` | pg_fts 1.5.8 | **PostgreSQL** | forked wholesale |
| Sparsemap (succinct bitmap, used for tombstones and trigram postings) | `src/util/sparsemap.c`, `include/weave/sparsemap_impl.h` | vendored in pg_fts; upstream v5.4.0 | **MIT**, © 2024 Gregory Burd | vendored, symbols namespaced |
| SuRF trie, universal-Levenshtein expansion, regex AST, trigram tiling, LIKE translation, pattern cache, UTF-8 helpers | `src/query/{surf,uleven,regex_ast,tiling,like_translate,pattern_cache,regex_grammar,regex_tokens,re_match,trgm_similarity,extract,parser,hash}.c`, `src/util/utf8.c`, `include/weave/{surf,uleven,regex_ast,tiling,like_translate,pattern_cache,re_match,utf8,popcount,hash}.h` | pg_tre 3.2.1, commit `e03d6a83` | **MIT** → relicensed | imported and renamed; in `OBJS` since 0.5.0 (Z1/Z2), channel not yet reachable |
| Approximate/regex matcher behind the fuzzy channel's verification step | `vendor/tre/**` | laurikari/tre, commit `d0e0c997336b3210f05b3e1daa7bb5cb9900d274` (`v0.8.0-145-gd0e0c99`, version string 0.9.0) | **BSD-2**, © 2001–2009 Ville Laurikari | vendored in-tree, one local patch, symbols **not** renamed — see below |
| Vector quantizer: rotation, Lloyd–Max codebook, encode/decode, packing | `include/weave/quantize.h`, `src/vector/{quantize,pack}.c` | turbovec 1.0.0 | **MIT** | **reimplemented in C**, not ported |
| Vamana graph over quantized codes; filter pushed into traversal; runtime ISA dispatch; multi-modal plan operators | `include/weave/graph.h`, `src/vector/graph.c` | pg_turbovec 2.1.0 (Apache-2.0), zvec (Apache-2.0) | — | **ideas only, no code** |
| Fused-threshold top-k | `include/weave/{channel,fuse}.h`, `src/am/fuse.c` | new | PostgreSQL | written for pg_weave |
| Fuzzy-channel GUCs and the TRE compile/match wall-clock deadlines | `include/weave/regex.h`, `src/query/fuzzy_guc.c` | new (replaces pg_tre's unimported `src/module.c`) | PostgreSQL | written for pg_weave |

### pg_fts — no issue

pg_fts is PostgreSQL-licensed and authored by the same person. The fork is a
relayout plus a mechanical symbol rename, reproduced exactly by
`ci/fork-rename.sh`, which is kept in-tree as provenance rather than deleted
after use. A reviewer can re-run it against a fresh `git archive` of pg_fts and
diff the result.

### pg_tre — MIT, same author, relicensed

pg_tre is MIT and authored by the same person, so relicensing to the PostgreSQL
License is the author's to do. Recorded: source commit
`e03d6a833170c9b58b709a8845f30d52685dce69`. The file mapping, the rename rules,
and every ambiguity encountered are in `doc/specs/IMPORT_pg_tre.md`.

Deliberately **not** imported, and the reason matters: pg_tre's index access
method, page manager, LSM run catalog, pending list, and **custom WAL resource
manager (rmgr 140)**. pg_weave has its own segment engine from pg_fts, and it uses
`GenericXLog` exclusively. A custom rmgr is more efficient and it is the thing
that would make crash-safety unauditable and the extension untrusted. That is
rule 2 in `AGENTS.md`.

### turbovec — MIT, reimplemented not ported

`src/vector/quantize.c` and `src/vector/pack.c` are new C written against
`include/weave/quantize.h`. turbovec is used as a specification and as an oracle
for test fixtures. Even where the license would permit a port, a port would be
the wrong artifact: the Rust code carries `rayon`, `rand_chacha`, and `statrs`
dependencies whose internals leak into its wire format, and a PostgreSQL
extension needs `palloc`, memory contexts, and `CHECK_FOR_INTERRUPTS`.

The wire format is **not** compatible with turbovec's, and pg_weave does not
claim it is. In particular the ChaCha8 seed in `src/vector/quantize.c` is
pg_weave's own frozen constant.

### zvec — Apache-2.0, ideas only, do not open with intent to copy

zvec is Alibaba's, Apache-2.0. **No line of it may be copied into pg_weave.**
Two concrete reasons, not squeamishness:

1. **License incompatibility in practice.** Apache-2.0 carries an explicit patent
   grant and a `NOTICE`-file attribution requirement. Combining Apache-2.0 source
   into a PostgreSQL-licensed distribution means the combined work can no longer
   be offered under the PostgreSQL License alone, which forfeits the contrib-track
   claim.
2. **Architectural incompatibility anyway.** zvec is C++17 with Arrow/Parquet,
   RocksDB, ANTLR, and CRoaring dependencies. None of that can live inside a
   PostgreSQL extension regardless of licensing.

The ideas taken are architectural and freely usable: pushing a filter into graph
traversal rather than applying it afterwards; a systematic runtime ISA dispatch
matrix so one binary is optimal on old and new CPUs; and treating vector,
lexical, and scalar recall as interchangeable plan operators under one optimizer.
Ideas are not copyrightable; implementations are. Where an idea is used, the spec
cites zvec so the lineage is visible.

### TRE — BSD-2, vendored in-tree at Z1

The fuzzy channel's final verification step needs a real approximate-regex
matcher. `src/query/re_match.c` (imported from pg_tre) is glue for **TRE**,
`https://github.com/laurikari/tre`. TRE is **2-clause BSD**, compatible with the
PostgreSQL License, and requires only that the copyright notice be preserved:
it is `vendor/tre/LICENSE`, byte-identical to upstream's.

Recorded for Z1:

| what | value |
|---|---|
| upstream | `https://github.com/laurikari/tre` |
| pinned commit | `d0e0c997336b3210f05b3e1daa7bb5cb9900d274` (`git describe`: `v0.8.0-145-gd0e0c99`; `configure.ac` says `AC_INIT([TRE], [0.9.0])`) |
| license | 2-clause BSD, © 2001–2009 Ville Laurikari, verbatim in `vendor/tre/LICENSE` |
| how incorporated | **copied in-tree, not a submodule** |
| local patch | `vendor/tre/patches/tre-progress-hook.patch`, **already applied** to the copied sources |

**Copied, not a submodule, and not built by autotools.** pg_tre carries TRE as a
git submodule and runs `autoreconf && ./configure && make` on it from its own
Makefile. pg_weave does neither. A submodule-free tree is a property this
project already relies on (`nix build` only sees git-tracked files; a source
tarball from `git archive` is complete), and an autotools sub-build inside a
PGXS build is a second toolchain in the critical path. Instead the eleven
`.c` files of `libtre_la_SOURCES` (with `TRE_APPROX` on) are compiled as
ordinary translation units by `OBJS`, and the two headers `./configure` would
have generated are checked in as **generated files with their provenance**
(rule 5 below):

- `vendor/tre/local_includes/tre-config.h` — the feature macros `<tre.h>` needs
- `vendor/tre/config.h` — the rest of what `lib/*.c` consult

Both say at the top that they are pg_weave's, not upstream's, and both explain
each non-obvious setting. Two are worth naming here: `TRE_USE_ALLOCA` is
**off**, because upstream's `alloca()` path sizes one stack allocation from the
compiled NFA and the input length, which in a backend is an unbounded stack
overrun rather than an `ereport`; and `HAVE_GETTEXT` is off, so there is no
libintl link dependency. `vendor/tre/lib/xmalloc.c`, `lib/tre-filter.c`,
`local_includes/regex.h`, and everything under `src/`, `utils/`, `tests/`,
`po/`, `python/`, `win32/`, and `doc/` are **not** copied: they are upstream's
malloc-debugging shim, an unused filter engine, the system-ABI `regex.h`
alias header, and the `agrep` tool / test suite / NLS catalogues. Only the
include paths of the vendored TUs and of `src/query/re_match.c` carry
`vendor/tre`, so a bare `tre.h` or `config.h` can never shadow a PostgreSQL
header elsewhere in the build.

**The local patch, and why two symbols keep their `tre_` names.**
`vendor/tre/patches/tre-progress-hook.patch` injects periodic calls to two
hooks into upstream's compile and match loops — `tre_compile_progress_check()`
in `lib/tre-compile.c` and `tre_progress_check()` in `lib/tre-match-approx.c`,
`lib/tre-match-backtrack.c`, `lib/tre-match-parallel.c` — declared there as
plain externs (`__attribute__((weak))` with a no-op default in the two files
that own them, so upstream still links standalone). The strong definitions are
in `src/query/re_match.c`, which must not include `postgres.h`; they forward to
the wall-clock deadline hooks in `src/query/fuzzy_guc.c`.

These two names are the **only** identifiers in the imported pg_tre code that
were deliberately left unrenamed (EXCEPTION 2 in `doc/specs/IMPORT_pg_tre.md`).
They cross the vendored library's ABI boundary: `vendor/tre` references them by
name, through no header pg_weave controls. Renaming them would still compile
and still link — the weak defaults satisfy the vendored call sites — and would
silently disable both timeouts, which is the failure mode this project cares
about most: no fixed-output test can see a missing timeout.

The patch is checked in *already applied*, so the tree builds with no patch
step. It is kept alongside as provenance: `patch -R -p1 < patches/tre-progress-hook.patch`
inside `vendor/tre` reproduces pristine upstream for the four touched files, and
`git diff` against a fresh checkout of `d0e0c99` then shows nothing. It differs
from pg_tre's copy only in comment text (`pg_tre` → `pg_weave`, and the pointer
to the strong definitions now reads `src/query/re_match.c`); every code line and
both symbol names are identical.

### Lime — not vendored; `regex_grammar.c` is a checked-in generated artifact

pg_tre also vendors `lime`, an LALR(1) parser generator (same author,
`codeberg.org/gregburd/lime`), used to generate `regex_grammar.c`. The generated
file and its `.y` source are both imported. Lime is a **build-time** tool, so it
is needed only to regenerate the parser, not to build pg_weave.

Decided at Z1: **do not vendor it.** `src/query/regex_grammar.c` and `.h` are
treated as checked-in generated artifacts, marked `linguist-generated` in
`.gitattributes`, next to their source `src/query/regex_grammar.y`. To
regenerate, build Lime out of tree and run what pg_tre's Makefile runs:

```sh
lime -q -T<lime>/limpar.c -dsrc/query src/query/regex_grammar.y
```

The cost of the choice, stated plainly: a grammar change is not a one-command
operation for someone who does not already have Lime. That is the right trade
while the grammar is frozen, and it buys a tree with no submodules and no
second code generator in the build.

## Rules for contributions

1. Every file not originally written for pg_weave gets a header banner naming its
   origin, its upstream license, and the commit it came from — and a row in the
   table above. No silent imports.
2. No new dependency under a license other than PostgreSQL, BSD-2, BSD-3, MIT, or
   ISC. Specifically **no Apache-2.0, no GPL, no LGPL**, and no "MIT plus a
   patent clause" variants without review.
3. No vendored storage or execution engine. pg_weave's crash-safety story is that
   every byte goes through the PostgreSQL buffer manager and `GenericXLog`; a
   vendored engine destroys that, and with it the reason to prefer pg_weave over
   the AGPL alternative.
4. Contributions are accepted under the PostgreSQL License. A contributor who
   cannot license their work that way cannot have it merged, no matter how good it
   is.
5. Generated files are checked in **with** their source and regeneration command,
   and marked `linguist-generated` in `.gitattributes`.
