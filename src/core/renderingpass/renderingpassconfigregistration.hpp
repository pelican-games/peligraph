#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FullscreenPassContainer;
class RenderingPassContainer;
class RenderTarget;
class RenderTargetContainer;
class ShaderContainer;

struct RenderingPassConfigRegistrationDependencies {
    RenderTarget &render_target;
    RenderTargetContainer &render_target_container;
    ShaderContainer &shader_container;
    FullscreenPassContainer &fullscreen_pass_container;
    RenderingPassContainer &pass_container;
};

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies);

} // namespace Pelican
