# Standalone libFuzzer tree for lib_convert. clang-only (it needs the fuzzer
# runtime) -- ../fuzz.sh picks a fuzzer-capable clang and passes it in as
# CMAKE_C_COMPILER. Kept separate from the strict analysis build.
#
# TARGET: this library's own string parsers -- the p101_parse_* family in
# src/integer.c and parse_in_port_t()/convert_address() in src/networking.c.
# Those are the functions that turn UNTRUSTED text (argv, config files, network
# input) into integers and sockaddrs, so they are the right thing to fuzz.
#
# src/*.c is compiled INTO the fuzz binary rather than linked from the installed
# libp101_convert. That is what makes the run coverage-guided: only code built
# with -fsanitize=fuzzer gets the coverage counters libFuzzer steers on (watch
# "cov:" climb). Linking the installed .so instead would leave the parsers as an
# opaque black box.
#
# Unlike a program's harness, there is no main() to rename and no exit() to
# redirect here -- a library has neither.
cmake_minimum_required(VERSION 3.14)
project(p101_convert_fuzz C)

if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing needs clang with libFuzzer (-fsanitize=fuzzer); got '${CMAKE_C_COMPILER_ID}'. Use ./fuzz.sh, which picks a fuzzer-capable clang.")
endif ()

# The dependencies from ../config.cmake (p101_convert_LINK_LIBRARIES).
set(P101_LIBS p101_error p101_env p101_c p101_posix)
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

# -fsanitize=fuzzer pulls in libFuzzer (and its own main); +address,undefined so
# every generated input is also checked for memory errors and UB, which is what
# turns "it did not crash" into "it did not corrupt memory or do anything the
# standard leaves undefined".
set(_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -g -O1)

add_executable(fuzz
        fuzz_convert.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/integer.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/networking.c"
)

target_include_directories(fuzz PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        ${_P101_INC_DIRS}
)
target_compile_options(fuzz PRIVATE ${_FUZZ_FLAGS})
target_link_options(fuzz PRIVATE ${_FUZZ_FLAGS})
target_link_libraries(fuzz PRIVATE ${_P101_RESOLVED} m)
