#pragma once

#include <cstdint>
#include <string_view>

namespace Pelican {

enum class ResourceExtentKind : std::uint8_t {
    output_relative,
    fixed,
};

std::string_view resourceExtentKindName(ResourceExtentKind kind);

// Typed physical-size contract carried beside a resource pattern. It is
// intentionally binding-specific: two resources using the same format and
// materialization pattern may live at different resolutions.
struct ResourceExtentPlan {
    ResourceExtentKind kind =
        ResourceExtentKind::output_relative;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const ResourceExtentPlan &) const = default;
};

// RC-3a1: also the engine's 2D extent (width and height in texels) where
// Vulkan has its Extent2D: a render target's fixed size, a registered
// image's size. Core maps it (vulkanimagevocabulary).
struct ResolvedResourceExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const ResolvedResourceExtent &) const = default;
};

// This is the producer's sole float-to-integer extent rule. Runtime image
// allocation and compiled-plan validation both call this resolver; wire
// consumers receive the resulting integers and do not repeat the arithmetic.
ResolvedResourceExtent resolveResourceExtent(
    const ResourceExtentPlan &plan,
    ResolvedResourceExtent output_extent);

} // namespace Pelican
