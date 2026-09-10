#!/usr/bin/env bash
# ci/check-alloc.sh -- allocation-safety lint for pg_weave.
#
# Flags any palloc()/repalloc() whose SIZE expression contains a
# corpus/vocabulary-scale quantity (df, sumtf, vocabulary size, posting count,
# etc.) that is NOT allocated through the huge-safe path
# (WEAVE_ALLOC_MAYBE_HUGE / WEAVE_REALLOC_MAYBE_HUGE / MemoryContextAllocHuge /
# repalloc_huge, or an inline `> MaxAllocSize ? ...Huge : palloc` ternary).
#
# Motivation (inherited from pg_fts, this extension's ancestor): production
# crashes were all the same failure mode -- an allocation sized from a
# corpus-scale term without a MaxAllocSize guard -- found one function at a
# time by a real large multi-GB / multi-million-doc build. This is the
# systematic sweep that catches the *next* one at commit time instead of in
# production.
#
# A flagged line is a *candidate*, not a proven bug: an allocation whose
# corpus-scale driver is provably bounded elsewhere (by maintenance_work_mem,
# by one document, by the query size, or by a preceding `> MaxAllocSize`
# ereport) is annotated with a trailing  // alloc-ok: <reason>  comment to
# allowlist it. Adding such an allocation without either the huge-safe path or
# an alloc-ok annotation fails CI -- forcing a human to classify every new
# corpus-scale allocation as bounded-or-huge-safe.
#
# Exit 0 = clean; exit 1 = one or more unannotated corpus-scale plain allocs.

set -euo pipefail
cd "$(dirname "$0")/.."

# Files with corpus/vocabulary-scale allocations (the AM + doc/analyze paths).
# pg_weave's RELAYOUT: pg_fts_am.c -> src/am/{am,ambuild,amvacuum,amscan}.c (one
# file until task L1 split it), pg_fts_doc.c -> src/query/doc.c,
# pg_fts_analyze.c -> src/query/analyze.c, pg_fts_tsanalyze.c ->
# src/query/tsanalyze.c.
FILES="src/am/am.c src/am/ambuild.c src/am/amvacuum.c src/am/amscan.c src/pages/trgm_page.c src/query/doc.c src/query/analyze.c src/query/tsanalyze.c"

# Size-drivers that scale with the corpus / vocabulary / one document (unbounded
# in principle) -- as opposed to query size or fixed structs. Matched as whole
# words in the allocation's argument text.
SCALE='df|gdf|sumtf|nterms|nout|ndocs|nposts|maxdocids|maxaccs|naccs|npos|maxpos|maxposts|parena_cap|ocap|sumdoclen|maxraw|doclen|ndistinct'

fail=0
for f in $FILES; do
	[ -f "$f" ] || continue
	# plain palloc0()/repalloc() (NOT the huge-safe forms), whose args mention a
	# scale driver, and which are not annotated alloc-ok.
	while IFS= read -r line; do
		n="${line%%:*}"
		text="${line#*:}"
		# skip the huge-safe allocator forms and the inline ternary guard
		case "$text" in
			*WEAVE_ALLOC_MAYBE_HUGE*|*WEAVE_REALLOC_MAYBE_HUGE*|*MemoryContextAllocHuge*|*repalloc_huge*) continue ;;
		esac
		# skip lines explicitly allowlisted with a reason
		case "$text" in *alloc-ok:*) continue ;; esac
		# a plain palloc/repalloc call with a scale-driver word in it?
		if printf '%s' "$text" | grep -qE '\b(palloc0?|repalloc)\(' \
		   && printf '%s' "$text" | grep -qwE "$SCALE"; then
			echo "UNGUARDED corpus-scale alloc: $f:$n:$text"
			fail=1
		fi
	done < <(grep -nE '\b(palloc0?|repalloc)\(' "$f")
done

if [ "$fail" -ne 0 ]; then
	echo ""
	echo "Each line above allocates from a corpus/vocabulary/document-scale size"
	echo "without the huge-safe path.  Either:"
	echo "  - use WEAVE_ALLOC_MAYBE_HUGE / WEAVE_REALLOC_MAYBE_HUGE (or the inline"
	echo "    '> MaxAllocSize ? MemoryContextAllocHuge : palloc' form), OR"
	echo "  - if the size is provably bounded (by maintenance_work_mem, one"
	echo "    document, the query, or a preceding '> MaxAllocSize' ereport),"
	echo "    annotate the line with a trailing  // alloc-ok: <one-line reason>."
	exit 1
fi
echo "alloc-safety: no unguarded corpus/vocabulary-scale allocations"
