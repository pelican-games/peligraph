#ifndef PELICAN_VAT_GLSL
#define PELICAN_VAT_GLSL

struct PelicanVatVertex {
    vec3 position;
    vec3 normal;
};

PelicanVatVertex pelican_vat_vertex(
    sampler2D positions, sampler2D normals,
    vec4 boundsMinFrameCount, vec4 boundsExtentFps, ivec4 flags,
    float time, int vertexIndex, vec3 inNormal) {
    int textureWidth = textureSize(positions, 0).x;
    int vertex = clamp(vertexIndex - flags.x, 0, max(textureWidth - 1, 0));
    float frameCount = max(boundsMinFrameCount.w, 1.0);
    float frame = max(time, 0.0) * boundsExtentFps.w;
    frame = flags.y != 0 ? mod(frame, frameCount)
                         : clamp(frame, 0.0, frameCount - 1.0);
    float frame0 = floor(frame);
    float frame1 = flags.y != 0 ? mod(frame0 + 1.0, frameCount)
                               : min(frame0 + 1.0, frameCount - 1.0);
    float blend = fract(frame);
    vec3 position0 = boundsMinFrameCount.xyz +
        texelFetch(positions, ivec2(vertex, int(frame0)), 0).rgb * boundsExtentFps.xyz;
    vec3 position1 = boundsMinFrameCount.xyz +
        texelFetch(positions, ivec2(vertex, int(frame1)), 0).rgb * boundsExtentFps.xyz;
    PelicanVatVertex result;
    result.position = mix(position0, position1, blend);
    result.normal = inNormal;
    if (flags.z != 0) {
        result.normal = normalize(mix(
            texelFetch(normals, ivec2(vertex, int(frame0)), 0).rgb,
            texelFetch(normals, ivec2(vertex, int(frame1)), 0).rgb, blend));
    }
    return result;
}

#endif
