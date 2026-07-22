#pragma once

#include "modeltemplate.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

namespace Pelican {

// CPU-side primitive payload shared by glTF validation, bounds extraction,
// and the eventual GPU upload. Keeping this data-only type outside the buffer
// container lets import/queue tests exercise geometry contracts without
// inheriting allocator or Vulkan buffer headers.
struct CommonPolygonVertData {
    std::vector<std::uint32_t> indices;
    std::vector<glm::vec3> pos;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec4> tangent;
    std::vector<glm::vec2> texcoord;
    std::vector<glm::vec4> color;
    std::vector<glm::i16vec4> joint;
    std::vector<glm::vec4> weight;
    std::vector<MorphTargetVertexData> morph_targets;
    std::uint32_t morph_weight_offset = 0;
};

// Pure CPU extraction shared by validation and live upload paths. Throws when
// a referenced position/delta is non-finite or an index is out of range.
std::shared_ptr<const ModelPrimitiveBoundsSource>
makePrimitiveBoundsSource(const CommonPolygonVertData &data);

} // namespace Pelican
