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

layout(location = 0) in vec3 corner_position;
layout(location = 1) in vec3 corner_normal;
layout(location = 2) in vec3 instance_position;
layout(location = 3) in float instance_scalar;
layout(location = 4) in vec4 instance_color;

layout(location = 0) out vec3 local_position;
layout(location = 1) out vec3 view_normal;
layout(location = 2) out float normalized_value;
layout(location = 3) out float outside_range;
layout(location = 4) out vec4 vertex_color;

void main() {
    vec4 source_position = vec4(instance_position, 1.0);
    vec3 instance_in_fixed = (fixed_from_source * source_position).xyz;
    // point_params.x is the sphere radius, hence twice it is the user-facing
    // diameter and the cube side length shared with the native pass.
    vec3 vertex_in_fixed = instance_in_fixed + corner_position * (2.0 * point_params.x);
    gl_Position = view_projection * vec4(vertex_in_fixed, 1.0);

    local_position = corner_position;
    view_normal = mat3(view) * corner_normal;
    float selected = instance_scalar;
    if (modes.w >= 0) {
        vec3 absolute_fixed = instance_in_fixed + render_origin.xyz;
        selected = absolute_fixed[modes.w];
    }
    float span = max(range_params.y - range_params.x, 1.0e-9);
    normalized_value = clamp((selected - range_params.x) / span, 0.0, 1.0);
    outside_range = (selected >= range_params.x && selected <= range_params.y) ? 0.0 : 1.0;
    vertex_color = instance_color;

    if (modes.x == 0 && outside_range > 0.5 && range_params.z <= 0.003) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
}
