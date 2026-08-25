# Converts compiled SPIR-V binaries into an includable C++ header with
# byte arrays. Invoked at build time: cmake -DVERT=... -DFRAG=... -DOUT=... -P this.cmake
file(READ "${VERT}" v HEX)
file(READ "${FRAG}" f HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," v_bytes "${v}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," f_bytes "${f}")
file(WRITE "${OUT}"
"// Generated from shaders/tri.vert and shaders/yuv.frag - do not edit.
#pragma once
#include <cstdint>
namespace wlvk_shaders {
alignas(4) inline const uint8_t tri_vert_spv[] = {${v_bytes}};
alignas(4) inline const uint8_t yuv_frag_spv[] = {${f_bytes}};
}  // namespace wlvk_shaders
")
