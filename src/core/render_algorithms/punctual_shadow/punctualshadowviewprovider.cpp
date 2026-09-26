#include "punctualshadowviewprovider.hpp"
#include "punctualshadowview.hpp"
#include "../../light/lightcontainer.hpp"
#include "../../renderer/viewfamilyproviderregistry.hpp"
#include "../../renderingpass/framegraphruntime.hpp"
#include "../../renderingpass/renderfeatureparameteraccess.hpp"
#include "../../renderingpass/rendertargetcontainer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {
struct ShadowFamily {
    std::string_view family;
    std::string_view contract;
    bool point;
};
constexpr std::array shadow_families{
    ShadowFamily{pointShadowViewFamilyId, "point_shadow", true},
    ShadowFamily{spotShadowViewFamilyId, "spot_shadow", false}};

const CompiledSurfaceResourceContract *findContract(const CompiledFrameGraphExecution &graph,
                                                    const ShadowFamily &family) {
    if (!graph.render_pipeline) return nullptr;
    const auto &contracts = graph.render_pipeline->surface_resource_contracts;
    const auto found = std::find_if(contracts.begin(), contracts.end(), [&](const auto &entry) {
        return entry.contract.name == family.contract;
    });
    return found == contracts.end() ? nullptr : &*found;
}

std::uint32_t viewCount(const LightContainer &lights, const ShadowFamily &family) {
    const auto count = family.point ? lights.pointLights().size() : lights.spotLights().size();
    // A zero-light graph retains a cleared dummy target; no light references it.
    const auto views = std::max(std::size_t{1}, count) * (family.point ? 6 : 1);
    if (views > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("punctual shadow layer count exceeds uint32");
    return static_cast<std::uint32_t>(views);
}

GlobalRenderTargetId targetId(const CompiledFrameGraphExecution &graph,
                              const CompiledSurfaceResourceContract &contract) {
    const auto binding = graph.render_target_bindings.find(contract.resource);
    if (binding == graph.render_target_bindings.end() || !isConcreteRenderTarget(binding->second))
        throw std::runtime_error("punctual shadow contract requires a concrete depth target");
    return binding->second;
}

RenderViewFamily buildFamily(const RenderViewFamilyProviderContext &context,
                             const ShadowFamily &family) {
    const auto *contract = findContract(context.frame_graph, family);
    if (!contract) throw std::runtime_error("punctual shadow family requires its surface resource contract");
    const auto &features = context.frame_graph.render_pipeline->feature_instances;
    const auto feature = std::find_if(features.begin(), features.end(), [&](const auto &entry) {
        return entry.feature == contract->provider_feature;
    });
    if (feature == features.end() || !findRenderFeatureParameter(*feature, "near_distance") ||
        !findRenderFeatureParameter(*feature, "far_distance"))
        throw std::runtime_error("punctual shadow feature requires near_distance and far_distance");
    const auto near_distance = renderFeatureFloatParameter(*feature, "near_distance", 0);
    const auto far_distance = renderFeatureFloatParameter(*feature, "far_distance", 0);
    if (family.point) {
        const std::array dummy{PointLight{.position = {0, 0, 0}}};
        const auto lights = context.lights.pointLights();
        return buildPointShadowViewFamily(lights.empty() ? std::span<const PointLight>{dummy} : lights,
                                         near_distance, far_distance);
    }
    const std::array dummy{SpotLight{.position = {0, 0, 0}, .direction = {0, 0, -1}, .outerConeAngle = 45}};
    const auto lights = context.lights.spotLights();
    return buildSpotShadowViewFamily(lights.empty() ? std::span<const SpotLight>{dummy} : lights,
                                    near_distance, far_distance);
}

void validateFamily(const RenderViewFamilies &families,
                    const RenderViewFamilyProviderContext &context, const ShadowFamily &definition) {
    const auto *contract = findContract(context.frame_graph, definition);
    if (!contract) return; // Authored families can be used without the standard receiver.
    const auto *family = families.find(definition.family);
    const auto count = viewCount(context.lights, definition);
    const auto target = context.render_targets.getMetadata(targetId(context.frame_graph, *contract));
    if (!family || family->views.size() != count || target.array_layers != count ||
        target.extent.width != target.extent.height)
        throw std::runtime_error("punctual shadow views require a square target with matching light/face layers");
}
} // namespace

void registerPunctualShadowViewProviders(RenderViewFamilyProviderRegistry &registry) {
    for (const auto family : shadow_families) {
        registry.registerProvider({
            .name = family.point ? "standard.point_shadow_v1" : "standard.spot_shadow_v1",
            .family_id = std::string{family.family},
            // Point: index = light * 6 + face. Spot: index = light.
            .axes = family.point
                ? std::vector<RenderViewAxisDefinition>{
                      {.name = std::string{renderViewAxisLight}},
                      {.name = std::string{renderViewAxisFace},
                       .compile_time_length = [](const RenderViewAxisLengthContext &) { return 6u; },
                       .labels = renderViewAxisFaceLabels()}}
                : std::vector<RenderViewAxisDefinition>{
                      {.name = std::string{renderViewAxisLight}}},
            .build = [family](const RenderViewFamilies &, const RenderViewFamilyProviderContext &context) {
                return buildFamily(context, family);
            },
            .validate = [family](const RenderViewFamilies &families, const RenderViewFamilyProviderContext &context) {
                validateFamily(families, context, family);
            }});
    }
}

bool preparePunctualShadowRenderTargets(const CompiledFrameGraphExecution &graph,
    const LightContainer &lights, RenderTargetContainer &targets) {
    bool changed = false;
    for (const auto family : shadow_families) {
        if (const auto *contract = findContract(graph, family))
            changed |= targets.setRuntimeArrayLayers(targetId(graph, *contract), viewCount(lights, family));
    }
    return changed;
}
} // namespace Pelican
