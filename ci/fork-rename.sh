#!/usr/bin/env bash
#
# ci/fork-rename.sh -- one-shot provenance script, kept in-tree for audit.
#
# This is the exact, reviewable transformation that produced pg_weave's initial
# lexical-channel engine from pg_weave 1.5.8.  It is NOT part of the build; it
# documents how the fork was made so a reviewer can reproduce and diff it:
#
#     mkdir /tmp/fork && cd /tmp/fork && git init
#     git -C ~/ws/pg_weave archive <sha> | tar -x
#     bash ci/fork-rename.sh
#
# Symbol namespace mapping.  Every class below was verified collision-free
# against every other class before the fork was taken (see doc/LICENSING.md
# "Provenance").
#
#     WEAVE_*        -> WEAVE_*      engine macros
#     BM25<Upper>*  -> Weave*       engine typedefs (WeaveSegMeta -> WeaveSegMeta)
#     weave_*        -> weave_*      engine functions
#     bare "weave"   -> "weave"      prose referring to the engine/AM
#     bare "BM25"   -> KEPT         the Okapi BM25 ranking function; a real name
#     WEAVE_*         -> WEAVE_*      SQL-surface macros
#     Weave*          -> Weave*       SQL-surface typedefs
#     weave_*         -> weave_*      SQL-callable functions
#     wdoc        -> wdoc         SQL type: analyzed lexical document
#     wquery      -> wquery       SQL type: parsed query
#     pg_weave        -> pg_weave     module / extension / library
#     AM name "fts" -> "weave"
#
# Note the deliberate asymmetry on BM25: `WeaveSegMeta` is our struct and gets
# renamed, but "BM25 ranking" is Robertson & Sparck Jones's scoring function and
# keeping that name is a documentation-correctness requirement.  The \b anchors
# are load-bearing for this: `_` is a word character to sed, so `\bbm25_` does
# not match inside `weave_bm25_opts`.
#
set -euo pipefail
cd "$(dirname "$0")/.."

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# 1. Drop pg_weave release history and top-level project files.  pg_weave starts
#    its own version line at 0.1.0 and authors its own build/docs; carrying 47
#    foreign upgrade scripts would misrepresent history.
# ---------------------------------------------------------------------------
say "dropping pg_weave release history and project files"
mkdir -p src/am src/pages src/postings src/query src/vector src/util src/wal \
         include/weave sql expected t test/fuzz test/hegel test/isolation \
         bench doc/specs ci examples
mv pg_weave--1.5.8.sql sql/pg_weave--0.1.0.sql
rm -f pg_weave--*.sql
rm -f CHANGELOG.md CAPABILITIES.md DEFERRED.md ROADMAP.md RELEASING.md \
      renovate.json pg_weave-managed-service-readiness.md SECURITY.md \
      META.json Makefile meson.build meson_options.txt flake.nix flake.lock \
      README.md AGENTS.md CONTRIBUTING.md CODE_OF_CONDUCT.md pg_weave.control \
      LICENSE
rm -rf specs bench_vac doc .agent .github .forgejo .claude .kiro .direnv \
       .envrc .mcp.json .editorconfig .gitattributes .gitignore \
       .agent-steering-domains.md ci/announce.sh ci/coverage.sh ci/noop.pl

# ---------------------------------------------------------------------------
# 2. Relayout into PostgreSQL-core shape: private headers under include/weave/,
#    implementation under src/<subsystem>/.  Mirrors core's
#    src/include/access/nbtree.h + src/backend/access/nbtree/*.c split.
# ---------------------------------------------------------------------------
say "relayout to src/ + include/weave/"
mv pg_weave.h          include/weave/weave.h
mv pg_weave_am.h       include/weave/am.h
mv pg_weave_for.h      include/weave/for.h
mv pg_weave_docvalid.h include/weave/docvalid.h
mv pg_weave_sm.h       include/weave/sparsemap.h
mv vendor/sm.h       include/weave/sparsemap_impl.h
mv vendor/sm.c       src/util/sparsemap.c
rmdir vendor

mv pg_weave_am.c         src/am/am.c
mv pg_weave_am_scan.c    src/am/amscan.c
mv pg_weave_customscan.c src/am/customscan.c
mv pg_weave_aux.c        src/am/amaux.c
mv pg_weave_trgm_index.c src/pages/trgm_page.c
mv pg_weave_query.c      src/query/parse.c
mv pg_weave_doc.c        src/query/doc.c
mv pg_weave_rank.c       src/query/rank.c
mv pg_weave_analyze.c    src/query/analyze.c
mv pg_weave_tsanalyze.c  src/query/tsanalyze.c
mv pg_weave_trgm.c       src/query/trgm.c
mv pg_weave_lev.c        src/query/lev.c
mv pg_weave_match.c      src/query/match.c
mv pg_weave_migrate.c    src/util/migrate.c

mv sql/pg_weave.sql                sql/weave.sql
mv expected/pg_weave.out           expected/weave.out
mv expected/weave_concurrency.out expected/weave_concurrency.out
mv expected/weave_cic.out         expected/weave_cic.out

# ---------------------------------------------------------------------------
# 3. Rewrite #include paths first: the old paths embed the old prefixes, so
#    doing this after the identifier pass would need a second, subtler pass.
#    pg_weave's am.c is a unity build (#include of three .c files); that structure
#    is preserved for now -- splitting it is task L1 in doc/PHASES.md.
# ---------------------------------------------------------------------------
say "rewriting include paths"
mapfile -t CFILES < <(find src include test -type f \( -name '*.c' -o -name '*.h' \))
sed -i \
  -e 's|#include "pg_weave\.h"|#include "weave/weave.h"|' \
  -e 's|#include "pg_weave_am\.h"|#include "weave/am.h"|' \
  -e 's|#include "pg_weave_for\.h"|#include "weave/for.h"|' \
  -e 's|#include "pg_weave_docvalid\.h"|#include "weave/docvalid.h"|' \
  -e 's|#include "pg_weave_sm\.h"|#include "weave/sparsemap.h"|' \
  -e 's|#include "vendor/sm\.h"|#include "weave/sparsemap_impl.h"|' \
  -e 's|#include "sm\.h"|#include "weave/sparsemap_impl.h"|' \
  -e 's|#include "pg_weave_lev\.c"|#include "../query/lev.c"|' \
  -e 's|#include "pg_weave_am_scan\.c"|#include "amscan.c"|' \
  -e 's|#include "pg_weave_trgm_index\.c"|#include "../pages/trgm_page.c"|' \
  "${CFILES[@]}"

# ---------------------------------------------------------------------------
# 4. Identifier rename.
# ---------------------------------------------------------------------------
say "renaming identifiers"
mapfile -t ALL < <(find src include sql expected t test bench ci examples \
        -type f \( -name '*.c' -o -name '*.h' -o -name '*.sql' -o -name '*.out' \
           -o -name '*.pl' -o -name '*.spec' -o -name '*.md' -o -name '*.sh' \
           -o -name '*.py' -o -name '*.sgml' -o -name '*.conf' \))
sed -i \
  -e 's/\bpg_fts_/pg_weave_/g' \
  -e 's/\bpg_fts\b/pg_weave/g' \
  -e 's/\bPG_FTS_/WEAVE_/g' \
  -e 's/\bPG_FTS\b/WEAVE/g' \
  -e 's/\bftsdoc/wdoc/g' \
  -e 's/\bFTSDOC/WDOC/g' \
  -e 's/\bftsquery/wquery/g' \
  -e 's/\bFTSQUERY/WQUERY/g' \
  -e 's/\bBM25_/WEAVE_/g' \
  -e 's/\bBM25\([A-Z]\)/Weave\1/g' \
  -e 's/\bBm25/Weave/g' \
  -e 's/\bbm25_/weave_/g' \
  -e 's/\bbm25shared\b/weaveshared/g' \
  -e 's/\bbm25\b/weave/g' \
  -e 's/\bFTS_/WEAVE_/g' \
  -e 's/\bFts/Weave/g' \
  -e 's/\bfts_/weave_/g' \
  "${ALL[@]}"

# The access-method name itself: narrowly scoped, deliberately last.
say "renaming the access method"
sed -i -e 's/get_index_am_oid("weave"/get_index_am_oid("weave"/g' \
       -e 's/\ban fts index\b/a weave index/g' \
       -e 's/\bACCESS METHOD fts\b/ACCESS METHOD weave/g' \
       -e 's/\bUSING fts\b/USING weave/g' \
       -e 's/\bwdoc_fts_ops\b/wdoc_lex_ops/g' \
       "${ALL[@]}"

# ---------------------------------------------------------------------------
# 5. Audit.  Anything in the first section is a rename miss and must be triaged
#    by hand; the second section is expected to be non-empty.
# ---------------------------------------------------------------------------
say "audit: residual old identifiers (must be empty)"
grep -rnE '\b(weave|Weave|wdoc|wquery|weave_|Weave|WEAVE_|pg_weave)' \
     src include sql expected t test 2>/dev/null || echo "  (clean)"
say "audit: BM25 retained as the scoring-function name (expected non-zero)"
grep -rohE '\bBM25\b' src include sql | wc -l
