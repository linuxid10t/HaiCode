cmake_minimum_required(VERSION 3.20)
project(haicode CXX)

# Require a C++20-capable compiler. On an x86_gcc2 hybrid this catches the
# default gcc2 (GCC 2.95.3) toolchain before we try to probe libraries —
# HaiCode cannot build without the secondary-arch (GCC 13) toolchain.
if(NOT "cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    message(FATAL_ERROR
        "This compiler does not support C++20. On a hybrid Haiku you must use "
        "the secondary-architecture toolchain (GCC 13): run `setarch x86` "
        "(32-bit) or ensure /boot/system/bin/x86 (or x86_64) is on PATH before "
        "configuring. Current compiler: ${CMAKE_CXX_COMPILER}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Haiku system paths
set(HAIKU_SYSTEM "/boot/system")
set(HAIKU_DEVELOP "${HAIKU_SYSTEM}/develop")

# --- Haiku secondary-architecture detection -------------------------------
# On a hybrid Haiku (e.g. x86_gcc2 primary + GCC4 secondary), the libraries
# built for the modern toolchain live under an <arch> subdirectory of the
# usual lib paths: /boot/system/develop/lib/<arch> and /boot/system/lib/<arch>.
# On a pure single-arch system these subdirs don't exist and the flat paths
# are correct. Derive the arch from the compiler's predefined target macros
# (the same mapping HaikuConfig.h uses: __x86_64__ -> x86_64, __i386__ -> x86)
# so we link the matching ABI. The probe dumps only compiler builtins, so it
# sees these even though Haiku's own __HAIKU_ARCH lives in a header.
set(_haiku_arch_probe "")
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -E -dM -
    INPUT_FILE /dev/null
    OUTPUT_VARIABLE _haiku_pp_out
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_haiku_pp_out MATCHES "#define[ \t]+__x86_64__")
    set(_haiku_arch_probe "x86_64")
elseif(_haiku_pp_out MATCHES "#define[ \t]+__i386__")
    set(_haiku_arch_probe "x86")
endif()

# Build the candidate search directories: arch subdir first (hybrid), then
# flat path (single-arch / fallback).
set(HAIKU_ARCH_SUBDIR "")
set(HAIKU_DEVELOP_LIB_DIRS "${HAIKU_DEVELOP}/lib")
set(HAIKU_LIB_DIRS "${HAIKU_SYSTEM}/lib")
if(_haiku_arch_probe)
    set(HAIKU_ARCH_SUBDIR "/${_haiku_arch_probe}")
    set(HAIKU_DEVELOP_LIB_DIRS
        "${HAIKU_DEVELOP}/lib${HAIKU_ARCH_SUBDIR}"
        "${HAIKU_DEVELOP}/lib")
    set(HAIKU_LIB_DIRS
        "${HAIKU_SYSTEM}/lib${HAIKU_ARCH_SUBDIR}"
        "${HAIKU_SYSTEM}/lib")
    message(STATUS "Haiku: secondary arch '${_haiku_arch_probe}' -> "
                   "linking lib${HAIKU_ARCH_SUBDIR}")
else()
    message(STATUS "Haiku: no secondary arch detected (single-arch system)")
endif()

include_directories(
    ${HAIKU_DEVELOP}/headers/os
    ${HAIKU_DEVELOP}/headers/os/app
    ${HAIKU_DEVELOP}/headers/os/interface
    ${HAIKU_DEVELOP}/headers/os/support
    ${HAIKU_DEVELOP}/headers/os/storage
    ${HAIKU_DEVELOP}/headers/os/kernel
    ${HAIKU_DEVELOP}/headers/os/be_apps/Tracker
)

# Find required libraries
find_library(SQLITE3_LIB sqlite3 HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})
find_library(CURL_LIB curl HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})
find_library(NCURSES_LIB ncursesw HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})
find_library(BE_LIB be HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})
find_library(ROOT_LIB root HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})

if(NOT SQLITE3_LIB)
    message(WARNING "sqlite3 not found — run: pkgman install sqlite_devel")
endif()
if(NOT CURL_LIB)
    message(WARNING "curl not found — run: pkgman install curl_devel")
endif()

add_subdirectory(lib)
add_subdirectory(tui)
add_subdirectory(gui)
