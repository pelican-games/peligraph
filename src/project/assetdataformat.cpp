#include "assetdataformat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view assetDataSchema =
    "pelican.asset_data";
constexpr int assetDataCurrentVersion = 2;
constexpr float proceduralAabbMinimumThickness = 0x1p-7F;

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

std::array<float, 3> requireF32Vec3(
    const nlohmann::json &value, std::string_view context) {
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(std::string{context} +
                                 " must be a three-element array");
    }
    std::array<float, 3> result{};
    for (std::size_t axis = 0; axis < result.size(); ++axis) {
        if (!value.at(axis).is_number()) {
            throw std::runtime_error(std::string{context} +
                                     " entries must be numbers");
        }
        const auto authored = value.at(axis).get<double>();
        result[axis] = static_cast<float>(authored);
        if (!std::isfinite(authored) || !std::isfinite(result[axis])) {
            throw std::runtime_error(std::string{context} +
                                     " entries must be finite f32 values");
        }
    }
    return result;
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
         "materials", "textures", "procedural_geometry"},
        "asset_data.json");
    if (document.value(
            "schema", std::string{}) !=
        assetDataSchema) {
        throw std::runtime_error(
            "asset_data.json schema must be "
            "'pelican.asset_data'");
    }
    if (!document.contains("version") ||
        !document.at("version").is_number_integer()) {
        throw std::runtime_error(
            "asset_data.json requires an integer version");
    }
    // MG-2b: one version. A version 1 document (the same document without
    // procedural_geometry) is not read; a project that still has one rewrites
    // its "version" to 2 -- nothing else changes.
    if (document.at("version") != assetDataCurrentVersion) {
        throw std::runtime_error(
            "pelican.asset_data.version_unsupported@1: asset_data version " +
            document.at("version").dump() +
            " is not supported; this engine reads version " +
            std::to_string(assetDataCurrentVersion) + " only");
    }
    const auto version = document.at("version").get<int>();
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
    if (document.contains("procedural_geometry") &&
        !document.at("procedural_geometry").is_array()) {
        throw std::runtime_error(
            "asset_data.json procedural_geometry must be an array");
    }

    AssetDataFormatDocument result;
    result.version = static_cast<std::uint32_t>(version);
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

    if (document.contains("materials")) {
        const auto &materials = document.at("materials");
        result.materials.reserve(materials.size());
        std::set<std::string, std::less<>> material_paths;
        for (std::size_t index = 0; index < materials.size(); ++index) {
            const auto &entry = materials.at(index);
            const auto context = "asset_data.json materials[" +
                                 std::to_string(index) + "]";
            if (!entry.is_object()) {
                throw std::runtime_error(context + " must be an object");
            }
            requireOnlyKeys(entry, {"path"}, context);
            auto path = requireNonEmptyString(entry, "path", context);
            if (!material_paths.insert(path).second) {
                throw std::runtime_error(
                    "asset_data.json has duplicate material document path '" +
                    path + "'");
            }
            result.materials.push_back({.path = std::move(path)});
        }
    }

    if (!document.contains("procedural_geometry")) return result;
    const auto &procedural = document.at("procedural_geometry");
    result.procedural_geometry.reserve(procedural.size());
    std::set<std::string, std::less<>> procedural_names;
    for (std::size_t index = 0; index < procedural.size(); ++index) {
        const auto &entry = procedural.at(index);
        const auto context = "asset_data.json procedural_geometry[" +
                             std::to_string(index) + "]";
        if (!entry.is_object()) {
            throw std::runtime_error(context + " must be an object");
        }
        if (entry.contains("entries")) {
            throw std::runtime_error(
                "pelican.procedural_geometry_entries_unsupported@1: " +
                context + " uses the RT3-2b entries form");
        }
        requireOnlyKeys(entry, {"name", "kind", "aabb", "record"},
                        context);
        auto name = requireNonEmptyString(entry, "name", context);
        if (!procedural_names.emplace(name).second) {
            throw std::runtime_error(
                "asset_data.json has duplicate procedural geometry name '" +
                name + "'");
        }
        auto kind = requireNonEmptyString(entry, "kind", context);
        if (kind != "aabb_v1") {
            throw std::runtime_error(context + " kind must be 'aabb_v1'");
        }
        if (!entry.contains("aabb") || !entry.at("aabb").is_object()) {
            throw std::runtime_error(context + " requires object 'aabb'");
        }
        const auto &aabb = entry.at("aabb");
        requireOnlyKeys(aabb, {"min", "max"}, context + " aabb");
        if (!aabb.contains("min") || !aabb.contains("max")) {
            throw std::runtime_error(context + " aabb requires min and max");
        }
        const auto minimum = requireF32Vec3(aabb.at("min"),
                                            context + " aabb.min");
        const auto maximum = requireF32Vec3(aabb.at("max"),
                                            context + " aabb.max");
        for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
            if (!(minimum[axis] < maximum[axis]) ||
                maximum[axis] - minimum[axis] <
                    proceduralAabbMinimumThickness) {
                throw std::runtime_error(
                    "pelican.procedural_aabb_degenerate@1: asset='" + name +
                    "' primitive_id=0");
            }
        }
        if (!entry.contains("record") || !entry.at("record").is_object()) {
            throw std::runtime_error(context +
                                     " record must be a named object");
        }
        result.procedural_geometry.push_back({
            .name = std::move(name),
            .kind = std::move(kind),
            .aabb_min = minimum,
            .aabb_max = maximum,
            .record = entry.at("record"),
        });
    }
    return result;
}

} // namespace Pelican
