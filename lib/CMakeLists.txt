cmake_minimum_required(VERSION 3.20)

# Collect all core sources
file(GLOB_RECURSE HAICODE_SOURCES CONFIGURE_DEPENDS
    "src/config/*.cpp"
    "src/db/*.cpp"
    "src/session/*.cpp"
    "src/engine/*.cpp"
    "src/compaction/*.cpp"
    "src/provider/*.cpp"
    "src/auth/*.cpp"
    "src/tool/*.cpp"
    "src/permission/*.cpp"
    "src/pricing/*.cpp"
    "src/skills/*.cpp"
    "src/util/*.cpp"
)

add_library(haicode STATIC ${HAICODE_SOURCES})

target_include_directories(haicode PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(haicode PUBLIC
    ${SQLITE3_LIB}
    ${CURL_LIB}
    ${CRYPTO_LIB}
    ${ROOT_LIB}
    ${BE_LIB}
    network
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

add_executable(test_config_permission tests/test_config_permission.cpp)
target_link_libraries(test_config_permission haicode)

add_executable(test_step_budget tests/test_step_budget.cpp)
target_link_libraries(test_step_budget haicode)

add_executable(test_compaction tests/test_compaction.cpp)
target_link_libraries(test_compaction haicode)

add_executable(test_checkpoint tests/test_checkpoint.cpp)
target_link_libraries(test_checkpoint haicode)

add_executable(test_model_context tests/test_model_context.cpp)
target_link_libraries(test_model_context haicode)

add_executable(test_openai_translate tests/test_openai_translate.cpp)
target_link_libraries(test_openai_translate haicode)

add_executable(test_text_attachment tests/test_text_attachment.cpp)
target_link_libraries(test_text_attachment haicode)

add_executable(test_screenshot tests/test_screenshot.cpp)
target_link_libraries(test_screenshot haicode)

add_executable(test_tool_safety tests/test_tool_safety.cpp)
target_link_libraries(test_tool_safety haicode)

add_executable(test_skills tests/test_skills.cpp)
target_link_libraries(test_skills haicode)

add_executable(test_retry tests/test_retry.cpp)
target_link_libraries(test_retry haicode)

add_executable(test_http_client tests/test_http_client.cpp)
# Socket APIs (bind/accept/recv) live in libnetwork on Haiku, and nothing in
# libhaicode itself uses them, so the test links it directly.
target_link_libraries(test_http_client haicode network)

add_executable(test_symbols tests/test_symbols.cpp)
target_link_libraries(test_symbols haicode)

add_executable(test_codex_auth tests/test_codex_auth.cpp)
target_link_libraries(test_codex_auth haicode)

add_executable(test_permission_requests tests/test_permission_requests.cpp)
target_link_libraries(test_permission_requests haicode)

add_executable(test_db_upgrade tests/test_db_upgrade.cpp)
target_link_libraries(test_db_upgrade haicode)

add_executable(test_db_concurrency tests/test_db_concurrency.cpp)
target_link_libraries(test_db_concurrency haicode)

add_executable(test_config_write tests/test_config_write.cpp)
target_link_libraries(test_config_write haicode)

add_executable(test_config_trust tests/test_config_trust.cpp)
target_link_libraries(test_config_trust haicode)

add_executable(test_subprocess tests/test_subprocess.cpp)
target_link_libraries(test_subprocess haicode)

add_executable(test_turn_integrity tests/test_turn_integrity.cpp)
target_link_libraries(test_turn_integrity haicode)

# Register every test executable with CTest (ctest --test-dir build).
foreach(t test_db test_diff test_git_find test_process test_file_tools
         test_config_permission test_step_budget test_compaction
         test_checkpoint test_model_context test_openai_translate
         test_text_attachment test_screenshot test_tool_safety
         test_skills test_retry test_http_client test_symbols
         test_codex_auth test_permission_requests test_db_upgrade
         test_db_concurrency test_config_write test_config_trust
         test_subprocess test_turn_integrity)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
