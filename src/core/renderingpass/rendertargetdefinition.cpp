#include "rendertargetdefinition.hpp"
#include "rendertargetmetadata.hpp"

#include "../../project/graphcomposition.hpp"

namespace Pelican {

RenderTargetDefinition renderTargetDefinitionOf(const ImageResource &image) {
    if (image.format_unresolved) {
        throw GraphRegistryError("render_graph.image_format_unresolved@1", {{"resource", image.name}});
    }
    const bool fixed = image.extent.kind == ResourceExtentKind::fixed;
    return RenderTargetDefinition{
        .name = image.name,
        .format_class = image.format_class,
        .role = image.role,
        .extent_scale = fixed ? 1.0f : image.extent.scale_x,
        .fixed_extent = fixed ? std::optional{ResolvedResourceExtent{image.extent.width, image.extent.height}}
                              : std::nullopt,
        .format = image.format,
        .format_candidates = image.format_candidates,
        .usage = image.usage,
        .history = image.history,
        .history_clear_color = image.clear_color,
        .samples = 1,
        .mip_levels = image.mip_levels,
        .array_layers = image.array_layers,
        .dimension = image.dimension,
        .source = image.source,
        .persistent = image.persistent,
    };
}

std::vector<RenderTargetDefinition> renderTargetDefinitionsOf(const RenderGraphIR &render_graph) {
    std::vector<RenderTargetDefinition> result;
    result.reserve(render_graph.images.size());
    for (const auto &image : render_graph.images) result.push_back(renderTargetDefinitionOf(image));
    return result;
}

RenderTargetDefinition renderTargetDefinitionOf(const GenerationImage &image) {
    auto definition = renderTargetDefinitionOf(image.declaration);
    definition.format = image.format;
    definition.usage = image.usage;
    definition.samples = image.samples;
    definition.array_layers = image.array_layers;
    definition.storage_mode = image.storage;
    definition.alias_group = image.alias_group;
    definition.persistent = image.persistent;
    return definition;
}

RenderTargetMetadata renderTargetMetadataOf(const GenerationImage &image) {
    return RenderTargetMetadata{
        .name = image.declaration.name,
        .usage = image.usage,
        .format = image.format,
        .extent = image.extent,
        .history = image.declaration.history,
        .samples = image.samples,
        .mip_levels = resolveImageMipLevels(image.declaration.mip_levels, image.extent.width, image.extent.height),
        .array_layers = image.array_layers,
        .dimension = image.declaration.dimension,
        .storage_mode = image.storage,
        .alias_group = image.alias_group,
    };
}

} // namespace Pelican
