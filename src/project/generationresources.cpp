#include "generationresources.hpp"

#include "graphcomposition.hpp"
#include "materialscreeninput.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace Pelican {

ImageUsage engineImageUsage(const ImageUsageFacts &facts) {
    auto usage = facts.declared;
    if (facts.local_read) usage |= ImageUsageBit::input_attachment;
    if (facts.external_depth_source) usage |= ImageUsageBit::transfer_src;
    switch (facts.storage) {
    case RenderTargetStorageMode::materialized:
        break;
    case RenderTargetStorageMode::transient_attachment:
        usage |= ImageUsageBit::transient_attachment;
        break;
    case RenderTargetStorageMode::tile_local_attachment:
        usage = (usage & (ImageUsageBit::color_attachment | ImageUsageBit::depth_stencil_attachment)) |
                ImageUsageBit::input_attachment | ImageUsageBit::transient_attachment;
        break;
    }
    if (facts.history) usage |= ImageUsageBit::transfer_dst;
    return usage;
}

ImageUsage requiredImageUsage(ImageUseRequirement use) {
    switch (use) {
    case ImageUseRequirement::shader_input:
        return ImageUsageBit::sampled;
    case ImageUseRequirement::copy_source:
        return ImageUsageBit::transfer_src;
    case ImageUseRequirement::copy_destination:
        return ImageUsageBit::transfer_dst | ImageUsageBit::sampled;
    case ImageUseRequirement::initial_contents:
        return ImageUsageBit::transfer_dst;
    case ImageUseRequirement::file_source:
        return ImageUsageBit::sampled | ImageUsageBit::transfer_dst;
    case ImageUseRequirement::file_source_mips:
        return ImageUsageBit::sampled | ImageUsageBit::transfer_dst | ImageUsageBit::transfer_src;
    }
    throw std::logic_error("unknown image use requirement");
}

const GenerationImage *GenerationResources::findImage(std::string_view name) const {
    const auto found = std::find_if(images.begin(), images.end(),
                                    [&](const GenerationImage &image) { return image.declaration.name == name; });
    return found == images.end() ? nullptr : &*found;
}

const FrameGraphBufferDefinition *GenerationResources::findBuffer(std::string_view name) const {
    const auto found = std::find_if(buffers.begin(), buffers.end(),
                                    [&](const FrameGraphBufferDefinition &buffer) { return buffer.name == name; });
    return found == buffers.end() ? nullptr : &*found;
}

namespace {

[[noreturn]] void refuse(std::string code, nlohmann::json detail) {
    throw GraphRegistryError(std::move(code), std::move(detail));
}

// An image as the variants so far placed it.
struct MergedImage {
    ImageResource declaration;
    std::string first_variant;
    ImagePlacement placement;
    std::optional<RenderTargetStorageMode> storage;
    bool local_read = false;
    bool external_depth_source = false;
    bool runtime_array_layers = false;
};

// The first field two declarations of one buffer differ in.
std::string_view firstDifferentField(const FrameGraphBufferDefinition &left,
                                     const FrameGraphBufferDefinition &right) {
    if (left.size != right.size) return "size";
    if (left.extent_size != right.extent_size) return "size_from_extent";
    if (left.persistent != right.persistent) return "lifetime";
    if (left.host_source != right.host_source) return "host_source";
    if (left.command_layout != right.command_layout) return "command_layout";
    if (left.host_view_family != right.host_view_family) return "view_family";
    if (left.index_buffer != right.index_buffer) return "index_buffer";
    return "initial_f32";
}

} // namespace

GenerationResources resolveGenerationResources(std::span<const GenerationResourceVariant> variants,
                                               ResolvedResourceExtent output_extent) {
    GenerationResources result;
    result.variants.resize(variants.size());
    std::vector<MergedImage> merged;
    std::unordered_map<std::string, std::size_t> image_index;
    // Per variant: its placement of each of its images.
    std::vector<std::unordered_map<std::string, ImagePlacement>> placed(variants.size());

    for (std::size_t v = 0; v < variants.size(); ++v) {
        const auto &variant = variants[v];
        if (variant.render_graph == nullptr)
            throw std::logic_error("a generation's resource variant has no render graph: " + variant.name);
        const auto &images = variant.render_graph->images;
        std::unordered_map<std::string, const ImagePlacement *> placements;
        for (const auto &placement : variant.placements) {
            if (std::none_of(images.begin(), images.end(),
                             [&](const ImageResource &image) { return image.name == placement.resource; })) {
                refuse("render_graph.image_placement_invalid@1",
                       {{"variant", variant.name}, {"resource", placement.resource}, {"reason", "unknown_image"}});
            }
            if (!placements.emplace(placement.resource, &placement).second) {
                refuse("render_graph.image_placement_invalid@1",
                       {{"variant", variant.name}, {"resource", placement.resource}, {"reason", "duplicate"}});
            }
        }
        for (const auto &image : images) {
            ImagePlacement placement{.resource = image.name,
                                     .format = image.format,
                                     .array_layers = image.array_layers};
            if (!variant.placements.empty()) {
                const auto found = placements.find(image.name);
                if (found == placements.end()) {
                    refuse("render_graph.image_placement_invalid@1",
                           {{"variant", variant.name}, {"resource", image.name}, {"reason", "missing"}});
                }
                placement = *found->second;
            }
            placed[v].emplace(image.name, placement);
            const auto [at, inserted] = image_index.emplace(image.name, merged.size());
            if (inserted) {
                merged.push_back(MergedImage{.declaration = image,
                                             .first_variant = variant.name,
                                             .placement = placement,
                                             .storage = placement.storage,
                                             .local_read = placement.local_read,
                                             .external_depth_source = placement.external_depth_source});
                result.variants[v].images.push_back(merged.size() - 1);
                continue;
            }
            auto &entry = merged[at->second];
            const auto conflict = [&](std::string_view field) {
                refuse("render_graph.image_variant_conflict@1",
                       {{"resource", image.name},
                        {"field", std::string{field}},
                        {"variants", nlohmann::json::array({entry.first_variant, variant.name})}});
            };
            if (entry.placement.format != placement.format) conflict("format");
            if (entry.declaration.mip_levels != image.mip_levels) conflict("mip_levels");
            if (entry.declaration.dimension != image.dimension) conflict("dimension");
            if (entry.declaration.extent != image.extent) conflict("extent");
            if (entry.placement.samples != placement.samples) conflict("samples");
            if (entry.declaration.history != image.history) conflict("history");
            entry.declaration.usage |= image.usage;
            entry.declaration.persistent = entry.declaration.persistent || image.persistent;
            entry.placement.array_layers = std::max(entry.placement.array_layers, placement.array_layers);
            // Variant images share one allocation: materialized is the safe
            // superset when (for example) flat keeps a transient depth the XR
            // variant exports.
            if (entry.storage != placement.storage) entry.storage = RenderTargetStorageMode::materialized;
            entry.local_read = entry.local_read || placement.local_read;
            entry.external_depth_source = entry.external_depth_source || placement.external_depth_source;
            result.variants[v].images.push_back(at->second);
        }
        for (const auto &name : variant.runtime_layered_images)
            if (const auto found = image_index.find(name); found != image_index.end())
                merged[found->second].runtime_array_layers = true;
    }

    // The alias groups: a group stands when each variant has all its members
    // or none, and places every member it has in it.
    std::map<std::string, std::set<std::string>> groups;
    for (const auto &variant_placed : placed)
        for (const auto &[name, placement] : variant_placed)
            if (placement.alias_group) groups[*placement.alias_group].insert(name);
    std::set<std::string> unshared;
    for (const auto &[group, members] : groups) {
        for (const auto &variant_placed : placed) {
            std::size_t present = 0;
            bool placed_in_group = true;
            for (const auto &member : members) {
                const auto found = variant_placed.find(member);
                if (found == variant_placed.end()) continue;
                ++present;
                placed_in_group = placed_in_group && found->second.alias_group == group;
            }
            if (present == 0) continue;
            if (present != members.size() || !placed_in_group) unshared.insert(group);
        }
        // A layer count the runtime changes cannot share an allocation.
        for (const auto &member : members)
            if (merged[image_index.at(member)].runtime_array_layers) unshared.insert(group);
    }

    result.images.reserve(merged.size());
    for (auto &entry : merged) {
        GenerationImage image;
        image.format = entry.placement.format;
        image.samples = entry.placement.samples;
        image.array_layers = entry.placement.array_layers;
        image.storage = *entry.storage;
        image.persistent = entry.declaration.persistent;
        image.runtime_array_layers = entry.runtime_array_layers;
        image.usage = engineImageUsage({.declared = entry.declaration.usage,
                                        .history = entry.declaration.history,
                                        .storage = image.storage,
                                        .local_read = entry.local_read,
                                        .external_depth_source = entry.external_depth_source});
        image.extent = resolveResourceExtent(entry.declaration.extent, output_extent);
        // The group the variants placed it in, when that group stands.
        for (const auto &variant_placed : placed) {
            const auto found = variant_placed.find(entry.declaration.name);
            if (found == variant_placed.end() || !found->second.alias_group) continue;
            if (!unshared.contains(*found->second.alias_group)) image.alias_group = found->second.alias_group;
        }
        if (image.runtime_array_layers) {
            const auto invalid = [&](std::string_view reason) {
                refuse("render_graph.image_runtime_layers_invalid@1",
                       {{"resource", entry.declaration.name}, {"reason", std::string{reason}}});
            };
            if (entry.declaration.history) invalid("history");
            if (entry.declaration.dimension != ImageResourceDimension::two_d) invalid("not_2d");
            if (entry.declaration.source) invalid("file_source");
        }
        image.declaration = std::move(entry.declaration);
        result.images.push_back(std::move(image));
    }

    // Each group left: two members or more, of one contract.
    std::map<std::string, std::vector<const GenerationImage *>> standing;
    for (const auto &image : result.images)
        if (image.alias_group) standing[*image.alias_group].push_back(&image);
    for (const auto &[group, members] : standing) {
        if (members.size() < 2)
            refuse("render_graph.alias_group_invalid@1", {{"group", group}, {"reason", "member_count"}});
        const auto &contract = *members.front();
        for (const auto *member : members) {
            if (member->format != contract.format || member->usage != contract.usage ||
                member->declaration.extent != contract.declaration.extent || member->samples != contract.samples ||
                member->declaration.mip_levels != contract.declaration.mip_levels ||
                member->array_layers != contract.array_layers ||
                member->declaration.dimension != contract.declaration.dimension ||
                member->storage != contract.storage ||
                member->storage != RenderTargetStorageMode::materialized || member->declaration.history) {
                refuse("render_graph.alias_group_invalid@1",
                       {{"group", group}, {"reason", "contract"}, {"resource", member->declaration.name}});
            }
        }
    }

    // The buffers: one declaration per name, every field the same.
    std::unordered_map<std::string, std::pair<std::size_t, std::string>> buffer_index;
    for (std::size_t v = 0; v < variants.size(); ++v) {
        for (const auto &buffer : variants[v].render_graph->buffers) {
            const auto [at, inserted] =
                buffer_index.emplace(buffer.name, std::pair{result.buffers.size(), variants[v].name});
            if (inserted) {
                result.buffers.push_back(buffer);
            } else if (result.buffers[at->second.first] != buffer) {
                refuse("render_graph.buffer_variant_conflict@1",
                       {{"resource", buffer.name},
                        {"field", std::string{firstDifferentField(result.buffers[at->second.first], buffer)}},
                        {"variants", nlohmann::json::array({at->second.second, variants[v].name})}});
            }
            result.variants[v].buffers.push_back(at->second.first);
        }
    }
    for (auto &buffer : result.buffers) {
        if (!buffer.extent_size) continue;
        const auto *image = result.findImage(buffer.extent_size->resource);
        if (image == nullptr) {
            refuse("render_graph.buffer_invalid@1",
                   {{"resource", buffer.name}, {"field", "size_from_extent"}, {"reason", "unknown_image"}});
        }
        buffer.size = extentDerivedBufferBytes(buffer, image->extent);
        requireBufferSizeContract(buffer);
    }
    return result;
}

std::vector<std::string> runtimeLayeredImagesOf(const CompiledRenderPipeline &pipeline) {
    std::vector<std::string> result;
    for (const auto &contract : pipeline.surface_resource_contracts) {
        const auto &name = contract.contract.name;
        if ((name == directionalShadowInputContractName || name == pointShadowInputContractName ||
             name == spotShadowInputContractName) &&
            std::find(result.begin(), result.end(), contract.resource) == result.end()) {
            result.push_back(contract.resource);
        }
    }
    return result;
}

} // namespace Pelican
