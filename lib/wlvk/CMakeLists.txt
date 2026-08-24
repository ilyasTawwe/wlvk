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
        "${WAYLAND_PROTOCOLS_DIR}/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml"
        "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
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
    context.cpp
    window.cpp
    vma_impl.cpp
    "${VOLK_SOURCE}"
    ${GENERATED_SOURCES}
)
set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# Consumers compile their own translation units against volk.h, so these
# defines must match the library's; propagate them as usage requirements.
target_compile_definitions(wlvk PUBLIC
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WAYLAND_KHR
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wlvk PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_include_directories(wlvk PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../external"
    "${GENERATED_DIR}"
    "${VOLK_INCLUDE_DIR}")
target_link_libraries(wlvk PUBLIC
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::LIBDRM
    ${CMAKE_DL_LIBS}
)
