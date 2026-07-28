#pragma once

#include "renderingpass.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/targetrenderplanning.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class DebugDraw;
class DebugText;
class ComputeTaskContainer;
class FullscreenPassContainer;
class FrameGraphResourceContainer;
class FrameGraphRuntimeContainer;
struct RendererRuntimeGeneration;
class PathResolver;
class PipelineFactory;
class RenderCompilerProgram;
class RenderingPassContainer;
class RenderTarget;
class RenderTargetContainer;
class ShaderLibrary;
class ShadowDepthPassContainer;
class VelocityPassContainer;

enum class RenderPipelineGpuRegistrationFaultPoint {
    none,
    after_render_targets,
    after_frame_graph_buffers,
    after_compute_tasks,
    after_rendering_passes,
    after_runtime_prepare,
};

struct RenderingPassConfigRenderTargetDependencies {
    RenderTargetContainer &render_target_container;
};

struct RenderingPassConfigRuntimeDependencies {
    RenderTarget &render_target;
    ShaderLibrary &shader_library;
    FullscreenPassContainer &fullscreen_pass_container;
    PipelineFactory &pipeline_factory;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    VelocityPassContainer &velocity_pass_container;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
    std::function<DebugDraw &()> debug_draw_provider;
    std::function<DebugText &()> debug_text_provider;
};

struct RenderingPassConfigRegistrationDependencies {
    RenderingPassConfigRenderTargetDependencies render_targets;
    RenderingPassConfigRuntimeDependencies runtime;
    FrameGraphResourceContainer &frame_graph_resources;
    ComputeTaskContainer &compute_task_container;
    FrameGraphRuntimeContainer &frame_graph_runtime;
    RenderingPassContainer &pass_container;
    struct Options {
        RenderPipelineGraphVariant graph_variant =
            RenderPipelineGraphVariant::flat;
        // Source-level compiler seam. Null selects the built-in mixed
        // logical/Vulkan program. A supplied program must be shared by every
        // variant participating in one publication transaction.
        const RenderCompilerProgram
            *render_compiler_program = nullptr;
        // WP203b production wiring is enabled only for targets that can
        // provide one command context spanning the complete view family.
        // OpenXR enables this after its array-swapchain target is installed.
        bool enable_multiview_runtime = false;
        // Requests a typed device-depth export from each graph. The target
        // compiler keeps the inferred source materialized and adds the image
        // usage required by an external compositor copy.
        bool enable_external_depth_export = false;
        bool publish_enabled_features = true;
        std::string gpu_owner_scope;
        std::function<void()> prepare_additional_gpu_resources;
        // Runs after every program and GPU scope in the transaction has been
        // prepared, but before the single publication CAS.
        std::function<void(const RendererRuntimeGeneration &)>
            validate_prepared_generation;
        RenderPipelineGpuRegistrationFaultPoint fault_point =
            RenderPipelineGpuRegistrationFaultPoint::none;
    } options;
};

struct RenderingPassConfigRegistrationResult {
    std::vector<RenderingPassId> rendering_pass_ids;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::vector<std::shared_ptr<const VulkanTargetPlan>>
        target_plans;
    std::shared_ptr<const ResolvedSampleCountPlan> sample_count_plan;
    std::uint64_t runtime_generation = 0;
};

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJson(
    const std::string &json_path, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies);
RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies);
std::vector<RenderingPassConfigRegistrationResult>
registerRenderingPassConfigVariantsFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies> dependencies);

} // namespace Pelican
