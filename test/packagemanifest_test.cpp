#include "../src/project/packagemanifest.hpp"
#include "../src/project/projectformat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::filesystem::path templateRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
           "fixtures" / "package_template";
}

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.is_open());
    output << bytes;
}

// A scratch directory that goes away with the test, so nothing accumulates in
// the machine's temp directory.
class Scratch {
    std::filesystem::path root_;

  public:
    explicit Scratch(const std::string &name) {
        root_ = std::filesystem::temp_directory_path() /
                ("pelican_x1_unit_" + name + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;
    const std::filesystem::path &path() const noexcept { return root_; }
};

// A package copied out of the template, with the manifest transformed.
std::filesystem::path stagePackage(
    const std::filesystem::path &destination,
    const std::function<void(Json &)> &edit_manifest = {}) {
    std::filesystem::remove_all(destination);
    std::filesystem::copy(templateRoot(), destination,
                          std::filesystem::copy_options::recursive);
    if (edit_manifest) {
        auto manifest =
            Json::parse(readBytes(destination / packageManifestFile));
        edit_manifest(manifest);
        writeBytes(destination / packageManifestFile, manifest.dump(2));
    }
    return destination;
}

// The smallest project.json the package path reads.
void writeProject(const std::filesystem::path &project_root) {
    std::filesystem::create_directories(project_root);
    writeBytes(project_root / "project.json", R"json({
  "schema": "pelican.project",
  "version": 1,
  "name": "package-unit",
  "engine_min_version": "0.1.0"
}
)json");
}

std::string rejectionCode(const std::function<void()> &action) {
    try {
        action();
    } catch (const PackageError &error) {
        return error.code();
    } catch (const std::exception &error) {
        return std::string{"unnamed: "} + error.what();
    }
    return "accepted";
}

} // namespace

TEST_CASE("X-1 the package template parses as a pelican.package v1 manifest",
          "[package][x1]") {
    const auto manifest = readPackageManifest(templateRoot());
    REQUIRE(manifest.name_space == "acme.glowkit");
    REQUIRE(manifest.package_version == "1.0.0");
    REQUIRE(manifest.requirements.engine_min_version == "0.1.0");
    REQUIRE(manifest.requirements.sdk_abi.has_value());
    // The manifest declares the epoch this engine actually loads code with, so
    // the template installs into the engine it ships beside.
    REQUIRE(*manifest.requirements.sdk_abi == packageSdkAbiVersion());
    REQUIRE(manifest.provides.code);
    REQUIRE(manifest.provides.features ==
            std::vector<std::string>{"features/glow.json"});
    REQUIRE(manifest.provides.graphs ==
            std::vector<std::string>{"graphs/pulse.json"});
}

TEST_CASE("X-1 a package manifest is read with pelican.import strictness",
          "[package][x1]") {
    const auto bytes = readBytes(templateRoot() / packageManifestFile);

    const auto parseWith = [&bytes](const std::function<void(Json &)> &edit) {
        auto manifest = Json::parse(bytes);
        edit(manifest);
        return rejectionCode(
            [&manifest] { (void)parsePackageManifestJson(manifest); });
    };

    REQUIRE(parseWith([](Json &) {}) == "accepted");
    REQUIRE(parseWith([](Json &m) { m["surprise"] = 1; }) ==
            "package_manifest_invalid");
    REQUIRE(parseWith([](Json &m) { m["version"] = 2; }) ==
            "package_manifest_invalid");
    REQUIRE(parseWith([](Json &m) { m["schema"] = "pelican.project"; }) ==
            "package_manifest_invalid");
    REQUIRE(parseWith([](Json &m) { m["namespace"] = "acme glowkit"; }) ==
            "package_namespace_invalid");
    REQUIRE(parseWith([](Json &m) { m["namespace"] = "acme..kit"; }) ==
            "package_namespace_invalid");
    REQUIRE(parseWith([](Json &m) { m["package_version"] = "1.0"; }) ==
            "package_manifest_invalid");
    REQUIRE(parseWith([](Json &m) {
                m["provides"]["features"] = Json::array({"/abs/glow.json"});
            }) == "package_path_invalid");
    REQUIRE(parseWith([](Json &m) {
                m["provides"]["features"] =
                    Json::array({"../outside/glow.json"});
            }) == "package_path_invalid");
    REQUIRE(parseWith([](Json &m) {
                m["provides"]["features"] =
                    Json::array({"project://features/glow.json"});
            }) == "package_path_invalid");
    REQUIRE(parseWith([](Json &m) {
                m["provides"]["features"] = Json::array({"features\\glow.json"});
            }) == "package_path_invalid");
    REQUIRE(parseWith([](Json &m) { m["requires"].erase("sdk_abi"); }) ==
            "package_manifest_invalid");
    REQUIRE(parseWith([](Json &m) {
                m["requires"].erase("sdk_abi");
                m["provides"]["code"] = false;
            }) == "accepted");
}

TEST_CASE("X-1 install checks everything before it copies a byte",
          "[package][x1]") {
    Scratch scratch{"install"};
    const auto project = scratch.path() / "project";
    writeProject(project);

    const auto too_new = stagePackage(scratch.path() / "too_new", [](Json &m) {
        m["requires"]["engine_min_version"] = "9.0.0";
    });
    REQUIRE(rejectionCode([&] { (void)installPackage(too_new, project); }) ==
            "package_engine_too_new");

    const auto bad_abi = stagePackage(scratch.path() / "bad_abi", [](Json &m) {
        m["requires"]["sdk_abi"] = 999;
    });
    REQUIRE(rejectionCode([&] { (void)installPackage(bad_abi, project); }) ==
            "package_sdk_abi_mismatch");

    const auto needs = stagePackage(scratch.path() / "needs", [](Json &m) {
        m["dependencies"] = Json::array(
            {Json{{"namespace", "acme.absent"}, {"package_version", "1.0.0"}}});
    });
    REQUIRE(rejectionCode([&] { (void)installPackage(needs, project); }) ==
            "package_dependency_missing");

    const auto nameless = stagePackage(scratch.path() / "nameless");
    {
        auto feature = Json::parse(readBytes(nameless / "features/glow.json"));
        feature.erase("name");
        writeBytes(nameless / "features/glow.json", feature.dump(2));
    }
    REQUIRE(rejectionCode([&] { (void)installPackage(nameless, project); }) ==
            "package_feature_invalid");

    const auto no_manifest = stagePackage(scratch.path() / "no_manifest");
    std::filesystem::remove(no_manifest / packageManifestFile);
    REQUIRE(rejectionCode([&] { (void)installPackage(no_manifest, project); }) ==
            "package_manifest_missing");

    // Nothing above reached the project.
    REQUIRE_FALSE(std::filesystem::exists(project / "packages"));
    REQUIRE(readInstalledPackages(project).empty());
}

// PG-1 acceptance 3: `requires.capabilities` speaks the `requires` vocabulary of
// project.json. An unknown word, a word this build left out and a word it has
// are the three answers, and the first two are refused before a byte is copied.
TEST_CASE("PG-1 install checks requires.capabilities by the requires vocabulary",
          "[package][pg1]") {
    Scratch scratch{"capabilities"};
    const auto project = scratch.path() / "project";
    writeProject(project);

    const auto unknown = stagePackage(scratch.path() / "unknown", [](Json &m) {
        m["requires"]["capabilities"] = Json::array({"physics", "telepathy"});
    });
    REQUIRE(rejectionCode([&] { (void)installPackage(unknown, project); }) ==
            "package.capability_unknown@1");
    try {
        (void)installPackage(unknown, project);
    } catch (const PackageError &error) {
        REQUIRE(error.names() == std::vector<std::string>{"telepathy"});
    }

    // Every name is known before the first unavailable one is reported, so an
    // unknown word after an OFF one is still the unknown-word answer.
    const auto unknown_after = stagePackage(scratch.path() / "unknown_after", [](Json &m) {
        m["requires"]["capabilities"] = Json::array({"audio", "physics", "telepathy"});
    });
    REQUIRE(rejectionCode([&] { (void)installPackage(unknown_after, project); }) ==
            "package.capability_unknown@1");
    REQUIRE_FALSE(std::filesystem::exists(project / "packages"));

    // Each word this build left out answers under its module's name; each one
    // it has installs. The same rows decide both.
    for (const auto *word : {"animation", "audio", "physics", "xr", "vat", "seqplayer"}) {
        CAPTURE(word);
        const auto staged = stagePackage(scratch.path() / (std::string{"needs_"} + word),
            [word](Json &m) {
                m["namespace"] = std::string{"acme.needs_"} + word;
                m["requires"]["capabilities"] = Json::array({word});
            });
        const auto code = rejectionCode([&] { (void)installPackage(staged, project); });
        const auto unavailable = std::string{projectRequirementUnavailableCode(word)};
        if (unavailable.empty()) {
            REQUIRE(code == "accepted");
        } else {
            REQUIRE(unavailable == std::string{word} + ".module_unavailable@1");
            REQUIRE(code == unavailable);
        }
    }
}

TEST_CASE("X-1 install vendors the package and puts the namespace in the "
          "document",
          "[package][x1]") {
    Scratch scratch{"vendor"};
    const auto project = scratch.path() / "project";
    writeProject(project);
    const auto source = stagePackage(scratch.path() / "source");

    const auto result = installPackage(source, project);
    REQUIRE(result.manifest.name_space == "acme.glowkit");
    REQUIRE(result.feature_references ==
            std::vector<std::string>{
                "project://packages/acme.glowkit/features/glow.json"});
    REQUIRE(result.feature_names ==
            std::vector<std::string>{"acme.glowkit.glow"});
    REQUIRE(result.sha256.size() == 64);

    const auto vendored = project / "packages" / "acme.glowkit";
    REQUIRE(std::filesystem::is_directory(vendored));
    REQUIRE(std::filesystem::is_regular_file(vendored / "code" / "system.cpp"));
    const auto feature =
        Json::parse(readBytes(vendored / "features" / "glow.json"));
    REQUIRE(feature.at("name").get<std::string>() == "acme.glowkit.glow");

    const auto installed = readInstalledPackages(project);
    REQUIRE(installed.size() == 1);
    REQUIRE(installed.front().name_space == "acme.glowkit");
    REQUIRE(installed.front().package_version == "1.0.0");
    REQUIRE(installed.front().sha256 == result.sha256);
    // The digest is over the installed bytes, so it is checkable later.
    REQUIRE(packageTreeDigest(vendored) == result.sha256);

    const auto documents =
        readPackageFeatureDocuments(project, "acme.glowkit");
    REQUIRE(documents.size() == 1);
    REQUIRE(documents.front().error.empty());
    REQUIRE(documents.front().name == "acme.glowkit.glow");

    REQUIRE(rejectionCode([&] { (void)installPackage(source, project); }) ==
            "package_already_installed");

    // Editing a vendored file is visible in the digest, and the damaged
    // document is reported rather than thrown.
    auto damaged = Json::parse(readBytes(vendored / "features" / "glow.json"));
    damaged.erase("name");
    writeBytes(vendored / "features" / "glow.json", damaged.dump(2));
    REQUIRE(packageTreeDigest(vendored) != result.sha256);
    const auto damaged_documents =
        readPackageFeatureDocuments(project, "acme.glowkit");
    REQUIRE(damaged_documents.size() == 1);
    REQUIRE_FALSE(damaged_documents.front().error.empty());

    REQUIRE(rejectionCode([&] {
                uninstallPackage(
                    project, "acme.glowkit",
                    {"project://packages/acme.glowkit/features/glow.json"});
            }) == "package_in_use");
    REQUIRE(std::filesystem::exists(vendored));

    uninstallPackage(project, "acme.glowkit", {});
    REQUIRE_FALSE(std::filesystem::exists(vendored));
    REQUIRE(readInstalledPackages(project).empty());
    REQUIRE(rejectionCode(
                [&] { uninstallPackage(project, "acme.glowkit", {}); }) ==
            "package_not_installed");
}

} // namespace Pelican
