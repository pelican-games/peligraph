// MG-1: plan_project_migration / apply_project_migration over the editor RPC,
// and the migration-only endpoint an --rpc player serves when opening stopped
// at project.migration_required@1. The step is the test-only one.

#include "projectmigrationteststep.hpp"

#include "../src/core/communication/projectmigrationrpc.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/project/projectmigration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace Pelican {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

const fs::path corpus_dir =
    fs::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "project_corpus" / "v0";

std::string readBytes(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

struct CorpusCopy {
    fs::path base;
    fs::path root;
    explicit CorpusCopy(std::string_view name) {
        base = fs::temp_directory_path() /
               ("pelican_mg1_rpc_" + std::to_string(std::chrono::steady_clock::now()
                                                        .time_since_epoch()
                                                        .count()));
        root = base / std::string{name};
        fs::create_directories(base);
        fs::copy(corpus_dir / std::string{name}, root, fs::copy_options::recursive);
    }
    ~CorpusCopy() {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
};

std::string request(int id, std::string_view method, Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}
        .dump();
}

DocumentMigrationRegistry testRegistry() {
    auto registry = makeBuiltinDocumentMigrationRegistry();
    registerTestSceneKeyRenameMigrationStep(registry);
    return registry;
}

} // namespace

TEST_CASE("plan_project_migration and apply_project_migration are bound by the digest set",
          "[mg1][project-migration][rpc]") {
    CorpusCopy project{"example"};
    const auto registry = testRegistry();
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    configureProjectMigrationRpcHandlers(server, project.root, registry);

    const auto plan = Json::parse(server.processLine(request(1, "plan_project_migration")));
    CAPTURE(plan.dump());
    const auto &planned = plan.at("result");
    REQUIRE(planned.at("required") == true);
    REQUIRE(planned.at("documents").at(0).at("path") == "scenes/main.scene.json");

    // Without the CAS set the call is malformed.
    const auto missing = Json::parse(server.processLine(request(2, "apply_project_migration")));
    CHECK(missing.at("error").at("code") == -32602);

    // A digest set that is not the plan's writes nothing.
    auto stale = planned.at("expected");
    stale["scenes/main.scene.json"] = std::string(64, '0');
    const auto scene_before = readBytes(project.root / "scenes/main.scene.json");
    const auto refused = Json::parse(server.processLine(
        request(3, "apply_project_migration", {{"expected", stale}})));
    CAPTURE(refused.dump());
    CHECK(refused.at("error").at("data").at("error_code") ==
          std::string{projectMigrationPlanStaleCode});
    CHECK(readBytes(project.root / "scenes/main.scene.json") == scene_before);

    const auto applied = Json::parse(server.processLine(
        request(4, "apply_project_migration", {{"expected", planned.at("expected")}})));
    CAPTURE(applied.dump());
    CHECK(applied.at("result").at("applied") == true);
    CHECK(applied.at("result").at("restart_required") == true);
    CHECK(Json::parse(readBytes(project.root / "scenes/main.scene.json")).at("version") == 3);

    const auto after = Json::parse(server.processLine(request(5, "plan_project_migration")));
    CHECK(after.at("result").at("required") == false);
}

TEST_CASE("the migration-only endpoint refuses everything else by name until it has migrated",
          "[mg1][project-migration][rpc]") {
    CorpusCopy project{"sprite_demo"};
    const auto registry = testRegistry();
    ProjectMigrationError required{"placeholder@1", {}};
    try {
        requireProjectMigrationNotRequired(project.root, registry);
        FAIL("the project should need migration");
    } catch (const ProjectMigrationError &error) {
        required = error;
    }
    REQUIRE(required.code() == std::string{projectMigrationRequiredCode});

    // plan -> a gated method -> apply -> the same gated method.
    std::ostringstream script;
    script << request(1, "plan_project_migration") << "\n"
           << request(2, "get_status") << "\n";
    std::istringstream plan_input{script.str()};
    std::ostringstream plan_output;
    CHECK_FALSE(runProjectMigrationRpcServer(plan_input, plan_output, project.root,
                                             required, registry));
    std::vector<Json> responses;
    std::istringstream lines{plan_output.str()};
    for (std::string line; std::getline(lines, line);) responses.push_back(Json::parse(line));
    REQUIRE(responses.size() == 2);
    const auto expected = responses[0].at("result").at("expected");
    CHECK(responses[1].at("error").at("code") == -32000);
    CHECK_THAT(responses[1].at("error").at("message").get<std::string>(),
               Catch::Matchers::StartsWith("project.migration_required@1: {\"documents\""));

    std::istringstream apply_input{request(3, "apply_project_migration",
                                           {{"expected", expected}}) +
                                   "\n" + request(4, "get_status") + "\n"};
    std::ostringstream apply_output;
    CHECK(runProjectMigrationRpcServer(apply_input, apply_output, project.root, required,
                                       registry));
    responses.clear();
    std::istringstream apply_lines{apply_output.str()};
    for (std::string line; std::getline(apply_lines, line);) responses.push_back(Json::parse(line));
    REQUIRE(responses.size() == 2);
    CHECK(responses[0].at("result").at("applied") == true);
    CHECK_THAT(responses[1].at("error").at("message").get<std::string>(),
               Catch::Matchers::StartsWith("project.migration_restart_required@1"));
    CHECK_NOTHROW(requireProjectMigrationNotRequired(project.root, registry));
}

} // namespace Pelican
