cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE GUI_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(haicode-gui ${GUI_SOURCES})

target_include_directories(haicode-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Find tracker library for find_directory support
find_library(TRACKER_LIB tracker HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS})

# Compile the app resources (signature + B_SINGLE_LAUNCH) and attach them
# to the executable. rc/xres are Haiku system tools at /boot/system/bin.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/haicode.rsrc"
    COMMAND rc "${CMAKE_CURRENT_SOURCE_DIR}/haicode.rdef"
            -o "${CMAKE_CURRENT_BINARY_DIR}/haicode.rsrc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/haicode.rdef"
    COMMENT "Compiling haicode.rdef"
    VERBATIM)
add_custom_target(haicode-resources
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/haicode.rsrc")
add_dependencies(haicode-gui haicode-resources)
add_custom_command(
    TARGET haicode-gui POST_BUILD
    COMMAND xres -o "$<TARGET_FILE:haicode-gui>"
            "${CMAKE_CURRENT_BINARY_DIR}/haicode.rsrc"
    COMMENT "Attaching resources to haicode-gui"
    VERBATIM)

target_link_libraries(haicode-gui PRIVATE
    haicode
    ${BE_LIB}
    ${ROOT_LIB}
    $<$<BOOL:${TRACKER_LIB}>:${TRACKER_LIB}>
)
