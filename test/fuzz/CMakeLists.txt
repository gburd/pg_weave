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
