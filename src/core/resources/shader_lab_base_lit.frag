#version 460
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

float ridge(float value, float width) {
    return pow(clamp(1.0 - abs(value) / width, 0.0, 1.0), 3.4);
}

void main() {
    vec2 uv = inTexCoord;
    vec2 p = (uv - vec2(0.5)) * vec2(16.0 / 9.0, 1.0) * 2.25;
    vec2 grid = abs(fract(uv * 18.0) - vec2(0.5));
    float gridLine = 1.0 - smoothstep(0.018, 0.055, min(grid.x, grid.y));
    float core = 1.0 - smoothstep(0.08, 0.92, length(p));
    float river = ridge(sin(p.x * 3.1) + cos(p.y * 4.7), 0.35);
    vec3 wash = vec3(0.03, 0.09, 0.075) + vec3(uv.x * 0.10, uv.y * 0.08, 0.06);
    vec3 color = wash + vec3(0.10, 0.62, 0.38) * gridLine;
    color += vec3(0.30, 1.15, 0.72) * core;
    color += vec3(0.18, 0.82, 0.96) * river * 0.68;
    outColor = vec4(color, 1.0);
}
