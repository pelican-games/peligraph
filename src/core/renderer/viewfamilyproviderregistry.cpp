#include "viewfamilyproviderregistry.hpp"

#include "directionalshadowviewprovider.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/frameplanner.hpp"
#include "../renderingpass/rendertargetdefinition.hpp"
#include "../../project/viewselect.hpp"
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
#include "../render_algorithms/standardrenderalgorithms.hpp"
#endif

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
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
    std::set<std::string, std::less<>> axis_names;
    for (const auto &axis : provider.axes) {
        if (axis.name.empty() ||
            !axis_names.insert(axis.name).second) {
            throw std::invalid_argument(
                "render view-family provider '" +
                provider.name +
                "' declares an empty or duplicate axis");
        }
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

RenderViewAxisLengthContext renderViewAxisLengthContext(
    const CompiledRenderPipeline &pipeline) noexcept {
    return {
        .feature_instances =
            pipeline.feature_instances,
        .surface_resource_contracts =
            pipeline.surface_resource_contracts,
    };
}

namespace {

RenderViewAxisLength declaredAxisLength(
    const RenderViewAxisDefinition &definition,
    const RenderViewAxisLengthContext &context) {
    if (!definition.compile_time_length) {
        return {
            .kind =
                RenderViewAxisLengthKind::runtime,
        };
    }
    return {
        .kind =
            RenderViewAxisLengthKind::compile_time,
        .length =
            definition.compile_time_length(
                context),
    };
}

} // namespace

std::vector<std::string> renderViewAxisFaceLabels() {
    return {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
}

RenderViewAxisLength renderViewAxisLength(
    std::string_view family_id,
    std::string_view axis,
    const RenderViewAxisLengthContext &context,
    const RenderViewFamilyProviderRegistry &providers) {
    const auto *provider =
        providers.find(family_id);
    if (provider == nullptr) {
        return {};
    }
    for (const auto &definition :
         provider->axes) {
        if (definition.name == axis) {
            return declaredAxisLength(
                definition, context);
        }
    }
    return {};
}

RenderGraphViewAxisLengthResolver renderGraphViewAxisLengthResolver(
    const RenderViewFamilyProviderRegistry &providers) {
    return [&providers](
               std::string_view family, std::string_view axis,
               const std::vector<CompiledRenderFeatureInstance>
                   &feature_instances,
               const std::vector<CompiledSurfaceResourceContract>
                   &surface_resource_contracts) {
        const auto length = renderViewAxisLength(
            family, axis,
            RenderViewAxisLengthContext{
                .feature_instances = feature_instances,
                .surface_resource_contracts =
                    surface_resource_contracts,
            },
            providers);
        RenderGraphViewAxisLength result;
        result.length = length.length;
        switch (length.kind) {
        case RenderViewAxisLengthKind::unknown:
            result.kind = RenderGraphViewAxisKind::unknown;
            break;
        case RenderViewAxisLengthKind::runtime:
            result.kind = RenderGraphViewAxisKind::runtime;
            break;
        case RenderViewAxisLengthKind::compile_time:
            result.kind = RenderGraphViewAxisKind::compile_time;
            break;
        }
        return result;
    };
}

std::vector<RenderViewAxisDescription> renderViewFamilyAxes(
    std::string_view family_id,
    const RenderViewAxisLengthContext &context,
    const RenderViewFamilyProviderRegistry &providers) {
    std::vector<RenderViewAxisDescription> result;
    const auto *provider =
        providers.find(family_id);
    if (provider == nullptr) {
        return result;
    }
    result.reserve(provider->axes.size());
    for (const auto &definition :
         provider->axes) {
        result.push_back({
            .name = definition.name,
            .length = declaredAxisLength(
                definition, context),
            .labels = definition.labels,
        });
    }
    return result;
}

void validateRenderViewSelects(
    std::span<const FrameGraphDefinition> graphs,
    const CompiledRenderPipeline &pipeline,
    const RenderViewFamilyProviderRegistry &providers) {
    const auto context =
        renderViewAxisLengthContext(pipeline);
    for (const auto &graph : graphs) {
        for (const auto &node : graph.nodes) {
            if (node.view_select.empty()) {
                continue;
            }
            if (node.view_family ==
                mainRenderViewFamilyId) {
                throwViewSelectRefusal(
                    viewSelectPrimaryFamilyCode,
                    {{"pass", node.name},
                     {"family", node.view_family}});
            }
            for (const auto &[axis, value] :
                 node.view_select) {
                const auto length =
                    renderViewAxisLength(
                        node.view_family, axis,
                        context, providers);
                switch (length.kind) {
                case RenderViewAxisLengthKind::unknown:
                    throwViewSelectRefusal(
                        viewSelectAxisUnknownCode,
                        {{"pass", node.name},
                         {"family", node.view_family},
                         {"axis", axis}});
                case RenderViewAxisLengthKind::runtime:
                    throwViewSelectRefusal(
                        viewSelectRuntimeAxisCode,
                        {{"pass", node.name},
                         {"family", node.view_family},
                         {"axis", axis}});
                case RenderViewAxisLengthKind::compile_time:
                    if (value >= length.length) {
                        throwViewSelectRefusal(
                            viewSelectAxisUnknownCode,
                            {{"pass", node.name},
                             {"family", node.view_family},
                             {"axis", axis},
                             {"value", value},
                             {"length", length.length}});
                    }
                    break;
                }
            }
        }
    }
}

namespace {

// VF-1: one raster attachment of a secondary-family pass, with the extent
// its target is declared at.
struct ViewFamilyRasterWrite {
    std::string pass;
    std::string target;
    std::optional<vk::Extent2D> fixed_extent;
    float extent_scale = 1.0f;
};

bool sameDeclaredExtent(const ViewFamilyRasterWrite &left,
                        const ViewFamilyRasterWrite &right) {
    if (left.fixed_extent || right.fixed_extent) {
        return left.fixed_extent && right.fixed_extent &&
               *left.fixed_extent == *right.fixed_extent;
    }
    return left.extent_scale == right.extent_scale;
}

nlohmann::json declaredExtentJson(const ViewFamilyRasterWrite &write) {
    if (write.fixed_extent) {
        return nlohmann::json::array({write.fixed_extent->width,
                                      write.fixed_extent->height});
    }
    return {{"output_scale", write.extent_scale}};
}

const RenderLoopOrigin *passLoopOrigin(
    const CompiledRenderPipeline &pipeline, std::string_view pass) {
    for (const auto &provenance : pipeline.pass_provenance) {
        if (provenance.name == pass && provenance.loop_origin) {
            return &*provenance.loop_origin;
        }
    }
    return nullptr;
}

} // namespace

void validateRenderViewFamilyExtents(
    std::span<const FrameGraphDefinition> graphs,
    std::span<const RenderTargetDefinition> targets,
    const CompiledRenderPipeline &pipeline) {
    std::map<std::string, const RenderTargetDefinition *, std::less<>>
        definitions;
    for (const auto &target : targets) {
        definitions.emplace(target.name, &target);
    }
    std::map<std::string, ViewFamilyRasterWrite, std::less<>> first_write;
    for (const auto &graph : graphs) {
        for (const auto &node : graph.nodes) {
            if (node.kind != FramePlanNodeKind::render ||
                node.view_family == mainRenderViewFamilyId) {
                continue;
            }
            for (const auto &attachment : node.attachments) {
                const auto found = definitions.find(attachment.resource);
                if (found == definitions.end()) {
                    continue;
                }
                ViewFamilyRasterWrite write{
                    .pass = node.name,
                    .target = attachment.resource,
                    .fixed_extent = found->second->fixed_extent,
                    .extent_scale = found->second->extent_scale,
                };
                const auto [expected, inserted] =
                    first_write.try_emplace(node.view_family, write);
                if (inserted ||
                    sameDeclaredExtent(expected->second, write)) {
                    continue;
                }
                nlohmann::json fields{
                    {"family", node.view_family},
                    {"pass", write.pass},
                    {"target", write.target},
                    {"extent", declaredExtentJson(write)},
                    {"expected_pass", expected->second.pass},
                    {"expected_target", expected->second.target},
                    {"expected_extent",
                     declaredExtentJson(expected->second)},
                };
                const auto originJson = [](const RenderLoopOrigin &origin) {
                    return nlohmann::json{{"loop", origin.loop},
                                          {"element", origin.element},
                                          {"node", origin.node}};
                };
                if (const auto *origin =
                        passLoopOrigin(pipeline, write.pass)) {
                    fields["loop_origin"] = originJson(*origin);
                }
                if (const auto *origin = passLoopOrigin(
                        pipeline, expected->second.pass)) {
                    fields["expected_loop_origin"] = originJson(*origin);
                }
                throw std::runtime_error(viewSelectRefusalMessage(
                    viewFamilyExtentMismatchCode, fields));
            }
        }
    }
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
