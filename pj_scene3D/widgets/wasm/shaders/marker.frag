#version 440

layout(location = 0) in vec4 marker_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    fragment_color = marker_color;
}
