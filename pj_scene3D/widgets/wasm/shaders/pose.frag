#version 440

layout(location = 0) in vec3 view_normal;
layout(location = 1) in vec4 arrow_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    const vec3 light_direction = normalize(vec3(0.30, 0.55, 0.80));
    const float ambient = 0.35;
    float lambert = max(dot(normalize(view_normal), light_direction), 0.0);
    float shading = ambient + ((1.0 - ambient) * lambert);
    // The QRhi widget renders directly to an sRGB display target; unlike the
    // native HDR pass, this path must not pre-linearize and later re-encode.
    fragment_color = vec4(arrow_color.rgb * shading, arrow_color.a);
}
