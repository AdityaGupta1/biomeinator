cmake_minimum_required(VERSION 3.20)
project(GigaMinecraft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(DXC_EXECUTABLE NAMES dxc REQUIRED)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")

set(SHADER_DIR "${SRC_DIR}/shaders")

set(GENERATED_SHADER_HEADERS "${CMAKE_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GENERATED_SHADER_HEADERS})
include_directories(${GENERATED_SHADER_HEADERS})

file(GLOB_RECURSE SHADER_FILES "${SHADER_DIR}/*.hlsl")
file(GLOB_RECURSE EXISTING_COMPILED_SHADERS "${GENERATED_SHADER_HEADERS}/*.fxh")

message(STATUS "Cleaning generated shaders...")
file(REMOVE_RECURSE ${GENERATED_SHADER_HEADERS})
file(MAKE_DIRECTORY ${GENERATED_SHADER_HEADERS})

message(STATUS "Compiling shaders...")

set(COMPILED_HEADERS "")
foreach(SHADER ${SHADER_FILES})
    file(RELATIVE_PATH REL_PATH ${SHADER_DIR} ${SHADER})
    get_filename_component(NAME_WE ${REL_PATH} NAME_WE)
    string(REPLACE "\\" "/" REL_PATH_UNIX ${REL_PATH})
    string(REPLACE ".hlsl" ".fxh" FXH_FILENAME ${REL_PATH_UNIX})
    get_filename_component(VAR_NAME ${NAME_WE} NAME_WE)
    set(VAR_NAME "compiled_${VAR_NAME}")

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
                /Vn ${VAR_NAME}
                /I ${SHADER_DIR}
        DEPENDS ${SHADER}
        COMMENT "Compiling and embedding ${SHADER} to ${FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${FXH_OUT})
    set_source_files_properties(${FXH_OUT} PROPERTIES GENERATED TRUE)
endforeach()

file(GLOB_RECURSE SRC_FILES "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.h")

add_executable(GigaMinecraft ${SRC_FILES} ${COMPILED_HEADERS})

target_link_libraries(GigaMinecraft PRIVATE
    user32
    d3d12
    dxgi
)
