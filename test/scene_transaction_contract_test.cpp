#include "../src/core/loader/sceneload.hpp"
#include "../src/core/loader/sceneloadfault.hpp"
#include "../src/core/loader/sceneparticipantmanifest.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/model/modelresourcerelease.hpp"
#include "../src/core/userpublic/details/event/registerer.hpp"
#include "scene_load_fault_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <type_traits>

namespace Pelican {
struct Wp380RollbackMutationEvent {
    std::int32_t scene_tag = 0;
};
} // namespace Pelican

PELICAN_REGISTER_EVENT(Pelican::Wp380RollbackMutationEvent);

namespace Pelican {
namespace {

const SceneParticipantManifestEntry &manifestRow(
    std::string_view owner, std::string_view field) {
    const auto rows = sceneParticipantManifest();
    const auto found = std::find_if(
        rows.begin(), rows.end(), [&](const auto &row) {
            return row.owner == owner && row.field == field;
        });
    REQUIRE(found != rows.end());
    return *found;
}

} // namespace

static_assert(std::is_move_constructible_v<SceneLoadRequest>);
static_assert(!std::is_copy_constructible_v<SceneLoadRequest>);
static_assert(noexcept(sceneParticipantManifest()));
static_assert(noexcept(scenePublicApiManifest()));
static_assert(noexcept(sceneParticipantPresenceName(
    SceneParticipantPresence::real)));

TEST_CASE("WP380c result phase enum covers the exact executor trace",
          "[WP380][contract][result]") {
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::prepare) == 0);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::early_publish) == 1);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::publish) == 2);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::activate) == 3);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::commit_metadata) == 4);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::retire) == 5);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::flush) == 6);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::finish) == 7);
    REQUIRE(std::variant_size_v<SceneLoadResult> == 3);
}

TEST_CASE("WP380d phase trace makes overflow observable",
          "[WP380d][contract][trace]") {
    SceneLoadPhaseTrace trace;
    constexpr std::array phases{
        SceneLoadPhase::prepare, SceneLoadPhase::early_publish,
        SceneLoadPhase::publish, SceneLoadPhase::activate,
        SceneLoadPhase::commit_metadata, SceneLoadPhase::retire,
        SceneLoadPhase::flush, SceneLoadPhase::finish,
    };
    for (const auto phase : phases) trace.record(phase);
    REQUIRE_FALSE(trace.overflow);
    trace.record(SceneLoadPhase::prepare);
    REQUIRE(trace.overflow);
    REQUIRE(std::ranges::equal(trace.view(), phases));
}

TEST_CASE("WP380c release report has fixed capacity and explicit overflow",
          "[WP380c][contract][model-release]") {
    ReleaseReport report;
    for (std::uint32_t index = 0; index < 10; ++index) {
        report.add({ReleaseStage::vertbuf_morph_delta,
                    ReleaseErrorCode::operation_failed, index});
    }
    REQUIRE(report.error_count == ReleaseReport::capacity);
    REQUIRE(report.overflow_count == 2);
    REQUIRE(report.errors.front().index == 0);
    REQUIRE(report.errors.back().index == 7);
}

TEST_CASE("WP380 field manifest is unique complete and build-derived",
          "[WP380][contract][manifest]") {
#if PELICAN_WITH_PHYSICS
    constexpr auto expected_physics = SceneParticipantPresence::real;
#else
    constexpr auto expected_physics = SceneParticipantPresence::stub;
#endif
#if PELICAN_WITH_SEQPLAYER
    constexpr auto expected_seq = SceneParticipantPresence::real;
#else
    constexpr auto expected_seq = SceneParticipantPresence::stub;
#endif
#if PELICAN_WITH_VAT
    constexpr auto expected_vat = SceneParticipantPresence::real;
#else
    constexpr auto expected_vat = SceneParticipantPresence::stub;
#endif
#if PELICAN_WITH_AUDIO
    constexpr auto expected_audio = SceneParticipantPresence::real;
#else
    constexpr auto expected_audio = SceneParticipantPresence::uninstalled;
#endif
#if PELICAN_WITH_RPC
    constexpr auto expected_rpc = SceneParticipantPresence::real;
#else
    constexpr auto expected_rpc = SceneParticipantPresence::absent;
#endif

    const auto rows = sceneParticipantManifest();
    REQUIRE_FALSE(rows.empty());
    std::set<std::pair<std::string, std::string>> keys;
    std::set<std::string> mutations;
    std::size_t real_participant_mutation_rows = 0;
    for (const auto &row : rows) {
        REQUIRE_FALSE(row.owner.empty());
        REQUIRE_FALSE(row.field.empty());
        REQUIRE_FALSE(row.prepare_owner.empty());
        REQUIRE_FALSE(row.publication.empty());
        REQUIRE_FALSE(row.rollback.empty());
        REQUIRE_FALSE(row.retirement.empty());
        REQUIRE_FALSE(row.snapshot_schema.empty());
        REQUIRE(keys.emplace(row.owner, row.field).second);
        if (!row.mutation_id.empty()) {
            REQUIRE(mutations.emplace(row.mutation_id).second);
        }

        if (row.role == SceneManifestRole::participant &&
            row.presence == SceneParticipantPresence::real &&
            !row.mutation_id.empty()) {
            ++real_participant_mutation_rows;
        }
        if (row.presence != SceneParticipantPresence::real ||
            row.role == SceneManifestRole::invariant ||
            row.role == SceneManifestRole::cache ||
            row.role == SceneManifestRole::excluded_wp380b ||
            row.role == SceneManifestRole::non_rollback ||
            row.role == SceneManifestRole::reject ||
            row.role == SceneManifestRole::frame_lifetime ||
            row.role == SceneManifestRole::read_only) {
            REQUIRE(row.mutation_id.empty());
        }
        if (row.role == SceneManifestRole::production_root) {
            REQUIRE(row.mutation_id.empty() ==
                    (row.presence != SceneParticipantPresence::real));
        }
    }
    // WP380c v3 measures only real participant rows backed by an executor
    // mutation call site. The other manifest rows are presence/schema
    // inventory, not pretend mutation participants.
    REQUIRE(real_participant_mutation_rows == 14);

    REQUIRE_FALSE(manifestRow("rng_sequence", "current_seed")
                      .mutation_id.empty());
    REQUIRE(manifestRow("rng_sequence", "increment").mutation_id.empty());
    REQUIRE(manifestRow("rng_sequence", "increment").role ==
            SceneManifestRole::invariant);

    REQUIRE(manifestRow("metadata", "document").presence ==
            SceneParticipantPresence::real);
    REQUIRE(std::string{manifestRow("metadata", "active_authority")
                            .publication} ==
            "commit_metadata:single-swap");
    REQUIRE(manifestRow("physics", "active_trigger_pairs").presence ==
            expected_physics);
    REQUIRE(manifestRow("seqplayer", "instances").presence ==
            expected_seq);
    REQUIRE(manifestRow("vatplayer", "instance").presence == expected_vat);
    REQUIRE(manifestRow("audio", "play").presence ==
            expected_audio);
    REQUIRE(manifestRow("root.rpc", "direct_load").presence == expected_rpc);
    REQUIRE_FALSE(manifestRow("procedural", "aabb_buffer")
                      .mutation_id.empty());
    REQUIRE_FALSE(manifestRow("procedural", "allocation_generation")
                      .mutation_id.empty());
    REQUIRE_FALSE(manifestRow("procedural", "instance_snapshot")
                      .mutation_id.empty());
    REQUIRE_FALSE(manifestRow("procedural", "retire_queue")
                      .mutation_id.empty());
}

TEST_CASE("WP380 manifest fixes exclusions and all five production call sites",
          "[WP380][contract][manifest][roots]") {
    REQUIRE(manifestRow("model_asset", "warm_cache").role ==
            SceneManifestRole::cache);
    REQUIRE(manifestRow("registry_slot", "candidate_registry").presence ==
            SceneParticipantPresence::absent);
    REQUIRE(manifestRow("audio", "play").role ==
            SceneManifestRole::effect);
    REQUIRE(manifestRow("persistence", "save_data").role ==
            SceneManifestRole::effect);

    for (const auto owner : {
             "root.startup", "root.rpc", "root.pending",
             "root.import.unversioned", "root.import.versioned"}) {
        REQUIRE(manifestRow(owner, owner == std::string_view{"root.pending"}
                                       ? "apply"
                                       : (owner == std::string_view{"root.import.unversioned"} ||
                                                  owner == std::string_view{"root.import.versioned"}
                                              ? "callback"
                                              : "direct_load"))
                    .role == SceneManifestRole::production_root);
    }
}

TEST_CASE("WP380 every installed root disconnect mutation toggles the executor gate",
          "[WP380][contract][manifest][roots][mutation]") {
    struct RootCase {
        SceneLoadRoot root;
        std::string_view owner;
        std::string_view field;
    };
    constexpr RootCase cases[]{
        {SceneLoadRoot::startup, "root.startup", "direct_load"},
        {SceneLoadRoot::rpc, "root.rpc", "direct_load"},
        {SceneLoadRoot::pending, "root.pending", "apply"},
        {SceneLoadRoot::import_unversioned,
         "root.import.unversioned", "callback"},
        {SceneLoadRoot::import_versioned,
         "root.import.versioned", "callback"},
    };
    auto document = AuthoringSceneDocument::load(
        R"({"schema":"pelican.scene","version":1,"scenes":{"known":{"objects":[]}}})",
        SceneRevision{1}, 1);
    auto projection = ResolvedSceneResolver::prepare(
        std::move(document), SceneResolverGeneration{1});
    SceneLoader loader;

    for (const auto &entry : cases) {
        const auto mutation = manifestRow(entry.owner, entry.field).mutation_id;
        INFO("mutation=" << mutation);
#if !PELICAN_WITH_RPC
        if (entry.root == SceneLoadRoot::rpc) {
            REQUIRE(mutation.empty());
            continue;
        }
#endif
        REQUIRE_FALSE(mutation.empty());
        test_support::ManifestSceneLoadFault injector{mutation};

        injector.setEnabled(false);
        REQUIRE_FALSE(internal::sceneLoadRootDisconnected(
            entry.root, &injector));
        REQUIRE(injector.saw(SceneLoadFaultPhase::prepare,
                             entry.owner));

        injector.resetCalls();
        injector.setEnabled(true);
        test_support::setSceneLoadFaultInjectorForTesting(loader, &injector);
        auto request = SceneLoadRequest::borrowed(
            "known", projection, entry.root);
        const auto result = loader.load(std::move(request));
        const auto *failure = std::get_if<PreCommitFailure>(&result);
        REQUIRE(failure != nullptr);
        REQUIRE(failure->phase == SceneLoadPhase::prepare);
        REQUIRE(failure->error == "injected scene-load root disconnect");
        REQUIRE(injector.saw(SceneLoadFaultPhase::prepare,
                             entry.owner));
    }
    test_support::setSceneLoadFaultInjectorForTesting(loader, nullptr);
}

TEST_CASE("WP380 event discard bypass leaves the C event in the live queue",
          "[WP380][transaction][event-outbox][mutation]") {
    auto &events = internal::getEventRegisterer();
    const auto mutation =
        manifestRow("event_outbox", "pending_events").mutation_id;
    test_support::ManifestSceneLoadFault injector{mutation};

    for (const bool enabled : {false, true}) {
        INFO("fault enabled=" << enabled);
        internal::clearPendingEvents();
        injector.resetCalls();
        injector.setEnabled(enabled);
        {
            internal::PreparedEventOutbox outbox{events, 0};
            {
                internal::EventOutboxScope scope{outbox};
                events.emit(Wp380RollbackMutationEvent{3});
            }
            internal::rollbackSceneEventOutbox(outbox, &injector);
        }

        REQUIRE(injector.saw(SceneLoadFaultPhase::flush,
                             "event_outbox"));
        REQUIRE(events.pendingEventCount() == (enabled ? 1U : 0U));
        if (enabled) {
            const auto pending = events.pendingEventsForTesting();
            REQUIRE(pending.front().type ==
                    std::type_index{typeid(Wp380RollbackMutationEvent)});
            REQUIRE(std::static_pointer_cast<
                        const Wp380RollbackMutationEvent>(
                        pending.front().payload)
                        ->scene_tag == 3);
        }
    }
    internal::clearPendingEvents();
}

TEST_CASE("WP380 pending-scene discard bypass publishes the C request",
          "[WP380][transaction][pending-scene][mutation]") {
    const auto mutation =
        manifestRow("pending_scene", "request").mutation_id;
    test_support::ManifestSceneLoadFault injector{mutation};

    for (const bool enabled : {false, true}) {
        std::optional<SceneId> live{"B_pending"};
        std::optional<SceneId> candidate{"C_pending"};
        injector.resetCalls();
        injector.setEnabled(enabled);
        internal::rollbackScenePendingCandidate(
            live, candidate, &injector);
        REQUIRE(injector.saw(SceneLoadFaultPhase::flush,
                             "pending_scene"));
        REQUIRE(live == std::optional<SceneId>{
                            enabled ? "C_pending" : "B_pending"});
    }
}

} // namespace Pelican
