#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

struct PrefabListEntry {
    std::string name;
    std::vector<std::string> required_parameters;
};

[[nodiscard]] std::vector<PrefabListEntry> parsePrefabList(
    const nlohmann::json &reply);
[[nodiscard]] std::string prefabMenuLabel(const PrefabListEntry &entry);
[[nodiscard]] std::string prefabObjectBaseName(std::string_view prefab);
[[nodiscard]] std::string uniquePrefabObjectName(
    std::string_view base, const std::vector<std::string> &taken);
[[nodiscard]] std::string uniquePrefabInstanceId(
    std::string_view object_name, const std::vector<std::string> &taken);
[[nodiscard]] nlohmann::json prefabObjectDeclaration(
    const std::string &name, const std::string &prefab,
    std::string instance_id = {});

} // namespace PelicanStudio
