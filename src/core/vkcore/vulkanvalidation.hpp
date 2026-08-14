#pragma once

#include "../launchconfig.hpp"

#include <span>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view vulkanValidationLayerName =
    "VK_LAYER_KHRONOS_validation";

struct VulkanValidationSelection {
    bool available = false;
    bool enabled = false;
    bool synchronization_validation = false;
    std::string reason{"not_selected"};
};

struct VulkanValidationStatus {
    std::string layer{vulkanValidationLayerName};
    bool available = false;
    bool enabled = false;
    bool synchronization_validation = false;
    std::string reason{"not_initialized"};
};

bool vulkanValidationBuildDefaultEnabled() noexcept;

VulkanValidationSelection selectVulkanValidation(
    VulkanValidationMode mode,
    std::span<const std::string> supported_layers);

VulkanValidationStatus finalizeVulkanValidationStatus(
    const VulkanValidationSelection &selection,
    bool layer_enabled_on_created_instance,
    bool synchronization_validation_enabled_on_created_instance);

} // namespace Pelican
