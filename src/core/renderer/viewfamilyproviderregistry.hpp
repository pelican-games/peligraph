#pragma once

#include "viewfamily.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct CompiledFrameGraphExecution;
class LightContainer;
class RenderTargetContainer;

// Immutable engine inputs shared by runtime view-family providers. Providers
// return logical view data only; the scheduler and backend continue to own
// execution, temporal state, resources, and Vulkan lowering.
struct RenderViewFamilyProviderContext {
    const CompiledFrameGraphExecution
        &frame_graph;
    const LightContainer &lights;
    const RenderTargetContainer
        &render_targets;
};

struct RenderViewFamilyProviderDefinition {
    std::string name;
    std::string family_id;
    std::function<RenderViewFamily(
        const RenderViewFamilies &,
        const RenderViewFamilyProviderContext &)>
        build;
    std::function<void(
        const RenderViewFamilies &,
        const RenderViewFamilyProviderContext &)>
        validate;
};

// Source-level registry used by engine and optional render-algorithm packages.
// Authored RenderViewFamilies remain the highest-priority replacement path:
// a provider runs only when a graph-required family was not supplied.
class RenderViewFamilyProviderRegistry {
    std::vector<
        RenderViewFamilyProviderDefinition>
        providers_;

  public:
    void registerProvider(
        RenderViewFamilyProviderDefinition
            provider);

    const RenderViewFamilyProviderDefinition *
    find(std::string_view family_id) const
        noexcept;

    const std::vector<
        RenderViewFamilyProviderDefinition> &
    providers() const noexcept {
        return providers_;
    }
};

RenderViewFamilyProviderRegistry &
renderViewFamilyProviderRegistry();

RenderViewFamilies resolveRuntimeRenderViewFamilies(
    const RenderViewFamilies &authored,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers =
            renderViewFamilyProviderRegistry());

// Call after generic RenderViewFamilies validation so provider callbacks can
// assume structurally valid family/view identities.
void validateRuntimeRenderViewFamilyProviderContracts(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers =
            renderViewFamilyProviderRegistry());

} // namespace Pelican
