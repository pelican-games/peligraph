#pragma once

#include "passshapepolicy.hpp"

#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace PelicanStudio {

using FramePlanResourceKind = Pelican::PassShapeResourceKind;

FramePlanResourceKind decodeFramePlanResourceKind(
    const nlohmann::json &resource, std::string_view context);

std::string_view framePlanResourceKindName(FramePlanResourceKind kind);

} // namespace PelicanStudio
