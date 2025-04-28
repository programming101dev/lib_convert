# Project metadata
set(PROJECT_NAME "p101_convert")
set(PROJECT_VERSION "0.0.1")
set(PROJECT_DESCRIPTION "Integer and networking conversion utilities")
set(PROJECT_LANGUAGE "C")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Common compiler flags
set(STANDARD_FLAGS
        -D_POSIX_C_SOURCE=200809L
        -D_XOPEN_SOURCE=700
        #-D_GNU_SOURCE
        #-D_DARWIN_C_SOURCE
        #-D__BSD_VISIBLE
        -Werror
)

# Define library targets
set(LIBRARY_TARGETS p101_convert)

# Source files for the library
set(p101_convert_SOURCES
        src/integer.c
        src/networking.c
)

# Header files for installation
set(p101_convert_HEADERS
        include/p101_convert/integer.h
        include/p101_convert/networking.h
)

# Linked libraries required for this project
set(p101_convert_LINK_LIBRARIES
        p101_error
        p101_env
        p101_c
        p101_posix
)
