#pragma once

// RC-1b: what registerRenderTargetDefinitions makes of a set of render target
// definitions, for tests that cannot create the RenderTargetContainer (it
// needs a Vulkan device): each target's metadata as getMetadata reports it
// and its alias token as aliasGroup does.

#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetdefinition.hpp"
#include "../src/core/renderingpass/rendertargetmemory.hpp"
#include "../src/core/renderingpass/rendertargetmetadata.hpp"
#include "../src/project/imagesubresource.hpp"
#include "../src/project/generationresources.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include "render_graph_ir_test_support.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::test_support {

struct RegisteredRenderTargetStandIn {
    RenderTargetMetadata metadata;
    std::optional<std::uint64_t> alias_group;
};

// RC-3c2a: the declarations of a config (or of a graph root's rendering
// section): its render_targets and buffers alone, as parseRenderGraph reads
// them (whatever its passes say).
inline nlohmann::json resourceDeclarationsOf(const nlohmann::json &document) {
    const auto &config = document.contains("rendering") ? document.at("rendering") : document;
    nlohmann::json declarations = nlohmann::json::object();
    for (const auto *key : {"render_targets", "buffers"})
        if (const auto found = config.find(key); found != config.end()) declarations[key] = *found;
    return declarations;
}

// RC-3c2a: a config's render targets as the compiler makes them (the IR's
// declarations, renderTargetDefinitionsOf).
inline std::vector<RenderTargetDefinition> renderTargetDefinitionsOf(const nlohmann::json &config) {
    return Pelican::renderTargetDefinitionsOf(TestSupport::renderGraphOf(resourceDeclarationsOf(config)));
}

// RC-3c2a: `render_graph`'s config images as the GPU registration takes them
// from one variant's physical plan (imagePlacementsOf, then the generation's
// resource description at `output_extent`).
inline std::vector<RenderTargetDefinition> plannedRenderTargets(
    const RenderGraphIR &render_graph, const RenderingTargetPlanCompilation &compilation,
    ResolvedResourceExtent output_extent = {1920, 1080}) {
    const auto variant = GenerationResourceVariant{
        .name = "test",
        .render_graph = &render_graph,
        .placements = imagePlacementsOf(Pelican::renderTargetDefinitionsOf(render_graph), compilation),
    };
    const auto resources = resolveGenerationResources(std::span{&variant, 1}, output_extent);
    std::vector<RenderTargetDefinition> result;
    for (const auto &image : resources.images) result.push_back(renderTargetDefinitionOf(image));
    return result;
}

// RC-3c2a: a config's buffers as the generation's resource description sizes
// them at `output_extent` (one variant, its images as declared).
inline std::vector<FrameGraphBufferDefinition> bufferDefinitionsOf(
    const nlohmann::json &config, ResolvedResourceExtent output_extent = {1920, 1080}) {
    const auto render_graph = TestSupport::renderGraphOf(resourceDeclarationsOf(config));
    const auto variant = GenerationResourceVariant{.name = "test", .render_graph = &render_graph};
    return resolveGenerationResources(std::span{&variant, 1}, output_extent).buffers;
}

// `targets` as the generation's resource description leaves them, registered at
// `base_extent` in their order: the extent rule of resolveRenderTargetExtent,
// the mip count the image gets, and one alias token per alias group name,
// from 1, as registration hands them out.
inline std::vector<RegisteredRenderTargetStandIn>
registerRenderTargetsWithoutDevice(
    std::span<const RenderTargetDefinition> targets,
    vk::Extent2D base_extent) {
    std::map<std::string, std::uint64_t, std::less<>> tokens;
    std::vector<RegisteredRenderTargetStandIn> registered;
    registered.reserve(targets.size());
    for (const auto &target : targets) {
        const auto extent = resolveResourceExtent(
            target.fixed_extent
                ? ResourceExtentPlan{
                      .kind = ResourceExtentKind::fixed,
                      .width = target.fixed_extent->width,
                      .height = target.fixed_extent->height,
                  }
                : ResourceExtentPlan{
                      .kind = ResourceExtentKind::output_relative,
                      .scale_x = target.extent_scale,
                      .scale_y = target.extent_scale,
                  },
            ResolvedResourceExtent{base_extent.width, base_extent.height});
        std::optional<std::uint64_t> token;
        if (target.alias_group) {
            const auto next = tokens.size() + 1;
            token = tokens.try_emplace(*target.alias_group, next)
                        .first->second;
        }
        registered.push_back(RegisteredRenderTargetStandIn{
            .metadata =
                RenderTargetMetadata{
                    .name = target.name,
                    .usage = target.usage,
                    .format = target.format,
                    .extent = extent,
                    .history = target.history,
                    .samples = target.samples,
                    .mip_levels = resolveImageMipLevels(
                        target.mip_levels, extent.width, extent.height),
                    .array_layers = target.array_layers,
                    .dimension = target.dimension,
                    .storage_mode = target.storage_mode,
                    .alias_group = target.alias_group,
                },
            .alias_group = token,
        });
    }
    return registered;
}

// What get_frame_plan's total counts of them (registeredRenderTargetMemory);
// `used` says which ones a graph's plan uses.
inline std::vector<RegisteredTargetMemory> registeredTargetMemoryOf(
    std::span<const RegisteredRenderTargetStandIn> registered,
    const std::function<bool(std::string_view)> &used) {
    std::vector<RegisteredTargetMemory> memory;
    memory.reserve(registered.size());
    for (const auto &target : registered) {
        memory.push_back(registeredRenderTargetMemory(
            target.metadata, target.alias_group,
            used(target.metadata.name)));
    }
    return memory;
}

} // namespace Pelican::test_support
