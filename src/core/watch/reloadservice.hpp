#pragma once

#include "../container.hpp"
#include "filewatcher.hpp"
#include "reloadtransaction.hpp"
#include "reloadtransition.hpp"
#include "../../project/renderconfigdocument.hpp"

#include <functional>
#include <cstdint>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {
class MaterialContainer;
class ModelAssetContainer;
class PathResolver;
struct RendererRuntimeGeneration;
}

namespace Pelican::watch {

inline constexpr std::string_view materialReloadParticipantName = "pelican.materials";
// EB-2. A material document asset_data lists that the engine refused at start
// (ProjectMaterialAssetContainer::refusedDocument): a change of its file the
// watch sees reads it again and, when it registers, takes it in
// (ProjectMaterialAssetContainer::recoverDocument). A registered document's
// change stays with pelican.materials (the values reload).
inline constexpr std::string_view materialDocumentReloadParticipantName =
    "pelican.material_documents";
inline constexpr std::string_view shaderReloadParticipantName = "pelican.shaders";
// F-5. The reload_shaders RPC's refusals.
inline constexpr std::string_view shaderReloadFailedErrorCode =
    "shader_reload_failed@1";
inline constexpr std::string_view shaderReloadUnavailableErrorCode =
    "shader_reload_unavailable@1";
// VI-1b. A committed shader reload that one or more fullscreen / raster
// passes refused: the reloaded shader's pass-input set disagrees with the
// pass's inputs, so the pass keeps drawing with its current pipeline.
// error_fields.refused_passes names them. reload_shaders answers it as a
// result, not an error; reload status keeps it as a failure.
inline constexpr std::string_view shaderReloadRefusedErrorCode =
    "shader_reload_refused@1";
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
    bool staged = false;
    std::uint64_t staged_generation = 0;
    std::string error;
    std::string error_code;
    nlohmann::ordered_json error_fields = nlohmann::ordered_json::object();

    bool failed() const noexcept { return !error.empty(); }
};

struct RuntimeReloadSummary {
    std::size_t attempted = 0;
    std::size_t committed = 0;
    std::size_t failed = 0;
};

struct AuthoredRuntimeReloadResult {
    bool attempted = false;
    bool committed = false;
    bool staged = false;
    std::uint64_t staged_generation = 0;
    std::uint64_t published_generation = 0;
    std::string error;
    std::string post_commit_error;
    // Q-14e: the exception `error` was read from, when there was one.
    std::exception_ptr exception;
};

using RenderPipelineCompanionReloadObserver =
    std::function<void(std::span<const ReloadRequest>)>;

class ScopedRenderPipelineCompanionReloadObserver {
    RenderPipelineCompanionReloadObserver previous_;

  public:
    explicit ScopedRenderPipelineCompanionReloadObserver(
        RenderPipelineCompanionReloadObserver observer);
    ~ScopedRenderPipelineCompanionReloadObserver();
    ScopedRenderPipelineCompanionReloadObserver(
        const ScopedRenderPipelineCompanionReloadObserver &) = delete;
    ScopedRenderPipelineCompanionReloadObserver &operator=(
        const ScopedRenderPipelineCompanionReloadObserver &) = delete;
};

struct RuntimeReloadParticipant {
    RuntimeReloadBoundary boundary = RuntimeReloadBoundary::frame_start;
    // The participant owns its domain-specific prepare/commit/rollback/retire
    // transaction. ReloadService owns safe-boundary scheduling and reporting.
    std::function<RuntimeReloadResult(RuntimeReloadTrigger)> apply;
    std::function<void(nlohmann::json &)> describe;
};

// Where a file-triggered batch is counted in the runtime status table.
// `immediate` bills the apply as soon as the watcher frame delivers it.
// `at_runtime_boundary` means the participant carries its own result forward
// and bills it from its runtime transaction instead, so ReloadService must not
// count the same apply twice.
enum class ReloadStatusReporting : std::uint8_t {
    immediate,
    at_runtime_boundary,
};

// Apply route 1: one call per claimed request.
struct PerRequestReloadApply {
    std::function<bool(const ReloadRequest &, ReloadCoordinator &)> enqueue;
};

// Apply route 2: requests claimed by one participant in the same watcher frame
// are delivered once. This is intended for whole-domain transactions such as
// render-pipeline recompilation, where rebuilding once per changed dependency
// would publish intermediate generations.
struct BatchReloadApply {
    std::function<bool(std::span<const ReloadRequest>)> apply_batch;
};

// Apply route 3: a batch owner absorbs selected requests claimed by other named
// participants so all affected domains publish one generation.
// A source claimed by both the owner and an accepted companion is delivered in
// both spans of one coordinated call; unrelated ambiguous claims fail.
// Owners run in topological order over these edges: an owner named as another
// owner's companion is applied after that owner, so the outer transaction keeps
// its companions no matter which participant registered first.
struct CompanionBatchReloadApply {
    std::vector<std::string> companion_participants;
    std::function<bool(
        std::string_view,
        const ReloadRequest &)>
        companion_claims;
    std::function<bool(
        std::span<const ReloadRequest>,
        std::span<const ReloadRequest>)>
        apply;
    ReloadStatusReporting status_reporting =
        ReloadStatusReporting::immediate;
};

// A participant that answers the file watcher. `claims` decides ownership, the
// variant decides how owned requests are delivered, and the apply routes are
// mutually exclusive by construction rather than by a runtime validator.
struct FileReloadRole {
    std::function<bool(const ReloadRequest &)> claims;
    std::variant<PerRequestReloadApply, BatchReloadApply,
                 CompanionBatchReloadApply>
        apply;
    // Retirement callbacks must not throw. ReloadService still guards the
    // boundary so one faulty participant cannot terminate frame teardown.
    std::function<bool(std::shared_ptr<const void>, ReloadCoordinator &)> retire;
};

// Engine-owned authoring documents may supply private candidate bytes.
// The participant must preflight them, invoke source_commit as the last
// fallible pre-publication operation, and then publish without throwing.
struct AuthoredCandidateReloadRole {
    std::function<AuthoredRuntimeReloadResult(
        RenderConfigCandidateDocumentSet,
        std::function<void()>,
        std::function<void(AuthoredRuntimeReloadResult)>)>
        apply;
};

struct ReloadParticipant {
    std::string name;
    std::optional<FileReloadRole> file;
    std::optional<RuntimeReloadParticipant> runtime;
    std::optional<AuthoredCandidateReloadRole> authored;
};

DECLARE_MODULE(ReloadService) {
  public:
    ReloadService() = default;
    ~ReloadService();
    // Called once the startup resources are loaded, and again (PJ-1) when
    // project.json's hot_reload or asset_stores change in place: the watcher
    // is replaced by one over the resolver's stores and declaration as they
    // are now (its inventory seeded from the disk as it is now).
    void setup(const PathResolver &resolver);
    void applyFrame();
    RuntimeReloadSummary applyRuntimeBoundary(RuntimeReloadBoundary boundary);
    RuntimeReloadResult applyRuntimeNow(
        std::string_view name,
        RuntimeReloadTrigger trigger = RuntimeReloadTrigger::manual);
    // F-5. What the last explicit shader reload found to have moved: the
    // primary sources it recompiled (or failed to), and its cache counts.
    struct ShaderReloadChanges {
        std::vector<AssetKey> changed;
        std::size_t cache_hits = 0;
        std::size_t cache_misses = 0;
    };
    const ShaderReloadChanges &lastShaderReloadChanges() const noexcept {
        return last_shader_reload_changes_;
    }
    // True once after an explicit shader reload committed outside the
    // render_start boundary: the renderer rebinds its fullscreen inputs, as
    // it does after a publication billed at that boundary.
    bool takeShaderPublication() noexcept {
        return std::exchange(shader_publication_pending_, false);
    }
    AuthoredRuntimeReloadResult applyAuthoredCandidate(
        std::string_view name,
        RenderConfigCandidateDocumentSet candidate,
        std::function<void()> source_commit,
        std::function<void(AuthoredRuntimeReloadResult)> completion = {});
    bool requestRuntimeReload(std::string_view name);
    nlohmann::json statusJson() const;
    // The typed subset of statusJson() that the editor gate treats as a
    // transition. Participant `details` are excluded by construction.
    ReloadTransitionFacts transitionFacts() const;
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
    std::unique_ptr<PreparedReloadTransaction> prepareRenderPipelineModelReload(
        std::span<const ReloadRequest> requests);
    // MT-1 (bind_material): the models `keys` name rebuilt now, through one
    // model reload transaction committed before this returns (the same group
    // a file change of theirs would enqueue). Throws what the transaction
    // threw; nothing is published then.
    void reloadModelsNow(std::span<const AssetKey> keys);

  private:
    struct RuntimeParticipantStatus {
        std::uint64_t attempted = 0;
        std::uint64_t applied = 0;
        std::uint64_t failed = 0;
        std::string last_error;
        std::string error_code;
        nlohmann::ordered_json error_fields = nlohmann::ordered_json::object();
    };

    // One request of the watcher frame. `claimant` is an index into
    // `participants_`, never a pointer: an apply callback may register or
    // unregister participants and reallocate that vector.
    static constexpr std::size_t no_claimant =
        static_cast<std::size_t>(-1);
    struct RequestSlot {
        const ReloadRequest *request = nullptr;
        std::size_t claimant = no_claimant;
        // Claimed by the owner and by one of its accepted companions, so it
        // belongs in both spans of the coordinated call.
        bool shared_claim = false;
        bool ok = true;
        bool done = false;
        // No pass may touch a slot that failed, was already applied, or that
        // nobody claimed.
        bool pending() const noexcept {
            return ok && !done && claimant != no_claimant;
        }
    };

    void ensureBuiltInParticipants();
    ReloadCoordinator::RetireSink retireSink();
    bool applyRequest(const ReloadRequest &request);
    // `excluded_participant` (HW-1) does not claim any of these requests.
    std::vector<bool> applyRequests(std::span<const ReloadRequest> requests,
                                    std::string_view excluded_participant = {});
    std::vector<std::size_t> companionOwnerOrder() const;
    bool applyClaimedRequest(std::size_t claimant,
                             const ReloadRequest &request);
    bool applyClaimedBatch(
        std::size_t claimant,
        std::span<const ReloadRequest> requests);
    bool applyClaimedCompanionBatch(
        std::size_t claimant,
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
    // HW-1: project.json `hot_reload` as of setup (nullopt: undeclared).
    std::optional<std::vector<std::string>> watch_paths_;
    // PJ-1: setup again (edit_project_settings) replaces the watcher; the
    // FW-1 totals of the watchers it replaced, so status keeps counting.
    std::uint64_t carried_scans_ = 0;
    std::uint64_t carried_hashed_files_ = 0;
    std::uint64_t carried_hashed_bytes_ = 0;
    ReloadCoordinator transactions_;
    std::vector<ReloadParticipant> participants_;
    std::unordered_map<std::string, RuntimeParticipantStatus> runtime_status_;
    std::unordered_set<std::string> requested_runtime_reloads_;
    std::optional<RuntimeReloadResult> pending_shader_runtime_result_;
    ShaderReloadChanges last_shader_reload_changes_;
    bool shader_publication_pending_ = false;
    std::uint64_t shader_cache_hits_ = 0;
    std::uint64_t shader_cache_misses_ = 0;
    MaterialContainer *material_participant_source_ = nullptr;
    ModelAssetContainer *model_participant_source_ = nullptr;
};

} // namespace Pelican::watch

