#include "prefabplacement.hpp"

#include "inspectormodel.hpp"

#include <algorithm>
#include <stdexcept>

namespace PelicanStudio {
namespace {

const nlohmann::json &requiredField(const nlohmann::json &object,
                                    std::string_view key) {
    if (!object.is_object()) throw std::runtime_error("prefab entry must be an object");
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        throw std::runtime_error("prefab entry requires " + std::string{key});
    }
    return *found;
}

std::string requiredName(const nlohmann::json &object, std::string_view key) {
    const auto &value = requiredField(object, key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
        throw std::runtime_error(std::string{key} + " must be a non-empty string");
    }
    return value.get<std::string>();
}

} // namespace

std::vector<PrefabListEntry> parsePrefabList(const nlohmann::json &reply) {
    const auto &prefabs = requiredField(reply, "prefabs");
    if (!prefabs.is_array()) throw std::runtime_error("prefabs must be an array");
    std::vector<PrefabListEntry> result;
    for (const auto &prefab : prefabs) {
        PrefabListEntry entry{.name = requiredName(prefab, "name")};
        const auto &parameters = requiredField(prefab, "parameters");
        if (!parameters.is_array()) {
            throw std::runtime_error("prefab parameters must be an array");
        }
        for (const auto &parameter : parameters) {
            const std::string name = requiredName(parameter, "name");
            const auto &required = requiredField(parameter, "required");
            if (!required.is_boolean()) {
                throw std::runtime_error("prefab parameter required must be a boolean");
            }
            if (required.get<bool>()) entry.required_parameters.push_back(name);
        }
        result.push_back(std::move(entry));
    }
    return result;
}

std::string prefabMenuLabel(const PrefabListEntry &entry) {
    if (entry.required_parameters.empty()) return entry.name;
    std::string label = entry.name + " (needs ";
    for (std::size_t index = 0; index < entry.required_parameters.size(); ++index) {
        if (index != 0) label += ", ";
        label += entry.required_parameters[index];
    }
    return label + ")";
}

std::string prefabObjectBaseName(std::string_view prefab) {
    return modelObjectBaseName(prefab).substr(0, 48);
}

std::string uniquePrefabObjectName(std::string_view base,
                                   const std::vector<std::string> &taken) {
    const auto used = [&taken](const std::string &name) {
        return std::find(taken.begin(), taken.end(), name) != taken.end();
    };
    for (unsigned suffix = 1;; ++suffix) {
        std::string name = std::string{base} + "_" + std::to_string(suffix);
        if (!used(name)) return name;
    }
}

std::string uniquePrefabInstanceId(std::string_view object_name,
                                   const std::vector<std::string> &taken) {
    const auto used = [&taken](const std::string &name) {
        return std::find(taken.begin(), taken.end(), name) != taken.end();
    };
    const std::string candidate = "gi_" + std::string{object_name};
    if (!used(candidate)) return candidate;
    for (unsigned suffix = 2;; ++suffix) {
        const std::string name = candidate + "_" + std::to_string(suffix);
        if (!used(name)) return name;
    }
}

nlohmann::json prefabObjectDeclaration(const std::string &name,
                                       const std::string &prefab,
                                       std::string instance_id) {
    auto declaration = addObjectDeclaration(AddObjectKind::Empty, name);
    if (instance_id.empty()) instance_id = "gi_" + name;
    declaration["prefab"] = {{"ref", prefab}, {"instance_id", std::move(instance_id)}};
    return declaration;
}

} // namespace PelicanStudio
