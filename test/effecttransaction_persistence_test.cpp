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
#include <fstream>

namespace Pelican {
namespace {

struct PersistenceSandbox {
    std::filesystem::path base;
    std::filesystem::path project;
    std::filesystem::path user;
    PersistenceSandbox() {
        base = std::filesystem::temp_directory_path() /
            ("pelican_wp380b_persistence_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        project = base / "project";
        user = base / "user";
        std::filesystem::create_directories(project);
    }
    ~PersistenceSandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

void configurePersistence(const PersistenceSandbox &sandbox) {
    setupLogger();
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(PathResolver).setup(
        sandbox.project, false,
        nlohmann::json{{"schema", "pelican.project"}, {"version", 1},
                       {"name", "wp380b-persistence"},
                       {"engine_min_version", "0.1.0"}}.dump(),
        sandbox.user);
}

class PersistenceFlushFault final : public SceneLoadFaultInjector {
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

TEST_CASE("WP380b persistence shadow and overlays work without audio",
          "[WP380b][effect][persistence][cpu][mutation]") {
    PersistenceSandbox sandbox;
    FastModuleContainer modules;
    configurePersistence(sandbox);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    context.setGameSettings({{"world", "B"}});
    context.saveData("slot_existing", {{"value", 379}});

    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        context.setGameSettings({{"world", "C"}});
        context.saveSettings();
        context.saveData("slot_c", {{"value", 380}});
        context.saveData("slot_existing", {{"value", 380}});
        REQUIRE(context.gameSettings().at("world") == "C");
        REQUIRE(context.loadData("slot_c") ==
                nlohmann::json{{"value", 380}});
        const auto saves = context.listSaves();
        REQUIRE(saves.size() == 2);
        for (const auto &save : saves) {
            REQUIRE(save.timestamp == std::filesystem::file_time_type{});
        }
    }

    REQUIRE(context.gameSettings().at("world") == "B");
    REQUIRE_FALSE(std::filesystem::exists(
        sandbox.user / "saves" / "slot_c.json"));
    REQUIRE(context.loadData("slot_existing") ==
            nlohmann::json{{"value", 379}});
    REQUIRE_FALSE(std::filesystem::exists(sandbox.user / "settings.json"));

    SceneLoadPhaseTrace trace;
    PersistenceFlushFault fault;
    std::vector<CommittedWithEffectFailure::EffectError> errors;
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        context.saveSettings();
        context.saveData("slot_d", {{"value", 381}});
        transaction.flush(&fault, trace);
        errors = transaction.effectErrors();
    }
    REQUIRE(errors.size() == 1);
    REQUIRE(errors.front().api == "saveData");
    REQUIRE(std::filesystem::is_regular_file(sandbox.user / "settings.json"));
    REQUIRE_FALSE(std::filesystem::exists(
        sandbox.user / "saves" / "slot_d.json"));
}

TEST_CASE("WP380e persistence keys use physical Windows path identity",
          "[WP380e][effect][persistence][identity][cpu][mutation]") {
    PersistenceSandbox sandbox;
    FastModuleContainer modules;
    configurePersistence(sandbox);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    SceneLoadPhaseTrace trace;
    const auto before = persistence.preparedWriteCommitCountForTesting();
    {
        internal::PreparedEffectTransaction transaction{persistence, loader};
        internal::EffectTransactionScope scope{transaction};
        context.saveData("Foo", {{"value", 1}});
        context.saveData("foo", {{"value", 2}});
        transaction.flush(nullptr, trace);
        REQUIRE(transaction.effectErrors().empty());
    }
#ifdef _WIN32
    REQUIRE(persistence.preparedWriteCommitCountForTesting() - before == 1);
    REQUIRE(context.loadData("Foo") == nlohmann::json{{"value", 2}});
#endif
}

TEST_CASE("WP380e persistence read failures are named and absence alone is nullopt",
          "[WP380e][effect][persistence][read-error][cpu]") {
    PersistenceSandbox sandbox;
    FastModuleContainer modules;
    configurePersistence(sandbox);
    auto &persistence = GET_MODULE(Persistence);
    auto &loader = GET_MODULE(SceneLoader);
    GameContext context;
    REQUIRE_FALSE(context.loadData("absent").has_value());
    std::filesystem::create_directories(sandbox.user / "saves");
    {
        std::ofstream corrupt{sandbox.user / "saves" / "corrupt.json"};
        corrupt << "{not-json";
    }
    REQUIRE_THROWS_AS(context.loadData("corrupt"), PersistenceReadError);
    REQUIRE_THROWS_WITH(context.loadData("corrupt"),
                        Catch::Matchers::ContainsSubstring(
                            "persistence_read_failed@1"));

    SceneLoadPhaseTrace trace;
    internal::PreparedEffectTransaction transaction{persistence, loader};
    internal::EffectTransactionScope scope{transaction};
    {
        internal::EffectDestroyScope destroy_scope;
        REQUIRE_FALSE(context.loadData("corrupt").has_value());
    }
    transaction.flush(nullptr, trace);
    const auto errors = transaction.effectErrors();
    REQUIRE(errors.size() == 1);
    REQUIRE(errors.front().api == "loadData");
    REQUIRE(errors.front().error_code == "persistence_read_failed@1");
    REQUIRE(errors.front().error_fields.at("path").get<std::string>().find(
                "corrupt.json") != std::string::npos);
}

} // namespace Pelican
