#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/graphicsviewcontract.hpp"
#include "../shader/shaderreference.hpp"
#include "../../project/materialdrawtag.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/materialscreeninput.hpp"
#include "../../project/shaderresourceport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
PELICAN_DEFINE_HANDLE(FrameGraphBufferId, int)

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

inline constexpr FrameGraphBufferId noFrameGraphBufferId() {
    return FrameGraphBufferId{-1};
}

inline constexpr bool isValidFrameGraphBufferId(FrameGraphBufferId id) {
    return id.value >= 0;
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

struct MaterialPassInputBinding {
    MaterialPassInputContract contract;
    GlobalRenderTargetId target = noRenderTargetId();
    bool history = false;
};

using MaterialPassScreenInputBinding = MaterialPassInputBinding;

struct MaterialPassResourceBinding {
    ShaderResourcePortDefinition port;
    GlobalRenderTargetId target = noRenderTargetId();
    std::string buffer;
    // Runtime compilation pins a concrete generation-owned buffer handle.
    // Authored/pass-parser definitions retain the logical name above.
    FrameGraphBufferId buffer_id = noFrameGraphBufferId();
    bool history = false;
    LogicalReadFootprint footprint{
        LogicalReadFootprintKind::arbitrary,
        std::nullopt,
    };

    bool isImage() const {
        return isConcreteRenderTarget(target);
    }
    bool isBuffer() const {
        return !buffer.empty();
    }
};

enum class GpuDrawFallback : std::uint8_t {
    cpu_draw_queue,
};

enum class GpuDrawExecutionMode : std::uint8_t {
    automatic,
    cpu,
};

enum class GpuDrawSourceLayout : std::uint8_t {
    fixed_state_v1,
    draw_queue_segments_v1,
};

// A material pass may replace CPU-compiled fixed-state draw ranges with
// GPU-written indexed indirect commands. Segmented layout retains CPU-owned
// pipeline/material binding and gives each DrawQueue state range disjoint
// command/count output slots.
struct GpuDrawSourceDefinition {
    std::string commands;
    std::string count;
    std::string segments;
    std::uint32_t max_draw_count = 0;
    vk::DeviceSize command_offset = 0;
    vk::DeviceSize count_offset = 0;
    GpuDrawSourceLayout layout =
        GpuDrawSourceLayout::fixed_state_v1;
    GpuDrawFallback fallback =
        GpuDrawFallback::cpu_draw_queue;
    GpuDrawExecutionMode execution =
        GpuDrawExecutionMode::automatic;
    FrameGraphBufferId commands_id =
        noFrameGraphBufferId();
    FrameGraphBufferId count_id =
        noFrameGraphBufferId();
    FrameGraphBufferId segments_id =
        noFrameGraphBufferId();

    bool operator==(
        const GpuDrawSourceDefinition &) const = default;
};

struct MaterialPassInfo {
    uint32_t material_start = 0;
    uint32_t material_count = 0;
    MaterialPassContract contract = MaterialPassContract::legacy_gbuffer_v1;
    std::optional<MaterialDrawTagFilter> material_filter;
    // Opaque project-authored name selecting an alternate material resource.
    // The name carries no engine technique semantics.
    std::optional<std::string> material_variant;
    std::vector<MaterialPassScreenInputBinding> screen_inputs;
    // Feature-owned public resources share the material pass-input descriptor
    // ABI but are not authored by individual .surface files.
    std::vector<MaterialPassInputBinding> surface_resources;
    // Project-authored semantic ports are matched against each surface's
    // typed resource_ports. These named bindings create graph read edges;
    // materials that declare no matching port allocate no descriptor.
    std::vector<MaterialPassResourceBinding> material_resources;
    // fixed_state_v1 selects exactly one range. draw_queue_segments_v1 may
    // select multiple ranges while each GPU command remains inside the
    // CPU-authored state segment associated with that range.
    std::optional<GpuDrawSourceDefinition> gpu_draw_source;
};

enum class FullscreenPushConstantData {
    eNone,
    eCameraPosition,
    eProjectionView,
};

enum class FullscreenInputFilter : std::uint8_t {
    linear,
    nearest,
};

enum class FullscreenInputAddressMode : std::uint8_t {
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

struct FullscreenInputSampling {
    FullscreenInputFilter filter = FullscreenInputFilter::linear;
    FullscreenInputAddressMode address_mode =
        FullscreenInputAddressMode::repeat;

    bool operator==(const FullscreenInputSampling &) const = default;
};

struct FullscreenPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
    FullscreenPushConstantData push_constants = FullscreenPushConstantData::eNone;
    bool uses_light_data = false;
    // Empty preserves the legacy linear/repeat policy for every image input.
    // When authored, entries map one-to-one to PassDefinition::input_targets.
    std::vector<FullscreenInputSampling> input_sampling;
    // Optional typed annotations over input/input@history. Inputs without an
    // entry keep the existing raw set/binding shader ABI.
    std::vector<ShaderResourcePortDefinition> resource_ports;
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

struct VelocityPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference skinned_vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct UiPassInfo {};

#if PELICAN_WITH_IMGUI
struct ImGuiPassInfo {};
#endif

using PassInfo = std::variant<MaterialPassInfo, FullscreenPassInfo, DebugDrawPassInfo, DebugTextPassInfo,
                              ShadowDepthPassInfo, VelocityPassInfo, UiPassInfo
#if PELICAN_WITH_IMGUI
                              , ImGuiPassInfo
#endif
                              >;

// Immutable provenance for a pass implementation selected from the typed
// provider registry. The provider may replace implementation data, but never
// the logical contract represented by contract/fingerprint.
struct PassImplementationSelection {
    std::string provider;
    std::string implementation;
    std::string contract;
    std::uint64_t contract_fingerprint = 0;
    std::uint64_t provider_owner = 0;
    std::uint64_t provider_identity = 0;
    std::uint32_t provider_generation = 0;
    std::uint32_t provider_version = 0;
    std::uint64_t provider_capability_bits = 0;
    bool explicitly_selected = false;

    bool operator==(const PassImplementationSelection &) const = default;
};

enum class PassInputViewDimension : std::uint8_t {
    shared_2d,
    sequential_2d,
    layered_2d_array,
};

struct RenderPassViewInvocation {
    std::uint32_t logical_view_count = 1;
    std::uint32_t view_index = 0;
};

struct PassAttachmentOperations {
    vk::AttachmentLoadOp load_op =
        vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp store_op =
        vk::AttachmentStoreOp::eStore;

    bool operator==(
        const PassAttachmentOperations &) const = default;
};

// A pass labels the resolution space in which its raster work is defined.
// The compiler uses the scene domain to derive the camera/jitter render
// extent. Output and independent work (for example UI and shadow maps) do not
// participate in that inference.
enum class RenderResolutionDomain : std::uint8_t {
    unclassified,
    scene,
    output,
    independent,
};

struct PassDefinition {
    PassDefinition() : output_depth{noRenderTargetId()} {}

    std::string name;

    std::vector<GlobalRenderTargetId> output_color;
    GlobalRenderTargetId output_depth;
    std::vector<GlobalRenderTargetId> input_targets;
    std::vector<bool> input_target_history;
    // Runtime physical annotation aligned with input_targets. Parsed logical
    // definitions leave it empty; runtime compilation fills it from the
    // immutable Vulkan target plan.
    std::vector<PassInputViewDimension> input_target_views;
    std::vector<std::string> input_buffers;
    std::vector<std::string> region_tags;

    PassInfo pass_info = MaterialPassInfo{};
    std::optional<std::string> requested_implementation_provider;
    std::optional<PassImplementationSelection> implementation_selection;

    vk::AttachmentLoadOp color_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp color_store_op = vk::AttachmentStoreOp::eStore;
    vk::AttachmentLoadOp depth_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp depth_store_op = vk::AttachmentStoreOp::eDontCare;
    // Runtime-only physical overrides. Authored JSON retains the compact
    // pass-wide defaults above; the target-plan compiler expands them to
    // individual attachments and may safely override selected entries.
    std::vector<PassAttachmentOperations>
        physical_color_attachment_operations;
    std::optional<PassAttachmentOperations>
        physical_depth_attachment_operations;
    vk::ClearColorValue clear_color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;

    bool isMaterial() const { return std::holds_alternative<MaterialPassInfo>(pass_info); }
    bool isFullscreen() const { return std::holds_alternative<FullscreenPassInfo>(pass_info); }
    bool isDebugDraw() const { return std::holds_alternative<DebugDrawPassInfo>(pass_info); }
    bool isDebugText() const { return std::holds_alternative<DebugTextPassInfo>(pass_info); }
    bool isShadowDepth() const { return std::holds_alternative<ShadowDepthPassInfo>(pass_info); }
    bool isVelocity() const { return std::holds_alternative<VelocityPassInfo>(pass_info); }
    bool isUi() const { return std::holds_alternative<UiPassInfo>(pass_info); }
#if PELICAN_WITH_IMGUI
    bool isImGui() const { return std::holds_alternative<ImGuiPassInfo>(pass_info); }
#endif

    PassAttachmentOperations colorAttachmentOperations(
        std::size_t index) const {
        if (physical_color_attachment_operations.empty()) {
            return {
                color_load_op,
                color_store_op,
            };
        }
        return physical_color_attachment_operations.at(index);
    }

    PassAttachmentOperations depthAttachmentOperations() const {
        return physical_depth_attachment_operations.value_or(
            PassAttachmentOperations{
                depth_load_op,
                depth_store_op,
            });
    }

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
    VelocityPassInfo &velocityInfo() { return std::get<VelocityPassInfo>(pass_info); }
    const VelocityPassInfo &velocityInfo() const { return std::get<VelocityPassInfo>(pass_info); }
};

struct ComputeIndirectDispatchDefinition {
    std::string buffer;
    vk::DeviceSize offset = 0;

    bool operator==(
        const ComputeIndirectDispatchDefinition &) const =
        default;
};

struct ComputeDispatchDefinition {
    uint32_t groups_x = 1;
    uint32_t groups_y = 1;
    uint32_t groups_z = 1;
    std::string groups_from;
    uint32_t local_size = 1;
    std::optional<ComputeIndirectDispatchDefinition>
        indirect;
};

enum class ComputeTaskSchedule : std::uint8_t {
    // Executes once for the logical frame and may feed every view.
    per_frame,
    // Executes once for each logical view. Physical lowering may keep these
    // invocations sequential while other scopes use Vulkan multiview.
    per_view,
};

std::string_view computeTaskScheduleName(
    ComputeTaskSchedule schedule);

struct ComputeTaskDefinition {
    std::string name;
    ShaderReference shader = ShaderReference{"", ShaderStage::compute, ShaderReferenceKind::explicit_file, false};
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<std::string> after;
    std::vector<std::string> before;
    // Optional typed annotations over reads/writes. The dependency lists
    // remain authoritative and are not synthesized from these declarations.
    std::vector<ShaderResourcePortDefinition> resource_ports;
    ComputeDispatchDefinition dispatch;
    ComputeTaskSchedule schedule =
        ComputeTaskSchedule::per_frame;
};

struct CompiledComputeTask {
    ComputeTaskDefinition definition;
    ComputeTaskId task_id;
};

struct RenderingPassDefinition {
    std::string name;
    std::vector<PassDefinition> passes;
};

inline constexpr std::uint32_t
    unusedPhysicalAttachmentMapping =
        std::numeric_limits<std::uint32_t>::max();

// Concrete attachment-slot contract for one pass inside a physical rendering
// scope. Arrays are indexed by the scope-wide color attachment slot. Their
// values are fragment output locations and input-attachment indices,
// respectively, or unusedPhysicalAttachmentMapping.
struct CompiledPassRenderingContract {
    std::size_t scope_index =
        std::numeric_limits<std::size_t>::max();
    std::string scope_id;
    std::vector<GlobalRenderTargetId>
        color_attachments;
    GlobalRenderTargetId depth_attachment =
        noRenderTargetId();
    std::vector<std::uint32_t>
        color_attachment_locations;
    std::vector<std::uint32_t>
        color_attachment_input_indices;
    std::uint32_t depth_attachment_input_index =
        unusedPhysicalAttachmentMapping;
    // Dynamic rendering begins once for the whole physical scope. The first
    // writer determines each attachment's load/clear operation and the last
    // writer determines its final store operation. These arrays are indexed
    // by color_attachments and are identical on every pass in the scope.
    std::vector<PassAttachmentOperations>
        scope_color_attachment_operations;
    std::vector<std::array<float, 4>>
        scope_color_clear_values;
    std::optional<PassAttachmentOperations>
        scope_depth_attachment_operations;
    float scope_depth_clear_value = 1.0f;
    std::uint32_t scope_stencil_clear_value = 0;
    // A multi-node rendering scope is one Vulkan dynamic-rendering instance.
    // local_read_scope is the stricter subset that also needs the
    // dynamic-rendering-local-read mappings and feature.
    bool fused_rendering_scope = false;
    bool local_read_scope = false;

    bool operator==(
        const CompiledPassRenderingContract &) const =
        default;
};

struct CompiledPass {
    PassDefinition definition;
    PassId pass_id;
    GraphicsPipelineViewContract view;
    CompiledPassRenderingContract rendering;
};

struct CompiledRenderingPass {
    std::string name;
    std::vector<CompiledPass> passes;
    std::vector<CompiledComputeTask> compute_tasks;
};

} // namespace Pelican
