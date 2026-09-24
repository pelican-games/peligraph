#include "../src/core/build_features.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/effecttransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/persistence/persistence.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#if PELICAN_WITH_RPC
#include "../src/core/communication/rpcserver.hpp"
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// PG-2b. Behavior::onDestroy is noexcept, so a facade that throws there ends
// the process in std::terminate before BehaviorAttachmentArena's catch sees
// it. In a destruction callback the audio and physics facades record the
// refusal of a module the build left out (build_feature_disabled@1) or of a
// query no provider answers (physics.provider_unavailable@1) and answer
// empty. These cases drive the production path -- a registered behavior
// destroyed by the arena, whose destroyInstance opens the EffectDestroyScope
// -- and run in every build; run_build_units_smoke.cmake runs the [pg2] tag
// in the audio-OFF, physics-OFF and provider-only builds, where the refusals
// happen, and requires the PG2B_ lines they print.

namespace Pelican {
namespace {

enum class DestroyProbe { none, audio, physics };
DestroyProbe destroy_probe = DestroyProbe::none;
std::vector<std::string> destroy_trace;

struct ProbeParams {
    std::string label;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&ProbeParams::label>("label"), "probe"));
};

class DestroyFacadeBehavior final : public Behavior {
  public:
    using Params = ProbeParams;

    void onDestroy(BehaviorContext &ctx) noexcept override {
        switch (destroy_probe) {
        case DestroyProbe::none:
            break;
        case DestroyProbe::audio: {
            const auto audio = ctx.audio();
            const auto state = audio.soundState(SoundHandle{{7}});
            destroy_trace.push_back(state ? "soundState:value" : "soundState:nullopt");
            const auto handle = audio.playSound("project://missing.wav");
            destroy_trace.push_back(handle.value == 0 ? "playSound:invalid"
                                                      : "playSound:valid");
            break;
        }
        case DestroyProbe::physics: {
            const auto physics = ctx.physics();
            const phys::Ray ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F};
            const phys::Sphere sphere{{0.0F, 0.0F, 0.0F}, 0.5F};
            const vec3 delta{1.0F, 0.0F, 0.0F};
            destroy_trace.push_back(physics.raycastAll(ray).empty()
                                        ? "raycastAll:empty" : "raycastAll:hits");
            destroy_trace.push_back(physics.raycastClosest(ray)
                                        ? "raycastClosest:hit" : "raycastClosest:nullopt");
            destroy_trace.push_back(physics.overlapAll(sphere).empty()
                                        ? "overlapAll:empty" : "overlapAll:hits");
            destroy_trace.push_back(physics.shapeCastAll(sphere, delta).empty()
                                        ? "shapeCastAll:empty" : "shapeCastAll:hits");
            destroy_trace.push_back(physics.shapeCastClosest(sphere, delta)
                                        ? "shapeCastClosest:hit"
                                        : "shapeCastClosest:nullopt");
            break;
        }
        }
        destroy_trace.push_back("returned");
    }
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(DestroyFacadeBehavior, "pg2b_destroy_facade", 1);

namespace {

// A throw out of the noexcept onDestroy calls std::terminate. Under the Debug
// CRT that would stop at an abort() dialog; the guard ends the process at
// once and by name instead, so the regression fails the run rather than
// hanging it.
class TerminateGuard {
    std::terminate_handler previous_;

  public:
    TerminateGuard()
        : previous_{std::set_terminate([] {
              std::fputs("PG2B_TERMINATE: a facade threw out of a noexcept onDestroy\n",
                         stderr);
              std::fflush(stderr);
              std::_Exit(3);
          })} {}
    ~TerminateGuard() { std::set_terminate(previous_); }
    TerminateGuard(const TerminateGuard &) = delete;
    TerminateGuard &operator=(const TerminateGuard &) = delete;
};

struct Sandbox {
    std::filesystem::path base;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_pg2b_destroy_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(base / "project");
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void configureRuntime(const Sandbox &sandbox) {
    setupLogger();
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(
        sandbox.base / "project", false,
        nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
                       {"name", "pg2b-destroy"},
                       {"engine_min_version", "0.1.0"}}.dump(),
        sandbox.base / "user");
    GET_MODULE(ECSPredefinedRegistration).reg();
    (void)GET_MODULE(BehaviorAttachmentArena);
}

// Attaches one pg2b_destroy_facade behavior to a fresh object and runs its
// activation, as a scene load's publish and activate do.
GameObjectId attachProbe() {
    auto prepared = prepareSceneBehaviorAttachments(
        nlohmann::json::array({{{"name", "Probe"},
                                {"components",
                                 nlohmann::json::array({{{"name", "behavior"},
                                                         {"type", "pg2b_destroy_facade"},
                                                         {"params", nlohmann::json::object()}}})}}}),
        BehaviorRegistryAvailability::active);
    REQUIRE(prepared.size() == 1);
    const auto entity =
        GameObjects::createWithComponents(std::span<const ComponentId>{}, {});
    std::vector<BoundSceneBehaviorAttachment> bound;
    bound.push_back({.prepared = std::move(prepared.front()), .entity = entity});
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(std::move(bound));
    arena.activatePublished();
    return entity;
}

// A scene load retiring the object: the arena destroys the behavior while the
// load's effect transaction is open, so its onDestroy runs in the destroy
// phase. Returns what the transaction recorded.
std::vector<CommittedWithEffectFailure::EffectError>
destroyInsideSceneTransaction(DestroyProbe probe) {
    (void)attachProbe();
    destroy_trace.clear();
    destroy_probe = probe;
    internal::PreparedEffectTransaction transaction{GET_MODULE(Persistence),
                                                    GET_MODULE(SceneLoader)};
    {
        internal::EffectTransactionScope scope{transaction};
        const TerminateGuard guard;
        GET_MODULE(BehaviorAttachmentArena).deactivateAll();
    }
    destroy_probe = DestroyProbe::none;
    return transaction.effectErrors();
}

// An object removed during play: no scene transaction is open, and the arena
// destroys the behavior on its own.
void destroyOutsideSceneTransaction(DestroyProbe probe) {
    const auto entity = attachProbe();
    destroy_trace.clear();
    destroy_probe = probe;
    {
        const TerminateGuard guard;
        GET_MODULE(BehaviorAttachmentArena).preDestroyEntity(entity);
    }
    destroy_probe = DestroyProbe::none;
}

[[maybe_unused]] void requireRecorded(
    const std::vector<CommittedWithEffectFailure::EffectError> &errors,
    const std::vector<std::string> &apis, std::string_view code) {
    REQUIRE(errors.size() == apis.size());
    for (std::size_t index = 0; index < apis.size(); ++index) {
        CAPTURE(index);
        REQUIRE(errors[index].api == apis[index]);
        REQUIRE(errors[index].error_code == std::string{code});
        REQUIRE(errors[index].phase == "destroy");
        REQUIRE(errors[index].error_fields.contains("reason"));
        std::cout << "PG2B_DESTROY_RECORDED api=" << errors[index].api
                  << " code=" << errors[index].error_code << '\n';
    }
}

} // namespace

// The rule itself, measured in every build: a refusal is recorded only inside
// a destruction callback, and is the caller's to throw everywhere else.
TEST_CASE("PG-2b a facade refusal is recorded only in a destruction callback",
          "[pg2][pg2b][destroy]") {
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureRuntime(sandbox);
    using Transaction = internal::PreparedEffectTransaction;
    const auto absorb = [](Transaction::ErrorApi api, Transaction::ErrorCode code) {
        return internal::absorbDestroyRefusal(api, code,
                                              Transaction::ErrorFields::handle_reason,
                                              7, "refused");
    };

    // No destruction callback: the facade throws.
    REQUIRE_FALSE(absorb(Transaction::ErrorApi::sound_state,
                         Transaction::ErrorCode::build_feature_disabled));
    // A destruction callback outside a scene load: the log carries it.
    {
        internal::EffectDestroyScope destroy_scope;
        REQUIRE(absorb(Transaction::ErrorApi::sound_state,
                       Transaction::ErrorCode::build_feature_disabled));
    }
    REQUIRE_FALSE(absorb(Transaction::ErrorApi::sound_state,
                         Transaction::ErrorCode::build_feature_disabled));

    Transaction transaction{GET_MODULE(Persistence), GET_MODULE(SceneLoader)};
    internal::EffectTransactionScope scope{transaction};
    // A scene load's activation callback throws.
    REQUIRE_FALSE(absorb(Transaction::ErrorApi::sound_state,
                         Transaction::ErrorCode::build_feature_disabled));
    REQUIRE(transaction.effectErrors().empty());
    {
        internal::EffectDestroyScope destroy_scope;
        REQUIRE(absorb(Transaction::ErrorApi::sound_state,
                       Transaction::ErrorCode::build_feature_disabled));
        REQUIRE(absorb(Transaction::ErrorApi::raycast_closest,
                       Transaction::ErrorCode::physics_provider_unavailable));
    }
    const auto errors = transaction.effectErrors();
    REQUIRE(errors.size() == 2);
    REQUIRE(errors[0].api == "soundState");
    REQUIRE(errors[0].error_code == "build_feature_disabled@1");
    REQUIRE(errors[0].phase == "destroy");
    REQUIRE(errors[0].error_fields.at("handle").get<std::uint64_t>() == 7);
    REQUIRE(errors[0].error_fields.at("reason").get<std::string>() == "refused");
    REQUIRE(errors[1].api == "raycastClosest");
    REQUIRE(errors[1].error_code == "physics.provider_unavailable@1");
}

// Acceptance 1: audio OFF, onDestroy asks soundState and playSound.
TEST_CASE("PG-2b an audio call in onDestroy of a build without audio is recorded",
          "[pg2][pg2b][destroy][audio]") {
#if PELICAN_WITH_AUDIO
    REQUIRE(PELICAN_WITH_AUDIO);
#else
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureRuntime(sandbox);

    const auto errors = destroyInsideSceneTransaction(DestroyProbe::audio);
    REQUIRE(destroy_trace == std::vector<std::string>{
                                 "soundState:nullopt", "playSound:invalid", "returned"});
    requireRecorded(errors, {"soundState", "playSound"}, "build_feature_disabled@1");
    REQUIRE(errors[0].error_fields.at("handle").get<std::uint64_t>() == 7);

    destroyOutsideSceneTransaction(DestroyProbe::audio);
    REQUIRE(destroy_trace == std::vector<std::string>{
                                 "soundState:nullopt", "playSound:invalid", "returned"});

    // Outside a destruction callback the same call still throws, a scene
    // load's activation callback included.
    const GameContext context;
    REQUIRE_THROWS_AS(context.audio().soundState(SoundHandle{{7}}),
                      BuildFeatureCallDisabledError);
    internal::PreparedEffectTransaction transaction{GET_MODULE(Persistence),
                                                    GET_MODULE(SceneLoader)};
    internal::EffectTransactionScope scope{transaction};
    REQUIRE_THROWS_AS(context.audio().soundState(SoundHandle{{7}}),
                      BuildFeatureCallDisabledError);
    REQUIRE(transaction.effectErrors().empty());
#endif
}

// Acceptance 2 and 3: physics OFF, or physics ON with no provider; onDestroy
// asks every query. With a provider the same queries answer and nothing is
// recorded.
TEST_CASE("PG-2b a physics query in onDestroy without physics or a provider is recorded",
          "[pg2][pg2b][destroy][physics]") {
    const Sandbox sandbox;
    FastModuleContainer modules;
    configureRuntime(sandbox);
    const std::vector<std::string> queries{"raycastAll", "raycastClosest", "overlapAll",
                                           "shapeCastAll", "shapeCastClosest"};
    const std::vector<std::string> empty_trace{
        "raycastAll:empty",   "raycastClosest:nullopt",   "overlapAll:empty",
        "shapeCastAll:empty", "shapeCastClosest:nullopt", "returned"};

    const auto errors = destroyInsideSceneTransaction(DestroyProbe::physics);
    REQUIRE(destroy_trace == empty_trace);
#if !PELICAN_WITH_PHYSICS
    constexpr std::string_view code = "build_feature_disabled@1";
#elif !PELICAN_WITH_BUILTIN_PHYSICS && !PELICAN_WITH_JOLT_PHYSICS
    constexpr std::string_view code = "physics.provider_unavailable@1";
#else
    constexpr std::string_view code{};
#endif
    if (code.empty()) {
        // A provider answers: an empty world, and nothing to record.
        REQUIRE(errors.empty());
        return;
    }
    requireRecorded(errors, queries, code);

    destroyOutsideSceneTransaction(DestroyProbe::physics);
    REQUIRE(destroy_trace == empty_trace);

    const GameContext context;
    const phys::Ray ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F};
    internal::PreparedEffectTransaction transaction{GET_MODULE(Persistence),
                                                    GET_MODULE(SceneLoader)};
    internal::EffectTransactionScope scope{transaction};
#if !PELICAN_WITH_PHYSICS
    REQUIRE_THROWS_AS(context.physics().raycastClosest(ray), BuildFeatureCallDisabledError);
#else
    REQUIRE_THROWS_AS(context.physics().raycastClosest(ray), phys::ProviderUnavailableError);
#endif
    REQUIRE(transaction.effectErrors().empty());
}

// Acceptance 4: the RPC raycast of a build without physics refuses by name
// instead of answering the stub's "hit": null.
TEST_CASE("PG-2b the RPC raycast of a build without physics is physics.module_unavailable@1",
          "[pg2][pg2b][rpc]") {
#if PELICAN_WITH_RPC && !PELICAN_WITH_PHYSICS
    setupLogger();
    FastModuleContainer modules;
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    internal::configurePhysicsRpcHandlers(server, GET_MODULE(PhysWorld));
    const auto response = nlohmann::json::parse(server.processLine(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "raycast"},
                       {"params",
                        {{"origin", {0, 0, 0}}, {"direction", {0, 0, 1}}, {"max_distance", 10}}}}
            .dump()));
    CAPTURE(response.dump());
    REQUIRE_FALSE(response.contains("result"));
    const auto &error = response.at("error");
    REQUIRE(error.at("code") == -32000);
    REQUIRE(error.at("data").at("code") == "physics.module_unavailable@1");
    REQUIRE(error.at("data").at("option") == "PELICAN_WITH_PHYSICS");
    REQUIRE_THAT(error.at("message").get<std::string>(),
                 Catch::Matchers::StartsWith("physics.module_unavailable@1:"));
    std::cout << "PG2B_RPC_RAYCAST code=" << error.at("data").at("code").get<std::string>()
              << '\n';
#else
    REQUIRE(true);
#endif
}

} // namespace Pelican
