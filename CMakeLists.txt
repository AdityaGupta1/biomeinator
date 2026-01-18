cmake_minimum_required(VERSION 3.24)
project(Biomeinator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ==================================================
# Biomeinator
# ==================================================

FetchContent_Declare(
	glm
	GIT_REPOSITORY	https://github.com/g-truc/glm.git
	GIT_TAG 		1.0.2
)

FetchContent_MakeAvailable(glm)

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
set(GENERATED_SHADER_HEADERS_DIR "${CMAKE_BINARY_DIR}/generated_shaders")

file(MAKE_DIRECTORY ${GENERATED_SHADER_HEADERS_DIR})

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

SET(SHADER_VERSION "6_6")

# Compile shaders to headers
set(COMPILED_HEADERS "")
foreach(SHADER IN LISTS SHADER_FILES)
    file(RELATIVE_PATH REL_PATH ${SHADER_DIR} ${SHADER})
    string(REPLACE "\\" "/" REL_PATH_UNIX ${REL_PATH})
    string(REPLACE ".hlsl" ".fxh" FXH_FILENAME ${REL_PATH_UNIX})
    set(FXH_OUT "${GENERATED_SHADER_HEADERS_DIR}/${FXH_FILENAME}")
    get_filename_component(FXH_DIR ${FXH_OUT} DIRECTORY)
    file(MAKE_DIRECTORY ${FXH_DIR})

    get_filename_component(FILE_NAME "${REL_PATH}" NAME)
    string(REGEX REPLACE "\\.hlsl$" "" NAME_NO_HLSL "${FILE_NAME}")
    string(REPLACE "." "_" NAME_SAFE "${NAME_NO_HLSL}")

    if (FILE_NAME MATCHES "\\.rgs\\.hlsl$")
        set(DXC_TARGET "lib_${SHADER_VERSION}")
        set(DXC_ENTRY "")
    elseif (FILE_NAME MATCHES "\\.vs\\.hlsl$")
        set(DXC_TARGET "vs_${SHADER_VERSION}")
        set(DXC_ENTRY "/E vsMain")
    elseif (FILE_NAME MATCHES "\\.ps\\.hlsl$")
        set(DXC_TARGET "ps_${SHADER_VERSION}")
        set(DXC_ENTRY "/E psMain")
    elseif (FILE_NAME MATCHES "\\.cs\\.hlsl$")
        set(DXC_TARGET "cs_${SHADER_VERSION}")
        set(DXC_ENTRY "/E csMain")
    else()
        message(FATAL_ERROR "Unknown shader stage for ${FILE_NAME} (expected .rgs.hlsl, .vs.hlsl, .ps.hlsl, .cs.hlsl)")
    endif()

    add_custom_command(
        OUTPUT ${FXH_OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${FXH_DIR}
        COMMAND ${DXC_EXECUTABLE}
                ${SHADER}
                /T ${DXC_TARGET}
                ${DXC_ENTRY}
                /Fh ${FXH_OUT}
                /Vn ${NAME_SAFE}_shaderBytecode
                /HV 2021
        DEPENDS ${SHADER} ${SHADER_INCLUDE_FILES} ${COMMON_HEADERS}
        COMMENT "Compiling and embedding ${SHADER} to ${FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${FXH_OUT})
    set_source_files_properties(${FXH_OUT} PROPERTIES GENERATED TRUE)
endforeach()

# Mark shader source files as headers only
set_source_files_properties(${SHADER_FILES} ${SHADER_INCLUDE_FILES} ${COMPILED_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)

# Sets up Visual Studio filters based on directory structure
function(assign_source_group FILTER_ROOT BASE_DIR FILES)
    foreach(FILE IN LISTS FILES)
        file(RELATIVE_PATH REL_PATH "${BASE_DIR}" "${FILE}")
        get_filename_component(DIR_PATH "${REL_PATH}" DIRECTORY)
        string(REPLACE "\\" "/" DIR_PATH "${DIR_PATH}")
        string(REPLACE "/" "\\" FILTER_PATH "${DIR_PATH}")
        source_group("${FILTER_ROOT}\\${FILTER_PATH}" FILES "${FILE}")
    endforeach()
endfunction()

assign_source_group("src" "${SRC_DIR}" "${SRC_FILES};${SHADER_FILES};${SHADER_INCLUDE_FILES}")
assign_source_group("generated_shaders" "${GENERATED_SHADER_HEADERS_DIR}" "${COMPILED_HEADERS}")

set(EXT "${CMAKE_SOURCE_DIR}/external")

file(GLOB IMGUI_SRC
    "${EXT}/imgui/*.cpp"
    "${EXT}/imgui/*.h"
    "${EXT}/implot/*.cpp"
    "${EXT}/implot/*.h"
)

list(APPEND IMGUI_SRC
    "${EXT}/imgui/backends/imgui_impl_win32.cpp"
    "${EXT}/imgui/backends/imgui_impl_win32.h"
    "${EXT}/imgui/backends/imgui_impl_dx12.cpp"
    "${EXT}/imgui/backends/imgui_impl_dx12.h"
)

assign_source_group("external" "${EXT}" "${IMGUI_SRC}")

add_executable(Biomeinator
    ${SRC_FILES}
    ${IMGUI_SRC}
    ${COMPILED_HEADERS}
    ${SHADER_FILES}
    ${SHADER_INCLUDE_FILES}
)

target_include_directories(Biomeinator PRIVATE
    ${SRC_DIR}
    ${GENERATED_SHADER_HEADERS_DIR}
    "${EXT}/cxxopts/include"
    "${EXT}/DirectX-Headers/include/directx"
    "${EXT}/imgui"
    "${EXT}/imgui/backends"
    "${EXT}/implot"
    "${EXT}/json/include"
    "${EXT}/nvapi"
    "${EXT}/stb/include"
    "${EXT}/streamline/include"
    "${EXT}/tinygltf/include"
)

target_link_directories(Biomeinator PRIVATE
    "${EXT}/dxc/lib"
    "${EXT}/nvapi/amd64"
    "${EXT}/streamline/lib/x64"
)

target_link_libraries(Biomeinator PRIVATE user32 sl.interposer d3dcompiler nvapi64 glm::glm-header-only)

set(STREAMLINE_DLL_DIR "${EXT}/streamline/bin/x64")

set(RUNTIME_DLLS
    "${EXT}/dxc/bin/dxcompiler.dll"
    "${EXT}/dxc/bin/dxil.dll"
    "${STREAMLINE_DLL_DIR}/sl.interposer.dll"
    "${STREAMLINE_DLL_DIR}/sl.dlss_d.dll"
    "${STREAMLINE_DLL_DIR}/nvngx_dlssd.dll"
    #"${STREAMLINE_DLL_DIR}/development/nvngx_dlssd.dll"
)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RUNTIME_DLLS}
    $<TARGET_FILE_DIR:Biomeinator>)

set(AGILITY_SDK_DLLS
    "${EXT}/AgilitySDK/bin/D3D12Core.dll"
    $<$<CONFIG:Debug>:${EXT}/AgilitySDK/bin/d3d12SDKLayers.dll>
)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:Biomeinator>/D3D12
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${AGILITY_SDK_DLLS}
            $<TARGET_FILE_DIR:Biomeinator>/D3D12
)

set(ASSETS_SRC_DIR "${CMAKE_SOURCE_DIR}/assets")
set(ASSETS_DST_DIR "$<TARGET_FILE_DIR:Biomeinator>/assets")

file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS
    "${ASSETS_SRC_DIR}/*"
)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_DST_DIR}"
    VERBATIM
)

foreach(src IN LISTS ASSET_FILES)
    file(RELATIVE_PATH rel "${ASSETS_SRC_DIR}" "${src}")
    set(dst "${ASSETS_DST_DIR}/${rel}")
    get_filename_component(dst_dir "${dst}" DIRECTORY)

    add_custom_command(TARGET Biomeinator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${dst}"
        VERBATIM
    )
endforeach()

target_compile_definitions(Biomeinator PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TARGET_FILE_DIR="$<TARGET_FILE_DIR:Biomeinator>"
)

# ==================================================
# BiomeinatorTests
# ==================================================

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
