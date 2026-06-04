#include "renderingpassconfigregistration.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetjsonparser.hpp"

namespace Pelican {

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderTargetContainer &rt_container,
                                         RenderingPassRuntimeDependencies dependencies,
                                         RenderingPassContainer &pass_container) {
    const auto rendering_pass_data = loadRenderingPassConfigJson(json_path);
    const auto render_target_definitions = parseRenderTargetDefinitionsFromJson(rendering_pass_data, base_extent);
    registerRenderTargetDefinitions(render_target_definitions, rt_container);

    dependencies.render_target_container = &rt_container;

    const auto pass_definitions = parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, rt_container);
    for (const auto &pass_definition : pass_definitions) {
        pass_container.registerCompiledRenderingPass(compileRenderingPassRuntime(pass_definition, dependencies));
    }
}

} // namespace Pelican
