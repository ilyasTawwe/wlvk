cmake_minimum_required(VERSION 4.0)
project(wlvk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# No C++ modules here; skipping the scan avoids GCC-only flags
# (-fmodules-ts, -fmodule-mapper, ...) that clangd cannot parse.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(WLVK_BUILD_EXAMPLES "Build wlvk examples" ON)

add_subdirectory(lib/wlvk)

if(WLVK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
