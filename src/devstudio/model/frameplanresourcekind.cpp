#include "frameplanresourcekind.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace PelicanStudio {

FramePlanResourceKind decodeFramePlanResourceKind(
    const nlohmann::json &resource, std::string_view context) {
    const auto kind = resource.find("kind");
    if (kind == resource.end()) {
        throw std::runtime_error(
            "resource_kind_missing: " + std::string{context} +
            ".kind is missing");
    }
    if (kind->is_null()) {
        throw std::runtime_error(
            "resource_kind_null: " + std::string{context} +
            ".kind is null");
    }
    if (!kind->is_string()) {
        throw std::runtime_error(
            "resource_kind_type: " + std::string{context} +
            ".kind must be a string");
    }
    const auto value = kind->get<std::string>();
    if (value.empty()) {
        throw std::runtime_error(
            "resource_kind_empty: " + std::string{context} +
            ".kind is empty");
    }
    if (value == "render_target") {
        return FramePlanResourceKind::render_target;
    }
    if (value == "frame_target") {
        return FramePlanResourceKind::frame_target;
    }
    if (value == "buffer") {
        return FramePlanResourceKind::buffer;
    }
    throw std::runtime_error(
        "resource_kind_unknown: " + std::string{context} +
        ".kind has unknown value '" + value + "'");
}

std::string_view framePlanResourceKindName(FramePlanResourceKind kind) {
    switch (kind) {
    case FramePlanResourceKind::render_target:
        return "render_target";
    case FramePlanResourceKind::frame_target:
        return "frame_target";
    case FramePlanResourceKind::buffer:
        return "buffer";
    }
    throw std::runtime_error("Unknown frame plan resource kind");
}

} // namespace PelicanStudio
