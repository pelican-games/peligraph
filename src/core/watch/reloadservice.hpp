#pragma once

#include "../container.hpp"
#include "filewatcher.hpp"
#include "reloadtransaction.hpp"

#include <functional>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {
class MaterialContainer;
class ModelAssetContainer;
class PathResolver;
struct RendererRuntimeGeneration;
}

namespace Pelican::watch {

inline constexpr std::string_view materialReloadParticipantName = "pelican.materials";
inline constexpr std::string_view shaderReloadParticipantName = "pelican.shaders";
inline constexpr std::string_view gameLogicReloadParticipantName = "pelican.game_logic";
inline constexpr std::string_view modelReloadParticipantName = "pelican.models";
inline constexpr std::string_view renderPipelineReloadParticipantName =
    "pelican.render_pipeline";

enum class RuntimeReloadBoundary : std::uint8_t {
    frame_start,
    render_start,
};

enum class RuntimeReloadTrigger : std::uint8_t {
    poll,
    requested,
    manual,
};

struct RuntimeReloadResult {
    bool attempted = false;
    bool committed = false;
    std::string error;

    bool failed() const noexcept { return !error.empty(); }
};

struct RuntimeReloadSummary {
    std::size_t attempted = 0;
    std::size_t committed = 0;
    std::size_t failed = 0;
};

struct RuntimeReloadParticipant {
    RuntimeReloadBoundary boundary = RuntimeReloadBoundary::frame_start;
    // The participant owns its domain-specific prepare/commit/rollback/retire
    // transaction. ReloadService owns safe-boundary scheduling and reporting.
    std::function<RuntimeReloadResult(RuntimeReloadTrigger)> apply;
    std::function<void(nlohmann::json &)> describe;
};

struct ReloadParticipant {
    std::string name;
    std::function<bool(const ReloadRequest &)> claims;
    std::function<bool(const ReloadRequest &, ReloadCoordinator &)> enqueue;
    // Requests claimed by one participant in the same watcher frame are
    // delivered once. This is intended for whole-domain transactions such as
    // render-pipeline recompilation, where rebuilding once per changed
    // dependency would publish intermediate generations.
    std::function<bool(std::span<const ReloadRequest>)> apply_batch;
    // A batch owner may absorb selected requests claimed by other named
    // participants so all affected domains publish one generation. The owner
    // still needs apply_batch for ordinary standalone batches.
    std::vector<std::string> companion_participants;
    std::function<bool(
        std::string_view,
        const ReloadRequest &)>
        companion_claims;
    std::function<bool(
        std::span<const ReloadRequest>,
        std::span<const ReloadRequest>)>
        apply_with_companions;
    // Retirement callbacks must not throw. ReloadService still guards the
    // boundary so one faulty participant cannot terminate frame teardown.
    std::function<bool(std::shared_ptr<const void>, ReloadCoordinator &)> retire;
    std::optional<RuntimeReloadParticipant> runtime;
};

DECLARE_MODULE(ReloadService) {
  public:
    ReloadService() = default;
    ~ReloadService();
    void setup(const PathResolver &resolver);
    void applyFrame();
    RuntimeReloadSummary applyRuntimeBoundary(RuntimeReloadBoundary boundary);
    RuntimeReloadResult applyRuntimeNow(std::string_view name);
    bool requestRuntimeReload(std::string_view name);
    nlohmann::json statusJson() const;
    FileWatcher *watcherForTesting() noexcept { return watcher_.get(); }
    ReloadCoordinator &transactions() noexcept { return transactions_; }
    bool applyRequestForTesting(const ReloadRequest &request);
    std::vector<bool> applyRequestsForTesting(std::span<const ReloadRequest> requests);
    void registerParticipant(ReloadParticipant participant);
    bool unregisterParticipant(std::string_view name) noexcept;
    std::vector<std::string> participantNames() const;
    // Invoked by the render-graph pre-publication hook. Throws on any shader,
    // material, or graphics-pipeline candidate failure so the graph arena can
    // roll back without publishing a mixed generation.
    void applyRenderPipelineCompanionReload(
        const RendererRuntimeGeneration &generation,
        std::span<const ReloadRequest>
            companion_requests);

  private:
    struct RuntimeParticipantStatus {
        std::uint64_t attempted = 0;
        std::uint64_t applied = 0;
        std::uint64_t failed = 0;
        std::string last_error;
    };

    void ensureBuiltInParticipants();
    ReloadCoordinator::RetireSink retireSink();
    bool applyRequest(const ReloadRequest &request);
    std::vector<bool> applyRequests(std::span<const ReloadRequest> requests);
    bool applyClaimedRequest(ReloadParticipant &claimant,
                             const ReloadRequest &request);
    bool applyClaimedBatch(
        ReloadParticipant &claimant,
        std::span<const ReloadRequest> requests);
    bool applyClaimedCompanionBatch(
        ReloadParticipant &claimant,
        std::span<const ReloadRequest> requests,
        std::span<const ReloadRequest>
            companion_requests);
    bool applyShaderReloadBatch(std::span<const ReloadRequest> shader_requests,
                                std::span<const AssetKey> material_documents);
    RuntimeReloadResult forceShaderReload();
    void mergeShaderRuntimeResult(RuntimeReloadResult result);
    RuntimeReloadResult invokeRuntimeParticipant(ReloadParticipant &participant,
                                                 RuntimeReloadTrigger trigger);
    nlohmann::json runtimeStatusJson() const;
    std::unique_ptr<FileWatcher> watcher_;
    ReloadCoordinator transactions_;
    std::vector<ReloadParticipant> participants_;
    std::unordered_map<std::string, RuntimeParticipantStatus> runtime_status_;
    std::unordered_set<std::string> requested_runtime_reloads_;
    std::optional<RuntimeReloadResult> pending_shader_runtime_result_;
    std::uint64_t shader_cache_hits_ = 0;
    std::uint64_t shader_cache_misses_ = 0;
    MaterialContainer *material_participant_source_ = nullptr;
    ModelAssetContainer *model_participant_source_ = nullptr;
};

} // namespace Pelican::watch

