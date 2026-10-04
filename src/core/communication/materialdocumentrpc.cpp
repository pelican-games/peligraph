#include "materialdocumentrpc.hpp"

#include "rpcserver.hpp"

#include "../asset/model.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/resolvedscene.hpp"
#include "../log.hpp"
#include "../material/material.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/projectmaterialasset.hpp"
#include "../model/engineprimitives.hpp"
#include "../vkcore/core.hpp"
#include "../watch/assetkey.hpp"
#include "../watch/reloadservice.hpp"

#include "../../project/assetdataformat.hpp"
#include "../../project/materialdrawloop.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/materiallowering.hpp"
#include "../../project/projectdocumenttransaction.hpp"
#include "../../project/surfaceformat.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view materialSchema = "pelican.material";
constexpr std::string_view bindingsSchema = "pelican.material_bindings";

constexpr std::string_view externalModificationCode = "external_modification";
constexpr std::string_view valueInvalidCode = "material.value_invalid@1";
constexpr std::string_view paramUnknownCode = "material.param_unknown@1";
constexpr std::string_view documentUnknownCode = "material.document_unknown@1";
constexpr std::string_view documentInvalidCode = "material.document_invalid@1";
constexpr std::string_view nameUnknownCode = "material.name_unknown@1";
constexpr std::string_view nameInvalidCode = "material.name_invalid@1";
constexpr std::string_view nameConflictCode = "material.name_conflict@1";
constexpr std::string_view pathInvalidCode = "material.path_invalid@1";
constexpr std::string_view pathConflictCode = "material.path_conflict@1";
constexpr std::string_view surfaceUnknownCode = "material.surface_unknown@1";
constexpr std::string_view modelUnknownCode = "material.model_unknown@1";
constexpr std::string_view bindingInvalidCode = "material.binding_invalid@1";
constexpr std::string_view transactionBusyCode = "material.transaction_busy@1";
constexpr std::string_view routingConflictCode = "material.routing_conflict@1";

[[noreturn]] void refuse(std::string_view method, const std::string &message, std::string_view code,
                         Json fields) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, std::string{method} + ": " + message,
                              Json{{"error_code", std::string{code}}, {"error_fields", std::move(fields)}});
}

[[noreturn]] void invalidParams(std::string_view method, const std::string &message) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, std::string{method} + " " + message);
}

void requireOnly(std::string_view method, const Json &params, std::initializer_list<std::string_view> keys) {
    if (!params.is_object()) invalidParams(method, "params must be an object");
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            invalidParams(method, "does not take '" + key + "'");
    }
}

std::string requireString(std::string_view method, const Json &params, std::string_view key) {
    const auto found = params.find(std::string{key});
    if (found == params.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
        invalidParams(method, "requires a non-empty string '" + std::string{key} + "'");
    return found->get<std::string>();
}

std::optional<std::string> optionalString(std::string_view method, const Json &params, std::string_view key) {
    const auto found = params.find(std::string{key});
    if (found == params.end()) return std::nullopt;
    if (!found->is_string() || found->get_ref<const std::string &>().empty())
        invalidParams(method, "'" + std::string{key} + "' must be a non-empty string");
    return found->get<std::string>();
}

std::string readFileBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// A document as the engine writes it: the keys in the order they had (a new
// key last), two spaces, and the file's own line ending and final newline. A
// new file is LF with a final newline.
std::string renderJsonDocument(const OrderedJson &document, std::string_view previous) {
    auto text = document.dump(2);
    if (previous.empty() || previous.ends_with('\n')) text += '\n';
    if (previous.find("\r\n") == std::string_view::npos) return text;
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (const char ch : text) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    return crlf;
}

Json toJson(const OrderedJson &value) { return Json::parse(value.dump()); }
OrderedJson toOrdered(const Json &value) { return OrderedJson::parse(value.dump()); }

bool isIdentifier(std::string_view value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    });
}

// --- asset_data and the material documents it lists.

struct AssetDataFile {
    std::string reference;
    std::filesystem::path path;
    std::string bytes;
    std::string digest;
    OrderedJson json;
};

AssetDataFile readAssetData(PathResolver &paths) {
    auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    if (config == nullptr) throw std::runtime_error("no project is open");
    AssetDataFile file;
    file.reference = config->assetDataReference();
    file.path = paths.resolveExistingFile(file.reference);
    file.bytes = readFileBytes(file.path);
    file.digest = projectDocumentDigest(file.bytes);
    file.json = OrderedJson::parse(file.bytes);
    return file;
}

std::vector<std::string> listedMaterialPaths(const OrderedJson &asset_data) {
    std::vector<std::string> paths;
    const auto materials = asset_data.find("materials");
    if (materials == asset_data.end() || !materials->is_array()) return paths;
    for (const auto &entry : *materials)
        if (entry.is_object() && entry.contains("path") && entry.at("path").is_string())
            paths.push_back(entry.at("path").get<std::string>());
    return paths;
}

std::optional<std::string> normalizedOrNull(const PathResolver &paths, std::string_view reference) {
    try {
        return paths.normalizedReference(reference);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// The asset_data materials[] path `path` names, as asset_data writes it.
std::optional<std::string> findListedMaterial(const AssetDataFile &asset_data, const PathResolver &paths,
                                              std::string_view path) {
    const auto listed = listedMaterialPaths(asset_data.json);
    for (const auto &entry : listed)
        if (entry == path) return entry;
    const auto normalized = normalizedOrNull(paths, path);
    if (!normalized) return std::nullopt;
    for (const auto &entry : listed)
        if (normalizedOrNull(paths, entry) == normalized) return entry;
    return std::nullopt;
}

struct MaterialDocumentFile {
    std::string reference;
    std::filesystem::path path;
    std::string bytes;
    std::string digest;
    OrderedJson json;
};

MaterialDocumentFile readMaterialDocument(std::string_view method, const PathResolver &paths,
                                          const std::string &reference) {
    MaterialDocumentFile document;
    document.reference = reference;
    try {
        document.path = paths.resolveExistingFile(reference);
        document.bytes = readFileBytes(document.path);
    } catch (const std::exception &error) {
        refuse(method, "material document '" + reference + "' cannot be read: " + error.what(),
               documentUnknownCode, {{"path", reference}, {"reason", error.what()}});
    }
    document.digest = projectDocumentDigest(document.bytes);
    try {
        document.json = OrderedJson::parse(document.bytes);
    } catch (const std::exception &error) {
        refuse(method, "material document '" + reference + "' is not JSON: " + error.what(), documentInvalidCode,
               {{"path", reference}, {"reason", error.what()}});
    }
    if (!document.json.is_object() || document.json.value("schema", std::string{}) != materialSchema ||
        !document.json.contains("materials") || !document.json.at("materials").is_array()) {
        refuse(method, "'" + reference + "' is not a pelican.material document", documentInvalidCode,
               {{"path", reference}, {"reason", "not a pelican.material document with a materials array"}});
    }
    return document;
}

std::size_t locateMaterial(std::string_view method, const MaterialDocumentFile &document,
                           const std::optional<std::string> &name) {
    const auto &materials = document.json.at("materials");
    if (!name) {
        if (materials.size() == 1) return 0;
        refuse(method,
               "'" + document.reference + "' holds " + std::to_string(materials.size()) +
                   " materials; name the one meant",
               nameUnknownCode,
               {{"name", nullptr},
                {"path", document.reference},
                {"reason", "the document holds " + std::to_string(materials.size()) + " materials"}});
    }
    for (std::size_t index = 0; index < materials.size(); ++index)
        if (materials.at(index).is_object() && materials.at(index).value("name", std::string{}) == *name)
            return index;
    refuse(method, "'" + document.reference + "' has no material '" + *name + "'", nameUnknownCode,
           {{"name", *name}, {"path", document.reference}, {"reason", "not in the document"}});
}

std::vector<std::string> materialNames(const OrderedJson &document) {
    std::vector<std::string> names;
    const auto materials = document.find("materials");
    if (materials == document.end() || !materials->is_array()) return names;
    for (const auto &material : *materials)
        if (material.is_object() && material.contains("name") && material.at("name").is_string())
            names.push_back(material.at("name").get<std::string>());
    return names;
}

bool registeredFrom(std::string_view name, std::string_view reference) {
    const auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>();
    if (project == nullptr) return false;
    const auto document = project->documentOf(name);
    return document && *document == reference;
}

// --- surfaces.

class SurfaceCache {
    const PathResolver &paths_;
    std::map<std::string, SurfaceFormatDocument> loaded_;

  public:
    explicit SurfaceCache(const PathResolver &paths) : paths_{paths} {}

    // Throws std::runtime_error when the surface does not load or parse.
    const SurfaceFormatDocument &load(const std::string &reference) {
        if (const auto found = loaded_.find(reference); found != loaded_.end()) return found->second;
        if (!reference.ends_with(".surface")) throw std::runtime_error("a surface reference ends in .surface");
        auto document = parseSurfaceFormat(paths_.loadText(reference), reference);
        return loaded_.emplace(reference, std::move(document)).first->second;
    }

    // Every surface a material document's materials and variants name.
    MaterialSurfaceCatalog catalogFor(const OrderedJson &document) {
        MaterialSurfaceCatalog catalog;
        const auto take = [&](const OrderedJson &object) {
            if (!object.is_object()) return;
            const auto surface = object.find("surface");
            if (surface == object.end() || !surface->is_string()) return;
            const auto reference = surface->get<std::string>();
            if (catalog.contains(reference)) return;
            try {
                catalog.emplace(reference, load(reference));
            } catch (const std::exception &) {
                // Left out: the parse names the surface it could not validate.
            }
        };
        for (const auto &material : document.at("materials")) {
            take(material);
            if (material.is_object() && material.contains("variants") && material.at("variants").is_object())
                for (const auto &[name, variant] : material.at("variants").items()) {
                    (void)name;
                    take(variant);
                }
        }
        return catalog;
    }
};

const SurfaceFormatDocument &requireSurface(std::string_view method, SurfaceCache &surfaces,
                                            const std::string &reference) {
    try {
        return surfaces.load(reference);
    } catch (const std::exception &error) {
        refuse(method, "surface '" + reference + "' cannot be read: " + error.what(), surfaceUnknownCode,
               {{"surface", reference}, {"reason", error.what()}});
    }
}

Json paramValueJson(const SurfaceParamValue &value) {
    switch (value.type) {
    case SurfaceParamType::integer:
        return value.integer_value;
    case SurfaceParamType::floating:
        return value.values[0];
    default: {
        auto components = Json::array();
        for (std::size_t index = 0; index < value.component_count; ++index)
            components.push_back(value.values[index]);
        return components;
    }
    }
}

Json surfaceSchemaJson(const std::string &reference, const SurfaceFormatDocument &surface) {
    auto params = Json::array();
    for (const auto &param : surface.params) {
        Json entry{{"name", param.name},
                   {"type", std::string{surfaceParamTypeName(param.type)}},
                   {"default", paramValueJson(param.default_value)},
                   {"encoding", param.encoding}};
        if (param.min) entry["min"] = *param.min;
        if (param.max) entry["max"] = *param.max;
        if (param.hint) entry["hint"] = *param.hint;
        params.push_back(std::move(entry));
    }
    auto textures = Json::array();
    for (const auto &texture : surface.textures) {
        textures.push_back({{"name", texture.name},
                            {"default", texture.default_reference},
                            {"color_space", texture.color_space},
                            {"role", texture.role == SurfaceTextureRole::color ? "color" : "data"},
                            {"dimension", std::string{surfaceTextureDimensionName(texture.dimension)}}});
    }
    return {{"surface", reference}, {"params", std::move(params)}, {"textures", std::move(textures)}};
}

const SurfaceParamDefinition *findParam(const SurfaceFormatDocument &surface, std::string_view name) {
    const auto found = std::find_if(surface.params.begin(), surface.params.end(),
                                    [name](const auto &param) { return param.name == name; });
    return found == surface.params.end() ? nullptr : &*found;
}

const SurfaceTextureDefinition *findTexture(const SurfaceFormatDocument &surface, std::string_view name) {
    const auto found = std::find_if(surface.textures.begin(), surface.textures.end(),
                                    [name](const auto &texture) { return texture.name == name; });
    return found == surface.textures.end() ? nullptr : &*found;
}

// Why `value` is not one the declaration takes: its type, then every
// component against the declared min / max. nullopt: it is.
std::optional<std::string> valueProblem(const Json &value, const SurfaceParamDefinition &param) {
    const std::string type{surfaceParamTypeName(param.type)};
    std::vector<double> components;
    const auto numbers = [&](std::size_t low, std::size_t high) -> bool {
        if (!value.is_array() || value.size() < low || value.size() > high) return false;
        for (const auto &component : value) {
            if (!component.is_number()) return false;
            components.push_back(component.get<double>());
        }
        return true;
    };
    switch (param.type) {
    case SurfaceParamType::floating:
        if (!value.is_number()) return "must be a number (" + type + ")";
        components.push_back(value.get<double>());
        break;
    case SurfaceParamType::integer:
        if (!value.is_number_integer()) return "must be an integer (" + type + ")";
        components.push_back(static_cast<double>(value.get<std::int64_t>()));
        break;
    case SurfaceParamType::vec2:
        if (!numbers(2, 2)) return "must be an array of 2 numbers (" + type + ")";
        break;
    case SurfaceParamType::vec3:
        if (!numbers(3, 3)) return "must be an array of 3 numbers (" + type + ")";
        break;
    case SurfaceParamType::vec4:
        if (!numbers(4, 4)) return "must be an array of 4 numbers (" + type + ")";
        break;
    case SurfaceParamType::color:
        if (!numbers(3, 4)) return "must be an array of 3 or 4 numbers (" + type + ")";
        break;
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto where = components.size() == 1 ? std::string{"value"} : "component " + std::to_string(index);
        if (param.min && components[index] < *param.min)
            return where + " " + Json(components[index]).dump() + " is below the declared min " +
                   Json(*param.min).dump();
        if (param.max && components[index] > *param.max)
            return where + " " + Json(components[index]).dump() + " is above the declared max " +
                   Json(*param.max).dump();
    }
    return std::nullopt;
}

std::optional<std::string> textureProblem(const Json &value, const PathResolver &paths) {
    if (!value.is_string()) return "must be a project:// or engine:// reference string";
    const auto reference = value.get<std::string>();
    if (reference.find('\\') != std::string::npos) return "must use forward slashes";
    if (reference.starts_with("engine://")) return std::nullopt;
    if (!reference.starts_with("project://")) return "must be a project:// or engine:// reference";
    try {
        (void)paths.resolveExistingFile(reference);
    } catch (const std::exception &error) {
        return std::string{"does not resolve to a file: "} + error.what();
    }
    return std::nullopt;
}

// values / textures of a create or an edit, merged into `target` (an object
// key of the material). `previous` receives each key's earlier value (null:
// absent). Unknown names and values the surface does not take are refused.
void mergeSurfaceInputs(std::string_view method, const std::string &path, const Json &given,
                        std::string_view key, const SurfaceFormatDocument &surface, const PathResolver &paths,
                        OrderedJson &material, OrderedJson *previous) {
    if (!given.is_object()) invalidParams(method, "'" + std::string{key} + "' must be an object");
    const std::string field{key};
    const bool textures = field == "textures";
    auto target = material.contains(field) && material.at(field).is_object() ? material.at(field)
                                                                              : OrderedJson::object();
    auto before = OrderedJson::object();
    for (const auto &[name, value] : given.items()) {
        const bool declared = textures ? findTexture(surface, name) != nullptr : findParam(surface, name) != nullptr;
        if (!declared) {
            refuse(method, "the surface declares no " + std::string{textures ? "texture" : "param"} + " '" + name + "'",
                   paramUnknownCode, {{"path", path}, {"param", name}, {"kind", textures ? "texture" : "value"}});
        }
        before[name] = target.contains(name) ? target.at(name) : OrderedJson(nullptr);
        if (value.is_null()) {
            target.erase(name);
            continue;
        }
        const auto problem = textures ? textureProblem(value, paths) : valueProblem(value, *findParam(surface, name));
        if (problem) {
            refuse(method, std::string{textures ? "texture" : "value"} + " '" + name + "' " + *problem,
                   valueInvalidCode, {{"path", path}, {"param", name}, {"reason", *problem}});
        }
        target[name] = toOrdered(value);
    }
    if (target.empty()) material.erase(field);
    else material[field] = std::move(target);
    if (previous) (*previous)[field] = std::move(before);
}

// The candidate document as the engine reads it at start: parsed with its
// surfaces, and every material lowered (MaterialFormat + lowering).
void checkCandidateDocument(std::string_view method, const std::string &path, const OrderedJson &document,
                            SurfaceCache &surfaces) {
    try {
        auto catalog = surfaces.catalogFor(document);
        const auto parsed = parseMaterialFormatJson(toJson(document), catalog);
        for (const auto &definition : parsed.materials) {
            if (!definition.surface) throw std::runtime_error("material '" + definition.name + "' requires a surface");
            const auto surface = catalog.find(*definition.surface);
            if (surface == catalog.end()) (void)surfaces.load(*definition.surface);
            (void)lowerMaterial(definition, catalog.at(*definition.surface), projectMaterialLoweringCapabilities());
            (void)lowerMaterialVariants(definition, catalog, projectMaterialLoweringCapabilities());
        }
    } catch (const JsonRpcHandlerError &) {
        throw;
    } catch (const std::exception &error) {
        refuse(method, "'" + path + "' would not load: " + error.what(), documentInvalidCode,
               {{"path", path}, {"reason", error.what()}});
    }
}

void commitDocuments(std::string_view method, const std::filesystem::path &project_root,
                     std::span<const ProjectCandidateDocument> documents, const std::string &path) {
    try {
        (void)commitProjectDocuments(project_root, documents);
    } catch (const ProjectDocumentExternalModification &error) {
        refuse(method, error.what(), externalModificationCode, {{"path", path}, {"expected", nullptr}, {"actual", nullptr}});
    } catch (const ProjectDocumentTransactionBusy &error) {
        refuse(method, error.what(), transactionBusyCode, {{"path", error.path().generic_string()}});
    }
}

ProjectCandidateDocument replacement(const std::string &reference, const std::filesystem::path &path,
                                     const std::string &digest, std::string bytes) {
    return ProjectCandidateDocument{
        .reference = reference,
        .normalized_reference = reference,
        .path = path,
        .operation = ProjectDocumentOperation::replace,
        .expected = {ProjectDocumentExistence::present, digest},
        .bytes = std::move(bytes),
    };
}

ProjectCandidateDocument creation(const std::string &reference, const std::filesystem::path &path,
                                  std::string bytes) {
    return ProjectCandidateDocument{
        .reference = reference,
        .normalized_reference = reference,
        .path = path,
        .operation = ProjectDocumentOperation::create,
        .expected = {ProjectDocumentExistence::missing, {}},
        .bytes = std::move(bytes),
    };
}

// asset_data.json is the engine's cached copy too (list_assets, the model
// and material containers read it); a write publishes the new bytes to it.
// The answer is why the copy was not refreshed (nullopt: it was).
std::optional<std::string> publishAssetData(PathResolver &paths, std::string_view bytes) {
    auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    if (config == nullptr) return "no project is open";
    try {
        config->publishAssetDataJson(config->prepareAssetDataJson(bytes, paths));
    } catch (const std::exception &error) {
        LOG_WARNING(logger, "asset_data.json written; the engine's copy was not refreshed: {}", error.what());
        return error.what();
    }
    return std::nullopt;
}

Json appliedLive(Json answer) {
    answer["applied"] = "live";
    answer["restart_required"] = nullptr;
    return answer;
}

Json appliedAtRestart(Json answer, const std::string &reason) {
    answer["applied"] = "restart_required";
    answer["restart_required"] = {{"reason", reason}};
    LOG_WARNING(logger, "project document written; restart required: {}", reason);
    return answer;
}

OrderedJson routingJson(const MaterialVariantRouting &routing) {
    return {{"alpha_mode", std::string{materialAlphaModeName(routing.alpha_mode)}},
            {"double_sided", routing.double_sided}};
}

std::optional<MaterialVariantRouting> routingFromJson(const OrderedJson &value) {
    if (!value.is_object() || !value.contains("alpha_mode") || !value.at("alpha_mode").is_string() ||
        !value.contains("double_sided") || !value.at("double_sided").is_boolean())
        return std::nullopt;
    const auto mode = value.at("alpha_mode").get<std::string>();
    MaterialVariantRouting routing;
    if (mode == "opaque") routing.alpha_mode = MaterialAlphaMode::opaque;
    else if (mode == "mask") routing.alpha_mode = MaterialAlphaMode::mask;
    else if (mode == "blend") routing.alpha_mode = MaterialAlphaMode::blend;
    else return std::nullopt;
    routing.double_sided = value.at("double_sided").get<bool>();
    return routing;
}

// MT-3: a model's material bindings document with the entries that name one
// material. A binding carries the routing of the material it names, and a
// model whose binding's routing is not its material's does not load.
struct BindingsNaming {
    std::string model;
    std::string reference;
    std::filesystem::path path;
    std::string bytes;
    std::string digest;
    OrderedJson json;
    std::vector<std::size_t> entries;
};

// Every asset_data model whose bindings document names `material`. A
// document that does not read is passed over: the model does not load with it
// whatever this edit does.
std::vector<BindingsNaming> bindingsNaming(const AssetDataFile &asset_data, const PathResolver &paths,
                                           const std::string &material) {
    std::vector<BindingsNaming> result;
    const auto models = asset_data.json.find("models");
    if (models == asset_data.json.end() || !models->is_array()) return result;
    for (const auto &model : *models) {
        if (!model.is_object() || !model.contains("material_bindings") || !model.at("material_bindings").is_string())
            continue;
        BindingsNaming entry{.model = model.value("name", std::string{}),
                             .reference = model.at("material_bindings").get<std::string>()};
        try {
            entry.path = paths.resolveExistingFile(entry.reference);
            entry.bytes = readFileBytes(entry.path);
            entry.json = OrderedJson::parse(entry.bytes);
        } catch (const std::exception &) {
            continue;
        }
        if (!entry.json.is_object() || !entry.json.contains("bindings") || !entry.json.at("bindings").is_array())
            continue;
        const auto &bindings = entry.json.at("bindings");
        for (std::size_t index = 0; index < bindings.size(); ++index)
            if (bindings.at(index).is_object() && bindings.at(index).value("material", std::string{}) == material)
                entry.entries.push_back(index);
        if (entry.entries.empty()) continue;
        entry.digest = projectDocumentDigest(entry.bytes);
        result.push_back(std::move(entry));
    }
    return result;
}

// --- list_materials / get_material / get_surface_schema.

Json listMaterials(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "list_materials";
    if (!params.is_null() && !(params.is_object() && params.empty())) invalidParams(method, "takes no parameters");
    const auto asset_data = readAssetData(paths);
    auto documents = Json::array();
    for (const auto &reference : listedMaterialPaths(asset_data.json)) {
        Json entry{{"path", reference}, {"material_digest", nullptr}, {"materials", Json::array()}};
        std::string bytes;
        try {
            bytes = readFileBytes(paths.resolveExistingFile(reference));
        } catch (const std::exception &error) {
            entry["status"] = "missing";
            entry["reason"] = error.what();
            documents.push_back(std::move(entry));
            continue;
        }
        entry["material_digest"] = projectDocumentDigest(bytes);
        OrderedJson document;
        try {
            document = OrderedJson::parse(bytes);
        } catch (const std::exception &error) {
            entry["status"] = "invalid";
            entry["reason"] = error.what();
            documents.push_back(std::move(entry));
            continue;
        }
        if (!document.is_object() || document.value("schema", std::string{}) != materialSchema ||
            !document.contains("materials") || !document.at("materials").is_array()) {
            entry["status"] = "invalid";
            entry["reason"] = "not a pelican.material document with a materials array";
            documents.push_back(std::move(entry));
            continue;
        }
        entry["status"] = "loaded";
        for (const auto &material : document.at("materials")) {
            if (!material.is_object() || !material.contains("name") || !material.at("name").is_string()) continue;
            const auto name = material.at("name").get<std::string>();
            const auto surface = material.find("surface");
            const auto tags = material.find("tags");
            entry["materials"].push_back({
                {"name", name},
                {"surface", surface != material.end() && surface->is_string() ? Json(surface->get<std::string>())
                                                                              : Json(nullptr)},
                {"tags", tags != material.end() && tags->is_array() ? toJson(*tags) : Json::array()},
                {"id", materialDrawSubjectKey(name, reference, reference, std::nullopt)},
                {"registered", registeredFrom(name, reference)},
            });
        }
        documents.push_back(std::move(entry));
    }
    return {{"asset_data_digest", asset_data.digest}, {"documents", std::move(documents)}};
}

OrderedJson getMaterial(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "get_material";
    requireOnly(method, params, {"path", "name"});
    const auto path = requireString(method, params, "path");
    const auto name = optionalString(method, params, "name");
    const auto asset_data = readAssetData(paths);
    const auto listed = findListedMaterial(asset_data, paths, path);
    if (!listed)
        refuse(method, "asset_data materials[] does not list '" + path + "'", documentUnknownCode,
               {{"path", path}, {"reason", "not listed in asset_data materials[]"}});
    const auto document = readMaterialDocument(method, paths, *listed);
    const auto index = locateMaterial(method, document, name);
    const auto &material = document.json.at("materials").at(index);
    const auto material_name = material.value("name", std::string{});
    OrderedJson answer;
    answer["path"] = *listed;
    answer["material_digest"] = document.digest;
    answer["name"] = material_name;
    answer["names"] = materialNames(document.json);
    answer["registered"] = registeredFrom(material_name, *listed);
    answer["material"] = material;
    return answer;
}

Json getSurfaceSchema(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "get_surface_schema";
    requireOnly(method, params, {"surface"});
    const auto reference = requireString(method, params, "surface");
    SurfaceCache surfaces{paths};
    return surfaceSchemaJson(reference, requireSurface(method, surfaces, reference));
}

// --- list_surfaces.

std::string_view blendModeName(SurfaceBlendMode mode) {
    switch (mode) {
    case SurfaceBlendMode::opaque: return "opaque";
    case SurfaceBlendMode::blend: return "blend";
    case SurfaceBlendMode::additive: return "additive";
    }
    return "unknown";
}

std::string_view cullModeName(SurfaceCullMode mode) {
    switch (mode) {
    case SurfaceCullMode::none: return "none";
    case SurfaceCullMode::front: return "front";
    case SurfaceCullMode::back: return "back";
    }
    return "unknown";
}

std::string_view depthCompareName(SurfaceDepthCompare compare) {
    switch (compare) {
    case SurfaceDepthCompare::never: return "never";
    case SurfaceDepthCompare::less: return "less";
    case SurfaceDepthCompare::equal: return "equal";
    case SurfaceDepthCompare::less_equal: return "less_equal";
    case SurfaceDepthCompare::greater: return "greater";
    case SurfaceDepthCompare::not_equal: return "not_equal";
    case SurfaceDepthCompare::greater_equal: return "greater_equal";
    case SurfaceDepthCompare::always: return "always";
    }
    return "unknown";
}

// Whether the `//!` header (the source before the code) has a render_state
// key. The parsed document holds the defaults when it has none.
bool declaresRenderState(std::string_view source, std::size_t code_offset) {
    const auto header = source.substr(0, std::min(code_offset, source.size()));
    std::size_t start = 0;
    while (start < header.size()) {
        auto end = header.find('\n', start);
        if (end == std::string_view::npos) end = header.size();
        auto line = header.substr(start, end - start);
        start = end + 1;
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line.substr(first, 3) != "//!") continue;
        line = line.substr(first + 3);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        auto key = line.substr(0, colon);
        const auto key_first = key.find_first_not_of(" \t");
        if (key_first == std::string_view::npos) continue;
        key = key.substr(key_first, key.find_last_not_of(" \t") - key_first + 1);
        if (key == "render_state") return true;
    }
    return false;
}

// The engine's surface manifests (`surfaces/<family>/manifest.json`, e.g.
// OpenPBR's) name each surface they list and the material routing it is.
struct EngineSurfaceVariant {
    std::string manifest;
    std::string name;
    std::optional<std::string> alpha_mode;
    std::optional<bool> double_sided;
};

std::map<std::string, EngineSurfaceVariant> engineSurfaceVariants() {
    std::map<std::string, EngineSurfaceVariant> variants;
    for (const auto id : registeredEngineResourceIds()) {
        if (!id.starts_with("surfaces/") || !id.ends_with("/manifest.json")) continue;
        const auto bytes = engineResource(id);
        if (!bytes) continue;
        const auto manifest = Json::parse(bytes->begin(), bytes->end(), nullptr, false);
        if (!manifest.is_object() || !manifest.contains("variants") || !manifest.at("variants").is_array())
            continue;
        for (const auto &variant : manifest.at("variants")) {
            if (!variant.is_object() || !variant.contains("surface") || !variant.at("surface").is_string() ||
                !variant.contains("name") || !variant.at("name").is_string())
                continue;
            EngineSurfaceVariant entry{.manifest = std::string{id}, .name = variant.at("name").get<std::string>()};
            if (variant.contains("alpha_mode") && variant.at("alpha_mode").is_string())
                entry.alpha_mode = variant.at("alpha_mode").get<std::string>();
            if (variant.contains("double_sided") && variant.at("double_sided").is_boolean())
                entry.double_sided = variant.at("double_sided").get<bool>();
            variants.emplace(variant.at("surface").get<std::string>(), std::move(entry));
        }
    }
    return variants;
}

// Every `*.surface` under the project directory, as project:// references,
// sorted. Hidden directories (.git, .pelican, ...) and symlinked ones are
// not entered.
std::vector<std::string> projectSurfaceReferences(const std::filesystem::path &root) {
    std::vector<std::string> references;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it{root, std::filesystem::directory_options::skip_permission_denied,
                                                     error};
    if (error) return references;
    for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(error)) {
        if (error) break;
        const auto &entry = *it;
        std::error_code status;
        if (entry.is_directory(status)) {
            const auto name = entry.path().filename().generic_u8string();
            if ((!name.empty() && name.front() == u8'.') || entry.is_symlink(status)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(status) || entry.path().extension() != ".surface") continue;
        const auto relative = entry.path().lexically_relative(root).generic_u8string();
        references.push_back("project://" + std::string{relative.begin(), relative.end()});
    }
    std::sort(references.begin(), references.end());
    return references;
}

// One list_surfaces entry: the surface read as get_surface_schema reads it,
// or `error` when it does not read or parse.
Json surfaceListEntry(const PathResolver &paths, const std::string &reference, std::string_view source,
                      const EngineSurfaceVariant *variant) {
    Json entry{{"reference", reference}, {"source", std::string{source}}};
    if (variant) entry["name"] = variant->name;
    try {
        const auto text = paths.loadText(reference);
        const auto surface = parseSurfaceFormat(text, reference);
        entry["params"] = surface.params.size();
        entry["textures"] = surface.textures.size();
        // A material's routing as create_material takes it: only where the
        // engine's manifest says it.
        if (variant && variant->alpha_mode && variant->double_sided)
            entry["routing"] = {{"alpha_mode", *variant->alpha_mode}, {"double_sided", *variant->double_sided}};
        if (declaresRenderState(text, surface.code_offset)) {
            const auto &state = surface.render_state;
            entry["render_state"] = {{"blend", std::string{blendModeName(state.blend)}},
                                     {"cull", std::string{cullModeName(state.cull)}},
                                     {"depth_test", state.depth_test},
                                     {"depth_write", state.depth_write},
                                     {"depth_compare", std::string{depthCompareName(state.depth_compare)}}};
        }
    } catch (const std::exception &error) {
        entry["error"] = error.what();
    }
    return entry;
}

Json listSurfaces(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "list_surfaces";
    if (!params.is_null() && !(params.is_object() && params.empty())) invalidParams(method, "takes no parameters");
    auto surfaces = Json::array();
    const auto variants = engineSurfaceVariants();
    std::vector<std::string> engine;
    for (const auto id : registeredEngineResourceIds())
        if (id.starts_with("surfaces/") && id.ends_with(".surface")) engine.push_back("engine://" + std::string{id});
    std::sort(engine.begin(), engine.end());
    for (const auto &reference : engine) {
        const auto variant = variants.find(reference);
        surfaces.push_back(
            surfaceListEntry(paths, reference, "engine", variant == variants.end() ? nullptr : &variant->second));
    }
    if (paths.isSetup())
        for (const auto &reference : projectSurfaceReferences(paths.projectRoot()))
            surfaces.push_back(surfaceListEntry(paths, reference, "project", nullptr));
    return {{"surfaces", std::move(surfaces)}};
}

// --- edit_material.

// MT-3 stage 2: an edit outside values (textures, tags, routing, variants,
// surface) applied without a restart. The document's materials are
// registered anew (shader, pipeline, textures, draw tags, route), the models
// drawing with the previous registration -- by their template or a scene
// object's own material -- are rebuilt in place (their draw items take the
// new one), and the previous registration is released after the frames in
// flight. `rebound`: the bindings documents this edit rewrote, which the
// models' declarations take before the rebuild.
Json rebuildMaterial(Json answer, const std::string &path, const std::string &name,
                     const std::vector<std::string> &structural, const std::vector<BindingsNaming> &rebound,
                     MaterialContainer &materials, watch::ReloadService *reload_service) {
    std::string keys;
    for (const auto &key : structural) keys += (keys.empty() ? "" : ", ") + key;
    auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>();
    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    if (reload_service == nullptr) reload_service = FastModuleContainer::tryGet<watch::ReloadService>();
    if (project == nullptr || models == nullptr || reload_service == nullptr)
        return appliedAtRestart(std::move(answer), keys + " of material '" + name +
                                                       "' are read when the engine starts (this host cannot "
                                                       "rebuild the material in place)");
    if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();

    std::vector<std::pair<std::string, std::optional<PrimitiveMaterialBindingDocument>>> exchanged;
    const auto restoreBindings = [&] {
        for (auto &[model, previous] : exchanged) models->restoreMaterialBindings(model, std::move(previous));
    };
    ProjectMaterialAssetContainer::Reregistered registered;
    try {
        for (const auto &bindings : rebound)
            if (models->hasModel(bindings.model))
                exchanged.emplace_back(bindings.model, models->exchangeMaterialBindings(bindings.model, bindings.reference));
        registered = project->reregisterDocument(path);
    } catch (const std::exception &error) {
        restoreBindings();
        return appliedAtRestart(std::move(answer), keys + " of material '" + name +
                                                       "' could not be registered in place: " + error.what());
    }

    auto rebuilt = models->modelsDrawingMaterials(registered.previous);
    for (const auto &[model, previous] : exchanged) {
        (void)previous;
        if (std::find(rebuilt.begin(), rebuilt.end(), model) == rebuilt.end()) rebuilt.push_back(model);
    }
    try {
        std::vector<watch::AssetKey> reload;
        for (const auto &model : rebuilt) reload.push_back(models->reloadKey(model));
        if (!reload.empty()) reload_service->reloadModelsNow(reload);
    } catch (const std::exception &error) {
        // The previous registration stays: the models still draw with it.
        restoreBindings();
        return appliedAtRestart(std::move(answer), "material '" + name +
                                                       "' was registered again but the models drawing it could "
                                                       "not be rebuilt in place: " + error.what());
    }
    ReleaseErrorSink::record("edited material release",
                             materials.releaseModelResources(std::move(registered.previous), {}, true));
    LOG_INFO(logger, "material '{}' rebuilt in place ({}); {} model(s) rebuilt", name, keys, rebuilt.size());
    answer["applied"] = "rebuilt";
    answer["restart_required"] = nullptr;
    answer["rebuilt_models"] = rebuilt;
    return answer;
}

// VR-1: an engine surface manifest (OpenPBR's) pairs each surface it lists
// with one routing, and a material on such a surface is drawn by that pair
// (lowering refuses a routing whose pipeline state is not its surface's).
// Studio sends the surface and the routing each on its own, so the half not
// sent follows the half sent, in the same edit: a surface alone takes its
// routing (the material holding one), a routing alone takes the surface of
// that routing from the manifest of the material's surface. Both sent, or a
// surface no manifest lists, are taken as they are.
void pairSurfaceVariant(Json &params, const OrderedJson &material) {
    const bool surface_sent = params.contains("surface");
    const bool routing_sent = params.contains("routing") && params.at("routing").is_object();
    if (surface_sent == routing_sent) return;
    const auto variants = engineSurfaceVariants();
    const auto routingOf = [](const EngineSurfaceVariant &variant) -> std::optional<OrderedJson> {
        if (!variant.alpha_mode || !variant.double_sided) return std::nullopt;
        return OrderedJson{{"alpha_mode", *variant.alpha_mode}, {"double_sided", *variant.double_sided}};
    };
    if (surface_sent) {
        if (params.contains("routing") || !material.contains("routing")) return;
        const auto found = variants.find(params.at("surface").get<std::string>());
        if (found == variants.end()) return;
        const auto routing = routingOf(found->second);
        if (!routing) return;
        const auto held = routingFromJson(material.at("routing"));
        if (!held || *held != *routingFromJson(*routing)) params["routing"] = toJson(*routing);
        return;
    }
    if (!material.contains("surface") || !material.at("surface").is_string()) return;
    const auto current = variants.find(material.at("surface").get<std::string>());
    const auto wanted = routingFromJson(toOrdered(params.at("routing")));
    if (current == variants.end() || !wanted) return;
    for (const auto &[reference, variant] : variants) {
        if (variant.manifest != current->second.manifest) continue;
        const auto routing = routingOf(variant);
        if (!routing || *routingFromJson(*routing) != *wanted) continue;
        if (reference != current->first) params["surface"] = reference;
        return;
    }
}

Json editMaterial(const Json &sent, PathResolver &paths, watch::ReloadService *reload_service) {
    constexpr std::string_view method = "edit_material";
    Json params = sent;
    requireOnly(method, params,
                {"base_material_digest", "path", "name", "values", "textures", "tags", "routing", "variants",
                 "surface"});
    const auto base = requireString(method, params, "base_material_digest");
    const auto path = requireString(method, params, "path");
    const auto name = optionalString(method, params, "name");
    for (const auto *key : {"tags", "routing", "variants"}) {
        const auto found = params.find(key);
        if (found == params.end() || found->is_null()) continue;
        const bool ok = std::string_view{key} == "tags" ? found->is_array() : found->is_object();
        if (!ok)
            invalidParams(method, "'" + std::string{key} + "' must be " +
                                      (std::string_view{key} == "tags" ? "an array" : "an object") + " or null");
    }
    if (params.contains("surface") && !params.at("surface").is_string())
        invalidParams(method, "'surface' must be a string reference");

    const auto asset_data = readAssetData(paths);
    const auto listed = findListedMaterial(asset_data, paths, path);
    if (!listed)
        refuse(method, "asset_data materials[] does not list '" + path + "'", documentUnknownCode,
               {{"path", path}, {"reason", "not listed in asset_data materials[]"}});
    const auto document = readMaterialDocument(method, paths, *listed);
    if (base != document.digest) {
        refuse(method,
               "'" + *listed + "' changed since it was read (expected sha256 " + base + ", found " +
                   document.digest + ")",
               externalModificationCode, {{"path", *listed}, {"expected", base}, {"actual", document.digest}});
    }
    const auto index = locateMaterial(method, document, name);
    auto candidate = document.json;
    auto &material = candidate.at("materials").at(index);
    const auto material_name = material.value("name", std::string{});
    auto previous = OrderedJson::object();
    SurfaceCache surfaces{paths};
    pairSurfaceVariant(params, material);

    if (params.contains("surface")) {
        previous["surface"] = material.contains("surface") ? material.at("surface") : OrderedJson(nullptr);
        const auto reference = params.at("surface").get<std::string>();
        (void)requireSurface(method, surfaces, reference);
        material["surface"] = reference;
    }
    const auto surface_reference = material.contains("surface") && material.at("surface").is_string()
                                       ? std::optional<std::string>{material.at("surface").get<std::string>()}
                                       : std::nullopt;
    for (const auto *key : {"values", "textures"}) {
        if (!params.contains(key)) continue;
        if (!surface_reference)
            refuse(method, "material '" + material_name + "' names no surface to take " + key, documentInvalidCode,
                   {{"path", *listed}, {"reason", "the material names no surface"}});
        const auto &surface = requireSurface(method, surfaces, *surface_reference);
        mergeSurfaceInputs(method, *listed, params.at(key), key, surface, paths, material, &previous);
    }
    if (surface_reference && params.contains("surface")) {
        // What the material already sets has to be declared by the new surface.
        const auto &surface = requireSurface(method, surfaces, *surface_reference);
        for (const auto *key : {"values", "textures"}) {
            if (!material.contains(key) || !material.at(key).is_object()) continue;
            const bool textures = std::string_view{key} == "textures";
            for (const auto &[param, value] : material.at(key).items()) {
                (void)value;
                const bool declared =
                    textures ? findTexture(surface, param) != nullptr : findParam(surface, param) != nullptr;
                if (!declared)
                    refuse(method, "surface '" + *surface_reference + "' declares no '" + param + "'",
                           paramUnknownCode,
                           {{"path", *listed}, {"param", param}, {"kind", textures ? "texture" : "value"}});
            }
        }
    }
    for (const auto *key : {"tags", "routing", "variants"}) {
        if (!params.contains(key)) continue;
        previous[key] = material.contains(key) ? material.at(key) : OrderedJson(nullptr);
        if (params.at(key).is_null()) material.erase(key);
        else material[key] = toOrdered(params.at(key));
    }
    // MT-3 stage 1: a routing change is carried into every binding that names
    // the material, in the same transaction (the model would not load with a
    // binding whose routing is not its material's). A routing taken off
    // cannot be carried: a binding needs one.
    const auto field = [](const OrderedJson &object, const std::string &key) {
        return object.contains(key) ? object.at(key) : OrderedJson(nullptr);
    };
    const auto &original = document.json.at("materials").at(index);
    std::vector<BindingsNaming> rebound;
    if (field(original, "routing") != field(material, "routing"))
        rebound = bindingsNaming(asset_data, paths, material_name);
    if (!rebound.empty() && !material.contains("routing")) {
        auto bound_by = Json::array();
        std::string models;
        for (const auto &bindings : rebound) {
            const auto &entry = bindings.json.at("bindings").at(bindings.entries.front());
            bound_by.push_back({{"model", bindings.model},
                                {"routing", entry.contains("routing") ? toJson(entry.at("routing")) : Json(nullptr)}});
            models += (models.empty() ? "'" : ", '") + bindings.model + "'";
        }
        refuse(method,
               "material '" + material_name + "' is bound by model " + models +
                   ", whose bindings need its routing; bind them to another material first",
               routingConflictCode,
               {{"path", *listed}, {"material", material_name}, {"routing", nullptr}, {"bound_by", std::move(bound_by)}});
    }
    checkCandidateDocument(method, *listed, candidate, surfaces);
    if (!rebound.empty()) {
        const auto routing = routingFromJson(material.at("routing"));
        if (!routing)
            refuse(method, "material '" + material_name + "' routing is not {alpha_mode, double_sided}",
                   documentInvalidCode, {{"path", *listed}, {"reason", "routing is not {alpha_mode, double_sided}"}});
        for (auto &bindings : rebound) {
            for (const auto entry : bindings.entries)
                bindings.json.at("bindings").at(entry)["routing"] = routingJson(*routing);
            try {
                (void)parsePrimitiveMaterialBindingJson(toJson(bindings.json));
            } catch (const std::exception &error) {
                refuse(method, "model '" + bindings.model + "' material bindings would not load: " + error.what(),
                       bindingInvalidCode, {{"model", bindings.model}, {"reason", error.what()}});
            }
        }
    }

    Json answer{{"committed", true},
                {"path", *listed},
                {"name", material_name},
                {"previous_material_digest", document.digest},
                {"previous", toJson(previous)}};
    if (candidate == document.json) {
        answer["changed"] = false;
        answer["material_digest"] = document.digest;
        return appliedLive(std::move(answer));
    }
    auto bytes = renderJsonDocument(candidate, document.bytes);
    const auto digest = projectDocumentDigest(bytes);
    std::vector<ProjectCandidateDocument> documents;
    documents.push_back(replacement(*listed, document.path, document.digest, std::move(bytes)));
    auto rewritten = Json::array();
    for (const auto &bindings : rebound) {
        auto rebound_bytes = renderJsonDocument(bindings.json, bindings.bytes);
        rewritten.push_back({{"model", bindings.model},
                             {"material_bindings", bindings.reference},
                             {"bindings_digest", projectDocumentDigest(rebound_bytes)},
                             {"previous_bindings_digest", bindings.digest}});
        documents.push_back(replacement(bindings.reference, bindings.path, bindings.digest, std::move(rebound_bytes)));
    }
    commitDocuments(method, paths.projectRoot(), documents, *listed);
    answer["changed"] = true;
    answer["material_digest"] = digest;
    answer["rewritten_bindings"] = std::move(rewritten);
    LOG_INFO(logger, "material '{}' in '{}' edited", material_name, *listed);

    std::vector<std::string> structural;
    // Only what this edit really changed decides: values alone are live.
    for (const auto &[key, value] : previous.items()) {
        (void)value;
        if (key != "values" && field(original, key) != field(material, key)) structural.push_back(key);
    }
    if (!registeredFrom(material_name, *listed))
        return appliedAtRestart(std::move(answer), "material '" + material_name +
                                                       "' is not registered in this engine; it is read when the "
                                                       "engine starts");
    auto *materials = FastModuleContainer::tryGet<MaterialContainer>();
    if (materials == nullptr)
        return appliedAtRestart(std::move(answer), "this host runs no material container");
    if (!structural.empty()) return rebuildMaterial(std::move(answer), *listed, material_name, structural, rebound,
                                                    *materials, reload_service);
    try {
        const std::array keys{watch::makeAssetKey(*listed)};
        if (auto commit = materials->prepareSurfaceMaterialReload({}, keys)) commit();
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the values could not be applied in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// --- create_material.

std::string projectRelative(std::string_view method, const std::string &path) {
    std::string relative = path;
    if (relative.starts_with("project://")) relative = relative.substr(std::string_view{"project://"}.size());
    const auto invalid = [&](const std::string &reason) {
        refuse(method, "'" + path + "' " + reason, pathInvalidCode, {{"path", path}, {"reason", reason}});
    };
    if (relative.empty()) invalid("names no file");
    if (relative.find('\\') != std::string::npos) invalid("must use forward slashes");
    if (relative.find("://") != std::string::npos) invalid("must be a project:// or project-relative path");
    if (relative.front() == '/' || (relative.size() > 1 && relative[1] == ':')) invalid("must be project-relative");
    if (!relative.ends_with(".json")) invalid("must end in .json");
    const std::filesystem::path parsed{relative};
    for (const auto &part : parsed)
        if (part == ".." || part == ".") invalid("must not contain '.' or '..' segments");
    return relative;
}

std::string defaultMaterialName(const std::string &relative) {
    auto file = std::filesystem::path{relative}.filename().generic_string();
    for (const std::string_view suffix : {".material.json", ".json"})
        if (file.ends_with(suffix)) return file.substr(0, file.size() - suffix.size());
    return file;
}

// Where a name is held already: a listed document (path) or a model's glTF
// material (model; a project material of that name would make the model's
// bindings ambiguous and stop it loading).
std::optional<Json> nameHolder(const std::string &name, const AssetDataFile &asset_data, const PathResolver &paths) {
    for (const auto &reference : listedMaterialPaths(asset_data.json)) {
        try {
            const auto names = materialNames(OrderedJson::parse(readFileBytes(paths.resolveExistingFile(reference))));
            if (std::find(names.begin(), names.end(), name) != names.end()) return Json{{"path", reference}};
        } catch (const std::exception &) {
        }
    }
    if (const auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>())
        if (const auto document = project->documentOf(name)) return Json{{"path", *document}};
    return std::nullopt;
}

Json createMaterial(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "create_material";
    requireOnly(method, params, {"path", "surface", "name", "values", "tags", "routing"});
    const auto path = requireString(method, params, "path");
    const auto surface_reference = requireString(method, params, "surface");
    if (params.contains("tags") && !params.at("tags").is_array()) invalidParams(method, "'tags' must be an array");
    if (params.contains("routing") && !params.at("routing").is_object())
        invalidParams(method, "'routing' must be an object");
    const auto relative = projectRelative(method, path);
    const auto name = optionalString(method, params, "name").value_or(defaultMaterialName(relative));
    if (!isIdentifier(name))
        refuse(method, "'" + name + "' is not a material name", nameInvalidCode,
               {{"name", name}, {"reason", "a material name is letters, digits and '_'"}});

    const auto asset_data = readAssetData(paths);
    const auto file = paths.projectRoot() / std::filesystem::path{relative};
    if (findListedMaterial(asset_data, paths, relative) || std::filesystem::exists(file))
        refuse(method, "'" + relative + "' is there already", pathConflictCode, {{"path", relative}});
    if (auto holder = nameHolder(name, asset_data, paths)) {
        Json fields{{"name", name}};
        fields.update(*holder);
        refuse(method, "material '" + name + "' exists already", nameConflictCode, std::move(fields));
    }
    if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>())
        if (const auto holder = models->modelDeclaringMaterial(name))
            refuse(method, "model '" + *holder + "' has a glTF material named '" + name + "'", nameConflictCode,
                   {{"name", name}, {"model", *holder}});

    SurfaceCache surfaces{paths};
    const auto &surface = requireSurface(method, surfaces, surface_reference);
    OrderedJson material;
    material["name"] = name;
    if (params.contains("tags")) material["tags"] = toOrdered(params.at("tags"));
    material["surface"] = surface_reference;
    if (params.contains("values"))
        mergeSurfaceInputs(method, relative, params.at("values"), "values", surface, paths, material, nullptr);
    material["routing"] = params.contains("routing")
                              ? toOrdered(params.at("routing"))
                              : OrderedJson{{"alpha_mode", "opaque"}, {"double_sided", false}};
    OrderedJson document;
    document["schema"] = std::string{materialSchema};
    document["version"] = 1;
    document["materials"] = OrderedJson::array({material});
    checkCandidateDocument(method, relative, document, surfaces);

    auto asset_json = asset_data.json;
    if (!asset_json.contains("materials") || !asset_json.at("materials").is_array())
        asset_json["materials"] = OrderedJson::array();
    asset_json["materials"].push_back({{"path", relative}});
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
               {{"path", asset_data.reference}, {"reason", error.what()}});
    }
    auto bytes = renderJsonDocument(document, {});
    const auto digest = projectDocumentDigest(bytes);
    auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    const auto asset_digest = projectDocumentDigest(asset_bytes);
    const std::array documents{
        creation(relative, file, std::move(bytes)),
        replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes),
    };
    commitDocuments(method, paths.projectRoot(), documents, relative);
    publishAssetData(paths, asset_bytes);
    LOG_INFO(logger, "material '{}' created in '{}'", name, relative);

    Json answer{{"committed", true},
                {"changed", true},
                {"path", relative},
                {"name", name},
                {"material_digest", digest},
                {"asset_data_digest", asset_digest},
                {"previous_asset_data_digest", asset_data.digest}};
    if (FastModuleContainer::tryGet<MaterialContainer>() == nullptr)
        return appliedAtRestart(std::move(answer), "this host runs no material container");
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        if (auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>()) {
            (void)project->registerDocument(relative);
        } else {
            // Made now, from the asset_data just published: it registers every
            // listed document, this one with them.
            (void)GET_MODULE(ProjectMaterialAssetContainer);
        }
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the material could not be registered in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// --- get_material_bindings / bind_material.

using PrimitiveKey = std::pair<std::uint32_t, std::uint32_t>;

struct BindingEntry {
    std::string material;
    std::optional<std::string> usd_path;
    std::optional<MaterialVariantRouting> routing;
};

struct ModelBindingState {
    std::string model;
    std::optional<std::size_t> asset_index;
    std::optional<std::string> bindings_reference;
    std::filesystem::path bindings_path;
    std::string bindings_bytes;
    std::string bindings_digest;
    OrderedJson bindings_json;
    std::vector<ModelMaterialSlot> slots;
    std::map<PrimitiveKey, BindingEntry> current;
};

ModelBindingState readBindingState(std::string_view method, const std::string &model,
                                   const AssetDataFile &asset_data, const PathResolver &paths) {
    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    if (models == nullptr)
        refuse(method, "this host runs no model container", modelUnknownCode,
               {{"model", model}, {"reason", "no model container"}});
    ModelBindingState state;
    state.model = model;
    if (const auto declared = asset_data.json.find("models"); declared != asset_data.json.end() && declared->is_array())
        for (std::size_t index = 0; index < declared->size(); ++index)
            if (declared->at(index).is_object() && declared->at(index).value("name", std::string{}) == model)
                state.asset_index = index;
    const bool primitive = isEnginePrimitiveReference(model);
    if (!state.asset_index && !primitive)
        refuse(method, "asset_data declares no model '" + model + "'", modelUnknownCode,
               {{"model", model}, {"reason", "not declared in asset_data models[]"}});
    try {
        if (primitive) requireKnownEnginePrimitive(model);
        if (!models->hasModel(model)) {
            if (!primitive) throw std::runtime_error("the model is not loaded by this engine");
            (void)models->getModelTemplateByName(model);
        }
        state.slots = models->materialSlots(model);
    } catch (const std::exception &error) {
        refuse(method, "model '" + model + "': " + error.what(), modelUnknownCode,
               {{"model", model}, {"reason", error.what()}});
    }
    if (state.asset_index) {
        const auto &entry = asset_data.json.at("models").at(*state.asset_index);
        if (entry.contains("material_bindings") && entry.at("material_bindings").is_string())
            state.bindings_reference = entry.at("material_bindings").get<std::string>();
    }
    if (state.bindings_reference) {
        try {
            state.bindings_path = paths.resolveExistingFile(*state.bindings_reference);
            state.bindings_bytes = readFileBytes(state.bindings_path);
            state.bindings_json = OrderedJson::parse(state.bindings_bytes);
            const auto parsed = parsePrimitiveMaterialBindingJson(toJson(state.bindings_json));
            for (const auto &binding : parsed.bindings)
                state.current.insert_or_assign(PrimitiveKey{binding.mesh_index, binding.primitive_index},
                                               BindingEntry{binding.material, binding.usd_path, binding.routing});
        } catch (const std::exception &error) {
            refuse(method, "model '" + model + "' material bindings: " + error.what(), bindingInvalidCode,
                   {{"model", model}, {"reason", error.what()}});
        }
        state.bindings_digest = projectDocumentDigest(state.bindings_bytes);
    }
    for (const auto &slot : state.slots)
        state.current.try_emplace(PrimitiveKey{slot.mesh, slot.primitive},
                                  BindingEntry{slot.slot, std::nullopt, slot.routing});
    return state;
}

// MT-2: the scene objects (every scene of the document as the editor holds it,
// prefab-generated objects included) whose simplemodelview draws `model`, and
// how many of them carry their own materials (a non-empty
// simplemodelview.materials).
std::pair<std::size_t, std::size_t> countModelObjects(const std::string &model) {
    std::size_t objects = 0;
    std::size_t overrides = 0;
    const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    if (config == nullptr) return {objects, overrides};
    for (const auto &scene : config->resolvedScene().scenes()) {
        for (const auto &object : scene.objects) {
            for (const auto &component : object.components) {
                if (component.name != "simplemodelview" || !component.effective_json.is_object() ||
                    component.effective_json.value("model", std::string{}) != model)
                    continue;
                ++objects;
                const auto materials = component.effective_json.find("materials");
                if (materials != component.effective_json.end() && materials->is_object() &&
                    materials->contains("rows") && materials->at("rows").is_array() &&
                    !materials->at("rows").empty())
                    ++overrides;
                break;
            }
        }
    }
    return {objects, overrides};
}

Json getMaterialBindings(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "get_material_bindings";
    requireOnly(method, params, {"model"});
    const auto model = requireString(method, params, "model");
    const auto state = readBindingState(method, model, readAssetData(paths), paths);
    const auto [objects, object_overrides] = countModelObjects(model);
    auto primitives = Json::array();
    for (const auto &slot : state.slots) {
        const auto &entry = state.current.at({slot.mesh, slot.primitive});
        primitives.push_back({{"mesh", slot.mesh},
                              {"primitive", slot.primitive},
                              {"slot", slot.slot},
                              {"material", entry.material},
                              {"usd_path", entry.usd_path ? Json(*entry.usd_path) : Json(nullptr)}});
    }
    return {{"model", model},
            {"material_bindings", state.bindings_reference ? Json(*state.bindings_reference) : Json(nullptr)},
            {"bindings_digest", state.bindings_reference ? Json(state.bindings_digest) : Json(nullptr)},
            {"primitives", std::move(primitives)},
            {"objects", objects},
            {"object_overrides", object_overrides}};
}

struct BindingMaterial {
    MaterialVariantRouting routing;
    bool live = true;
};

BindingMaterial resolveBindingMaterial(std::string_view method, const std::string &name,
                                       const ModelBindingState &state, const AssetDataFile &asset_data,
                                       const PathResolver &paths) {
    std::optional<BindingMaterial> slot;
    for (const auto &candidate : state.slots)
        if (candidate.slot == name && candidate.routing) slot = BindingMaterial{*candidate.routing, true};
    // MT-3: the routing the document holds, which the next start checks the
    // binding against; live when this engine registered the material from it.
    std::optional<BindingMaterial> project;
    {
        for (const auto &reference : listedMaterialPaths(asset_data.json)) {
            OrderedJson document;
            try {
                document = OrderedJson::parse(readFileBytes(paths.resolveExistingFile(reference)));
            } catch (const std::exception &) {
                continue;
            }
            if (!document.contains("materials") || !document.at("materials").is_array()) continue;
            for (const auto &material : document.at("materials")) {
                if (!material.is_object() || material.value("name", std::string{}) != name) continue;
                const auto routing = material.contains("routing") ? routingFromJson(material.at("routing"))
                                                                  : std::nullopt;
                if (!routing)
                    refuse(method, "material '" + name + "' declares no routing, which a binding needs",
                           bindingInvalidCode,
                           {{"model", state.model}, {"reason", "material '" + name + "' has no routing"}});
                project = BindingMaterial{*routing, registeredFrom(name, reference)};
            }
        }
    }
    if (slot && project)
        refuse(method, "'" + name + "' is both a glTF material of the model and a project material",
               bindingInvalidCode,
               {{"model", state.model}, {"reason", "material name '" + name + "' is ambiguous"}});
    if (slot) return *slot;
    if (project) return *project;
    refuse(method, "no material '" + name + "' to bind", nameUnknownCode,
           {{"name", name}, {"reason", "neither a project material nor a glTF material of model '" + state.model + "'"}});
}

std::string bindingsBaseName(const std::string &model) {
    std::string base = isEnginePrimitiveReference(model) ? model.substr(enginePrimitivePrefix.size()) : model;
    for (auto &ch : base)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
              ch == '-'))
            ch = '_';
    return base.empty() ? std::string{"model"} : base;
}

// MT-2b: bind_material whose result is the model's own materials. The model's
// material_bindings document is erased and its asset_data declaration taken
// off (an engine primitive's entry, which only the bind made, goes with it),
// and the model is rebuilt without bindings.
Json unbindModel(std::string_view method, const ModelBindingState &state, const AssetDataFile &asset_data,
                 PathResolver &paths, watch::ReloadService *reload_service, Json previous) {
    const auto &model = state.model;
    Json answer{{"committed", true},
                {"model", model},
                {"previous_bindings_digest", state.bindings_reference ? Json(state.bindings_digest) : Json(nullptr)},
                {"previous", {{"bindings", std::move(previous)}}},
                {"material_bindings", nullptr},
                {"bindings_digest", nullptr}};
    if (!state.bindings_reference) {
        answer["changed"] = false;
        return appliedLive(std::move(answer));
    }
    auto asset_json = asset_data.json;
    auto &models_json = asset_json.at("models");
    auto &entry = models_json.at(*state.asset_index);
    entry.erase("material_bindings");
    if (isEnginePrimitiveReference(model) && entry.size() == 2 && entry.value("path", std::string{}) == model)
        models_json.erase(models_json.begin() + static_cast<std::ptrdiff_t>(*state.asset_index));
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
               {{"path", asset_data.reference}, {"reason", error.what()}});
    }
    const auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    const std::array documents{
        ProjectCandidateDocument{
            .reference = *state.bindings_reference,
            .normalized_reference = *state.bindings_reference,
            .path = state.bindings_path,
            .operation = ProjectDocumentOperation::erase,
            .expected = {ProjectDocumentExistence::present, state.bindings_digest},
            .bytes = {},
        },
        replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes),
    };
    commitDocuments(method, paths.projectRoot(), documents, *state.bindings_reference);
    publishAssetData(paths, asset_bytes);
    answer["asset_data_digest"] = projectDocumentDigest(asset_bytes);
    answer["previous_asset_data_digest"] = asset_data.digest;
    answer["changed"] = true;
    LOG_INFO(logger, "material bindings of model '{}' taken off ('{}' erased)", model, *state.bindings_reference);

    if (reload_service == nullptr) reload_service = FastModuleContainer::tryGet<watch::ReloadService>();
    if (reload_service == nullptr)
        return appliedAtRestart(std::move(answer),
                                "this host has no reload service; the model's own materials apply when the engine starts");
    auto &models = GET_MODULE(ModelAssetContainer);
    auto replaced = models.declaredMaterialBindings(model);
    models.restoreMaterialBindings(model, std::nullopt);
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        const std::array keys{models.reloadKey(model)};
        reload_service->reloadModelsNow(keys);
    } catch (const std::exception &error) {
        models.restoreMaterialBindings(model, std::move(replaced));
        return appliedAtRestart(std::move(answer),
                                std::string{"the model could not be rebuilt in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

Json bindMaterial(const Json &params, PathResolver &paths, watch::ReloadService *reload_service) {
    constexpr std::string_view method = "bind_material";
    requireOnly(method, params, {"model", "material", "primitive", "slot", "bindings"});
    const auto model = requireString(method, params, "model");
    const bool listed_form = params.contains("bindings");
    if (listed_form && (params.contains("material") || params.contains("primitive") || params.contains("slot")))
        invalidParams(method, "takes either 'bindings' or 'material' with 'primitive' / 'slot'");
    if (params.contains("primitive") && params.contains("slot"))
        invalidParams(method, "takes 'primitive' or 'slot', not both");

    const auto asset_data = readAssetData(paths);
    auto state = readBindingState(method, model, asset_data, paths);

    // The primitives asked for, each with the material it is to carry.
    std::map<PrimitiveKey, std::string> requested;
    const auto requirePrimitive = [&](const Json &value, std::string_view where) {
        const auto index = [](const Json &object, const char *key) {
            return object.contains(key) && object.at(key).is_number_integer() &&
                   object.at(key).get<std::int64_t>() >= 0 &&
                   object.at(key).get<std::int64_t>() <= std::numeric_limits<std::uint32_t>::max();
        };
        if (!value.is_object() || !index(value, "mesh") || !index(value, "primitive"))
            invalidParams(method, std::string{where} + " must be {mesh, primitive} (non-negative integers)");
        const PrimitiveKey key{value.at("mesh").get<std::uint32_t>(), value.at("primitive").get<std::uint32_t>()};
        if (!state.current.contains(key))
            refuse(method,
                   "model '" + model + "' has no mesh " + std::to_string(key.first) + " primitive " +
                       std::to_string(key.second),
                   bindingInvalidCode, {{"model", model}, {"reason", "no such primitive"}});
        return key;
    };
    if (listed_form) {
        const auto &bindings = params.at("bindings");
        if (!bindings.is_array() || bindings.empty()) invalidParams(method, "'bindings' must be a non-empty array");
        for (const auto &binding : bindings) {
            if (!binding.is_object() || !binding.contains("material") || !binding.at("material").is_string())
                invalidParams(method, "'bindings' entries are {mesh, primitive, material}");
            requested.insert_or_assign(requirePrimitive(binding, "a 'bindings' entry"),
                                       binding.at("material").get<std::string>());
        }
    } else {
        const auto material = requireString(method, params, "material");
        if (params.contains("primitive")) {
            requested.emplace(requirePrimitive(params.at("primitive"), "'primitive'"), material);
        } else if (params.contains("slot")) {
            if (!params.at("slot").is_string()) invalidParams(method, "'slot' must be a string");
            const auto slot = params.at("slot").get<std::string>();
            for (const auto &candidate : state.slots)
                if (candidate.slot == slot) requested.emplace(PrimitiveKey{candidate.mesh, candidate.primitive}, material);
            if (requested.empty())
                refuse(method, "no primitive of model '" + model + "' carries '" + slot + "'", bindingInvalidCode,
                       {{"model", model}, {"reason", "no primitive carries slot '" + slot + "'"}});
        } else {
            for (const auto &[key, entry] : state.current) {
                (void)entry;
                requested.emplace(key, material);
            }
        }
    }

    bool live = true;
    auto previous = Json::array();
    std::map<std::string, BindingMaterial> resolved;
    for (const auto &[key, material] : requested) {
        auto found = resolved.find(material);
        if (found == resolved.end())
            found = resolved.emplace(material, resolveBindingMaterial(method, material, state, asset_data, paths)).first;
        live = live && found->second.live;
        auto &entry = state.current.at(key);
        if (entry.material == material && entry.routing == found->second.routing) continue;
        previous.push_back({{"mesh", key.first}, {"primitive", key.second}, {"material", entry.material}});
        entry.material = material;
        entry.routing = found->second.routing;
    }

    // MT-2b: every primitive back on the material its model file (or engine
    // primitive) gives it is the model with no bindings: the declaration and
    // its document are taken off instead of written (a binding document could
    // not even name the engine primitive's pelican.primitive.default).
    const bool model_default = std::all_of(state.slots.begin(), state.slots.end(), [&](const ModelMaterialSlot &slot) {
        const auto &entry = state.current.at({slot.mesh, slot.primitive});
        return !slot.slot.empty() && entry.material == slot.slot && entry.routing == slot.routing;
    });
    if (model_default) return unbindModel(method, state, asset_data, paths, reload_service, std::move(previous));

    // The document: the one asset_data names, rewritten in place, or a new
    // one covering every primitive (a binding document is whole-model).
    std::set<std::string> usd_paths;
    for (const auto &[key, entry] : state.current)
        if (entry.usd_path) usd_paths.insert(*entry.usd_path);
    for (auto &[key, entry] : state.current) {
        if (!entry.routing)
            refuse(method,
                   "mesh " + std::to_string(key.first) + " primitive " + std::to_string(key.second) +
                       " carries no named glTF material; bind it too",
                   bindingInvalidCode, {{"model", model}, {"reason", "a primitive has no material to keep"}});
        if (entry.usd_path) continue;
        auto usd_path = "/mesh_" + std::to_string(key.first) + "/primitive_" + std::to_string(key.second);
        for (int suffix = 2; usd_paths.contains(usd_path); ++suffix)
            usd_path = "/mesh_" + std::to_string(key.first) + "/primitive_" + std::to_string(key.second) + "_" +
                       std::to_string(suffix);
        usd_paths.insert(usd_path);
        entry.usd_path = usd_path;
    }
    OrderedJson document;
    if (state.bindings_reference) {
        document = state.bindings_json;
        auto &bindings = document.at("bindings");
        for (auto &binding : bindings) {
            const PrimitiveKey key{binding.at("mesh").get<std::uint32_t>(), binding.at("primitive").get<std::uint32_t>()};
            const auto &entry = state.current.at(key);
            binding["material"] = entry.material;
            binding["routing"] = routingJson(*entry.routing);
        }
    } else {
        document["schema"] = std::string{bindingsSchema};
        document["version"] = 1;
        document["model"] = isEnginePrimitiveReference(model)
                                ? model
                                : asset_data.json.at("models").at(*state.asset_index).at("path").get<std::string>();
        auto bindings = OrderedJson::array();
        for (const auto &[key, entry] : state.current) {
            bindings.push_back({{"usd_path", *entry.usd_path},
                                {"mesh", key.first},
                                {"primitive", key.second},
                                {"material", entry.material},
                                {"routing", routingJson(*entry.routing)}});
        }
        document["bindings"] = std::move(bindings);
    }
    try {
        (void)parsePrimitiveMaterialBindingJson(toJson(document));
    } catch (const std::exception &error) {
        refuse(method, std::string{"the bindings would not load: "} + error.what(), bindingInvalidCode,
               {{"model", model}, {"reason", error.what()}});
    }

    Json answer{{"committed", true},
                {"model", model},
                {"previous_bindings_digest", state.bindings_reference ? Json(state.bindings_digest) : Json(nullptr)},
                {"previous", {{"bindings", previous}}}};
    std::vector<ProjectCandidateDocument> documents;
    std::optional<std::string> asset_bytes;
    std::string reference;
    std::string digest;
    if (state.bindings_reference) {
        reference = *state.bindings_reference;
        auto bytes = renderJsonDocument(document, state.bindings_bytes);
        digest = projectDocumentDigest(bytes);
        if (bytes != state.bindings_bytes)
            documents.push_back(replacement(reference, state.bindings_path, state.bindings_digest, std::move(bytes)));
    } else {
        const auto base = bindingsBaseName(model);
        reference = "materials/" + base + ".bindings.json";
        for (int suffix = 2; std::filesystem::exists(paths.projectRoot() / reference); ++suffix)
            reference = "materials/" + base + "_" + std::to_string(suffix) + ".bindings.json";
        auto bytes = renderJsonDocument(document, {});
        digest = projectDocumentDigest(bytes);
        documents.push_back(creation(reference, paths.projectRoot() / reference, std::move(bytes)));
        auto asset_json = asset_data.json;
        if (state.asset_index) {
            asset_json.at("models").at(*state.asset_index)["material_bindings"] = reference;
        } else {
            if (!asset_json.contains("models") || !asset_json.at("models").is_array())
                asset_json["models"] = OrderedJson::array();
            asset_json["models"].push_back({{"name", model}, {"path", model}, {"material_bindings", reference}});
        }
        try {
            (void)parseAssetDataFormatJson(toJson(asset_json));
        } catch (const std::exception &error) {
            refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
                   {{"path", asset_data.reference}, {"reason", error.what()}});
        }
        asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
        documents.push_back(replacement(asset_data.reference, asset_data.path, asset_data.digest, *asset_bytes));
        answer["asset_data_digest"] = projectDocumentDigest(*asset_bytes);
        answer["previous_asset_data_digest"] = asset_data.digest;
    }
    answer["material_bindings"] = reference;
    answer["bindings_digest"] = digest;
    if (documents.empty()) {
        answer["changed"] = false;
        return appliedLive(std::move(answer));
    }
    commitDocuments(method, paths.projectRoot(), documents, reference);
    if (asset_bytes) publishAssetData(paths, *asset_bytes);
    answer["changed"] = true;
    LOG_INFO(logger, "material bindings of model '{}' written to '{}'", model, reference);

    if (!live)
        return appliedAtRestart(std::move(answer),
                                "a bound material is not registered in this engine; it is read when the engine starts");
    // The endpoint resolves its modules once, when it is made; a host that
    // made the reload service after that still has it.
    if (reload_service == nullptr) reload_service = FastModuleContainer::tryGet<watch::ReloadService>();
    if (reload_service == nullptr)
        return appliedAtRestart(std::move(answer),
                                "this host has no reload service; the bindings apply when the engine starts");
    auto &models = GET_MODULE(ModelAssetContainer);
    std::optional<PrimitiveMaterialBindingDocument> replaced;
    try {
        replaced = models.exchangeMaterialBindings(model, reference);
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer), std::string{"the bindings could not be read back: "} + error.what());
    }
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        const std::array keys{models.reloadKey(model)};
        reload_service->reloadModelsNow(keys);
    } catch (const std::exception &error) {
        models.restoreMaterialBindings(model, std::move(replaced));
        return appliedAtRestart(std::move(answer),
                                std::string{"the model could not be rebuilt in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// --- MI-1: register_model / unregister_model.

constexpr std::string_view modelPathInvalidCode = "model.path_invalid@1";
constexpr std::string_view modelFormatUnsupportedCode = "model.format_unsupported@1";
constexpr std::string_view modelFileMissingCode = "model.file_missing@1";
constexpr std::string_view modelPathConflictCode = "model.path_conflict@1";
constexpr std::string_view modelNameInvalidCode = "model.name_invalid@1";
constexpr std::string_view modelNameConflictCode = "model.name_conflict@1";
constexpr std::string_view modelLoadFailedCode = "model.load_failed@1";
constexpr std::string_view modelUnknownNameCode = "model.unknown@1";
constexpr std::string_view modelInUseCode = "model.in_use@1";
constexpr std::string_view modelTransactionBusyCode = "model.transaction_busy@1";

constexpr std::array<std::string_view, 3> modelExtensions{".glb", ".gltf", ".vrm"};

void commitModelDocument(std::string_view method, const std::filesystem::path &project_root,
                         const ProjectCandidateDocument &document) {
    try {
        (void)commitProjectDocuments(project_root, std::span{&document, std::size_t{1}});
    } catch (const ProjectDocumentExternalModification &error) {
        refuse(method, error.what(), externalModificationCode,
               {{"path", document.reference}, {"expected", nullptr}, {"actual", nullptr}});
    } catch (const ProjectDocumentTransactionBusy &error) {
        refuse(method, error.what(), modelTransactionBusyCode, {{"path", error.path().generic_string()}});
    }
}

// A project-relative model file as asset_data writes it (project:// taken
// off), checked for its form only.
std::string modelRelativePath(std::string_view method, const std::string &path) {
    std::string relative = path;
    if (relative.starts_with("project://")) relative = relative.substr(std::string_view{"project://"}.size());
    const auto invalid = [&](const std::string &reason, const std::string &detail) {
        refuse(method, "'" + path + "' " + detail, modelPathInvalidCode, {{"path", path}, {"reason", reason}});
    };
    if (relative.empty()) invalid("empty", "names no file");
    if (relative.find('\\') != std::string::npos) invalid("backslash", "must use forward slashes");
    if (relative.find("://") != std::string::npos || relative.front() == '/' ||
        (relative.size() > 1 && relative[1] == ':'))
        invalid("absolute", "must be a project:// or project-relative path");
    if (relative.find('#') != std::string::npos)
        invalid("fragment", "names a fragment; a model is registered by its whole file");
    for (const auto &part : std::filesystem::path{relative})
        if (part == ".." || part == ".") invalid("dot_segment", "must not contain '.' or '..' segments");
    return relative;
}

std::string lowerModelExtension(const std::string &relative) {
    auto extension = std::filesystem::path{relative}.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

void requireModelName(std::string_view method, const std::string &name) {
    const auto invalid = [&](const std::string &reason, const std::string &detail) {
        refuse(method, "model name '" + name + "' " + detail, modelNameInvalidCode,
               {{"name", name}, {"reason", reason}});
    };
    if (name.empty()) invalid("empty", "is empty");
    if (name.find('#') != std::string::npos) invalid("fragment", "must not contain '#'");
    if (name.starts_with("engine://")) invalid("engine_reference", "must not be an engine:// reference");
}

// The asset_data models[] entries, or an empty array.
const OrderedJson &listedModels(const AssetDataFile &asset_data) {
    static const OrderedJson empty = OrderedJson::array();
    const auto models = asset_data.json.find("models");
    return models != asset_data.json.end() && models->is_array() ? *models : empty;
}

Json registerModelDocument(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "register_model";
    requireOnly(method, params, {"path", "name"});
    const auto path = requireString(method, params, "path");
    if (params.contains("name") && !params.at("name").is_string()) invalidParams(method, "'name' must be a string");

    const auto relative = modelRelativePath(method, path);
    const auto extension = lowerModelExtension(relative);
    if (std::find(modelExtensions.begin(), modelExtensions.end(), extension) == modelExtensions.end()) {
        auto supported = Json::array();
        for (const auto value : modelExtensions) supported.push_back(std::string{value});
        refuse(method, "'" + relative + "' is not a .glb, .gltf or .vrm file", modelFormatUnsupportedCode,
               {{"path", relative}, {"extension", extension}, {"supported", std::move(supported)}});
    }
    try {
        (void)paths.resolveExistingFile(relative);
    } catch (const std::exception &error) {
        refuse(method, "'" + relative + "' is not a file in the project: " + error.what(), modelFileMissingCode,
               {{"path", relative}});
    }

    const auto asset_data = readAssetData(paths);
    const auto normalized = normalizedOrNull(paths, relative);
    for (const auto &entry : listedModels(asset_data)) {
        if (!entry.is_object() || !entry.contains("path") || !entry.at("path").is_string()) continue;
        auto listed = entry.at("path").get<std::string>();
        if (listed.find('#') != std::string::npos || isEnginePrimitiveReference(listed)) continue;
        if (listed.starts_with("project://")) listed = listed.substr(std::string_view{"project://"}.size());
        if (listed == relative || (normalized && normalizedOrNull(paths, listed) == normalized)) {
            const auto holder = entry.value("name", std::string{});
            refuse(method, "'" + relative + "' is registered already as model '" + holder + "'",
                   modelPathConflictCode, {{"path", relative}, {"name", holder}});
        }
    }

    const auto name = params.contains("name") ? params.at("name").get<std::string>()
                                              : std::filesystem::path{relative}.stem().generic_string();
    requireModelName(method, name);
    for (const auto &entry : listedModels(asset_data))
        if (entry.is_object() && entry.value("name", std::string{}) == name)
            refuse(method, "model '" + name + "' exists already", modelNameConflictCode, {{"name", name}});

    // Read as the engine reads it at start, before anything is written.
    try {
        ModelAssetContainer::preflightModel(name, relative);
    } catch (const std::exception &error) {
        refuse(method, "'" + relative + "' does not load: " + error.what(), modelLoadFailedCode,
               {{"path", relative}, {"message", error.what()}});
    }

    auto asset_json = asset_data.json;
    if (!asset_json.contains("models") || !asset_json.at("models").is_array()) asset_json["models"] = OrderedJson::array();
    asset_json["models"].push_back({{"name", name}, {"path", relative}});
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), modelLoadFailedCode,
               {{"path", relative}, {"message", error.what()}});
    }
    const auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    commitModelDocument(method, paths.projectRoot(),
                        replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes));
    LOG_INFO(logger, "model '{}' registered from '{}'", name, relative);

    Json answer{{"committed", true},
                {"name", name},
                {"path", relative},
                {"asset_data_digest", projectDocumentDigest(asset_bytes)},
                {"previous_asset_data_digest", asset_data.digest}};
    if (const auto reason = publishAssetData(paths, asset_bytes))
        return appliedAtRestart(std::move(answer), "the engine's copy of asset_data was not refreshed: " + *reason);
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        if (auto *models = FastModuleContainer::tryGet<ModelAssetContainer>()) {
            models->registerModel(name);
        } else {
            // Made now, from the asset_data just published: it registers
            // every listed model, this one with them.
            (void)GET_MODULE(ModelAssetContainer);
        }
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the model could not be registered in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

Json unregisterModelDocument(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "unregister_model";
    requireOnly(method, params, {"name"});
    if (!params.contains("name") || !params.at("name").is_string()) invalidParams(method, "requires a string 'name'");
    const auto name = params.at("name").get<std::string>();
    const auto primitive = [&] {
        refuse(method, "model '" + name + "' is an engine primitive; it is not unregistered", modelNameInvalidCode,
               {{"name", name}, {"reason", "engine_primitive"}});
    };
    if (name.starts_with("engine://")) primitive();

    const auto asset_data = readAssetData(paths);
    const auto &models = listedModels(asset_data);
    std::optional<std::size_t> index;
    for (std::size_t candidate = 0; candidate < models.size(); ++candidate)
        if (models.at(candidate).is_object() && models.at(candidate).value("name", std::string{}) == name)
            index = candidate;
    if (!index)
        refuse(method, "asset_data declares no model '" + name + "'", modelUnknownNameCode, {{"name", name}});
    const auto removed = models.at(*index);
    if (isEnginePrimitiveReference(removed.value("path", std::string{}))) primitive();

    // Every scene of the document as the editor holds it (prefab-generated
    // objects included), as get_material_bindings counts `objects`.
    std::size_t count = 0;
    auto objects = Json::array();
    if (const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>()) {
        for (const auto &scene : config->resolvedScene().scenes())
            for (const auto &object : scene.objects)
                for (const auto &component : object.components) {
                    if (component.name != "simplemodelview" || !component.effective_json.is_object() ||
                        component.effective_json.value("model", std::string{}) != name)
                        continue;
                    if (count++ < 8)
                        objects.push_back(object.name.value_or(
                            "#" + std::to_string(object.authoring_object_id.value)));
                    break;
                }
    }
    if (count != 0)
        refuse(method, "model '" + name + "' is drawn by " + std::to_string(count) + " scene object(s)",
               modelInUseCode, {{"name", name}, {"count", count}, {"objects", std::move(objects)}});

    auto asset_json = asset_data.json;
    asset_json.at("models").erase(*index);
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
               {{"path", asset_data.reference}, {"reason", error.what()}});
    }
    const auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    commitModelDocument(method, paths.projectRoot(),
                        replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes));
    LOG_INFO(logger, "model '{}' unregistered", name);

    Json answer{{"committed", true},
                {"name", name},
                {"removed", toJson(removed)},
                {"asset_data_digest", projectDocumentDigest(asset_bytes)},
                {"previous_asset_data_digest", asset_data.digest}};
    if (const auto reason = publishAssetData(paths, asset_bytes))
        return appliedAtRestart(std::move(answer), "the engine's copy of asset_data was not refreshed: " + *reason);
    auto *container = FastModuleContainer::tryGet<ModelAssetContainer>();
    if (container == nullptr) return appliedLive(std::move(answer));
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        if (const auto kept = container->unregisterModel(name))
            return appliedAtRestart(std::move(answer), "the engine keeps the model until it starts again: " + *kept);
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the model could not be released in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// --- EB-1: delete_material / rename_material / register_material.

constexpr std::string_view inUseCode = "material.in_use@1";
constexpr std::string_view documentSharedCode = "material.document_shared@1";

// A listed material document that holds a named material.
struct HeldMaterial {
    MaterialDocumentFile document;
    std::size_t index = 0;
};

// The listed document holding `name`, read as list_materials reads it: a
// listed document that does not read is passed over (it holds no name the
// engine could have registered).
std::optional<HeldMaterial> findHeldMaterial(const AssetDataFile &asset_data, const PathResolver &paths,
                                             const std::string &name) {
    for (const auto &reference : listedMaterialPaths(asset_data.json)) {
        HeldMaterial held;
        held.document.reference = reference;
        try {
            held.document.path = paths.resolveExistingFile(reference);
            held.document.bytes = readFileBytes(held.document.path);
            held.document.json = OrderedJson::parse(held.document.bytes);
        } catch (const std::exception &) {
            continue;
        }
        if (!held.document.json.is_object() || !held.document.json.contains("materials") ||
            !held.document.json.at("materials").is_array())
            continue;
        const auto &materials = held.document.json.at("materials");
        for (std::size_t index = 0; index < materials.size(); ++index) {
            if (!materials.at(index).is_object() || materials.at(index).value("name", std::string{}) != name) continue;
            held.document.digest = projectDocumentDigest(held.document.bytes);
            held.index = index;
            return held;
        }
    }
    return std::nullopt;
}

// `name` as a delete or a rename sends it: a material a listed document
// holds. An engine material (pelican.*) and a model's glTF material are
// refused by name; anything else is unknown.
HeldMaterial requireHeldMaterial(std::string_view method, const AssetDataFile &asset_data, const PathResolver &paths,
                                 const std::string &name) {
    if (name.starts_with("pelican.") || name.starts_with("engine://"))
        refuse(method, "material '" + name + "' is an engine material; it is not " +
                           (method == "delete_material" ? "deleted" : "renamed"),
               nameInvalidCode, {{"name", name}, {"reason", "engine_material"}});
    if (auto held = findHeldMaterial(asset_data, paths, name)) return std::move(*held);
    if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>())
        if (const auto holder = models->modelDeclaringMaterial(name))
            refuse(method, "'" + name + "' is a glTF material of model '" + *holder + "', not a project material",
                   nameInvalidCode, {{"name", name}, {"reason", "model_material"}, {"model", *holder}});
    refuse(method, "no document asset_data materials[] lists holds material '" + name + "'", nameUnknownCode,
           {{"name", name}, {"reason", "not in a listed material document"}});
}

// MT-2: the scene objects (every scene of the document as the editor holds
// it, prefab-generated ones included) whose own materials name `name`.
std::pair<std::size_t, Json> objectsOverridingWith(const std::string &name) {
    std::size_t count = 0;
    auto objects = Json::array();
    const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    if (config == nullptr) return {count, std::move(objects)};
    for (const auto &scene : config->resolvedScene().scenes())
        for (const auto &object : scene.objects)
            for (const auto &component : object.components) {
                if (component.name != "simplemodelview" || !component.effective_json.is_object()) continue;
                const auto materials = component.effective_json.find("materials");
                if (materials == component.effective_json.end() || !materials->is_object() ||
                    !materials->contains("rows") || !materials->at("rows").is_array())
                    continue;
                const auto &rows = materials->at("rows");
                const bool names = std::any_of(rows.begin(), rows.end(), [&](const auto &row) {
                    return row.is_object() && row.value("material", std::string{}) == name;
                });
                if (!names) continue;
                if (count++ < 8)
                    objects.push_back(object.name.value_or("#" + std::to_string(object.authoring_object_id.value)));
                break;
            }
    return {count, std::move(objects)};
}

std::string withoutProjectScheme(std::string_view reference) {
    if (reference.starts_with("project://")) reference.remove_prefix(std::string_view{"project://"}.size());
    return std::string{reference};
}

// Whether `json` (a render document) names the material `name` of `document`:
// a draw-loop matching key "<document>#<name>" anywhere, or a ray binding
// rule's material {document, material_name?}. `whole_document`: a rule that
// names the document without a material_name counts too (the document
// leaves asset_data).
bool rendersMaterial(const Json &json, const PathResolver &paths, const std::string &document,
                     const std::string &name, bool whole_document) {
    const auto plain = withoutProjectScheme(document);
    const auto normalized = normalizedOrNull(paths, plain);
    const auto sameDocument = [&](const std::string &candidate) {
        const auto other = withoutProjectScheme(candidate);
        return other == plain || (normalized && normalizedOrNull(paths, other) == normalized);
    };
    if (json.is_string()) {
        const auto &text = json.get_ref<const std::string &>();
        const auto hash = text.rfind('#');
        return hash != std::string::npos && text.substr(hash + 1) == name && sameDocument(text.substr(0, hash));
    }
    if (json.is_array()) {
        for (const auto &element : json)
            if (rendersMaterial(element, paths, document, name, whole_document)) return true;
        return false;
    }
    if (!json.is_object()) return false;
    const auto material = json.find("material");
    if (material != json.end() && material->is_object() && material->contains("document") &&
        material->at("document").is_string() && sameDocument(material->at("document").get<std::string>())) {
        const auto named = material->find("material_name");
        if (named == material->end() ? whole_document : (named->is_string() && *named == name)) return true;
    }
    for (const auto &[key, value] : json.items()) {
        (void)key;
        if (rendersMaterial(value, paths, document, name, whole_document)) return true;
    }
    return false;
}

// Every render document of the project (pelican.graph, pelican.render_pipeline,
// pelican.render_feature, pelican.ray_bindings; hidden and symlinked
// directories are not entered) that names the material, project-relative and
// sorted. These are written by the render document editor, not in a material
// transaction.
Json renderDocumentsNaming(const PathResolver &paths, const std::string &document, const std::string &name,
                           bool whole_document) {
    auto found = Json::array();
    if (!paths.isSetup()) return found;
    const auto root = paths.projectRoot();
    std::vector<std::string> references;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it{root, std::filesystem::directory_options::skip_permission_denied,
                                                     error};
    if (error) return found;
    for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(error)) {
        if (error) break;
        const auto &entry = *it;
        std::error_code status;
        if (entry.is_directory(status)) {
            const auto file = entry.path().filename().generic_u8string();
            if ((!file.empty() && file.front() == u8'.') || entry.is_symlink(status)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(status) || entry.path().extension() != ".json") continue;
        Json json;
        try {
            json = Json::parse(readFileBytes(entry.path()));
        } catch (const std::exception &) {
            continue;
        }
        if (!json.is_object() || !json.contains("schema") || !json.at("schema").is_string()) continue;
        const auto schema = json.at("schema").get<std::string>();
        if (schema != "pelican.graph" && schema != "pelican.render_pipeline" && schema != "pelican.render_feature" &&
            schema != "pelican.ray_bindings")
            continue;
        if (!rendersMaterial(json, paths, document, name, whole_document)) continue;
        const auto relative = entry.path().lexically_relative(root).generic_u8string();
        references.emplace_back(relative.begin(), relative.end());
    }
    std::sort(references.begin(), references.end());
    for (auto &reference : references) found.push_back(std::move(reference));
    return found;
}

Json deleteMaterial(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "delete_material";
    requireOnly(method, params, {"name"});
    const auto name = requireString(method, params, "name");
    const auto asset_data = readAssetData(paths);
    const auto held = requireHeldMaterial(method, asset_data, paths, name);
    const auto &path = held.document.reference;
    const auto names = materialNames(held.document.json);
    if (names.size() != 1)
        refuse(method,
               "'" + path + "' holds " + std::to_string(names.size()) +
                   " materials; delete_material takes a document of one material off asset_data",
               documentSharedCode, {{"name", name}, {"path", path}, {"names", names}});

    // Everything that names it, by name: nothing is written while any does.
    auto bindings = Json::array();
    for (const auto &naming : bindingsNaming(asset_data, paths, name)) bindings.push_back(naming.model);
    auto [count, objects] = objectsOverridingWith(name);
    auto render_documents = renderDocumentsNaming(paths, path, name, true);
    if (!bindings.empty() || count != 0 || !render_documents.empty()) {
        std::string where;
        if (!bindings.empty()) where += "bound by " + std::to_string(bindings.size()) + " model(s)";
        if (count != 0) where += (where.empty() ? "" : ", ") + std::string{"set on "} + std::to_string(count) +
                                 " scene object(s)";
        if (!render_documents.empty())
            where += (where.empty() ? "" : ", ") + std::string{"named by "} +
                     std::to_string(render_documents.size()) + " render document(s)";
        refuse(method, "material '" + name + "' is " + where, inUseCode,
               {{"name", name},
                {"bindings", std::move(bindings)},
                {"objects", std::move(objects)},
                {"count", count},
                {"render_documents", std::move(render_documents)}});
    }

    auto asset_json = asset_data.json;
    auto &listed = asset_json.at("materials");
    OrderedJson removed;
    for (auto entry = listed.begin(); entry != listed.end(); ++entry) {
        if (entry->is_object() && entry->value("path", std::string{}) == path) {
            removed = *entry;
            listed.erase(entry);
            break;
        }
    }
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
               {{"path", asset_data.reference}, {"reason", error.what()}});
    }
    const auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    const std::array documents{replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes)};
    commitDocuments(method, paths.projectRoot(), documents, asset_data.reference);
    LOG_INFO(logger, "material '{}' deleted ('{}' left on disk)", name, path);

    Json answer{{"committed", true},
                {"name", name},
                {"removed", toJson(removed)},
                {"asset_data_digest", projectDocumentDigest(asset_bytes)},
                {"previous_asset_data_digest", asset_data.digest}};
    if (const auto reason = publishAssetData(paths, asset_bytes))
        return appliedAtRestart(std::move(answer), "the engine's copy of asset_data was not refreshed: " + *reason);
    auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>();
    if (project == nullptr || !registeredFrom(name, path)) return appliedLive(std::move(answer));
    // Nothing in the documents names it any more; a model the engine still
    // draws with it (a runtime instance the documents do not hold) keeps it.
    if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>()) {
        const std::array ids{project->materialByName(name)};
        const auto drawing = models->modelsDrawingMaterials(ids);
        if (!drawing.empty())
            return appliedAtRestart(std::move(answer), "the engine keeps the material until it starts again: model '" +
                                                           drawing.front() + "' still draws with it");
    }
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        auto released = project->unregisterDocument(path);
        if (auto *materials = FastModuleContainer::tryGet<MaterialContainer>())
            ReleaseErrorSink::record("deleted material release",
                                     materials->releaseModelResources(std::move(released), {}, true));
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the material could not be released in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// register_material {path}: a material document already in the project,
// listed in asset_data materials[] and registered (delete_material's way
// back: its removed.path).
Json registerMaterial(const Json &params, PathResolver &paths) {
    constexpr std::string_view method = "register_material";
    requireOnly(method, params, {"path"});
    const auto relative = projectRelative(method, requireString(method, params, "path"));
    const auto asset_data = readAssetData(paths);
    if (findListedMaterial(asset_data, paths, relative))
        refuse(method, "'" + relative + "' is listed already", pathConflictCode, {{"path", relative}});
    const auto document = readMaterialDocument(method, paths, relative);
    const auto names = materialNames(document.json);
    for (const auto &name : names) {
        if (auto holder = nameHolder(name, asset_data, paths)) {
            Json fields{{"name", name}};
            fields.update(*holder);
            refuse(method, "material '" + name + "' exists already", nameConflictCode, std::move(fields));
        }
        if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>())
            if (const auto holder = models->modelDeclaringMaterial(name))
                refuse(method, "model '" + *holder + "' has a glTF material named '" + name + "'", nameConflictCode,
                       {{"name", name}, {"model", *holder}});
    }
    SurfaceCache surfaces{paths};
    checkCandidateDocument(method, relative, document.json, surfaces);

    auto asset_json = asset_data.json;
    if (!asset_json.contains("materials") || !asset_json.at("materials").is_array())
        asset_json["materials"] = OrderedJson::array();
    asset_json["materials"].push_back({{"path", relative}});
    try {
        (void)parseAssetDataFormatJson(toJson(asset_json));
    } catch (const std::exception &error) {
        refuse(method, std::string{"asset_data would not load: "} + error.what(), documentInvalidCode,
               {{"path", asset_data.reference}, {"reason", error.what()}});
    }
    const auto asset_bytes = renderJsonDocument(asset_json, asset_data.bytes);
    const std::array documents{replacement(asset_data.reference, asset_data.path, asset_data.digest, asset_bytes)};
    commitDocuments(method, paths.projectRoot(), documents, relative);
    LOG_INFO(logger, "material document '{}' registered", relative);

    Json answer{{"committed", true},
                {"path", relative},
                {"names", names},
                {"material_digest", document.digest},
                {"asset_data_digest", projectDocumentDigest(asset_bytes)},
                {"previous_asset_data_digest", asset_data.digest}};
    if (const auto reason = publishAssetData(paths, asset_bytes))
        return appliedAtRestart(std::move(answer), "the engine's copy of asset_data was not refreshed: " + *reason);
    if (FastModuleContainer::tryGet<MaterialContainer>() == nullptr)
        return appliedAtRestart(std::move(answer), "this host runs no material container");
    try {
        if (auto *core = FastModuleContainer::tryGet<VulkanManageCore>()) core->waitIdle();
        if (auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>()) {
            (void)project->registerDocument(relative);
        } else {
            (void)GET_MODULE(ProjectMaterialAssetContainer);
        }
    } catch (const std::exception &error) {
        return appliedAtRestart(std::move(answer),
                                std::string{"the material could not be registered in place: "} + error.what());
    }
    return appliedLive(std::move(answer));
}

// rename_material {name, new_name}: the material document's name and every
// model's material_bindings entry naming it, in one transaction. A scene
// object's own material (the editor journal's) and a render document (the
// render document editor's) cannot be written with them: a rename they name
// is refused, material.in_use@1.
Json renameMaterial(const Json &params, PathResolver &paths, watch::ReloadService *reload_service) {
    constexpr std::string_view method = "rename_material";
    requireOnly(method, params, {"name", "new_name"});
    const auto name = requireString(method, params, "name");
    const auto new_name = requireString(method, params, "new_name");
    if (!isIdentifier(new_name))
        refuse(method, "'" + new_name + "' is not a material name", nameInvalidCode,
               {{"name", new_name}, {"reason", "a material name is letters, digits and '_'"}});
    const auto asset_data = readAssetData(paths);
    const auto held = requireHeldMaterial(method, asset_data, paths, name);
    const auto &path = held.document.reference;
    Json answer{{"committed", true},
                {"path", path},
                {"name", new_name},
                {"previous_name", name},
                {"previous_material_digest", held.document.digest}};
    if (new_name == name) {
        answer["changed"] = false;
        answer["material_digest"] = held.document.digest;
        answer["rewritten_bindings"] = Json::array();
        return appliedLive(std::move(answer));
    }
    if (auto holder = nameHolder(new_name, asset_data, paths)) {
        Json fields{{"name", new_name}};
        fields.update(*holder);
        refuse(method, "material '" + new_name + "' exists already", nameConflictCode, std::move(fields));
    }
    if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>())
        if (const auto holder = models->modelDeclaringMaterial(new_name))
            refuse(method, "model '" + *holder + "' has a glTF material named '" + new_name + "'", nameConflictCode,
                   {{"name", new_name}, {"model", *holder}});

    auto [count, objects] = objectsOverridingWith(name);
    auto render_documents = renderDocumentsNaming(paths, path, name, false);
    if (count != 0 || !render_documents.empty()) {
        std::string where;
        if (count != 0) where += "set on " + std::to_string(count) + " scene object(s)";
        if (!render_documents.empty())
            where += (where.empty() ? "" : ", ") + std::string{"named by "} +
                     std::to_string(render_documents.size()) + " render document(s)";
        refuse(method, "material '" + name + "' is " + where + ", which a rename does not rewrite", inUseCode,
               {{"name", name},
                {"bindings", Json::array()},
                {"objects", std::move(objects)},
                {"count", count},
                {"render_documents", std::move(render_documents)}});
    }

    auto candidate = held.document.json;
    candidate.at("materials").at(held.index)["name"] = new_name;
    SurfaceCache surfaces{paths};
    checkCandidateDocument(method, path, candidate, surfaces);
    auto rebound = bindingsNaming(asset_data, paths, name);
    for (auto &bindings : rebound) {
        for (const auto entry : bindings.entries) bindings.json.at("bindings").at(entry)["material"] = new_name;
        try {
            (void)parsePrimitiveMaterialBindingJson(toJson(bindings.json));
        } catch (const std::exception &error) {
            refuse(method, "model '" + bindings.model + "' material bindings would not load: " + error.what(),
                   bindingInvalidCode, {{"model", bindings.model}, {"reason", error.what()}});
        }
    }

    auto bytes = renderJsonDocument(candidate, held.document.bytes);
    const auto digest = projectDocumentDigest(bytes);
    std::vector<ProjectCandidateDocument> documents;
    documents.push_back(replacement(path, held.document.path, held.document.digest, std::move(bytes)));
    auto rewritten = Json::array();
    for (const auto &bindings : rebound) {
        auto rebound_bytes = renderJsonDocument(bindings.json, bindings.bytes);
        rewritten.push_back({{"model", bindings.model},
                             {"material_bindings", bindings.reference},
                             {"bindings_digest", projectDocumentDigest(rebound_bytes)},
                             {"previous_bindings_digest", bindings.digest}});
        documents.push_back(replacement(bindings.reference, bindings.path, bindings.digest, std::move(rebound_bytes)));
    }
    commitDocuments(method, paths.projectRoot(), documents, path);
    answer["changed"] = true;
    answer["material_digest"] = digest;
    answer["rewritten_bindings"] = std::move(rewritten);
    LOG_INFO(logger, "material '{}' in '{}' renamed '{}'", name, path, new_name);

    if (!registeredFrom(name, path))
        return appliedAtRestart(std::move(answer), "material '" + name +
                                                       "' is not registered in this engine; it is read when the "
                                                       "engine starts");
    auto *materials = FastModuleContainer::tryGet<MaterialContainer>();
    if (materials == nullptr) return appliedAtRestart(std::move(answer), "this host runs no material container");
    // As an edit outside values (MT-3): registered anew under the new name,
    // the models drawing it rebuilt with the rewritten bindings.
    return rebuildMaterial(std::move(answer), path, new_name, std::vector<std::string>{"name"}, rebound, *materials,
                           reload_service);
}

} // namespace

void configureMaterialDocumentRpcHandlers(RpcServer &server, PathResolver &path_resolver,
                                          watch::ReloadService *reload_service) {
    server.setHandler("list_materials", [&path_resolver](const Json &params) {
        return listMaterials(params, path_resolver);
    });
    // The material object as the document holds it: its key order is the
    // document's.
    server.setOrderedHandler(
        "get_material", [&path_resolver](const Json &params) { return getMaterial(params, path_resolver); },
        [](const OrderedJson &) { return true; });
    server.setHandler("get_surface_schema", [&path_resolver](const Json &params) {
        return getSurfaceSchema(params, path_resolver);
    });
    server.setHandler("list_surfaces", [&path_resolver](const Json &params) {
        return listSurfaces(params, path_resolver);
    });
    server.setHandler("edit_material", [&path_resolver, reload_service](const Json &params) {
        return editMaterial(params, path_resolver, reload_service);
    });
    server.setHandler("create_material", [&path_resolver](const Json &params) {
        return createMaterial(params, path_resolver);
    });
    server.setHandler("get_material_bindings", [&path_resolver](const Json &params) {
        return getMaterialBindings(params, path_resolver);
    });
    server.setHandler("bind_material", [&path_resolver, reload_service](const Json &params) {
        return bindMaterial(params, path_resolver, reload_service);
    });
    // MI-1: a model file already in the project, listed in (or taken off)
    // asset_data models[] and registered in (or released from) the engine.
    server.setHandler("register_model", [&path_resolver](const Json &params) {
        return registerModelDocument(params, path_resolver);
    });
    server.setHandler("unregister_model", [&path_resolver](const Json &params) {
        return unregisterModelDocument(params, path_resolver);
    });
    // EB-1: a material taken off asset_data (and put back), and renamed.
    server.setHandler("delete_material", [&path_resolver](const Json &params) {
        return deleteMaterial(params, path_resolver);
    });
    server.setHandler("register_material", [&path_resolver](const Json &params) {
        return registerMaterial(params, path_resolver);
    });
    server.setHandler("rename_material", [&path_resolver, reload_service](const Json &params) {
        return renameMaterial(params, path_resolver, reload_service);
    });
}

} // namespace Pelican
