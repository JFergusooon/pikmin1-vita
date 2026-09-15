#version 450

// Single-texture modulate: the one-stage TEV configuration used by the
// opening's materials 2, 5 and 6.

layout(binding = 0) uniform sampler2D tex0;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_texcoord0;

layout(location = 0) out vec4 out_color;

void main() {
  out_color = texture(tex0, v_texcoord0) * v_color;
}
