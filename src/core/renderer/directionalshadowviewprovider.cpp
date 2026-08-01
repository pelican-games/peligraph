#include "directionalshadowviewprovider.hpp"

#include "directionalshadowcascade.hpp"
#include "viewfamilyproviderregistry.hpp"
#include "../light/lightcontainer.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderfeatureparameteraccess.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

struct DirectionalShadowRuntimeContract {
    DirectionalShadowCascadeSettings settings;
    std::optional<GlobalRenderTargetId>
        target_id;
    std::optional<RenderTargetMetadata>
        target;
};

DirectionalShadowRuntimeContract
directionalShadowRuntimeContract(
    const CompiledFrameGraphExecution
        &frame_graph,
    const RenderTargetContainer &targets) {
    DirectionalShadowRuntimeContract result;
    if (!frame_graph.render_pipeline) {
        return result;
    }
    const auto contract = std::find_if(
        frame_graph.render_pipeline
            ->surface_resource_contracts
            .begin(),
        frame_graph.render_pipeline
            ->surface_resource_contracts
            .end(),
        [](const CompiledSurfaceResourceContract
               &candidate) {
            return candidate.contract.name ==
                   directionalShadowInputContractName;
        });
    if (contract ==
        frame_graph.render_pipeline
            ->surface_resource_contracts
            .end()) {
        return result;
    }
    const auto binding =
        frame_graph.render_target_bindings.find(
            contract->resource);
    if (binding ==
            frame_graph.render_target_bindings
                .end() ||
        !isConcreteRenderTarget(
            binding->second)) {
        throw std::runtime_error(
            "directional shadow contract is not bound to a concrete "
            "render target");
    }
    result.target =
        targets.getMetadata(
            binding->second);
    result.target_id = binding->second;

    const auto feature = std::find_if(
        frame_graph.render_pipeline
            ->feature_instances.begin(),
        frame_graph.render_pipeline
            ->feature_instances.end(),
        [&](const CompiledRenderFeatureInstance
                &candidate) {
            return candidate.feature ==
                   contract->provider_feature;
        });
    if (feature ==
        frame_graph.render_pipeline
            ->feature_instances.end()) {
        return result;
    }
    result.settings.cascade_count =
        renderFeatureUnsignedParameter(
            *feature, "cascade_count", 1);
    result.settings.max_distance =
        renderFeatureFloatParameter(
            *feature, "max_distance",
            result.settings.max_distance);
    result.settings.split_lambda =
        renderFeatureFloatParameter(
            *feature, "split_lambda",
            result.settings.split_lambda);
    result.settings.stabilize =
        renderFeatureBoolParameter(
            *feature, "stabilize",
            result.settings.stabilize);
    return result;
}

std::uint32_t directionalShadowLightSlots(
    const LightContainer &lights) {
    if (lights.directionalLightCount() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "directional shadow light count exceeds the runtime index range");
    }
    return std::max(
        1u,
        static_cast<std::uint32_t>(
            lights.directionalLightCount()));
}

std::uint32_t directionalShadowViewCount(
    const LightContainer &lights,
    std::uint32_t cascade_count) {
    const auto count =
        static_cast<std::uint64_t>(
            directionalShadowLightSlots(lights)) *
        cascade_count;
    if (count >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "directional shadow view count exceeds the runtime index range");
    }
    return static_cast<std::uint32_t>(count);
}

std::string directionalShadowViewId(
    std::uint32_t light_index,
    std::uint32_t cascade_index) {
    return directionalShadowLightCascadeRenderViewId(
        light_index, cascade_index);
}

RenderViewFamily buildDirectionalShadowFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    const auto contract =
        directionalShadowRuntimeContract(
            context.frame_graph,
            context.render_targets);
    if (contract.settings.cascade_count == 0 ||
        contract.settings.cascade_count >
        maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow cascade_count exceeds the runtime ABI");
    }
    const auto view_count =
        directionalShadowViewCount(
            context.lights,
            contract.settings.cascade_count);
    if (contract.target &&
        contract.target->array_layers <
            view_count) {
        throw std::runtime_error(
            "directional shadow target has fewer array layers than the "
            "runtime shadow view count");
    }
    if (view_count > 1 && !contract.target) {
        throw std::runtime_error(
            "multiple directional shadow views require a typed shadow "
            "target contract");
    }

    RenderViewFamily result{
        .family_id =
            std::string{
                directionalShadowRenderViewFamilyId},
    };
    result.views.reserve(view_count);
    const auto light_slots =
        directionalShadowLightSlots(
            context.lights);
    for (std::uint32_t light = 0;
         light < light_slots; ++light) {
        if (contract.settings.cascade_count > 1) {
            if (!contract.target) {
                throw std::runtime_error(
                    "cascaded directional shadows require a typed shadow "
                    "target contract");
            }
            auto cascades =
                buildDirectionalShadowCascadeFamily(
                    families.require(
                        mainRenderViewFamilyId),
                    context.lights.directionalLightCount() == 0
                        ? context.lights
                              .directionalShadowDirection()
                        : context.lights
                              .directionalShadowDirection(
                                  light),
                    {
                        contract.target->extent.width,
                        contract.target->extent.height,
                    },
                    contract.settings);
            for (std::uint32_t cascade = 0;
                 cascade < cascades.views.size();
                 ++cascade) {
                cascades.views[cascade].view_id =
                    directionalShadowViewId(
                        light, cascade);
                result.views.push_back(
                    std::move(
                        cascades.views[cascade]));
            }
            continue;
        }
        const auto source =
            context.lights.directionalLightCount() == 0
                ? context.lights
                      .directionalShadowView()
                : context.lights
                      .directionalShadowView(light);
        result.views.push_back(
            RenderViewParameters{
                .view = source.view,
                .projection = source.projection,
                .camera_position =
                    source.camera_position,
                .first_person_view = false,
                .view_id =
                    directionalShadowViewId(
                        light, 0),
            });
    }
    return result;
}

void validateDirectionalShadowFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    const auto contract =
        directionalShadowRuntimeContract(
            context.frame_graph,
            context.render_targets);
    if (contract.settings.cascade_count == 0 ||
        contract.settings.cascade_count >
            maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow cascade_count exceeds the runtime ABI");
    }
    const auto *family =
        families.find(
            directionalShadowRenderViewFamilyId);
    if (family == nullptr) {
        return;
    }
    const auto expected_view_count =
        directionalShadowViewCount(
            context.lights,
            contract.settings.cascade_count);
    if (family->views.size() !=
        expected_view_count) {
        throw std::runtime_error(
            "directional shadow view family cardinality does not match "
            "the light and cascade counts");
    }
    if (contract.target &&
        family->views.size() >
            contract.target->array_layers) {
        throw std::runtime_error(
            "directional shadow view family has more views than the "
            "shadow target has array layers");
    }
    if (contract.settings.cascade_count == 1) {
        return;
    }
    const auto light_slots =
        directionalShadowLightSlots(
            context.lights);
    for (std::uint32_t light = 0;
         light < light_slots; ++light) {
        float prior_far = 0.0f;
        for (std::uint32_t cascade = 0;
             cascade <
                 contract.settings.cascade_count;
             ++cascade) {
            const auto index =
                static_cast<std::size_t>(light) *
                    contract.settings.cascade_count +
                cascade;
            const auto &view =
                family->views[index];
            if (!view.depth_range) {
                throw std::runtime_error(
                    "multi-view directional shadow families require a "
                    "depth_range for every cascade");
            }
            if (cascade != 0) {
                const auto tolerance =
                    std::max(
                        1.0e-4f,
                        std::abs(prior_far) *
                            1.0e-5f);
                if (std::abs(
                        view.depth_range
                                ->near_distance -
                        prior_far) >
                    tolerance) {
                    throw std::runtime_error(
                        "directional shadow cascade depth ranges must be "
                        "contiguous per light");
                }
            }
            if (light != 0) {
                const auto &reference =
                    *family->views[cascade]
                         .depth_range;
                const auto tolerance =
                    std::max(
                        1.0e-4f,
                        std::abs(reference.far_distance) *
                            1.0e-5f);
                if (std::abs(
                        view.depth_range
                                ->near_distance -
                        reference.near_distance) >
                        tolerance ||
                    std::abs(
                        view.depth_range
                                ->far_distance -
                        reference.far_distance) >
                        tolerance) {
                    throw std::runtime_error(
                        "directional shadow cascade depth ranges differ "
                        "between lights");
                }
            }
            prior_far =
                view.depth_range
                    ->far_distance;
        }
    }
}

} // namespace

bool prepareDirectionalShadowRenderTarget(
    const CompiledFrameGraphExecution
        &frame_graph,
    const LightContainer &lights,
    RenderTargetContainer &render_targets) {
    const auto contract =
        directionalShadowRuntimeContract(
            frame_graph, render_targets);
    if (!contract.target_id) {
        return false;
    }
    if (contract.settings.cascade_count == 0 ||
        contract.settings.cascade_count >
            maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow cascade_count exceeds the runtime ABI");
    }
    return render_targets.setRuntimeArrayLayers(
        *contract.target_id,
        directionalShadowViewCount(
            lights,
            contract.settings.cascade_count));
}

void registerDirectionalShadowViewFamilyProvider(
    RenderViewFamilyProviderRegistry
        &registry) {
    registry.registerProvider(
        RenderViewFamilyProviderDefinition{
            .name =
                "engine.directional_shadow_v1",
            .family_id =
                std::string{
                    directionalShadowRenderViewFamilyId},
            .build =
                buildDirectionalShadowFamily,
            .validate =
                validateDirectionalShadowFamily,
        });
}

} // namespace Pelican
