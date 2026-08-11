#include "project.hpp"
#include "selection.hpp"
#include "sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
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
    const auto model = ProjectOutlinerModel::open(project_root);

    REQUIRE(model.projectName() == "example");
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

    const auto model =
        ProjectOutlinerModel::open(sandbox.root / "project.json");
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
    const auto project = ProjectOutlinerModel::open(project_root);
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
    const auto project = ProjectOutlinerModel::open(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example");
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
    const auto project = ProjectOutlinerModel::open(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example");
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
