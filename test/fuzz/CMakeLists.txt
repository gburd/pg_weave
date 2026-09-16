cmake_minimum_required(VERSION 3.14)
project(pg_weave_fuzz C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# The code under test lives two directories up (the extension root); we compile
# the pure-C headers (weave/for.h, weave/docvalid.h) directly into the fuzzer
# binaries -- single source of truth, no duplication.  These headers carry NO
# PostgreSQL dependency, so the fuzzers link nothing but libc.
set(PG_FTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ASan + UBSan: an out-of-bounds read/write or any UB is a hard failure.  The
# harness is designed to be run under these; without them it still runs but is
# far less sensitive.  run.sh forces clang with both on.
add_compile_options(-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

foreach(fuzzer fuzz_for fuzz_docvalid fuzz_block)
    add_executable(${fuzzer} ${fuzzer}.c)
    target_include_directories(${fuzzer} PRIVATE ${PG_FTS_ROOT})
    add_test(NAME ${fuzzer} COMMAND ${fuzzer})
endforeach()

# Planted-bug ("does the harness have teeth?") targets, NOT added as normal
# ctests -- they are EXPECTED to abort under ASan; run.sh asserts they do.
#  - _noclamp: the 0.3.4 count clamp reverted (count>128 overflows gaps[128]).
#  - _signed: the shipped ONE-SIDED clamp (count>INT_MAX -> negative int ->
#    negative n into weave_for_unpack -> wild read; the residual signed-cast gap).
#  - _randstream: a fully-random FOR stream (corrupt width byte -> read past page,
#    the residual gap the bytelen guard does not cross-check).
add_executable(fuzz_block_noclamp fuzz_block.c)
target_include_directories(fuzz_block_noclamp PRIVATE ${PG_FTS_ROOT})
target_compile_definitions(fuzz_block_noclamp PRIVATE FUZZ_NO_CLAMP=1)

add_executable(fuzz_block_signed fuzz_block.c)
target_include_directories(fuzz_block_signed PRIVATE ${PG_FTS_ROOT})
target_compile_definitions(fuzz_block_signed PRIVATE FUZZ_SIGNED_COUNT=1)

add_executable(fuzz_block_randstream fuzz_block.c)
target_include_directories(fuzz_block_randstream PRIVATE ${PG_FTS_ROOT})
target_compile_definitions(fuzz_block_randstream PRIVATE FUZZ_RANDOM_STREAM=1)

# The page-bound arithmetic (include/weave/pagebound.h): the integer-domain guard
# every entry walk and blob read takes its end bound from.  doc/GAPS.md G22.
add_executable(fuzz_pagebound fuzz_pagebound.c)
target_include_directories(fuzz_pagebound PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
add_test(NAME fuzz_pagebound COMMAND fuzz_pagebound)

# Planted-bug variants, again compile-time removals in the REAL header:
#  - _rawsub: the pre-G22 `avail = pd_lower - contents_offset`, unsigned and
#    unguarded, which underflows to ~2^64 and reads past the page.
#  - _nolow: only the `lower > blcksz` half of the bound, which is how the bug
#    actually gets written.  Both are EXPECTED to abort; run.sh asserts they do.
add_executable(fuzz_pagebound_rawsub fuzz_pagebound.c)
target_include_directories(fuzz_pagebound_rawsub PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
target_compile_definitions(fuzz_pagebound_rawsub PRIVATE WEAVE_PAGEBOUND_PLANT_RAW_SUB=1)

add_executable(fuzz_pagebound_nolow fuzz_pagebound.c)
target_include_directories(fuzz_pagebound_nolow PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
target_compile_definitions(fuzz_pagebound_nolow PRIVATE WEAVE_PAGEBOUND_PLANT_NO_LOW_GUARD=1)

# The Z3 surf vocabulary trie (include/weave/surftrie.h + src/query/surftrie.c).
# The only fuzz target with a companion .c: the builder and reader are too big to
# live in a header, so the target links the REAL implementation rather than a
# transcription.  run.sh is the authoritative runner; this is here so `ctest`
# picks the target up too.
add_executable(fuzz_surftrie fuzz_surftrie.c ${PG_FTS_ROOT}/src/query/surftrie.c)
target_include_directories(fuzz_surftrie PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
add_test(NAME fuzz_surftrie COMMAND fuzz_surftrie)

# Planted-bug variants.  These are compile-time removals in the REAL validator,
# not a weakened copy of it, because a weakened copy drifts out of step with the
# code it is supposed to model.  Both are EXPECTED to abort.
#  - _nosize: no "length must equal what the counts imply" guard, so a corrupt
#    count puts a whole section past the buffer (ASan).
#  - _nosel: no select-sample recomputation, so a corrupt sample is a wild slot
#    index -- a WRONG ANSWER first, which the harness's independent brute-force
#    postcondition catches, and an out-of-bounds read second.
add_executable(fuzz_surftrie_nosize fuzz_surftrie.c ${PG_FTS_ROOT}/src/query/surftrie.c)
target_include_directories(fuzz_surftrie_nosize PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
target_compile_definitions(fuzz_surftrie_nosize PRIVATE WEAVE_SURF_PLANT_NO_SIZE_GUARD=1)

add_executable(fuzz_surftrie_nosel fuzz_surftrie.c ${PG_FTS_ROOT}/src/query/surftrie.c)
target_include_directories(fuzz_surftrie_nosel PRIVATE ${PG_FTS_ROOT} ${PG_FTS_ROOT}/include)
target_compile_definitions(fuzz_surftrie_nosel PRIVATE WEAVE_SURF_PLANT_NO_SELECT_GUARD=1)
