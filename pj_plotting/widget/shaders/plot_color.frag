// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 frag_color;

void main() {
    frag_color = v_color;
}
