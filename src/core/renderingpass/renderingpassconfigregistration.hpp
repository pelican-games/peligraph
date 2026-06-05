#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FullscreenPassContainer;
class RenderingPassContainer;
class RenderTarget;
class RenderTargetContainer;
class ShaderContainer;

struct RenderingPassConfigRenderTargetDependencies {
    RenderTargetContainer &render_target_container;
};

struct RenderingPassConfigRuntimeDependencies {
    RenderTarget &render_target;
    ShaderContainer &shader_container;
    FullscreenPassContainer &fullscreen_pass_container;
};

struct RenderingPassConfigRegistrationDependencies {
    RenderingPassConfigRenderTargetDependencies render_targets;
    RenderingPassConfigRuntimeDependencies runtime;
    RenderingPassContainer &pass_container;
};

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies);

} // namespace Pelican
