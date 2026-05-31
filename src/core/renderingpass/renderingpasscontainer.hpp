#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/shader.hpp"
#include "renderingpass.hpp"
#include "rendertargetcontainer.hpp"
#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct MaterialPassInfo {
    uint32_t material_start = 0;
    uint32_t material_count = 0;
};

enum class FullscreenPushConstantData {
    eNone,
    eCameraPosition,
    eProjectionView,
};

struct FullscreenPassInfo {
    GlobalShaderId vert_shader;
    GlobalShaderId frag_shader;
    FullscreenPushConstantData push_constants = FullscreenPushConstantData::eNone;
    bool uses_light_data = false;
};

struct UiPassInfo {};

using PassInfo = std::variant<MaterialPassInfo, FullscreenPassInfo, UiPassInfo>;

struct PassDefinition {
    PassDefinition() : output_depth{-1} {}

    std::string name;

    std::vector<GlobalRenderTargetId> output_color;
    GlobalRenderTargetId output_depth;
    std::vector<GlobalRenderTargetId> input_targets;

    PassInfo pass_info = MaterialPassInfo{};

    vk::AttachmentLoadOp color_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp color_store_op = vk::AttachmentStoreOp::eStore;
    vk::ClearColorValue clear_color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};

    bool isMaterial() const { return std::holds_alternative<MaterialPassInfo>(pass_info); }
    bool isFullscreen() const { return std::holds_alternative<FullscreenPassInfo>(pass_info); }
    bool isUi() const { return std::holds_alternative<UiPassInfo>(pass_info); }

    MaterialPassInfo &materialInfo() { return std::get<MaterialPassInfo>(pass_info); }
    const MaterialPassInfo &materialInfo() const { return std::get<MaterialPassInfo>(pass_info); }
    FullscreenPassInfo &fullscreenInfo() { return std::get<FullscreenPassInfo>(pass_info); }
    const FullscreenPassInfo &fullscreenInfo() const { return std::get<FullscreenPassInfo>(pass_info); }
};

struct RenderingPassDefinition {
    std::string name;
    std::vector<PassDefinition> passes;
};

DECLARE_MODULE(RenderingPassContainer) {
    struct InternalRenderingPass {
        RenderingPassDefinition definition;
        std::vector<PassId> pass_ids;
    };

    ResourceContainer<RenderingPassId, InternalRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    RenderingPassId registerRenderingPass(const RenderingPassDefinition &definition);
    RenderingPassId getRenderingPassIdByName(const std::string &name) const;
    std::span<const PassId> getPasses(RenderingPassId rendering_pass_id) const;
    const PassDefinition &getPassDefinition(RenderingPassId rendering_pass_id, size_t pass_index) const;
};

} // namespace Pelican
