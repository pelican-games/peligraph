#include "renderingpassconfigregistration.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "rendertargetconfigregistration.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetjsonparser.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include <utility>

namespace Pelican {

namespace {

RenderingPassRuntimeDependencies toRuntimeDependencies(
    RenderingPassConfigRuntimeDependencies &dependencies,
    const RenderTargetMetadataResolver &rt_metadata,
    const RenderTargetImageViewResolver &rt_views) {
    return RenderingPassRuntimeDependencies{
        &dependencies.render_target,
        &rt_metadata,
        &rt_views,
        &dependencies.shader_container,
        &dependencies.fullscreen_pass_container,
    };
}

} // namespace

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies) {
    const auto rendering_pass_data = loadRenderingPassConfigJson(json_path);
    const auto render_target_definitions = parseRenderTargetDefinitionsFromJson(rendering_pass_data);
    registerRenderTargetDefinitions(render_target_definitions, base_extent,
                                    dependencies.render_targets.render_target_container);

    const RenderTargetNameResolver rt_resolver{dependencies.render_targets.render_target_container};
    const RenderTargetMetadataResolver rt_metadata{dependencies.render_targets.render_target_container};
    const RenderTargetImageViewResolver rt_views{dependencies.render_targets.render_target_container};
    const auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, rt_resolver, rt_metadata);
    auto compiled_passes =
        compileRenderingPassesRuntime(pass_definitions,
                                      toRuntimeDependencies(dependencies.runtime, rt_metadata, rt_views));
    for (auto &compiled_pass : compiled_passes) {
        dependencies.pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
    }
}

} // namespace Pelican
