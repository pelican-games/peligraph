#include "renderresolutiondomain.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

std::string_view renderResolutionDomainName(
    RenderResolutionDomain domain) {
    switch (domain) {
    case RenderResolutionDomain::unclassified:
        return "unclassified";
    case RenderResolutionDomain::scene:
        return "scene";
    case RenderResolutionDomain::output:
        return "output";
    case RenderResolutionDomain::independent:
        return "independent";
    }
    throw std::runtime_error("Unknown render resolution domain");
}

RenderResolutionDomain defaultRenderResolutionDomain(
    RenderPassType pass_type) {
    switch (pass_type) {
    case RenderPassType::material:
    case RenderPassType::velocity:
    case RenderPassType::picking:
        return RenderResolutionDomain::scene;
    case RenderPassType::output_transform:
    case RenderPassType::ui:
    case RenderPassType::gizmo:
    case RenderPassType::imgui:
        return RenderResolutionDomain::output;
    case RenderPassType::shadow_depth:
        return RenderResolutionDomain::independent;
    case RenderPassType::fullscreen:
    case RenderPassType::raster:
    case RenderPassType::debug_draw:
    case RenderPassType::debug_text:
    case RenderPassType::canonical_anchor:
    case RenderPassType::snapshot_copy:
        return RenderResolutionDomain::unclassified;
    }
    return RenderResolutionDomain::unclassified;
}

RenderResolutionDomain parseRenderResolutionDomain(
    const nlohmann::json &pass_json, RenderPassType pass_type,
    const std::string &pass_name) {
    if (!pass_json.contains("resolution_domain")) {
        return defaultRenderResolutionDomain(pass_type);
    }
    const auto &encoded = pass_json.at("resolution_domain");
    if (!encoded.is_string()) {
        throw std::runtime_error(
            "Pass resolution_domain must be a string: " + pass_name);
    }
    const auto value = encoded.get<std::string>();
    if (value == "unclassified" || value == "none") {
        return RenderResolutionDomain::unclassified;
    }
    if (value == "scene") {
        return RenderResolutionDomain::scene;
    }
    if (value == "output") {
        return RenderResolutionDomain::output;
    }
    if (value == "independent") {
        return RenderResolutionDomain::independent;
    }
    throw std::runtime_error(
        "Unknown pass resolution_domain '" + value + "': " +
        pass_name);
}

} // namespace Pelican
