#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {
// Only the suffix the compiler actually appended may be removed. An absent
// variant record preserves the name (including an author's literal #xr).
inline std::string planningAuthorGraphName(std::string name, const nlohmann::json &variant) {
    const auto suffix = variant.is_object() ? variant.value("name_suffix", std::string{}) : std::string{};
    if (!suffix.empty() && name.ends_with(suffix)) name.resize(name.size() - suffix.size());
    return name;
}
inline nlohmann::json planningRequire(nlohmann::json value) {
    return {{"value", std::move(value)}, {"strength", "require"}};
}
// The writer shared by Studio and the planner round-trip test. Runtime and
// loop nodes remain in adjacency order, but only authorable names are written.
inline nlohmann::json freezeFramePlanPolicy(const nlohmann::json &physical,
    const std::vector<std::string> &order, const std::set<std::string> &editable,
    const std::set<std::string> &loops, const nlohmann::json &root,
    const std::map<std::string, std::string> &authored_names = {},
    const std::set<std::string> *editable_resources = nullptr) {
    using Json = nlohmann::json;
    Json result = Json::object();
    const auto candidate = physical.at("backend_selection").at("selected_candidate").get<std::string>();
    const std::map<std::string, std::string> backends{
        {"pelican.vulkan.materialized_plan@1", "materialized"},
        {"pelican.vulkan.transient_plan@1", "transient"},
        {"pelican.vulkan.tile_local_plan@1", "tile_local"}};
    result["backend"] = planningRequire(backends.at(candidate));
    std::map<std::string, std::string> scopes;
    for (const auto &scope : physical.at("scopes")) for (const auto &node : scope.at("nodes"))
        scopes[node.get<std::string>()] = scope.at("id").get<std::string>();
    for (std::size_t i = 1; i < order.size(); ++i) {
        const auto found = authored_names.find(order[i]);
        const auto name = found == authored_names.end() ? order[i] : found->second;
        if (loops.contains(order[i]) || !editable.contains(name)) continue;
        const bool same = scopes.contains(order[i]) && scopes.contains(order[i - 1]) && scopes.at(order[i]) == scopes.at(order[i - 1]);
        result["nodes"][name] = {{"scope_break", planningRequire(!same)}, {"scope_join", planningRequire(same)}};
    }
    std::set<std::string> used;
    if (const auto resources = root.find("resources"); resources != root.end())
        for (const auto &entry : *resources)
            if (entry.is_object() && entry.contains("alias_group")) {
                const auto &group = entry.at("alias_group");
                const auto value = group.is_object() ? group.value("value", Json{}) : group;
                if (value.is_string()) used.insert(value.get<std::string>());
            }
    std::size_t number = 0;
    for (const auto &group : physical.at("alias_groups")) {
        ++number;
        if (group.at("resources").size() < 2) continue;
        std::string name = "frozen_" + std::to_string(number);
        while (used.contains(name)) name = "frozen_" + std::to_string(++number);
        used.insert(name);
        for (const auto &resource : group.at("resources"))
            if (!editable_resources || editable_resources->contains(resource.get<std::string>()))
                result["resources"][resource.get<std::string>()]["alias_group"] = planningRequire(name);
    }
    for (const auto &resource : physical.at("resources"))
        if (resource.at("representation") == "materialized_image" &&
            (!editable_resources || editable_resources->contains(resource.at("logical_resource").get<std::string>())))
            result["resources"][resource.at("logical_resource").get<std::string>()]["representation"] = planningRequire("materialized");
    return result;
}
} // namespace PelicanStudio
