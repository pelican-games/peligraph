#pragma once

#include "passfieldownership.hpp"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace Pelican {

// A pass labels the resolution space in which its raster work is defined.
// The compiler uses the scene domain to derive the camera/jitter render
// extent. Output and independent work (for example UI and shadow maps) do not
// participate in that inference.
enum class RenderResolutionDomain : std::uint8_t {
    unclassified,
    scene,
    output,
    independent,
};

std::string_view renderResolutionDomainName(
    RenderResolutionDomain domain);

// RC-3a2: the one place a pass type's domain is decided when the pass does
// not state one (Studio's schema shows the same value as the fallback).
RenderResolutionDomain defaultRenderResolutionDomain(
    RenderPassType pass_type);

// A pass's `resolution_domain`: "scene", "output", "independent",
// "unclassified" or its synonym "none"; absent is the type's default.
RenderResolutionDomain parseRenderResolutionDomain(
    const nlohmann::json &pass_json, RenderPassType pass_type,
    const std::string &pass_name);

} // namespace Pelican
