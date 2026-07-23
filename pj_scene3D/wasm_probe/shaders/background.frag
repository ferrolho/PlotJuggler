#version 440

layout(binding = 2) uniform sampler2D float_texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_volume_color;
layout(location = 0) out vec4 frag_color;

void main() {
    if (v_uv.x < 0.5) {
        frag_color = v_volume_color;
    } else {
        frag_color = texture(float_texture, vec2((v_uv.x - 0.5) * 2.0, v_uv.y));
    }
}
