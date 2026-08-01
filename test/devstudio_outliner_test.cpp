#include "project.hpp"
#include "sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(main.objects.size() == 46);
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
    REQUIRE(keys.size() == 46);
    REQUIRE(display_names.size() == 46);
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
