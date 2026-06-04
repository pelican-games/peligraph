#include "renderingpassconfigregistration.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "rendertargetconfigregistration.hpp"
#include "rendertargetjsonparser.hpp"
#include <utility>

namespace Pelican {

namespace {

RenderingPassRuntimeDependencies toRuntimeDependencies(
    RenderingPassConfigRegistrationDependencies &dependencies) {
    return RenderingPassRuntimeDependencies{
        &dependencies.render_target,
        &dependencies.render_target_container,
        &dependencies.shader_container,
        &dependencies.fullscreen_pass_container,
    };
}

} // namespace

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies) {
    const auto rendering_pass_data = loadRenderingPassConfigJson(json_path);
    const auto render_target_definitions = parseRenderTargetDefinitionsFromJson(rendering_pass_data);
    registerRenderTargetDefinitions(render_target_definitions, base_extent, dependencies.render_target_container);

    const auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, dependencies.render_target_container);
    auto compiled_passes = compileRenderingPassesRuntime(pass_definitions, toRuntimeDependencies(dependencies));
    for (auto &compiled_pass : compiled_passes) {
        dependencies.pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
    }
}

} // namespace Pelican
