#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Camera;
class MaterialDrawLoopTable;
class FrameGraphResourceContainer;
class FrameResources;
class LightContainer;
class MaterialContainer;
class PolygonInstanceContainer;
class ShadowDepthPassContainer;
class VelocityPassContainer;
class VertBufContainer;

// MP-1: the materials one material pass drew in a logical frame: each
// effective material (the record a draw bound, after the pass's variant and
// its draw-loop steps) in the order first drawn, with the indirect draw
// commands issued for it, summed over every invocation of the pass.
struct MaterialPassDrawnMaterial {
    int material = -1;
    std::uint64_t draws = 0;
};

struct MaterialPassDrawnMaterials {
    std::vector<MaterialPassDrawnMaterial> materials;
    std::unordered_map<int, std::size_t> index;

    void add(int material, std::uint64_t draws) {
        const auto [found, inserted] = index.try_emplace(material, materials.size());
        if (inserted) materials.push_back({material, 0});
        materials[found->second].draws += draws;
    }
};

// By pass name, for the material passes the frame recorded.
using MaterialPassDrawTally =
    std::map<std::string, MaterialPassDrawnMaterials, std::less<>>;

struct MaterialRendererDependencies {
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    const FrameResources &frame_resources;
    const FrameGraphResourceContainer
        &frame_graph_resources;
    LightContainer &light_container;
    const Camera &camera;
    glm::mat4 view_projection{1.0f};
    bool first_person_view = false;
    std::uint32_t draw_sort_view_index = 0;
    std::string_view view_family;
    std::optional<std::uint32_t>
        secondary_view_index;
    // LP-2: the draw-step table of the generation being recorded. Null or
    // empty draws every material pass the way it always has.
    const MaterialDrawLoopTable *draw_loops = nullptr;
    // MP-1: where the frame's material passes count what they drew; null
    // counts nothing.
    MaterialPassDrawTally *drawn_materials = nullptr;
};

// LP-2: one indirect draw a material pass issued, with what was bound for
// it. Recorded only while a test installs a sink.
struct MaterialDrawRecord {
    // material, shadow_depth or velocity (picking draws as velocity).
    std::string kind = "material";
    std::string pass;
    // The range's material (the one the draw queue names).
    int material = -1;
    // The record whose descriptor set and GPU slot the draw used.
    int effective_material = -1;
    int pipeline = -1;
    std::uint64_t descriptor_set = 0;
    bool pipeline_bound = false;
    bool descriptor_set_bound = false;
    std::uint32_t gpu_material_index = 0;
    std::uint32_t source_material_index = 0;
    std::uint64_t offset = 0;
    std::uint32_t draw_count = 0;
    bool gpu_draw = false;

    bool operator==(const MaterialDrawRecord &) const = default;
};

DECLARE_MODULE(MaterialRenderer) {
    std::vector<MaterialDrawRecord> *draw_record_sink = nullptr;

  public:
    MaterialRenderer();
    // LP-2: every material-pass draw is appended to the sink until it is
    // reset to null.
    void recordMaterialDrawsForTesting(
        std::vector<MaterialDrawRecord> *sink) noexcept {
        draw_record_sink = sink;
    }
    void render(vk::CommandBuffer cmd_buf, PassId pass_id,
                const PassDefinition &pass,
                const MaterialRendererDependencies &dependencies,
                RenderPassViewInvocation invocation = {}) const;

    void renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                 const PassDefinition &pass,
                                 uint32_t material_start, uint32_t material_count,
                                 const MaterialRendererDependencies &dependencies,
                                 RenderPassViewInvocation invocation = {}) const;
    void renderShadowDepth(vk::CommandBuffer cmd_buf, PassId pass_id,
                           const ShadowDepthPassContainer &shadow_depth_pass_container,
                           const MaterialRendererDependencies &dependencies) const;
    void renderVelocity(vk::CommandBuffer cmd_buf, PassId pass_id,
                        const VelocityPassContainer &velocity_pass_container,
                        const MaterialRendererDependencies &dependencies) const;
};

} // namespace Pelican
