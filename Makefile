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
	src/am/amsize.o \
	src/util/migrate.o \
	src/query/trgm.o \
	src/util/sparsemap.o \
	src/query/match.o \
	src/vector/quantize.o \
	src/vector/pack.o \
	src/vector/wvec.o \
	$(FUZZY_OBJS) \
	$(TRE_OBJS)

# --- Fuzzy/regex/prefix channel (imported from pg_tre; see
# doc/specs/IMPORT_pg_tre.md).  Ordinary translation units, unlike the
# src/am/am.c unity build.  Wired into the build by task Z1/Z2; the channel
# is not yet reachable from the access method or the planner (Z3 onward), so
# these compile and link but nothing calls them yet.
FUZZY_OBJS = \
	src/query/fuzzy_guc.o \
	src/query/hash.o \
	src/query/surf.o \
	src/query/uleven.o \
	src/query/regex_ast.o \
	src/query/regex_grammar.o \
	src/query/regex_tokens.o \
	src/query/parser.o \
	src/query/extract.o \
	src/query/tiling.o \
	src/query/like_translate.o \
	src/query/pattern_cache.o \
	src/query/trgm_similarity.o \
	src/query/re_match.o \
	src/util/utf8.o

# --- Vendored TRE (laurikari/tre d0e0c99, BSD-2; see doc/LICENSING.md).
# Compiled in-tree as plain translation units rather than driven through
# TRE's autotools: no submodule, no ./configure step in the extension build,
# and the two feature headers TRE's configure would have generated are
# checked in (vendor/tre/config.h, vendor/tre/local_includes/tre-config.h).
# The source list mirrors libtre_la_SOURCES from vendor/tre/lib/Makefile.am
# with TRE_APPROX on; xmalloc.c is upstream's malloc-debugging shim and is
# not built (MALLOC_DEBUGGING is off, so xmalloc.h degrades to malloc()).
TRE_OBJS = \
	vendor/tre/lib/tre-ast.o \
	vendor/tre/lib/tre-compile.o \
	vendor/tre/lib/tre-match-approx.o \
	vendor/tre/lib/tre-match-backtrack.o \
	vendor/tre/lib/tre-match-parallel.o \
	vendor/tre/lib/tre-mem.o \
	vendor/tre/lib/tre-parse.o \
	vendor/tre/lib/tre-stack.o \
	vendor/tre/lib/regcomp.o \
	vendor/tre/lib/regerror.o \
	vendor/tre/lib/regexec.o

# Headers moved under include/weave/ (see RELAYOUT); every .c file uses
# quoted "weave/foo.h" includes, so the include root needs to be on -I.
PG_CPPFLAGS = -I$(srcdir)/include

# TRE's headers are deliberately NOT on the global include path: its
# local_includes/ would put a bare "tre.h" next to PostgreSQL's headers, and
# vendor/tre/config.h would shadow any other <config.h>.  Only the two kinds
# of translation unit that need them get them -- the vendored TRE sources
# themselves, and src/query/re_match.c, which reaches into tre-internal.h for
# tre_tnfa_t.num_states and (deliberately) does not include postgres.h.
TRE_CPPFLAGS = \
	-DHAVE_CONFIG_H \
	-I$(srcdir)/vendor/tre \
	-I$(srcdir)/vendor/tre/lib \
	-I$(srcdir)/vendor/tre/local_includes

EXTENSION = pg_weave
DATA = sql/pg_weave--0.1.0.sql sql/pg_weave--0.1.0--0.2.0.sql sql/pg_weave--0.2.0--0.3.0.sql sql/pg_weave--0.3.0--0.4.0.sql sql/pg_weave--0.4.0--0.5.0.sql
PGFILEDESC = "pg_weave - unified lexical + vector + fuzzy retrieval in one index"

# sql/ and expected/ are already at the top level (PGXS's built-in default
# --inputdir=$(srcdir) for pg_regress), so plain REGRESS with no REGRESS_OPTS
# picks up sql/<name>.sql + expected/<name>.out directly. No relayout fix
# needed here.
REGRESS = weave unicode_fold idx_scan_stats wvec orderby

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

# Imported pg_tre sources and vendored TRE use C99 mixed declarations and their own
# fallthrough conventions, which PostgreSQL's warning set forbids.  Scope the
# override to those objects rather than rewriting imported code -- a local rewrite
# would have to be redone on every upstream port, and ci/port-upstream.sh exists
# precisely so ports stay mechanical.  Our own code stays warning-clean under the
# full set.
$(FUZZY_OBJS) $(TRE_OBJS): CFLAGS += -Wno-declaration-after-statement -Wno-implicit-fallthrough

# --- Vendored TRE and its glue: include paths and upstream-warning relief ---
# TRE is 2001-2009 C: mixed declarations, K&R-era prototypes, and its own
# fallthrough convention.  Patching it to satisfy PostgreSQL's warning set
# would make every future re-pin a merge conflict, so the warnings are
# suppressed for the vendored files only.
# The include flags go in CPPFLAGS because that is the one variable both the
# .o rule and the LLVM-bitcode .bc rule use (the .bc rule uses
# BITCODE_CFLAGS, not CFLAGS -- a with_llvm=yes build fails to find
# <config.h> if the -I lands only in CFLAGS).
TRE_TUS = $(TRE_OBJS) src/query/re_match.o
$(TRE_TUS) $(TRE_TUS:.o=.bc): CPPFLAGS += $(TRE_CPPFLAGS)

TRE_WARN_OFF = -Wno-declaration-after-statement -Wno-implicit-fallthrough \
	-Wno-unused-parameter -Wno-missing-prototypes -Wno-sign-compare
$(TRE_OBJS): CFLAGS += $(TRE_WARN_OFF)
$(TRE_OBJS:.o=.bc): BITCODE_CFLAGS += $(TRE_WARN_OFF)

# regex_grammar.c is Lime-generated (source: src/query/regex_grammar.y); do
# not hand-edit it, and do not hold generated code to the project's warning
# standard.  Regeneration needs Lime, which pg_weave does not vendor -- see
# doc/LICENSING.md "Lime".
src/query/regex_grammar.o: CFLAGS += -Wno-unused-parameter -Wno-missing-prototypes

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

# --- Standalone tests: no PostgreSQL server, no extension install -------------
#
# These gate the algorithmic cores directly.  They exist because a
# fixed-expected-output regression test structurally cannot catch a codec or bound
# that is subtly wrong: the answers stay plausible, they are just wrong.  See
# doc/TESTING.md, and AGENTS.md rule 1.
#
# Every target here must build with nothing but a C compiler and -I include.  The
# inherited cmocka+hegel suite (test_for.c, test_for_props.c, test_docvalid.c,
# test_lev.c) is deliberately NOT here: it needs two external libraries, so it
# cannot gate a build, which is why dependency-free equivalents are being written
# one at a time.
CHECK_CC ?= cc
STANDALONE_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -I include

.PHONY: check-standalone
check-standalone:
	@set -e; \
	tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	echo "== FOR codec: fast extractor vs the bit-by-bit implementation it replaced =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/for test/hegel/test_for_get.c -lm; \
	$$tmp/for | tail -1; \
	echo "== vector quantizer: rotation, codebook, encode, packing, block bound (C2) =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/q test/hegel/test_quantize.c \
		src/vector/quantize.c src/vector/pack.c -lm; \
	$$tmp/q | tail -1; \
	echo "== ALL STANDALONE CHECKS PASSED =="

# Cross-version sparsemap wire compatibility.  Separate because it needs the
# sparsemap repository to extract the PREVIOUS release's sources -- it verifies that
# blobs already on disk still read correctly, which is the hazard re-vendoring
# carries and an ordinary dependency bump does not.  Skips with a loud notice rather
# than failing when the repo is absent, so CI without it stays green while a
# developer re-vendoring locally always runs it.
.PHONY: check-sparsemap-wire
check-sparsemap-wire:
	@if [ -d "$${SPARSEMAP_REPO:-$$HOME/ws/sparsemap}/.git" ]; then \
		bash test/hegel/run_sparsemap_wire.sh | tail -2; \
	else \
		echo "SKIP check-sparsemap-wire: no sparsemap repo (set SPARSEMAP_REPO)"; \
		echo "  This is the gate that proves re-vendoring does not silently"; \
		echo "  reinterpret blobs already written to disk.  Run it before any"; \
		echo "  sparsemap version change."; \
	fi

# Fuzz/corruption harness: the parse-untrusted-bytes paths under ASan+UBSan.
# On-disk bytes are not trusted, so a corrupt page must produce a clean ERROR and
# never a crash or a wrong answer.  The harness also builds four PLANTED-BUG
# variants and requires each to abort, so a toothless harness fails instead of
# passing vacuously.  Needs clang.
.PHONY: check-fuzz
check-fuzz:
	@if command -v $${CC:-clang} >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then \
		bash test/fuzz/run.sh | tail -3; \
	else \
		echo "SKIP check-fuzz: clang not found (ASan+UBSan harness needs it)"; \
	fi

.PHONY: check-all
check-all: check-ascii check-alloc check-unity check-rename check-standalone check-fuzz
	@echo "== ALL LINT AND STANDALONE GATES PASSED =="
