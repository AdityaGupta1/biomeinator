cmake_minimum_required(VERSION 3.24)
project(Biomeinator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TODO: remove dxc stuff
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

include_directories(
    ${SRC_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/external/include"
    ${GENERATED_SHADER_HEADERS}
)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/lib)

# Collect all source files and keep the list up to date when files are added
file(GLOB_RECURSE SRC_FILES
    CONFIGURE_DEPENDS
    "${SRC_DIR}/*.cpp"
    "${SRC_DIR}/*.h"
)

# Collect all shaders (hlsl and hlsli)
file(GLOB_RECURSE SHADER_FILES_ALL
    CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.hlsl"
    "${SHADER_DIR}/*.hlsli"
    "${SHADER_DIR}/*.slang"
    "${SHADER_DIR}/*.slangh"
)

set(SHADER_FILES "")
set(SHADER_INCLUDE_FILES "")
foreach(FILE IN LISTS SHADER_FILES_ALL)
    if (FILE MATCHES ".*\\.hlsli$")
        list(APPEND SHADER_INCLUDE_FILES ${FILE})
    else()
        list(APPEND SHADER_FILES ${FILE})
    endif()
endforeach()

# Headers that shaders depend on
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
                /T lib_6_3
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

# Visual Studio filters for project structure
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

target_compile_definitions(Biomeinator PRIVATE CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(Biomeinator PRIVATE user32 d3d12 dxgi slang)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/external/bin/slang.dll
    $<TARGET_FILE_DIR:Biomeinator>)

set(DXC_DLLS
    ${CMAKE_SOURCE_DIR}/external/bin/dxcompiler.dll
    ${CMAKE_SOURCE_DIR}/external/bin/dxil.dll
)

foreach(dll ${DXC_DLLS})
    add_custom_command(TARGET Biomeinator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${dll}
        $<TARGET_FILE_DIR:Biomeinator>)
endforeach()