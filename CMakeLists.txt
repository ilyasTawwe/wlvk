cmake_minimum_required(VERSION 4.0)
project(codotaku_media VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# No C++ modules here; skipping the scan avoids GCC-only flags
# (-fmodules-ts, -fmodule-mapper, ...) that clangd cannot parse.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# System libvolk.a is built without platform defines, so its WSI entry points
# are absent; compile volk.c ourselves with VK_USE_PLATFORM_WAYLAND_KHR instead.
find_path(VOLK_INCLUDE_DIR NAMES volk.h REQUIRED)
find_file(VOLK_SOURCE NAMES volk.c PATHS "${VOLK_INCLUDE_DIR}" REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(GENERATED_SOURCES "")
foreach(PROTOCOL_XML
        "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
        "${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml"
        "${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml"
        "${WAYLAND_PROTOCOLS_DIR}/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml")
    if(NOT EXISTS "${PROTOCOL_XML}")
        message(FATAL_ERROR "protocol not found at ${PROTOCOL_XML}")
    endif()
    get_filename_component(PROTOCOL_NAME "${PROTOCOL_XML}" NAME_WE)
    set(PROTOCOL_HEADER "${GENERATED_DIR}/${PROTOCOL_NAME}-client-protocol.h")
    set(PROTOCOL_CODE "${GENERATED_DIR}/${PROTOCOL_NAME}-protocol.c")
    add_custom_command(
        OUTPUT "${PROTOCOL_HEADER}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${PROTOCOL_XML}" "${PROTOCOL_HEADER}"
        DEPENDS "${PROTOCOL_XML}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${PROTOCOL_CODE}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${PROTOCOL_XML}" "${PROTOCOL_CODE}"
        DEPENDS "${PROTOCOL_XML}"
        VERBATIM
    )
    set_source_files_properties("${PROTOCOL_CODE}" PROPERTIES COMPILE_OPTIONS "-w")
    list(APPEND GENERATED_SOURCES "${PROTOCOL_HEADER}" "${PROTOCOL_CODE}")
endforeach()

add_library(wlvk STATIC
    lib/wlvk/context.cpp
    lib/wlvk/window.cpp
    lib/wlvk/vma_impl.cpp
    "${VOLK_SOURCE}"
    ${GENERATED_SOURCES}
)
set_source_files_properties(lib/wlvk/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

function(codotaku_configure_target target)
    target_compile_definitions("${target}" PRIVATE
        VK_NO_PROTOTYPES
        VK_USE_PLATFORM_WAYLAND_KHR
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

codotaku_configure_target(wlvk)
target_include_directories(wlvk PUBLIC
    lib external "${GENERATED_DIR}" "${VOLK_INCLUDE_DIR}")
target_link_libraries(wlvk PUBLIC
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::LIBDRM
    ${CMAKE_DL_LIBS}
)

add_executable(codotaku-media src/main.cpp)
codotaku_configure_target(codotaku-media)
target_link_libraries(codotaku-media PRIVATE wlvk)
