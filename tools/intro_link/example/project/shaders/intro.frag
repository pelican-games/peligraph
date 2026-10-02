#version 450

// Everything the picture depends on comes through the push constant:
// the time, the aspect ratio and the curve tracks (fade, pulse, scene).
layout(push_constant) uniform Push {
    float time;
    float aspect;
    float track[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// Edit this colour while the development form runs to see the pipeline rebuilt.
const vec3 BASE = vec3(1.0, 0.45, 0.15);

float scene_distance(vec3 p) {
    float radius = 0.7 + 0.35 * pc.track[1];
    float scene = pc.track[2];
    if (scene > 1.5) {                       // scene 2: a field of spheres
        p.xz = mod(p.xz + 1.0, 2.0) - 1.0;
        radius *= 0.5;
    }
    float d = length(p) - radius;
    if (scene > 0.5 && scene < 1.5)          // scene 1: ripples on the sphere
        d += 0.06 * sin(10.0 * p.x + 4.0 * pc.time) * sin(10.0 * p.y) * sin(10.0 * p.z);
    return d;
}

vec3 scene_normal(vec3 p) {
    const vec2 e = vec2(0.002, 0.0);
    return normalize(vec3(scene_distance(p + e.xyy) - scene_distance(p - e.xyy),
                          scene_distance(p + e.yxy) - scene_distance(p - e.yxy),
                          scene_distance(p + e.yyx) - scene_distance(p - e.yyx)));
}

void main() {
    vec2 p = (uv * 2.0 - 1.0) * vec2(pc.aspect, -1.0);
    float a = 0.4 * pc.time;
    vec3 ro = vec3(3.0 * sin(a), 0.6, 3.0 * cos(a));
    vec3 forward = normalize(-ro);
    vec3 right = normalize(cross(forward, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(right, forward);
    vec3 rd = normalize(forward * 1.6 + right * p.x + up * p.y);

    // background: stripes that scroll with time
    vec3 color = mix(vec3(0.05, 0.06, 0.12), BASE.bgr * 0.5,
                     0.5 + 0.5 * sin(20.0 * p.y + 3.0 * pc.time));
    float t = 0.0;
    for (int i = 0; i < 96; ++i) {
        float d = scene_distance(ro + rd * t);
        if (d < 0.001) {
            vec3 n = scene_normal(ro + rd * t);
            vec3 light = normalize(vec3(0.6, 0.8, 0.4));
            float diffuse = max(dot(n, light), 0.0);
            float rim = pow(1.0 - max(dot(n, -rd), 0.0), 3.0);
            color = BASE * (0.15 + diffuse) + rim * vec3(0.6, 0.8, 1.0) * pc.track[1];
            break;
        }
        t += d;
        if (t > 20.0) break;
    }
    out_color = vec4(color * pc.track[0], 1.0);
}
