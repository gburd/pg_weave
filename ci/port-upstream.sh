#!/usr/bin/env bash
#
# ci/port-upstream.sh -- port a pg_fts commit into pg_weave's namespace/layout.
#
# pg_weave's lexical channel is a fork of pg_fts (doc/LICENSING.md).  Upstream
# keeps improving it, and re-deriving each fix by hand is both slow and how
# transcription errors get in.  This applies the same mechanical transformation
# ci/fork-rename.sh applied to the tree, but to a *diff*.
#
# Usage:
#	 ci/port-upstream.sh <commit-ish> [path-to-pg_fts]
#	 ci/port-upstream.sh HEAD ~/ws/pg_fts
#
# It prints the rewritten patch to stdout and, with -a, applies it.  Always
# review before applying: the rename is mechanical but the surrounding code has
# diverged (relayout, the L1 split of am.c into four TUs, the extra channels), so
# a hunk can apply cleanly and still be wrong.
#
# After applying, the non-negotiable checks are:
#	 make && make installcheck && make check-rename
# plus the standalone property tests, because a lexical-channel change can
# perturb the FOR codec and nothing in pg_regress would notice.
#
set -euo pipefail

APPLY=0
if [ "${1:-}" = "-a" ]; then APPLY=1; shift; fi

COMMIT=${1:?usage: ci/port-upstream.sh [-a] <commit-ish> [pg_fts-path]}
UPSTREAM=${2:-$HOME/ws/pg_fts}
cd "$(dirname "$0")/.."

[ -d "$UPSTREAM/.git" ] || { echo "no git repo at $UPSTREAM" >&2; exit 1; }

# Path mapping, mirroring the relayout in ci/fork-rename.sh.  Anything not listed
# is reported rather than silently dropped -- a missing mapping means upstream
# touched a file we relaid out differently, and guessing is worse than stopping.
map_path() {
	case "$1" in
		pg_fts_am.c)         echo src/am/am.c ;;
		pg_fts_am_scan.c)    echo src/am/amscan.c ;;
		pg_fts_customscan.c) echo src/am/customscan.c ;;
		pg_fts_aux.c)        echo src/am/amaux.c ;;
		pg_fts_trgm_index.c) echo src/pages/trgm_page.c ;;
		pg_fts_query.c)      echo src/query/parse.c ;;
		pg_fts_doc.c)        echo src/query/doc.c ;;
		pg_fts_rank.c)       echo src/query/rank.c ;;
		pg_fts_analyze.c)    echo src/query/analyze.c ;;
		pg_fts_tsanalyze.c)  echo src/query/tsanalyze.c ;;
		pg_fts_trgm.c)       echo src/query/trgm.c ;;
		pg_fts_lev.c)        echo src/query/lev.c ;;
		pg_fts_match.c)      echo src/query/match.c ;;
		pg_fts_migrate.c)    echo src/util/migrate.c ;;
		pg_fts.h)            echo include/weave/weave.h ;;
		pg_fts_am.h)         echo include/weave/am.h ;;
		pg_fts_for.h)        echo include/weave/for.h ;;
		pg_fts_docvalid.h)   echo include/weave/docvalid.h ;;
		pg_fts_sm.h)         echo include/weave/sparsemap.h ;;
		vendor/sm.c)         echo src/util/sparsemap.c ;;
		vendor/sm.h)         echo include/weave/sparsemap_impl.h ;;
		# Deliberately NOT mapped.  Upstream's regression files carry pg_fts
		# release bookkeeping -- the CREATE EXTENSION VERSION literal in
		# particular -- which always conflicts with our own version line.  They
		# are reported as unmapped so any genuinely new test case in them gets
		# ported by hand rather than dragged in with the bookkeeping.
		sql/pg_fts.sql)      echo "" ;;
		expected/pg_fts.out) echo "" ;;
		*)                   echo "" ;;
	esac
}

PATCH=$(mktemp)
git -C "$UPSTREAM" show --no-color --format= "$COMMIT" > "$PATCH"

# Which files does the commit touch, and can we map all of them?
UNMAPPED=""
while read -r f; do
	[ -z "$f" ] && continue
	if [ -z "$(map_path "$f")" ]; then UNMAPPED="$UNMAPPED $f"; fi
done < <(git -C "$UPSTREAM" show --no-color --name-only --format= "$COMMIT")

if [ -n "$UNMAPPED" ]; then
	echo "# NOT MAPPED (review by hand, these hunks are dropped):" >&2
	for f in $UNMAPPED; do echo "#   $f" >&2; done
fi

OUT=$(mktemp)
: > "$OUT"

# Split the patch per file so unmapped files can be dropped wholesale.
awk '/^diff --git /{n++} {print > ("'"$PATCH"'.part." n)}' "$PATCH"
for part in "$PATCH".part.*; do
	[ -s "$part" ] || continue
	src=$(sed -n 's|^diff --git a/\(.*\) b/.*|\1|p' "$part" | head -1)
	[ -z "$src" ] && continue
	dst=$(map_path "$src")
	[ -z "$dst" ] && continue

	sed -e "s|^diff --git a/$src b/$src|diff --git a/$dst b/$dst|" \
		-e "s|^--- a/$src|--- a/$dst|" \
		-e "s|^+++ b/$src|+++ b/$dst|" "$part" >> "$OUT"
done
rm -f "$PATCH".part.*

# Include-path rewrites first, exactly as ci/fork-rename.sh step 3 -- the old
# paths embed the old prefixes, so the identifier pass below would turn
# #include "pg_fts.h" into #include "pg_weave.h", which does not exist.
sed -i \
	-e 's|#include "pg_fts\.h"|#include "weave/weave.h"|' \
	-e 's|#include "pg_fts_am\.h"|#include "weave/am.h"|' \
	-e 's|#include "pg_fts_for\.h"|#include "weave/for.h"|' \
	-e 's|#include "pg_fts_docvalid\.h"|#include "weave/docvalid.h"|' \
	-e 's|#include "pg_fts_sm\.h"|#include "weave/sparsemap.h"|' \
	-e 's|#include "vendor/sm\.h"|#include "weave/sparsemap_impl.h"|' \
	"$OUT"

# Upstream pg_fts still #includes pg_fts_lev.c, pg_fts_am_scan.c and
# pg_fts_trgm_index.c into pg_fts_am.c.  pg_weave does not: task L1 split am.c
# into am.c/ambuild.c/amvacuum.c/amscan.c and made lev.c and trgm_page.c ordinary
# translation units.  A hunk that touches one of those three #include lines has no
# target here and must be dropped by hand -- flag it rather than silently
# rewriting it, because a hunk landing in the wrong one of four files compiles.
if grep -qE '^\+.*#include "pg_fts_(lev|am_scan|trgm_index)\.c"' "$OUT"; then
	say "WARNING: patch touches an upstream unity #include; pg_weave has no such line (task L1)"
fi

# The identifier rename, identical to ci/fork-rename.sh step 4 plus the
# compound-identifier fixes that step missed the first time.  \b anchoring is
# load-bearing: it is what keeps BM25F, bm25+, and BM25L_DELTA intact.
sed -i \
	-e 's/\bpg_fts_/pg_weave_/g' \
	-e 's/\bpg_fts\b/pg_weave/g' \
	-e 's/\bPG_FTS_TEST_HOOKS\b/WEAVE_TEST_HOOKS/g' \
	-e 's/\bPG_FTS_/WEAVE_/g' \
	-e 's/\bPG_FTS\b/WEAVE/g' \
	-e 's/\bto_ftsdoc/to_wdoc/g' \
	-e 's/\bto_ftsquery/to_wquery/g' \
	-e 's/\btsquery_to_ftsquery\b/tsquery_to_wquery/g' \
	-e 's/\bsetftsweight\b/setwdocweight/g' \
	-e 's/\bftsdoc/wdoc/g' \
	-e 's/FTSDOC/WDOC/g' \
	-e 's/\bftsquery/wquery/g' \
	-e 's/FTSQUERY/WQUERY/g' \
	-e 's/\bDatumGetFtsDoc/DatumGetWDoc/g' \
	-e 's/\bDatumGetFtsQuery/DatumGetWQuery/g' \
	-e 's/\bBM25_/WEAVE_/g' \
	-e 's/\bBM25\([A-Z]\)/Weave\1/g' \
	-e 's/\bBm25/Weave/g' \
	-e 's/\bbm25_/weave_/g' \
	-e 's/\bbm25shared\b/weaveshared/g' \
	-e 's/\bbm25leader\b/weaveleader/g' \
	-e 's/\bbm25\b/weave/g' \
	-e 's/\bFTS_/WEAVE_/g' \
	-e 's/\bFts/Weave/g' \
	-e 's/\bfts_/weave_/g' \
	-e 's/get_index_am_oid("fts"/get_index_am_oid("weave"/g' \
	-e 's/\ban fts index\b/a weave index/g' \
	-e 's/\bnon-fts\b/non-weave/g' \
	-e 's/\bfts index\b/weave index/g' \
	"$OUT"

# Restore the scoring-function names the blanket BM25<Upper> rule mangles.
sed -i -e 's/\bWeaveF\b/BM25F/g' -e 's/\bWeaveL_DELTA\b/BM25L_DELTA/g' \
	-e 's/\bWeavePLUS_DELTA\b/BM25PLUS_DELTA/g' -e 's/\bWeaveL\b/BM25L/g' \
	"$OUT"

if [ "$APPLY" = 1 ]; then
	git apply --3way -v "$OUT" || {
		echo "# patch did not apply cleanly; inspect $OUT" >&2
		exit 1
	}
	echo "# applied.  Now run: make && make installcheck && make check-rename" >&2
else
	cat "$OUT"
fi
