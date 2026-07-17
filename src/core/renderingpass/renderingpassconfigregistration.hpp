#pragma once

#include "renderingpass.hpp"

#include <functional>
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
class PathResolver;
class RenderingPassContainer;
class RenderTarget;
class RenderTargetContainer;
class ShaderLibrary;

struct RenderingPassConfigRenderTargetDependencies {
    RenderTargetContainer &render_target_container;
};

struct RenderingPassConfigRuntimeDependencies {
    RenderTarget &render_target;
    ShaderLibrary &shader_library;
    FullscreenPassContainer &fullscreen_pass_container;
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
        std::function<bool(std::string_view, const nlohmann::json &)> include_feature;
        std::function<void(const nlohmann::json &)> validate_composed_config;
        std::string rendering_pass_name_suffix;
        bool publish_enabled_features = true;
    } options;
};

struct RenderingPassConfigRegistrationResult {
    std::vector<RenderingPassId> rendering_pass_ids;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
};

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJson(
    const std::string &json_path, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies);
RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies);

} // namespace Pelican
