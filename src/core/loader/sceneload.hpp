#pragma once

#include "resolvedscene.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

using SceneId = std::string;

struct SceneAuthority {
    std::uint64_t run_nonce = 0;
    std::uint64_t scene_epoch = 0;

    bool operator==(const SceneAuthority &) const = default;
};

enum class SceneLoadPhase : std::uint8_t {
    prepare,
    early_publish,
    publish,
    activate,
    commit_metadata,
    retire,
    flush,
    finish,
};

inline constexpr std::size_t sceneLoadPhaseCount = 8;

struct SceneLoadPhaseTrace {
    std::array<SceneLoadPhase, sceneLoadPhaseCount> phases{};
    std::uint8_t count = 0;
    // A trace is a fixed pre-commit allocation.  Never silently discard an
    // ordering observation: callers must reject an overflowing trace.
    bool overflow = false;

    void record(SceneLoadPhase phase) noexcept {
        if (count != 0 && phases[count - 1U] == phase) return;
        if (count < phases.size()) phases[count++] = phase;
        else overflow = true;
    }

    std::span<const SceneLoadPhase> view() const noexcept {
        return {phases.data(), count};
    }
};

static_assert(noexcept(std::declval<SceneLoadPhaseTrace &>().record(
    SceneLoadPhase::prepare)));

// Fault injection is deliberately orthogonal to the externally reported
// pre-commit phases.  It names every executor edge, including the no-throw
// commit/retire/flush tail, so a manifest row can exercise the exact edge it
// claims to cover.
enum class SceneLoadFaultPhase : std::uint8_t {
    prepare,
    publish,
    activate,
    commit,
    retire,
    flush,
};

class SceneLoadFaultInjector {
  public:
    virtual ~SceneLoadFaultInjector() = default;
    virtual bool shouldMutate(SceneLoadFaultPhase phase,
                              std::string_view participant_name,
                              std::string_view mutation_id) noexcept = 0;
};

enum class SceneLoadRoot : std::uint8_t {
    unspecified,
    startup,
    rpc,
    pending,
    import_unversioned,
    import_versioned,
};

namespace scene_load_mutation {
inline constexpr std::string_view metadata_document =
    "metadata.document.drop";
inline constexpr std::string_view metadata_projection =
    "metadata.projection.drop";
inline constexpr std::string_view ecs_liveness = "ecs.liveness.bypass";
inline constexpr std::string_view light_directional_names =
    "light.directional-names.bypass";
inline constexpr std::string_view event_pending = "queue.event.bypass";
inline constexpr std::string_view rng_current_seed =
    "queue.rng-current-seed.bypass";
inline constexpr std::string_view rng_state = "queue.rng-state.bypass";
inline constexpr std::string_view pending_scene =
    "queue.pending-scene.bypass";
inline constexpr std::string_view behavior_attachments =
    "behavior.attachments.bypass";
inline constexpr std::string_view transient_retire =
    "loader.transient-retire.bypass";
inline constexpr std::string_view root_startup = "root.startup.disconnect";
inline constexpr std::string_view root_rpc = "root.rpc.disconnect";
inline constexpr std::string_view root_pending = "root.pending.disconnect";
inline constexpr std::string_view root_import_unversioned =
    "root.import-v1.disconnect";
inline constexpr std::string_view root_import_versioned =
    "root.import-v2.disconnect";
inline constexpr std::string_view effect_audio_play =
    "effect.audio-play.fail";
inline constexpr std::string_view effect_audio_stop =
    "effect.audio-stop.fail";
inline constexpr std::string_view effect_audio_bus_volume =
    "effect.audio-bus-volume.fail";
inline constexpr std::string_view effect_save_data =
    "effect.save-data.fail";
inline constexpr std::string_view effect_save_settings =
    "effect.save-settings.fail";
} // namespace scene_load_mutation

struct PreCommitFailure {
    SceneLoadPhase phase = SceneLoadPhase::prepare;
    std::string error;
    std::exception_ptr exception;
    std::vector<std::string> release_errors;
};

struct CommittedWithEffectFailure {
    SceneAuthority active_authority{};
    struct EffectError {
        std::string api;
        std::string error_code;
        nlohmann::ordered_json error_fields;
        std::string phase;
    };
    std::vector<EffectError> effect_errors;
    SceneLoadPhaseTrace phase_trace{};
};

struct SceneLoadCommitted {
    SceneAuthority active_authority{};
    SceneLoadPhaseTrace phase_trace{};
};

using SceneLoadResult =
    std::variant<SceneLoadCommitted, PreCommitFailure,
                 CommittedWithEffectFailure>;

// Owns the one metadata candidate used by an imported scene.  Its definition
// is private to ProjectBasicConfig so a caller cannot publish only the
// document or only the resolved projection.
class MetadataCommitToken {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit MetadataCommitToken(std::unique_ptr<Impl> impl) noexcept;
    void prepareRuntimeIdentity(SceneId scene_id,
                                SceneAuthority authority,
                                SceneLoadPhaseTrace &phase_trace);
    void publishDocumentOnlyForFaultInjection() noexcept;
    void commit() noexcept;
    void commit(SceneLoadPhaseTrace &phase_trace) noexcept;
    friend class ProjectBasicConfig;
    friend class SceneLoader;

  public:
    MetadataCommitToken() noexcept;
    ~MetadataCommitToken();
    MetadataCommitToken(MetadataCommitToken &&) noexcept;
    MetadataCommitToken &operator=(MetadataCommitToken &&) noexcept;
    MetadataCommitToken(const MetadataCommitToken &) = delete;
    MetadataCommitToken &operator=(const MetadataCommitToken &) = delete;

    explicit operator bool() const noexcept { return impl_ != nullptr; }
    const SceneProjectionState &projection() const;
    const AuthoringSceneDocument &document() const;
};

struct SceneLoadRequest {
    struct Borrowed {
        std::reference_wrapper<const SceneProjectionState> committed;
    };

    struct Owned {
        std::reference_wrapper<const SceneProjectionState> candidate;
        std::reference_wrapper<const AuthoringSceneDocument> document;
        MetadataCommitToken token;

        explicit Owned(MetadataCommitToken metadata)
            : candidate{std::cref(metadata.projection())},
              document{std::cref(metadata.document())},
              token{std::move(metadata)} {}
    };

    SceneId scene_id;
    std::variant<Borrowed, Owned> source;
    bool consumes_pending_request = false;
    SceneLoadRoot root = SceneLoadRoot::unspecified;

    static SceneLoadRequest borrowed(
        SceneId scene_id, const SceneProjectionState &committed,
        SceneLoadRoot root = SceneLoadRoot::unspecified) {
        return SceneLoadRequest{
            .scene_id = std::move(scene_id),
            .source = Borrowed{std::cref(committed)},
            .root = root,
        };
    }

    static SceneLoadRequest owned(SceneId scene_id,
                                  MetadataCommitToken metadata,
                                  SceneLoadRoot root =
                                      SceneLoadRoot::unspecified) {
        return SceneLoadRequest{
            .scene_id = std::move(scene_id),
            .source = Owned{std::move(metadata)},
            .root = root,
        };
    }

    const SceneProjectionState &projection() const {
        if (const auto *borrowed = std::get_if<Borrowed>(&source)) {
            return borrowed->committed.get();
        }
        return std::get<Owned>(source).candidate.get();
    }
};

using SceneImportCallback =
    std::function<SceneLoadResult(SceneLoadRequest &&)>;

struct SceneImportResult {
    SceneRevision revision{};
    SceneLoadResult load_result{SceneLoadCommitted{}};

    SceneImportResult() = default;
    SceneImportResult(SceneRevision imported_revision)
        : revision{imported_revision} {}
    SceneImportResult(SceneRevision imported_revision,
                      SceneLoadResult imported_result)
        : revision{imported_revision},
          load_result{std::move(imported_result)} {}

    operator SceneRevision() const noexcept { return revision; }
};

// Production roots use this bridge to retain the historic throwing surface
// while the executor itself returns an explicit pre/post-commit result.
// Pre-commit failure preserves the historic throwing contract. A committed
// effect failure is still a successful scene publication and is returned to
// the caller for reporting.
const SceneLoadResult &requireSceneLoadCommitted(const SceneLoadResult &result);
nlohmann::ordered_json effectErrorsJson(
    std::span<const CommittedWithEffectFailure::EffectError> errors);

} // namespace Pelican
