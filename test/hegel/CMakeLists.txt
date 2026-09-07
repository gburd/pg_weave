cmake_minimum_required(VERSION 3.14)
project(pg_weave_hegel_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    hegel
    GIT_REPOSITORY https://github.com/gburd/hegel-c.git
    GIT_TAG main
)
FetchContent_MakeAvailable(hegel)

find_package(cmocka CONFIG QUIET)
if(NOT cmocka_FOUND)
    find_library(CMOCKA_LIBRARY cmocka)
    find_path(CMOCKA_INCLUDE_DIR cmocka.h)
    if(NOT CMOCKA_LIBRARY OR NOT CMOCKA_INCLUDE_DIR)
        message(FATAL_ERROR "cmocka not found (install libcmocka-dev / cmocka)")
    endif()
    add_library(cmocka::cmocka UNKNOWN IMPORTED)
    set_target_properties(cmocka::cmocka PROPERTIES
        IMPORTED_LOCATION "${CMOCKA_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMOCKA_INCLUDE_DIR}")
endif()

enable_testing()

# The code under test lives one directory up (the extension root); we compile
# the pure-C codec header and the pure Levenshtein .c directly into the test
# binaries -- single source of truth, no duplication.
set(PG_FTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_for test_for.c)
target_include_directories(test_for PRIVATE ${PG_FTS_ROOT})
target_link_libraries(test_for PRIVATE hegel cmocka::cmocka)
add_test(NAME test_for COMMAND test_for)

add_executable(test_lev test_lev.c)
target_include_directories(test_lev PRIVATE ${PG_FTS_ROOT})
target_link_libraries(test_lev PRIVATE hegel cmocka::cmocka)
add_test(NAME test_lev COMMAND test_lev)

add_executable(test_for_props test_for_props.c)
target_include_directories(test_for_props PRIVATE ${PG_FTS_ROOT})
target_link_libraries(test_for_props PRIVATE hegel cmocka::cmocka)
add_test(NAME test_for_props COMMAND test_for_props)

add_executable(test_docvalid test_docvalid.c)
target_include_directories(test_docvalid PRIVATE ${PG_FTS_ROOT})
target_link_libraries(test_docvalid PRIVATE hegel cmocka::cmocka)
add_test(NAME test_docvalid COMMAND test_docvalid)
