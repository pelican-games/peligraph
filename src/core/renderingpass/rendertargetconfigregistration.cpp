#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "../vkcore/core.hpp"
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace Pelican {

vk::ClearColorValue physicalRenderTargetHistoryClearColor(
    const RenderTargetDefinition &definition) {
    const auto format_name =
        vk::to_string(definition.format);
    const auto signed_integer =
        format_name.ends_with("Sint");
    const auto unsigned_integer =
        format_name.ends_with("Uint");
    std::array<float, 4> floating{};
    std::array<std::int32_t, 4> signed_values{};
    std::array<std::uint32_t, 4> unsigned_values{};
    for (std::size_t component = 0;
         component <
         definition.history_clear_color.size();
         ++component) {
        const auto value =
            definition.history_clear_color[component];
        if (!std::isfinite(value)) {
            throw std::runtime_error(
                "render target history clear value must be finite: " +
                definition.name);
        }
        if (signed_integer) {
            if (std::trunc(value) != value ||
                value <
                    static_cast<double>(
                        std::numeric_limits<
                            std::int32_t>::min()) ||
                value >
                    static_cast<double>(
                        std::numeric_limits<
                            std::int32_t>::max())) {
                throw std::runtime_error(
                    "signed integer render target history clear "
                    "value is fractional or out of range: " +
                    definition.name);
            }
            signed_values[component] =
                static_cast<std::int32_t>(value);
            continue;
        }
        if (unsigned_integer) {
            if (std::trunc(value) != value ||
                value < 0.0 ||
                value >
                    static_cast<double>(
                        std::numeric_limits<
                            std::uint32_t>::max())) {
                throw std::runtime_error(
                    "unsigned integer render target history clear "
                    "value is fractional or out of range: " +
                    definition.name);
            }
            unsigned_values[component] =
                static_cast<std::uint32_t>(value);
            continue;
        }
        if (value <
                -static_cast<double>(
                    std::numeric_limits<float>::max()) ||
            value >
                static_cast<double>(
                    std::numeric_limits<float>::max())) {
            throw std::runtime_error(
                "floating render target history clear value is "
                "out of range: " +
                definition.name);
        }
        floating[component] =
            static_cast<float>(value);
    }
    if (signed_integer) {
        return vk::ClearColorValue{
            signed_values};
    }
    if (unsigned_integer) {
        return vk::ClearColorValue{
            unsigned_values};
    }
    return vk::ClearColorValue{floating};
}

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
                                          physicalRenderTargetHistoryClearColor(
                                              definition),
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
