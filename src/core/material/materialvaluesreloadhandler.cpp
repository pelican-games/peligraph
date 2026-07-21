#include "materialvaluesreloadhandler.hpp"

#include "../log.hpp"
#include "../vkcore/core.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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
        {"render_path", materialRenderPathName(material.render_path)},
        {"pass", material.exact_pass},
    };
    signature["texture_overrides"] = nlohmann::json::array();
    for (const auto &texture : material.texture_overrides) {
        signature["texture_overrides"].push_back({
            {"name", texture.name},
            {"reference", texture.reference},
        });
    }
    if (material.routing) {
        signature["routing"] = {
            {"alpha_mode", materialAlphaModeName(material.routing->alpha_mode)},
            {"double_sided", material.routing->double_sided},
        };
    } else {
        signature["routing"] = nullptr;
    }
    return signature.dump();
}

bool sameLayout(const Std140Layout &left, const Std140Layout &right) {
    if (left.size != right.size || left.alignment != right.alignment ||
        left.members.size() != right.members.size()) return false;
    for (std::size_t index = 0; index < left.members.size(); ++index) {
        const auto &a = left.members[index];
        const auto &b = right.members[index];
        if (a.name != b.name || a.type != b.type || a.offset != b.offset ||
            a.size != b.size || a.alignment != b.alignment) return false;
    }
    return true;
}

bool sameTextureContract(const SurfaceTextureDefinition &left,
                         const SurfaceTextureDefinition &right) {
    return left.name == right.name &&
           left.default_reference == right.default_reference &&
           left.color_space == right.color_space && left.role == right.role;
}

bool materialBindingContractMatches(const SurfaceFormatDocument &left,
                                    const SurfaceFormatDocument &right) {
    if (left.language != right.language || left.screen_inputs != right.screen_inputs ||
        left.textures.size() != right.textures.size()) return false;
    for (std::size_t index = 0; index < left.textures.size(); ++index) {
        if (!sameTextureContract(left.textures[index], right.textures[index])) return false;
    }
    const auto &a = left.render_state;
    const auto &b = right.render_state;
    return a.blend == b.blend && a.cull == b.cull && a.depth_test == b.depth_test &&
           a.depth_write == b.depth_write && a.depth_compare == b.depth_compare;
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
    SurfaceFormatDocument document;
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
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    DeferredMaterialModel deferred_model = DeferredMaterialModel::standard_pbr_v1;
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
                                     TrackedSurface{dependency, std::move(resource), payload,
                                                    surface->second})
                       .first;
        }

        const auto &live_values = materials_.materials.get(binding.material).custom_values;
        const auto &live_material = materials_.materials.get(binding.material);
        if (live_material.route != lowered.route) {
            throw std::runtime_error("material '" + binding.name +
                                     "' registered route does not match its source");
        }
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
            std::move(resource), fake->second.resource, dependency, payload,
            lowered.route, lowered.deferred_eligibility.model});
    }
    files_.emplace(key, std::move(tracked));
}

bool MaterialValuesReloadHandler::handles(const watch::AssetKey &key) const {
    return files_.contains(key);
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
                if (lowered.route != material.route ||
                    lowered.deferred_eligibility.model != material.deferred_model) {
                    throw std::runtime_error(
                        "material '" + material.name +
                        "' changed render route/model; shader and pipeline reload transaction required");
                }
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

std::function<void()> MaterialValuesReloadHandler::prepareSurfaceReload(
    const std::map<watch::AssetKey, SurfaceFormatDocument> &surface_documents,
    std::span<const watch::AssetKey> material_documents) {
    struct SurfaceUpdate {
        TrackedSurface *tracked = nullptr;
        SurfaceFormatDocument document;
        std::shared_ptr<SurfacePayload> payload;
    };
    struct FileUpdate {
        TrackedFile *tracked = nullptr;
        MaterialSurfaceCatalog surfaces;
        std::shared_ptr<DocumentPayload> document_payload;
    };
    struct MaterialUpdate {
        TrackedMaterial *tracked = nullptr;
        std::shared_ptr<ValuesPayload> payload;
        std::array<std::uint32_t, materialCustomValueCapacity / 4> packed{};
    };
    struct Candidate {
        bool committed = false;
        std::vector<watch::StagedResource> staged;
        std::vector<SurfaceUpdate> surface_updates;
        std::vector<FileUpdate> file_updates;
        std::vector<MaterialUpdate> material_updates;
    };

    std::set<watch::AssetKey> requested_documents{material_documents.begin(),
                                                   material_documents.end()};
    for (const auto &key : requested_documents) {
        if (!files_.contains(key)) {
            throw std::runtime_error("material reload document is not tracked: " +
                                     watch::assetKeyString(key));
        }
    }

    auto candidate = std::make_shared<Candidate>();
    const auto snapshot = coordinator_.registry().snapshot();
    for (const auto &[key, document] : surface_documents) {
        const auto found = surfaces_.find(key);
        if (found == surfaces_.end()) continue;
        if (!materialBindingContractMatches(found->second.document, document)) {
            throw std::runtime_error(
                "surface '" + watch::assetKeyString(key) +
                "' changed texture, screen-input, or render-state bindings; "
                "live descriptor migration is required");
        }
        const auto current = snapshot.find(found->second.resource);
        if (!current) throw std::runtime_error("surface dependency resource is stale");
        const auto previous = current->payloadAs<SurfacePayload>();
        const auto layout = makeSurfaceStd140Layout(document);
        auto payload = std::make_shared<SurfacePayload>(
            SurfacePayload{previous->reference, layout});
        const auto compatibility = current->compatibility_revision +
            (sameLayout(previous->layout, layout) ? 0u : 1u);
        candidate->staged.push_back(watch::StagedResource{
            found->second.resource, payload, {key}, compatibility, 0});
        candidate->surface_updates.push_back(
            SurfaceUpdate{&found->second, document, std::move(payload)});
    }

    for (auto &[file_key, tracked] : files_) {
        bool affected = requested_documents.contains(file_key);
        if (!affected) {
            affected = std::ranges::any_of(tracked.materials, [&](const auto &material) {
                return surface_documents.contains(material.surface_dependency);
            });
        }
        if (!affected) continue;

        auto next_surfaces = tracked.surfaces;
        for (auto &[reference, document] : next_surfaces) {
            const auto replacement = surface_documents.find(surfaceDependency(reference));
            if (replacement != surface_documents.end()) document = replacement->second;
        }

        const auto parsed = parseMaterialFormatJson(readJsonFile(tracked.path), next_surfaces);
        const auto bytes = static_cast<std::size_t>(std::filesystem::file_size(tracked.path));
        auto document_payload = std::make_shared<DocumentPayload>(DocumentPayload{file_key, bytes});
        const auto current_document = snapshot.find(tracked.document_resource);
        if (!current_document) throw std::runtime_error("material document resource is stale");
        candidate->staged.push_back(watch::StagedResource{
            tracked.document_resource, document_payload, {file_key},
            current_document->compatibility_revision, bytes});
        candidate->file_updates.push_back(
            FileUpdate{&tracked, next_surfaces, std::move(document_payload)});

        for (auto &material : tracked.materials) {
            const auto &definition = findMaterial(parsed, material.name);
            if (!definition.surface || *definition.surface != material.surface_reference) {
                throw std::runtime_error("material '" + material.name +
                                         "' changed its surface reference");
            }
            if (nonValuesSignature(definition) != material.non_values_signature) {
                throw std::runtime_error("material '" + material.name +
                                         "' changed fields outside values");
            }
            const auto found_surface = next_surfaces.find(*definition.surface);
            if (found_surface == next_surfaces.end()) {
                throw std::runtime_error("material '" + material.name +
                                         "' surface is unavailable");
            }
            auto lowered = lowerMaterial(definition, found_surface->second);
            if (lowered.route != material.route ||
                lowered.deferred_eligibility.model != material.deferred_model) {
                throw std::runtime_error(
                    "material '" + material.name +
                    "' changed render route/model; shader and pipeline reload transaction required");
            }
            if (lowered.values.size() != lowered.values_layout.size ||
                lowered.values.size() > materialCustomValueCapacity) {
                throw std::runtime_error("material '" + material.name +
                                         "' values exceed the live material buffer contract");
            }
            const auto current = snapshot.find(material.resource);
            if (!current) throw std::runtime_error("material values resource is stale");
            const auto previous = current->payloadAs<ValuesPayload>();
            const bool layout_changed = !sameLayout(previous->layout, lowered.values_layout);
            const bool changed = layout_changed || previous->values != lowered.values;
            auto payload = std::make_shared<ValuesPayload>(ValuesPayload{
                material.name, material.material, lowered.values_layout,
                lowered.values, changed});
            candidate->staged.push_back(watch::StagedResource{
                material.resource, payload, {file_key, material.surface_dependency},
                current->compatibility_revision + (layout_changed ? 1u : 0u),
                payload->values.size()});
            MaterialUpdate update{&material, payload, {}};
            if (!payload->values.empty()) {
                std::memcpy(update.packed.data(), payload->values.data(),
                            payload->values.size());
            }
            candidate->material_updates.push_back(std::move(update));
        }
    }

    if (candidate->staged.empty()) return {};
    return [this, candidate] {
        if (candidate->committed) {
            throw std::runtime_error("surface/material reload candidate was already committed");
        }

        const bool writes_gpu = std::ranges::any_of(
            candidate->material_updates,
            [](const auto &update) { return update.payload->changed; });
        if (writes_gpu) GET_MODULE(VulkanManageCore).waitIdle();
        for (const auto &update : candidate->material_updates) {
            if (!update.payload->changed) continue;
            const auto offset = sizeof(MaterialGpuData) * update.payload->material.value +
                                offsetof(MaterialGpuData, custom_values);
            GET_MODULE(VulkanManageCore).writeBuf(
                materials_.material_buffer, update.packed.data(), offset,
                sizeof(update.packed));
        }

        (void)coordinator_.registry().publish(candidate->staged);
        for (auto &update : candidate->surface_updates) {
            update.tracked->document = std::move(update.document);
            update.tracked->live_payload = update.payload;
        }
        for (auto &update : candidate->file_updates) {
            update.tracked->surfaces = std::move(update.surfaces);
            update.tracked->document_payload = update.document_payload;
        }
        for (auto &update : candidate->material_updates) {
            auto &live = materials_.materials.get(update.payload->material);
            live.custom_values_layout = update.payload->layout;
            live.custom_values = update.payload->values;
            update.tracked->live_payload = update.payload;
        }
        candidate->committed = true;
    };
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
