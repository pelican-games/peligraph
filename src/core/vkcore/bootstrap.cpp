#include "bootstrap.hpp"

#include <algorithm>

namespace Pelican {

void appendUniqueVulkanExtensions(std::vector<std::string> &destination,
                                  std::span<const char *const> extensions) {
    for (const auto *extension : extensions) {
        if (extension == nullptr || *extension == '\0') continue;
        if (std::find(destination.begin(), destination.end(), extension) == destination.end()) {
            destination.emplace_back(extension);
        }
    }
}

std::vector<const char *> vulkanExtensionNamePointers(
    const std::vector<std::string> &extensions) {
    std::vector<const char *> result;
    result.reserve(extensions.size());
    for (const auto &extension : extensions) result.push_back(extension.c_str());
    return result;
}

std::optional<std::string> firstMissingVulkanExtension(
    std::span<const std::string> required, std::span<const std::string> supported) {
    for (const auto &extension : required) {
        if (std::find(supported.begin(), supported.end(), extension) == supported.end()) {
            return extension;
        }
    }
    return std::nullopt;
}

std::optional<std::string> firstMissingRequiredVulkanFeature(
    const RequiredVulkanFeatureSupport &support) {
    if (!support.multi_draw_indirect) return "multiDrawIndirect";
    if (!support.draw_indirect_first_instance) return "drawIndirectFirstInstance";
    if (!support.shader_draw_parameters) return "shaderDrawParameters";
    if (!support.dynamic_rendering) return "dynamicRendering";
    return std::nullopt;
}

} // namespace Pelican
