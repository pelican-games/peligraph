#include "planarreflectionviewprovider.hpp"

#include "planarreflectionview.hpp"
#include "../../renderer/viewfamilyproviderregistry.hpp"
#include "../../renderingpass/framegraphruntime.hpp"
#include "../../renderingpass/renderfeatureparameteraccess.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace Pelican {
namespace {

std::optional<PlanarReflectionViewSettings>
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
                   planarReflectionRenderFeatureName;
        });
    if (feature ==
        frame_graph.render_pipeline
            ->feature_instances.end()) {
        return std::nullopt;
    }
    return PlanarReflectionViewSettings{
        .clip_plane =
            RenderViewClipPlane{
                .normal =
                    {
                        renderFeatureFloatParameter(
                            *feature,
                            "plane_x", 0.0f),
                        renderFeatureFloatParameter(
                            *feature,
                            "plane_y", 1.0f),
                        renderFeatureFloatParameter(
                            *feature,
                            "plane_z", 0.0f),
                    },
                .offset =
                    renderFeatureFloatParameter(
                        *feature,
                        "plane_offset", 0.0f),
            },
        .preserve_raster_winding =
            renderFeatureBoolParameter(
                *feature,
                "preserve_raster_winding",
                true),
        .oblique_near_plane =
            renderFeatureBoolParameter(
                *feature,
                "oblique_near_plane",
                true),
    };
}

RenderViewFamily buildFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    const auto settings =
        runtimeSettings(
            context.frame_graph);
    if (!settings) {
        throw std::runtime_error(
            "compiled frame graph requires '$reflection/planar' "
            "but no authored family or standard planar_reflection "
            "feature is available");
    }
    return buildPlanarReflectionViewFamily(
        families.require(
            mainRenderViewFamilyId),
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
    const auto &main =
        families.require(
            mainRenderViewFamilyId);
    const auto *reflection =
        families.find(
            planarReflectionRenderViewFamilyId);
    if (reflection == nullptr ||
        reflection->views.size() !=
            main.views.size()) {
        throw std::runtime_error(
            "standard planar reflection requires one reflected view "
            "for every main-family view");
    }
    for (const auto &view :
         reflection->views) {
        if (!view.clip_plane) {
            throw std::runtime_error(
                "standard planar reflection views require a clip plane");
        }
    }
}

} // namespace

void registerStandardPlanarReflectionViewProvider(
    RenderViewFamilyProviderRegistry
        &registry) {
    registry.registerProvider(
        RenderViewFamilyProviderDefinition{
            .name =
                "standard.planar_reflection_v1",
            .family_id =
                std::string{
                    planarReflectionRenderViewFamilyId},
            .build = buildFamily,
            .validate = validateFamily,
        });
}

} // namespace Pelican
