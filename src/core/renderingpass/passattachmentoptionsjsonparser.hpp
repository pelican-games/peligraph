#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

void validatePassAttachmentOptionsHaveOutputs(
    const nlohmann::json &pass_json, std::string_view pass_name);
void parsePassAttachmentOptionsFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);

} // namespace Pelican
