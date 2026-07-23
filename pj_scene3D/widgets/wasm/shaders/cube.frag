#version 440

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

layout(location = 0) in vec3 local_position;
layout(location = 1) in vec3 view_normal;
layout(location = 2) in float normalized_value;
layout(location = 3) in float outside_range;
layout(location = 4) in vec4 vertex_color;
layout(location = 0) out vec4 fragment_color;

float cubeEdgeFactor(vec3 local) {
    vec3 distance_to_plane = vec3(0.5) - abs(local);
    float minimum_distance = min(distance_to_plane.x, min(distance_to_plane.y, distance_to_plane.z));
    float maximum_distance = max(distance_to_plane.x, max(distance_to_plane.y, distance_to_plane.z));
    float middle_distance = distance_to_plane.x + distance_to_plane.y + distance_to_plane.z
                            - minimum_distance - maximum_distance;
    return 1.0 - smoothstep(0.0, 0.06, middle_distance);
}

void main() {
    vec4 base;
    if (modes.x == 1) {
        base = vec4(solid_params.rgb, 1.0);
    } else if (modes.x == 2) {
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

    vec3 normal = normalize(view_normal);
    vec3 light_direction = normalize(vec3(0.4, 0.5, 0.8));
    float shading = 0.35 + (0.65 * max(dot(normal, light_direction), 0.0));
    vec3 outlined = mix(base.rgb, base.rgb * 0.4, cubeEdgeFactor(local_position));
    vec3 color = render_mode.x != 0 ? pow(max(outlined, vec3(0.0)), vec3(2.2)) : outlined;
    fragment_color = vec4(color * shading, base.a);
}
