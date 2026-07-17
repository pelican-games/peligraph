#include "model.hpp"

#include "../animation/animationservice.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../model/gltf.hpp"
#include "../parallel_prepare.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../startup.hpp"
#include "../watch/assetkey.hpp"
#include "../watch/reloadqueue.hpp"
#include "../watch/reloadtransaction.hpp"
#include "../../project/materialformat.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {
namespace {

constexpr std::string_view modelTemplateTable = "model-template";

struct ModelDeclaration {
    std::string name;
    std::string reference;
    std::filesystem::path path;
    std::optional<AssetFragmentRef> fragment;
    watch::AssetKey container_key;
    bool ascii = false;
    bool scene_node_instance = false;
    std::optional<PrimitiveMaterialBindingDocument> material_bindings;
};

std::string lowerExtension(const std::filesystem::path &path) {
    auto result = path.extension().string();
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string fragmentText(const std::optional<AssetFragmentRef> &fragment) {
    return fragment ? fragment->kind + "/" + fragment->path : std::string{};
}

watch::AssetKey logicalModelSource(std::string_view name) {
    return {"@runtime/model/" + std::string{name}, {}};
}

std::size_t fileBytes(const std::filesystem::path &path) {
    std::error_code error;
    const auto value = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::size_t>(value);
}

bool sameRigLayout(const std::shared_ptr<SkeletalModelData> &left,
                   const std::shared_ptr<SkeletalModelData> &right) {
    if (static_cast<bool>(left) != static_cast<bool>(right)) return false;
    if (!left) return true;
    if (left->nodes.size() != right->nodes.size() ||
        left->joint_nodes != right->joint_nodes ||
        left->skin_bindings.size() != right->skin_bindings.size())
        return false;
    for (std::size_t index = 0; index < left->nodes.size(); ++index) {
        if (left->nodes[index].parent != right->nodes[index].parent ||
            left->nodes[index].name != right->nodes[index].name)
            return false;
    }
    for (std::size_t index = 0; index < left->skin_bindings.size(); ++index) {
        const auto &a = left->skin_bindings[index];
        const auto &b = right->skin_bindings[index];
        if (a.palette_offset != b.palette_offset || a.joint_count != b.joint_count)
            return false;
    }
    return true;
}

PreparedGltf prepareDeclaration(const ModelDeclaration &declaration) {
    // Preparation is used by startup worker threads as well as the frame
    // owner. GltfLoader is stateless; constructing it locally avoids asking
    // the module graph for a first initialization while ModelAssetContainer's
    // constructor is waiting for those workers.
    GltfLoader loader;
    if (declaration.ascii)
        return loader.prepareGltf(declaration.path.string(), declaration.fragment);
    if (declaration.scene_node_instance && declaration.fragment)
        return loader.prepareGltfBinarySceneNode(declaration.path.string(),
                                                 *declaration.fragment);
    return loader.prepareGltfBinary(declaration.path.string(), declaration.fragment);
}

PrimitiveMaterialBindingDocument loadMaterialBindings(std::string_view reference,
                                                       std::string_view model_name,
                                                       const std::filesystem::path &model_path) {
    auto &resolver = GET_MODULE(PathResolver);
    const auto resolved = resolver.resolveExistingFileReference(reference);
    const auto *path = std::get_if<std::filesystem::path>(&resolved);
    if (path == nullptr) {
        if (const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
            throw std::runtime_error("model '" + std::string{model_name} +
                                     "' material bindings '" + std::string{reference} +
                                     "' must not contain fragment #" +
                                     fragment->fragment.kind + "/" +
                                     fragment->fragment.path);
        }
        throw std::runtime_error("model '" + std::string{model_name} +
                                 "' material bindings '" + std::string{reference} +
                                 "' must resolve to a project file");
    }
    std::ifstream input{*path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("model '" + std::string{model_name} +
                                 "' material bindings file is missing: " + path->string());
    }
    try {
        auto document = parsePrimitiveMaterialBindingJson(nlohmann::json::parse(input));
        const auto declared_model = resolver.resolveExistingFile(document.model);
        std::error_code equivalent_error;
        if (!std::filesystem::equivalent(declared_model, model_path,
                                         equivalent_error) || equivalent_error) {
            throw std::runtime_error("declares model '" + document.model +
                                     "' but asset resolves to '" + model_path.string() + "'");
        }
        return document;
    } catch (const std::exception &error) {
        throw std::runtime_error("model '" + std::string{model_name} +
                                 "' material bindings '" + std::string{reference} +
                                 "': " + error.what());
    }
}

void applyDeclarationBindings(ModelTemplate &model,
                              const ModelDeclaration &declaration) {
    if (!declaration.material_bindings) return;
    const auto fragment = fragmentText(declaration.fragment);
    applyPrimitiveMaterialBindings(
        model, *declaration.material_bindings, declaration.name,
        fragment.empty() ? std::nullopt
                         : std::optional<std::string_view>{fragment});
}

ModelDeclaration resolveDeclaration(std::string name, std::string reference,
                                    bool scene_node_instance,
                                    std::optional<std::string> material_bindings = std::nullopt) {
    const auto parsed = parsePathReference(reference);
    auto &resolver = GET_MODULE(PathResolver);
    std::filesystem::path physical;
    std::optional<AssetFragmentRef> fragment = parsed.fragment;
    watch::AssetKey container_key;
    const std::filesystem::path parsed_path{parsed.path};
    // ProjectBasicConfig resolves model references to physical paths before
    // exposing assetDataJson. On Windows, preserve the root-name check as
    // well as is_absolute() so a canonical drive path never falls back into
    // the project-reference parser (which correctly rejects backslashes).
    if (parsed_path.is_absolute() || parsed_path.has_root_name()) {
        if (parsed_path.has_root_name() && !parsed_path.has_root_directory())
            throw std::runtime_error("drive-relative model paths are not supported: " +
                                     parsed.path);
        std::error_code canonical_error;
        physical = std::filesystem::weakly_canonical(parsed_path, canonical_error);
        if (canonical_error)
            throw std::runtime_error("failed to normalize model path: " + parsed.path);

        const auto keyUnder = [&](const std::filesystem::path &root,
                                  std::string_view mount) -> std::optional<watch::AssetKey> {
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(physical, root, relative_error);
            if (relative_error || relative.empty() || relative.is_absolute()) return std::nullopt;
            for (const auto &component : relative)
                if (component == "..") return std::nullopt;
            return watch::makeAssetKey(mount, relative);
        };
        for (const auto &store : resolver.stores()) {
            if (const auto key = keyUnder(store.root, store.mount)) {
                container_key = *key;
                break;
            }
        }
        if (container_key.path.empty()) {
            const auto key = keyUnder(resolver.projectRoot(), {});
            if (!key)
                throw std::runtime_error("model path is outside watched project stores: " +
                                         physical.string());
            container_key = *key;
        }
    } else {
        const auto resolved = resolver.resolveExistingFileReference(reference);
        if (const auto *path = std::get_if<std::filesystem::path>(&resolved)) {
            physical = *path;
        } else if (const auto *with_fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
            physical = with_fragment->path;
            fragment = with_fragment->fragment;
        } else {
            throw std::runtime_error("model reference does not resolve to a file: " + reference);
        }
        container_key = watch::makeAssetKey(parsed.path);
    }
    const auto extension = lowerExtension(physical);
    // .vrm is a binary glTF container and has always been accepted by the
    // binary loader path (projects/example の AliciaSolid.vrm が現行利用者).
    if (extension != ".glb" && extension != ".gltf" && extension != ".vrm")
        throw std::runtime_error("model reference requires a .glb/.gltf/.vrm file: " + reference);
    auto declaration = ModelDeclaration{
        .name = std::move(name),
        .reference = std::move(reference),
        .path = std::move(physical),
        .fragment = std::move(fragment),
        .container_key = std::move(container_key),
        .ascii = extension == ".gltf",
        .scene_node_instance = scene_node_instance,
    };
    if (material_bindings) {
        declaration.material_bindings =
            loadMaterialBindings(*material_bindings, declaration.name, declaration.path);
    }
    return declaration;
}

} // namespace

struct ModelAssetContainer::Impl {
    struct PayloadTracker {
        std::unordered_set<const void *> values;
    };
    struct CommitBatch {
        std::vector<std::string> names;
        bool instance_capacity_checked = false;
        bool installed = false;
    };
    struct ModelPayload {
        std::string name;
        ModelTemplate candidate;
        std::shared_ptr<CommitBatch> batch;
        std::shared_ptr<PayloadTracker> tracker;
        bool owns_candidate = false;

        ~ModelPayload() {
            if (tracker) tracker->values.erase(this);
            if (owns_candidate) releaseModelGpuResources(candidate, false);
        }
    };
    struct Record {
        ModelDeclaration declaration;
        ModelTemplate model;
        watch::LogicalResourceRef logical;
        std::shared_ptr<const void> live_payload;
        bool declared = false;
    };
    struct PendingCandidate {
        PreparedGltf prepared;
        ModelTemplate preview;
        bool rig_changed = false;
    };

    std::unordered_map<std::string, Record> records;
    std::map<watch::AssetKey, std::set<std::string>> by_container;
    watch::ReloadCoordinator *coordinator = nullptr;
    std::shared_ptr<PayloadTracker> tracker = std::make_shared<PayloadTracker>();
    std::uint64_t next_asset_identity = 1;

    std::shared_ptr<ModelPayload> makePayload(std::string name) {
        auto payload = std::make_shared<ModelPayload>();
        payload->name = std::move(name);
        payload->tracker = tracker;
        tracker->values.insert(payload.get());
        return payload;
    }

    void declare(Record &record) {
        if (record.declared || coordinator == nullptr) return;
        auto payload = makePayload(record.declaration.name);
        record.logical = coordinator->registry().declareResource(
            std::string{modelTemplateTable}, logicalModelSource(record.declaration.name), payload,
            {record.declaration.container_key}, record.model.compatibility_revision,
            fileBytes(record.declaration.path));
        record.live_payload = std::move(payload);
        record.declared = true;
    }

    void add(ModelDeclaration declaration, ModelTemplate model) {
        if (records.contains(declaration.name))
            throw std::runtime_error("duplicate model asset name: " + declaration.name);
        model.asset_id = ModelAssetId{next_asset_identity++};
        model.content_revision = 1;
        model.compatibility_revision = 0;
        const auto name = declaration.name;
        const auto key = declaration.container_key;
        auto [found, inserted] = records.emplace(
            name, Record{std::move(declaration), std::move(model), {}, {}, false});
        if (!inserted) throw std::runtime_error("failed to register model asset: " + name);
        by_container[key].insert(name);
        declare(found->second);
    }

    void installBatch(const std::shared_ptr<CommitBatch> &batch) {
        if (!batch || batch->installed) return;
        struct Installation {
            Record *record{};
            std::shared_ptr<ModelPayload> payload;
            std::shared_ptr<const void> registry_payload;
        };
        std::vector<Installation> installs;
        installs.reserve(batch->names.size());
        const auto snapshot = coordinator->registry().snapshot();
        std::vector<ModelInstanceRebuild> rebuilds;
        for (const auto &name : batch->names) {
            auto &record = records.at(name);
            const auto current = snapshot.find(record.logical);
            if (!current || !tracker->values.contains(current->payload.get()))
                throw std::runtime_error("published model reload payload is missing");
            auto payload = std::static_pointer_cast<const ModelPayload>(current->payload);
            auto mutable_payload = std::const_pointer_cast<ModelPayload>(payload);
            if (mutable_payload->batch.get() != batch.get() || !mutable_payload->owns_candidate)
                throw std::runtime_error("published model reload batch is incomplete");
            rebuilds.push_back({record.model.asset_id, &mutable_payload->candidate});
            installs.push_back({&record, std::move(mutable_payload), current->payload});
        }

        if (auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
            if (!instances->canRebuildModelInstances(rebuilds))
                throw std::runtime_error("Render command capacity exceeded by model reload batch");
            instances->rebuildModelInstances(rebuilds);
        }

        std::vector<ModelTemplate> retired;
        retired.reserve(installs.size());
        for (auto &install : installs) {
            retired.push_back(std::move(install.record->model));
            install.record->model = std::move(install.payload->candidate);
            install.payload->owns_candidate = false;
            install.record->live_payload = std::move(install.registry_payload);
        }
        batch->installed = true;
        for (std::size_t index = 0; index < installs.size(); ++index) {
            const auto *previous = retired[index].skeletal.get();
            const auto *replacement = installs[index].record->model.skeletal.get();
            if (previous || replacement) {
                Animation::animationServiceRuntime().reloadAsset(previous,
                                                                 replacement);
            }
        }
        for (auto &model : retired) releaseModelGpuResources(model, true);
    }
};

ModelAssetContainer::ModelAssetContainer() : impl_{std::make_unique<Impl>()} {
    StartupPhaseTimer startup_timer{&StartupMetrics::addModels};
    const auto assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    std::vector<ModelDeclaration> declarations;
    declarations.reserve(assets.size());
    for (const auto &asset : assets) {
        std::optional<std::string> material_bindings;
        if (const auto found = asset.find("material_bindings"); found != asset.end()) {
            if (!found->is_string()) {
                throw std::runtime_error("model '" +
                                         asset.value("name", std::string{"<unnamed>"}) +
                                         "' material_bindings must be a string reference");
            }
            material_bindings = found->get<std::string>();
        }
        declarations.push_back(resolveDeclaration(asset.at("name").get<std::string>(),
                                                  asset.at("path").get<std::string>(), false,
                                                  std::move(material_bindings)));
    }
    const auto prepared = parallelPrepareOrdered<PreparedGltf>(
        declarations.size(),
        [&](std::size_t index) { return prepareDeclaration(declarations[index]); }, 4);
    auto &loader = GET_MODULE(GltfLoader);
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        auto model = loader.commit(prepared[index]);
        applyDeclarationBindings(model, declarations[index]);
        impl_->add(std::move(declarations[index]), std::move(model));
    }
}

ModelAssetContainer::~ModelAssetContainer() {
    if (!impl_) return;
    for (auto &[_, record] : impl_->records) releaseModelGpuResources(record.model, false);
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) {
    if (const auto found = impl_->records.find(name); found != impl_->records.end())
        return found->second.model;

    const auto parsed = parsePathReference(name);
    if (!parsed.fragment) throw std::out_of_range("unknown model asset: " + name);
    auto declaration = resolveDeclaration(name, name, true);
    auto prepared = prepareDeclaration(declaration);
    auto model = GET_MODULE(GltfLoader).commit(std::move(prepared));
    impl_->add(std::move(declaration), std::move(model));
    return impl_->records.at(name).model;
}

void ModelAssetContainer::attachReloadCoordinator(watch::ReloadCoordinator &coordinator) {
    if (impl_->coordinator != nullptr && impl_->coordinator != &coordinator)
        throw std::runtime_error("ModelAssetContainer cannot change reload coordinator");
    impl_->coordinator = &coordinator;
    for (auto &[_, record] : impl_->records) impl_->declare(record);
}

bool ModelAssetContainer::handlesReload(const watch::AssetKey &key) const {
    return impl_->by_container.contains(key);
}

bool ModelAssetContainer::enqueueReload(const watch::ReloadRequest &request,
                                        watch::ReloadCoordinator &coordinator) {
    if (&coordinator != impl_->coordinator) throw std::runtime_error("model reload coordinator mismatch");
    const auto affected = impl_->by_container.find(request.key);
    if (affected == impl_->by_container.end()) return false;

    auto batch = std::make_shared<Impl::CommitBatch>();
    batch->names.assign(affected->second.begin(), affected->second.end());
    auto pending = std::make_shared<std::map<std::string, Impl::PendingCandidate>>();
    const auto snapshot = coordinator.registry().snapshot();
    watch::ReloadTransactionGroup group{"model " + watch::assetKeyString(request.key)};
    for (const auto &name : batch->names) {
        auto &record = impl_->records.at(name);
        const auto current = snapshot.find(record.logical);
        if (!current) throw std::runtime_error("model logical resource is stale: " + name);
        auto &candidate = (*pending)[name];
        group.add(watch::ReloadActor{
            .name = "model template " + name,
            .target = record.logical,
            .parse = [request, declaration = record.declaration, pending, name] {
                if (request.kind == watch::ReloadKind::removed)
                    throw std::runtime_error("model file is missing: " + declaration.path.string());
                pending->at(name).prepared = prepareDeclaration(declaration);
            },
            .validate = [this, pending, name] {
                auto &item = pending->at(name);
                item.preview = GET_MODULE(GltfLoader).inspect(item.prepared);
                applyDeclarationBindings(item.preview,
                                         impl_->records.at(name).declaration);
                const auto &live = impl_->records.at(name).model;
                item.rig_changed = !sameRigLayout(live.skeletal, item.preview.skeletal);
            },
            .stage = [this, pending, batch, name, current] {
                if (!batch->instance_capacity_checked) {
                    std::vector<ModelInstanceRebuild> replacements;
                    replacements.reserve(batch->names.size());
                    for (const auto &candidate_name : batch->names) {
                        const auto &live = impl_->records.at(candidate_name).model;
                        replacements.push_back({live.asset_id,
                                                &pending->at(candidate_name).preview});
                    }
                    if (auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>();
                        instances && !instances->canRebuildModelInstances(replacements))
                        throw std::runtime_error("Render command capacity exceeded by model reload batch");
                    batch->instance_capacity_checked = true;
                }

                auto &record = impl_->records.at(name);
                auto &item = pending->at(name);
                auto model = GET_MODULE(GltfLoader).commit(item.prepared);
                applyDeclarationBindings(model, record.declaration);
                model.asset_id = record.model.asset_id;
                model.content_revision = current->content_revision + 1;
                model.compatibility_revision = current->compatibility_revision +
                                               (item.rig_changed ? 1u : 0u);
                auto payload = impl_->makePayload(name);
                payload->candidate = std::move(model);
                payload->batch = batch;
                payload->owns_candidate = true;
                return watch::StagedResourceData{
                    payload, {record.declaration.container_key},
                    payload->candidate.compatibility_revision,
                    fileBytes(record.declaration.path)};
            },
        });
    }
    coordinator.enqueue(std::move(group));
    return true;
}

bool ModelAssetContainer::retireReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    if (&coordinator != impl_->coordinator || !payload ||
        !impl_->tracker->values.contains(payload.get()))
        return false;
    try {
        std::shared_ptr<Impl::CommitBatch> batch;
        for (const auto &[_, record] : impl_->records) {
            if (record.live_payload.get() != payload.get()) continue;
            const auto current = coordinator.registry().snapshot().find(record.logical);
            if (!current || !impl_->tracker->values.contains(current->payload.get()))
                throw std::runtime_error("model reload publication is missing");
            batch = std::static_pointer_cast<const Impl::ModelPayload>(current->payload)->batch;
            break;
        }
        if (batch) impl_->installBatch(batch);
        return true;
    } catch (const std::exception &error) {
        if (logger) LOG_ERROR(logger, "model reload retirement failed: {}", error.what());
        return true;
    } catch (...) {
        if (logger) LOG_ERROR(logger, "model reload retirement failed with an unknown error");
        return true;
    }
}

ModelAssetId ModelAssetContainer::assetIdForTesting(const std::string &name) const {
    return impl_->records.at(name).model.asset_id;
}

std::uint64_t ModelAssetContainer::contentRevisionForTesting(const std::string &name) const {
    return impl_->records.at(name).model.content_revision;
}

std::uint64_t ModelAssetContainer::compatibilityRevisionForTesting(const std::string &name) const {
    return impl_->records.at(name).model.compatibility_revision;
}

} // namespace Pelican
