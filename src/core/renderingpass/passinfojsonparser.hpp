#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

void parsePassTypeFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseFullscreenPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseDebugDrawPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseDebugTextPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseShadowDepthPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json);

} // namespace Pelican
