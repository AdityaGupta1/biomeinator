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
    GIT_REPOSITORY  https://github.com/g-truc/glm.git
    GIT_TAG         1.0.2
)

FetchContent_MakeAvailable(glm)

# Prefer the vendored dxc (matches the vendored dxcompiler.dll/dxil.dll and supports
# newer shader models than the Windows SDK one)
if(NOT DXC_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/bin/dxc.exe")
    set(DXC_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/external/dxc/bin/dxc.exe")
endif()

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
list(FILTER SRC_FILES EXCLUDE REGEX "/(tests|scanner)/")

file(GLOB_RECURSE SHADER_FILES
    CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.hlsl"
)

file(GLOB_RECURSE SHADER_INCLUDE_FILES
    CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.hlsli"
)

file(GLOB_RECURSE COMMON_HEADERS CONFIGURE_DEPENDS "${COMMON_DIR}/*.h")

SET(SHADER_VERSION "6_9")

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
                -enable-16bit-types
                /I "${SHADER_DIR}"
                /I "${CMAKE_SOURCE_DIR}/external/NRC/Include"
        DEPENDS ${SHADER} ${SHADER_INCLUDE_FILES} ${COMMON_HEADERS}
        COMMENT "Compiling and embedding ${SHADER} to ${FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${FXH_OUT})
    set_source_files_properties(${FXH_OUT} PROPERTIES GENERATED TRUE)
endforeach()

# --- GPUSorting (external) compute shader kernels ---
# DeviceRadixSort.hlsl exposes four entry points; compile each into its own .fxh.
# Tuning defaults in SortCommon.hlsl already match the NVIDIA pairs preset
# (KEYS_PER_THREAD=15, D_DIM=512, PART_SIZE=7680, D_TOTAL_SMEM=7936) so we don't
# need to pass tuning macros — only the key/payload/order config.
set(GPU_SORT_SHADER "${CMAKE_SOURCE_DIR}/external/GPUSorting/GPUSortingD3D12/Shaders/DeviceRadixSort.hlsl")
set(GPU_SORT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/GPUSorting/GPUSortingD3D12/Shaders")
set(GPU_SORT_DEFINES "-DSORT_PAIRS" "-DKEY_UINT" "-DPAYLOAD_UINT" "-DSHOULD_ASCEND" "-DENABLE_16_BIT")

# DeviceRadixSort.hlsl transitively includes only SortCommon.hlsl from the
# GPUSorting shader directory. List its real dependency explicitly instead of
# globbing the whole directory so edits to unrelated kernels (OneSweep,
# FFXParallelSort, etc.) don't trigger a needless rebuild of our four kernels.
set(GPU_SORT_INCLUDE_FILES "${GPU_SORT_INCLUDE_DIR}/SortCommon.hlsl")

set(GPU_SORT_KERNELS
    InitDeviceRadixSort gpu_sort_init_cs
    Upsweep             gpu_sort_upsweep_cs
    Scan                gpu_sort_scan_cs
    Downsweep           gpu_sort_downsweep_cs
)

list(LENGTH GPU_SORT_KERNELS GPU_SORT_KERNELS_LEN)
math(EXPR GPU_SORT_KERNELS_LAST "${GPU_SORT_KERNELS_LEN} - 1")
foreach(IDX RANGE 0 ${GPU_SORT_KERNELS_LAST} 2)
    math(EXPR NAME_IDX "${IDX} + 1")
    list(GET GPU_SORT_KERNELS ${IDX} GPU_SORT_KERNEL_ENTRY)
    list(GET GPU_SORT_KERNELS ${NAME_IDX} GPU_SORT_OUT_NAME)
    set(GPU_SORT_FXH_OUT "${GENERATED_SHADER_HEADERS_DIR}/${GPU_SORT_OUT_NAME}.fxh")

    add_custom_command(
        OUTPUT ${GPU_SORT_FXH_OUT}
        COMMAND ${DXC_EXECUTABLE}
                ${GPU_SORT_SHADER}
                /T cs_${SHADER_VERSION}
                /E ${GPU_SORT_KERNEL_ENTRY}
                /Fh ${GPU_SORT_FXH_OUT}
                /Vn ${GPU_SORT_OUT_NAME}_shaderBytecode
                /HV 2021
                -enable-16bit-types
                ${GPU_SORT_DEFINES}
                /I "${GPU_SORT_INCLUDE_DIR}"
        DEPENDS ${GPU_SORT_SHADER} ${GPU_SORT_INCLUDE_FILES}
        COMMENT "Compiling GPUSorting kernel ${GPU_SORT_KERNEL_ENTRY} -> ${GPU_SORT_FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${GPU_SORT_FXH_OUT})
    set_source_files_properties(${GPU_SORT_FXH_OUT} PROPERTIES GENERATED TRUE)
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

set(LZ4_BUILD_CLI OFF CACHE BOOL "Build lz4 CLI" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "Build lz4c CLI" FORCE)
add_subdirectory("${EXT}/lz4/build/cmake" lz4)

set(FASTNOISE2_TOOLS OFF CACHE BOOL "Build FastNoise2 tools (Node Editor)" FORCE)
set(FASTNOISE2_TESTS OFF CACHE BOOL "Build FastNoise2 tests" FORCE)
set(FASTNOISE2_UTILITY OFF CACHE BOOL "Build FastNoise2 utility tools" FORCE)
add_subdirectory("${EXT}/FastNoise2")

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
    "${EXT}/FastNoise2/include"
    "${EXT}/imgui"
    "${EXT}/imgui/backends"
    "${EXT}/implot"
    "${EXT}/json/include"
    "${EXT}/nvapi"
    "${EXT}/stb/include"
    "${EXT}/streamline/include"
    "${EXT}/tinygltf/include"
    "${EXT}/NRC/Include"
)

target_link_directories(Biomeinator PRIVATE
    "${EXT}/dxc/lib"
    "${EXT}/nvapi/amd64"
    "${EXT}/streamline/lib/x64"
    "${EXT}/NRC/Lib"
)

target_link_libraries(Biomeinator PRIVATE user32 sl.interposer d3dcompiler nvapi64 NRC_D3D12 glm::glm-header-only FastNoise lz4_static)

set(STREAMLINE_DLL_DIR "${EXT}/streamline/bin/x64")

set(RUNTIME_DLLS
    "${EXT}/dxc/bin/dxcompiler.dll"
    "${EXT}/dxc/bin/dxil.dll"
    "${STREAMLINE_DLL_DIR}/sl.interposer.dll"
    "${STREAMLINE_DLL_DIR}/sl.dlss_d.dll"
    "${STREAMLINE_DLL_DIR}/nvngx_dlssd.dll"
    #"${STREAMLINE_DLL_DIR}/development/nvngx_dlssd.dll"
    "${EXT}/NRC/Bin/NRC_D3D12.dll"
    "${EXT}/NRC/Bin/cudart64_12.dll"
    "${EXT}/NRC/Bin/nvrtc64_120_0.dll"
    "${EXT}/NRC/Bin/nvrtc-builtins64_128.dll"
)

add_custom_command(TARGET Biomeinator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RUNTIME_DLLS}
    $<TARGET_FILE_DIR:Biomeinator>)

set(AGILITY_SDK_DLLS
    "${EXT}/AgilitySDK/bin/D3D12Core.dll"
    "$<$<NOT:$<CONFIG:Release>>:${EXT}/AgilitySDK/bin/d3d12SDKLayers.dll>"
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

add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_DST_DIR}"
    VERBATIM
)

foreach(src IN LISTS ASSET_FILES)
    file(RELATIVE_PATH rel "${ASSETS_SRC_DIR}" "${src}")
    set(dst "${ASSETS_DST_DIR}/${rel}")
    get_filename_component(dst_dir "${dst}" DIRECTORY)

    add_custom_command(TARGET copy_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}" "${dst}"
        VERBATIM
    )
endforeach()

add_dependencies(Biomeinator copy_assets)

target_compile_definitions(Biomeinator PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TARGET_FILE_DIR="$<TARGET_FILE_DIR:Biomeinator>"
    ENABLE_ASSERTS=$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>
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
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    BIOMEINATOR_EXE_PATH="$<TARGET_FILE:Biomeinator>"
)

# ==================================================
# BiomeScanner
# ==================================================

# Local web tool for exploring the biome field: serves an interactive seed map and searches for
# seeds with a target biome near spawn.
set(BIOME_SCANNER_SRC
    "${SRC_DIR}/scanner/main.cpp"
    "${SRC_DIR}/scanner/index.html"
    "${SRC_DIR}/logger.cpp"
    "${SRC_DIR}/terrain/biome.cpp"
    "${SRC_DIR}/terrain/biome_noise.cpp"
    "${SRC_DIR}/terrain/structure/decorator.cpp"
    "${SRC_DIR}/terrain/structure/structure_gen.cpp"
)
assign_source_group("src" "${SRC_DIR}" "${BIOME_SCANNER_SRC}")

add_executable(BiomeScanner ${BIOME_SCANNER_SRC})
target_include_directories(BiomeScanner PRIVATE
    ${SRC_DIR}
    "${EXT}/cpp-httplib/include"
    "${EXT}/FastNoise2/include"
    "${EXT}/json/include"
)
target_link_libraries(BiomeScanner PRIVATE glm::glm-header-only FastNoise)
target_compile_definitions(BiomeScanner PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ENABLE_ASSERTS=$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>
)
