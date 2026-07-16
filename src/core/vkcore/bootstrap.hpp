#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

void appendUniqueVulkanExtensions(std::vector<std::string> &destination,
                                  std::span<const char *const> extensions);
std::vector<const char *> vulkanExtensionNamePointers(
    const std::vector<std::string> &extensions);
std::optional<std::string> firstMissingVulkanExtension(
    std::span<const std::string> required, std::span<const std::string> supported);

} // namespace Pelican
