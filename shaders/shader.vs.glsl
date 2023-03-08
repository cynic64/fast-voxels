#version 450

layout (push_constant, std140) uniform PushConstants {
       vec4 iResolution;
       vec4 iMouse;
       float iFrame;
       float iTime;
} constants;

vec2 positions[6] = vec2[](
        vec2(-1.0, -1.0),
        vec2(1.0, -1.0),
        vec2(1.0, 1.0),
        vec2(-1.0, -1.0),
        vec2(1.0, 1.0),
        vec2(-1.0, 1.0)
);

layout (location = 0) out vec2 out_pos;

void main() {
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);

        out_pos = gl_Position.xy * 0.5 + 0.5;
}
