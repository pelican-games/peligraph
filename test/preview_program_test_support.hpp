#pragma once

// RC-3c1b: the preview program as the registration makes it, without a
// device: the authoring document loaded and lowered, the production
// compiler program with the registration's requests
// (makeRenderGraphVariantRequests: flat, then the request-local data_only
// preview), and previewGraphProgramOf. A refusal only the preview raises
// comes back as an unavailable program with its reason (PV-1); one flat
// raises too is thrown, as the registration throws it.
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/previewgraph.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/renderingpass/renderingpassconfigregistration.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/project/authoringlowering.hpp"

#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace Pelican::TestSupport {

// The compiler program's preview artifact for this graph root document.
inline RenderCompilerProgramVariantOutput compiledPreviewVariant(
    std::string_view document, std::function<std::string(std::string_view)> load_document,
    bool runtime_shader_compiler_enabled) {
    const auto lowering =
        lowerAuthoringDocument(loadRenderingPassConfigJsonFromString(document, "preview test"));
    const PathResolver paths;
    // As registerRenderGraphVariantFamilyFromJsonData asks: the flat runtime
    // variant first (its composition decides which failures are the
    // preview's own, RG-3), then the preview. Headless: no ImGui pass.
    const std::array flat{RenderingPassConfigRegistrationDependencies::Options{}};
    auto requests = makeRenderGraphVariantRequests(std::span{flat}, true);
    requests.front().compose_runtime_config = nullptr;
    const auto transforms = graphTransformRegistry().snapshot();
    const auto strategies = renderStrategyRegistry().snapshot();
    const auto subgraphs = subgraphReplacementRegistry().snapshot();
    const VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(),
        RenderCompilerProgramInput{
            .rendering_config = lowering.document,
            .source_name = "preview test",
            .path_resolver = paths,
            .load_document = std::move(load_document),
            .runtime_shader_compiler_enabled = runtime_shader_compiler_enabled,
            .graph_transforms = transforms,
            .render_strategies = strategies,
            .subgraph_replacements = subgraphs,
            .backend_context = backend,
            .variants = requests,
        });
    return std::move(output.variants.back());
}

// The registration's preview program for this graph root document.
inline PreviewGraphProgram registeredPreviewProgram(
    std::string_view document, std::function<std::string(std::string_view)> load_document,
    bool runtime_shader_compiler_enabled) {
    return previewGraphProgramOf(
        compiledPreviewVariant(document, std::move(load_document), runtime_shader_compiler_enabled));
}

// What refused this preview, in the refusal's own words: the unavailable
// program's reason, or the shared stages' exception; "" when available.
inline std::string previewRefusalMessage(
    std::string_view document, std::function<std::string(std::string_view)> load_document,
    bool runtime_shader_compiler_enabled) {
    try {
        const auto program =
            registeredPreviewProgram(document, std::move(load_document), runtime_shader_compiler_enabled);
        return program.available ? std::string{} : program.unavailable_reason.value("message", std::string{"?"});
    } catch (const std::exception &error) {
        return error.what();
    }
}

} // namespace Pelican::TestSupport
