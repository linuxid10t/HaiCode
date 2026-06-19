cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE GUI_SOURCES "src/*.cpp")

add_executable(haicode-gui ${GUI_SOURCES})

target_include_directories(haicode-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Find tracker library for find_directory support
find_library(TRACKER_LIB tracker HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})

target_link_libraries(haicode-gui PRIVATE
    haicode
    ${BE_LIB}
    ${ROOT_LIB}
    $<$<BOOL:${TRACKER_LIB}>:${TRACKER_LIB}>
)
