#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace PelicanStudio {

enum class FramePlanResourceKind {
    render_target,
    frame_target,
    buffer,
};

FramePlanResourceKind decodeFramePlanResourceKind(
    const nlohmann::json &resource, std::string_view context);

std::string_view framePlanResourceKindName(FramePlanResourceKind kind);

} // namespace PelicanStudio
