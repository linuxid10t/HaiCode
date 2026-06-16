cmake_minimum_required(VERSION 3.20)
project(haicode CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Haiku system paths
set(HAIKU_SYSTEM "/boot/system")
set(HAIKU_DEVELOP "${HAIKU_SYSTEM}/develop")

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
find_library(SQLITE3_LIB sqlite3 HINTS ${HAIKU_DEVELOP}/lib /boot/system/lib)
find_library(CURL_LIB curl HINTS ${HAIKU_DEVELOP}/lib /boot/system/lib)
find_library(NCURSES_LIB ncursesw HINTS ${HAIKU_DEVELOP}/lib /boot/system/lib)
find_library(BE_LIB be HINTS ${HAIKU_DEVELOP}/lib /boot/system/lib)
find_library(ROOT_LIB root HINTS ${HAIKU_DEVELOP}/lib /boot/system/lib)

if(NOT SQLITE3_LIB)
    message(WARNING "sqlite3 not found — run: pkgman install sqlite_devel")
endif()
if(NOT CURL_LIB)
    message(WARNING "curl not found — run: pkgman install curl_devel")
endif()

add_subdirectory(lib)
add_subdirectory(tui)
add_subdirectory(gui)
