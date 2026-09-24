#include "../src/core/build_features.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/effecttransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/persistence/persistence.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

// PG-2 acceptance 1 and 2, the facade half. The same translation unit runs in
// every build: with the module the facade is not asked here, without it every
// entry of the facade fails with one type, BuildFeatureCallDisabledError,
// whose code() is build_feature_disabled@1 and feature() the option.
// run_build_units_smoke.cmake runs the [pg2] tag in the audio-OFF and
// physics-OFF builds.

namespace Pelican {
namespace {

static_assert(std::is_base_of_v<BuildFeatureDisabledError, BuildFeatureCallDisabledError>);
static_assert(std::is_base_of_v<std::runtime_error, phys::ProviderUnavailableError>);

struct Call {
    const char *entry;
    std::function<void()> call;
};

// Every call must fail with the derived type; a code-less
// BuildFeatureDisabledError, a runtime_error or a return value is a failure.
[[maybe_unused]] void requireEveryCallDisabled(std::string_view option,
                                               const std::vector<Call> &calls) {
    for (const auto &call : calls) {
        CAPTURE(call.entry);
        bool refused = false;
        try {
            call.call();
        } catch (const BuildFeatureCallDisabledError &error) {
            refused = true;
            REQUIRE(std::string{error.code()} == "build_feature_disabled@1");
            REQUIRE(error.feature() == std::string{option});
            REQUIRE(std::string{error.what()} ==
                    buildFeatureDisabledMessage(
                        option, std::string{call.entry} + " is unavailable"));
            std::cout << "PG2_FACADE entry=" << call.entry << " code=" << error.code()
                      << " feature=" << error.feature() << '\n';
        }
        REQUIRE(refused);
    }
}

struct Sandbox {
    std::filesystem::path base;
    Sandbox() {
        base = std::filesystem::temp_directory_path() /
               ("pelican_pg2_facade_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(base / "project");
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

} // namespace

TEST_CASE("PG-2 the facade failure is a BuildFeatureDisabledError with a name",
          "[pg2][facade]") {
    // A game that catches the frozen base type still catches it, with the
    // same feature() and prose message the base always carried.
    try {
        throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", "playSound");
        FAIL("throwBuildFeatureCallDisabled returned");
    } catch (const BuildFeatureDisabledError &error) {
        REQUIRE(error.feature() == "PELICAN_WITH_AUDIO");
        REQUIRE(std::string{error.what()} ==
                "This binary was built with PELICAN_WITH_AUDIO=OFF: playSound is unavailable");
        REQUIRE(isBuildFeatureDisabledError(error));
        const auto *named = dynamic_cast<const BuildFeatureCallDisabledError *>(&error);
        REQUIRE(named != nullptr);
        REQUIRE(std::string{named->code()} == "build_feature_disabled@1");
    }
    const phys::ProviderUnavailableError unavailable{"raycast"};
    REQUIRE(std::string{unavailable.code()} == "physics.provider_unavailable@1");
    REQUIRE_THAT(std::string{unavailable.what()},
                 Catch::Matchers::StartsWith("physics.provider_unavailable@1:"));
}

TEST_CASE("PG-2 the audio facade answers a build without audio by name",
          "[pg2][facade][audio]") {
#if PELICAN_WITH_AUDIO
    REQUIRE(PELICAN_WITH_AUDIO);
#else
    const GameContext context;
    const auto audio = context.audio();
    requireEveryCallDisabled("PELICAN_WITH_AUDIO", {
        {"playSound", [&] { (void)audio.playSound("project://missing.wav"); }},
        {"stopSound", [&] { audio.stopSound(SoundHandle{{1}}); }},
        {"setBusVolume", [&] { audio.setBusVolume("master", 0.5F); }},
        {"busVolume", [&] { (void)audio.busVolume("master"); }},
        {"isPlaying", [&] { (void)audio.isPlaying(SoundHandle{{1}}); }},
        {"setSoundVolume", [&] { (void)audio.setSoundVolume(SoundHandle{{1}}, 0.5F); }},
        {"setSoundPitch", [&] { (void)audio.setSoundPitch(SoundHandle{{1}}, 1.0F); }},
        {"setSoundPaused", [&] { (void)audio.setSoundPaused(SoundHandle{{1}}, true); }},
        {"soundState", [&] { (void)audio.soundState(SoundHandle{{1}}); }},
    });
#endif
}

// The scene effect transaction stages the audio calls a scene callback makes.
// In an activation callback it throws the facade's type; in a destruction
// callback, where nothing may throw, it records the same name.
TEST_CASE("PG-2 scene effect staging refuses audio by the same name",
          "[pg2][facade][audio][effect]") {
#if PELICAN_WITH_AUDIO
    REQUIRE(PELICAN_WITH_AUDIO);
#else
    const Sandbox sandbox;
    FastModuleContainer modules;
    setupLogger();
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(
        sandbox.base / "project", false,
        nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
                       {"name", "pg2-facade"},
                       {"engine_min_version", "0.1.0"}}.dump(),
        sandbox.base / "user");
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    const GameContext context;
    const auto audio = context.audio();
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        requireEveryCallDisabled("PELICAN_WITH_AUDIO", {
            {"playSound", [&] { (void)audio.playSound("project://missing.wav"); }},
            {"stopSound", [&] { audio.stopSound(SoundHandle{{1}}); }},
            {"setBusVolume", [&] { audio.setBusVolume("master", 0.5F); }},
            {"busVolume", [&] { (void)audio.busVolume("master"); }},
            {"isPlaying", [&] { (void)audio.isPlaying(SoundHandle{{1}}); }},
        });
    }
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        transaction.setPhase(internal::PreparedEffectTransaction::CallbackPhase::destroy);
        REQUIRE_NOTHROW((void)audio.playSound("project://missing.wav"));
        const auto errors = transaction.effectErrors();
        REQUIRE(errors.size() == 1);
        REQUIRE(errors.front().api == "playSound");
        REQUIRE(errors.front().error_code ==
                std::string{BuildFeatureCallDisabledError::error_code});
    }
#endif
}

TEST_CASE("PG-2 the physics facade answers a build without physics by name",
          "[pg2][facade][physics]") {
#if PELICAN_WITH_PHYSICS
    REQUIRE(PELICAN_WITH_PHYSICS);
#else
    const GameContext context;
    const auto physics = context.physics();
    const phys::Ray ray{};
    const phys::Sphere sphere{};
    requireEveryCallDisabled("PELICAN_WITH_PHYSICS", {
        {"raycastAll", [&] { (void)physics.raycastAll(ray); }},
        {"raycastClosest", [&] { (void)physics.raycastClosest(ray); }},
        {"overlapAll", [&] { (void)physics.overlapAll(sphere); }},
        {"shapeCastAll", [&] { (void)physics.shapeCastAll(sphere, vec3{1.0F, 0.0F, 0.0F}); }},
        {"shapeCastClosest", [&] { (void)physics.shapeCastClosest(sphere, vec3{1.0F, 0.0F, 0.0F}); }},
    });
#endif
}

// PG-2 acceptance 2: the animation facade returns Status, so a build without
// the module answers with the Status the frozen contract names, not with an
// exception.
TEST_CASE("PG-2 the animation facade stays a Status in a build without animation",
          "[pg2][facade][animation]") {
#if PELICAN_WITH_ANIMATION
    REQUIRE(PELICAN_WITH_ANIMATION);
#else
    const GameContext context;
    const auto animation = context.animation();
    REQUIRE(animation.setParameter(GameObjectId{}, "speed", 1.0) ==
            Animation::Status::not_found);
    REQUIRE(animation.forceState(GameObjectId{}, "Run") == Animation::Status::not_found);
#endif
}

} // namespace Pelican
