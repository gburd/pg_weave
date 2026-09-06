# pg_weave -- standalone PGXS build
#
# Build against an installed PostgreSQL (>= 17):
#     make PG_CONFIG=/path/to/pg_config
#     make install PG_CONFIG=/path/to/pg_config
#     make installcheck PG_CONFIG=/path/to/pg_config   # regression + isolation + TAP
#
# PG_CONFIG defaults to whatever `pg_config` is on PATH.

MODULE_big = pg_weave

# Only separately-compiled translation units. src/am/amscan.c, src/query/lev.c
# and src/pages/trgm_page.c are #included directly into src/am/am.c (unity
# build) and must NOT be listed here or they will be compiled twice / linked
# with duplicate symbols.
OBJS = \
	$(WIN32RES) \
	src/query/analyze.o \
	src/query/tsanalyze.o \
	src/query/doc.o \
	src/query/parse.o \
	src/query/rank.o \
	src/am/am.o \
	src/am/customscan.o \
	src/am/amaux.o \
	src/util/migrate.o \
	src/query/trgm.o \
	src/util/sparsemap.o \
	src/query/match.o \
	src/vector/quantize.o \
	src/vector/pack.o \
	src/vector/wvec.o

# Headers moved under include/weave/ (see RELAYOUT); every .c file uses
# quoted "weave/foo.h" includes, so the include root needs to be on -I.
PG_CPPFLAGS = -I$(srcdir)/include

EXTENSION = pg_weave
DATA = sql/pg_weave--0.1.0.sql sql/pg_weave--0.1.0--0.2.0.sql
PGFILEDESC = "pg_weave - unified lexical + vector + fuzzy retrieval in one index"

# sql/ and expected/ are already at the top level (PGXS's built-in default
# --inputdir=$(srcdir) for pg_regress), so plain REGRESS with no REGRESS_OPTS
# picks up sql/<name>.sql + expected/<name>.out directly. No relayout fix
# needed here.
REGRESS = weave unicode_fold idx_scan_stats wvec

# --- Isolation tests -------------------------------------------------------
# pg_isolation_regress hardcodes its two lookup paths relative to a SINGLE
# --inputdir: "$(inputdir)/specs/<name>.spec" and "$(inputdir)/expected/<name>.out"
# (verified against the installed pg_isolation_regress binary; "specs" is not
# independently selectable via ISOLATION_OPTS the way REGRESS_OPTS composes
# sql/+expected/). Our relayout keeps specs under test/isolation/ while
# expected/ stays at the top level (shared with REGRESS), so no single
# --inputdir value satisfies both halves. Rather than pass ISOLATION_OPTS
# (which would require expected/ to move under test/isolation/ too, splitting
# it from the REGRESS expected/ directory), mirror the two .spec files into a
# top-level specs/ directory that PGXS's default inputdir="." already expects,
# keeping test/isolation/ as the single source of truth. The "specs" target is
# hooked in below, AFTER `include $(PGXS)`, so it does not become the default
# make goal (the first target in the combined makefile wins that, and we want
# PGXS's own "all" to stay default).
ISOLATION = weave_concurrency weave_cic

TAP_TESTS = 1

# quantize.c uses sqrt/log/exp for the Lloyd-Max solver.
SHLIB_LINK += -lm

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

.PHONY: specs
specs:
	@mkdir -p specs
	@for f in weave_concurrency weave_cic; do \
		cp -f test/isolation/$$f.spec specs/$$f.spec; \
	done

installcheck: specs
check: specs

# Vendored sparsemap uses C99 mixed declarations and its own /* FALLTHROUGH */
# comment convention (not recognized by -Wimplicit-fallthrough=5); suppress
# those warnings for it only (public symbols are namespaced via
# include/weave/sparsemap.h / sparsemap_impl.h).
src/util/sparsemap.o: CFLAGS += -Wno-declaration-after-statement -Wno-implicit-fallthrough

# --- Source distribution (PGXN release artifact) ---------------------------
# `make dist` produces pg_weave-$(DISTVERSION).zip in PGXN layout (all files
# under a pg_weave-$(DISTVERSION)/ prefix) straight from the committed tree
# via git archive, so it always matches the tag and never includes build
# artifacts. DISTVERSION is read from META.json (single source of truth).
DISTVERSION = $(shell grep -m1 '"version"' META.json | sed -E 's/.*"version": *"([^"]+)".*/\1/')
DISTNAME = pg_weave-$(DISTVERSION)

.PHONY: dist
dist:
	@test -n "$(DISTVERSION)" || { echo "could not read version from META.json" >&2; exit 1; }
	git archive --format=zip --prefix=$(DISTNAME)/ -o $(DISTNAME).zip HEAD
	@echo "created $(DISTNAME).zip"

# --- Pure-ASCII install SQL guard -------------------------------------------
# A non-ASCII byte anywhere in an install/upgrade .sql (e.g. a UTF-8 default
# like ellipsis '\u2026') makes CREATE EXTENSION FAIL on a non-UTF-8 server
# database (LATIN1, EUC_JP, ...) with "invalid byte sequence for encoding".
# This guard keeps the shipped SQL installable on every server encoding.
# Run standalone (`make check-ascii`); also runnable in CI.
.PHONY: check-ascii
check-ascii:
	@bad=$$(grep -lP '[^\x00-\x7F]' sql/pg_weave--*.sql pg_weave.control 2>/dev/null); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: non-ASCII bytes in install SQL (breaks CREATE EXTENSION on non-UTF-8 servers):" >&2; \
		for f in $$bad; do echo "  $$f:"; grep -nP '[^\x00-\x7F]' "$$f" | head; done >&2; \
		exit 1; \
	fi; \
	echo "install SQL is pure ASCII (installs on any server encoding)"

# Allocation-safety lint: flag any palloc/repalloc sized from a corpus/
# vocabulary-scale quantity that is not huge-safe. Run standalone
# (`make check-alloc`); in CI.
.PHONY: check-alloc
check-alloc:
	@bash ci/check-alloc.sh

# --- Rename-completeness lint ------------------------------------------------
# pg_weave was forked from pg_fts (bm25_->weave_, fts_->weave_, ftsdoc->wdoc,
# ftsquery->wquery, AM name fts->weave, opclass wdoc_fts_ops->wdoc_lex_ops).
# Flags any leftover pg_fts-era identifier under src/ or include/. Matching is
# case-insensitive because the old type names surface both lower-cased
# ("ftsdoc") and upper-cased inside macros ("PG_GETARG_FTSDOC"). "bm25" itself
# is NOT flagged wholesale -- it is the retained scoring-algorithm name
# (weave_bm25, weave_bm25_opts, weave_bm25f, WEAVE_BM25L, WEAVE_BM25PLUS,
# and the literal variant strings "bm25+"/"bm25l"/"bm25f"/"bm25s" are all
# legitimate). Only a bare bm25_ prefix at a word boundary (the OLD C symbol
# prefix, e.g. a stray "bm25_foo") is treated as a residual.
.PHONY: check-rename
check-rename:
	@fail=0; \
	hits=$$(grep -rnEi 'ftsdoc|ftsquery|pg_fts' src include 2>/dev/null); \
	if [ -n "$$hits" ]; then echo "$$hits"; fail=1; fi; \
	hits2=$$(grep -rnE '(^|[^A-Za-z0-9_])bm25_[A-Za-z_]' src include 2>/dev/null); \
	if [ -n "$$hits2" ]; then echo "$$hits2"; fail=1; fi; \
	if [ "$$fail" -ne 0 ]; then \
		echo "" >&2; \
		echo "ERROR: residual pg_fts-era identifier(s) found (see above)." >&2; \
		echo "Allowed: the BM25 scoring-function name and its variants" >&2; \
		echo "(weave_bm25, weave_bm25_opts, weave_bm25f, WEAVE_BM25L," >&2; \
		echo "WEAVE_BM25PLUS, 'bm25+', 'bm25l', 'bm25f', 'bm25s')." >&2; \
		exit 1; \
	fi; \
	echo "check-rename: clean (no residual bm25_/ftsdoc/ftsquery/pg_fts identifiers)"

# --- Unity-build guard -------------------------------------------------------
# src/am/am.c #includes amscan.c, ../query/lev.c and ../pages/trgm_page.c
# directly (unity build). If any of the three ever end up separately listed
# in OBJS *and* still #included by am.c, their symbols get compiled/linked
# twice. Guard both halves: they must NOT be in OBJS, and they MUST still be
# #included by am.c.
.PHONY: check-unity
check-unity:
	@fail=0; \
	for f in "src/am/amscan.c" "src/query/lev.c" "src/pages/trgm_page.c"; do \
		base=$$(basename $$f | sed 's/\.c$$/.o/'); \
		if echo "$(OBJS)" | grep -qE "(^| )([^ ]*/)?$$base( |$$)"; then \
			echo "ERROR: $$f is separately listed in OBJS (breaks the am.c unity build)" >&2; \
			fail=1; \
		fi; \
		if ! grep -qE '#include[[:space:]]*"([^"]*/)?'"$$(basename $$f)"'"' src/am/am.c; then \
			echo "ERROR: src/am/am.c no longer #includes $$f (unity build broken)" >&2; \
			fail=1; \
		fi; \
	done; \
	if [ "$$fail" -ne 0 ]; then exit 1; fi; \
	echo "check-unity: amscan.c/lev.c/trgm_page.c are unity-included, not in OBJS"
