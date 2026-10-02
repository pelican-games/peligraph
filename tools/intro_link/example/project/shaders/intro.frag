#version 450

// Everything the picture depends on comes through the push constant:
// v = (time, fade, pulse, scene), the last three from the curve tracks.
// Written for size: one distance function, no branches on the scene, and the
// push constant read once per function.
layout(push_constant) uniform Push { vec4 v; } pc;

layout(location = 0) out vec4 out_color;

// Edit this colour while the development form runs to see the pipeline rebuilt.
const vec3 BASE = vec3(1.0, 0.45, 0.15);

// scene 0: a sphere, 1: ripples on the sphere, 2: a field of smaller spheres.
float scene_distance(vec3 p) {
    vec4 v = pc.v;
    float field = step(1.5, v.w);
    vec3 q = p;
    q.xz = mix(q.xz, mod(q.xz + 1.0, 2.0) - 1.0, field);
    vec3 r = sin(10.0 * q + vec3(4.0 * v.x, 0.0, 0.0));
    return length(q) - 0.35 * (2.0 + v.z) * (1.0 - 0.5 * field)
         + 0.06 * v.w * (2.0 - v.w) * r.x * r.y * r.z;
}

void main() {
    vec4 v = pc.v;
    // the 1280x720 window with y down; the camera's up is flipped to match
    vec2 p = (gl_FragCoord.xy - vec2(640.0, 360.0)) / 360.0;
    float a = 0.4 * v.x;
    vec3 ro = vec3(3.0 * sin(a), 0.6, 3.0 * cos(a));
    vec3 forward = normalize(-ro);
    vec3 right = vec3(cos(a), 0.0, -sin(a));
    vec3 rd = normalize(mat3(right, cross(forward, right), forward) * vec3(p, 1.6));

    // background: stripes that scroll with time
    vec3 color = BASE.bgr * (0.3 + 0.2 * sin(3.0 * v.x - 20.0 * p.y));
    vec3 q = ro;
    float t = 0.0, d = 1.0;
    for (int i = 0; i < 96 && d > 0.001 && t < 20.0; ++i) {
        q = ro + rd * t;
        d = scene_distance(q);
        t += d;
    }
    if (d < 0.001) {
        vec3 n = vec3(0.0);
        for (int i = 0; i < 3; ++i) {
            vec3 e = vec3(0.0);
            e[i] = 0.001;
            n += e * (scene_distance(q + e) - d);
        }
        n = normalize(n);
        float diffuse = max(dot(n, vec3(0.6, 0.8, 0.0)), 0.0);
        float rim = pow(1.0 + min(dot(n, rd), 0.0), 3.0);
        color = BASE * (0.15 + diffuse) + rim * vec3(0.6, 0.8, 1.0) * v.z;
    }
    out_color = color.xyzz * v.y; // the alpha is not shown (opaque swapchain)
}
