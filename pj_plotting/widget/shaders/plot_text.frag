// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(binding = 0) uniform sampler2D text_texture;

void main() {
    frag_color = texture(text_texture, v_uv);
}
