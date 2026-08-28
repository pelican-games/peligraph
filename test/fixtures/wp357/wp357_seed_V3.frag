#version 460
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float x = float(p.x) / float(9);
    float y = float(p.y) / float(4);
    float edge = (p.x == 0 ? 0.011 : 0.0) +
                 (p.x == 9 ? 0.019 : 0.0) +
                 (p.y == 0 ? 0.007 : 0.0) +
                 (p.y == 4 ? 0.013 : 0.0);
    float stage = float(3);
    outColor = vec4(
        0.018 + 0.004 * stage + 0.027 * x + 0.016 * y + edge,
        0.021 + 0.003 * stage + 0.014 * x + 0.031 * y + 0.5 * edge,
        0.015 + 0.005 * stage + 0.023 * x + 0.021 * y + 0.75 * edge,
        0.024 + 0.004 * stage + 0.019 * x + 0.017 * y + 0.6 * edge);
}
