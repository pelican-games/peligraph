#include "model.hpp"
#include "../loader/modelphysicsbinding.hpp"
#include "../loader/scene.hpp"
#include "../loader/componentcodec.hpp"
#include "../loader/authoringscenedocument.hpp"

#if PELICAN_WITH_ANIMATION
#include "../animation/animationservice.hpp"
#endif
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../material/projectmaterialasset.hpp"
#include "../model/engineprimitives.hpp"
#include "../model/gltf.hpp"
#include "../parallel_prepare.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderingpass/framegraphruntime.hpp"
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
#include <type_traits>

namespace Pelican {
namespace {

constexpr std::string_view modelTemplateTable = "model-template";

std::string releaseExceptionMessage(const std::exception_ptr &error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception &exception) {
        return exception.what();
    } catch (...) {
        return "non-standard model candidate failure";
    }
    return "unknown model candidate failure";
}

ReleaseReport releaseReportFromException(
    const std::exception_ptr &error) noexcept {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const ModelResourceReleaseException &exception) {
        return exception.releaseReport();
    } catch (...) {
    }
    return {};
}

void logReleaseReport(std::string_view context,
                      const ReleaseReport &report) noexcept {
    for (const auto &error : report.view()) {
        if (logger) {
            LOG_ERROR(logger, "{}: stage={} code={} index={}", context,
                      releaseStageName(error.stage),
                      releaseErrorCodeName(error.code), error.index);
        }
    }
    if (report.overflow_count != 0 && logger) {
        LOG_ERROR(logger, "{}: {} additional release errors", context,
                  report.overflow_count);
    }
}

struct ModelDeclaration {
    std::optional<CanonicalModelAssetKey> asset_key;
    std::string name;
    std::string reference;
    std::filesystem::path path;
    std::optional<AssetFragmentRef> fragment;
    watch::AssetKey container_key;
    bool ascii = false;
    bool scene_node_instance = false;
    // GP-5: `reference` is engine://primitives/<name>; `path` is empty and
    // the shape is generated, not read.
    bool engine_primitive = false;
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
    if (declaration.engine_primitive)
        return loader.prepareEnginePrimitive(declaration.reference,
                                             declaration.asset_key);
    if (declaration.ascii)
        return loader.prepareGltf(declaration.path.string(), declaration.fragment,
                                  declaration.asset_key);
    if (declaration.scene_node_instance && declaration.fragment)
        return loader.prepareGltfBinarySceneNode(declaration.path.string(),
                                                 *declaration.fragment,
                                                 declaration.asset_key);
    return loader.prepareGltfBinary(declaration.path.string(),
                                    declaration.fragment,
                                    declaration.asset_key);
}

PrimitiveMaterialBindingDocument loadMaterialBindings(std::string_view reference,
                                                       std::string_view model_name,
                                                       const std::filesystem::path &model_path,
                                                       std::string_view primitive_reference = {}) {
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
        // GP-5: a generated primitive has no file to compare; the document
        // names the same engine reference the asset declares.
        if (!primitive_reference.empty()) {
            if (document.model != primitive_reference)
                throw std::runtime_error("declares model '" + document.model +
                                         "' but asset is '" + std::string{primitive_reference} + "'");
            return document;
        }
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
    auto &project = GET_MODULE(ProjectMaterialAssetContainer);
    const auto project_materials = project.namedMaterials();
    validateNamedMaterialResolutionDomains(
        model, project_materials,
        declaration.name);
    if (!declaration.material_bindings) return;
    const auto fragment = fragmentText(declaration.fragment);
    // FO-1: while a listed material document was refused at start, a binding
    // to a material nothing registered keeps the primitive's own material
    // (a warning in the problem list) instead of refusing the model.
    std::vector<std::string> unresolved;
    applyPrimitiveMaterialBindings(
        model, *declaration.material_bindings, declaration.name,
        fragment.empty() ? std::nullopt
                         : std::optional<std::string_view>{fragment},
        project_materials,
        [&](const PrimitiveMaterialBinding &binding) {
            if (!project.hasRefusedDocuments()) return false;
            unresolved.push_back(binding.material);
            return true;
        });
    if (!unresolved.empty()) {
        LOG_WARNING(logger,
                    "model '{}': its material bindings name material(s) no registered document holds; "
                    "those primitives draw with the model's own material",
                    declaration.name);
    }
    // EB-2: empty takes a warning noted before off (a document refused at
    // start registered since, and the model rebuilt).
    project.noteUnresolvedBindings(declaration.name, std::move(unresolved));
}

std::set<std::string> occurrenceIds(const ModelTemplate &model) {
    std::set<std::string> result;
    for (const auto &material : model.material_primitives)
        for (const auto &primitive : material.primitives)
            if (primitive.occurrence &&
                !primitive.occurrence->occurrence_id.empty())
                result.insert(primitive.occurrence->occurrence_id);
    return result;
}

std::set<std::string> ambiguousOccurrenceIds(
    const ModelTemplate &model) {
    std::set<std::string> result;
    for (const auto &material : model.material_primitives)
        for (const auto &primitive : material.primitives)
            if (primitive.occurrence && primitive.occurrence->ambiguous &&
                !primitive.occurrence->occurrence_id.empty())
                result.insert(primitive.occurrence->occurrence_id);
    return result;
}

ModelDeclaration resolveDeclaration(std::string name, std::string reference,
                                    bool scene_node_instance,
                                    std::optional<std::string> material_bindings = std::nullopt) {
    // GP-5: engine://primitives/<name> is claimed before the project file
    // resolver sees it. The reference is its own identity: nothing is on disk,
    // so nothing is watched and nothing reloads it.
    if (isEnginePrimitiveReference(reference)) {
        requireKnownEnginePrimitive(reference);
        auto declaration = ModelDeclaration{
            .name = std::move(name),
            .reference = reference,
            .container_key = watch::AssetKey{reference, {}},
            .scene_node_instance = scene_node_instance,
            .engine_primitive = true,
        };
        if (material_bindings) {
            declaration.material_bindings = loadMaterialBindings(
                *material_bindings, declaration.name, {}, declaration.reference);
        }
        return declaration;
    }
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

        const auto key = watch::assetKeyForFile(resolver, physical);
        if (!key) throw std::runtime_error("model path is outside watched project stores: " + physical.string());
        container_key = *key;
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
    // ProjectBasicConfig rewrites model paths to physical absolute paths for
    // loading.  Do not let that machine-local path become authored identity:
    // ray material selectors and reload-stable provenance use the canonical
    // project-relative key.
    auto authored_reference = container_key.path;
    if (fragment) {
        authored_reference += "#" + fragment->kind + "/" +
                              fragment->path;
    }
    auto declaration = ModelDeclaration{
        .name = std::move(name),
        .reference = std::move(authored_reference),
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

// A models[] entry of the published asset_data (paths made physical by
// ProjectBasicConfig), declared as the engine declares it at start.
ModelDeclaration declarationFromAssetEntry(const nlohmann::json &asset) {
    std::optional<std::string> material_bindings;
    if (const auto found = asset.find("material_bindings"); found != asset.end()) {
        if (!found->is_string()) {
            throw std::runtime_error("model '" +
                                     asset.value("name", std::string{"<unnamed>"}) +
                                     "' material_bindings must be a string reference");
        }
        material_bindings = found->get<std::string>();
    }
    auto declaration = resolveDeclaration(asset.at("name").get<std::string>(),
                                          asset.at("path").get<std::string>(), false,
                                          std::move(material_bindings));
    declaration.asset_key = CanonicalModelAssetKey{declaration.name};
    return declaration;
}

std::vector<watch::AssetKey> modelDependencies(const ModelDeclaration &declaration,
                                              const PreparedGltf &prepared) {
    std::set<watch::AssetKey> keys{declaration.container_key};
    const auto &resolver = GET_MODULE(PathResolver);
    for (const auto &file : prepared.dataFiles())
        if (auto key = watch::assetKeyForFile(resolver, file)) keys.insert(std::move(*key));
    return {keys.begin(), keys.end()};
}

} // namespace

struct ModelAssetContainer::Impl {
    struct PayloadTracker {
        std::unordered_set<const void *> values;
    };
    struct CommitBatch;
    struct ModelPayload {
        std::string name;
        std::vector<watch::AssetKey> dependencies;
        ModelTemplate candidate;
        std::set<std::string> occurrence_ids;
        std::set<std::string> ambiguous_occurrence_ids;
        std::weak_ptr<CommitBatch> batch;
        std::shared_ptr<PayloadTracker> tracker;
        bool owns_candidate = false;

        ~ModelPayload() {
            if (tracker) tracker->values.erase(this);
            if (owns_candidate) ReleaseErrorSink::record(
                "unpublished model reload candidate", releaseModelGpuResources(candidate, false));
        }
    };
    struct Record {
        ModelDeclaration declaration;
        ModelTemplate model;
        watch::LogicalResourceRef logical;
        std::shared_ptr<const void> live_payload;
        bool declared = false;
        std::vector<watch::AssetKey> dependencies;
    };
    struct CommitBatch {
        struct Installation {
            Record *record = nullptr;
            std::shared_ptr<ModelPayload> payload;
            std::shared_ptr<const void> registry_payload;
            // Filled in by the publish commit. ModelTemplate::skeletal is a
            // shared_ptr, so the pointee address survives the template move
            // and stays valid while the retired payload owns it.
            const SkeletalModelData *retired_skeletal = nullptr;
        };
        std::vector<std::string> names;
        std::map<std::string, std::shared_ptr<ModelPayload>> staged_payloads;
        std::vector<Installation> installs;
        std::map<watch::AssetKey, std::set<std::string>> source_index;
        std::optional<PolygonInstanceContainer::PreparedSceneState>
            renderer_candidate;
        std::optional<PhysWorld::PreparedState> physics_candidate;
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::vector<std::string> ambiguous;
        bool instance_capacity_checked = false;
        bool installed = false;
    };
    struct PendingCandidate {
        PreparedGltf prepared;
        ModelTemplate preview;
        bool rig_changed = false;
    };

    std::unordered_map<std::string, Record> records;
    std::map<watch::AssetKey, std::set<std::string>> by_source;
    watch::ReloadCoordinator *coordinator = nullptr;
    std::shared_ptr<PayloadTracker> tracker = std::make_shared<PayloadTracker>();
    std::uint64_t next_asset_identity = 1;
    std::uint64_t catalog_generation = 1;
    std::vector<std::string> catalog_added;
    std::vector<std::string> catalog_removed;
    std::vector<std::string> catalog_ambiguous;

    ReleaseReport releaseAllModels(bool deferred = false) noexcept {
        ReleaseReport report;
        for (auto &[_, record] : records) {
            report.append(releaseModelGpuResources(record.model, deferred));
        }
        return report;
    }

    ~Impl() {
        ReleaseErrorSink::record("model container teardown release",
                                 releaseAllModels());
    }

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
            record.dependencies, record.model.compatibility_revision,
            fileBytes(record.declaration.path));
        record.live_payload = std::move(payload);
        record.declared = true;
    }

    void add(ModelDeclaration declaration, ModelTemplate &&model, std::vector<watch::AssetKey> dependencies) {
        if (records.contains(declaration.name))
            throw std::runtime_error("duplicate model asset name: " + declaration.name);
        model.asset_id = ModelAssetId{next_asset_identity++};
        model.authored_asset_reference = declaration.reference;
        model.content_revision = 1;
        model.compatibility_revision = 0;
        const auto name = declaration.name;
        auto [found, inserted] = records.emplace(
            name, Record{std::move(declaration), std::move(model), {}, {}, false, std::move(dependencies)});
        if (!inserted) throw std::runtime_error("failed to register model asset: " + name);
        for (const auto &key : found->second.dependencies) by_source[key].insert(name);
        declare(found->second);
    }

    void publishBatch(const std::shared_ptr<CommitBatch> &batch) noexcept {
        if (!batch || batch->installed) return;
        static_assert(std::is_nothrow_move_assignable_v<ModelTemplate>);
        if (batch->renderer_candidate) {
            FastModuleContainer::tryGet<PolygonInstanceContainer>()
                ->publishPreparedScene(
                    std::move(*batch->renderer_candidate));
            batch->renderer_candidate.reset();
        }
        if (batch->physics_candidate) {
            FastModuleContainer::tryGet<PhysWorld>()->publishPrepared(
                std::move(*batch->physics_candidate));
            batch->physics_candidate.reset();
        }
        for (auto &install : batch->installs) {
            auto retired_payload = std::const_pointer_cast<ModelPayload>(
                std::static_pointer_cast<const ModelPayload>(
                    install.record->live_payload));
            install.retired_skeletal = install.record->model.skeletal.get();
            retired_payload->candidate =
                std::move(install.record->model);
            retired_payload->owns_candidate = true;
            install.record->model =
                std::move(install.payload->candidate);
            install.payload->owns_candidate = false;
            install.record->live_payload = std::move(install.registry_payload);
            install.record->dependencies = std::move(install.payload->dependencies);
        }
        by_source.swap(batch->source_index);
        // The animation rig generation advances inside the same commit that
        // swaps the templates: one publish, one generation step. Handles that
        // named the retired rig answer stale_generation from here on.
        for (const auto &install : batch->installs) {
            const auto *previous = install.retired_skeletal;
            const auto *replacement = install.record->model.skeletal.get();
            if (!previous && !replacement) continue;
#if PELICAN_WITH_ANIMATION
            Animation::animationServiceRuntime().reloadAsset(
                install.record->model.asset_id, previous, replacement);
#else
            (void)previous;
            (void)replacement;
#endif
        }
        catalog_added.swap(batch->added);
        catalog_removed.swap(batch->removed);
        catalog_ambiguous.swap(batch->ambiguous);
        ++catalog_generation;
        batch->installed = true;
        batch->installs.clear();
        batch->staged_payloads.clear();
    }
};

ModelAssetContainer::ModelAssetContainer() : impl_{std::make_unique<Impl>()} {
    StartupPhaseTimer startup_timer{&StartupMetrics::addModels};
    const auto assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    std::vector<ModelDeclaration> declarations;
    declarations.reserve(assets.size());
    for (const auto &asset : assets) declarations.push_back(declarationFromAssetEntry(asset));
    const auto prepared = parallelPrepareOrdered<PreparedGltf>(
        declarations.size(),
        [&](std::size_t index) { return prepareDeclaration(declarations[index]); }, 4);
    auto &loader = GET_MODULE(GltfLoader);
    try {
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            auto model = loader.commit(
                prepared[index],
                GltfAuthoredAssetReference{declarations[index].reference});
            try {
                applyDeclarationBindings(model, declarations[index]);
                auto dependencies = modelDependencies(declarations[index], prepared[index]);
                impl_->add(std::move(declarations[index]),
                           std::move(model), std::move(dependencies));
            } catch (...) {
                const auto original = std::current_exception();
                auto report = releaseReportFromException(original);
                report.append(releaseModelGpuResources(model, false));
                if (!report.empty()) {
                    throw ModelResourceReleaseException{
                        releaseExceptionMessage(original), report};
                }
                throw;
            }
        }
    } catch (...) {
        const auto original = std::current_exception();
        auto report = releaseReportFromException(original);
        report.append(impl_->releaseAllModels());
        if (!report.empty()) {
            logReleaseReport("failed model candidate release", report);
            throw ModelResourceReleaseException{
                releaseExceptionMessage(original), report};
        }
        throw;
    }
}

ModelAssetContainer::~ModelAssetContainer() = default;

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) {
    if (const auto found = impl_->records.find(name); found != impl_->records.end())
        return found->second.model;

    // GP-5: engine://primitives/<name> resolves without an asset_data entry,
    // on first use, like a fragment reference below. Scene load, editor
    // spawn/edit and runtime spawn all reach models through this call.
    if (isEnginePrimitiveReference(name)) {
        auto declaration = resolveDeclaration(name, name, false);
        auto prepared = prepareDeclaration(declaration);
        auto dependencies = modelDependencies(declaration, prepared);
        auto model = GET_MODULE(GltfLoader).commit(
            std::move(prepared),
            GltfAuthoredAssetReference{declaration.reference});
        try {
            applyDeclarationBindings(model, declaration);
            impl_->add(std::move(declaration), std::move(model), std::move(dependencies));
        } catch (...) {
            ReleaseErrorSink::record("failed engine primitive release",
                                     releaseModelGpuResources(model, false));
            throw;
        }
        return impl_->records.at(name).model;
    }

    const auto parsed = parsePathReference(name);
    if (!parsed.fragment) throw UnknownModelAssetError{name};
    auto declaration = resolveDeclaration(name, name, true);
    auto prepared = prepareDeclaration(declaration);
    auto dependencies = modelDependencies(declaration, prepared);
    auto model = GET_MODULE(GltfLoader).commit(
        std::move(prepared),
        GltfAuthoredAssetReference{declaration.reference});
    impl_->add(std::move(declaration), std::move(model), std::move(dependencies));
    return impl_->records.at(name).model;
}

void ModelAssetContainer::attachReloadCoordinator(watch::ReloadCoordinator &coordinator) {
    if (impl_->coordinator != nullptr && impl_->coordinator != &coordinator)
        throw std::runtime_error("ModelAssetContainer cannot change reload coordinator");
    impl_->coordinator = &coordinator;
    for (auto &[_, record] : impl_->records) impl_->declare(record);
}

bool ModelAssetContainer::handlesReload(const watch::AssetKey &key) const {
    return impl_->by_source.contains(key);
}

bool ModelAssetContainer::enqueueReload(const watch::ReloadRequest &request,
                                        watch::ReloadCoordinator &coordinator) {
    if (!handlesReload(request.key)) return false;
    coordinator.enqueue(makeReloadGroup(std::span{&request, std::size_t{1}}, coordinator));
    return true;
}

watch::ReloadTransactionGroup ModelAssetContainer::makeReloadGroup(
    std::span<const watch::ReloadRequest> requests, watch::ReloadCoordinator &coordinator) {
    if (&coordinator != impl_->coordinator) throw std::runtime_error("model reload coordinator mismatch");
    std::set<std::string> affected;
    for (const auto &request : requests) {
        const auto found = impl_->by_source.find(request.key);
        if (found == impl_->by_source.end()) throw std::runtime_error("untracked model reload source");
        affected.insert(found->second.begin(), found->second.end());
    }

    auto batch = std::make_shared<Impl::CommitBatch>();
    batch->names.assign(affected.begin(), affected.end());
    auto pending = std::make_shared<std::map<std::string, Impl::PendingCandidate>>();
    const auto snapshot = coordinator.registry().snapshot();
    watch::ReloadTransactionGroup group{"model templates"};
    for (const auto &name : batch->names) {
        auto &record = impl_->records.at(name);
        const auto current = snapshot.find(record.logical);
        if (!current) throw std::runtime_error("model logical resource is stale: " + name);
        auto &candidate = (*pending)[name];
        group.add(watch::ReloadActor{
            .name = "model template " + name,
            .target = record.logical,
            .parse = [declaration = record.declaration, pending, name] {
                // The current document decides which files are needed. A
                // removed former buffer may have been replaced in this batch.
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
                // Catalog preparation is a pure projection of the inspect
                // result and its authored asset key. Do it before commit can
                // allocate any GPU-backed model resources.
                auto candidate_occurrences = occurrenceIds(item.preview);
                auto candidate_ambiguous =
                    ambiguousOccurrenceIds(item.preview);
                auto dependencies = modelDependencies(record.declaration, item.prepared);
                auto model = GET_MODULE(GltfLoader).commit(
                    item.prepared,
                    GltfAuthoredAssetReference{
                        record.declaration.reference});
                applyDeclarationBindings(model, record.declaration);
                model.asset_id = record.model.asset_id;
                model.authored_asset_reference =
                    record.declaration.reference;
                model.content_revision = current->content_revision + 1;
                model.compatibility_revision = current->compatibility_revision +
                                               (item.rig_changed ? 1u : 0u);
                auto payload = impl_->makePayload(name);
                payload->candidate = std::move(model);
                payload->dependencies = std::move(dependencies);
                payload->occurrence_ids =
                    std::move(candidate_occurrences);
                payload->ambiguous_occurrence_ids =
                    std::move(candidate_ambiguous);
                payload->batch = batch;
                payload->owns_candidate = true;
                batch->staged_payloads[name] = payload;
                return watch::StagedResourceData{
                    payload, payload->dependencies,
                    payload->candidate.compatibility_revision,
                    fileBytes(record.declaration.path)};
            },
        });
    }
    group.setPreparePublish([this, batch] {
        batch->source_index = impl_->by_source;
        batch->installs.clear();
        batch->installs.reserve(batch->names.size());
        std::vector<ModelInstanceRebuild> rebuilds;
        rebuilds.reserve(batch->names.size());
        std::set<std::string> old_ids;
        std::set<std::string> new_ids;
        std::set<std::string> ambiguous;
        for (const auto &name : batch->names) {
            auto &record = impl_->records.at(name);
            const auto payload = batch->staged_payloads.at(name);
            if (!payload || !payload->owns_candidate)
                throw std::runtime_error(
                    "model reload candidate batch is incomplete");
            for (const auto &key : record.dependencies) {
                auto found = batch->source_index.find(key);
                found->second.erase(name);
                if (found->second.empty()) batch->source_index.erase(found);
            }
            for (const auto &key : payload->dependencies) batch->source_index[key].insert(name);
            rebuilds.push_back(
                {record.model.asset_id, &payload->candidate});
            batch->installs.push_back(
                {&record, payload, payload});
            const auto previous = occurrenceIds(record.model);
            const auto &replacement = payload->occurrence_ids;
            old_ids.insert(previous.begin(), previous.end());
            new_ids.insert(replacement.begin(), replacement.end());
            const auto &candidate_ambiguous =
                payload->ambiguous_occurrence_ids;
            ambiguous.insert(candidate_ambiguous.begin(),
                             candidate_ambiguous.end());
        }
        batch->added.clear();
        batch->removed.clear();
        std::set_difference(new_ids.begin(), new_ids.end(),
                            old_ids.begin(), old_ids.end(),
                            std::back_inserter(batch->added));
        std::set_difference(old_ids.begin(), old_ids.end(),
                            new_ids.begin(), new_ids.end(),
                            std::back_inserter(batch->removed));
        batch->ambiguous.assign(ambiguous.begin(), ambiguous.end());
        if (impl_->catalog_generation ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error(
                "model occurrence catalog generation exhausted");
        if (auto *instances =
                FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
            batch->renderer_candidate =
                instances->prepareModelInstanceRebuild(rebuilds);
            if (const auto *runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>()) {
                if (const auto generation = runtime->snapshot()) {
                    const auto geometry = instances->rayQueryGeometryInstances(&*batch->renderer_candidate);
                    // Active graph references must remain valid through this
                    // model publication. Topology changes require detaching
                    // affected graphs before replacing their model geometry.
                    for (const auto &[id, program] : generation->programs) {
                        validateGraphDeformedAttributes(program.frame_graph.deformed_attributes, geometry);
                        for (const auto &replacement : rebuilds)
                            validateGraphPoseAttributesForModel(program.frame_graph.pose_attributes,
                                replacement.asset_id, *replacement.replacement);
                    }
                }
            }
        }
        auto *physics = FastModuleContainer::tryGet<PhysWorld>();
        const auto *loader = FastModuleContainer::tryGet<SceneLoader>();
        const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
        if (physics && loader && config) {
            const auto *scene = config->resolvedScene().findScene(loader->currentScene());
            if (scene) {
                auto next = physics->snapshotPrepared();
                bool changed = false;
                for (const auto &object : scene->objects) {
                    const auto runtime = std::find_if(loader->runtimeObjectBindings().begin(),
                        loader->runtimeObjectBindings().end(), [&](const auto &binding) {
                            return binding.authoring_object_id == object.authoring_object_id;
                        });
                    if (runtime == loader->runtimeObjectBindings().end()) continue;
                    for (const auto &component : object.components) {
                        if (component.name != "simplemodelview") continue;
                        const auto &data = std::any_cast<const SimpleModelViewCodecData &>(
                            component.requireRuntimeValue());
                        const auto candidate = batch->staged_payloads.find(data.model);
                        if (candidate == batch->staged_payloads.end()) continue;
                        replaceModelPhysicsBinding(next.bindings, candidate->second->candidate,
                            runtimeObjectIdentityName(loader->currentScene(),
                                object.authoring_object_id, object.name.value_or(std::string{})),
                            runtime->object_id);
                        changed = true;
                    }
                }
                if (changed) batch->physics_candidate = physics->prepareBindings(std::move(next.bindings));
            }
        }
    });
    group.setPublish([this, batch]() noexcept {
        impl_->publishBatch(batch);
    });
    group.setRollback([batch]() noexcept {
        batch->renderer_candidate.reset();
        batch->physics_candidate.reset();
        batch->installs.clear();
        batch->staged_payloads.clear();
    });
    return group;
}

bool ModelAssetContainer::retireReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    if (&coordinator != impl_->coordinator || !payload ||
        !impl_->tracker->values.contains(payload.get()))
        return false;
    try {
        auto retired = std::const_pointer_cast<Impl::ModelPayload>(
            std::static_pointer_cast<const Impl::ModelPayload>(payload));
        if (retired->owns_candidate) {
            ReleaseErrorSink::record(
                "retired model release",
                releaseModelGpuResources(retired->candidate, true));
            retired->owns_candidate = false;
        }
        return true;
    } catch (const std::exception &error) {
        if (logger) LOG_ERROR(logger, "model reload retirement failed: {}", error.what());
        return true;
    } catch (...) {
        if (logger) LOG_ERROR(logger, "model reload retirement failed with an unknown error");
        return true;
    }
}

bool ModelAssetContainer::hasModel(const std::string &name) const {
    return impl_->records.contains(name);
}

std::vector<ModelMaterialSlot> ModelAssetContainer::materialSlots(const std::string &name) const {
    const auto &declaration = impl_->records.at(name).declaration;
    const auto preview = GET_MODULE(GltfLoader).inspect(prepareDeclaration(declaration));
    std::map<std::pair<std::uint32_t, std::uint32_t>, ModelMaterialSlot> slots;
    for (const auto &group : preview.material_primitives) {
        const auto named = std::find_if(preview.named_materials.begin(), preview.named_materials.end(),
                                        [&](const auto &material) { return material.material == group.material; });
        for (const auto &primitive : group.primitives) {
            slots.try_emplace({primitive.mesh_index, primitive.primitive_index},
                              ModelMaterialSlot{
                                  .mesh = primitive.mesh_index,
                                  .primitive = primitive.primitive_index,
                                  .slot = named != preview.named_materials.end() ? named->name : std::string{},
                                  .routing = named != preview.named_materials.end()
                                                 ? named->routing
                                                 : std::optional<MaterialVariantRouting>{},
                              });
        }
    }
    std::vector<ModelMaterialSlot> result;
    result.reserve(slots.size());
    for (auto &[key, slot] : slots) {
        (void)key;
        result.push_back(std::move(slot));
    }
    return result;
}

std::optional<PrimitiveMaterialBindingDocument>
ModelAssetContainer::declaredMaterialBindings(const std::string &name) const {
    return impl_->records.at(name).declaration.material_bindings;
}

std::optional<PrimitiveMaterialBindingDocument>
ModelAssetContainer::exchangeMaterialBindings(const std::string &name, const std::string &reference) {
    auto &declaration = impl_->records.at(name).declaration;
    auto bindings = declaration.engine_primitive
                        ? loadMaterialBindings(reference, declaration.name, {}, declaration.reference)
                        : loadMaterialBindings(reference, declaration.name, declaration.path);
    return std::exchange(declaration.material_bindings, std::move(bindings));
}

void ModelAssetContainer::restoreMaterialBindings(const std::string &name,
                                                  std::optional<PrimitiveMaterialBindingDocument> previous) {
    impl_->records.at(name).declaration.material_bindings = std::move(previous);
}

watch::AssetKey ModelAssetContainer::reloadKey(const std::string &name) const {
    return impl_->records.at(name).declaration.container_key;
}

std::optional<std::string> ModelAssetContainer::modelDeclaringMaterial(std::string_view material) const {
    for (const auto &[name, record] : impl_->records)
        for (const auto &named : record.model.named_materials)
            if (named.name == material) return name;
    return std::nullopt;
}

std::vector<std::string>
ModelAssetContainer::modelsDrawingMaterials(std::span<const GlobalMaterialId> materials) const {
    std::vector<ModelAssetId> instanced;
    if (const auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>())
        instanced = instances->modelsDrawingMaterials(materials);
    std::vector<std::string> result;
    for (const auto &[name, record] : impl_->records) {
        const auto drawn = [&] {
            if (std::find(instanced.begin(), instanced.end(), record.model.asset_id) != instanced.end()) return true;
            return std::any_of(record.model.material_primitives.begin(), record.model.material_primitives.end(),
                               [&](const auto &group) {
                                   return std::find(materials.begin(), materials.end(), group.material) !=
                                          materials.end();
                               });
        };
        if (drawn()) result.push_back(name);
    }
    return result;
}

void ModelAssetContainer::preflightModel(const std::string &name, const std::string &reference) {
    auto declaration = resolveDeclaration(name, reference, false);
    declaration.asset_key = CanonicalModelAssetKey{declaration.name};
    auto preview = GET_MODULE(GltfLoader).inspect(prepareDeclaration(declaration));
    applyDeclarationBindings(preview, declaration);
}

void ModelAssetContainer::registerModel(const std::string &name) {
    const auto assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    const auto entry = std::find_if(assets.begin(), assets.end(), [&](const nlohmann::json &asset) {
        return asset.is_object() && asset.value("name", std::string{}) == name;
    });
    if (entry == assets.end())
        throw std::runtime_error("the engine's asset_data declares no model '" + name + "'");
    auto declaration = declarationFromAssetEntry(*entry);
    if (const auto held = impl_->records.find(name); held != impl_->records.end()) {
        // Kept by an unregister that could not release it: the same file is
        // the same model.
        if (held->second.declaration.container_key == declaration.container_key) return;
        throw std::runtime_error("duplicate model asset name: " + name);
    }
    // As the constructor installs each entry at start.
    auto prepared = prepareDeclaration(declaration);
    auto model = GET_MODULE(GltfLoader).commit(prepared, GltfAuthoredAssetReference{declaration.reference});
    try {
        applyDeclarationBindings(model, declaration);
        auto dependencies = modelDependencies(declaration, prepared);
        impl_->add(std::move(declaration), std::move(model), std::move(dependencies));
    } catch (...) {
        ReleaseErrorSink::record("failed model registration release", releaseModelGpuResources(model, false));
        throw;
    }
}

std::optional<std::string> ModelAssetContainer::unregisterModel(const std::string &name) {
    const auto found = impl_->records.find(name);
    if (found == impl_->records.end()) return std::nullopt;
    auto &record = found->second;
    if (record.model.skeletal)
        return "the model has an animation rig, which the animation service may still hold";
    if (const auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>();
        instances && instances->hasModelInstances(record.model.asset_id))
        return "a live instance still draws the model";
    if (record.declared && impl_->coordinator != nullptr)
        (void)impl_->coordinator->registry().destroyResource(record.logical);
    for (const auto &key : record.dependencies) {
        const auto source = impl_->by_source.find(key);
        if (source == impl_->by_source.end()) continue;
        source->second.erase(name);
        if (source->second.empty()) impl_->by_source.erase(source);
    }
    // As a reload retires a replaced template: frames in flight may still
    // read its resources.
    ReleaseErrorSink::record("unregistered model release", releaseModelGpuResources(record.model, true));
    impl_->records.erase(found);
    return std::nullopt;
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

nlohmann::json ModelAssetContainer::occurrenceCatalogStatus() const {
    return {
        {"generation", impl_->catalog_generation},
        {"added", impl_->catalog_added},
        {"removed", impl_->catalog_removed},
        {"ambiguous", impl_->catalog_ambiguous},
    };
}

} // namespace Pelican
