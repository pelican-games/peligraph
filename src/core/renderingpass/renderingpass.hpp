#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(RenderingPassId, int)
PELICAN_DEFINE_HANDLE(PassId, int)

PELICAN_DEFINE_HANDLE(GlobalRenderTargetId, int)

inline constexpr int invalidRenderingPassIdValue = -1;
inline constexpr int invalidRenderTargetIdValue = -1;
inline constexpr int swapchainRenderTargetIdValue = -2;

inline constexpr RenderingPassId invalidRenderingPassId() {
    return RenderingPassId{invalidRenderingPassIdValue};
}

inline constexpr bool isValidRenderingPassId(RenderingPassId pass_id) {
    return pass_id.value >= 0;
}

inline constexpr GlobalRenderTargetId noRenderTargetId() {
    return GlobalRenderTargetId{invalidRenderTargetIdValue};
}

inline constexpr GlobalRenderTargetId swapchainRenderTargetId() {
    return GlobalRenderTargetId{swapchainRenderTargetIdValue};
}

inline constexpr bool isConcreteRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value >= 0;
}

inline constexpr bool isSpecialRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value < 0;
}

inline constexpr bool isSwapchainRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value == swapchainRenderTargetIdValue;
}

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
    std::string vert_shader_path;
    std::string frag_shader_path;
    FullscreenPushConstantData push_constants = FullscreenPushConstantData::eNone;
    bool uses_light_data = false;
};

struct UiPassInfo {};

using PassInfo = std::variant<MaterialPassInfo, FullscreenPassInfo, UiPassInfo>;

struct PassDefinition {
    PassDefinition() : output_depth{noRenderTargetId()} {}

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

struct CompiledPass {
    PassDefinition definition;
    PassId pass_id;
};

struct CompiledRenderingPass {
    std::string name;
    std::vector<CompiledPass> passes;
};

} // namespace Pelican
