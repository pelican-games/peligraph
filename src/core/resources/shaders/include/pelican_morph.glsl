#ifndef PELICAN_MORPH_GLSL
#define PELICAN_MORPH_GLSL

#include "pelican_sets.glsl"

#define PELICAN_MAX_MORPH_WEIGHTS 256
#define PELICAN_MAX_MORPH_VERTICES 262144

struct PelicanMorphInstance {
    uint weight_count;
    uint layout_generation_low;
    uint layout_generation_high;
    uint reserved;
};

struct PelicanMorphVertexMetadata {
    uint delta_offset;
    uint vertex_stride;
    uint weight_offset;
    uint target_count;
};

struct PelicanMorphDelta {
    vec4 position;
    vec4 normal;
    vec4 tangent;
};

struct PelicanMorphedVertex {
    vec3 position;
    vec3 normal;
    vec3 tangent;
};

layout(set = PELICAN_SET_FREE, binding = PELICAN_MORPH_INSTANCE_BINDING, std430)
readonly buffer PelicanMorphInstances {
    PelicanMorphInstance instances[];
} pelicanMorphInstances;

layout(set = PELICAN_SET_FREE, binding = PELICAN_MORPH_WEIGHT_BINDING, std430)
readonly buffer PelicanMorphWeights {
    float weights[];
} pelicanMorphWeights;

layout(set = PELICAN_SET_FREE, binding = PELICAN_PREVIOUS_MORPH_WEIGHT_BINDING, std430)
readonly buffer PelicanPreviousMorphWeights {
    float weights[];
} pelicanPreviousMorphWeights;

layout(set = PELICAN_SET_FREE, binding = PELICAN_MORPH_METADATA_BINDING, std430)
readonly buffer PelicanMorphMetadata {
    PelicanMorphVertexMetadata vertices[];
} pelicanMorphMetadata;

layout(set = PELICAN_SET_FREE, binding = PELICAN_MORPH_DELTA_BINDING, std430)
readonly buffer PelicanMorphDeltas {
    PelicanMorphDelta deltas[];
} pelicanMorphDeltas;

PelicanMorphedVertex pelican_morph_vertex(vec3 position, vec3 normal, vec3 tangent,
                                          int vertex_index, bool previous) {
    PelicanMorphedVertex result = PelicanMorphedVertex(position, normal, tangent);
    uint instance_index = uint(gl_BaseInstance);
    PelicanMorphInstance instance = pelicanMorphInstances.instances[instance_index];
    if (instance.weight_count == 0 || vertex_index < 0 ||
        uint(vertex_index) >= PELICAN_MAX_MORPH_VERTICES) {
        return result;
    }
    PelicanMorphVertexMetadata metadata =
        pelicanMorphMetadata.vertices[uint(vertex_index)];
    if (metadata.target_count == 0) return result;

    uint weight_base = instance_index * PELICAN_MAX_MORPH_WEIGHTS +
                       metadata.weight_offset;
    for (uint target = 0; target < metadata.target_count; ++target) {
        float weight = previous
                           ? pelicanPreviousMorphWeights.weights[weight_base + target]
                           : pelicanMorphWeights.weights[weight_base + target];
        PelicanMorphDelta delta = pelicanMorphDeltas.deltas[
            metadata.delta_offset + target * metadata.vertex_stride];
        result.position += weight * delta.position.xyz;
        result.normal += weight * delta.normal.xyz;
        result.tangent += weight * delta.tangent.xyz;
    }
    return result;
}

#endif
