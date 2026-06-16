cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE TUI_SOURCES "src/*.cpp")

add_executable(haicode-tui ${TUI_SOURCES})

target_include_directories(haicode-tui PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(haicode-tui PRIVATE
    haicode
    ${NCURSES_LIB}
    ${BE_LIB}
    ${ROOT_LIB}
)
