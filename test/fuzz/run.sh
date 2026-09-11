#!/usr/bin/env bash
#
# run.sh -- build and run the pg_weave fuzz/corruption harness under clang
# AddressSanitizer + UndefinedBehaviorSanitizer.
#
# Exit 0  = all fuzzers ran clean (no overflow, no UB): the parse-untrusted-
#           bytes functions never crash on any input.
# Exit !0 = a fuzzer detected an overflow/UB, OR the planted-bug check failed
#           to detect a reverted 0.3.4 clamp (i.e. the harness is toothless).
#
# No CMake required: this compiles the three self-contained fuzzers directly.
# The CI-wiring agent can invoke this as-is (see test/fuzz/README.md).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CC="${CC:-clang}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
CFLAGS="-std=c99 -g -O1 -Wall -Wextra -I$root -I$root/include $SAN"

export ASAN_OPTIONS="abort_on_error=1:detect_leaks=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo "== building fuzzers ($CC, ASan+UBSan) =="
for f in fuzz_for fuzz_docvalid fuzz_block fuzz_chandesc; do
    $CC $CFLAGS "$here/$f.c" -o "$out/$f"
done
# fuzz_surftrie is the one fuzzer with a companion .c: the Z3 trie's builder and
# reader are too big to live in a header, so the target links the REAL
# src/query/surftrie.c (no transcription, no modeling gap).
$CC $CFLAGS "$here/fuzz_surftrie.c" "$root/src/query/surftrie.c" -o "$out/fuzz_surftrie"
# planted-bug binaries: fuzz_block with (a) the count clamp reverted, (b) the
# shipped ONE-SIDED clamp (misses count>INT_MAX -> negative int -> wild read),
# and (c) a fully-random FOR stream (corrupt width -> read past page).  All MUST
# abort under ASan.
$CC $CFLAGS -DFUZZ_NO_CLAMP=1 "$here/fuzz_block.c" -o "$out/fuzz_block_noclamp"
$CC $CFLAGS -DFUZZ_SIGNED_COUNT=1 "$here/fuzz_block.c" -o "$out/fuzz_block_signed"
$CC $CFLAGS -DFUZZ_RANDOM_STREAM=1 "$here/fuzz_block.c" -o "$out/fuzz_block_randstream"
$CC $CFLAGS -DFUZZ_NO_SUMTF_GUARD=1 "$here/fuzz_block.c" -o "$out/fuzz_block_nosumtf"
# planted-bug binary for the v6 channel-descriptor decoder: the "descriptor array
# must fit the readable bytes" guard removed.  MUST abort under ASan.
$CC $CFLAGS -DFUZZ_NO_ARRAY_GUARD=1 "$here/fuzz_chandesc.c" -o "$out/fuzz_chandesc_noarray"
# planted-bug binaries for the Z3 surf trie.  Unlike the ones above these are
# compile-time removals in the REAL validator (-DWEAVE_SURF_PLANT_*) rather than a
# weakened transcription of it, because a transcribed copy drifts out of step with
# the code it models and then proves nothing.  Both MUST abort.
$CC $CFLAGS -DWEAVE_SURF_PLANT_NO_SIZE_GUARD=1 "$here/fuzz_surftrie.c" \
    "$root/src/query/surftrie.c" -o "$out/fuzz_surftrie_nosize"
$CC $CFLAGS -DWEAVE_SURF_PLANT_NO_SELECT_GUARD=1 "$here/fuzz_surftrie.c" \
    "$root/src/query/surftrie.c" -o "$out/fuzz_surftrie_nosel"

echo "== running fuzzers =="
rc=0
for f in fuzz_for fuzz_docvalid fuzz_block fuzz_chandesc fuzz_surftrie; do
    if "$out/$f"; then
        echo "PASS: $f"
    else
        echo "FAIL: $f (sanitizer or assert fired)"
        rc=1
    fi
done

echo "== planted-bug checks: teeth builds MUST abort under ASan =="
# The no-clamp build feeds count>128 into a 128-array; ASan must abort it.
# We expect a NON-zero exit; a zero exit means the harness did not catch the
# reverted-clamp overflow -> the harness is toothless -> fail the whole run.
if "$out/fuzz_block_noclamp" >/dev/null 2>&1; then
    echo "FAIL: fuzz_block_noclamp exited 0 -- harness did NOT catch the reverted clamp!"
    rc=1
else
    echo "PASS: fuzz_block_noclamp aborted as expected (count-clamp teeth)"
fi

# The signed-count build uses the shipped one-sided clamp; count>INT_MAX casts
# negative and drives weave_for_unpack with a negative n -> wild read.  ASan must
# abort -- proving the harness detects the residual signed-cast gap.
if "$out/fuzz_block_signed" >/dev/null 2>&1; then
    echo "FAIL: fuzz_block_signed exited 0 -- harness did NOT catch the signed-count wild read!"
    rc=1
else
    echo "PASS: fuzz_block_signed aborted as expected (signed-count teeth)"
fi

# The random-stream build feeds a corrupt WIDTH byte; weave_for_unpack then reads
# past bytelen/page.  ASan must abort it -- proving the harness detects the
# residual corrupt-width read (a gap the 0.3.4 guards leave open).
if "$out/fuzz_block_randstream" >/dev/null 2>&1; then
    echo "FAIL: fuzz_block_randstream exited 0 -- harness did NOT catch the corrupt-width read!"
    rc=1
else
    echo "PASS: fuzz_block_randstream aborted as expected (corrupt-width teeth)"
fi

# The no-sumtf-guard build reverts the 1.0.1 positions-decode: an inflated tfs[]
# makes Sum(tf) exceed what posbytelen encodes, so weave_for_unpack reads past the
# block -> ASan OOB (and the real code would over-alloc past MaxAllocSize).  ASan
# must abort -- proving the harness detects the 1.0.1 read-path crash class.
if "$out/fuzz_block_nosumtf" >/dev/null 2>&1; then
    echo "FAIL: fuzz_block_nosumtf exited 0 -- harness did NOT catch the inflated-sumtf read!"
    rc=1
else
    echo "PASS: fuzz_block_nosumtf aborted as expected (sumtf-vs-posbytelen teeth)"
fi

# The chandesc no-array-guard build lets a corrupt nweft walk the descriptor
# array past the buffer; ASan must abort it -- proving the harness detects the
# missing-length-guard class on the v6 channel-descriptor page.
if "$out/fuzz_chandesc_noarray" >/dev/null 2>&1; then
    echo "FAIL: fuzz_chandesc_noarray exited 0 -- harness did NOT catch the missing array guard!"
    rc=1
else
    echo "PASS: fuzz_chandesc_noarray aborted as expected (descriptor-array teeth)"
fi

# The surf-trie no-size-guard build lets a corrupt count put a whole section past
# the buffer; ASan must abort it -- proving the harness detects the
# missing-length-guard class on the Z3 trie image.
if "$out/fuzz_surftrie_nosize" >/dev/null 2>&1; then
    echo "FAIL: fuzz_surftrie_nosize exited 0 -- harness did NOT catch the missing size guard!"
    rc=1
else
    echo "PASS: fuzz_surftrie_nosize aborted as expected (image-size teeth)"
fi

# The no-select-guard build trusts a corrupt select sample, which is a WRONG
# ANSWER (navigation to a node that is not there) before it is an out-of-bounds
# read.  The harness's independent brute-force postcondition is what fires first;
# either that assert or ASan must abort -- a wrong answer is exactly what a
# sanitizer alone cannot see.
if "$out/fuzz_surftrie_nosel" >/dev/null 2>&1; then
    echo "FAIL: fuzz_surftrie_nosel exited 0 -- harness did NOT catch the unvalidated select sample!"
    rc=1
else
    echo "PASS: fuzz_surftrie_nosel aborted as expected (select-sample teeth)"
fi

if [ "$rc" -eq 0 ]; then
    echo "== ALL CLEAN =="
else
    echo "== FAILURES ABOVE =="
fi
exit "$rc"
