cmake_minimum_required(VERSION 3.20)
project(GigaMinecraft LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(DXC_EXECUTABLE NAMES dxc REQUIRED)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(GENERATED_SHADER_HEADERS "${CMAKE_BINARY_DIR}/generated_shaders")

file(MAKE_DIRECTORY ${GENERATED_SHADER_HEADERS})
include_directories(${GENERATED_SHADER_HEADERS})

file(GLOB SHADERS "${SHADER_DIR}/*.hlsl")

set(COMPILED_HEADERS "")
foreach(SHADER ${SHADERS})
    get_filename_component(NAME_WE ${SHADER} NAME_WE)
    set(FXH_OUT "${GENERATED_SHADER_HEADERS}/${NAME_WE}.fxh")
    set(VAR_NAME "compiled_${NAME_WE}")

    add_custom_command(
        OUTPUT ${FXH_OUT}
        COMMAND ${DXC_EXECUTABLE}
                ${SHADER}
                /T lib_6_3
                /Fh ${FXH_OUT}
                /Vn ${VAR_NAME}
        DEPENDS ${SHADER}
        COMMENT "Compiling and embedding ${SHADER} to ${FXH_OUT}"
        VERBATIM
    )

    list(APPEND COMPILED_HEADERS ${FXH_OUT})
    set_source_files_properties(${FXH_OUT} PROPERTIES GENERATED TRUE)
endforeach()

add_executable(GigaMinecraft ${SRC_DIR}/main.cpp ${COMPILED_HEADERS})

target_link_libraries(GigaMinecraft PRIVATE
    user32
    d3d12
    dxgi
)
