cmake_minimum_required(VERSION 3.24)
project(Biomeinator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB_RECURSE SRC_FILES
    CONFIGURE_DEPENDS
    "${SRC_DIR}/*.cpp"
    "${SRC_DIR}/*.h"
    "${SRC_DIR}/*.slang"
)

# set up Visual Studio filters based on directory structure
foreach(FILE IN LISTS SRC_FILES)
    file(RELATIVE_PATH REL_PATH "${SRC_DIR}" "${FILE}")
    get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
    string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
    string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
    source_group("src\\${FILTER_PATH}" FILES "${FILE}")
endforeach()

add_executable(Biomeinator ${SRC_FILES})

target_include_directories(Biomeinator PRIVATE
    ${SRC_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/external/include"
)

target_link_directories(Biomeinator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/lib"
)

target_compile_definitions(Biomeinator PRIVATE CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(Biomeinator PRIVATE user32 d3d12 dxgi slang)

file(GLOB RUNTIME_DLLS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/external/bin/*.dll")

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RUNTIME_DLLS}
    $<TARGET_FILE_DIR:Biomeinator>)
