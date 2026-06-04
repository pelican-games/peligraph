#include "renderingpassconfigregistration.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include <utility>

namespace Pelican {

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderTargetContainer &rt_container,
                                         RenderingPassRuntimeDependencies dependencies,
                                         RenderingPassContainer &pass_container) {
    auto compiled_passes = loadCompiledRenderingPassesFromJson(json_path, base_extent, rt_container, dependencies);
    for (auto &compiled_pass : compiled_passes) {
        pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
    }
}

} // namespace Pelican
