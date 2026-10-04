#version 460
#extension GL_EXT_ray_tracing : require

// ray_tracing.rchit: a ray tracing set Studio wrote (raygen + miss + closest hit).
// Runs at the nearest surface a ray hits. It says where: the normal needs
// the triangle's vertices, which a set without a ray group layout cannot
// read, so the ray generation shader takes it from the rays beside it.

// The payload the three stages share: xyz is where the ray hit, w is 1
// on a hit and 0 when the miss shader ran.
layout(location = 0) rayPayloadInEXT vec4 hit;

void main() {
    hit = vec4(gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT, 1.0);
}
