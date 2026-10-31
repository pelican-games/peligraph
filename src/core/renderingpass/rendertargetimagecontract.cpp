#include "rendertargetimagecontract.hpp"
#include "vulkanimagevocabulary.hpp"
#include "../../project/generationresources.hpp"
#include <stdexcept>
#include <map>
#include <algorithm>
#include <set>

namespace Pelican {

RenderTargetImageContract renderTargetImageContract(
    const RenderTargetDefinition &definition, vk::Extent2D base_extent,
    std::uint32_t graphics_queue_family, bool attachment,
    const RenderTargetImageContract *live) {
    RenderTargetImageContract result;
    result.usage = attachment
        ? definition.usage & (ImageUsageBit::color_attachment |
                              ImageUsageBit::depth_stencil_attachment)
        : engineImageUsage({.declared = definition.usage,
                            .history = definition.history,
                            .storage = definition.storage_mode});
    if (live) {
        std::string changed;
        if ((live->usage & result.usage) != result.usage) changed = "usage expansion";
        if (live->image.arrayLayers < definition.array_layers) {
            if (!changed.empty()) changed += ", ";
            changed += "array_layers expansion";
        }
        if (!changed.empty()) throw std::runtime_error(
            "Render target re-registration changed its physical contract: " + definition.name + " (" + changed + ")");
        return *live;
    }
    const auto extent = resolveResourceExtent(
        definition.fixed_extent
            ? ResourceExtentPlan{.kind = ResourceExtentKind::fixed,
                  .width = definition.fixed_extent->width,
                  .height = definition.fixed_extent->height}
            : ResourceExtentPlan{.kind = ResourceExtentKind::output_relative,
                  .scale_x = definition.extent_scale,
                  .scale_y = definition.extent_scale},
        {base_extent.width, base_extent.height});
    if (!extent.width || !extent.height)
        throw std::runtime_error("Render target extent became zero-sized: " + definition.name);
    if (definition.dimension == ImageResourceDimension::cube &&
        (definition.array_layers != 6 || extent.width != extent.height))
        throw std::runtime_error("Cube render target image creation requires a square extent and exactly six array layers: " + definition.name);
    auto &info = result.image;
    info.imageType = vk::ImageType::e2D;
    info.format = toVulkanFormat(definition.format);
    info.extent = vk::Extent3D{extent.width, extent.height, 1};
    info.mipLevels = attachment ? 1 : resolveImageMipLevels(
        definition.mip_levels, extent.width, extent.height);
    info.arrayLayers = definition.array_layers;
    info.samples = attachment ? static_cast<vk::SampleCountFlagBits>(definition.samples)
                              : vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = toVulkanImageUsage(result.usage);
    info.sharingMode = vk::SharingMode::eExclusive;
    info.initialLayout = vk::ImageLayout::eUndefined;
    if (!attachment && (definition.alias_group || definition.planned_memory_region)) info.flags |= vk::ImageCreateFlagBits::eAlias;
    if (definition.dimension == ImageResourceDimension::cube)
        info.flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    result.queue_family = graphics_queue_family;
    return result;
}

} // namespace Pelican
