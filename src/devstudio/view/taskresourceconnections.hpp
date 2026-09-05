#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace PelicanStudio {

// Rebind a task's resource uses together, preserving port names and shader ABI.
inline nlohmann::json rebindTaskResources(
    const nlohmann::json &task,
    const std::map<std::string, std::string> &replacements) {
    using Json = nlohmann::json;
    if (!task.is_object() || !task.contains("resource_ports") || !task.at("resource_ports").is_object())
        throw std::invalid_argument("Task resource_ports must be an object.");
    std::set<std::string> resources;
    for (const auto &[port, declaration] : task.at("resource_ports").items()) {
        if (declaration.is_string()) resources.insert(declaration.get<std::string>());
        else if (declaration.is_object()) {
            if (declaration.contains("resource") && !declaration.at("resource").is_string())
                throw std::invalid_argument("Port resource must be a string: " + port);
            resources.insert(declaration.value("resource", port));
        } else throw std::invalid_argument("Port must be a string or object: " + port);
    }
    for (const auto &[from, to] : replacements) {
        if (!resources.contains(from)) throw std::invalid_argument("Unknown source resource: " + from);
        if (from.empty() || to.empty()) throw std::invalid_argument("Resource names must not be empty.");
    }
    const auto mapped = [&](const std::string &resource) -> std::string {
        const auto found = replacements.find(resource);
        return found == replacements.end() ? resource : found->second;
    };
    auto result = task;
    for (auto &[port, declaration] : result["resource_ports"].items()) {
        const auto old = declaration.is_string() ? declaration.get<std::string>() : declaration.value("resource", port);
        const auto next = mapped(old);
        if (old == next) continue;
        if (declaration.is_string()) declaration = next;
        else declaration["resource"] = next;
    }
    for (const auto *field : {"reads", "writes"}) {
        if (!task.contains(field) || task.at(field).is_null()) continue;
        const auto &original = task.at(field);
        if (original.is_string()) result[field] = mapped(original.get<std::string>());
        else if (original.is_array()) {
            auto values = Json::array();
            for (const auto &value : original) {
                if (!value.is_string()) throw std::invalid_argument(std::string{field} + " must contain resource names.");
                values.push_back(mapped(value.get<std::string>()));
            }
            result[field] = std::move(values);
        } else throw std::invalid_argument(std::string{field} + " must be a resource name or array.");
    }
    if (task.contains("read_footprints")) {
        if (!task.at("read_footprints").is_object()) throw std::invalid_argument("read_footprints must be an object.");
        auto footprints = Json::object();
        for (const auto &[resource, footprint] : task.at("read_footprints").items()) {
            const auto next = mapped(resource);
            if (footprints.contains(next)) throw std::invalid_argument("Read footprint collision: " + next);
            footprints[next] = footprint;
        }
        result["read_footprints"] = std::move(footprints);
    }
    if (task.contains("dispatch")) {
        const auto &dispatch = task.at("dispatch");
        if (!dispatch.is_object()) throw std::invalid_argument("dispatch must be an object.");
        if (dispatch.contains("indirect")) {
            const auto &indirect = dispatch.at("indirect");
            if (!indirect.is_object() || !indirect.contains("buffer") || !indirect.at("buffer").is_string())
                throw std::invalid_argument("dispatch.indirect must contain a buffer name.");
            result["dispatch"]["indirect"]["buffer"] = mapped(indirect.at("buffer").get<std::string>());
        }
    }
    return result;
}

} // namespace PelicanStudio
