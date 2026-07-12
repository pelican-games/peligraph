#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shaderreference.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(RenderingPassId, int)
PELICAN_DEFINE_HANDLE(PassId, int)

PELICAN_DEFINE_HANDLE(GlobalRenderTargetId, int)
PELICAN_DEFINE_HANDLE(ComputeTaskId, int)

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
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
    FullscreenPushConstantData push_constants = FullscreenPushConstantData::eNone;
    bool uses_light_data = false;
};

struct DebugDrawPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct DebugTextPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct ShadowDepthPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
};

struct UiPassInfo {};

#if PELICAN_WITH_IMGUI
struct ImGuiPassInfo {};
#endif

using PassInfo = std::variant<MaterialPassInfo, FullscreenPassInfo, DebugDrawPassInfo, DebugTextPassInfo,
                              ShadowDepthPassInfo, UiPassInfo
#if PELICAN_WITH_IMGUI
                              , ImGuiPassInfo
#endif
                              >;

struct PassDefinition {
    PassDefinition() : output_depth{noRenderTargetId()} {}

    std::string name;

    std::vector<GlobalRenderTargetId> output_color;
    GlobalRenderTargetId output_depth;
    std::vector<GlobalRenderTargetId> input_targets;
    std::vector<std::string> input_buffers;

    PassInfo pass_info = MaterialPassInfo{};

    vk::AttachmentLoadOp color_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp color_store_op = vk::AttachmentStoreOp::eStore;
    vk::AttachmentLoadOp depth_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp depth_store_op = vk::AttachmentStoreOp::eDontCare;
    vk::ClearColorValue clear_color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};

    bool isMaterial() const { return std::holds_alternative<MaterialPassInfo>(pass_info); }
    bool isFullscreen() const { return std::holds_alternative<FullscreenPassInfo>(pass_info); }
    bool isDebugDraw() const { return std::holds_alternative<DebugDrawPassInfo>(pass_info); }
    bool isDebugText() const { return std::holds_alternative<DebugTextPassInfo>(pass_info); }
    bool isShadowDepth() const { return std::holds_alternative<ShadowDepthPassInfo>(pass_info); }
    bool isUi() const { return std::holds_alternative<UiPassInfo>(pass_info); }
#if PELICAN_WITH_IMGUI
    bool isImGui() const { return std::holds_alternative<ImGuiPassInfo>(pass_info); }
#endif

    MaterialPassInfo &materialInfo() { return std::get<MaterialPassInfo>(pass_info); }
    const MaterialPassInfo &materialInfo() const { return std::get<MaterialPassInfo>(pass_info); }
    FullscreenPassInfo &fullscreenInfo() { return std::get<FullscreenPassInfo>(pass_info); }
    const FullscreenPassInfo &fullscreenInfo() const { return std::get<FullscreenPassInfo>(pass_info); }
    DebugDrawPassInfo &debugDrawInfo() { return std::get<DebugDrawPassInfo>(pass_info); }
    const DebugDrawPassInfo &debugDrawInfo() const { return std::get<DebugDrawPassInfo>(pass_info); }
    DebugTextPassInfo &debugTextInfo() { return std::get<DebugTextPassInfo>(pass_info); }
    const DebugTextPassInfo &debugTextInfo() const { return std::get<DebugTextPassInfo>(pass_info); }
    ShadowDepthPassInfo &shadowDepthInfo() { return std::get<ShadowDepthPassInfo>(pass_info); }
    const ShadowDepthPassInfo &shadowDepthInfo() const { return std::get<ShadowDepthPassInfo>(pass_info); }
};

struct ComputeDispatchDefinition {
    uint32_t groups_x = 1;
    uint32_t groups_y = 1;
    uint32_t groups_z = 1;
    std::string groups_from;
    uint32_t local_size = 1;
};

struct ComputeTaskDefinition {
    std::string name;
    ShaderReference shader = ShaderReference{"", ShaderStage::compute, ShaderReferenceKind::explicit_file, false};
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<std::string> after;
    std::vector<std::string> before;
    ComputeDispatchDefinition dispatch;
    std::string schedule = "per_frame";
};

struct CompiledComputeTask {
    ComputeTaskDefinition definition;
    ComputeTaskId task_id;
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
    std::vector<CompiledComputeTask> compute_tasks;
};

} // namespace Pelican
