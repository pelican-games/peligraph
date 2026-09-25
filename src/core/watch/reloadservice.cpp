#include "reloadservice.hpp"

#include "../asset/model.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../fullscreenpass/passinputlayout.hpp"
#include "../fullscreenpass/shaderinputmismatchjson.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "../launchconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../material/materialcontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "reloadgate.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

namespace Pelican::watch {

namespace {
thread_local RenderPipelineCompanionReloadObserver
    render_pipeline_companion_reload_observer;

// VI-1b: every shader reload checks each fullscreen / raster candidate
// against its pass's inputs by the rule edit and load use; a candidate that
// disagrees is left out and its pass keeps drawing.
PipelineReplacementCheck passInputReloadCheck() {
    auto *container = FastModuleContainer::tryGet<FullscreenPassContainer>();
    if (container == nullptr) return {};
    return [container](PipelineHandle handle, const ShaderReflection &candidate) {
        container->checkReloadCandidate(handle, candidate);
    };
}

// VI-1b: what a committed reload did, told to the pass container (which
// keeps the refusals get_frame_plan reports) and, when a pass refused, to
// the result: a failure the reload status keeps, with the refused passes.
void takePassInputReloadOutcome(const PipelineRebuildResult &rebuilt,
                                RuntimeReloadResult &runtime) {
    auto *container = FastModuleContainer::tryGet<FullscreenPassContainer>();
    if (container != nullptr) {
        container->recordReloadOutcome(rebuilt.replaced, rebuilt.refused);
    }
    if (rebuilt.refused.empty()) return;
    auto passes = nlohmann::ordered_json::array();
    std::string message;
    for (const auto &refused : rebuilt.refused) {
        FullscreenReloadRefusal refusal;
        refusal.reason = refused.error;
        try {
            if (refused.exception) std::rethrow_exception(refused.exception);
        } catch (const FullscreenInputBindingMismatch &mismatch) {
            refusal.mismatch = mismatch.info();
        } catch (...) {
        }
        passes.push_back(reloadRefusalJson(refusal, true));
        if (!message.empty()) message += "\n";
        message += "Shader reload refused for pass '" + refusal.mismatch.pass +
                   "', which keeps drawing with its previous shader: " +
                   refused.error;
    }
    runtime.error = std::move(message);
    runtime.error_code = std::string{shaderReloadRefusedErrorCode};
    if (!runtime.error_fields.is_object()) {
        runtime.error_fields = nlohmann::ordered_json::object();
    }
    runtime.error_fields["refused_passes"] = std::move(passes);
}

// VI-1b: PELICAN_DECLARE_VARIADIC_INPUTS twice does not compile, so the
// whole reload fails as a compile does; the failure carries the refusal the
// pass compiler would have given (without the pass: the compile names none).
void addCompileRefusalFields(RuntimeReloadResult &runtime) {
    if (!compileLogShowsMultipleVariadicGroups(runtime.error)) return;
    if (!runtime.error_fields.is_object()) {
        runtime.error_fields = nlohmann::ordered_json::object();
    }
    runtime.error_fields["shader_input_mismatch"] =
        shaderInputMismatchJson(multipleVariadicGroupsCompileMismatch().info());
}

void appendReloadErrorFields(nlohmann::ordered_json &pending,
                             const nlohmann::ordered_json &fields) {
    if (!fields.is_object()) return;
    if (!pending.is_object()) pending = nlohmann::ordered_json::object();
    for (const auto &[key, value] : fields.items()) {
        if (key == "refused_passes" && pending.contains(key) &&
            pending[key].is_array() && value.is_array()) {
            for (const auto &entry : value) pending[key].push_back(entry);
            continue;
        }
        pending[key] = value;
    }
}
}

ScopedRenderPipelineCompanionReloadObserver::
    ScopedRenderPipelineCompanionReloadObserver(
        RenderPipelineCompanionReloadObserver observer)
    : previous_{
          std::move(render_pipeline_companion_reload_observer)} {
    render_pipeline_companion_reload_observer =
        std::move(observer);
}

ScopedRenderPipelineCompanionReloadObserver::
    ~ScopedRenderPipelineCompanionReloadObserver() {
    render_pipeline_companion_reload_observer =
        std::move(previous_);
}

namespace {

std::string_view runtimeBoundaryName(RuntimeReloadBoundary boundary) {
    switch (boundary) {
    case RuntimeReloadBoundary::frame_start: return "frame_start";
    case RuntimeReloadBoundary::render_start: return "render_start";
    }
    return "unknown";
}

void appendReloadError(std::string &destination, std::string message) {
    if (message.empty()) return;
    if (!destination.empty()) destination += "; ";
    destination += std::move(message);
}

}

ReloadService::~ReloadService() {
    if (watcher_) watcher_->stop();
}

void ReloadService::registerParticipant(ReloadParticipant participant) {
    // The apply routes are mutually exclusive by construction now, so what is
    // left here is what a type cannot say: a required callback is not null, a
    // companion list names other participants, and the name is unique.
    if (participant.name.empty()) {
        throw std::invalid_argument("reload participant name cannot be empty");
    }
    if (participant.file) {
        const bool has_apply = std::visit(
            [](const auto &route) {
                using Route = std::decay_t<decltype(route)>;
                if constexpr (std::is_same_v<Route, PerRequestReloadApply>) {
                    return static_cast<bool>(route.enqueue);
                } else if constexpr (std::is_same_v<Route, BatchReloadApply>) {
                    return static_cast<bool>(route.apply_batch);
                } else {
                    return static_cast<bool>(route.apply);
                }
            },
            participant.file->apply);
        if (!participant.file->claims || !has_apply) {
            throw std::invalid_argument("reload participant '" + participant.name +
                                        "' requires claims plus one apply callback");
        }
        if (const auto *companions =
                std::get_if<CompanionBatchReloadApply>(&participant.file->apply)) {
            if (companions->companion_participants.empty()) {
                throw std::invalid_argument(
                    "reload participant '" + participant.name +
                    "' companion apply requires at least one companion "
                    "participant");
            }
            std::unordered_set<std::string> companion_names;
            for (const auto &companion :
                 companions->companion_participants) {
                if (companion.empty() ||
                    companion == participant.name ||
                    !companion_names.insert(companion).second) {
                    throw std::invalid_argument(
                        "reload participant '" +
                        participant.name +
                        "' has an invalid companion participant");
                }
            }
        }
    }
    if (participant.runtime && !participant.runtime->apply) {
        throw std::invalid_argument("runtime reload participant '" + participant.name +
                                    "' requires an apply callback");
    }
    if (participant.authored && !participant.authored->apply) {
        throw std::invalid_argument("authored reload participant '" + participant.name +
                                    "' requires an apply callback");
    }
    if (!participant.file && !participant.runtime &&
        !participant.authored) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' has no file, runtime or authored-candidate callbacks");
    }
    if (std::ranges::any_of(participants_, [&](const auto &existing) {
            return existing.name == participant.name;
        })) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' is already registered");
    }
    if (participant.runtime) runtime_status_.try_emplace(participant.name);
    participants_.push_back(std::move(participant));
}

bool ReloadService::unregisterParticipant(std::string_view name) noexcept {
    const auto found = std::ranges::find(participants_, name, &ReloadParticipant::name);
    if (found == participants_.end()) return false;
    runtime_status_.erase(found->name);
    requested_runtime_reloads_.erase(found->name);
    participants_.erase(found);
    return true;
}

std::vector<std::string> ReloadService::participantNames() const {
    std::vector<std::string> result;
    result.reserve(participants_.size());
    for (const auto &participant : participants_) result.push_back(participant.name);
    std::ranges::sort(result);
    return result;
}

RuntimeReloadResult ReloadService::invokeRuntimeParticipant(
    ReloadParticipant &participant, RuntimeReloadTrigger trigger) {
    RuntimeReloadResult result;
    try {
        result = participant.runtime->apply(trigger);
    } catch (const std::exception &error) {
        result = {.attempted = true,
                  .error = error.what(),
                  .error_code = "runtime_reload_exception@1",
                  .error_fields = {{"reason", error.what()}}};
    } catch (...) {
        result = {.attempted = true,
                  .error = "unknown runtime reload failure",
                  .error_code = "runtime_reload_exception@1",
                  .error_fields = {{"reason", "non_standard_exception"}}};
    }

    if (result.committed || !result.error.empty()) result.attempted = true;
    auto &status = runtime_status_[participant.name];
    if (result.attempted) ++status.attempted;
    if (result.committed) ++status.applied;
    if (!result.error.empty()) {
        ++status.failed;
        status.last_error = result.error;
        status.error_code = result.error_code;
        status.error_fields = result.error_fields.is_object()
                                  ? result.error_fields
                                  : nlohmann::ordered_json::object();
        if (logger) {
            LOG_WARNING(logger, "runtime reload participant '{}' failed: {}",
                        participant.name, result.error);
        }
    } else if (result.attempted) {
        status.last_error.clear();
        status.error_code.clear();
        status.error_fields = nlohmann::ordered_json::object();
    }
    return result;
}

RuntimeReloadSummary ReloadService::applyRuntimeBoundary(RuntimeReloadBoundary boundary) {
    ensureBuiltInParticipants();
    RuntimeReloadSummary summary;
    for (auto &participant : participants_) {
        if (!participant.runtime || participant.runtime->boundary != boundary) continue;
        const bool requested = requested_runtime_reloads_.erase(participant.name) != 0;
        const auto result = invokeRuntimeParticipant(
            participant, requested ? RuntimeReloadTrigger::requested : RuntimeReloadTrigger::poll);
        if (result.attempted) ++summary.attempted;
        if (result.committed) ++summary.committed;
        if (result.failed()) ++summary.failed;
    }
    return summary;
}

RuntimeReloadResult ReloadService::applyRuntimeNow(std::string_view name,
                                                   RuntimeReloadTrigger trigger) {
    ensureBuiltInParticipants();
    const auto found = std::ranges::find(participants_, name, &ReloadParticipant::name);
    if (found == participants_.end()) {
        return {.attempted = true,
                .error = "runtime reload participant '" + std::string{name} +
                         "' is not registered"};
    }
    if (!found->runtime) {
        return {.attempted = true,
                .error = "reload participant '" + std::string{name} +
                         "' has no runtime transaction"};
    }
    requested_runtime_reloads_.erase(found->name);
    return invokeRuntimeParticipant(*found, trigger);
}

AuthoredRuntimeReloadResult ReloadService::applyAuthoredCandidate(
    std::string_view name,
    RenderConfigCandidateDocumentSet candidate,
    std::function<void()> source_commit,
    std::function<void(AuthoredRuntimeReloadResult)> completion) {
    ensureBuiltInParticipants();
    const auto found = std::ranges::find(
        participants_, name, &ReloadParticipant::name);
    if (found == participants_.end()) {
        return {
            .attempted = true,
            .error = "authored reload participant '" +
                     std::string{name} + "' is not registered",
        };
    }
    if (!found->authored) {
        return {
            .attempted = true,
            .error = "reload participant '" + std::string{name} +
                     "' does not accept authored candidates",
        };
    }
    try {
        auto result = found->authored->apply(
            std::move(candidate), std::move(source_commit),
            std::move(completion));
        result.attempted = true;
        return result;
    } catch (const std::exception &error) {
        return {.attempted = true,
                .error = error.what(),
                .exception = std::current_exception()};
    } catch (...) {
        return {
            .attempted = true,
            .error = "unknown authored render pipeline reload failure",
        };
    }
}

bool ReloadService::requestRuntimeReload(std::string_view name) {
    ensureBuiltInParticipants();
    const auto found = std::ranges::find(participants_, name, &ReloadParticipant::name);
    if (found == participants_.end() || !found->runtime) return false;
    requested_runtime_reloads_.insert(found->name);
    return true;
}

void ReloadService::mergeShaderRuntimeResult(RuntimeReloadResult result) {
    if (!result.attempted && !result.committed && result.error.empty()) return;
    if (!pending_shader_runtime_result_) {
        pending_shader_runtime_result_ = std::move(result);
        return;
    }
    auto &pending = *pending_shader_runtime_result_;
    pending.attempted = pending.attempted || result.attempted;
    pending.committed = pending.committed || result.committed;
    appendReloadError(pending.error, std::move(result.error));
    // VI-1b: a refusal's code and refused passes ride along.
    if (pending.error_code.empty()) pending.error_code = std::move(result.error_code);
    appendReloadErrorFields(pending.error_fields, result.error_fields);
}

bool ReloadService::applyShaderReloadBatch(
    std::span<const ReloadRequest> shader_requests,
    std::span<const AssetKey> material_documents) {
    auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
    auto *pipelines = FastModuleContainer::tryGet<PipelineFactory>();
    if (library == nullptr || pipelines == nullptr || shader_requests.empty()) return false;

    std::vector<AssetKey> changed_keys;
    changed_keys.reserve(shader_requests.size());
    for (const auto &request : shader_requests) changed_keys.push_back(request.key);

    RuntimeReloadResult runtime{.attempted = true};
    try {
        auto prepared = library->prepareReload(changed_keys);
        if (prepared.empty()) {
            runtime.error = "shader change no longer has a tracked dependency";
        } else {
            shader_cache_hits_ += prepared.cache_hits;
            shader_cache_misses_ += prepared.cache_misses;
            std::function<void()> material_commit;
            if (auto *materials = FastModuleContainer::tryGet<MaterialContainer>()) {
                material_commit = materials->prepareSurfaceMaterialReload(
                    prepared.surface_documents, material_documents);
            }
            const auto rebuilt = pipelines->rebuildPrepared(
                std::move(prepared), material_commit, {},
                passInputReloadCheck());
            runtime.committed = rebuilt.committed;
            if (!rebuilt.committed) {
                runtime.error = rebuilt.last_error.empty()
                                    ? "shader/pipeline candidate transaction failed"
                                    : rebuilt.last_error;
            } else {
                // VI-1b: the file watch's reload refuses a pass the same way
                // reload_shaders does, and the status keeps it.
                takePassInputReloadOutcome(rebuilt, runtime);
            }
        }
    } catch (const std::exception &error) {
        runtime.error = error.what();
    } catch (...) {
        runtime.error = "unknown shader reload transaction failure";
    }
    if (!runtime.committed) {
        library->recordReloadFailure(changed_keys, runtime.error);
        addCompileRefusalFields(runtime);
    }
    mergeShaderRuntimeResult(runtime);
    return runtime.committed;
}

std::unique_ptr<PreparedReloadTransaction> ReloadService::prepareRenderPipelineModelReload(
    std::span<const ReloadRequest> requests) {
    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    if (!models) return {};
    std::vector<ReloadRequest> model_requests;
    for (const auto &request : requests)
        if (models->handlesReload(request.key)) model_requests.push_back(request);
    if (model_requests.empty()) return {};
    return transactions_.prepare(models->makeReloadGroup(model_requests, transactions_), retireSink());
}

void ReloadService::applyRenderPipelineCompanionReload(
    const RendererRuntimeGeneration &generation,
    std::span<const ReloadRequest>
        companion_requests) {
    if (render_pipeline_companion_reload_observer) {
        render_pipeline_companion_reload_observer(
            companion_requests);
    }
    auto *library =
        FastModuleContainer::tryGet<ShaderLibrary>();
    auto *pipelines =
        FastModuleContainer::tryGet<PipelineFactory>();
    auto *materials =
        FastModuleContainer::tryGet<MaterialContainer>();
    if (library == nullptr || pipelines == nullptr) {
        throw std::runtime_error(
            "coordinated render-pipeline reload requires "
            "ShaderLibrary and PipelineFactory");
    }

    std::vector<AssetKey> shader_keys;
    std::vector<AssetKey> material_documents;
    for (const auto &request :
         companion_requests) {
        if (library->handlesReload(request.key)) {
            shader_keys.push_back(request.key);
            continue;
        }
        if (materials != nullptr &&
            materials->handlesMaterialValuesReload(
                request.key)) {
            material_documents.push_back(
                request.key);
            continue;
        }
        if (const auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
            models && models->handlesReload(request.key)) continue;
        throw std::runtime_error(
            "coordinated render-pipeline reload received "
            "an unsupported companion request: " +
            assetKeyString(request.key));
    }

    RuntimeReloadResult runtime;
    std::vector<AssetKey> recorded_keys =
        shader_keys;
    try {
        MaterialRuntimeGenerationReloadPlan
            material_plan;
        if (materials != nullptr) {
            material_plan =
                materials
                    ->prepareRuntimeGenerationReload(
                        generation);
        }
        runtime.attempted =
            !shader_keys.empty() ||
            material_plan.changesPipelineAbi();

        auto prepared = library->prepareReload(
            shader_keys,
            material_plan.shader_overrides);
        recorded_keys = prepared.changed_keys;
        if (!shader_keys.empty() &&
            prepared.empty()) {
            throw std::runtime_error(
                "shader change no longer has a tracked "
                "dependency");
        }
        shader_cache_hits_ +=
            prepared.cache_hits;
        shader_cache_misses_ +=
            prepared.cache_misses;

        std::function<void()> material_commit;
        if (materials != nullptr) {
            material_commit =
                materials
                    ->prepareSurfaceMaterialReload(
                        prepared.surface_documents,
                        material_documents);
        }
        std::function<void()> coordinated_commit;
        if (material_commit ||
            material_plan.commit) {
            coordinated_commit =
                [material_commit =
                     std::move(material_commit),
                 generation_commit =
                     std::move(
                         material_plan.commit)] {
                    if (material_commit) {
                        material_commit();
                    }
                    if (generation_commit) {
                        generation_commit();
                    }
                };
        }

        if (prepared.empty() &&
            material_plan.pipeline_overrides.empty() &&
            !coordinated_commit) {
            return;
        }
        const auto rebuilt =
            pipelines->rebuildPrepared(
                std::move(prepared),
                coordinated_commit,
                material_plan.pipeline_overrides,
                passInputReloadCheck());
        if (!rebuilt.committed) {
            throw std::runtime_error(
                rebuilt.last_error.empty()
                    ? "coordinated shader/material/pipeline "
                      "candidate transaction failed"
                    : rebuilt.last_error);
        }
        runtime.committed =
            runtime.attempted;
        // VI-1b.
        takePassInputReloadOutcome(rebuilt, runtime);
    } catch (const std::exception &error) {
        runtime.error = error.what();
        if (!recorded_keys.empty()) {
            library->recordReloadFailure(
                recorded_keys, runtime.error);
        }
        mergeShaderRuntimeResult(
            std::move(runtime));
        throw;
    } catch (...) {
        runtime.error =
            "unknown coordinated render-pipeline reload "
            "failure";
        if (!recorded_keys.empty()) {
            library->recordReloadFailure(
                recorded_keys, runtime.error);
        }
        mergeShaderRuntimeResult(
            std::move(runtime));
        throw;
    }
    mergeShaderRuntimeResult(
        std::move(runtime));
}

RuntimeReloadResult ReloadService::forceShaderReload() {
    auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
    auto *pipelines = FastModuleContainer::tryGet<PipelineFactory>();
    if (library == nullptr || pipelines == nullptr) return {};

    RuntimeReloadResult runtime{.attempted = true};
    std::vector<AssetKey> changed_keys;
    last_shader_reload_changes_ = {};
    try {
        // F-5: only the units whose compile identity moved are republished,
        // so an explicit request with nothing to do attempts nothing.
        auto changed = library->prepareReloadChanged();
        auto &prepared = changed.prepared;
        changed_keys = prepared.changed_keys;
        last_shader_reload_changes_ = {
            .changed = changed_keys,
            .cache_hits = prepared.cache_hits,
            .cache_misses = prepared.cache_misses,
        };
        if (!changed.error.empty()) throw std::runtime_error(changed.error);
        if (prepared.empty()) {
            runtime.attempted = false;
            return runtime;
        }
        shader_cache_hits_ += prepared.cache_hits;
        shader_cache_misses_ += prepared.cache_misses;
        std::function<void()> material_commit;
        if (auto *materials = FastModuleContainer::tryGet<MaterialContainer>()) {
            material_commit = materials->prepareSurfaceMaterialReload(
                prepared.surface_documents, {});
        }
        const auto rebuilt = pipelines->rebuildPrepared(
            std::move(prepared), material_commit, {}, passInputReloadCheck());
        runtime.committed = rebuilt.committed;
        if (!rebuilt.committed) {
            runtime.error = rebuilt.last_error.empty()
                                ? "forced shader/pipeline transaction failed"
                                : rebuilt.last_error;
        } else {
            // VI-1b: the passes that refused keep drawing; the result names
            // them (reload_shaders answers it, reload status keeps it).
            takePassInputReloadOutcome(rebuilt, runtime);
        }
    } catch (const std::exception &error) {
        if (!runtime.committed) runtime.error = error.what();
    } catch (...) {
        if (!runtime.committed) runtime.error = "unknown forced shader reload failure";
    }
    if (runtime.committed) {
        shader_publication_pending_ = true;
        if (runtime.error_code == shaderReloadRefusedErrorCode) {
            auto changed = nlohmann::ordered_json::array();
            for (const auto &key : changed_keys) changed.push_back(assetKeyString(key));
            runtime.error_fields["changed"] = std::move(changed);
        }
        return runtime;
    }
    library->recordReloadFailure(changed_keys, runtime.error);
    auto changed = nlohmann::ordered_json::array();
    for (const auto &key : changed_keys) changed.push_back(assetKeyString(key));
    runtime.error_code = std::string{shaderReloadFailedErrorCode};
    runtime.error_fields = {{"changed", std::move(changed)}};
    addCompileRefusalFields(runtime);
    return runtime;
}

void ReloadService::ensureBuiltInParticipants() {
    const auto has_participant = [this](std::string_view name) {
        return std::ranges::any_of(participants_, [&](const auto &participant) {
            return participant.name == name;
        });
    };

    if (!has_participant(shaderReloadParticipantName)) {
        registerParticipant(ReloadParticipant{
            .name = std::string{shaderReloadParticipantName},
            .file = FileReloadRole{
                .claims = [](const ReloadRequest &request) {
                    const auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
                    return library != nullptr && library->handlesReload(request.key);
                },
                // One shader/pipeline transaction per watcher frame, with the
                // material documents that changed alongside it. Material value
                // documents ride along only when a shader actually changed;
                // on their own they stay with the material participant.
                .apply = CompanionBatchReloadApply{
                    .companion_participants = {
                        std::string{materialReloadParticipantName}},
                    .companion_claims = [](std::string_view,
                                           const ReloadRequest &request) {
                        const auto *materials =
                            FastModuleContainer::tryGet<MaterialContainer>();
                        return materials != nullptr &&
                               materials->handlesMaterialValuesReload(request.key);
                    },
                    .apply = [this](std::span<const ReloadRequest> shader_requests,
                                    std::span<const ReloadRequest> companions) {
                        std::vector<AssetKey> material_documents;
                        material_documents.reserve(companions.size());
                        for (const auto &companion : companions) {
                            material_documents.push_back(companion.key);
                        }
                        return applyShaderReloadBatch(shader_requests,
                                                      material_documents);
                    },
                    // The shader transaction hands its result to the runtime
                    // participant below, which bills it at render_start.
                    .status_reporting =
                        ReloadStatusReporting::at_runtime_boundary,
                },
            },
            .runtime = RuntimeReloadParticipant{
                .boundary = RuntimeReloadBoundary::render_start,
                .apply = [this](RuntimeReloadTrigger trigger) {
                    if (pending_shader_runtime_result_) {
                        auto result = std::move(*pending_shader_runtime_result_);
                        pending_shader_runtime_result_.reset();
                        return result;
                    }
                    auto *gate = FastModuleContainer::tryGet<ReloadGate>();
                    if (gate == nullptr) return RuntimeReloadResult{};
                    if (!gate->shaderReloadEnabled()) {
                        // F-5: an explicit request passes a gate shut only
                        // for the file watch (an RPC driver, or no watch
                        // asked for); replay and strict assets refuse it.
                        if (trigger != RuntimeReloadTrigger::requested) return RuntimeReloadResult{};
                        auto refusal = gate->explicitShaderReloadRefusal();
                        if (!refusal.empty()) {
                            return RuntimeReloadResult{
                                .error_code = std::string{shaderReloadUnavailableErrorCode},
                                .error_fields = {{"reason", std::move(refusal)}},
                            };
                        }
                    }
                    if (trigger == RuntimeReloadTrigger::poll) return RuntimeReloadResult{};
                    return forceShaderReload();
                },
                .describe = [this](nlohmann::json &output) {
                    const auto *gate = FastModuleContainer::tryGet<ReloadGate>();
                    const auto gate_status = gate != nullptr ? gate->snapshot() : ReloadGateSnapshot{};
                    const auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
                    const auto tracking = library != nullptr
                                              ? library->reloadTrackingStatus()
                                              : ShaderReloadTrackingStatus{};
                    output = {
                        {"source", "file_watcher"},
                        {"enabled", gate != nullptr && gate_status.enabled},
                        {"available", library != nullptr &&
                                          FastModuleContainer::tryGet<PipelineFactory>() != nullptr},
                        {"tracked_units", tracking.units},
                        {"tracked_bundles", tracking.bundles},
                        {"tracked_dependencies", tracking.dependencies},
                        {"cache_hits", shader_cache_hits_},
                        {"cache_misses", shader_cache_misses_},
                        {"gate_reason", gate_status.reason.empty()
                                            ? nlohmann::json(nullptr)
                                            : nlohmann::json(gate_status.reason)},
                    };
                    // VI-1b: every pass still drawing with the shader before
                    // a reload it refused, whichever reload that was (a later
                    // reload of another shader clears last_error, not this).
                    auto refused = nlohmann::json::array();
                    if (const auto *container =
                            FastModuleContainer::tryGet<FullscreenPassContainer>()) {
                        for (const auto &refusal : container->reloadRefusals()) {
                            refused.push_back(nlohmann::json(reloadRefusalJson(refusal, true)));
                        }
                    }
                    output["refused_passes"] = std::move(refused);
                },
            },
        });
    }

    if (!has_participant(gameLogicReloadParticipantName)) {
        registerParticipant(ReloadParticipant{
            .name = std::string{gameLogicReloadParticipantName},
            .runtime = RuntimeReloadParticipant{
                .boundary = RuntimeReloadBoundary::frame_start,
                .apply = [](RuntimeReloadTrigger trigger) {
                    if (!configuredGameLogicStatus().configured) return RuntimeReloadResult{};
                    const auto *config = FastModuleContainer::tryGet<EngineLaunchConfig>();
                    // Automatic DLL polling historically belongs to the
                    // interactive loop. Headless/RPC drivers reload only via
                    // their explicit manual command.
                    if (trigger == RuntimeReloadTrigger::poll && config != nullptr &&
                        (config->headless || config->rpc)) {
                        return RuntimeReloadResult{};
                    }
                    const auto attempt = trigger == RuntimeReloadTrigger::manual
                                             ? reloadConfiguredGameLogicAttempt(true)
                                             : pollConfiguredGameLogicAttempt(
                                                   trigger == RuntimeReloadTrigger::requested);
                    return RuntimeReloadResult{
                        .attempted = attempt.attempted,
                        .committed = attempt.committed,
                        .error = attempt.error,
                        .error_code = attempt.error_code,
                        .error_fields = attempt.error_fields,
                    };
                },
                .describe = [](nlohmann::json &output) {
                    const auto status = configuredGameLogicStatus();
                    output = {
                        {"configured", status.configured},
                        {"loaded", status.loaded},
                        {"reloading", status.reloading},
                        {"pending", status.pending},
                        {"source_busy_retries", status.source_busy_retries},
                        {"generation", status.generation},
                        {"systems", status.system_count},
                        {"source", status.source.empty()
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(status.source.generic_string())},
                        {"loaded_copy", status.loaded_copy.empty()
                                            ? nlohmann::json(nullptr)
                                            : nlohmann::json(status.loaded_copy.generic_string())},
                        {"domain_error", status.last_error.empty()
                                             ? nlohmann::json(nullptr)
                                             : nlohmann::json(status.last_error)},
                        {"error_code", status.error_code.empty()
                                           ? nlohmann::json(nullptr)
                                           : nlohmann::json(status.error_code)},
                        {"error_fields", status.error_fields},
                    };
                },
            },
        });
    }

    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    const bool model_participant_registered = has_participant(modelReloadParticipantName);
    if (models != model_participant_source_ ||
        (models != nullptr && !model_participant_registered)) {
        if (model_participant_registered) unregisterParticipant(modelReloadParticipantName);
        model_participant_source_ = models;
        if (models != nullptr) {
            models->attachReloadCoordinator(transactions_);
            registerParticipant(ReloadParticipant{
                .name = std::string{modelReloadParticipantName},
                .file = FileReloadRole{
                    .claims = [models](const ReloadRequest &request) {
                        return models->handlesReload(request.key);
                    },
                    .apply = BatchReloadApply{
                        .apply_batch = [this, models](
                                           std::span<const ReloadRequest> requests) {
                            const auto before = transactions_.status();
                            transactions_.enqueue(
                                models->makeReloadGroup(requests, transactions_));
                            transactions_.applyFrame(retireSink());
                            const auto after = transactions_.status();
                            return after.failed == before.failed &&
                                   after.applied > before.applied;
                        },
                    },
                    .retire = [models](std::shared_ptr<const void> payload,
                                       ReloadCoordinator &coordinator) noexcept {
                        return models->retireReloadPayload(std::move(payload), coordinator);
                    },
                },
            });
        }
    }

    auto *materials = FastModuleContainer::tryGet<MaterialContainer>();
    const bool material_participant_registered = has_participant(materialReloadParticipantName);
    if (materials == material_participant_source_ &&
        (materials == nullptr || material_participant_registered)) return;

    if (material_participant_registered) unregisterParticipant(materialReloadParticipantName);
    material_participant_source_ = materials;
    if (materials == nullptr) return;

    registerParticipant(ReloadParticipant{
        .name = std::string{materialReloadParticipantName},
        .file = FileReloadRole{
            .claims = [materials](const ReloadRequest &request) {
                return materials->handlesTextureReload(request.key) ||
                       materials->handlesMaterialValuesReload(request.key);
            },
            .apply = PerRequestReloadApply{
                .enqueue = [materials](const ReloadRequest &request,
                                       ReloadCoordinator &coordinator) {
                    return materials->enqueueTextureReload(request, coordinator) ||
                           materials->enqueueMaterialValuesReload(request, coordinator);
                },
            },
            .retire = [materials](std::shared_ptr<const void> payload,
                                  ReloadCoordinator &coordinator) noexcept {
                return materials->retireTextureReloadPayload(payload, coordinator) ||
                       materials->retireMaterialValuesReloadPayload(std::move(payload),
                                                                    coordinator);
            },
        },
    });
}

void ReloadService::setup(const PathResolver &resolver) {
    if (watcher_) watcher_->stop();
    ensureBuiltInParticipants();
    std::vector<WatchStore> stores;
    stores.push_back({"project", resolver.projectRoot(), {}});
    for (const auto &store : resolver.stores()) stores.push_back({store.name, store.root, store.mount});
    auto *gate = &GET_MODULE(ReloadGate);
    watcher_ = std::make_unique<FileWatcher>(std::move(stores), [gate] { return gate->snapshot(); });
    watcher_->start();
}

void ReloadService::applyFrame() {
    ensureBuiltInParticipants();
    // Runtime state is replaced before file-backed resources and before any
    // frame phase can observe it. Render-boundary participants run separately.
    (void)applyRuntimeBoundary(RuntimeReloadBoundary::frame_start);
    if (watcher_) {
        watcher_->applyFrameBatch([this](std::span<const ReloadRequest> requests) {
            return applyRequests(requests);
        });
    }
    transactions_.applyFrame(retireSink());
}

ReloadCoordinator::RetireSink ReloadService::retireSink() {
    return [this](std::shared_ptr<const void> payload) noexcept {
        // Indexed, not a range-for: a retirement callback may register or
        // unregister participants and reallocate the vector under us.
        for (std::size_t index = 0; index < participants_.size(); ++index) {
            if (!participants_[index].file ||
                !participants_[index].file->retire) {
                continue;
            }
            const auto name = participants_[index].name;
            try {
                if (participants_[index].file->retire(payload, transactions_)) return;
            } catch (const std::exception &error) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' retirement failed: {}",
                              name, error.what());
                }
            } catch (...) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' retirement failed with an unknown error",
                              name);
                }
            }
        }
        // CPU-only registry payloads need no delayed destruction. Future GPU
        // handlers identify their own payload here and forward ownership to
        // DeletionQueue through the same RetireSink.
    };
}

bool ReloadService::applyRequest(const ReloadRequest &request) {
    const auto results = applyRequests(std::span{&request, std::size_t{1}});
    return !results.empty() && results.front();
}

bool ReloadService::applyClaimedRequest(std::size_t claimant,
                                        const ReloadRequest &request) {
    const auto claimant_name = participants_[claimant].name;
    const auto enqueue =
        std::get<PerRequestReloadApply>(participants_[claimant].file->apply).enqueue;
    if (!enqueue(request, transactions_)) {
        if (logger) {
            LOG_ERROR(logger, "reload participant '{}' claimed '{}' but did not enqueue it",
                      claimant_name, assetKeyString(request.key));
        }
        return false;
    }
    const auto before = transactions_.status();
    transactions_.applyFrame(retireSink());
    const auto after = transactions_.status();
    return after.failed == before.failed && after.applied > before.applied;
}

bool ReloadService::applyClaimedBatch(
    std::size_t claimant,
    std::span<const ReloadRequest> requests) {
    // Everything the status update needs is read before the callback runs:
    // the callback may reallocate `participants_`.
    const auto claimant_name = participants_[claimant].name;
    const bool bills_here = participants_[claimant].runtime.has_value();
    const auto apply_batch =
        std::get<BatchReloadApply>(participants_[claimant].file->apply).apply_batch;
    bool applied = false;
    std::string error;
    try {
        applied = apply_batch(requests);
        if (!applied) {
            error = "file-triggered batch apply failed";
        }
    } catch (const std::exception &caught) {
        error = caught.what();
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' batch failed: {}",
                claimant_name, caught.what());
        }
    } catch (...) {
        error = "unknown file-triggered batch failure";
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' batch failed with an unknown error",
                claimant_name);
        }
    }
    if (bills_here) {
        auto &status = runtime_status_[claimant_name];
        ++status.attempted;
        if (applied) {
            ++status.applied;
            status.last_error.clear();
        } else {
            ++status.failed;
            status.last_error = std::move(error);
        }
    }
    return applied;
}

bool ReloadService::applyClaimedCompanionBatch(
    std::size_t claimant,
    std::span<const ReloadRequest> requests,
    std::span<const ReloadRequest>
        companion_requests) {
    const auto claimant_name = participants_[claimant].name;
    const auto &route = std::get<CompanionBatchReloadApply>(
        participants_[claimant].file->apply);
    // A participant that bills its own transaction at a runtime boundary must
    // not be counted here as well.
    const bool bills_here =
        participants_[claimant].runtime.has_value() &&
        route.status_reporting == ReloadStatusReporting::immediate;
    const auto apply = route.apply;
    bool applied = false;
    std::string error;
    try {
        applied = apply(
            requests, companion_requests);
        if (!applied) {
            error =
                "file-triggered coordinated batch apply failed";
        }
    } catch (const std::exception &caught) {
        error = caught.what();
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' coordinated batch "
                "failed: {}",
                claimant_name, caught.what());
        }
    } catch (...) {
        error =
            "unknown file-triggered coordinated batch failure";
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' coordinated batch "
                "failed with an unknown error",
                claimant_name);
        }
    }
    if (bills_here) {
        auto &status =
            runtime_status_[claimant_name];
        ++status.attempted;
        if (applied) {
            ++status.applied;
            status.last_error.clear();
        } else {
            ++status.failed;
            status.last_error =
                std::move(error);
        }
    }
    return applied;
}

std::vector<std::size_t> ReloadService::companionOwnerOrder() const {
    std::vector<std::size_t> owners;
    for (std::size_t index = 0; index < participants_.size(); ++index) {
        const auto &participant = participants_[index];
        if (participant.file &&
            std::holds_alternative<CompanionBatchReloadApply>(
                participant.file->apply)) {
            owners.push_back(index);
        }
    }
    if (owners.size() < 2) return owners;

    // An owner that another owner names as a companion must run later, so the
    // outer transaction absorbs the inner one's requests instead of finding
    // them already consumed. Registration order only breaks ties.
    const auto companions_of =
        [this](std::size_t participant) -> const std::vector<std::string> & {
        return std::get<CompanionBatchReloadApply>(
                   participants_[participant].file->apply)
            .companion_participants;
    };
    std::vector<std::vector<std::size_t>> after(owners.size());
    std::vector<std::size_t> waiting(owners.size(), 0);
    for (std::size_t owner = 0; owner < owners.size(); ++owner) {
        const auto &names = companions_of(owners[owner]);
        for (std::size_t other = 0; other < owners.size(); ++other) {
            if (other == owner) continue;
            if (std::find(names.begin(), names.end(),
                          participants_[owners[other]].name) == names.end()) {
                continue;
            }
            after[owner].push_back(other);
            ++waiting[other];
        }
    }

    std::vector<std::size_t> order;
    order.reserve(owners.size());
    std::vector<bool> placed(owners.size(), false);
    while (order.size() < owners.size()) {
        std::size_t next = owners.size();
        for (std::size_t owner = 0; owner < owners.size(); ++owner) {
            if (!placed[owner] && waiting[owner] == 0) {
                next = owner;
                break;
            }
        }
        if (next == owners.size()) break;
        placed[next] = true;
        order.push_back(owners[next]);
        for (const auto blocked : after[next]) --waiting[blocked];
    }
    if (order.size() != owners.size()) {
        if (logger) {
            LOG_WARNING(logger,
                        "reload companion participants form a cycle; applying "
                        "batch owners in registration order");
        }
        return owners;
    }
    return order;
}

std::vector<bool> ReloadService::applyRequests(
    std::span<const ReloadRequest> requests) {
    ensureBuiltInParticipants();
    // One slot per request. Every pass below reads `pending()` instead of
    // repeating the guard, and an applied slot is never touched again.
    std::vector<RequestSlot> slots(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        slots[index].request = &requests[index];
    }
    const auto companion_route =
        [this](std::size_t participant) -> const CompanionBatchReloadApply * {
        const auto &candidate = participants_[participant];
        if (!candidate.file) return nullptr;
        return std::get_if<CompanionBatchReloadApply>(&candidate.file->apply);
    };
    const auto accepts = [&](std::size_t owner, std::size_t other,
                             const ReloadRequest &request) {
        const auto *route = companion_route(owner);
        return route != nullptr &&
            std::find(route->companion_participants.begin(),
                      route->companion_participants.end(),
                      participants_[other].name) !=
                route->companion_participants.end() &&
            (!route->companion_claims ||
             route->companion_claims(participants_[other].name, request));
    };
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto &request = requests[index];
        auto &slot = slots[index];
        for (std::size_t participant = 0; participant < participants_.size();
             ++participant) {
            if (!participants_[participant].file) continue;
            const auto participant_name = participants_[participant].name;
            bool claimed = false;
            try {
                claimed = participants_[participant].file->claims(request);
            } catch (const std::exception &error) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' claim failed for '{}': {}",
                              participant_name, assetKeyString(request.key), error.what());
                }
                slot.ok = false;
                break;
            } catch (...) {
                if (logger) {
                    LOG_ERROR(logger,
                              "reload participant '{}' claim failed for '{}' with an unknown error",
                              participant_name, assetKeyString(request.key));
                }
                slot.ok = false;
                break;
            }
            if (!claimed) continue;
            if (slot.claimant != no_claimant) {
                if (accepts(slot.claimant, participant, request)) {
                    slot.shared_claim = true;
                    continue;
                }
                if (!slot.shared_claim && accepts(participant, slot.claimant, request)) {
                    slot.shared_claim = true;
                    slot.claimant = participant;
                    continue;
                }
                if (logger) {
                    LOG_ERROR(logger, "reload request '{}' is claimed by both '{}' and '{}'",
                              assetKeyString(request.key),
                              participants_[slot.claimant].name, participant_name);
                }
                slot.ok = false;
                break;
            }
            slot.claimant = participant;
        }
    }

    for (const auto owner : companionOwnerOrder()) {
        // The order was computed before any apply ran. A callback that
        // registers or unregisters participants invalidates it; skip rather
        // than address a different participant by a stale index.
        if (owner >= participants_.size() || !participants_[owner].file ||
            !std::holds_alternative<CompanionBatchReloadApply>(
                participants_[owner].file->apply)) {
            continue;
        }
        const auto &route = std::get<CompanionBatchReloadApply>(
            participants_[owner].file->apply);
        std::vector<std::size_t> owned_indices;
        std::vector<std::size_t> companion_indices;
        std::vector<ReloadRequest> owned_batch;
        std::vector<ReloadRequest> companion_batch;
        for (std::size_t index = 0;
             index < requests.size(); ++index) {
            const auto &slot = slots[index];
            if (!slot.pending()) continue;
            if (slot.claimant == owner) {
                owned_indices.push_back(index);
                owned_batch.push_back(*slot.request);
                if (slot.shared_claim) companion_batch.push_back(*slot.request);
                continue;
            }
            const auto &claimant_name = participants_[slot.claimant].name;
            const auto named = std::find(
                route.companion_participants.begin(),
                route.companion_participants.end(),
                claimant_name);
            if (named ==
                route.companion_participants.end()) {
                continue;
            }
            if (route.companion_claims &&
                !route.companion_claims(
                    claimant_name,
                    *slot.request)) {
                continue;
            }
            companion_indices.push_back(index);
            companion_batch.push_back(
                *slot.request);
        }
        if (owned_batch.empty()) {
            continue;
        }
        const bool applied =
            applyClaimedCompanionBatch(
                owner, owned_batch,
                companion_batch);
        for (const auto index : owned_indices) {
            slots[index].ok = applied;
            slots[index].done = true;
        }
        for (const auto index :
             companion_indices) {
            slots[index].ok = applied;
            slots[index].done = true;
        }
    }

    for (std::size_t participant = 0; participant < participants_.size();
         ++participant) {
        if (!participants_[participant].file ||
            !std::holds_alternative<BatchReloadApply>(
                participants_[participant].file->apply)) {
            continue;
        }
        std::vector<std::size_t> indices;
        std::vector<ReloadRequest> batch;
        for (std::size_t index = 0;
             index < requests.size(); ++index) {
            if (!slots[index].pending() ||
                slots[index].claimant != participant) {
                continue;
            }
            indices.push_back(index);
            batch.push_back(*slots[index].request);
        }
        if (batch.empty()) continue;
        const bool applied =
            applyClaimedBatch(participant, batch);
        for (const auto index : indices) {
            slots[index].ok = applied;
            slots[index].done = true;
        }
    }

    // Only per-request claimants can still be pending: a batch owner with a
    // pending slot always had a non-empty batch above.
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (!slots[index].pending()) continue;
        const auto claimant = slots[index].claimant;
        if (claimant >= participants_.size() || !participants_[claimant].file ||
            !std::holds_alternative<PerRequestReloadApply>(
                participants_[claimant].file->apply)) {
            if (logger) {
                LOG_ERROR(logger,
                          "reload request '{}' lost its claimant before apply",
                          assetKeyString(slots[index].request->key));
            }
            slots[index].ok = false;
            slots[index].done = true;
            continue;
        }
        slots[index].ok = applyClaimedRequest(claimant, *slots[index].request);
        slots[index].done = true;
    }

    std::vector<bool> results(requests.size(), true);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        results[index] = slots[index].ok;
    }
    return results;
}

bool ReloadService::applyRequestForTesting(const ReloadRequest &request) {
    return applyRequest(request);
}

std::vector<bool> ReloadService::applyRequestsForTesting(
    std::span<const ReloadRequest> requests) {
    return applyRequests(requests);
}

nlohmann::json ReloadService::runtimeStatusJson() const {
    auto result = nlohmann::json::object();
    for (const auto &participant : participants_) {
        if (!participant.runtime) continue;
        const auto found = runtime_status_.find(participant.name);
        const RuntimeParticipantStatus empty;
        const auto &status = found != runtime_status_.end() ? found->second : empty;
        auto details = nlohmann::json::object();
        if (participant.runtime->describe) {
            try {
                participant.runtime->describe(details);
            } catch (const std::exception &error) {
                details = {{"status_error", error.what()}};
            } catch (...) {
                details = {{"status_error", "unknown runtime reload status failure"}};
            }
        }
        result[participant.name] = {
            {"boundary", runtimeBoundaryName(participant.runtime->boundary)},
            {"requested", requested_runtime_reloads_.contains(participant.name)},
            {"attempted", status.attempted},
            {"applied", status.applied},
            {"failed", status.failed},
            {"last_error", status.last_error.empty()
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(status.last_error)},
            {"error_code", status.error_code.empty()
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(status.error_code)},
            {"error_fields", status.error_fields},
            {"details", std::move(details)},
        };
    }
    return result;
}

ReloadTransitionFacts ReloadService::transitionFacts() const {
    ReloadTransitionFacts facts;
    if (!watcher_) {
        facts.state = "disabled";
        facts.epoch = GET_MODULE(ReloadGate).snapshot().epoch;
    } else {
        const auto status = watcher_->status();
        facts.state = std::string{watcherStateName(status.state)};
        facts.epoch = status.epoch;
    }
    for (const auto &participant : participants_) {
        if (!participant.runtime) continue;
        const auto found = runtime_status_.find(participant.name);
        const RuntimeParticipantStatus empty;
        const auto &status = found != runtime_status_.end() ? found->second : empty;
        facts.participants.push_back({
            .name = participant.name,
            .requested = requested_runtime_reloads_.contains(participant.name),
            .attempted = status.attempted,
            .applied = status.applied,
            .failed = status.failed,
            .error_code = status.error_code,
            .last_error = status.last_error,
        });
    }
    return facts;
}

nlohmann::json ReloadService::statusJson() const {
    const auto gate = GET_MODULE(ReloadGate).snapshot();
    const auto resources = transactions_.status();
    auto resource_error = nlohmann::json(nullptr);
    if (resources.last_reload_error) {
        resource_error = {{"path", resources.last_reload_error->path},
                          {"kind", resources.last_reload_error->kind},
                          {"message", resources.last_reload_error->message}};
    }
    auto model_catalog = nlohmann::json(nullptr);
    if (const auto *models =
            FastModuleContainer::tryGet<ModelAssetContainer>())
        model_catalog = models->occurrenceCatalogStatus();
    if (!watcher_) {
        return {{"state", "disabled"}, {"epoch", gate.epoch},
                {"error", gate.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(gate.reason)},
                {"applied", resources.applied}, {"failed", resources.failed},
                {"participants", participantNames()},
                {"runtime", runtimeStatusJson()},
                {"model_catalog", model_catalog},
                {"last_reload_error", std::move(resource_error)}};
    }
    const auto status = watcher_->status();
    return {{"state", watcherStateName(status.state)}, {"epoch", status.epoch},
            {"error", status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error)},
            {"applied", resources.applied}, {"failed", resources.failed},
            {"participants", participantNames()},
            {"runtime", runtimeStatusJson()},
            {"model_catalog", model_catalog},
            {"last_reload_error", std::move(resource_error)}};
}

} // namespace Pelican::watch

