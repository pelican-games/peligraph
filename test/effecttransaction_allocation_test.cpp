#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/effecttransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/persistence/persistence.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <utility>

namespace {
thread_local bool count_allocations = false;
thread_local bool fail_allocations = false;
thread_local std::size_t allocation_count = 0;
}

void *operator new(std::size_t size) {
    if (count_allocations) {
        ++allocation_count;
        if (fail_allocations) throw std::bad_alloc{};
    }
    if (void *result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace Pelican {
namespace {

static_assert(noexcept(
    std::declval<internal::PreparedEffectTransaction &>().flush(
        std::declval<SceneLoadFaultInjector *>(),
        std::declval<SceneLoadPhaseTrace &>())));

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path project;
    std::filesystem::path user;

    Sandbox() {
        base = std::filesystem::temp_directory_path() /
            ("pelican_wp380e_allocation_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        project = base / "project";
        user = base / "user";
        std::filesystem::create_directories(project);
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

class AllocationFlushFault final : public SceneLoadFaultInjector {
  public:
    bool shouldMutate(SceneLoadFaultPhase phase,
                      std::string_view participant,
                      std::string_view mutation) noexcept override {
        return phase == SceneLoadFaultPhase::flush &&
               participant == "persistence" &&
               mutation == scene_load_mutation::effect_save_data;
    }
};

} // namespace

TEST_CASE("WP380e flush failure reporting is allocation-free",
          "[WP380e][effect][allocation][cpu]") {
    Sandbox sandbox;
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(
        sandbox.project, false,
        nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
                       {"name", "wp380e-allocation"},
                       {"engine_min_version", "0.1.0"}}.dump(),
        sandbox.user);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    SceneLoadPhaseTrace trace;
    AllocationFlushFault fault;

    {
        internal::PreparedEffectTransaction empty{persistence, loader};
        internal::EffectTransactionScope empty_scope{empty};
        allocation_count = 0;
        count_allocations = true;
        empty.flush(nullptr, trace);
        count_allocations = false;
        REQUIRE(allocation_count == 0);
    }

    internal::PreparedEffectTransaction transaction{persistence, loader};
    internal::EffectTransactionScope scope{transaction};
    context.save().saveData("allocation", {{"value", 380}});

    allocation_count = 0;
    count_allocations = true;
    fail_allocations = true;
    transaction.flush(&fault, trace);
    fail_allocations = false;
    count_allocations = false;

    REQUIRE(allocation_count == 0);
    REQUIRE(transaction.fixedErrorCount() == 1);
    const auto errors = transaction.effectErrors();
    REQUIRE(errors.size() == 1);
    REQUIRE(errors.front().error_code == "persistence_write_failed@1");
}

} // namespace Pelican
