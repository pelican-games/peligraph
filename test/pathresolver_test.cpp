#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/project/shadersourceresolver.hpp"
#include "../src/core/log.hpp"

#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "project_format";
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

ProjectEnvelope projectEnvelope(nlohmann::json project) {
    project["schema"] = "pelican.project";
    project["version"] = 1;
    return parseProjectEnvelopeJson(project).envelope;
}

std::filesystem::path weaklyCanonical(const std::filesystem::path &path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    REQUIRE_FALSE(ec);
    return canonical;
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;
    std::filesystem::path outside;

    Sandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_pathresolver_" + suffix);
        root = base / "project";
        outside = base / "outside";

        std::filesystem::create_directories(root / "assets");
        std::filesystem::create_directories(outside);
        writeText(root / "assets" / "a.txt", "asset-content");
        writeText(outside / "outside.txt", "outside-content");
        writeText(outside / "secret.txt", "secret-content");
    }

    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

std::string materializeRef(const nlohmann::json &scenario, const Sandbox &sandbox) {
    auto ref = scenario.at("ref").get<std::string>();
    if (ref == "$ABSOLUTE_OUTSIDE_FILE") {
        ref = (sandbox.outside / "outside.txt").generic_string();
    }
    return ref;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

PathResolver &resolverForTest() {
    ensureLogger();
    return GET_MODULE(PathResolver);
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "escape") {
        REQUIRE(contains(message, "escapes project root"));
        REQUIRE(contains(message, "outside"));
    } else if (error_kind == "absolute") {
        REQUIRE(contains(message, "Absolute project path"));
    } else if (error_kind == "unknown_engine_id") {
        REQUIRE(contains(message, "Unknown engine resource id"));
        REQUIRE(contains(message, "default_config.json"));
    } else if (error_kind == "separator") {
        REQUIRE(contains(message, "Backslash path separators"));
    } else if (error_kind == "empty") {
        REQUIRE(contains(message, "Empty project path"));
    } else {
        FAIL("unknown fixture error_kind: " << error_kind);
    }
}

void runScenario(const nlohmann::json &scenario, const Sandbox &sandbox) {
    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false);

    const auto mode = scenario.at("mode").get<std::string>();
    const auto ref = materializeRef(scenario, sandbox);

    if (mode == "resolve_project") {
        const auto resolved = resolver.resolveProjectRef(ref);
        const auto path = std::get<std::filesystem::path>(resolved);
        REQUIRE(path == weaklyCanonical(sandbox.root / "assets" / "a.txt"));

        if (scenario.contains("same_as")) {
            const auto same = resolver.resolveProjectRef(scenario.at("same_as").get<std::string>());
            REQUIRE(path == std::get<std::filesystem::path>(same));
        }
        return;
    }

    if (mode == "load_text") {
        const auto text = resolver.loadText(ref);
        REQUIRE(contains(text, scenario.at("contains").get<std::string>()));
        return;
    }

    FAIL("unknown fixture mode: " << mode);
}

} // namespace

TEST_CASE("PathResolver project format fixtures", "[pathresolver]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");
    Sandbox sandbox;

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto scenario = readJson(fixtureRoot() / file);
            const auto mode = scenario.value("mode", std::string{});
            if (mode == "resolve_project" || mode == "load_text") {
                const auto expected = entry.at("expect").get<std::string>();

                if (expected == "ok") {
                    REQUIRE_NOTHROW(runScenario(scenario, sandbox));
                } else {
                    std::string message;
                    try {
                        runScenario(scenario, sandbox);
                    } catch (const std::exception &ex) {
                        message = ex.what();
                    }

                    REQUIRE_FALSE(message.empty());
                    requireErrorKind(message, entry.at("error_kind").get<std::string>());
                }
            } else {
                SUCCEED("mode owned by another fixture test");
            }
        }
    }

    resolverForTest().resetForTesting();
}

// Four tests share test/fixtures/project_format and each handles the modes it
// owns, falling through the rest. That arrangement is only honest while every
// mode present is owned by someone: a fixture whose mode is misspelled, or a
// new mode nobody wired up, would otherwise fall through all four and be
// reported as four passes. This is the assertion that makes the fall-throughs
// above safe, so it must list every owner.
TEST_CASE("project format fixture modes are all covered", "[pathresolver][project-format]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");
    const std::set<std::string> owned{
        "resolve_project",  // PathResolver project format fixtures
        "load_text",        // PathResolver project format fixtures
        "scene_format",     // Scene format fixtures accept only v1 documents
        "project_config",   // ProjectBasicConfig loads project.json fixtures
        "shader_stem",      // shader library resolves shader stem project format fixtures
    };

    std::set<std::string> seen;
    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        const auto scenario = readJson(fixtureRoot() / file);
        const auto mode = scenario.value("mode", std::string{});
        INFO("fixture " << file);
        REQUIRE_FALSE(mode.empty());
        REQUIRE(owned.contains(mode));
        seen.insert(mode);
    }

    // The converse: an owner listed here with no fixture left to exercise it
    // means the coverage claim above has quietly stopped meaning anything.
    REQUIRE(seen == owned);
}

TEST_CASE("PathResolver resolves engine resources before setup", "[pathresolver]") {
    auto &resolver = resolverForTest();
    resolver.resetForTesting();

    REQUIRE(contains(resolver.loadText("engine://default_config.json"), "basic_config"));
    REQUIRE_THROWS_WITH(resolver.resolveProjectRef("assets/a.txt"),
                        Catch::Matchers::ContainsSubstring("setup"));

    resolver.resetForTesting();
}

TEST_CASE("Engine resource registry matches project format fixture", "[pathresolver]") {
#if !PELICAN_WITH_VAT || !PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    SKIP("engine resource registry fixture is validated only for the full resource set");
#else
    const auto fixture = readJson(fixtureRoot() / "engine_resources.json");
    REQUIRE(fixture.at("schema").get<std::string>() == "pelican.engine_resources");
    REQUIRE(fixture.at("version").get<int>() == 1);

    const auto fixture_ids = fixture.at("ids").get<std::vector<std::string>>();
    std::vector<std::string> registered_ids;
    for (const auto id : registeredEngineResourceIds()) {
        registered_ids.emplace_back(id);
    }

    REQUIRE(registered_ids == fixture_ids);
#endif
}

TEST_CASE(
    "Standard render algorithm package is a removable engine resource set",
    "[pathresolver][render-algorithm]") {
    constexpr std::string_view resource_id =
        "render_algorithms/planar_reflection/standard_prefilter.comp";
    const auto ids =
        registeredEngineResourceIds();
    const auto registered =
        std::find(
            ids.begin(), ids.end(),
            resource_id);
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    REQUIRE(registered != ids.end());
    REQUIRE(engineResource(resource_id));
#else
    REQUIRE(registered == ids.end());
    REQUIRE_FALSE(engineResource(resource_id));
#endif
}

TEST_CASE("PathResolver setup is single-use outside tests", "[pathresolver]") {
    Sandbox sandbox;
    auto &resolver = resolverForTest();
    resolver.resetForTesting();

    resolver.setup(sandbox.root, false);
    REQUIRE_THROWS_WITH(resolver.setup(sandbox.root, false),
                        Catch::Matchers::ContainsSubstring("more than once"));

    resolver.resetForTesting();
}

TEST_CASE("PathResolver existing-file APIs report concrete paths", "[pathresolver]") {
    Sandbox sandbox;
    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false);

    const auto asset = resolver.resolveExistingFile("assets/a.txt");
    REQUIRE(asset == weaklyCanonical(sandbox.root / "assets" / "a.txt"));

    std::string missing_message;
    try {
        (void)resolver.resolveExistingFile("assets/missing.txt");
    } catch (const std::exception &ex) {
        missing_message = ex.what();
    }
    REQUIRE(contains(missing_message, weaklyCanonical(sandbox.root / "assets" / "missing.txt").string()));

    REQUIRE_THROWS_WITH(resolver.resolveExistingFile("engine://default_config.json"),
                        Catch::Matchers::ContainsSubstring("engine resources"));
    REQUIRE(resolver.loadBytes("assets/a.txt").size() == std::string_view{"asset-content"}.size());

    resolver.resetForTesting();
}

TEST_CASE("PathResolver rejects symlink escapes after canonicalization", "[pathresolver]") {
    Sandbox sandbox;
    const auto link = sandbox.root / "link_outside";
    std::error_code ec;
    std::filesystem::create_directory_symlink(sandbox.outside, link, ec);
    if (ec) {
        SKIP("directory symlink creation unavailable: " << ec.message());
    }

    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false);

    REQUIRE_THROWS_WITH(resolver.resolveProjectRef("link_outside/secret.txt"),
                        Catch::Matchers::ContainsSubstring("escapes project root"));

    resolver.resetForTesting();
}

TEST_CASE("PathResolver resolves user scheme with user-dir override", "[pathresolver]") {
    Sandbox sandbox;
    const auto user_root = sandbox.base / "user";
    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false,
                   projectEnvelope({{"name", "fixture-project"}}),
                   user_root);

    const auto resolved = resolver.resolveProjectRef("user://settings.json");
    REQUIRE(std::get<std::filesystem::path>(resolved) == weaklyCanonical(user_root / "settings.json"));
    REQUIRE_THROWS_WITH(resolver.resolveProjectRef("user://../secret.json"),
                        Catch::Matchers::ContainsSubstring("escapes user root"));

    resolver.resetForTesting();
}

TEST_CASE("PathResolver keeps legacy project paths when no stores are declared", "[pathresolver]") {
    Sandbox sandbox;
    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false,
                   projectEnvelope({{"name", "fixture-project"}}));

    const auto resolved = resolver.resolveProjectRef("assets/a.txt");
    REQUIRE(std::get<std::filesystem::path>(resolved) == weaklyCanonical(sandbox.root / "assets" / "a.txt"));
    REQUIRE(resolver.stores().empty());

    resolver.resetForTesting();
}

TEST_CASE("PathResolver resolves declared asset stores and local overrides", "[pathresolver]") {
    Sandbox sandbox;
    auto &resolver = resolverForTest();
    resolver.resetForTesting();

    const auto project = nlohmann::json{
        {"name", "fixture-project"},
        {"asset_stores", {{"main", {{"mount", "../outside"},
                                     {"manifest", "assets.manifest.json"}}}}},
    };
    resolver.setup(sandbox.root, false, projectEnvelope(project));
    REQUIRE(std::get<std::filesystem::path>(resolver.resolveProjectRef("../outside/outside.txt")) ==
            weaklyCanonical(sandbox.outside / "outside.txt"));
    REQUIRE(resolver.stores().size() == 1);
    REQUIRE(resolver.stores().front().name == "main");
    REQUIRE(resolver.stores().front().root == weaklyCanonical(sandbox.outside));
    REQUIRE(resolver.stores().front().manifest == weaklyCanonical(sandbox.root / "assets.manifest.json"));
    resolver.resetForTesting();

    const auto override_root = sandbox.base / "override_assets";
    writeText(override_root / "a.txt", "override-content");
    writeText(sandbox.root / ".pelican" / "local.json",
              nlohmann::json{{"asset_stores", {{"main", override_root.generic_string()}}}}.dump());
    const auto project_with_assets_store = nlohmann::json{
        {"name", "fixture-project"},
        {"asset_stores", {{"main", {{"mount", "assets"}}}}},
    };
    resolver.setup(sandbox.root, false,
                   projectEnvelope(project_with_assets_store));
    REQUIRE(std::get<std::filesystem::path>(resolver.resolveProjectRef("assets/a.txt")) ==
            weaklyCanonical(override_root / "a.txt"));

    resolver.resetForTesting();
}

TEST_CASE("PathResolver rejects invalid asset store declarations and overrides", "[pathresolver]") {
    {
        Sandbox sandbox;
        auto &resolver = resolverForTest();
        resolver.resetForTesting();
        const auto project = nlohmann::json{
            {"name", "fixture-project"},
            {"asset_stores",
             {
                 {"main", {{"mount", "assets"}}},
                 {"nested", {{"mount", "assets/models"}}},
             }},
        };
        REQUIRE_THROWS_WITH(resolver.setup(sandbox.root, false,
                                           projectEnvelope(project)),
                            Catch::Matchers::ContainsSubstring("overlap"));
        resolver.resetForTesting();
    }

    {
        Sandbox sandbox;
        auto &resolver = resolverForTest();
        resolver.resetForTesting();
        const auto project = nlohmann::json{
            {"name", "fixture-project"},
            {"asset_stores", {{"main", {{"mount", "assets"}}}}},
        };
        resolver.setup(sandbox.root, false, projectEnvelope(project));
        REQUIRE_THROWS_WITH(resolver.resolveProjectRef("assets/../secret.txt"),
                            Catch::Matchers::ContainsSubstring("escapes mount root"));
        resolver.resetForTesting();
    }

    {
        Sandbox sandbox;
        auto &resolver = resolverForTest();
        resolver.resetForTesting();
        const auto project = nlohmann::json{
            {"name", "fixture-project"},
            {"asset_stores", {{"main", {{"mount", "assets"}}}}},
        };
        writeText(sandbox.root / ".pelican" / "local.json",
                  nlohmann::json{{"asset_stores", {{"missing", sandbox.outside.generic_string()}}}}.dump());
        REQUIRE_THROWS_WITH(resolver.setup(sandbox.root, false,
                                           projectEnvelope(project)),
                            Catch::Matchers::ContainsSubstring("undeclared asset store"));
        resolver.resetForTesting();
    }

    {
        Sandbox sandbox;
        auto &resolver = resolverForTest();
        resolver.resetForTesting();
        const auto project = nlohmann::json{
            {"name", "fixture-project"},
            {"asset_stores", {{"main", {{"mount", "assets"}}}}},
        };
        writeText(sandbox.root / ".pelican" / "local.json",
                  nlohmann::json{{"asset_stores", {{"main", sandbox.outside.generic_string()}}},
                                 {"rules", nlohmann::json::object()}}
                      .dump());
        REQUIRE_THROWS_WITH(resolver.setup(sandbox.root, false,
                                           projectEnvelope(project)),
                            Catch::Matchers::ContainsSubstring("unsupported key"));
        resolver.resetForTesting();
    }

    {
        Sandbox sandbox;
        auto &resolver = resolverForTest();
        resolver.resetForTesting();
        const auto project = nlohmann::json{
            {"name", "fixture-project"},
            {"asset_stores", {{"main", {{"mount", "assets"},
                                         {"manifest", "../assets.manifest.json"}}}}},
        };
        resolver.setup(sandbox.root, false, projectEnvelope(project));
        REQUIRE_FALSE(resolver.stores().front().manifest);
        REQUIRE(resolver.stores().front().manifest_error);
        REQUIRE(resolver.stores().front().manifest_error->find("manifest escapes project root") !=
                std::string::npos);
        resolver.resetForTesting();
    }
}

TEST_CASE("PathResolver parses asset fragments without loading subassets", "[pathresolver]") {
    const auto canonical = parsePathReference("assets/character.glb#node/Root/Arm/Cube");
    REQUIRE(canonical.path == "assets/character.glb");
    REQUIRE(canonical.fragment);
    REQUIRE(canonical.fragment->kind == "node");
    REQUIRE(canonical.fragment->path == "Root/Arm/Cube");
    REQUIRE(canonical.fragment->address_kind == AssetFragmentAddressKind::full_path);

    const auto sugar = parsePathReference("assets/character.glb#mesh/Cube");
    REQUIRE(sugar.fragment);
    REQUIRE(sugar.fragment->kind == "mesh");
    REQUIRE(sugar.fragment->path == "Cube");
    REQUIRE(sugar.fragment->address_kind == AssetFragmentAddressKind::name);

    REQUIRE_THROWS_WITH(parsePathReference("assets/character.glb#mesh"),
                        Catch::Matchers::ContainsSubstring("Invalid asset fragment"));
    REQUIRE_THROWS_WITH(parsePathReference("assets/character.glb#mesh/3"),
                        Catch::Matchers::ContainsSubstring("Invalid asset fragment"));
    REQUIRE_THROWS_WITH(parsePathReference("assets/character.glb#mesh/Cube#material/Body"),
                        Catch::Matchers::ContainsSubstring("multiple '#'"));

    Sandbox sandbox;
    writeText(sandbox.root / "assets" / "character.glb", "fake glb");
    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false);

    const auto resolved = resolver.resolveProjectRef("assets/character.glb#node/Root/Arm/Cube");
    const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved);
    REQUIRE(fragment != nullptr);
    REQUIRE(fragment->path == weaklyCanonical(sandbox.root / "assets" / "character.glb"));
    REQUIRE(fragment->fragment.kind == "node");

    REQUIRE_THROWS_WITH(resolver.resolveExistingFile("assets/character.glb#mesh/Cube"),
                        Catch::Matchers::ContainsSubstring("Unsupported asset fragment kind: mesh"));

    const auto existing =
        resolver.resolveExistingFileReference("assets/character.glb#mesh/Cube");
    const auto *existing_fragment = std::get_if<ResolvedPathFragment>(&existing);
    REQUIRE(existing_fragment != nullptr);
    REQUIRE(existing_fragment->path ==
            weaklyCanonical(sandbox.root / "assets" / "character.glb"));
    REQUIRE(existing_fragment->fragment.path == "Cube");

    resolver.resetForTesting();
}

TEST_CASE("WP354 shader source open references preserve logical schemes and exact stage suffixes",
          "[pathresolver][wp354]") {
    Sandbox sandbox;
    const auto user_root = sandbox.base / "user";
    const auto mounted_root = sandbox.base / "mounted-store";
    writeText(sandbox.root / "shaders" / "local.vert", "project vertex");
    writeText(user_root / "shaders" / "personal.comp", "user compute");
    writeText(mounted_root / "fx" / "mounted.frag", "mounted fragment");
    writeText(
        sandbox.root / ".pelican" / "local.json",
        nlohmann::json{
            {"asset_stores",
             {{"shader-store", mounted_root.generic_string()}}}}
            .dump());
    const auto envelope = projectEnvelope({
        {"name", "fixture-project"},
        {"asset_stores",
         {{"shader-store", {{"mount", "mounted"}}}}},
    });

    auto &resolver = resolverForTest();
    resolver.resetForTesting();
    resolver.setup(sandbox.root, false, envelope, user_root);

    const auto unprefixed =
        resolver.resolveShaderSourceOpenReference(
            "shaders/local", ShaderSourceStage::vertex);
    REQUIRE(unprefixed.logical_ref ==
            std::optional<std::string>{
                "project://shaders/local.vert"});
    REQUIRE(unprefixed.physical_path ==
            weaklyCanonical(sandbox.root / "shaders" / "local.vert"));
    REQUIRE_FALSE(unprefixed.reason);

    const auto explicit_project =
        resolver.resolveShaderSourceOpenReference(
            "project://shaders/local", ShaderSourceStage::vertex);
    REQUIRE(explicit_project.logical_ref == unprefixed.logical_ref);
    REQUIRE(explicit_project.physical_path == unprefixed.physical_path);

    const auto user = resolver.resolveShaderSourceOpenReference(
        "user://shaders/personal", ShaderSourceStage::compute);
    REQUIRE(user.logical_ref ==
            std::optional<std::string>{
                "user://shaders/personal.comp"});
    REQUIRE(user.physical_path ==
            weaklyCanonical(user_root / "shaders" / "personal.comp"));

    const auto mount = resolver.resolveShaderSourceOpenReference(
        "mounted/fx/mounted", ShaderSourceStage::fragment);
    REQUIRE(mount.logical_ref ==
            std::optional<std::string>{
                "project://mounted/fx/mounted.frag"});
    REQUIRE(mount.physical_path ==
            weaklyCanonical(mounted_root / "fx" / "mounted.frag"));
    REQUIRE(mount.logical_ref->find(
                mounted_root.generic_string()) == std::string::npos);
    REQUIRE(mount.logical_ref->find(':') ==
            std::string{*mount.logical_ref}.find("project://") + 7);

    const auto engine = resolver.resolveShaderSourceOpenReference(
        "engine://ui", ShaderSourceStage::vertex);
    REQUIRE_FALSE(engine.logical_ref);
    REQUIRE(engine.reason ==
            ShaderSourceOpenReason::embedded_engine_resource);
    const auto missing_engine =
        resolver.resolveShaderSourceOpenReference(
            "engine://definitely_missing_wp354",
            ShaderSourceStage::fragment);
    REQUIRE_FALSE(missing_engine.logical_ref);
    REQUIRE(missing_engine.reason ==
            ShaderSourceOpenReason::source_not_found);

    const auto generated =
        resolver.resolveShaderSourceOpenReference(
            "generated://fixture", ShaderSourceStage::compute);
    REQUIRE(generated.reason == ShaderSourceOpenReason::generated);
    const auto missing = resolver.resolveShaderSourceOpenReference(
        "shaders/missing", ShaderSourceStage::closesthit);
    REQUIRE(missing.reason == ShaderSourceOpenReason::source_not_found);

    ProjectPathResolver project_resolver;
    project_resolver.setup(
        sandbox.root, false, envelope, user_root);
    REQUIRE(materializeShaderSourceOpenReference(
                project_resolver, *unprefixed.logical_ref) ==
            *unprefixed.physical_path);
    REQUIRE(materializeShaderSourceOpenReference(
                project_resolver, *user.logical_ref) ==
            *user.physical_path);
    REQUIRE(materializeShaderSourceOpenReference(
                project_resolver, *mount.logical_ref) ==
            *mount.physical_path);
    REQUIRE_THROWS_WITH(
        materializeShaderSourceOpenReference(
            project_resolver, "engine://ui.vert"),
        Catch::Matchers::ContainsSubstring(
            "requires a canonical project:// or user://"));
    for (const std::string invalid : {
             "project://shaders/../local.vert",
             "project:///absolute.vert",
             "project://shaders/no_stage_suffix",
             "project://shaders/unknown.glsl",
             "user://shaders/../personal.comp",
             "project://mounted/../escape.frag",
             "C:/machine/absolute.vert"}) {
        CAPTURE(invalid);
        REQUIRE_FALSE(
            isCanonicalShaderSourceOpenReference(invalid));
        REQUIRE_THROWS_WITH(
            materializeShaderSourceOpenReference(
                project_resolver, invalid),
            Catch::Matchers::ContainsSubstring("requires a canonical"));
    }
    REQUIRE(isCanonicalShaderSourceOpenReference(
        "project://mounted/fx/mounted.frag"));
    REQUIRE(isCanonicalShaderSourceOpenReference(
        "user://shaders/personal.comp"));
    REQUIRE(isCanonicalShaderSourceOpenReference(
        "project://shaders/local.vert", ShaderSourceStage::vertex));
    REQUIRE_FALSE(isCanonicalShaderSourceOpenReference(
        "project://shaders/local.vert", ShaderSourceStage::fragment));

    REQUIRE_THROWS_WITH(
        resolver.resolveShaderSourceOpenReference(
            "../outside", ShaderSourceStage::vertex),
        Catch::Matchers::ContainsSubstring("escapes project root"));
    REQUIRE_THROWS_WITH(
        resolver.resolveShaderSourceOpenReference(
            "project://../outside", ShaderSourceStage::vertex),
        Catch::Matchers::ContainsSubstring("escapes project root"));
    REQUIRE_THROWS_WITH(
        resolver.resolveShaderSourceOpenReference(
            "user://../outside", ShaderSourceStage::compute),
        Catch::Matchers::ContainsSubstring("escapes user root"));
    REQUIRE_THROWS_WITH(
        resolver.resolveShaderSourceOpenReference(
            "mounted/../outside", ShaderSourceStage::fragment),
        Catch::Matchers::ContainsSubstring("escapes mount root"));

    // Absolute paths are a CLI-only escape hatch.  Even when that hatch is
    // enabled, a project-owned shader stem must never be converted into a
    // machine-specific source_open_ref on the runtime wire.
    ProjectPathResolver absolute_enabled;
    absolute_enabled.setup(
        sandbox.root, true, envelope, user_root);
    const auto absolute_stem =
        (sandbox.root / "shaders" / "local").generic_string();
    REQUIRE_THROWS_WITH(
        Pelican::resolveShaderSourceOpenReference(
            absolute_enabled, absolute_stem,
            ShaderSourceStage::vertex),
        Catch::Matchers::ContainsSubstring(
            "Absolute project path references are not allowed"));

#if PELICAN_RUNTIME_SHADER_COMPILER
    INFO("runtime compiler ON uses the same source_open_ref resolver");
#else
    INFO("runtime compiler OFF uses the same source_open_ref resolver");
#endif
    resolver.resetForTesting();
    REQUIRE_THROWS_WITH(
        resolver.resolveShaderSourceOpenReference(
            "shaders/local", ShaderSourceStage::vertex),
        Catch::Matchers::ContainsSubstring(
            "PathResolver setup must be called"));
}

} // namespace Pelican
