#version 440

layout(binding = 1) uniform sampler3D volume_texture;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_volume_color;

void main() {
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0)
    );
    vec2 position = positions[gl_VertexIndex];
    v_uv = position * 0.5 + 0.5;
    // Match the production voxel shader's capability dependency: an exact
    // vertex-stage fetch from a 3D texture, forwarded for framebuffer proof.
    v_volume_color = texelFetch(volume_texture, ivec3(1, 1, 1), 0);
    gl_Position = vec4(position, 0.0, 1.0);
}
