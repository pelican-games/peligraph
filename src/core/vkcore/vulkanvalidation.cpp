#include "vulkanvalidation.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {

bool vulkanValidationBuildDefaultEnabled() noexcept {
#ifdef _DEBUG
    return true;
#else
    return false;
#endif
}

VulkanValidationSelection selectVulkanValidation(
    VulkanValidationMode mode,
    std::span<const std::string> supported_layers) {
    const bool available =
        std::find(supported_layers.begin(), supported_layers.end(),
                  vulkanValidationLayerName) != supported_layers.end();

    bool enabled = false;
    std::string reason;
    std::string request_source;
    switch (mode) {
    case VulkanValidationMode::build_default:
        enabled = vulkanValidationBuildDefaultEnabled();
        reason = enabled ? "enabled_by_debug_build_default"
                         : "disabled_by_non_debug_build_default";
        request_source = "the _DEBUG build default";
        break;
    case VulkanValidationMode::disabled:
        reason = "disabled_by_launch_option";
        break;
    case VulkanValidationMode::enabled:
        enabled = true;
        reason = "enabled_by_launch_option";
        request_source = "--vulkan-validation=on";
        break;
    }

    if (enabled && !available) {
        throw std::runtime_error(
            "pelican.vulkan.validation_layer_unavailable@1: " +
            request_source + " requires " +
            std::string{vulkanValidationLayerName} +
            ", but the Vulkan loader did not enumerate that layer");
    }

    // Synchronization validation remains coupled to the validation layer. It
    // is the most valuable (and most expensive) part of the existing _DEBUG
    // behavior, so selecting validation without it would be a misleading
    // partial mode and would not preserve today's default.
    return {
        .available = available,
        .enabled = enabled,
        .synchronization_validation = enabled,
        .reason = std::move(reason),
    };
}

VulkanValidationStatus finalizeVulkanValidationStatus(
    const VulkanValidationSelection &selection,
    bool layer_enabled_on_created_instance,
    bool synchronization_validation_enabled_on_created_instance) {
    if (layer_enabled_on_created_instance != selection.enabled ||
        synchronization_validation_enabled_on_created_instance !=
            selection.synchronization_validation) {
        throw std::logic_error(
            "pelican.vulkan.validation_instance_state_mismatch@1: "
            "created Vulkan instance validation state differs from the "
            "resolved selection");
    }
    return {
        .layer = std::string{vulkanValidationLayerName},
        .available = selection.available,
        .enabled = layer_enabled_on_created_instance,
        .synchronization_validation =
            synchronization_validation_enabled_on_created_instance,
        .reason = selection.reason,
    };
}

} // namespace Pelican
