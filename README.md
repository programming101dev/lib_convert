# lib_convert Repository Guide

Welcome to the `lib_convert` repository — integer and networking conversion utilities, part of the Programming 101 C library collection. This guide will help you set up, build, and install the library.

## API contracts

The integer functions parse one complete base-10 string. Leading whitespace
and a leading `+` are accepted; trailing whitespace or other trailing
characters are rejected. Signed, unsigned, positive, and negative variants are
available for native and fixed-width integer types.

Every integer failure raises an error and returns the caller's `default_value`.
Syntax failures use `P101_CONVERT_ERROR_SYNTAX`; range and sign failures use
`P101_CONVERT_ERROR_RANGE`. A null string is a failed API check.

`p101_convert_address` accepts:

- strict IPv4 and IPv6 literals; or
- an explicit Unix socket pathname containing `/` (`./socket` for the current
  directory).

It returns the populated `socklen_t`, sets `AF_UNSPEC` and returns zero on
failure, and preserves unrelated errors already present in the caller's error
object. Host names, empty strings, malformed literals, and ambiguous bare Unix
socket names are rejected. Name resolution belongs in the `getaddrinfo`
wrappers rather than this literal converter.

## **Table of Contents**

1. [Cloning the Repository](#cloning-the-repository)
2. [Prerequisites](#prerequisites)
3. [Configuring the Build](#configuring-the-build)
4. [Building](#building)
5. [Testing](#testing)
6. [Installing](#installing)
7. [Adding or Removing Files](#adding-or-removing-files)

## **Cloning the Repository**

Clone the repository using the following command:

```bash
git clone https://github.com/programming101dev/lib_convert.git
```

Navigate to the cloned directory:

```bash
cd lib_convert
```

Ensure the scripts are executable:

```bash
chmod +x *.sh
```

## **Prerequisites**

To ensure you have all of the required tools installed, run:

```bash
cmake -S . -B build
```

If you are missing tools follow these [instructions](https://docs.google.com/document/d/1ZPqlPD1mie5iwJ2XAcNGz7WeA86dTLerFXs9sAuwCco/edit?usp=drive_link). If something still looks wrong, `cmake -S . -B build` reports what actually works on this machine for this project.

## **Configuring the Build**

Tell CMake which compiler you want to use:

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=<compiler> -DP101_BUILD_LEVEL=1
```

To see the list of possible compilers:

```bash
cat supported_c_compilers.txt
```

Run it again any time to switch compilers; each compiler configures into its own build directory (e.g. `build-clang`, `build-gcc-15`).

## **Building**

To build the library run:

```bash
cmake --build build
```

This compiles through the strict analysis pipeline: the clang-format check, clang-tidy, cppcheck, the Clang static analyzer, and hundreds of warnings under `-Werror`. `cmake --build build --target format` applies the formatter and tidy fixes in place.

## **Testing**

`cmake -S . -B build -DP101_BUILD_LEVEL=3 && cmake --build build` is the one command to run before you submit: the format check, the strict build, the tests, and a short fuzz smoke run, with a single PASS/FAIL at the end.
`cmake -S . -B build -DP101_BUILD_LEVEL=2 && cmake --build build` runs the Unity unit-test suite in `test/`, and `configure and run the fuzz/ CMake project` runs the libFuzzer harness in `fuzz/` (needs a clang with the fuzzer runtime, e.g. Homebrew LLVM).

## **Installing**

To install the library run:

```bash
cmake --install build
```

You may need to run it via sudo, or give the user account access to the install directories. `cmake --build build --target uninstall` removes it again.

## **Adding or Removing Files**

The `CMakeLists.txt` is fixed and shared across every repository — do not edit it. When you add or remove a source or header, edit the lists in `config.cmake` (`p101_convert_SOURCES`, `p101_convert_HEADERS`, and `p101_convert_LINK_LIBRARIES`), then re-configure and build:

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=<compiler> -DP101_BUILD_LEVEL=1
cmake --build build
```
