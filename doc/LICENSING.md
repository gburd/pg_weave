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
| SuRF trie, universal-Levenshtein expansion, regex AST, trigram tiling, LIKE translation, pattern cache, UTF-8 helpers | `src/query/{surf,uleven,regex_ast,tiling,like_translate,pattern_cache,regex_grammar,regex_tokens,re_match,trgm_similarity,extract,parser}.c`, `include/weave/{surf,uleven,regex_ast,tiling,like_translate,pattern_cache,re_match,utf8,popcount,hash}.h` | pg_tre 3.2.1, commit `e03d6a83` | **MIT** → relicensed | imported and renamed; **not yet wired into the build** |
| Vector quantizer: rotation, Lloyd–Max codebook, encode/decode, packing | `include/weave/quantize.h`, `src/vector/{quantize,pack}.c` | turbovec 1.0.0 | **MIT** | **reimplemented in C**, not ported |
| Vamana graph over quantized codes; filter pushed into traversal; runtime ISA dispatch; multi-modal plan operators | `include/weave/graph.h`, `src/vector/graph.c` | pg_turbovec 2.1.0 (Apache-2.0), zvec (Apache-2.0) | — | **ideas only, no code** |
| Fused-threshold top-k | `include/weave/{channel,fuse}.h`, `src/am/fuse.c` | new | PostgreSQL | written for pg_weave |

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

## Third-party code to be vendored

### TRE (task Z1, not yet present)

The fuzzy channel's final verification step needs a real regex matcher.
`src/query/re_match.c` (imported from pg_tre) is glue for **TRE**,
`https://github.com/laurikari/tre`, which pg_tre vendors as a git submodule
pinned at `d0e0c99…`. TRE is **2-clause BSD**, which is compatible with the
PostgreSQL License and requires only that the copyright notice be preserved.

When Z1 lands, this section must record: the pinned commit, the BSD-2 text
included verbatim under `vendor/tre/`, and the local patch adding the
progress/deadline hooks (`tre_progress_check`, `tre_compile_progress_check` —
kept unrenamed precisely because they cross the vendored library's ABI boundary).

### Lime (probably not needed)

pg_tre also vendors `lime`, an LALR parser generator (same author,
`codeberg.org/gregburd/lime`), used to generate `regex_grammar.c`. The generated
file and its `.y` source are imported. Lime is a **build-time** tool, so it is
needed only to regenerate the parser, not to build pg_weave. Decide at Z1 whether
to vendor it or to treat `regex_grammar.c` as a checked-in generated artifact with
documented regeneration instructions. Prefer the latter: fewer submodules.

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
