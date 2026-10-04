#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// engine://ssao -- screen-space ambient occlusion over the G-buffer.
//
// What it computes, per pixel:
//   p  the pixel's surface point in view space (world G-buffer x view matrix)
//   n  its view-space normal (normal G-buffer, decoded from [0, 1])
//   F  an orthonormal frame whose third axis is n; a per-pixel seed direction
//      picks its rotation about n so neighbouring pixels probe differently
//   q  for each entry o of PROBE_OFFSETS: q = p + (F o) * PROBE_REACH
// Each probe q is projected to the screen and the surface the G-buffer holds
// at that pixel, s, is read back. The camera looks down -Z, so s lies in
// front of q when s.z > q.z; q is "blocked" when s.z >= q.z + DEPTH_EPSILON.
// A blocked probe is discounted when s is far from p in depth -- a wall well
// in front of the pixel does not enclose it -- by
//   smoothstep(0, 1, PROBE_REACH / |p.z - s.z|).
// The output is the open fraction 1 - (sum of blocked weights) / PROBE_COUNT
// in R: 1 is fully open, 0 fully enclosed. engine://ssao_blur smooths it.
//
// Method: the screen-space depth-buffer estimate of ambient occlusion of
// M. Mittring, "Finding Next Gen: CryEngine 2" (SIGGRAPH 2007 course notes)
// and D. Shanmugam, O. Arikan, "Hardware Accelerated Ambient Occlusion
// Techniques on GPUs" (I3D 2007), with the probes oriented by the surface
// normal.

const int PROBE_COUNT = 32;
// View-space distances: how far the probes reach from the surface, and how
// far in front of a probe a surface must be to count as blocking it.
const float PROBE_REACH = 0.5;
const float DEPTH_EPSILON = 0.025;

// Probe offsets in F's coordinates (x, y across the surface, z along n),
// inside the unit ball. They are not confined to the half above the surface
// (some have z < 0), so an open flat surface also reads partly blocked. The
// table is data that every SSAO image and test/fixtures/wp338_ssao depends
// on; changing it changes them.
const vec3 PROBE_OFFSETS[PROBE_COUNT] = vec3[PROBE_COUNT](
    vec3(0.5381, 0.1856, -0.4319), vec3(0.1379, 0.2486, 0.4430),
    vec3(0.3371, 0.5679, -0.0057), vec3(-0.6999, -0.0451, -0.0019),
    vec3(0.0689, -0.1598, -0.8547), vec3(0.0560, 0.0069, -0.1843),
    vec3(-0.0146, 0.1402, 0.0762), vec3(0.0100, -0.1924, -0.0344),
    vec3(-0.3577, -0.5301, -0.4358), vec3(-0.3169, 0.1063, 0.0158),
    vec3(0.0103, -0.5869, 0.0046), vec3(-0.0897, -0.4940, 0.3287),
    vec3(0.7119, -0.0154, -0.0918), vec3(-0.0533, 0.0596, -0.5411),
    vec3(0.0352, -0.0631, 0.5460), vec3(-0.4776, 0.2847, -0.5617),
    vec3(-0.5523, 0.3016, -0.2981), vec3(0.5866, -0.5577, 0.0125),
    vec3(0.0009, -0.1703, 0.2038), vec3(0.6225, 0.1332, 0.1240),
    vec3(0.0019, 0.2248, -0.2104), vec3(0.0003, 0.5564, 0.3860),
    vec3(0.0004, -0.1932, 0.1502), vec3(0.0003, -0.4137, 0.2820),
    vec3(0.0004, 0.1705, -0.4882), vec3(0.0004, 0.4439, 0.3384),
    vec3(0.0001, -0.0016, 0.2195), vec3(0.0001, 0.2185, -0.3243),
    vec3(0.0001, -0.5186, 0.1853), vec3(0.0001, -0.2142, -0.3803),
    vec3(0.0000, 0.0016, -0.282), vec3(0.0000, -0.0008, 0.4939)
);

// The surface point the G-buffer holds at uv, in view space.
vec3 viewSpacePointAt(vec2 uv) {
    vec3 world = pelican_sample_world(uv, pelican_view_index()).xyz;
    return (pelicanFrame.view * vec4(world, 1.0)).xyz;
}

// The surface normal the G-buffer holds at uv, in view space.
vec3 viewSpaceNormalAt(vec2 uv) {
    vec3 encoded = pelican_sample_normal(uv, pelican_view_index()).xyz;
    return normalize((pelicanFrame.view * vec4(encoded * 2.0 - 1.0, 0.0)).xyz);
}

// A value in [0, 1) per component from a 2D coordinate: the widely used
// fract(sin(x) * large) hash. Only the direction of the result is used.
vec3 frameSeed(vec2 coordinate) {
    float phase = dot(coordinate, vec2(12.9898, 78.233));
    float otherPhase = dot(coordinate, vec2(4.898, 7.23));
    return vec3(fract(sin(phase) * 43758.5453),
                fract(cos(phase) * 43758.5453),
                fract(sin(otherPhase) * 23458.5453));
}

// The frame F: Gram-Schmidt removes the seed's component along the unit
// normal, which leaves the first tangent axis.
mat3 probeFrame(vec3 unitNormal, vec3 seedDirection) {
    vec3 across = normalize(seedDirection - unitNormal * dot(seedDirection, unitNormal));
    vec3 along = cross(unitNormal, across);
    return mat3(across, along, unitNormal);
}

// The texture coordinate a view-space point projects to.
vec2 screenUvOf(vec3 viewPoint) {
    vec4 clip = pelicanFrame.projection * vec4(viewPoint, 1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}

void main() {
    vec3 surfacePoint = viewSpacePointAt(inUV);
    mat3 frame = probeFrame(viewSpaceNormalAt(inUV),
                            normalize(frameSeed(inUV * 1000.0)));

    float blockedWeight = 0.0;
    for (int probeIndex = 0; probeIndex < PROBE_COUNT; ++probeIndex) {
        vec3 probe = surfacePoint + (frame * PROBE_OFFSETS[probeIndex]) * PROBE_REACH;
        vec3 seen = viewSpacePointAt(screenUvOf(probe));
        float nearness = smoothstep(0.0, 1.0, PROBE_REACH / abs(surfacePoint.z - seen.z));
        blockedWeight += float(seen.z >= probe.z + DEPTH_EPSILON) * nearness;
    }

    float openFraction = 1.0 - blockedWeight / float(PROBE_COUNT);
    outColor = vec4(vec3(openFraction), 1.0);
}
