#include "reloadservice.hpp"

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
    const bool has_file_callbacks = participant.claims || participant.enqueue || participant.retire;
    if (static_cast<bool>(participant.claims) != static_cast<bool>(participant.enqueue)) {
        throw std::invalid_argument("reload participant '" + participant.name +
                                    "' requires both claims and enqueue callbacks");
    }
    if (has_file_callbacks && (!participant.claims || !participant.enqueue)) {
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

void ReloadService::ensureBuiltInParticipants() {
    const auto has_participant = [this](std::string_view name) {
        return std::ranges::any_of(participants_, [&](const auto &participant) {
            return participant.name == name;
        });
    };

    if (!has_participant(shaderReloadParticipantName)) {
        registerParticipant(ReloadParticipant{
            .name = std::string{shaderReloadParticipantName},
            .runtime = RuntimeReloadParticipant{
                .boundary = RuntimeReloadBoundary::render_start,
                .apply = [](RuntimeReloadTrigger) {
                    auto *gate = FastModuleContainer::tryGet<ReloadGate>();
                    if (gate == nullptr || !gate->shaderPollEnabled()) return RuntimeReloadResult{};

                    auto *library = FastModuleContainer::tryGet<ShaderLibrary>();
                    auto *pipelines = FastModuleContainer::tryGet<PipelineFactory>();
                    if (library == nullptr || pipelines == nullptr) return RuntimeReloadResult{};

                    const auto shaders = library->pollModifiedSources();
                    if (shaders.modified_bundles == 0) return RuntimeReloadResult{};

                    RuntimeReloadResult result{
                        .attempted = true,
                        .committed = shaders.reloaded_bundles != 0,
                    };
                    if (shaders.failed_bundles != 0) {
                        appendReloadError(
                            result.error,
                            "shader reload failed for " + std::to_string(shaders.failed_bundles) +
                                " bundle(s): " + shaders.last_error);
                    }
                    if (shaders.reloaded_bundles != 0) {
                        const auto rebuilt = pipelines->rebuildDirty();
                        if (rebuilt.failed_pipelines != 0) {
                            appendReloadError(
                                result.error,
                                "pipeline rebuild failed for " +
                                    std::to_string(rebuilt.failed_pipelines) + " pipeline(s): " +
                                    rebuilt.last_error);
                        }
                    }
                    return result;
                },
                .describe = [](nlohmann::json &output) {
                    const auto *gate = FastModuleContainer::tryGet<ReloadGate>();
                    const auto gate_status = gate != nullptr ? gate->snapshot() : ReloadGateSnapshot{};
                    output = {
                        {"enabled", gate != nullptr && gate_status.enabled},
                        {"available", FastModuleContainer::tryGet<ShaderLibrary>() != nullptr &&
                                          FastModuleContainer::tryGet<PipelineFactory>() != nullptr},
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
        watcher_->applyFrame([this](const ReloadRequest &request) {
            return applyRequest(request);
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
    ensureBuiltInParticipants();

    ReloadParticipant *claimant = nullptr;
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
            return false;
        } catch (...) {
            if (logger) {
                LOG_ERROR(logger, "reload participant '{}' claim failed for '{}' with an unknown error",
                          participant.name, assetKeyString(request.key));
            }
            return false;
        }
        if (!claimed) continue;
        if (claimant != nullptr) {
            if (logger) {
                LOG_ERROR(logger, "reload request '{}' is claimed by both '{}' and '{}'",
                          assetKeyString(request.key), claimant->name, participant.name);
            }
            return false;
        }
        claimant = &participant;
    }
    if (claimant == nullptr) return true;
    const auto claimant_name = claimant->name;
    const auto enqueue = claimant->enqueue;
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

bool ReloadService::applyRequestForTesting(const ReloadRequest &request) {
    return applyRequest(request);
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

