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

list(FILTER SRC_FILES EXCLUDE REGEX "/tests/")

# set up Visual Studio filters based on directory structure
foreach(FILE IN LISTS SRC_FILES)
    file(RELATIVE_PATH REL_PATH "${SRC_DIR}" "${FILE}")
    get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
    string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
    string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
    source_group("src\\${FILTER_PATH}" FILES "${FILE}")
endforeach()

add_executable(Biomeinator ${SRC_FILES})

set(EXT "${CMAKE_SOURCE_DIR}/external")

target_include_directories(Biomeinator PRIVATE
    ${SRC_DIR}
    "${EXT}/cxxopts/include"
    "${EXT}/json/include"
    "${EXT}/slang/include"
    "${EXT}/stb/include"
    "${EXT}/tinygltf/include"
)

target_link_directories(Biomeinator PRIVATE
    "${EXT}/dxc/lib/"
    "${EXT}/slang/lib/"
)

target_compile_definitions(Biomeinator PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(Biomeinator PRIVATE user32 d3d12 dxgi slang d3dcompiler)

set(RUNTIME_DLLS
    "${EXT}/dxc/bin/dxcompiler.dll"
    "${EXT}/dxc/bin/dxil.dll"
    "${EXT}/slang/bin/slang.dll"
)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RUNTIME_DLLS}
    $<TARGET_FILE_DIR:Biomeinator>)

file(GLOB TESTS_SRC CONFIGURE_DEPENDS
    "${SRC_DIR}/tests/*.cpp"
    "${SRC_DIR}/tests/*.h"
)

foreach(FILE IN LISTS TESTS_SRC)
    file(RELATIVE_PATH REL_PATH "${SRC_DIR}" "${FILE}")
    get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
    string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
    string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
    source_group("src\\${FILTER_PATH}" FILES "${FILE}")
endforeach()

add_executable(BiomeinatorTests ${TESTS_SRC})
target_include_directories(BiomeinatorTests PRIVATE
    ${SRC_DIR}
    "${EXT}/cxxopts/include"
    "${EXT}/json/include"
    "${EXT}/stb/include"
)
add_dependencies(BiomeinatorTests Biomeinator)
target_compile_definitions(BiomeinatorTests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    BIOMEINATOR_EXE_PATH="$<TARGET_FILE:Biomeinator>"
)
