# Standalone Unity test tree for lib_convert.
#
# Deliberately SEPARATE from the library's own CMakeLists.txt so that vendored
# Unity and the test code are NOT put through the project's strict
# clang-format / clang-tidy / cppcheck / Clang-Static-Analyzer / -Werror
# pipeline. Run it with ../test.sh, which configures this tree with the SAME
# compiler the main build is currently using.
#
# The code under test is compiled straight from ../src, NOT linked from an
# installed libp101_convert.  That matters: it means ./test.sh exercises the
# working tree you just edited, with no install.sh round-trip, and it is what
# makes --coverage able to attribute lines back to src/*.c.
#
# lib_convert's own dependencies (p101_error, p101_env, p101_c,  --
# see ../config.cmake p101_convert_LINK_LIBRARIES) ARE linked from their
# installed locations, so those must be installed first (../../setup.sh, or
# ./install.sh in each of those repos).
cmake_minimum_required(VERSION 3.14)
project(p101_convert_tests C CXX)
enable_testing()

set(P101_API_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/../api-manifest.tsv")
set(P101_UNIT_TEST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/unit-test-manifest.tsv")
if(NOT EXISTS "${P101_API_MANIFEST}" OR NOT EXISTS "${P101_UNIT_TEST_MANIFEST}")
    message(FATAL_ERROR "Public API and unit-test manifests are required")
endif()
file(STRINGS "${P101_API_MANIFEST}" P101_API_ROWS REGEX "^p101_")
file(STRINGS "${P101_UNIT_TEST_MANIFEST}" P101_UNIT_ROWS REGEX "^p101_")
set(P101_API_FUNCTIONS "")
set(P101_UNIT_FUNCTIONS "")
foreach(row IN LISTS P101_API_ROWS)
    string(REGEX MATCH "^p101_[A-Za-z0-9_]+" function_name "${row}")
    list(APPEND P101_API_FUNCTIONS "${function_name}")
endforeach()
foreach(row IN LISTS P101_UNIT_ROWS)
    string(REGEX MATCH "^p101_[A-Za-z0-9_]+" function_name "${row}")
    list(APPEND P101_UNIT_FUNCTIONS "${function_name}")
endforeach()
set(P101_UNIT_FUNCTIONS_UNIQUE ${P101_UNIT_FUNCTIONS})
list(REMOVE_DUPLICATES P101_UNIT_FUNCTIONS_UNIQUE)
list(LENGTH P101_API_FUNCTIONS P101_API_COUNT)
list(LENGTH P101_UNIT_FUNCTIONS P101_UNIT_COUNT)
list(LENGTH P101_UNIT_FUNCTIONS_UNIQUE P101_UNIT_UNIQUE_COUNT)
if(NOT P101_API_COUNT EQUAL P101_UNIT_COUNT OR NOT P101_UNIT_COUNT EQUAL P101_UNIT_UNIQUE_COUNT)
    message(FATAL_ERROR "Every public API must have exactly one unit-test row")
endif()
foreach(function_name IN LISTS P101_API_FUNCTIONS)
    if(NOT function_name IN_LIST P101_UNIT_FUNCTIONS_UNIQUE)
        message(FATAL_ERROR "Missing unit test for ${function_name}")
    endif()
endforeach()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/unity")
target_compile_options(unity PRIVATE -w)

option(P101_TEST_COVERAGE "Instrument the code under test for coverage" OFF)
set(_COV "")
if (P101_TEST_COVERAGE)
    set(_COV --coverage)
endif ()

# The dependencies from ../config.cmake (p101_convert_LINK_LIBRARIES).
set(P101_LIBS p101_error p101_env p101_tool_event p101_c p101_network)
set(P101_PUBLIC_INCLUDE_DIRS "" CACHE STRING "Extra p101 include dirs")
set(P101_PUBLIC_LINK_DIRS "" CACHE STRING "Extra p101 link dirs")
separate_arguments(P101_PUBLIC_INCLUDE_DIRS_LIST NATIVE_COMMAND "${P101_PUBLIC_INCLUDE_DIRS}")
separate_arguments(P101_PUBLIC_LINK_DIRS_LIST NATIVE_COMMAND "${P101_PUBLIC_LINK_DIRS}")
set(_P101_INC_DIRS ${P101_PUBLIC_INCLUDE_DIRS_LIST} /usr/local/include /opt/homebrew/include /opt/local/include)
set(_P101_LIB_DIRS ${P101_PUBLIC_LINK_DIRS_LIST} /usr/local/lib /usr/local/lib64 /opt/homebrew/lib /opt/local/lib)

set(_P101_RESOLVED "")
foreach (_l IN LISTS P101_LIBS)
    unset(_P101_LIB_${_l} CACHE)
    unset(_P101_LIB_${_l})
    find_library(_P101_LIB_${_l} NAMES ${_l} PATHS ${_P101_LIB_DIRS} NO_DEFAULT_PATH)
    if (NOT _P101_LIB_${_l})
        find_library(_P101_LIB_${_l} NAMES ${_l})
    endif ()
    if (_P101_LIB_${_l})
        list(APPEND _P101_RESOLVED "${_P101_LIB_${_l}}")
    else ()
        message(WARNING "p101 library '${_l}' not found in ${_P101_LIB_DIRS} -- install the p101 libs (../../setup.sh) or add the path.")
    endif ()
endforeach ()

# This library's own sources, compiled INTO each test binary.
set(P101_CODE_UNDER_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/integer.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/networking.c"
)

# p101_add_test(<name> <test sources...>) -- the code under test is added for you.
function(p101_add_test name)
    add_executable(${name} ${ARGN} ${P101_CODE_UNDER_TEST})
    target_include_directories(${name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../include"
            ${_P101_INC_DIRS}
    )
    target_link_libraries(${name} PRIVATE unity ${_P101_RESOLVED} m)
    target_compile_options(${name} PRIVATE ${_COV})
    target_link_options(${name} PRIVATE ${_COV})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

p101_add_test(test_integer test_integer.c)
p101_add_test(test_networking test_networking.c)
include(${CMAKE_CURRENT_SOURCE_DIR}/fault_shards.cmake)
foreach(p101_fault_shard IN LISTS P101_FAULT_SHARD_TESTS)
    p101_add_test(${p101_fault_shard} ${p101_fault_shard}.c)
endforeach()
p101_add_test(test_cpp_linkage test_cpp_linkage.cpp)
