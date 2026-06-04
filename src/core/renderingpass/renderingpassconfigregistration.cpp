#include "renderingpassconfigregistration.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderTargetContainer &rt_container,
                                         RenderingPassRuntimeDependencies dependencies,
                                         RenderingPassContainer &pass_container) {
    const auto pass_definitions = loadRenderingPassDefinitionsFromJson(json_path, base_extent, rt_container);
    dependencies.render_target_container = &rt_container;

    for (const auto &pass_definition : pass_definitions) {
        pass_container.registerCompiledRenderingPass(compileRenderingPassRuntime(pass_definition, dependencies));
    }
}

} // namespace Pelican
