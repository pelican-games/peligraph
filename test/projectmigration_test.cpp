// MG-1: moving a project written by an earlier engine to this one's document
// versions. The steps registered here are test-only
// (projectmigrationteststep.cpp); the production registry has none at v0.

#include "projectmigrationteststep.hpp"

#include "../src/devcli/projectmigratecommand.hpp"
#include "../src/project/projectdocumenttransaction.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/projectmigration.hpp"
#include "../src/project/sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Pelican {
namespace {

namespace fs = std::filesystem;
using OrderedJson = nlohmann::ordered_json;

const fs::path source_dir{PELICAN_TEST_SOURCE_DIR};
const fs::path corpus_dir = source_dir / "test" / "fixtures" / "project_corpus" / "v0";

std::string readBytes(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeBytes(const fs::path &path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct TemporaryDirectory {
    fs::path root;
    TemporaryDirectory() {
        root = fs::temp_directory_path() /
               ("pelican_mg1_" + std::to_string(std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count()));
        fs::create_directories(root);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// A writable copy of one corpus project.
fs::path copyCorpusProject(const TemporaryDirectory &temp, std::string_view name) {
    const auto destination = temp.root / std::string{name};
    fs::copy(corpus_dir / std::string{name}, destination, fs::copy_options::recursive);
    return destination;
}

// Every file under `root` by project-relative path, bytes and all. Two equal
// snapshots mean nothing was written, created or removed.
std::map<std::string, std::string> snapshot(const fs::path &root) {
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()) {
            files.emplace(entry.path().lexically_relative(root).generic_string(),
                          readBytes(entry.path()));
        }
    }
    return files;
}

DocumentMigrationRegistry testRegistry() {
    auto registry = makeBuiltinDocumentMigrationRegistry();
    registerTestSceneKeyRenameMigrationStep(registry);
    return registry;
}

template <class F> std::string migrationErrorCode(F &&open) {
    try {
        open();
    } catch (const ProjectMigrationError &error) {
        return error.code();
    }
    return {};
}

template <class F> ProjectMigrationError requireMigrationError(F &&open) {
    try {
        open();
    } catch (const ProjectMigrationError &error) {
        return error;
    }
    FAIL("expected a ProjectMigrationError");
    throw std::logic_error("unreachable");
}

// Opens `root` the way the player does before it starts the engine: the
// production gate, then the parsers that read project.json and the configured
// scene. Without the gate, a scene at the step's `from` version is one the
// production parser still reads, so the project would open.
void openLikeThePlayer(const fs::path &root, const DocumentMigrationRegistry &registry) {
    requireProjectMigrationNotRequired(root, registry);
    const auto project = parseProjectEnvelopeText(readBytes(root / "project.json"));
    const auto scene_ref =
        project.envelope.basic_config.at("scene_data_json").get<std::string>();
    (void)normalizeSceneDataJson(nlohmann::json::parse(readBytes(root / scene_ref)));
}

struct CliRun {
    int exit_code = 0;
    std::string out;
    std::string err;
    OrderedJson json() const { return OrderedJson::parse(out); }
};

CliRun runCli(const DocumentMigrationRegistry &registry, std::vector<std::string> args) {
    args.insert(args.begin(), "migrate");
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    std::ostringstream out;
    std::ostringstream err;
    CliRun run;
    run.exit_code = DevCli::runProjectMigrateCommand(
        static_cast<int>(argv.size()), argv.data(), registry, out, err);
    run.out = out.str();
    run.err = err.str();
    return run;
}

std::vector<std::string> corpusProjects() {
    std::vector<std::string> names;
    for (const auto &entry : fs::directory_iterator{corpus_dir}) {
        if (entry.is_directory()) names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

TEST_CASE("migration step registration refuses a bad step by name",
          "[mg1][project-migration][registry]") {
    auto registry = testRegistry();
    REQUIRE(registry.size() == 1);
    const auto noop = [](const OrderedJson &document, const ProjectContext &) {
        return StepResult{document, {}};
    };
    const auto refusal = [&](DocumentMigrationStep step) {
        return migrationErrorCode([&] { registry.registerStep(std::move(step)); });
    };

    // The same (schema, from) twice.
    CHECK(refusal({"other.id", "pelican.scene", 2, 3, "", noop}) ==
          std::string{projectMigrationStepDuplicateCode});
    // The same id twice.
    CHECK(refusal({std::string{testSceneKeyRenameStepId}, "pelican.ui", 1, 2, "", noop}) ==
          std::string{projectMigrationStepDuplicateCode});
    // A jump over a version.
    CHECK(refusal({"jump", "pelican.ui", 1, 3, "", noop}) == std::string{projectMigrationStepGapCode});
    // Back to where the chain has already been: with 2 -> 3 registered,
    // 3 -> 2 closes a cycle; 4 -> 4 is one on its own.
    CHECK(refusal({"back", "pelican.scene", 3, 2, "", noop}) ==
          std::string{projectMigrationStepCycleCode});
    CHECK(refusal({"self", "pelican.ui", 4, 4, "", noop}) == std::string{projectMigrationStepCycleCode});
    CHECK(refusal({"", "pelican.ui", 1, 2, "", noop}) == std::string{projectMigrationStepInvalidCode});
    CHECK(refusal({"noplan", "pelican.ui", 1, 2, "", {}}) ==
          std::string{projectMigrationStepInvalidCode});
    // Nothing a refusal saw was registered.
    CHECK(registry.size() == 1);
    CHECK(registry.find("pelican.ui", 1) == nullptr);
    CHECK(registry.find("pelican.scene", 3) == nullptr);

    registry.registerStep({"ui.one", "pelican.ui", 1, 2, "", noop});
    CHECK(registry.size() == 2);
    CHECK(registry.catalog().size() == 2);
}

TEST_CASE("opening a project with a document at a step's from version stops by name",
          "[mg1][project-migration][detect]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "example");
    const auto before = snapshot(root);

    // Control: with the production registry (no steps) the project opens.
    REQUIRE_NOTHROW(openLikeThePlayer(root, builtinDocumentMigrationRegistry()));

    const auto registry = testRegistry();
    const auto error = requireMigrationError([&] { openLikeThePlayer(root, registry); });
    CHECK(error.code() == std::string{projectMigrationRequiredCode});
    CHECK(error.fields() ==
          OrderedJson{{"documents",
                       OrderedJson::array({OrderedJson{{"path", "scenes/main.scene.json"},
                                                       {"schema", "pelican.scene"},
                                                       {"from", 2},
                                                       {"to", 3}}})}});
    CHECK_THAT(error.what(),
               Catch::Matchers::StartsWith("project.migration_required@1: {"));
    // The gate reads; it never writes.
    CHECK(snapshot(root) == before);
}

TEST_CASE("detection reads the raw envelope before project.json's own version check",
          "[mg1][project-migration][detect]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "sprite_demo");
    auto project = OrderedJson::parse(readBytes(root / "project.json"));
    project["version"] = 5;
    writeBytes(root / "project.json", project.dump(2));

    // The parser alone refuses version 5 by its own words.
    REQUIRE_THROWS_WITH(parseProjectEnvelopeText(readBytes(root / "project.json")),
                        Catch::Matchers::ContainsSubstring("exactly 1"));

    auto registry = makeBuiltinDocumentMigrationRegistry();
    const auto keep = [](const OrderedJson &document, const ProjectContext &) {
        return StepResult{document, {}};
    };
    registry.registerStep({"test.project.5", "pelican.project", 5, 6, "", keep});
    registry.registerStep({"test.project.6", "pelican.project", 6, 7, "", keep});
    const auto error =
        requireMigrationError([&] { requireProjectMigrationNotRequired(root, registry); });
    CHECK(error.code() == std::string{projectMigrationRequiredCode});
    // `to` is where the chain of steps ends, not the next version.
    CHECK(error.fields().at("documents") ==
          OrderedJson::array({OrderedJson{{"path", "project.json"},
                                          {"schema", "pelican.project"},
                                          {"from", 5},
                                          {"to", 7}}}));
}

TEST_CASE("project migrate --dry-run writes nothing and apply rewrites with a backup",
          "[mg1][project-migration][cli]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "example");
    const auto registry = testRegistry();
    const auto original = snapshot(root);
    const auto scene_before = original.at("scenes/main.scene.json");
    const auto project_before = original.at("project.json");
    REQUIRE(project_before.find("saved_by_engine") == std::string::npos);

    const auto dry = runCli(registry, {"--project", root.string(), "--dry-run"});
    REQUIRE(dry.exit_code == 0);
    const auto plan = dry.json();
    CHECK(plan.at("dry_run") == true);
    CHECK(plan.at("required") == true);
    CHECK(plan.at("documents").size() == 1);
    CHECK(plan.at("documents").at(0).at("path") == "scenes/main.scene.json");
    CHECK(plan.at("documents").at(0).at("steps") ==
          OrderedJson::array({std::string{testSceneKeyRenameStepId}}));
    CHECK(plan.at("expected").at("scenes/main.scene.json") ==
          projectDocumentDigest(scene_before));
    CHECK(plan.at("expected").at("project.json") == projectDocumentDigest(project_before));
    // Not a byte, not a directory.
    CHECK(snapshot(root) == original);
    CHECK_FALSE(fs::exists(root / ".pelican"));

    const auto applied = runCli(registry, {"--project", root.string()});
    REQUIRE(applied.exit_code == 0);
    const auto result = applied.json();
    CHECK(result.at("applied") == true);
    CHECK(result.at("saved_by_engine").is_null());
    CHECK(result.at("saved_by_engine_after") == std::string{pelicanEngineVersion});

    // The scene was rewritten by the step, and its envelope says so.
    const auto scene_after = OrderedJson::parse(readBytes(root / "scenes/main.scene.json"));
    CHECK(scene_after.at("version") == 3);
    CHECK(scene_after.contains("scene_table"));
    CHECK_FALSE(scene_after.contains("scenes"));
    CHECK(scene_after.at("scene_table") == OrderedJson::parse(scene_before).at("scenes"));

    // The backup holds the bytes every written document had.
    const auto backup = root / result.at("backup").get<std::string>();
    CHECK_THAT(result.at("backup").get<std::string>(),
               Catch::Matchers::StartsWith(".pelican/migration/"));
    CHECK(readBytes(backup / "scenes/main.scene.json") == scene_before);
    CHECK(readBytes(backup / "project.json") == project_before);
    CHECK(OrderedJson::parse(readBytes(backup / "migration.json")).at("schema") ==
          "pelican.project_migration_backup");

    // project.json gained `saved_by_engine` and nothing else: every other
    // byte is where it was.
    const auto project_after = readBytes(root / "project.json");
    auto expected_project = nlohmann::json::parse(project_before);
    expected_project["saved_by_engine"] = std::string{pelicanEngineVersion};
    CHECK(nlohmann::json::parse(project_after) == expected_project);
    const std::string inserted =
        ",\n  \"saved_by_engine\": \"" + std::string{pelicanEngineVersion} + "\"";
    CHECK(project_after.size() == project_before.size() + inserted.size());
    CHECK(parseProjectEnvelopeText(project_after).envelope.saved_by_engine ==
          std::string{pelicanEngineVersion});

    // No other document changed, and the transaction left nothing behind.
    for (const auto &[path, bytes] : snapshot(root)) {
        if (path == "project.json" || path == "scenes/main.scene.json" ||
            path.starts_with(".pelican/migration/")) {
            continue;
        }
        CHECK(original.at(path) == bytes);
    }
    CHECK_FALSE(fs::exists(projectDocumentTransactionDirectory(root)));

    // It opens now, and a second migrate finds nothing and writes nothing.
    REQUIRE_NOTHROW(requireProjectMigrationNotRequired(root, registry));
    const auto settled = snapshot(root);
    const auto again = runCli(registry, {"--project", root.string()});
    REQUIRE(again.exit_code == 0);
    CHECK(again.json().at("required") == false);
    CHECK(again.json().at("applied") == false);
    CHECK(again.json().at("saved_by_engine") == std::string{pelicanEngineVersion});
    CHECK(snapshot(root) == settled);
}

TEST_CASE("an existing saved_by_engine is replaced in place",
          "[mg1][project-migration][cli]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "posegraph_demo");
    auto project = readBytes(root / "project.json");
    const std::string anchor = "\"version\": 1,";
    const auto at = project.find(anchor);
    REQUIRE(at != std::string::npos);
    project.insert(at + anchor.size(), "\n  \"saved_by_engine\": \"0.0.1\",");
    writeBytes(root / "project.json", project);

    const auto applied = runCli(testRegistry(), {"--project", root.string()});
    REQUIRE(applied.exit_code == 0);
    CHECK(applied.json().at("saved_by_engine") == "0.0.1");
    auto expected = project;
    expected.replace(expected.find("0.0.1"), 5, std::string{pelicanEngineVersion});
    CHECK(readBytes(root / "project.json") == expected);
}

TEST_CASE("a failure inside apply puts every document back to its bytes",
          "[mg1][project-migration][transaction]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "example");
    const auto registry = testRegistry();
    const auto original = snapshot(root);

    // The scene is written first, project.json second; fail after each.
    for (std::size_t fail_after : {std::size_t{0}, std::size_t{1}}) {
        INFO("fault after document " << fail_after);
        std::size_t written = 0;
        ProjectMigrationApplyOptions options;
        options.fault = [&](std::size_t index, const ProjectCandidateDocument &) {
            ++written;
            if (index == fail_after) throw std::runtime_error("injected fault");
        };
        const auto error =
            requireMigrationError([&] { applyProjectMigration(root, registry, options); });
        CHECK(error.code() == std::string{projectMigrationApplyFailedCode});
        CHECK_THAT(error.fields().at("message").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("injected fault"));
        // The fault ran after a document had been replaced on disk.
        CHECK(written == fail_after + 1);
        // Every document holds its original bytes; no backup, no transaction.
        CHECK(snapshot(root) == original);
        CHECK_FALSE(fs::exists(root / ".pelican" / "migration"));
    }
    // And the project is still one that has to be migrated.
    CHECK(migrationErrorCode([&] { requireProjectMigrationNotRequired(root, registry); }) ==
          std::string{projectMigrationRequiredCode});
}

TEST_CASE("apply is bound to the digests the plan returned",
          "[mg1][project-migration][cas]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "example");
    const auto registry = testRegistry();
    const auto plan = planProjectMigration(root, registry);
    REQUIRE(plan.required());

    // The scene changes after the plan was read.
    writeBytes(root / "scenes/main.scene.json",
               readBytes(root / "scenes/main.scene.json") + "\n");
    const auto moved = snapshot(root);
    ProjectMigrationApplyOptions options;
    options.expected_digests = plan.expectedDigests();
    const auto error =
        requireMigrationError([&] { applyProjectMigration(root, registry, options); });
    CHECK(error.code() == std::string{projectMigrationPlanStaleCode});
    CHECK(error.fields().at("expected").at("scenes/main.scene.json") ==
          plan.expectedDigests().at("scenes/main.scene.json"));
    CHECK(snapshot(root) == moved);

    // The plan of the project as it is now applies.
    options.expected_digests = planProjectMigration(root, registry).expectedDigests();
    CHECK(applyProjectMigration(root, registry, options).applied);
}

TEST_CASE("a manual item a step reports reaches the plan by name",
          "[mg1][project-migration][manual]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "sprite_demo");
    auto scene = OrderedJson::parse(readBytes(root / "scene.json"));
    scene["scene_table"] = OrderedJson::object();
    writeBytes(root / "scene.json", scene.dump(2));

    const auto plan = planProjectMigration(root, testRegistry()).toJson();
    REQUIRE(plan.at("documents").size() == 1);
    CHECK(plan.at("documents").at(0).at("manual_items") ==
          OrderedJson::array({OrderedJson{
              {"code", "pelican.scene.test_scene_table_taken@1"},
              {"pointer", "/scene_table"},
              {"message", "scene_table already exists; move `scenes` into it by hand"}}}));
}

TEST_CASE("every v0 corpus project migrates through the test step and then opens",
          "[mg1][project-migration][corpus]") {
    const auto registry = testRegistry();
    const auto names = corpusProjects();
    REQUIRE(names.size() == 7);
    for (const auto &name : names) {
        INFO("corpus project " << name);
        TemporaryDirectory temp;
        const auto root = copyCorpusProject(temp, name);
        const auto scene_ref = parseProjectEnvelopeText(readBytes(root / "project.json"))
                                   .envelope.basic_config.at("scene_data_json")
                                   .get<std::string>();

        // Exactly one document is at the step's `from` version: the scene.
        const auto detections = detectProjectMigration(root, registry);
        REQUIRE(detections.size() == 1);
        CHECK(detections.front().path == scene_ref);
        REQUIRE(migrationErrorCode([&] { requireProjectMigrationNotRequired(root, registry); }) ==
                std::string{projectMigrationRequiredCode});

        const auto result = applyProjectMigration(root, registry);
        REQUIRE(result.applied);
        // migrate -> open: the gate lets it through and project.json parses.
        REQUIRE_NOTHROW(requireProjectMigrationNotRequired(root, registry));
        const auto project = parseProjectEnvelopeText(readBytes(root / "project.json"));
        CHECK(project.envelope.saved_by_engine == std::string{pelicanEngineVersion});
        CHECK(OrderedJson::parse(readBytes(root / scene_ref)).at("version") == 3);
    }
}

TEST_CASE("the production registry has no steps and opens every shipped project and the corpus",
          "[mg1][project-migration][production]") {
    REQUIRE(builtinDocumentMigrationRegistry().empty());
    std::vector<fs::path> roots;
    for (const auto &name : corpusProjects()) roots.push_back(corpus_dir / name);
    for (const auto &entry : fs::directory_iterator{source_dir / "projects"}) {
        if (fs::is_regular_file(entry.path() / "project.json")) roots.push_back(entry.path());
    }
    REQUIRE(roots.size() == 14);
    for (const auto &root : roots) {
        INFO(root.string());
        CHECK(detectProjectMigration(root, builtinDocumentMigrationRegistry()).empty());
        CHECK_FALSE(planProjectMigration(root, builtinDocumentMigrationRegistry()).required());
    }
}

TEST_CASE("a document that is not an envelope is not a document",
          "[mg1][project-migration][detect]") {
    TemporaryDirectory temp;
    const auto root = copyCorpusProject(temp, "sprite_demo");
    // Not an object, not JSON, a nested `schema`, a hidden directory, and a
    // scene envelope under another extension: none of them is a document.
    writeBytes(root / "list.json", R"([{"schema":"pelican.scene","version":2}])");
    writeBytes(root / "broken.json", "{\"schema\": \"pelican.scene\", ");
    writeBytes(root / "nested.json", R"({"inner":{"schema":"pelican.scene","version":2}})");
    writeBytes(root / ".hidden" / "scene.json", R"({"schema":"pelican.scene","version":2})");
    writeBytes(root / "scene.txt", R"({"schema":"pelican.scene","version":2})");
    // `version` ahead of `schema`, in a large file: still read.
    writeBytes(root / "late" / "scene.json",
               R"({"version":2,"schema":"pelican.scene","scenes":{}})");

    const auto detections = detectProjectMigration(root, testRegistry());
    std::vector<std::string> paths;
    for (const auto &detection : detections) paths.push_back(detection.path);
    CHECK(paths == std::vector<std::string>{"late/scene.json", "scene.json"});
}

} // namespace Pelican
