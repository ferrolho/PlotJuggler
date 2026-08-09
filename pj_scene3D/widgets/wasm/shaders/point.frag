#version 440
#extension GL_GOOGLE_include_directive : require
#include "../../shaders/cube/cube_lighting.glslinc"

layout(std140, binding = 0) uniform PointUniforms {
    mat4 view_projection;
    mat4 fixed_from_source;
    mat4 view;
    vec4 render_origin;
    vec4 point_params;
    vec4 range_params;
    vec4 solid_params;
    ivec4 modes;
};

layout(binding = 1) uniform sampler2D color_lut;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in float normalized_value;
layout(location = 1) in float outside_range;
layout(location = 2) in vec4 vertex_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    float shading = 1.0;
    if (solid_params.w > 0.5) {
        vec2 coordinate = (2.0 * gl_PointCoord) - 1.0;
        float radius_squared = dot(coordinate, coordinate);
        if (radius_squared > 1.0) {
            discard;
        }
        vec3 normal = vec3(coordinate.x, -coordinate.y, sqrt(max(0.0, 1.0 - radius_squared)));
        shading = cubeLambert(normal);
    }

    vec4 base;
    if (modes.x == 1) {
        base = vec4(solid_params.rgb, 1.0);
    } else if (modes.x == 2) {
        // Match the native point-cloud pass: packed RGB/RGBA supplies display
        // colour, while range opacity remains the only transparency control.
        base = vec4(vertex_color.rgb, 1.0);
    } else {
        float lookup = modes.z != 0 ? 1.0 - normalized_value : normalized_value;
        float row = (float(modes.y) + 0.5) / 4.0;
        base = texture(color_lut, vec2(lookup, row));
        if (outside_range > 0.5) {
            base.a *= range_params.z;
        }
    }
    if (base.a <= 0.003) {
        discard;
    }
    vec3 color = render_mode.x != 0 ? pow(max(base.rgb, vec3(0.0)), vec3(2.2)) : base.rgb;
    fragment_color = vec4(color * shading, base.a);
}
