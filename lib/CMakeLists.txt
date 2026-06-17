cmake_minimum_required(VERSION 3.20)

# Collect all core sources
file(GLOB_RECURSE HAICODE_SOURCES
    "src/config/*.cpp"
    "src/db/*.cpp"
    "src/session/*.cpp"
    "src/engine/*.cpp"
    "src/provider/*.cpp"
    "src/tool/*.cpp"
    "src/permission/*.cpp"
    "src/pricing/*.cpp"
    "src/util/*.cpp"
)

add_library(haicode STATIC ${HAICODE_SOURCES})

target_include_directories(haicode PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(haicode PUBLIC
    ${SQLITE3_LIB}
    ${CURL_LIB}
    ${ROOT_LIB}
)

# Test executable for Phase 1 verification
add_executable(test_db tests/test_db.cpp)
target_link_libraries(test_db haicode)

add_executable(test_diff tests/test_diff.cpp)
target_link_libraries(test_diff haicode)

add_executable(test_git_find tests/test_git_find.cpp)
target_link_libraries(test_git_find haicode)

add_executable(test_process tests/test_process.cpp)
target_link_libraries(test_process haicode)

add_executable(test_file_tools tests/test_file_tools.cpp)
target_link_libraries(test_file_tools haicode)
