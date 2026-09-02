#include "../src/core/loader/sceneload.hpp"
#include "../src/core/loader/sceneloadfault.hpp"
#include "../src/core/loader/sceneparticipantmanifest.hpp"
#include "../src/core/loader/scene.hpp"
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
static_assert(noexcept(sceneParticipantPresenceName(
    SceneParticipantPresence::real)));

TEST_CASE("WP380 result has only prepare early_publish and activate precommit phases",
          "[WP380][contract][result]") {
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::prepare) == 0);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::early_publish) == 1);
    REQUIRE(static_cast<unsigned>(SceneLoadPhase::activate) == 2);
    REQUIRE(std::variant_size_v<SceneLoadResult> == 3);
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
    constexpr auto expected_audio = SceneParticipantPresence::absent;
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
    for (const auto &row : rows) {
        REQUIRE_FALSE(row.owner.empty());
        REQUIRE_FALSE(row.field.empty());
        REQUIRE_FALSE(row.prepare_owner.empty());
        REQUIRE_FALSE(row.publication.empty());
        REQUIRE_FALSE(row.rollback.empty());
        REQUIRE_FALSE(row.retirement.empty());
        REQUIRE_FALSE(row.snapshot_schema.empty());
        REQUIRE_FALSE(row.mutation_id.empty());
        REQUIRE(keys.emplace(row.owner, row.field).second);
        REQUIRE(mutations.emplace(row.mutation_id).second);
    }

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
    REQUIRE(manifestRow("audio", "voice_and_handles").presence ==
            expected_audio);
    REQUIRE(manifestRow("root.rpc", "direct_load").presence == expected_rpc);
}

TEST_CASE("WP380 manifest fixes exclusions and all five production call sites",
          "[WP380][contract][manifest][roots]") {
    REQUIRE(manifestRow("model_asset", "warm_cache").role ==
            SceneManifestRole::cache);
    REQUIRE(manifestRow("registry_slot", "candidate_registry").presence ==
            SceneParticipantPresence::absent);
    REQUIRE(manifestRow("audio", "voice_and_handles").role ==
            SceneManifestRole::excluded_wp380b);
    REQUIRE(manifestRow("persistence", "bytes_and_settings").role ==
            SceneManifestRole::excluded_wp380b);

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

TEST_CASE("WP380 all five root disconnect mutations toggle the executor gate",
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
        test_support::ManifestSceneLoadFault injector{mutation};

        injector.setEnabled(false);
        REQUIRE_FALSE(internal::sceneLoadRootDisconnected(
            entry.root, &injector));
        REQUIRE(injector.saw(SceneLoadFaultPhase::prepare,
                             entry.owner));

        injector.resetCalls();
        injector.setEnabled(true);
        loader.setFaultInjectorForTesting(&injector);
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
    loader.setFaultInjectorForTesting(nullptr);
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
