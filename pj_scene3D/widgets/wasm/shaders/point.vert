#version 440

layout(std140, binding = 0) uniform PointUniforms {
    mat4 view_projection;
    mat4 fixed_from_source;
    mat4 view;
    vec4 render_origin;
    vec4 point_params;  // world radius, pixel size, viewport height, projection[1][1]
    vec4 range_params;  // range min, range max, outside alpha, use world size
    vec4 solid_params;  // solid rgb, sphere flag
    ivec4 modes;        // color mode, colormap, invert, scalar axis
};

layout(location = 0) in vec3 position;
layout(location = 1) in float scalar_value;
layout(location = 2) in vec4 point_color;

layout(location = 0) out float normalized_value;
layout(location = 1) out float outside_range;
layout(location = 2) out vec4 vertex_color;

void main() {
    vec4 source_position = vec4(position, 1.0);
    gl_Position = view_projection * fixed_from_source * source_position;

    if (range_params.w > 0.5) {
        float depth_divisor = abs(gl_Position.w);
        if (depth_divisor < 1.0e-4) {
            depth_divisor = 1.0;
        } else if (render_origin.w > 0.5 && depth_divisor > 5.0) {
            // Match the desktop pass: retain distant-cloud readability without
            // letting projected world-size points dominate the near field.
            depth_divisor = sqrt(depth_divisor * 5.0);
        }
        float diameter_pixels = point_params.x * point_params.w * point_params.z / depth_divisor;
        gl_PointSize = clamp(diameter_pixels, 1.0, 64.0);
    } else {
        gl_PointSize = clamp(point_params.y, 1.0, 32.0);
    }

    float selected = scalar_value;
    if (modes.w >= 0) {
        vec3 fixed_position = (fixed_from_source * source_position).xyz + render_origin.xyz;
        selected = fixed_position[modes.w];
    }
    float span = max(range_params.y - range_params.x, 1.0e-9);
    normalized_value = clamp((selected - range_params.x) / span, 0.0, 1.0);
    outside_range = (selected >= range_params.x && selected <= range_params.y) ? 0.0 : 1.0;
    vertex_color = point_color;

    if (modes.x == 0 && outside_range > 0.5 && range_params.z <= 0.003) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 0.0;
    }
}
