#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SceneFormatDocument {
    int version = 2;
    bool uses_prefabs = false;
    nlohmann::json scenes;
    std::vector<std::string> warnings;
    // Persistent identities are document-wide, including inactive scenes.
    // Missing identities in legacy documents are allocated in declaration order.
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> authoring_object_ids;
    std::uint64_t next_authoring_object_id = 1;
    bool has_persistent_authoring_ids = false;
};

SceneFormatDocument normalizeSceneDataJson(
    const nlohmann::json &scene_data,
    std::uint64_t first_authoring_object_id = 1);

// Preserve an authored name when one exists; otherwise derive the canonical
// internal display identity used by runtime and authoring tools. Callers own
// the meaning of object_number (for example, a session object id or a
// one-based declaration number), but it must be non-zero.
std::string runtimeObjectIdentityName(std::string_view scene_id,
                                      std::uint64_t object_number,
                                      std::string_view authored_name);

} // namespace Pelican
