#pragma once

// RC-1b: what registerRenderTargetDefinitions makes of a set of render target
// definitions, for tests that cannot create the RenderTargetContainer (it
// needs a Vulkan device): each target's metadata as getMetadata reports it
// and its alias token as aliasGroup does.

#include "../src/core/renderingpass/rendertargetdefinition.hpp"
#include "../src/core/renderingpass/rendertargetmemory.hpp"
#include "../src/core/renderingpass/rendertargetmetadata.hpp"
#include "../src/project/imagesubresource.hpp"
#include "../src/project/targetrenderplanning.hpp"

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

// `targets` as applyRenderingTargetPlan leaves them, registered at
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
