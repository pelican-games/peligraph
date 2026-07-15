#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/log.hpp"
#include "../src/core/phys/physicsruntime.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/physics/abi_v2.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

using namespace std::chrono_literals;

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

std::filesystem::path requiredFixture(const char *name) {
    char *value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr ||
        value_size <= 1) {
        std::free(value);
        throw std::runtime_error(std::string{"missing fixture environment variable: "} + name);
    }
    const auto result = std::filesystem::absolute(value).lexically_normal();
    std::free(value);
    return result;
}

class Sandbox {
    std::filesystem::path root_path;

  public:
    Sandbox() {
        root_path = std::filesystem::temp_directory_path() /
                    ("pelican_physics_provider_dll_" +
                     std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(root_path, ec);
        ec.clear();
        std::filesystem::create_directories(root_path, ec);
        if (ec) throw std::runtime_error("cannot create physics provider DLL sandbox");
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_path, ec);
    }

    [[nodiscard]] const std::filesystem::path &root() const noexcept {
        return root_path;
    }
};

void replaceFixture(const std::filesystem::path &source,
                    const std::filesystem::path &destination) {
    std::error_code ec;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("cannot replace physics provider fixture: " + ec.message());
    }
}

struct FixtureControls {
    using StatusFn = std::uint32_t (*)();
    using VoidFn = void (*)();
    using BoolFn = bool (*)();

    StatusFn registration_status = nullptr;
    VoidFn arm_next_raycast = nullptr;
    BoolFn raycast_entered = nullptr;
    VoidFn resume_raycast = nullptr;
};

template <class Function>
Function fixtureFunction(HMODULE module, const char *name) {
    const auto address = GetProcAddress(module, name);
    if (address == nullptr) {
        throw std::runtime_error(std::string{"physics fixture export is missing: "} + name);
    }
    return reinterpret_cast<Function>(address);
}

FixtureControls fixtureControls(const GameLogicReloadStatus &status) {
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) {
        module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    }
    if (module == nullptr) {
        throw std::runtime_error("cannot locate loaded physics provider shadow DLL");
    }
    return FixtureControls{
        fixtureFunction<FixtureControls::StatusFn>(
            module, "pelican_physics_fixture_registration_status"),
        fixtureFunction<FixtureControls::VoidFn>(
            module, "pelican_physics_fixture_arm_next_raycast"),
        fixtureFunction<FixtureControls::BoolFn>(
            module, "pelican_physics_fixture_raycast_entered"),
        fixtureFunction<FixtureControls::VoidFn>(
            module, "pelican_physics_fixture_resume_raycast"),
    };
}

bool waitUntilEntered(const FixtureControls &controls) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (controls.raycast_entered()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return controls.raycast_entered();
}

ColliderComponent sphereCollider() {
    ColliderComponent collider;
    collider.shape = "sphere";
    collider.radius = 1.0F;
    return collider;
}

std::vector<phys::RaycastQueryHit> queryRay(PhysWorld &world) {
    return world.raycastAll(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, 10.0F},
        phys::QueryFilter{});
}

void requireFixtureHit(const std::vector<phys::RaycastQueryHit> &hits,
                       float distance) {
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].id == "target");
    REQUIRE(hits[0].distance == Catch::Approx(distance));
    REQUIRE(hits[0].position.x == Catch::Approx(distance));
    REQUIRE(hits[0].position.y == Catch::Approx(0.0F));
    REQUIRE(hits[0].normal.x == Catch::Approx(-1.0F));
}

std::optional<phys::ShapeCastQueryHit> queryShapeCast(PhysWorld &world) {
    return world.shapeCastClosest(
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 0.5F},
        {10.0F, 0.0F, 0.0F});
}

void requireFixtureShapeCast(const std::optional<phys::ShapeCastQueryHit> &hit) {
    REQUIRE(hit);
    REQUIRE(hit->id == "target");
    REQUIRE(hit->time_of_impact == Catch::Approx(0.375F));
    REQUIRE(hit->position.x == Catch::Approx(3.75F));
    REQUIRE(hit->normal.x == Catch::Approx(-1.0F));
}

constexpr const char *configuredProviderName() {
#if PELICAN_WITH_JOLT_PHYSICS
    return "pelican.jolt";
#elif PELICAN_WITH_BUILTIN_PHYSICS
    return "pelican.builtin";
#else
    return "";
#endif
}

} // namespace

TEST_CASE("public game DLL physics provider survives reload rollback and in-flight unload",
          "[physics-provider-dll]") {
    ensureLogger();
    const auto fixture_v1 = requiredFixture("PELICAN_PHYSICS_FIXTURE_V1");
    const auto fixture_v2 = requiredFixture("PELICAN_PHYSICS_FIXTURE_V2");
    const auto fixture_bad_abi = requiredFixture("PELICAN_PHYSICS_FIXTURE_BAD_ABI");

    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    world.bindCollider("target", sphereCollider(),
                       PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});

    Sandbox sandbox;
    const auto live_dll = sandbox.root() / "physics_provider_live.dll";
    replaceFixture(fixture_v1, live_dll);

    GameLogicReloader reloader;
    REQUIRE(reloader.initialize(live_dll));
    REQUIRE(reloader.status().generation == 1);
    REQUIRE(physics_internal::activeProviderName() == "fixture.physics.v1");
    auto controls = fixtureControls(reloader.status());
    REQUIRE(static_cast<Physics::Status>(controls.registration_status()) ==
            Physics::Status::ok);
    requireFixtureHit(queryRay(world), 0.625F);

    // The DLL replaces raycast only. Overlap remains independently available
    // from the configured backend, or unavailable in a provider-only build.
    const auto fallback_overlap = world.overlapAll(
        phys::Sphere{{3.0F, 0.0F, 0.0F}, 0.25F});
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(fallback_overlap == std::vector<std::string>{"target"});
#else
    REQUIRE(fallback_overlap.empty());
#endif

    // A V1 DLL cannot claim the additive V2 operation. Capability routing
    // therefore falls through to the configured backend when present.
    const auto v1_shape_cast = queryShapeCast(world);
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(v1_shape_cast);
    REQUIRE(v1_shape_cast->time_of_impact == Catch::Approx(0.15F).margin(2.0e-4F));
#else
    REQUIRE_FALSE(v1_shape_cast);
#endif

    // The source DLL can be overwritten because the loader owns a shadow copy.
    // Keep a V1 callback in flight while another engine thread attempts reload.
    replaceFixture(fixture_v2, live_dll);
    controls.arm_next_raycast();
    std::vector<phys::RaycastQueryHit> in_flight_hits;
    std::exception_ptr query_error;
    std::thread query_thread{[&] {
        try {
            in_flight_hits = queryRay(world);
        } catch (...) {
            query_error = std::current_exception();
        }
    }};

    const bool callback_entered = waitUntilEntered(controls);
    if (!callback_entered) {
        controls.resume_raycast();
        query_thread.join();
        reloader.shutdown();
        REQUIRE(callback_entered);
    }

    std::binary_semaphore reload_started{0};
    std::binary_semaphore reload_finished{0};
    bool reload_result = false;
    std::exception_ptr reload_error;
    std::thread reload_thread{[&] {
        reload_started.release();
        try {
            reload_result = reloader.reloadNow([] {}, [] {});
        } catch (...) {
            reload_error = std::current_exception();
        }
        reload_finished.release();
    }};
    reload_started.acquire();
    const bool reload_finished_while_callback_blocked =
        reload_finished.try_acquire_for(150ms);

    controls.resume_raycast();
    query_thread.join();
    reload_thread.join();

    REQUIRE_FALSE(reload_finished_while_callback_blocked);
    REQUIRE(query_error == nullptr);
    REQUIRE(reload_error == nullptr);
    REQUIRE(reload_result);
    requireFixtureHit(in_flight_hits, 0.625F);
    REQUIRE(reloader.status().generation == 2);
    REQUIRE(physics_internal::activeProviderName() == "fixture.physics.v2");
    requireFixtureHit(queryRay(world), 1.375F);
    requireFixtureShapeCast(queryShapeCast(world));

    // Candidate validation must not disturb the live V2 provider.
    replaceFixture(fixture_bad_abi, live_dll);
    REQUIRE_FALSE(reloader.reloadNow([] {}, [] {}));
    REQUIRE(reloader.status().generation == 2);
    REQUIRE(physics_internal::activeProviderName() == "fixture.physics.v2");
    requireFixtureHit(queryRay(world), 1.375F);
    requireFixtureShapeCast(queryShapeCast(world));

    // A failure after candidate activation reloads the previous shadow under a
    // fresh owner. The failed V1 callback table must not survive rollback.
    replaceFixture(fixture_v1, live_dll);
    int rebuild_calls = 0;
    REQUIRE_FALSE(reloader.reloadNow([] {}, [&] {
        if (++rebuild_calls == 1) {
            throw std::runtime_error("forced physics provider rebuild failure");
        }
    }));
    REQUIRE(rebuild_calls == 2);
    REQUIRE(reloader.status().generation == 2);
    REQUIRE(physics_internal::activeProviderName() == "fixture.physics.v2");
    requireFixtureHit(queryRay(world), 1.375F);
    requireFixtureShapeCast(queryShapeCast(world));

    reloader.shutdown();
    REQUIRE(physics_internal::activeProviderName() == configuredProviderName());
    const auto shutdown_shape_cast = queryShapeCast(world);
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(shutdown_shape_cast);
    REQUIRE(shutdown_shape_cast->time_of_impact ==
            Catch::Approx(0.15F).margin(2.0e-4F));
#else
    REQUIRE_FALSE(shutdown_shape_cast);
#endif
}

} // namespace Pelican
