#include "directionalshadowviewprovider.hpp"

#include "directionalshadowcascade.hpp"
#include "viewfamilyproviderregistry.hpp"
#include "../light/lightcontainer.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderfeatureparameteraccess.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Pelican {
namespace {

struct DirectionalShadowRuntimeContract {
    DirectionalShadowCascadeSettings settings;
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

RenderViewFamily buildDirectionalShadowFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    const auto contract =
        directionalShadowRuntimeContract(
            context.frame_graph,
            context.render_targets);
    if (contract.settings.cascade_count >
        maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow cascade_count exceeds the runtime ABI");
    }
    if (contract.target &&
        contract.target->array_layers <
            contract.settings
                .cascade_count) {
        throw std::runtime_error(
            "directional shadow target has fewer array layers than the "
            "configured cascade_count");
    }
    if (contract.settings.cascade_count >
        1) {
        if (!contract.target) {
            throw std::runtime_error(
                "cascaded directional shadows require a typed shadow "
                "target contract");
        }
        return buildDirectionalShadowCascadeFamily(
            families.require(
                mainRenderViewFamilyId),
            context.lights
                .directionalShadowDirection(),
            {
                contract.target->extent.width,
                contract.target->extent.height,
            },
            contract.settings);
    }
    const auto source =
        context.lights.directionalShadowView();
    return RenderViewFamily{
        .family_id =
            std::string{
                directionalShadowRenderViewFamilyId},
        .views =
            {RenderViewParameters{
                .view = source.view,
                .projection = source.projection,
                .camera_position =
                    source.camera_position,
                .first_person_view = false,
                .view_id =
                    std::string{
                        directionalShadowRenderViewId},
            }},
    };
}

void validateDirectionalShadowFamily(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context) {
    const auto contract =
        directionalShadowRuntimeContract(
            context.frame_graph,
            context.render_targets);
    const auto *family =
        families.find(
            directionalShadowRenderViewFamilyId);
    if (family == nullptr) {
        return;
    }
    if (family->views.empty() ||
        family->views.size() >
            maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow view family cardinality is outside the "
            "LightUBO ABI");
    }
    if (contract.target &&
        family->views.size() >
            contract.target->array_layers) {
        throw std::runtime_error(
            "directional shadow view family has more views than the "
            "shadow target has array layers");
    }
    if (family->views.size() == 1) {
        return;
    }
    float prior_far = 0.0f;
    for (std::size_t index = 0;
         index < family->views.size();
         ++index) {
        const auto &view =
            family->views[index];
        if (!view.depth_range) {
            throw std::runtime_error(
                "multi-view directional shadow families require a "
                "depth_range for every cascade");
        }
        if (index != 0) {
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
                    "contiguous");
            }
        }
        prior_far =
            view.depth_range
                ->far_distance;
    }
}

} // namespace

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
