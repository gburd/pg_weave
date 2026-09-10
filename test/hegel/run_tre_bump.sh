#!/usr/bin/env bash
#
# test/hegel/run_tre_bump.sh -- pg_tre 1521662 (TRE d0e0c997 -> f864ed0)
# regression: builds the vendored TRE library twice -- once from the exact
# pre-bump commit in THIS repo's own history, once from the current working
# tree -- and runs the same small harness against both, proving the two
# fixes actually change behavior on our copy rather than just moving a pin.
#
# Usage:
#   test/hegel/run_tre_bump.sh backref   # fast, default, part of check-standalone
#   test/hegel/run_tre_bump.sh intmax    # ~2 GiB alloc + ~10s, opt-in only
#
# OLD_REF is the last commit before the TRE bump (the vendor bump commit's
# own parent) -- fixed, not relative to HEAD, so this keeps testing the same
# historical bug regardless of how much history piles up on top later.
#
set -euo pipefail
cd "$(dirname "$0")/../.."

MODE=${1:-backref}
OLD_REF=${OLD_REF:-aae7a35}
CHECK_CC=${CHECK_CC:-cc}
WARN_OFF="-Wno-declaration-after-statement -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-missing-prototypes -Wno-sign-compare"
CF="-O2 -DHAVE_CONFIG_H $WARN_OFF"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TRE_LIB_FILES="regcomp.c regerror.c regexec.c tre-ast.c tre-compile.c \
tre-match-approx.c tre-match-backtrack.c tre-match-parallel.c tre-mem.c \
tre-parse.c tre-stack.c"

build_old() {
	mkdir -p "$WORK/old/lib"
	# vendor/tre is checked in with the progress-hook patch ALREADY applied
	# (doc/LICENSING.md) -- these blobs already have the hooks, no patch
	# step needed here.
	for f in $TRE_LIB_FILES; do
		git show "$OLD_REF:vendor/tre/lib/$f" > "$WORK/old/lib/$f"
	done
	for h in tre-ast.h tre-compile.h tre-internal.h tre-match-utils.h \
	         tre-mem.h tre-parse.h tre-stack.h xmalloc.h; do
		git show "$OLD_REF:vendor/tre/lib/$h" > "$WORK/old/lib/$h"
	done
	mkdir -p "$WORK/old-objs"
	for f in $TRE_LIB_FILES; do
		base=$(basename "$f" .c)
		"$CHECK_CC" $CF -Ivendor/tre -I"$WORK/old" -I"$WORK/old/lib" \
			-Ivendor/tre/local_includes \
			-c "$WORK/old/lib/$f" -o "$WORK/old-objs/$base.o"
	done
}

build_new() {
	mkdir -p "$WORK/new-objs"
	for f in $TRE_LIB_FILES; do
		base=$(basename "$f" .c)
		"$CHECK_CC" $CF -Ivendor/tre -Ivendor/tre/lib \
			-Ivendor/tre/local_includes \
			-c "vendor/tre/lib/$f" -o "$WORK/new-objs/$base.o"
	done
}

echo "old TRE = $OLD_REF (pre-1521662), new TRE = working tree (f864ed0)"
build_old
build_new

if [ "$MODE" = backref ]; then
	"$CHECK_CC" -O2 -I"$WORK/old" -I"$WORK/old/lib" -Ivendor/tre/local_includes \
		-DHAVE_CONFIG_H -o "$WORK/backref_old" \
		test/hegel/test_tre_backref.c "$WORK"/old-objs/*.o
	"$CHECK_CC" -O2 -Ivendor/tre -Ivendor/tre/lib -Ivendor/tre/local_includes \
		-DHAVE_CONFIG_H -o "$WORK/backref_new" \
		test/hegel/test_tre_backref.c "$WORK"/new-objs/*.o

	OLD_OUT=$("$WORK/backref_old")
	NEW_OUT=$("$WORK/backref_new")
	echo "old: $OLD_OUT"
	echo "new: $NEW_OUT"

	# Old (d0e0c997): wrong answer -- REG_NOMATCH (rc=1) for a pattern that
	# does match. If this ever reads rc=0, the fixture no longer reproduces
	# the historical bug and this assertion (not the fix) needs review.
	echo "$OLD_OUT" | grep -q '^rc=1 ' || {
		echo "FAIL: expected the pre-bump TRE to get this wrong (rc=1), got: $OLD_OUT" >&2
		exit 1
	}
	# New (f864ed0): right answer -- REG_OK, whole=[1,3), group1=[1,2),
	# exactly upstream's own e0d2777 regression values.
	echo "$NEW_OUT" | grep -q '^rc=0 whole_so=1 whole_eo=3 g1_so=1 g1_eo=2$' || {
		echo "FAIL: expected the bumped TRE to match [1,3)/[1,2), got: $NEW_OUT" >&2
		exit 1
	}
	echo "PASS: backref wrong-answer (pg_tre 2f7dcec) -- wrong before, right after, on our own vendor/tre"

elif [ "$MODE" = intmax ]; then
	"$CHECK_CC" -O2 -I"$WORK/old" -I"$WORK/old/lib" -Ivendor/tre/local_includes \
		-DHAVE_CONFIG_H -o "$WORK/intmax_old" \
		test/hegel/test_tre_intmax_crash.c "$WORK"/old-objs/*.o
	"$CHECK_CC" -O2 -Ivendor/tre -Ivendor/tre/lib -Ivendor/tre/local_includes \
		-DHAVE_CONFIG_H -o "$WORK/intmax_new" \
		test/hegel/test_tre_intmax_crash.c "$WORK"/new-objs/*.o

	set +e
	OLD_OUT=$("$WORK/intmax_old" 2>&1)
	OLD_RC=$?
	set -e
	echo "old: rc=$OLD_RC output=[$OLD_OUT]"
	# Old (d0e0c997): must die on the guard page (SIGSEGV = exit 128+11).
	if [ "$OLD_RC" -lt 128 ]; then
		echo "FAIL: expected the pre-bump TRE to crash on the guard page, exit=$OLD_RC" >&2
		exit 1
	fi

	NEW_OUT=$("$WORK/intmax_new")
	echo "new: $NEW_OUT"
	# New (f864ed0): clamps to TRE_MAX_STRING, stays inside the real
	# buffer, returns REG_NOMATCH (rc=1) cleanly.
	echo "$NEW_OUT" | grep -q '^rc=1$' || {
		echo "FAIL: expected the bumped TRE to return REG_NOMATCH cleanly, got: $NEW_OUT" >&2
		exit 1
	}
	echo "PASS: INT_MAX crash (pg_tre ad26b6d) -- crashed before, clean REG_NOMATCH after, on our own vendor/tre"
else
	echo "usage: $0 [backref|intmax]" >&2
	exit 2
fi
