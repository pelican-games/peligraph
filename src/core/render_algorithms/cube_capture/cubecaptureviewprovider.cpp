#include "cubecaptureviewprovider.hpp"

#include "cubecaptureview.hpp"
#include "../../renderer/viewfamilyproviderregistry.hpp"
#include "../../renderingpass/framegraphruntime.hpp"
#include "../../renderingpass/renderfeatureparameteraccess.hpp"
#include "../../renderingpass/rendertargetcontainer.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace Pelican {
namespace {

std::optional<CubeCaptureViewSettings>
runtimeSettings(
    const CompiledFrameGraphExecution
        &frame_graph) {
    if (!frame_graph.render_pipeline) {
        return std::nullopt;
    }
    const auto feature = std::find_if(
        frame_graph.render_pipeline
            ->feature_instances.begin(),
        frame_graph.render_pipeline
            ->feature_instances.end(),
        [](const CompiledRenderFeatureInstance
               &candidate) {
            return candidate.feature ==
                   cubeCaptureRenderFeatureName;
        });
    if (feature ==
        frame_graph.render_pipeline
            ->feature_instances.end()) {
        return std::nullopt;
    }
    return CubeCaptureViewSettings{
        .position =
            {
                renderFeatureFloatParameter(
                    *feature,
                    "position_x", 0.0f),
                renderFeatureFloatParameter(
                    *feature,
                    "position_y", 0.0f),
                renderFeatureFloatParameter(
                    *feature,
                    "position_z", 0.0f),
            },
        .near_distance =
            renderFeatureFloatParameter(
                *feature,
                "near_distance", 0.1f),
        .far_distance =
            renderFeatureFloatParameter(
                *feature,
                "far_distance", 1000.0f),
    };
}

RenderViewFamily buildFamily(
    const RenderViewFamilies &,
    const RenderViewFamilyProviderContext
        &context) {
    const auto settings =
        runtimeSettings(
            context.frame_graph);
    if (!settings) {
        throw std::runtime_error(
            "compiled frame graph requires '$capture/cube' but no "
            "authored family or standard cube_capture feature is "
            "available");
    }
    return buildCubeCaptureViewFamily(
        *settings);
}

void validateFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    if (!runtimeSettings(
            context.frame_graph)) {
        return;
    }
    const auto *capture =
        families.find(
            cubeCaptureRenderViewFamilyId);
    constexpr std::array<std::string_view, 6>
        expected_ids{
            cubeCapturePositiveXViewId,
            cubeCaptureNegativeXViewId,
            cubeCapturePositiveYViewId,
            cubeCaptureNegativeYViewId,
            cubeCapturePositiveZViewId,
            cubeCaptureNegativeZViewId,
        };
    if (capture == nullptr ||
        capture->views.size() !=
            expected_ids.size()) {
        throw std::runtime_error(
            "standard cube capture requires exactly six face views");
    }
    for (std::size_t index = 0;
         index < expected_ids.size(); ++index) {
        if (capture->views[index].view_id !=
            expected_ids[index]) {
            throw std::runtime_error(
                "standard cube capture view order must be "
                "+X, -X, +Y, -Y, +Z, -Z");
        }
    }

    const auto binding =
        context.frame_graph
            .render_target_bindings.find(
                "cube_capture_color");
    if (binding ==
        context.frame_graph
            .render_target_bindings.end()) {
        throw std::runtime_error(
            "standard cube capture requires render target "
            "'cube_capture_color'");
    }
    const auto metadata =
        context.render_targets.getMetadata(
            binding->second);
    if (metadata.dimension !=
            ImageResourceDimension::cube ||
        metadata.array_layers != 6 ||
        metadata.extent.width !=
            metadata.extent.height) {
        throw std::runtime_error(
            "standard cube capture output must be a square six-face "
            "cube render target");
    }
}

} // namespace

void registerStandardCubeCaptureViewProvider(
    RenderViewFamilyProviderRegistry
        &registry) {
    registry.registerProvider(
        RenderViewFamilyProviderDefinition{
            .name =
                "standard.cube_capture_v1",
            .family_id =
                std::string{
                    cubeCaptureRenderViewFamilyId},
            .build = buildFamily,
            .validate = validateFamily,
        });
}

} // namespace Pelican
