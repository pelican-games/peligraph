#include "reloadservice.hpp"

#include "../asset/model.hpp"
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

namespace Pelican::watch {

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
    if (participant.name.empty()) {
        throw std::invalid_argument("reload participant name cannot be empty");
    }
    const bool has_apply =
        static_cast<bool>(participant.enqueue) ||
        static_cast<bool>(participant.apply_batch);
    const bool has_file_callbacks =
        participant.claims || has_apply ||
        participant.retire;
    if (participant.enqueue &&
        participant.apply_batch) {
        throw std::invalid_argument(
            "reload participant '" + participant.name +
            "' cannot register both per-request and batch apply callbacks");
    }
    if (participant.apply_with_companions &&
        (!participant.apply_batch ||
         participant.companion_participants.empty())) {
        throw std::invalid_argument(
            "reload participant '" + participant.name +
            "' companion apply requires apply_batch and at "
            "least one companion participant");
    }
    if ((!participant.companion_participants.empty() ||
         participant.companion_claims) &&
        !participant.apply_with_companions) {
        throw std::invalid_argument(
            "reload participant '" + participant.name +
            "' declares companions without a companion apply "
            "callback");
    }
    std::unordered_set<std::string>
        companion_names;
    for (const auto &companion :
         participant.companion_participants) {
        if (companion.empty() ||
            companion == participant.name ||
            !companion_names.insert(companion).second) {
            throw std::invalid_argument(
                "reload participant '" +
                participant.name +
                "' has an invalid companion participant");
        }
    }
    if (static_cast<bool>(participant.claims) != has_apply) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' requires claims plus one apply callback");
    }
    if (has_file_callbacks &&
        (!participant.claims || !has_apply)) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' cannot register a retire callback by itself");
    }
    if (participant.runtime && !participant.runtime->apply) {
        throw std::invalid_argument("runtime reload participant '" + participant.name +
                                    "' requires an apply callback");
    }
    if (!has_file_callbacks && !participant.runtime) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' has no file or runtime callbacks");
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
        result = {.attempted = true, .error = error.what()};
    } catch (...) {
        result = {.attempted = true, .error = "unknown runtime reload failure"};
    }

    if (result.committed || !result.error.empty()) result.attempted = true;
    auto &status = runtime_status_[participant.name];
    if (result.attempted) ++status.attempted;
    if (result.committed) ++status.applied;
    if (!result.error.empty()) {
        ++status.failed;
        status.last_error = result.error;
        if (logger) {
            LOG_WARNING(logger, "runtime reload participant '{}' failed: {}",
                        participant.name, result.error);
        }
    } else if (result.attempted) {
        status.last_error.clear();
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

RuntimeReloadResult ReloadService::applyRuntimeNow(std::string_view name) {
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
    return invokeRuntimeParticipant(*found, RuntimeReloadTrigger::manual);
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
                std::move(prepared), material_commit);
            runtime.committed = rebuilt.committed;
            if (!rebuilt.committed) {
                runtime.error = rebuilt.last_error.empty()
                                    ? "shader/pipeline candidate transaction failed"
                                    : rebuilt.last_error;
            }
        }
    } catch (const std::exception &error) {
        runtime.error = error.what();
    } catch (...) {
        runtime.error = "unknown shader reload transaction failure";
    }
    if (!runtime.committed) library->recordReloadFailure(changed_keys, runtime.error);
    mergeShaderRuntimeResult(runtime);
    return runtime.committed;
}

void ReloadService::applyRenderPipelineCompanionReload(
    const RendererRuntimeGeneration &generation,
    std::span<const ReloadRequest>
        companion_requests) {
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
                material_plan.pipeline_overrides);
        if (!rebuilt.committed) {
            throw std::runtime_error(
                rebuilt.last_error.empty()
                    ? "coordinated shader/material/pipeline "
                      "candidate transaction failed"
                    : rebuilt.last_error);
        }
        runtime.committed =
            runtime.attempted;
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
    try {
        auto prepared = library->prepareReloadAll();
        changed_keys = prepared.changed_keys;
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
        const auto rebuilt = pipelines->rebuildPrepared(std::move(prepared), material_commit);
        runtime.committed = rebuilt.committed;
        if (!rebuilt.committed) {
            runtime.error = rebuilt.last_error.empty()
                                ? "forced shader/pipeline transaction failed"
                                : rebuilt.last_error;
        }
    } catch (const std::exception &error) {
        runtime.error = error.what();
    } catch (...) {
        runtime.error = "unknown forced shader reload failure";
    }
    if (!runtime.committed) library->recordReloadFailure(changed_keys, runtime.error);
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
            .claims = [](const ReloadRequest &request) {
                const auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
                return library != nullptr && library->handlesReload(request.key);
            },
            .enqueue = [this](const ReloadRequest &request, ReloadCoordinator &) {
                return applyShaderReloadBatch(std::span{&request, std::size_t{1}}, {});
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
                    if (gate == nullptr || !gate->shaderReloadEnabled()) return RuntimeReloadResult{};
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
                    };
                },
                .describe = [](nlohmann::json &output) {
                    const auto status = configuredGameLogicStatus();
                    output = {
                        {"configured", status.configured},
                        {"loaded", status.loaded},
                        {"reloading", status.reloading},
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
                .claims = [models](const ReloadRequest &request) {
                    return models->handlesReload(request.key);
                },
                .enqueue = [models](const ReloadRequest &request,
                                    ReloadCoordinator &coordinator) {
                    return models->enqueueReload(request, coordinator);
                },
                .retire = [models](std::shared_ptr<const void> payload,
                                   ReloadCoordinator &coordinator) noexcept {
                    return models->retireReloadPayload(std::move(payload), coordinator);
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
        .claims = [materials](const ReloadRequest &request) {
            return materials->handlesTextureReload(request.key) ||
                   materials->handlesMaterialValuesReload(request.key);
        },
        .enqueue = [materials](const ReloadRequest &request, ReloadCoordinator &coordinator) {
            return materials->enqueueTextureReload(request, coordinator) ||
                   materials->enqueueMaterialValuesReload(request, coordinator);
        },
        .retire = [materials](std::shared_ptr<const void> payload,
                              ReloadCoordinator &coordinator) noexcept {
            return materials->retireTextureReloadPayload(payload, coordinator) ||
                   materials->retireMaterialValuesReloadPayload(std::move(payload), coordinator);
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
        for (const auto &participant : participants_) {
            if (!participant.retire) continue;
            try {
                if (participant.retire(payload, transactions_)) return;
            } catch (const std::exception &error) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' retirement failed: {}",
                              participant.name, error.what());
                }
            } catch (...) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' retirement failed with an unknown error",
                              participant.name);
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

bool ReloadService::applyClaimedRequest(ReloadParticipant &claimant,
                                        const ReloadRequest &request) {
    const auto claimant_name = claimant.name;
    const auto enqueue = claimant.enqueue;
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
    ReloadParticipant &claimant,
    std::span<const ReloadRequest> requests) {
    bool applied = false;
    std::string error;
    try {
        applied = claimant.apply_batch(requests);
        if (!applied) {
            error = "file-triggered batch apply failed";
        }
    } catch (const std::exception &caught) {
        error = caught.what();
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' batch failed: {}",
                claimant.name, caught.what());
        }
    } catch (...) {
        error = "unknown file-triggered batch failure";
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' batch failed with an unknown error",
                claimant.name);
        }
    }
    if (claimant.runtime) {
        auto &status = runtime_status_[claimant.name];
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
    ReloadParticipant &claimant,
    std::span<const ReloadRequest> requests,
    std::span<const ReloadRequest>
        companion_requests) {
    bool applied = false;
    std::string error;
    try {
        applied = claimant.apply_with_companions(
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
                claimant.name, caught.what());
        }
    } catch (...) {
        error =
            "unknown file-triggered coordinated batch failure";
        if (logger) {
            LOG_ERROR(
                logger,
                "reload participant '{}' coordinated batch "
                "failed with an unknown error",
                claimant.name);
        }
    }
    if (claimant.runtime) {
        auto &status =
            runtime_status_[claimant.name];
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

std::vector<bool> ReloadService::applyRequests(
    std::span<const ReloadRequest> requests) {
    ensureBuiltInParticipants();
    std::vector<bool> results(requests.size(), true);
    std::vector<ReloadParticipant *> claimants(requests.size(), nullptr);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto &request = requests[index];
        for (auto &participant : participants_) {
            if (!participant.claims) continue;
            bool claimed = false;
            try {
                claimed = participant.claims(request);
            } catch (const std::exception &error) {
                if (logger) {
                    LOG_ERROR(logger, "reload participant '{}' claim failed for '{}': {}",
                              participant.name, assetKeyString(request.key), error.what());
                }
                results[index] = false;
                break;
            } catch (...) {
                if (logger) {
                    LOG_ERROR(logger,
                              "reload participant '{}' claim failed for '{}' with an unknown error",
                              participant.name, assetKeyString(request.key));
                }
                results[index] = false;
                break;
            }
            if (!claimed) continue;
            if (claimants[index] != nullptr) {
                if (logger) {
                    LOG_ERROR(logger, "reload request '{}' is claimed by both '{}' and '{}'",
                              assetKeyString(request.key), claimants[index]->name,
                              participant.name);
                }
                results[index] = false;
                break;
            }
            claimants[index] = &participant;
        }
    }

    std::vector<bool> consumed(requests.size(), false);
    for (auto &participant : participants_) {
        if (!participant.apply_with_companions) {
            continue;
        }
        std::vector<std::size_t> owned_indices;
        std::vector<std::size_t> companion_indices;
        std::vector<ReloadRequest> owned_batch;
        std::vector<ReloadRequest> companion_batch;
        for (std::size_t index = 0;
             index < requests.size(); ++index) {
            if (!results[index] || consumed[index] ||
                claimants[index] == nullptr) {
                continue;
            }
            if (claimants[index] == &participant) {
                owned_indices.push_back(index);
                owned_batch.push_back(requests[index]);
                continue;
            }
            const auto named = std::find(
                participant.companion_participants.begin(),
                participant.companion_participants.end(),
                claimants[index]->name);
            if (named ==
                participant.companion_participants.end()) {
                continue;
            }
            if (participant.companion_claims &&
                !participant.companion_claims(
                    claimants[index]->name,
                    requests[index])) {
                continue;
            }
            companion_indices.push_back(index);
            companion_batch.push_back(
                requests[index]);
        }
        if (owned_batch.empty()) {
            continue;
        }
        const bool applied =
            applyClaimedCompanionBatch(
                participant, owned_batch,
                companion_batch);
        for (const auto index : owned_indices) {
            results[index] = applied;
            consumed[index] = true;
        }
        for (const auto index :
             companion_indices) {
            results[index] = applied;
            consumed[index] = true;
        }
    }

    std::vector<std::size_t> shader_indices;
    std::vector<std::size_t> material_value_indices;
    auto *materials =
        FastModuleContainer::tryGet<MaterialContainer>();
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (!results[index] || consumed[index] ||
            claimants[index] == nullptr) {
            continue;
        }
        if (claimants[index]->name == shaderReloadParticipantName) {
            shader_indices.push_back(index);
        } else if (materials != nullptr &&
                   claimants[index]->name == materialReloadParticipantName &&
                   materials->handlesMaterialValuesReload(requests[index].key)) {
            material_value_indices.push_back(index);
        }
    }

    if (!shader_indices.empty()) {
        std::vector<ReloadRequest> shader_requests;
        std::vector<AssetKey> material_documents;
        shader_requests.reserve(shader_indices.size());
        material_documents.reserve(material_value_indices.size());
        for (const auto index : shader_indices) {
            shader_requests.push_back(requests[index]);
            consumed[index] = true;
        }
        for (const auto index : material_value_indices) {
            material_documents.push_back(requests[index].key);
            consumed[index] = true;
        }
        const bool applied = applyShaderReloadBatch(shader_requests, material_documents);
        for (const auto index : shader_indices) results[index] = applied;
        for (const auto index : material_value_indices) results[index] = applied;
    }

    for (auto &participant : participants_) {
        if (!participant.apply_batch) continue;
        std::vector<std::size_t> indices;
        std::vector<ReloadRequest> batch;
        for (std::size_t index = 0;
             index < requests.size(); ++index) {
            if (!results[index] || consumed[index] ||
                claimants[index] != &participant) {
                continue;
            }
            indices.push_back(index);
            batch.push_back(requests[index]);
        }
        if (batch.empty()) continue;
        const bool applied =
            applyClaimedBatch(participant, batch);
        for (const auto index : indices) {
            results[index] = applied;
            consumed[index] = true;
        }
    }

    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (!results[index] || consumed[index] || claimants[index] == nullptr) continue;
        results[index] = applyClaimedRequest(*claimants[index], requests[index]);
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
            {"details", std::move(details)},
        };
    }
    return result;
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
    if (!watcher_) {
        return {{"state", "disabled"}, {"epoch", gate.epoch},
                {"error", gate.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(gate.reason)},
                {"applied", resources.applied}, {"failed", resources.failed},
                {"participants", participantNames()},
                {"runtime", runtimeStatusJson()},
                {"last_reload_error", std::move(resource_error)}};
    }
    const auto status = watcher_->status();
    return {{"state", watcherStateName(status.state)}, {"epoch", status.epoch},
            {"error", status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error)},
            {"applied", resources.applied}, {"failed", resources.failed},
            {"participants", participantNames()},
            {"runtime", runtimeStatusJson()},
            {"last_reload_error", std::move(resource_error)}};
}

} // namespace Pelican::watch

