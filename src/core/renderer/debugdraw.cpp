#include "debugdraw.hpp"
#include "frameresources.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {

DebugDraw::DebugDraw() = default;
DebugDraw::~DebugDraw() = default;

PassId DebugDraw::registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                               ShaderBundleId frag_shader,
                               std::vector<std::string> shader_defines,
                               vk::SampleCountFlagBits samples,
                               bool ray_query, std::string acceleration_structure) {
    return passes.registerPass(color_format, vert_shader, frag_shader,
                               std::move(shader_defines), samples, ray_query,
                               std::move(acceleration_structure));
}

void DebugDraw::line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 color) {
    line(from_ndc, to_ndc, color, color);
}

void DebugDraw::line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 from_color, glm::vec4 to_color) {
    if (!passes.enabled()) {
        return;
    }
    vertices.push_back(DebugDrawVertex{glm::vec4{from_ndc, 1.0f}, from_color});
    vertices.push_back(DebugDrawVertex{glm::vec4{to_ndc, 1.0f}, to_color});
}

void DebugDraw::clear() {
    vertices.clear();
}

void DebugDraw::render(vk::CommandBuffer cmd_buf, PassId pass_id, const FrameResources &frame_resources) {
    if (!passes.enabled() || vertices.empty()) {
        return;
    }

    auto *record = passes.findPass(pass_id);
    if (record == nullptr) {
        throw std::runtime_error("DebugDraw pass pipeline not found");
    }

    passes.drawVertices(cmd_buf, *record, vertices.data(), vertices.size(),
                        frame_resources);
    vertices.clear();
}

} // namespace Pelican
