cmake_minimum_required(VERSION 3.24)
project(Biomeinator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Try to find dxc automatically
if(NOT DXC_EXECUTABLE)
    file(GLOB DXC_POSSIBLE_PATHS
        "$ENV{ProgramFiles}/Microsoft DirectX Shader Compiler/dxc.exe"
        "$ENV{VULKAN_SDK}/Bin/dxc.exe"
        "$ENV{VULKAN_SDK}/bin/dxc.exe"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/dxc.exe"
    )

    if(DXC_POSSIBLE_PATHS)
        list(SORT DXC_POSSIBLE_PATHS ORDER DESCENDING)
        list(GET DXC_POSSIBLE_PATHS 0 DXC_EXECUTABLE)
    endif()
endif()

if(NOT DXC_EXECUTABLE)
    find_program(DXC_EXECUTABLE NAMES dxc)
endif()

if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc not found. Set DXC_PATH or install the DirectX Shader Compiler.")
else()
    message(STATUS "Found dxc: ${DXC_EXECUTABLE}")
endif()

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(SHADER_DIR "${SRC_DIR}/shaders")
set(COMMON_DIR "${SRC_DIR}/rendering/common")
set(GENERATED_SHADER_HEADERS "${CMAKE_BINARY_DIR}/generated_shaders")

file(MAKE_DIRECTORY ${GENERATED_SHADER_HEADERS})

file(GLOB_RECURSE SRC_FILES
    CONFIGURE_DEPENDS
    "${SRC_DIR}/*.cpp"
    "${SRC_DIR}/*.h"
)
list(FILTER SRC_FILES EXCLUDE REGEX "/tests/")

file(GLOB_RECURSE SHADER_FILES
    CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.hlsl"
)

file(GLOB_RECURSE SHADER_INCLUDE_FILES
    CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.hlsli"
)

file(GLOB_RECURSE COMMON_HEADERS CONFIGURE_DEPENDS "${COMMON_DIR}/*.h")

# Compile shaders to headers
set(COMPILED_HEADERS "")
foreach(SHADER IN LISTS SHADER_FILES)
    file(RELATIVE_PATH REL_PATH ${SHADER_DIR} ${SHADER})
    get_filename_component(NAME_WE ${REL_PATH} NAME_WE)
    string(REPLACE "\\" "/" REL_PATH_UNIX ${REL_PATH})
    string(REPLACE ".hlsl" ".fxh" FXH_FILENAME ${REL_PATH_UNIX})
    set(FXH_OUT "${GENERATED_SHADER_HEADERS}/${FXH_FILENAME}")
    get_filename_component(FXH_DIR ${FXH_OUT} DIRECTORY)
    file(MAKE_DIRECTORY ${FXH_DIR})

    add_custom_command(
        OUTPUT ${FXH_OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${FXH_DIR}
        COMMAND ${DXC_EXECUTABLE}
                ${SHADER}
                /T lib_6_6
                /Fh ${FXH_OUT}
                /Vn ${NAME_WE}_shaderBytecode
                /I ${SHADER_DIR}
        DEPENDS ${SHADER} ${SHADER_INCLUDE_FILES} ${COMMON_HEADERS}
        COMMENT "Compiling and embedding ${SHADER} to ${FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${FXH_OUT})
    set_source_files_properties(${FXH_OUT} PROPERTIES GENERATED TRUE)
endforeach()

# Mark shader source files as headers only
set_source_files_properties(${SHADER_FILES} ${SHADER_INCLUDE_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

# Set up Visual Studio filters based on directory structure
foreach(FILE IN LISTS SRC_FILES SHADER_FILES SHADER_INCLUDE_FILES)
    file(RELATIVE_PATH REL_PATH "${SRC_DIR}" "${FILE}")
    get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
    string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
    string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
    source_group("src\\${FILTER_PATH}" FILES "${FILE}")
endforeach()

foreach(FXH IN LISTS COMPILED_HEADERS)
    file(RELATIVE_PATH REL_PATH "${GENERATED_SHADER_HEADERS}" "${FXH}")
    get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
    string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
    string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
    source_group("generated_shaders\\${FILTER_PATH}" FILES "${FXH}")
endforeach()

add_executable(Biomeinator
    ${SRC_FILES}
    ${COMPILED_HEADERS}
    ${SHADER_FILES}
    ${SHADER_INCLUDE_FILES}
)

set(EXT "${CMAKE_SOURCE_DIR}/external")

target_include_directories(Biomeinator PRIVATE
    ${SRC_DIR}
    ${GENERATED_SHADER_HEADERS}
    "${EXT}/cxxopts/include"
    "${EXT}/json/include"
    "${EXT}/nvapi"
    "${EXT}/stb/include"
    "${EXT}/tinygltf/include"
)

target_link_directories(Biomeinator PRIVATE
    "${EXT}/dxc/lib/"
    "${EXT}/nvapi/amd64"
)

target_compile_definitions(Biomeinator PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(Biomeinator PRIVATE user32 d3d12 dxgi d3dcompiler nvapi64)

set(RUNTIME_DLLS
    "${EXT}/dxc/bin/dxcompiler.dll"
    "${EXT}/dxc/bin/dxil.dll"
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
