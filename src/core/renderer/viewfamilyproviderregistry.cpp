#include "viewfamilyproviderregistry.hpp"

#include "directionalshadowviewprovider.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
#include "../render_algorithms/standardrenderalgorithms.hpp"
#endif

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {

void RenderViewFamilyProviderRegistry::
    registerProvider(
        RenderViewFamilyProviderDefinition
            provider) {
    if (provider.name.empty()) {
        throw std::invalid_argument(
            "render view-family provider requires a name");
    }
    validateRenderViewFamilyId(
        provider.family_id,
        "render view-family provider '" +
            provider.name + "'");
    if (!provider.build) {
        throw std::invalid_argument(
            "render view-family provider '" +
            provider.name +
            "' requires a build callback");
    }
    if (find(provider.family_id) != nullptr) {
        throw std::invalid_argument(
            "render view-family provider for '" +
            provider.family_id +
            "' is already registered");
    }
    providers_.push_back(
        std::move(provider));
}

const RenderViewFamilyProviderDefinition *
RenderViewFamilyProviderRegistry::find(
    std::string_view family_id) const
    noexcept {
    const auto found = std::find_if(
        providers_.begin(), providers_.end(),
        [family_id](
            const RenderViewFamilyProviderDefinition
                &provider) {
            return provider.family_id ==
                   family_id;
        });
    return found != providers_.end()
               ? &*found
               : nullptr;
}

RenderViewFamilyProviderRegistry &
renderViewFamilyProviderRegistry() {
    static auto *value = [] {
        auto registry =
            std::make_unique<
                RenderViewFamilyProviderRegistry>();
        registerDirectionalShadowViewFamilyProvider(
            *registry);
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
        registerStandardRenderAlgorithmProviders(
            *registry);
#endif
        return registry.release();
    }();
    return *value;
}

RenderViewFamilies resolveRuntimeRenderViewFamilies(
    const RenderViewFamilies &authored,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers) {
    RenderViewFamilies result;
    result.families.push_back(
        authored.require(
            mainRenderViewFamilyId));
    for (const auto &family :
         authored.families) {
        if (family.family_id ==
            mainRenderViewFamilyId) {
            continue;
        }
        result.families.push_back(
            family);
    }

    for (const auto &node :
         context.frame_graph.nodes) {
        if (node.view_family ==
            mainRenderViewFamilyId) {
            continue;
        }
        if (result.find(
                node.view_family) != nullptr) {
            continue;
        }
        const auto *provider =
            providers.find(
                node.view_family);
        if (provider == nullptr) {
            throw std::runtime_error(
                "compiled frame graph node '" +
                node.name +
                "' requires unavailable view family '" +
                node.view_family +
                "'; supply it explicitly or register a provider");
        }
        auto family =
            provider->build(
                result, context);
        if (family.family_id !=
            node.view_family) {
            throw std::runtime_error(
                "render view-family provider '" +
                provider->name +
                "' returned family '" +
                family.family_id +
                "' while resolving '" +
                node.view_family + "'");
        }
        result.families.push_back(
            std::move(family));
    }
    return result;
}

void validateRuntimeRenderViewFamilyProviderContracts(
    const RenderViewFamilies &families,
    const RenderViewFamilyProviderContext
        &context,
    const RenderViewFamilyProviderRegistry
        &providers) {
    std::set<std::string, std::less<>>
        required_families;
    for (const auto &node :
         context.frame_graph.nodes) {
        if (node.view_family !=
            mainRenderViewFamilyId) {
            required_families.insert(
                node.view_family);
        }
    }
    for (const auto &family_id :
         required_families) {
        const auto *provider =
            providers.find(family_id);
        if (provider != nullptr &&
            provider->validate) {
            provider->validate(
                families, context);
        }
    }
}

} // namespace Pelican
