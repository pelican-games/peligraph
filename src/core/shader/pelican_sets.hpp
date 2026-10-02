#pragma once

#include <cstdint>

namespace Pelican {

// Binding numbers are only unique within one descriptor set, so the list below
// is grouped by set. Read flat it looks like it collides -- the ray query TLAS
// and the material buffer are both 6, the light UBO and the skin palette are
// both 2 -- but each pair lives in a different set. Keep a constant in the
// block of the set it is declared in; src/core/resources/shaders/include/
// pelican_sets.glsl is the GLSL twin of this file and must keep the same
// name/value pairs.

inline constexpr uint32_t PELICAN_SET_FRAME = 0;
inline constexpr uint32_t PELICAN_SET_PASS_INPUT = 1;
inline constexpr uint32_t PELICAN_SET_MATERIAL = 2;
inline constexpr uint32_t PELICAN_SET_FREE = 3;

// Set 0 -- PELICAN_SET_FRAME: per-frame engine data, one layout for every
// pipeline. Declared in shaders/include/pelican_frame.glsl,
// shaders/include/pelican_lighting_v1.glsl and the ray query shaders; built in
// frameDescriptorSetLayoutBindings() (shader/pipelinefactory.cpp) and written
// in renderer/frameresources.cpp. validateFrameBindings() rejects anything in
// set 0 that is not one of these.
inline constexpr uint32_t PELICAN_FRAME_UBO_BINDING = 0;
inline constexpr uint32_t PELICAN_OBJECT_BUFFER_BINDING = 1;
inline constexpr uint32_t PELICAN_LIGHT_UBO_BINDING = 2;
inline constexpr uint32_t PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING = 3;
inline constexpr uint32_t PELICAN_FRAME_RESOLUTION_UBO_BINDING = 4;
inline constexpr uint32_t PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING = 5;
inline constexpr uint32_t PELICAN_RAY_QUERY_TLAS_BINDING = 6;

// Set 1 -- PELICAN_SET_PASS_INPUT: the attachments a pass reads from the pass
// before it. Bindings are the input index itself (0..7, see
// PELICAN_DECLARE_INPUT_n in shaders/include/pelican_view.glsl and
// maxMaterialPassInputs in material/materiallimits.hpp), so this set has no
// named constants.

// Set 2 -- PELICAN_SET_MATERIAL: the material's own textures and table.
// Bindings 0..5 are the fixed glTF samplers plus the two VAT samplers and are
// named in material/materiallimits.hpp (baseColorBinding .. vatNormalBinding);
// the three below continue that numbering, and the custom textures a surface
// declares occupy PELICAN_MATERIAL_CUSTOM_TEXTURE_FIRST_BINDING and up (see
// shader/spvlink.hpp and material/materialcontainer.cpp).
inline constexpr uint32_t PELICAN_MATERIAL_BUFFER_BINDING = 6;
inline constexpr uint32_t PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING = 7;
inline constexpr uint32_t PELICAN_MATERIAL_CUSTOM_TEXTURE_FIRST_BINDING = 8;

// Set 3 -- PELICAN_SET_FREE: whatever the drawing feature needs. Bindings 0
// and 1 are taken by the overlay renderers that own the whole set (the vertex
// buffer and, for text, the glyph atlas: resources/debug_draw.vert,
// resources/debug_text.{vert,frag}, resources/gizmo.vert), so the skinning,
// morph and material-instance buffers below start at 2. Declared in
// shaders/include/pelican_skinning.glsl, pelican_morph.glsl and
// pelican_material_instance.glsl. A ray tracing pipeline layout uses set 3 for
// the ray tables instead (rayTableDescriptorSet in shader/raygrouplayout.hpp);
// that is a different pipeline layout, so the numbers do not collide.
inline constexpr uint32_t PELICAN_SKIN_PALETTE_BINDING = 2;
inline constexpr uint32_t PELICAN_PREVIOUS_SKIN_PALETTE_BINDING = 3;
inline constexpr uint32_t PELICAN_MORPH_INSTANCE_BINDING = 4;
inline constexpr uint32_t PELICAN_MORPH_WEIGHT_BINDING = 5;
inline constexpr uint32_t PELICAN_PREVIOUS_MORPH_WEIGHT_BINDING = 6;
inline constexpr uint32_t PELICAN_MORPH_METADATA_BINDING = 7;
inline constexpr uint32_t PELICAN_MORPH_DELTA_BINDING = 8;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING = 9;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING = 10;
inline constexpr uint32_t PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING = 11;

// Push constants: the engine owns the first half of the range, the shader the
// second. Not part of any descriptor set.
inline constexpr uint32_t PELICAN_PUSH_ENGINE_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_SHADER_BYTES = 64;
inline constexpr uint32_t PELICAN_PUSH_TOTAL_BYTES = PELICAN_PUSH_ENGINE_BYTES + PELICAN_PUSH_SHADER_BYTES;
// LC-1: the element and count of the graph.loop copy a pass or compute task
// is (two uints, shaders/include/pelican_loop.glsl), in the shader's half.
// The engine pushes them to every stage whose range holds them.
inline constexpr uint32_t PELICAN_PUSH_LOOP_OFFSET = 112;
inline constexpr uint32_t PELICAN_PUSH_LOOP_BYTES = 8;
static_assert(PELICAN_PUSH_LOOP_OFFSET >= PELICAN_PUSH_ENGINE_BYTES &&
              PELICAN_PUSH_LOOP_OFFSET + PELICAN_PUSH_LOOP_BYTES <= PELICAN_PUSH_TOTAL_BYTES);

} // namespace Pelican
