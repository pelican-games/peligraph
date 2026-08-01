#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SceneFormatDocument {
    nlohmann::json scenes;
    std::vector<std::string> warnings;
};

SceneFormatDocument normalizeSceneDataJson(const nlohmann::json &scene_data);

// Preserve an authored name when one exists; otherwise derive the canonical
// internal display identity used by runtime and authoring tools. Callers own
// the meaning of object_number (for example, a session object id or a
// one-based declaration number), but it must be non-zero.
std::string runtimeObjectIdentityName(std::string_view scene_id,
                                      std::uint64_t object_number,
                                      std::string_view authored_name);

} // namespace Pelican
