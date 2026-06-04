#include "renderingpassconfigregistry.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include <utility>

namespace Pelican {

void RenderingPassConfigRegistry::registerFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                                   RenderTargetContainer &rt_container,
                                                   RenderingPassContainer &pass_container) const {
    auto compiled_passes = loadCompiledRenderingPassesFromJson(json_path, base_extent, rt_container);
    for (auto &compiled_pass : compiled_passes) {
        pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
    }
}

} // namespace Pelican
