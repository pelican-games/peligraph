#include "project.hpp"
#include "selection.hpp"
#include "sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using PelicanStudio::OutlinerScene;
using PelicanStudio::ProjectOutlinerModel;
using PelicanStudio::SelectionModel;
using PelicanStudio::SelectionUpdateKind;

const OutlinerScene &requireScene(const ProjectOutlinerModel &model,
                                  std::string_view scene_id) {
    for (const auto &scene : model.scenes()) {
        if (scene.scene_id == scene_id) {
            return scene;
        }
    }
    FAIL("missing scene: " << scene_id);
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not open " + path.string());
    return nlohmann::json::parse(stream);
}

nlohmann::json sceneTree(std::string scene_id,
                         const nlohmann::json &objects,
                         std::uint64_t revision = 1,
                         std::uint64_t first_authoring_id = 1) {
    nlohmann::json result{{"scene_revision", revision},
                          {"scene_id", scene_id},
                          {"objects", nlohmann::json::array()}};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        nlohmann::json object{
            {"scene_revision", revision},
            {"authoring_object_id", first_authoring_id + index},
            {"declaration_index", index},
            {"components", nlohmann::json::array()},
        };
        if (objects.at(index).contains("name")) {
            object["name"] = objects.at(index).at("name");
        }
        if (objects.at(index).contains("parent")) {
            object["parent"] = objects.at(index).at("parent");
        }
        result["objects"].push_back(std::move(object));
    }
    return result;
}

ProjectOutlinerModel exampleModel() {
    const auto project_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example";
    auto model = ProjectOutlinerModel::open(project_root);
    const auto normalized = Pelican::normalizeSceneDataJson(
        readJson(project_root / "scenes" / "main.scene.json"));
    REQUIRE(model.scenes().empty());
    REQUIRE(model.updateSceneTree(sceneTree(
        "default_scene",
        normalized.scenes.at("default_scene").at("objects"), 1)));
    REQUIRE(model.updateSceneTree(sceneTree(
        "scene_flow_second",
        normalized.scenes.at("scene_flow_second").at("objects"), 1,
        1000)));
    return model;
}

struct ProjectSandbox {
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() /
               ("pelican_devstudio_outliner_" + suffix);
        std::filesystem::create_directories(root / "scenes");
    }

    ~ProjectSandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(std::string_view relative_path, std::string_view contents) {
        std::ofstream stream(root / relative_path, std::ios::binary);
        REQUIRE(stream);
        stream << contents;
        REQUIRE(stream.good());
    }
};

} // namespace

TEST_CASE("Devstudio outliner opens the example without collapsing unnamed objects",
          "[devstudio][outliner][wp250]") {
    const auto project_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example";
    auto model = ProjectOutlinerModel::open(project_root);

    REQUIRE(model.projectName() == "example");
    REQUIRE(model.scenes().empty());
    const auto normalized = Pelican::normalizeSceneDataJson(
        readJson(project_root / "scenes" / "main.scene.json"));
    REQUIRE(model.updateSceneTree(sceneTree(
        "default_scene",
        normalized.scenes.at("default_scene").at("objects"), 1)));
    REQUIRE(model.updateSceneTree(sceneTree(
        "scene_flow_second",
        normalized.scenes.at("scene_flow_second").at("objects"), 1,
        1000)));
    REQUIRE(model.scenes().size() == 2);

    const auto &main = requireScene(model, "default_scene");
    const auto &second = requireScene(model, "scene_flow_second");
    REQUIRE(main.objects.size() == 47);
    REQUIRE(second.objects.size() == 2);

    std::set<std::pair<std::string, std::size_t>> keys;
    std::set<std::string> display_names;
    std::size_t unnamed_count = 0;
    for (std::size_t index = 0; index < main.objects.size(); ++index) {
        const auto &object = main.objects[index];
        REQUIRE(object.key.scene_id == "default_scene");
        REQUIRE(object.key.declaration_index == index);
        keys.emplace(object.key.scene_id, object.key.declaration_index);
        display_names.insert(object.display_name);
        if (object.display_name.starts_with("pelican://")) {
            ++unnamed_count;
            REQUIRE(object.display_name == Pelican::runtimeObjectIdentityName(
                                               "default_scene", index + 1, {}));
        }
    }
    REQUIRE(keys.size() == 47);
    REQUIRE(display_names.size() == 47);
    REQUIRE(unnamed_count == 32);
}

TEST_CASE("Devstudio outliner projects parent references onto declaration identities",
          "[devstudio][outliner][wp250]") {
    ProjectSandbox sandbox;
    sandbox.write("project.json", R"json({
  "schema": "pelican.project",
  "version": 1,
  "name": "tree-fixture",
  "basic_config": {"scene_data_json": "scenes/tree.scene.json"}
})json");
    sandbox.write("scenes/tree.scene.json", R"json({
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "main": {
      "objects": [
        {"name": "Root", "components": []},
        {"components": []},
        {"name": "Child", "parent": "Root", "components": []},
        {"name": "Grandchild", "parent": "Child", "components": []}
      ]
    }
  }
})json");

    auto model =
        ProjectOutlinerModel::open(sandbox.root / "project.json");
    REQUIRE(model.scenes().empty());
    const auto authored = readJson(sandbox.root / "scenes/tree.scene.json");
    REQUIRE(model.updateSceneTree(sceneTree(
        "main", authored.at("scenes").at("main").at("objects"))));
    const auto &scene = requireScene(model, "main");
    REQUIRE(scene.root_declaration_indices ==
            std::vector<std::size_t>{0, 1});
    REQUIRE(scene.objects[0].child_declaration_indices ==
            std::vector<std::size_t>{2});
    REQUIRE(scene.objects[2].parent_declaration_index == 0);
    REQUIRE(scene.objects[2].child_declaration_indices ==
            std::vector<std::size_t>{3});
    REQUIRE(scene.objects[3].parent_declaration_index == 2);
}

TEST_CASE("Viewport picks and outliner rows share declaration identity headlessly",
          "[devstudio][selection][wp264]") {
    const auto project_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example";
    const auto project = exampleModel();
    const auto &scene = requireScene(project, "default_scene");

    const auto unnamed = std::find_if(
        scene.objects.begin(), scene.objects.end(), [](const auto &object) {
            return object.display_name.starts_with("pelican://");
        });
    REQUIRE(unnamed != scene.objects.end());

    SelectionModel selection;
    selection.bindProject(&project);
    const auto pick_token = selection.beginViewportPick();
    const nlohmann::json pick_result{
        {"contract", 1},
        {"coordinate", {{"x", 12}, {"y", 34}}},
        {"extent", {{"width", 640}, {"height", 360}}},
        {"frame_index", 9},
        {"hit",
         {{"scene_id", unnamed->key.scene_id},
          {"declaration_index", unnamed->key.declaration_index},
          {"authoring_object_id", "session-only"}}},
    };

    const auto picked =
        selection.completeViewportPick(pick_token, pick_result.dump());
    REQUIRE(picked.kind == SelectionUpdateKind::changed);
    REQUIRE(selection.selected() == unnamed->key);
    REQUIRE(project.findObject(*selection.selected()) == &*unnamed);

    const auto outliner_selected =
        selection.selectFromOutliner(scene.objects.front().key);
    REQUIRE(outliner_selected.applied());
    REQUIRE(selection.selected() == scene.objects.front().key);
}

TEST_CASE("Newer outliner selection wins over an older viewport response",
          "[devstudio][selection][wp264]") {
    const auto project = exampleModel();
    const auto &scene = requireScene(project, "default_scene");

    SelectionModel selection;
    selection.bindProject(&project);
    const auto old_pick = selection.beginViewportPick();
    REQUIRE(selection.selectFromOutliner(scene.objects.at(1).key).applied());

    const nlohmann::json late_result{
        {"contract", 1},
        {"hit",
         {{"scene_id", scene.objects.at(2).key.scene_id},
          {"declaration_index",
           scene.objects.at(2).key.declaration_index}}},
    };
    REQUIRE(selection.completeViewportPick(old_pick, late_result.dump()).kind ==
            SelectionUpdateKind::stale);
    REQUIRE(selection.selected() == scene.objects.at(1).key);
}

TEST_CASE("Background clears selection while unavailable picking is explicit and preserves it",
          "[devstudio][selection][wp264]") {
    const auto project = exampleModel();
    const auto &scene = requireScene(project, "default_scene");

    SelectionModel selection;
    selection.bindProject(&project);
    REQUIRE(selection.selectFromOutliner(scene.objects.at(1).key).applied());

    const auto background_pick = selection.beginViewportPick();
    const auto cleared = selection.completeViewportPick(
        background_pick, R"json({"contract":1,"hit":null})json");
    REQUIRE(cleared.kind == SelectionUpdateKind::changed);
    REQUIRE_FALSE(selection.selected());

    REQUIRE(selection.selectFromOutliner(scene.objects.at(1).key).applied());
    const auto unavailable_pick = selection.beginViewportPick();
    const auto unavailable = selection.failViewportPick(
        unavailable_pick,
        "pick_object failed (RPC -32000): picking readback requires "
        "engine://features/picking.json in the active render graph");
    REQUIRE(unavailable.kind == SelectionUpdateKind::failed);
    REQUIRE(unavailable.message.find("engine://features/picking.json") !=
            std::string::npos);
    REQUIRE(selection.selected() == scene.objects.at(1).key);
}

TEST_CASE("Studio outliner follows RPC snapshot and structure revisions instead of disk",
          "[devstudio][outliner][wp360a]") {
    ProjectSandbox sandbox;
    sandbox.write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"rpc-authority",
  "basic_config":{"scene_data_json":"scenes/tree.scene.json"}
})json");
    sandbox.write("scenes/tree.scene.json", R"json({
  "schema":"pelican.scene","version":1,
  "scenes":{"main":{"objects":[{"name":"DiskOnly","components":[]}]}}
})json");

    auto model = ProjectOutlinerModel::open(sandbox.root);
    REQUIRE(model.scenes().empty());

    const nlohmann::json imported_objects = nlohmann::json::array({
        nlohmann::json{{"name", "ImportedSnapshot"}},
    });
    REQUIRE(model.updateSceneTree(
        sceneTree("main", imported_objects, 10, 900)));
    REQUIRE(requireScene(model, "main").objects.size() == 1);
    REQUIRE(requireScene(model, "main").objects[0].display_name ==
            "ImportedSnapshot");
    REQUIRE(requireScene(model, "main").objects[0].display_name !=
            "DiskOnly");

    const nlohmann::json edited_objects = nlohmann::json::array({
        nlohmann::json{{"name", "ImportedSnapshot"}},
        nlohmann::json{{"name", "RuntimeChild"},
                       {"parent", "ImportedSnapshot"}},
    });
    REQUIRE(model.updateSceneTree(sceneTree("main", edited_objects, 11, 900)));
    const auto &updated = requireScene(model, "main");
    REQUIRE(updated.objects.size() == 2);
    REQUIRE(updated.objects[1].parent_declaration_index == 0);
    REQUIRE(updated.objects[0].child_declaration_indices ==
            std::vector<std::size_t>{1});
    std::cout << "WP360A_OUTLINER disk=DiskOnly snapshot="
              << updated.objects[0].display_name
              << " structure_child=" << updated.objects[1].display_name
              << " revision=" << *model.sceneRevision() << '\n';
}
