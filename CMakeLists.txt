cmake_minimum_required(VERSION 4.0)
project(codotaku_media VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CODOTAKU_VALIDATION "Enable Vulkan validation layers" OFF)

# System libvolk.a is built without platform defines, so its WSI entry points
# are absent; compile volk.c ourselves with VK_USE_PLATFORM_WAYLAND_KHR instead.
find_path(VOLK_INCLUDE_DIR NAMES volk.h REQUIRED)
find_file(VOLK_SOURCE NAMES volk.c PATHS "${VOLK_INCLUDE_DIR}" REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
if(NOT EXISTS "${XDG_SHELL_XML}")
    message(FATAL_ERROR "xdg-shell.xml not found at ${XDG_SHELL_XML}")
endif()

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(XDG_SHELL_HEADER "${GENERATED_DIR}/xdg-shell-client-protocol.h")
set(XDG_SHELL_CODE "${GENERATED_DIR}/xdg-shell-protocol.c")

add_custom_command(
    OUTPUT "${XDG_SHELL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM
)
add_custom_command(
    OUTPUT "${XDG_SHELL_CODE}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_CODE}"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM
)

add_executable(codotaku-media
    src/main.cpp
    src/platform_wayland.cpp
    src/renderer_vulkan.cpp
    "${VOLK_SOURCE}"
    "${XDG_SHELL_HEADER}"
    "${XDG_SHELL_CODE}"
)
set_source_files_properties("${XDG_SHELL_CODE}" PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(codotaku-media PRIVATE src "${GENERATED_DIR}" "${VOLK_INCLUDE_DIR}")
target_link_libraries(codotaku-media PRIVATE
    PkgConfig::WAYLAND_CLIENT
    ${CMAKE_DL_LIBS}
)
target_compile_definitions(codotaku-media PRIVATE
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WAYLAND_KHR
)
if(CODOTAKU_VALIDATION)
    target_compile_definitions(codotaku-media PRIVATE CODOTAKU_ENABLE_VALIDATION)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(codotaku-media PRIVATE -Wall -Wextra -Wpedantic)
endif()
