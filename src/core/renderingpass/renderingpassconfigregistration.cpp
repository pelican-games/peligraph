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
#include <string_view>
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
        &dependencies.shader_library,
        &dependencies.fullscreen_pass_container,
        &dependencies.path_resolver,
        dependencies.warn_backend_specific_shader_refs,
    };
}

void registerRenderingPassConfigData(const nlohmann::json &rendering_pass_data, vk::Extent2D base_extent,
                                     RenderingPassConfigRegistrationDependencies dependencies) {
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

} // namespace

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies) {
    registerRenderingPassConfigData(loadRenderingPassConfigJson(json_path), base_extent, std::move(dependencies));
}

void registerRenderingPassConfigFromJsonData(std::string_view json_data, vk::Extent2D base_extent,
                                             RenderingPassConfigRegistrationDependencies dependencies) {
    registerRenderingPassConfigData(loadRenderingPassConfigJsonFromString(json_data, "ProjectBasicConfig"),
                                    base_extent, std::move(dependencies));
}

} // namespace Pelican
