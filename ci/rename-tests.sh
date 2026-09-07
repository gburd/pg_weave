#!/usr/bin/env bash
# ci/rename-tests.sh -- one-shot: rename the recovered pg_fts test suite.
#
# Provenance note, and a process failure worth recording.
#
# The fork used `git archive` to export pg_fts, and pg_fts's .gitattributes marks
# `test`, `bench`, `ci` and `.agent` as export-ignore -- deliberately, because
# `git archive` is also how it builds the PGXN release zip, which should carry the
# buildable extension and not dev scaffolding.  The consequence was silent: the
# fork dropped the ENTIRE property-test and fuzz suite.
#
#   test/hegel/test_for.c        FOR codec round-trip
#   test/hegel/test_for_props.c  FOR codec properties
#   test/hegel/test_docvalid.c   doc-validity decoder
#   test/hegel/test_lev.c        bounded Levenshtein automaton
#   test/fuzz/fuzz_for.c         FOR codec, adversarial bytes
#   test/fuzz/fuzz_block.c       posting-block parser
#   test/fuzz/fuzz_docvalid.c    doc-validity parser
#
# Nothing noticed for six days, and in that time the FOR codec was modified twice
# -- which is precisely the situation AGENTS.md rule 1 and doc/TESTING.md exist to
# prevent, and precisely the class of change a fixed-expected-output regression
# test cannot catch.  Recovered with `tar` rather than `git archive`.
#
# The lesson generalises: `git archive` is a RELEASE tool.  Use `tar` or `cp` when
# the intent is to copy a working tree.  ci/port-upstream.sh already carries a
# related caveat about upstream's regression files.
set -euo pipefail
cd "$(dirname "$0")/.."

mapfile -t T < <(find test/hegel test/fuzz test/a1_recycle -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.txt' -o -name '*.sh' -o -name '*.md' \))

# Include paths first, then identifiers -- same order and same rules as
# ci/fork-rename.sh, including the \b anchoring that keeps BM25F and BM25L intact.
sed -i \
  -e 's|"pg_fts_for\.h"|"weave/for.h"|' \
  -e 's|"pg_fts_docvalid\.h"|"weave/docvalid.h"|' \
  -e 's|"pg_fts\.h"|"weave/weave.h"|' \
  -e 's|"pg_fts_am\.h"|"weave/am.h"|' \
  -e 's|pg_fts_for\.h|weave/for.h|g' \
  -e 's|pg_fts_docvalid\.h|weave/docvalid.h|g' \
  -e 's/\bpg_fts_/pg_weave_/g' \
  -e 's/\bpg_fts\b/pg_weave/g' \
  -e 's/\bBM25_/WEAVE_/g' \
  -e 's/\bBM25\([A-Z]\)/Weave\1/g' \
  -e 's/\bbm25_/weave_/g' \
  -e 's/\bbm25\b/weave/g' \
  -e 's/\bFTS_/WEAVE_/g' \
  -e 's/\bFts/Weave/g' \
  -e 's/\bfts_/weave_/g' \
  "${T[@]}"

# Compound identifiers the \b rules miss, because `_` is a word character:
# to_ftsdoc / to_ftsquery have no boundary before "fts".  Missing these is how the
# first fork pass left `to_ftsdoc` behind in the SQL.
sed -i \
  -e 's/\bto_ftsdoc/to_wdoc/g' \
  -e 's/\bto_ftsquery/to_wquery/g' \
  -e 's/\bftsdoc/wdoc/g' \
  -e 's/\bftsquery/wquery/g' \
  -e 's/\bUSING fts\b/USING weave/g' \
  "${T[@]}"

sed -i -e 's/\bWeaveF\b/BM25F/g' -e 's/\bWeaveL\b/BM25L/g' "${T[@]}"

echo "residual old identifiers (must be empty):"
grep -rn 'pg_fts\|ftsdoc\|ftsquery\|\bbm25_' test/hegel test/fuzz test/a1_recycle \
    2>/dev/null || echo "  (clean)"
