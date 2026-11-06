#include "prefabplacement.hpp"
#include "project.hpp"

#include "../src/project/prefab.hpp"
#include "../src/project/projectformat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using Pelican::PrefabError;
using Pelican::PrefabErrorCode;
using PelicanStudio::PrefabListEntry;
using PelicanStudio::ProjectOutlinerModel;
namespace fs = std::filesystem;

fs::path projectRoot() {
    return fs::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "prefab_demo";
}

std::string readBytes(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("could not read " + path.string());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

Json outlineRow(std::uint64_t id, std::size_t index, const std::string &name) {
    return {{"authoring_object_id", id}, {"declaration_index", index},
            {"name", name}, {"parent_authoring_object_id", nullptr}};
}

} // namespace

TEST_CASE("PFB-1 Studio parses the real prefab registry and builds valid placement names",
          "[pfb1][devstudio][prefab]") {
    using namespace PelicanStudio;
    const auto root = projectRoot();
    const auto project = Pelican::parseProjectEnvelopeText(
        readBytes(root / "project.json"));
    const auto registry = Pelican::buildPrefabRegistrySnapshot(
        project.envelope.prefabs, [&](std::string_view path) {
            return readBytes(root / std::string{path});
        });
    const auto entries = parsePrefabList(Pelican::prefabRegistryListJson(registry));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "crate");
    CHECK(entries[0].required_parameters == std::vector<std::string>{"model"});
    CHECK(entries[1].name == "lamp");
    CHECK(entries[1].required_parameters.empty());
    CHECK(parsePrefabList(Json{{"prefabs", Json::array()}}).empty());
    CHECK_THROWS_AS(parsePrefabList(Json{{"prefabs", Json::object()}}),
                    std::runtime_error);
    CHECK_THROWS_AS(parsePrefabList(Json{{"prefabs", Json::array({
                        Json{{"parameters", Json::array()}}})}}),
                    std::runtime_error);
    CHECK_THROWS_AS(parsePrefabList(Json{{"prefabs", Json::array({
                        Json{{"name", "lamp"}, {"parameters", Json::array({
                            Json{{"name", "brightness"}, {"required", 0}}})}}})}}),
                    std::runtime_error);
    CHECK_THROWS_AS(parsePrefabList(Json{{"prefabs", Json::array({
                        Json{{"name", ""}, {"parameters", Json::array()}}})}}),
                    std::runtime_error);

    CHECK(prefabMenuLabel(PrefabListEntry{"lamp", {}}) == "lamp");
    CHECK(prefabMenuLabel(PrefabListEntry{"crate", {"model"}}) ==
          "crate (needs model)");
    CHECK(prefabMenuLabel(PrefabListEntry{"crate", {"a", "b"}}) ==
          "crate (needs a, b)");
    CHECK(uniquePrefabObjectName("lamp", {}) == "lamp_1");
    CHECK(uniquePrefabObjectName("lamp", {"lamp_1"}) == "lamp_2");
    CHECK(uniquePrefabObjectName("lamp", {"lamp_2"}) == "lamp_1");
    CHECK(prefabObjectBaseName("a-b") == "a_b");
    const auto long_name = prefabObjectBaseName(std::string(64, 'a'));
    CHECK(long_name.size() == 48);
    const auto object_name = uniquePrefabObjectName(long_name, {});
    CHECK(Pelican::isPrefabIdentifier("gi_" + object_name));
    CHECK(uniquePrefabInstanceId("lamp_1", {}) == "gi_lamp_1");
    CHECK(uniquePrefabInstanceId("lamp_1", {"gi_lamp_1"}) == "gi_lamp_1_2");
    CHECK(uniquePrefabInstanceId("lamp_2", {"gi_lamp_1"}) == "gi_lamp_2");
}

TEST_CASE("PFB-1 Studio declarations use the real prefab resolver and defaults",
          "[pfb1][devstudio][prefab][resolver]") {
    using namespace PelicanStudio;
    const auto root = projectRoot();
    const auto project = Pelican::parseProjectEnvelopeText(
        readBytes(root / "project.json"));
    const auto registry = Pelican::buildPrefabRegistrySnapshot(
        project.envelope.prefabs, [&](std::string_view path) {
            return readBytes(root / std::string{path});
        });
    const auto declaration = prefabObjectDeclaration("lamp_1", "lamp");
    REQUIRE(declaration.at("components").size() == 1);
    CHECK(declaration.at("components").at(0).at("name") == "transform");
    CHECK_FALSE(declaration.at("prefab").contains("parameters"));
    const auto instance = Pelican::resolvePrefabInstance(
        declaration.at("prefab"), registry);
    CHECK(instance.ref == "lamp");
    CHECK(instance.instance_id == "gi_lamp_1");
    REQUIRE(instance.parameters.size() == 1);
    CHECK(instance.parameters[0].name == "brightness");
    CHECK(instance.parameters[0].value_resolved.get<double>() == 2.0);
    CHECK_FALSE(instance.parameters[0].is_override);
    try {
        (void)Pelican::resolvePrefabInstance(
            prefabObjectDeclaration("crate_2", "crate").at("prefab"), registry);
        FAIL("crate without its required model must be refused");
    } catch (const PrefabError &error) {
        CHECK(error.code() == PrefabErrorCode::PrefabParameterRequired);
    }
}

TEST_CASE("PFB-1 Outliner keeps object names raw and prefab metadata separate",
          "[pfb1][devstudio][prefab][outliner]") {
    auto model = ProjectOutlinerModel::open(projectRoot());
    auto lamp = outlineRow(10, 0, "lamp_1");
    lamp["prefab_ref"] = "lamp";
    lamp["prefab_instance_id"] = "gi_lamp_1";
    CHECK(model.updateSceneOutline(Json{{"scene_revision", 1},
        {"scene_id", "default_scene"}, {"not_modified", false},
        {"objects", Json::array({lamp, outlineRow(11, 1, "Camera")})}}));
    const auto *scene = model.findScene("default_scene");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 2);
    CHECK(scene->objects[0].display_name == "lamp_1");
    CHECK(scene->objects[0].name == "lamp_1");
    CHECK(scene->objects[0].prefab_ref == "lamp");
    CHECK(scene->objects[0].prefab_instance_id == "gi_lamp_1");
    CHECK(scene->objects[1].display_name == "Camera");
    CHECK(scene->objects[1].prefab_ref.empty());

    auto invalid = lamp;
    invalid["prefab_ref"] = "";
    CHECK_THROWS_AS(model.updateSceneOutline(Json{{"scene_revision", 2},
        {"scene_id", "default_scene"}, {"objects", Json::array({invalid})}}),
                    std::runtime_error);
    invalid["prefab_ref"] = 1;
    CHECK_THROWS_AS(model.updateSceneOutline(Json{{"scene_revision", 2},
        {"scene_id", "default_scene"}, {"objects", Json::array({invalid})}}),
                    std::runtime_error);

    auto tree_model = ProjectOutlinerModel::open(projectRoot());
    auto tree_lamp = outlineRow(10, 0, "lamp_1");
    tree_lamp.erase("parent_authoring_object_id");
    tree_lamp["prefab_instance"] = {{"ref", "lamp"}, {"instance_id", "gi_lamp_1"}};
    CHECK(tree_model.updateSceneTree(Json{{"scene_revision", 1},
        {"scene_id", "default_scene"}, {"objects", Json::array({tree_lamp})}}));
    CHECK(tree_model.findScene("default_scene")->objects[0].prefab_ref.empty());
}
