#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

void parsePassAttachmentOptionsFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);

} // namespace Pelican
