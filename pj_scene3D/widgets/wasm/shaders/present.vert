#version 440

layout(location = 0) out vec2 texture_uv;

void main() {
    texture_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(texture_uv * 2.0 - 1.0, 0.0, 1.0);
}
