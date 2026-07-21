#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

struct RequiredVulkanFeatureSupport {
    bool multi_draw_indirect = false;
    bool draw_indirect_first_instance = false;
    bool shader_draw_parameters = false;
    bool dynamic_rendering = false;
};

void appendUniqueVulkanExtensions(std::vector<std::string> &destination,
                                  std::span<const char *const> extensions);
std::vector<const char *> vulkanExtensionNamePointers(
    const std::vector<std::string> &extensions);
std::optional<std::string> firstMissingVulkanExtension(
    std::span<const std::string> required, std::span<const std::string> supported);
std::optional<std::string> firstMissingRequiredVulkanFeature(
    const RequiredVulkanFeatureSupport &support);

} // namespace Pelican
