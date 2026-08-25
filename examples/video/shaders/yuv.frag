#version 450

// The sampler carries a VkSamplerYcbcrConversion: texture() returns RGB.
layout(binding = 0) uniform sampler2D u_yuv;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(u_yuv, v_uv);
}
