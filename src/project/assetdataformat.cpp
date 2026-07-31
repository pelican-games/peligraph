#include "assetdataformat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view assetDataSchema =
    "pelican.asset_data";
constexpr int assetDataVersion = 1;

void requireOnlyKeys(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> keys,
    std::string_view context) {
    for (auto entry = object.begin();
         entry != object.end(); ++entry) {
        const auto known = std::find(
            keys.begin(), keys.end(), entry.key()) !=
            keys.end();
        if (!known) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown key '" +
                entry.key() + "'");
        }
    }
}

std::string requireNonEmptyString(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view context) {
    const auto found =
        object.find(std::string{key});
    if (found == object.end() ||
        !found->is_string() ||
        found->get_ref<
            const std::string &>()
            .empty()) {
        throw std::runtime_error(
            std::string{context} +
            " requires non-empty string '" +
            std::string{key} + "'");
    }
    return found->get<std::string>();
}

} // namespace

AssetDataFormatDocument
parseAssetDataFormatJson(
    const nlohmann::json &document) {
    if (!document.is_object()) {
        throw std::runtime_error(
            "asset_data.json must be an object");
    }
    requireOnlyKeys(
        document,
        {"schema", "version", "models",
         "materials", "textures"},
        "asset_data.json");
    if (document.value(
            "schema", std::string{}) !=
        assetDataSchema) {
        throw std::runtime_error(
            "asset_data.json schema must be "
            "'pelican.asset_data'");
    }
    if (!document.contains("version") ||
        !document.at("version")
             .is_number_integer() ||
        document.at("version").get<int>() !=
            assetDataVersion) {
        throw std::runtime_error(
            "asset_data.json version must be exactly 1");
    }
    if (!document.contains("models") ||
        !document.at("models").is_array()) {
        throw std::runtime_error(
            "asset_data.json requires models array");
    }
    if (document.contains("textures") &&
        !document.at("textures").is_array()) {
        throw std::runtime_error(
            "asset_data.json textures must be an array");
    }
    if (document.contains("materials") &&
        !document.at("materials").is_array()) {
        throw std::runtime_error(
            "asset_data.json materials must be an array");
    }

    AssetDataFormatDocument result;
    const auto &models = document.at("models");
    result.models.reserve(models.size());
    std::set<std::string, std::less<>>
        model_names;
    for (std::size_t index = 0;
         index < models.size(); ++index) {
        const auto &entry = models.at(index);
        const auto context =
            "asset_data.json models[" +
            std::to_string(index) + "]";
        if (!entry.is_object()) {
            throw std::runtime_error(
                context + " must be an object");
        }
        requireOnlyKeys(
            entry,
            {"name", "path",
             "material_bindings"},
            context);
        auto name = requireNonEmptyString(
            entry, "name", context);
        if (!model_names.insert(name).second) {
            throw std::runtime_error(
                "asset_data.json has duplicate "
                "model name '" +
                name + "'");
        }
        auto path = requireNonEmptyString(
            entry, "path", context);
        std::optional<std::string>
            material_bindings;
        if (const auto found =
                entry.find("material_bindings");
            found != entry.end()) {
            if (!found->is_string() ||
                found->get_ref<
                    const std::string &>()
                    .empty()) {
                throw std::runtime_error(
                    context +
                    " material_bindings must be "
                    "a non-empty string");
            }
            material_bindings =
                found->get<std::string>();
        }
        result.models.push_back({
            .name = std::move(name),
            .path = std::move(path),
            .material_bindings =
                std::move(material_bindings),
        });
    }

    if (!document.contains("materials")) {
        return result;
    }
    const auto &materials =
        document.at("materials");
    result.materials.reserve(materials.size());
    std::set<std::string, std::less<>>
        material_paths;
    for (std::size_t index = 0;
         index < materials.size(); ++index) {
        const auto &entry =
            materials.at(index);
        const auto context =
            "asset_data.json materials[" +
            std::to_string(index) + "]";
        if (!entry.is_object()) {
            throw std::runtime_error(
                context + " must be an object");
        }
        requireOnlyKeys(
            entry, {"path"}, context);
        auto path = requireNonEmptyString(
            entry, "path", context);
        if (!material_paths.insert(path).second) {
            throw std::runtime_error(
                "asset_data.json has duplicate "
                "material document path '" +
                path + "'");
        }
        result.materials.push_back({
            .path = std::move(path),
        });
    }
    return result;
}

} // namespace Pelican
