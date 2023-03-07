#version 450

layout (push_constant, std140) uniform PushConstants {
       vec4 iResolution;
       vec4 iMouse;
       float iFrame;
       float iTime;
} constants;

layout (location = 0) in vec2 in_pos;

layout (location = 0) out vec4 out_color;

void main() {
	out_color = vec4(in_pos * 0.5 + 0.5, 0.0, 1.0);
}
