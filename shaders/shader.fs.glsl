#version 450

layout (push_constant, std140) uniform PushConstants {
       vec4 iResolution;
       vec4 iMouse;
       float iFrame;
       float iTime;
       int tex_width;
       int tex_layer;
} constants;

layout (location = 0) in vec2 in_pos;

layout (location = 0) out vec4 out_color;

layout (set = 0, binding = 0) uniform sampler3D texSampler;

void main() {
	vec4 s = texelFetch(texSampler, ivec3(in_pos * constants.tex_width, constants.tex_layer), 0);
	out_color = vec4(s.xyz, 1.0);
}
