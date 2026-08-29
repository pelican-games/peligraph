#pragma once

#include "authoringscenedocument.hpp"

#include <string>
#include <string_view>

namespace Pelican::internal {

// Narrow authoring-only operations for callers which must not see the raw
// view capability type (notably the production query translation unit).
std::string encodeAuthoringSceneSemantic(
    const AuthoringSceneDocument &document);
bool authoringSceneContains(
    const AuthoringSceneDocument &document, std::string_view scene_id);

} // namespace Pelican::internal
