#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SceneFormatDocument {
    nlohmann::json scenes;
    std::vector<std::string> warnings;
};

SceneFormatDocument normalizeSceneDataJson(const nlohmann::json &scene_data);

} // namespace Pelican
