find_program(GLSLC_EXECUTABLE NAMES glslc)
find_program(GLSLANG_EXECUTABLE NAMES glslangValidator)
if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_EXECUTABLE)
    message(STATUS "no glslc/glslangValidator found - skipping wlvk-video example")
    return()
endif()

set(VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/tri.vert.spv")
set(FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/yuv.frag.spv")
set(SPIRV_HEADER "${CMAKE_CURRENT_BINARY_DIR}/shaders_spirv.h")

if(GLSLC_EXECUTABLE)
    set(COMPILER "${GLSLC_EXECUTABLE}")
    set(GENERATE_ARGS "-O" "-o")
else()
    set(COMPILER "${GLSLANG_EXECUTABLE}")
    set(GENERATE_ARGS "-V" "-o")
endif()

add_custom_command(
    OUTPUT "${VERT_SPV}"
    COMMAND "${COMPILER}" ${GENERATE_ARGS} "${VERT_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tri.vert"
    DEPENDS "shaders/tri.vert"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAG_SPV}"
    COMMAND "${COMPILER}" ${GENERATE_ARGS} "${FRAG_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/yuv.frag"
    DEPENDS "shaders/yuv.frag"
    VERBATIM
)
add_custom_command(
    OUTPUT "${SPIRV_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -DVERT=${VERT_SPV} -DFRAG=${FRAG_SPV}
            -DOUT=${SPIRV_HEADER} -P
            "${CMAKE_CURRENT_SOURCE_DIR}/spirv_to_header.cmake"
    DEPENDS "${VERT_SPV}" "${FRAG_SPV}" "spirv_to_header.cmake"
    VERBATIM
)

add_executable(wlvk-video main.cpp "${SPIRV_HEADER}")
target_include_directories(wlvk-video PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(wlvk-video PRIVATE wlvk PkgConfig::FFMPEG)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wlvk-video PRIVATE -Wall -Wextra -Wpedantic)
endif()
