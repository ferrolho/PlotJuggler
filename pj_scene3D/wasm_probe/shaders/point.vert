#version 440

layout(location = 0) in vec2 a_position;

void main() {
    float instance_offset = gl_InstanceIndex == 0 ? -0.24 : 0.24;
    gl_Position = vec4(a_position + vec2(instance_offset, 0.0), 0.0, 1.0);
    gl_PointSize = 80.0;
}
