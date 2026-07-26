#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "../vkcore/core.hpp"
#include <map>
#include <stdexcept>

namespace Pelican {

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    std::map<std::string, std::size_t, std::less<>>
        alias_group_sizes;
    for (const auto &definition : definitions) {
        if (!definition.alias_group) continue;
        if (definition.alias_group->empty()) {
            throw std::runtime_error(
                "render target alias group must not be empty: " +
                definition.name);
        }
        ++alias_group_sizes[*definition.alias_group];
    }
    std::map<std::string, std::uint64_t, std::less<>>
        alias_group_tokens;
    for (const auto &[group, size] : alias_group_sizes) {
        if (size < 2) {
            throw std::runtime_error(
                "render target alias group requires at least two members: " +
                group);
        }
        std::optional<std::uint64_t> existing_token;
        std::size_t existing_members = 0;
        for (const auto &definition : definitions) {
            if (definition.alias_group != group) continue;
            const auto id =
                rt_container.getRenderTargetIdByName(
                    definition.name);
            if (!isConcreteRenderTarget(id)) continue;
            ++existing_members;
            const auto metadata =
                rt_container.getMetadata(id);
            const auto token =
                rt_container.aliasGroup(id);
            if (metadata.alias_group != group ||
                !token ||
                (existing_token &&
                 *existing_token != *token)) {
                throw std::runtime_error(
                    "live render target alias group contract changed during registration: " +
                    group);
            }
            existing_token = token;
        }
        if (existing_members != 0 &&
            existing_members != size) {
            throw std::runtime_error(
                "render target alias group registration cannot mix live and new generations: " +
                group);
        }
        alias_group_tokens.emplace(
            group,
            existing_token
                ? *existing_token
                : rt_container.createAliasGroupToken());
    }

    for (const auto &definition : definitions) {
        if (definition.format_class == "display") {
            const auto features = GET_MODULE(VulkanManageCore)
                                      .getPhysDevice()
                                      .getFormatProperties(definition.format)
                                      .optimalTilingFeatures;
            const auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                                  vk::FormatFeatureFlagBits::eTransferSrc;
            if ((features & required) != required) {
                throw std::runtime_error(
                    "resolver v1 display format lacks COLOR_ATTACHMENT or TRANSFER_SRC support");
            }
        }
        rt_container.registerRenderTarget(definition.name, base_extent, definition.format_class,
                                          definition.role,
                                          definition.extent_scale,
                                          definition.fixed_extent, definition.format, definition.usage,
                                          vma::MemoryUsage::eAutoPreferDevice, definition.history,
                                          definition.history_clear_color,
                                          definition.samples,
                                          definition.mip_levels,
                                          definition.array_layers,
                                          definition.storage_mode,
                                          definition.alias_group,
                                          definition.alias_group
                                              ? std::optional<std::uint64_t>{
                                                    alias_group_tokens.at(
                                                        *definition.alias_group)}
                                              : std::nullopt);
    }
}

} // namespace Pelican
