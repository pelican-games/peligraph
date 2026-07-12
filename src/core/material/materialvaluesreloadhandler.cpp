#include "materialvaluesreloadhandler.hpp"

#include "../log.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view documentTable = "material-document";
constexpr std::string_view surfaceFakeTable = "material-surface-fake";
constexpr std::string_view valuesTable = "material-values";

nlohmann::json readJsonFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("material values file is missing: " + path.string());
    }
    try {
        return nlohmann::json::parse(input);
    } catch (const std::exception &error) {
        throw std::runtime_error("material values JSON '" + path.string() + "': " +
                                 error.what());
    }
}

watch::AssetKey materialSource(const watch::AssetKey &document, std::string_view name) {
    auto source = document;
    if (!source.fragment.empty()) {
        throw std::runtime_error("reloadable material document key must not contain a fragment");
    }
    source.fragment = std::string{name};
    return source;
}

watch::AssetKey surfaceDependency(std::string_view reference) {
    constexpr std::string_view engine = "engine://";
    if (reference.starts_with(engine)) {
        return watch::makeAssetKey(std::string{"@engine/"} +
                                   std::string{reference.substr(engine.size())});
    }
    return watch::makeAssetKey(reference);
}

const MaterialDefinition &findMaterial(const MaterialFormatDocument &document,
                                       std::string_view name) {
    const auto found = std::find_if(document.materials.begin(), document.materials.end(),
                                    [name](const auto &material) {
                                        return material.name == name;
                                    });
    if (found == document.materials.end()) {
        throw std::runtime_error("material '" + std::string{name} +
                                 "' is missing from the reloaded document");
    }
    return *found;
}

std::string nonValuesSignature(const MaterialDefinition &material) {
    nlohmann::json signature{
        {"base_color_factor", material.base.base_color_factor},
        {"base_color_texture", material.base.base_color_texture},
        {"metallic_factor", material.base.metallic_factor},
        {"roughness_factor", material.base.roughness_factor},
        {"metallic_roughness_texture", material.base.metallic_roughness_texture},
        {"normal_texture", material.base.normal_texture},
        {"occlusion_texture", material.base.occlusion_texture},
        {"emissive_factor", material.base.emissive_factor},
        {"emissive_texture", material.base.emissive_texture},
        {"shader", material.shader},
        {"defines", material.defines},
        {"surface", material.surface},
    };
    return signature.dump();
}

} // namespace

struct MaterialValuesReloadHandler::DocumentPayload {
    watch::AssetKey key;
    std::size_t bytes = 0;
};

struct MaterialValuesReloadHandler::SurfacePayload {
    std::string reference;
    Std140Layout layout;
};

struct MaterialValuesReloadHandler::ValuesPayload {
    std::string name;
    GlobalMaterialId material;
    Std140Layout layout;
    std::vector<std::byte> values;
    bool changed = false;
};

struct MaterialValuesReloadHandler::PendingReload {
    std::optional<MaterialFormatDocument> document;
    std::map<std::string, LoweredMaterial> lowered;
    std::size_t file_bytes = 0;
};

struct MaterialValuesReloadHandler::TrackedSurface {
    watch::AssetKey dependency;
    watch::LogicalResourceRef resource;
    std::shared_ptr<const void> live_payload;
};

struct MaterialValuesReloadHandler::TrackedMaterial {
    std::string name;
    std::string surface_reference;
    std::string non_values_signature;
    GlobalMaterialId material;
    watch::LogicalResourceRef resource;
    watch::LogicalResourceRef surface_resource;
    watch::AssetKey surface_dependency;
    std::shared_ptr<const void> live_payload;
};

struct MaterialValuesReloadHandler::TrackedFile {
    std::filesystem::path path;
    MaterialSurfaceCatalog surfaces;
    watch::LogicalResourceRef document_resource;
    std::shared_ptr<const void> document_payload;
    std::vector<TrackedMaterial> materials;
};

MaterialValuesReloadHandler::MaterialValuesReloadHandler(
    MaterialContainer &materials, watch::ReloadCoordinator &coordinator)
    : materials_{materials}, coordinator_{coordinator} {}

MaterialValuesReloadHandler::~MaterialValuesReloadHandler() = default;

void MaterialValuesReloadHandler::track(
    const watch::AssetKey &key, std::filesystem::path path,
    MaterialSurfaceCatalog surfaces,
    std::span<const MaterialContainer::ReloadableMaterialValuesBinding> bindings) {
    if (files_.contains(key)) {
        throw std::runtime_error("reloadable material document is already tracked: " +
                                 watch::assetKeyString(key));
    }
    if (bindings.empty()) {
        throw std::runtime_error("reloadable material document requires at least one binding");
    }

    const auto parsed = parseMaterialFormatJson(readJsonFile(path), surfaces);
    auto document_payload = std::make_shared<DocumentPayload>(
        DocumentPayload{key, static_cast<std::size_t>(std::filesystem::file_size(path))});
    auto document_resource = coordinator_.registry().declareResource(
        std::string{documentTable}, key, document_payload, {key}, 0,
        document_payload->bytes);

    TrackedFile tracked{std::move(path), std::move(surfaces),
                        std::move(document_resource), document_payload, {}};
    tracked.materials.reserve(bindings.size());
    std::set<std::string> names;
    for (const auto &binding : bindings) {
        if (!names.insert(binding.name).second) {
            throw std::runtime_error("duplicate reloadable material binding: " + binding.name);
        }
        const auto &definition = findMaterial(parsed, binding.name);
        if (!definition.surface) {
            throw std::runtime_error("material '" + binding.name +
                                     "' has no surface for values reload");
        }
        const auto surface = tracked.surfaces.find(*definition.surface);
        if (surface == tracked.surfaces.end()) {
            throw std::runtime_error("material '" + binding.name + "' surface '" +
                                     *definition.surface + "' was not provided");
        }
        const auto lowered = lowerMaterial(definition, surface->second);
        if (!materials_.materialValuesLayoutMatches(binding.material,
                                                    lowered.values_layout)) {
            throw std::runtime_error("material '" + binding.name +
                                     "' registered layout does not match its source");
        }

        const auto dependency = surfaceDependency(*definition.surface);
        auto fake = surfaces_.find(dependency);
        if (fake == surfaces_.end()) {
            auto payload = std::make_shared<SurfacePayload>(
                SurfacePayload{*definition.surface, lowered.values_layout});
            auto resource = coordinator_.registry().declareResource(
                std::string{surfaceFakeTable}, dependency, payload, {dependency}, 0, 0);
            fake = surfaces_.emplace(dependency,
                                     TrackedSurface{dependency, std::move(resource), payload})
                       .first;
        }

        const auto &live_values = materials_.materials.get(binding.material).custom_values;
        if (live_values.size() != lowered.values_layout.size) {
            throw std::runtime_error("material '" + binding.name +
                                     "' registered values do not match its layout size");
        }
        auto payload = std::make_shared<ValuesPayload>(ValuesPayload{
            binding.name, binding.material, lowered.values_layout, live_values, false});
        const auto source = materialSource(key, binding.name);
        auto resource = coordinator_.registry().declareResource(
            std::string{valuesTable}, source, payload, {key, dependency}, 0,
            payload->values.size());
        tracked.materials.push_back(TrackedMaterial{
            binding.name, *definition.surface, nonValuesSignature(definition), binding.material,
            std::move(resource), fake->second.resource, dependency, payload});
    }
    files_.emplace(key, std::move(tracked));
}

bool MaterialValuesReloadHandler::enqueue(const watch::ReloadRequest &request,
                                          watch::ReloadCoordinator &coordinator) {
    if (&coordinator != &coordinator_) {
        throw std::runtime_error("material values reload used a different coordinator");
    }
    const auto found = files_.find(request.key);
    if (found == files_.end()) return false;

    const auto &tracked = found->second;
    const auto snapshot = coordinator_.registry().snapshot();
    const auto current_document = snapshot.find(tracked.document_resource);
    if (!current_document) throw std::runtime_error("material document resource is stale");
    auto pending = std::make_shared<PendingReload>();

    watch::ReloadTransactionGroup group{"material values " +
                                        watch::assetKeyString(request.key)};
    group.add(watch::ReloadActor{
        .name = "material document",
        .target = tracked.document_resource,
        .parse = [pending, request, path = tracked.path, surfaces = tracked.surfaces] {
            if (request.kind == watch::ReloadKind::removed) {
                throw std::runtime_error("material values file is missing: " + path.string());
            }
            pending->document = parseMaterialFormatJson(readJsonFile(path), surfaces);
            pending->file_bytes = static_cast<std::size_t>(std::filesystem::file_size(path));
        },
        .stage = [pending, key = request.key] {
            return watch::StagedResourceData{
                std::make_shared<DocumentPayload>(DocumentPayload{key, pending->file_bytes}),
                {key}, 0, pending->file_bytes};
        },
    });

    std::set<watch::LogicalResourceRef> fake_actors;
    for (const auto &material : tracked.materials) {
        if (!fake_actors.insert(material.surface_resource).second) continue;
        const auto current_surface = snapshot.find(material.surface_resource);
        if (!current_surface) throw std::runtime_error("fake surface dependency is stale");
        group.add(watch::ReloadActor{
            .name = "surface compatibility (HR1-M fake)",
            .target = material.surface_resource,
            .after = {tracked.document_resource},
            .stage = [current_surface] {
                return watch::StagedResourceData{
                    current_surface->payload, current_surface->dependencies,
                    current_surface->compatibility_revision, current_surface->live_bytes};
            },
        });
    }

    for (const auto &material : tracked.materials) {
        const auto current = snapshot.find(material.resource);
        if (!current) throw std::runtime_error("material values resource is stale");
        group.add(watch::ReloadActor{
            .name = "material values " + material.name,
            .target = material.resource,
            .after = {tracked.document_resource, material.surface_resource},
            .validate = [this, pending, material, surfaces = tracked.surfaces] {
                if (!pending->document) {
                    throw std::runtime_error("material document candidate was not parsed");
                }
                const auto &definition = findMaterial(*pending->document, material.name);
                if (!definition.surface || *definition.surface != material.surface_reference) {
                    throw std::runtime_error("material '" + material.name +
                                             "' changed surface/layout; HR2-S transaction required");
                }
                if (nonValuesSignature(definition) != material.non_values_signature) {
                    throw std::runtime_error(
                        "material '" + material.name +
                        "' changed fields outside values; HR1-M only updates values");
                }
                const auto surface = surfaces.find(*definition.surface);
                if (surface == surfaces.end()) {
                    throw std::runtime_error("material '" + material.name +
                                             "' surface is unavailable");
                }
                auto lowered = lowerMaterial(definition, surface->second);
                if (!materials_.materialValuesLayoutMatches(material.material,
                                                            lowered.values_layout)) {
                    throw std::runtime_error("material '" + material.name +
                                             "' changed values layout; HR2-S transaction required");
                }
                pending->lowered.insert_or_assign(material.name, std::move(lowered));
            },
            .stage = [pending, material, current, key = request.key] {
                const auto candidate = pending->lowered.find(material.name);
                if (candidate == pending->lowered.end()) {
                    throw std::runtime_error("material '" + material.name +
                                             "' candidate was not lowered");
                }
                const auto previous = current->payloadAs<ValuesPayload>();
                auto payload = std::make_shared<ValuesPayload>(ValuesPayload{
                    material.name, material.material, candidate->second.values_layout,
                    candidate->second.values,
                    previous->values != candidate->second.values});
                return watch::StagedResourceData{
                    payload, {key, material.surface_dependency},
                    current->compatibility_revision, payload->values.size()};
            },
        });
    }

    coordinator_.enqueue(std::move(group));
    return true;
}

bool MaterialValuesReloadHandler::retire(std::shared_ptr<const void> payload,
                                         watch::ReloadCoordinator &coordinator) noexcept {
    if (&coordinator != &coordinator_) return false;
    for (auto &[key, tracked] : files_) {
        for (auto &material : tracked.materials) {
            if (material.live_payload.get() != payload.get()) continue;
            try {
                const auto current = coordinator_.registry().snapshot().find(material.resource);
                if (!current) return false;
                const auto next = current->payloadAs<ValuesPayload>();
                if (next->changed) {
                    materials_.updateMaterialValues(next->material, next->layout, next->values);
                }
                material.live_payload = current->payload;
                return true;
            } catch (const std::exception &error) {
                if (logger) {
                    LOG_ERROR(logger, "material values reload retirement failed: {}", error.what());
                }
                return true;
            } catch (...) {
                if (logger) {
                    LOG_ERROR(logger,
                              "material values reload retirement failed with unknown error");
                }
                return true;
            }
        }
    }
    return false;
}

} // namespace Pelican
