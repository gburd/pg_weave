# pg_weave -- standalone PGXS build
#
# Build against an installed PostgreSQL (>= 17):
#     make PG_CONFIG=/path/to/pg_config
#     make install PG_CONFIG=/path/to/pg_config
#     make installcheck PG_CONFIG=/path/to/pg_config   # regression + isolation + TAP
#
# PG_CONFIG defaults to whatever `pg_config` is on PATH.

MODULE_big = pg_weave

# Every translation unit, compiled once each.  Until task L1 src/am/am.c was a
# unity build that #included src/am/amscan.c, src/query/lev.c and
# src/pages/trgm_page.c as text, and those three had to be kept OUT of this list
# (a `make check-unity` target guarded it, and AGENTS.md carried a hard rule).
# L1 split am.c into am.c/ambuild.c/amvacuum.c/amscan.c with the interface
# declared in include/weave/am.h, so there is nothing special left here.
OBJS = \
	$(WIN32RES) \
	src/query/analyze.o \
	src/query/tsanalyze.o \
	src/query/doc.o \
	src/query/parse.o \
	src/query/rank.o \
	src/query/lev.o \
	src/am/am.o \
	src/am/ambuild.o \
	src/am/amvacuum.o \
	src/am/amscan.o \
	src/am/fuse.o \
	src/am/fuseshuttle.o \
	src/am/vecdocmap.o \
	src/am/fusepath.o \
	src/am/customscan.o \
	src/am/amaux.o \
	src/am/amsize.o \
	src/am/amcheck.o \
	src/pages/trgm_page.o \
	src/util/migrate.o \
	src/query/trgm.o \
	src/query/cgram.o \
	src/util/sparsemap.o \
	src/query/match.o \
	src/query/gate.o \
	src/query/edist.o \
	src/query/lexshuttle.o \
	src/vector/quantize.o \
	src/vector/pack.o \
	src/vector/vecpage.o \
	src/vector/vecweft.o \
	src/vector/vecwrite.o \
	src/vector/vecstats.o \
	src/vector/vecscan.o \
	src/vector/vecshuttle.o \
	src/vector/kernels.o \
	src/vector/kernel_ops.o \
	src/vector/wvec.o \
	$(FUZZY_OBJS) \
	$(TRE_OBJS)

# --- Fuzzy/regex/prefix channel (imported from pg_tre; see
# doc/specs/IMPORT_pg_tre.md).  Wired into the build by task Z1/Z2; the channel
# is not yet reachable from the access method or the planner (Z3 onward), so
# these compile and link but nothing calls them yet.
FUZZY_OBJS = \
	src/query/fuzzy_guc.o \
	src/query/hash.o \
	src/query/surf.o \
	src/query/surftrie.o \
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

# --- Vendored TRE (laurikari/tre f864ed0, BSD-2; see doc/LICENSING.md).
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
DATA = sql/pg_weave--0.1.0.sql sql/pg_weave--0.1.0--0.2.0.sql sql/pg_weave--0.2.0--0.3.0.sql sql/pg_weave--0.3.0--0.4.0.sql sql/pg_weave--0.4.0--0.5.0.sql sql/pg_weave--0.5.0--0.6.0.sql sql/pg_weave--0.6.0--0.7.0.sql sql/pg_weave--0.7.0--0.8.0.sql sql/pg_weave--0.8.0--0.9.0.sql sql/pg_weave--0.9.0--0.10.0.sql sql/pg_weave--0.10.0--0.11.0.sql sql/pg_weave--0.11.0--0.12.0.sql sql/pg_weave--0.12.0--0.13.0.sql sql/pg_weave--0.13.0--0.14.0.sql sql/pg_weave--0.14.0--0.15.0.sql sql/pg_weave--0.15.0--0.16.0.sql sql/pg_weave--0.16.0--0.17.0.sql sql/pg_weave--0.17.0--0.18.0.sql sql/pg_weave--0.18.0--0.19.0.sql
PGFILEDESC = "pg_weave - unified lexical + vector + fuzzy retrieval in one index"

# sql/ and expected/ are already at the top level (PGXS's built-in default
# --inputdir=$(srcdir) for pg_regress), so plain REGRESS with no REGRESS_OPTS
# picks up sql/<name>.sql + expected/<name>.out directly. No relayout fix
# needed here.
REGRESS = weave unicode_fold idx_scan_stats wvec orderby chandesc surf vecindex vecscan vecorderby pendingvec chanstats fuzzyuleven regexdict edist cgram fuse_fallback fuse_degenerate fuse_pushdown

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

# PGXS ships an implicit `%.c: %.y` rule that runs bison, and both the Lime
# grammar and its generated output are committed.  `git checkout` writes them in
# index order with near-identical mtimes, so whether make thinks the .c is stale
# comes down to nanoseconds: equal mtimes mean "up to date", and one nanosecond
# of skew means bison is handed a Lime grammar, chokes on `%syntax_error`, and
# takes the build with it.  That is how CI's `sanitize` leg failed while
# `standalone` and both `test` legs passed on the very same commit, and it is
# what the `touch src/query/regex_grammar.c` folklore for fresh worktrees was
# working around.  An explicit rule with an empty recipe cancels implicit-rule
# search for this target, which is the documented way to say "this file is a
# source, not a derived file" (GNU make, "Using Empty Recipes").
src/query/regex_grammar.c: ;

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

# --- pd_lower read-guard lint ------------------------------------------------
# There must be exactly ONE reader of pd_lower in this codebase:
# weave_page_entry_end() in include/weave/am.h, which validates in the integer
# domain and returns an empty range for anything implausible. Every other
# occurrence must be an assignment (`pd_lower =` or `pd_lower +=`) on a page the
# writer owns.
#
# Why a lint and not a code review: pg_fts 1.7.0 fixed ONE unvalidated
# `page + pd_lower` in the merge's dictionary walk, where the bad value made the
# merge request an impossible allocation and left the index permanently
# unvacuumable. 1.7.1 then found EIGHT more sibling sites and routed them all
# through one helper, with the retrospective "I should have grepped the siblings
# then". This repository had the helper first and still had four stragglers,
# including one that was a genuine out-of-bounds READ: the trigram blob reader
# computed `avail = pd_lower - contents_offset` as an unsigned Size, so a torn
# page reporting pd_lower BELOW the contents offset underflowed to a huge value,
# and the Min() against the remaining blob length then clamped it to a length
# spanning the whole page CHAIN. It could not overrun the destination buffer,
# which is exactly why it read safe.
#
# Forming the pointer at all is undefined behaviour for an absurd value -- before
# any dereference -- so `ptr < (char *) page + pd_lower` is already UB and a
# sanitizer build will say so.
.PHONY: check-pdlower
check-pdlower:
	@bad=$$(grep -rn 'pd_lower' --include='*.c' --include='*.h' src/ include/ \
		| grep -v '^include/weave/am.h:' \
		| awk '{ line = $$0; sub(/^[^:]*:[0-9]*:/, "", line); \
			 if (line ~ /^[ \t]*(\*|\/\*|\/\/)/) next; \
			 if (line ~ /pd_lower[ \t]*\+?=/) next; \
			 print }'); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: pd_lower read outside weave_page_entry_end() (include/weave/am.h):" >&2; \
		echo "$$bad" >&2; \
		exit 1; \
	fi; \
	echo "pd_lower has exactly one validated reader"

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
#
# The ban on the bare upstream project name under src/ and include/ is absolute
# on purpose, and it costs something: a ported fix cannot cite its upstream SHA
# in the code comment. That citation goes in the commit message and in the
# bench/RESULTS_*.md or doc/specs/ page the comment points at, which is where
# this project keeps provenance anyway (doc/LICENSING.md,
# doc/specs/IMPORT_pg_tre.md). Discovered while porting the merge tombstone P0:
# three prose citations tripped the lint, and loosening it to recognize prose
# would have meant distinguishing a comment from a string literal in a shell
# grep, which is how a guard stops guarding.
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
#
# EVERY TEST IS RUN TO A LOG AND ITS OWN EXIT STATUS IS CHECKED, and the reason is
# that the obvious spelling -- `$$tmp/pk | tail -1` -- reports on `tail`.  A
# pipeline's status is the LAST command's, so `set -e` sees tail succeed no matter
# what the test did: sixteen of these eighteen suites were invoked that way, which
# means `make check-standalone` printed "ALL STANDALONE CHECKS PASSED" over an
# aborting test for as long as the target has existed.  It was found on 2026-09-22
# when test_pagekind started failing an assertion and the gate stayed green; see
# doc/GAPS.md G42.  This is the eighth and ninth member of the AGENTS.md family of
# "a result needs evidence that the specific thing you meant to run, ran" -- the
# same `| tail` / `| head` / `test -f` mistake, this time in the gate that fronts
# every property test in the project.
CHECK_CC ?= cc
STANDALONE_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -I include

.PHONY: check-standalone
check-standalone:
	@set -e; \
	tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	echo "== FOR codec: fast extractor vs the bit-by-bit implementation it replaced =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/for test/hegel/test_for_get.c -lm; \
	$$tmp/for > $$tmp/for.log 2>&1 || { cat $$tmp/for.log; exit 1; }; \
	tail -1 $$tmp/for.log; \
	echo "== vector quantizer: rotation, codebook, encode, packing, block bound (C2) =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/q test/hegel/test_quantize.c \
		src/vector/quantize.c src/vector/pack.c -lm; \
	$$tmp/q > $$tmp/q.log 2>&1 || { cat $$tmp/q.log; exit 1; }; \
	tail -1 $$tmp/q.log; \
	echo "== vector kernels: every available ISA path == the scalar oracle, bit for bit =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/k test/hegel/test_kernels.c \
		src/vector/kernels.c src/vector/quantize.c src/vector/pack.c -lm; \
	$$tmp/k > $$tmp/k.log || { cat $$tmp/k.log; exit 1; }; \
	sed -n '1p;$$p' $$tmp/k.log; \
	echo "== v5 doclen sidecar: random access over absolute offsets == gap decode =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/dlb test/hegel/test_doclen_block.c -lm; \
	$$tmp/dlb > $$tmp/dlb.log 2>&1 || { cat $$tmp/dlb.log; exit 1; }; \
	tail -1 $$tmp/dlb.log; \
	echo "== v6 page-kind space: encode/decode bijection + fail-closed vs a v5 reader =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/pk test/hegel/test_pagekind.c; \
	$$tmp/pk > $$tmp/pk.log 2>&1 || { cat $$tmp/pk.log; exit 1; }; \
	tail -1 $$tmp/pk.log; \
	echo "== v6 channel descriptor: pure validator on well-formed and corrupt images =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/cd test/fuzz/fuzz_chandesc.c; \
	$$tmp/cd > $$tmp/cd.log 2>&1 || { cat $$tmp/cd.log; exit 1; }; \
	tail -1 $$tmp/cd.log; \
	echo "== page-bound guard (G22): end offset in range, avail never underflows =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/pb test/fuzz/fuzz_pagebound.c; \
	$$tmp/pb > $$tmp/pb.log 2>&1 || { cat $$tmp/pb.log; exit 1; }; \
	tail -1 $$tmp/pb.log; \
	echo "== V5 32-lane packing: round-trip, lane isolation, move_lane/zero_lane, bounds =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/pack test/hegel/test_pack.c src/vector/pack.c; \
	$$tmp/pack > $$tmp/pack.log 2>&1 || { cat $$tmp/pack.log; exit 1; }; \
	tail -1 $$tmp/pack.log; \
	echo "== V7 code strips: coordinate slicing, page-image determinism, refusals =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/vecpage test/hegel/test_vecpage.c \
		src/vector/vecpage.c src/vector/pack.c; \
	$$tmp/vecpage > $$tmp/vecpage.log 2>&1 || { cat $$tmp/vecpage.log; exit 1; }; \
	tail -1 $$tmp/vecpage.log; \
	echo "== V7 vector weft: partition, round trip, statistics recompute, the merge MOVE =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/vecweft test/hegel/test_vecweft.c \
		src/vector/vecweft.c src/vector/vecpage.c src/vector/vecstats.c \
		src/vector/quantize.c src/vector/pack.c -lm; \
	$$tmp/vecweft > $$tmp/vecweft.log 2>&1 || { cat $$tmp/vecweft.log; exit 1; }; \
	tail -1 $$tmp/vecweft.log; \
	echo "== V7/V8 contract (C2): the block bound is an upper bound on every lane =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/vecbound test/hegel/test_vecbound.c \
		src/vector/vecstats.c src/vector/quantize.c src/vector/pack.c \
		src/vector/kernels.c -lm; \
	$$tmp/vecbound > $$tmp/vecbound.log 2>&1 || { cat $$tmp/vecbound.log; exit 1; }; \
	tail -1 $$tmp/vecbound.log; \
	echo "== V8 code-scan core: (C1) ascent, (C2) in the METRIC'S domain, mask skips =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/vecscan test/hegel/test_vecscan.c \
		src/vector/vecscan.c src/vector/vecweft.c src/vector/vecpage.c \
		src/vector/vecstats.c src/vector/kernels.c src/vector/quantize.c \
		src/vector/pack.c -lm; \
	$$tmp/vecscan > $$tmp/vecscan.log 2>&1 || { cat $$tmp/vecscan.log; exit 1; }; \
	tail -2 $$tmp/vecscan.log; \
	echo "== Z3 surf trie: trie membership == dictionary membership, prefix enumeration =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/surf test/hegel/test_surf.c \
		src/query/surftrie.c; \
	$$tmp/surf > $$tmp/surf.log 2>&1 || { cat $$tmp/surf.log; exit 1; }; \
	tail -1 $$tmp/surf.log; \
	echo "== Z5 uleven: exact vocabulary neighbourhood, skip soundness, char units =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/ul test/hegel/test_uleven.c -lm; \
	$$tmp/ul > $$tmp/ul.log 2>&1 || { cat $$tmp/ul.log; exit 1; }; \
	tail -1 $$tmp/ul.log; \
	echo "== Z7 gate shuttle (C1)+(C2)+(C5): monotone seek == linear oracle, +/-INF, backward refused =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/bounds test/hegel/test_bounds.c; \
	$$tmp/bounds > $$tmp/bounds.log 2>&1 || { cat $$tmp/bounds.log; exit 1; }; \
	tail -1 $$tmp/bounds.log; \
	echo "== F1/F5 fused top-k: fused == brute force, conjunctive gates, no backward seek =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/fuse test/hegel/test_fuse_props.c \
		src/am/fuse.c -lm; \
	$$tmp/fuse 60000 > $$tmp/fuse.log 2>&1 || { cat $$tmp/fuse.log; exit 1; }; \
	tail -4 $$tmp/fuse.log; \
	echo "== F6 lexical bound (C2): block_bound >= BM25 contribution, attained at (max_tf, min |D|) =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/lexb test/hegel/test_lexbound.c -lm; \
	$$tmp/lexb > $$tmp/lexb.log || { cat $$tmp/lexb.log; exit 1; }; \
	tail -3 $$tmp/lexb.log; \
	echo "== F8 vecdocmap (C1)+(C2): lane-to-docid relabelling, unclamped-blkend and flipped-comparator controls =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/vecdocmap test/hegel/test_vecdocmap.c \
		src/am/vecdocmap.c src/am/fuse.c -lm; \
	$$tmp/vecdocmap 4000 > $$tmp/vecdocmap.log || { cat $$tmp/vecdocmap.log; exit 1; }; \
	tail -12 $$tmp/vecdocmap.log; \
	echo "== Z9 edist shuttle (C1)+(C2): the bound never exceeds a true Levenshtein distance =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/edist test/hegel/test_edist.c -lm; \
	$$tmp/edist > $$tmp/edist.log 2>&1 || { cat $$tmp/edist.log; exit 1; }; \
	tail -2 $$tmp/edist.log; \
	echo "== TRE d0e0c997 -> f864ed0 (pg_tre 1521662): backref wrong-answer fix =="; \
	bash test/hegel/run_tre_bump.sh backref > $$tmp/tre.log 2>&1 || { cat $$tmp/tre.log; exit 1; }; \
	tail -1 $$tmp/tre.log; \
	echo "== pg_tre 2be8dbf (v3.2.5): literal '-' first/last in a bracket expression =="; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -I test/hegel/pgshim_regex -I src/query \
		-o $$tmp/rxdash test/hegel/test_regex_dash.c src/query/regex_tokens.c; \
	$$tmp/rxdash > $$tmp/rxdash.log 2>&1 || { cat $$tmp/rxdash.log; exit 1; }; \
	tail -1 $$tmp/rxdash.log; \
	echo "== ALL STANDALONE CHECKS PASSED =="

# The INT_MAX crash fix (pg_tre 1521662 / upstream ad26b6d) needs an actual
# buffer bigger than INT_MAX bytes (~2 GiB) plus a guard page to reproduce
# deterministically -- ~10s and ~2 GiB resident, not appropriate for the
# default gate every `make check-standalone` run hits. Separate target, same
# "skip loudly, do not fail CI silently" spirit as check-sparsemap-wire, but
# this one has no external dependency to be missing, so it always runs when
# invoked; it is just not invoked by check-standalone or check-all.
# Task Z9's mandatory measurement (doc/specs/FUZZY_CHANNEL.md sect. 5: tightness
# "must be measured before this is called done").  Not part of check-standalone:
# it is a BENCHMARK, and a benchmark in a build gate is a number nobody reads.
# It needs no server -- the bound is header-only -- so it is a plain compile.
# Results: bench/RESULTS_EDIST_BOUND.md.
.PHONY: bench-edist-bound
bench-edist-bound:
	@set -e; \
	tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CHECK_CC) $(STANDALONE_CFLAGS) -o $$tmp/eb bench/edist_bound.c -lm; \
	$$tmp/eb 0; \
	$$tmp/eb 1

.PHONY: check-tre-bump-intmax
check-tre-bump-intmax:
	@bash test/hegel/run_tre_bump.sh intmax

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
check-all: check-ascii check-alloc check-pdlower check-rename check-standalone check-fuzz
	@echo "== ALL LINT AND STANDALONE GATES PASSED =="
