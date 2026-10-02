#include "outlineredit.hpp"
#include "project.hpp"
#include "selection.hpp"
#include "sceneformat.hpp"
#include "../src/core/loader/authoringscenedocument.hpp"
#include "../src/core/loader/prefabprovider.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/project/prefab.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/projectmigration.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using PelicanStudio::OutlinerObjectKey;
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
        const auto path = root / relative_path;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
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

TEST_CASE("WP361 Studio offline open resolves the configured scene and every prefab",
          "[devstudio][outliner][wp361][prefab][offline]") {
    ProjectSandbox sandbox;
    sandbox.write("project.json", R"json({
      "schema":"pelican.project","version":1,"name":"prefab-offline",
      "basic_config":{"scene_data_json":"scenes/main.scene.json"},
      "prefabs":[{"name":"unit","path":"prefabs/unit.json"}]
    })json");
    sandbox.write("prefabs/unit.json", R"json({
      "schema":"pelican.prefab","version":1,"name":"unit",
      "parameters":[{"name":"texture","kind":"asset","asset_kind":"texture","default":"white"}],
      "components":[{"name":"sprite_view","id":"sprite","texture":{"$param":"texture"}}]
    })json");
    sandbox.write("scenes/main.scene.json", R"json({
      "schema":"pelican.scene","version":2,
      "scenes":{"main":{"objects":[
        {"name":"Instance","components":[{"name":"transform"}],
         "prefab":{"ref":"unit","instance_id":"gi_main","parameters":{"texture":"blue"}}}
      ]}}
    })json");

    const auto model = ProjectOutlinerModel::open(sandbox.root);
    REQUIRE(model.offlinePrefabExpansions().size() == 1);
    const auto &expansion = model.offlinePrefabExpansions().front();
    REQUIRE(expansion.scene_id == "main");
    REQUIRE(expansion.object_name == "Instance");
    REQUIRE(expansion.instance_id == "gi_main");
    REQUIRE(expansion.resolved_component_json.size() == 1);
    REQUIRE(nlohmann::json::parse(expansion.resolved_component_json.front()) ==
            nlohmann::json{{"name", "sprite_view"}, {"texture", "blue"}});

    const auto project = Pelican::parseProjectEnvelopeJson(
        readJson(sandbox.root / "project.json"));
    const auto registry = Pelican::buildPrefabRegistrySnapshot(
        project.envelope.prefabs, [&](std::string_view relative) {
            return readJson(sandbox.root / std::filesystem::path{relative})
                .dump();
        });
    const auto document = Pelican::AuthoringSceneDocument::load(
        readJson(sandbox.root / "scenes/main.scene.json").dump(),
        Pelican::SceneRevision{1});
    const auto engine = Pelican::ResolvedSceneResolver::resolve(
        document, Pelican::SceneResolverGeneration{1}, {}, registry,
        Pelican::buildProductionBindableProviderSnapshot(1));
    std::vector<nlohmann::json> engine_generated;
    for (const auto &component :
        engine.findScene("main")->objects.front().components) {
        if (component.origin.isGenerated()) {
            // source_json_exact is the engine resolver's expansion boundary;
            // effective_json additionally contains codec canonical defaults.
            engine_generated.push_back(component.source_json_exact);
        }
    }
    std::vector<nlohmann::json> studio_generated;
    for (const auto &component : expansion.resolved_component_json) {
        studio_generated.push_back(nlohmann::json::parse(component));
    }
    REQUIRE(studio_generated == engine_generated);

    sandbox.write("prefabs/unit.json", R"json({
      "schema":"pelican.prefab","version":1,"name":"unit",
      "parameters":[{"name":"radius","type":"f32","default":0.5}],
      "components":[{"name":"collider","id":"hit","shape":"box",
                     "radius":{"$param":"radius"}}]
    })json");
    sandbox.write("scenes/main.scene.json", R"json({
      "schema":"pelican.scene","version":2,
      "scenes":{"main":{"objects":[
        {"name":"Instance","components":[{"name":"transform"}],
         "prefab":{"ref":"unit","instance_id":"gi_main",
                   "parameters":{"radius":0.75}}}
      ]}}
    })json");
    std::optional<Pelican::PrefabErrorCode> studio_error;
    try {
        (void)ProjectOutlinerModel::open(sandbox.root);
    } catch (const Pelican::PrefabError &error) {
        studio_error = error.code();
    }
    REQUIRE(studio_error == Pelican::PrefabErrorCode::PrefabBindingInactive);

    const auto inactive_registry = Pelican::buildPrefabRegistrySnapshot(
        project.envelope.prefabs, [&](std::string_view relative) {
            return readJson(sandbox.root / std::filesystem::path{relative})
                .dump();
        });
    const auto inactive_document = Pelican::AuthoringSceneDocument::load(
        readJson(sandbox.root / "scenes/main.scene.json").dump(),
        Pelican::SceneRevision{1});
    std::optional<Pelican::PrefabErrorCode> engine_error;
    try {
        (void)Pelican::ResolvedSceneResolver::resolve(
            inactive_document, Pelican::SceneResolverGeneration{2}, {},
            inactive_registry,
            Pelican::buildProductionBindableProviderSnapshot(2));
    } catch (const Pelican::PrefabError &error) {
        engine_error = error.code();
    }
    REQUIRE(engine_error == studio_error);
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
  "version": 2,
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
  "schema":"pelican.scene","version":2,
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

namespace {

// OL-1: a scene tree whose objects carry the authoring ids given, in the
// order given -- what the engine answers after a duplicate put a copy in the
// middle, or an undo took one out.
nlohmann::json idTree(std::uint64_t revision, const nlohmann::json &objects) {
    nlohmann::json result{{"scene_revision", revision},
                          {"scene_id", "main"},
                          {"objects", nlohmann::json::array()}};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        nlohmann::json object{{"authoring_object_id", objects.at(index).at("id")},
                              {"declaration_index", index},
                              {"components", nlohmann::json::array()}};
        if (objects.at(index).contains("name")) object["name"] = objects.at(index).at("name");
        if (objects.at(index).contains("parent"))
            object["parent"] = objects.at(index).at("parent");
        result["objects"].push_back(std::move(object));
    }
    return result;
}

struct OutlinerEditFixture {
    ProjectSandbox sandbox;
    ProjectOutlinerModel model;

    OutlinerEditFixture() : model{open()} {
        // Camera, Cube, Floor, and Floor's child Leg with Leg's child Toe.
        REQUIRE(model.updateSceneTree(idTree(1, nlohmann::json::parse(R"([
            {"id": 1, "name": "Camera"},
            {"id": 2, "name": "Cube"},
            {"id": 3, "name": "Floor"},
            {"id": 4, "name": "Leg", "parent": "Floor"},
            {"id": 5, "name": "Toe", "parent": "Leg"}
        ])"))));
    }

    ProjectOutlinerModel open() {
        sandbox.write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"ol1",
  "basic_config":{"scene_data_json":"scenes/main.scene.json"}
})json");
        sandbox.write("scenes/main.scene.json", R"json({
  "schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}
})json");
        return ProjectOutlinerModel::open(sandbox.root);
    }

    const PelicanStudio::OutlinerObject &object(std::string_view name) const {
        for (const auto &candidate : requireScene(model, "main").objects)
            if (candidate.name == name) return candidate;
        FAIL("no object " << name);
    }
};

} // namespace

TEST_CASE("OL-1 the selection keeps its object by authoring id when a copy lands before it",
          "[devstudio][outliner][selection][ol1][negative-contrast]") {
    OutlinerEditFixture fixture;
    SelectionModel selection;
    selection.bindProject(&fixture.model);
    REQUIRE(selection.selectFromOutliner(fixture.object("Floor").key).applied());
    REQUIRE(selection.selected()->declaration_index == 2);
    REQUIRE(selection.selected()->authoring_object_id == 3);

    // Duplicate Cube: Cube_copy1 is declared right after Cube, so Floor and
    // everything after it move down one.
    REQUIRE(fixture.model.updateSceneTree(idTree(2, nlohmann::json::parse(R"([
        {"id": 1, "name": "Camera"},
        {"id": 2, "name": "Cube"},
        {"id": 6, "name": "Cube_copy1"},
        {"id": 3, "name": "Floor"},
        {"id": 4, "name": "Leg", "parent": "Floor"},
        {"id": 5, "name": "Toe", "parent": "Leg"}
    ])"))));
    CHECK(selection.followProject().kind == SelectionUpdateKind::changed);
    REQUIRE(selection.selected());
    // Still Floor -- not Cube_copy1, which now stands where Floor stood.
    CHECK(selection.selected()->authoring_object_id == 3);
    CHECK(selection.selected()->declaration_index == 3);
    CHECK(fixture.model.findObject(*selection.selected())->name == "Floor");
    CHECK(selection.followProject().kind == SelectionUpdateKind::unchanged);

    // An index-only key (a pick) is answered with the whole key.
    REQUIRE(selection.selectFromOutliner(OutlinerObjectKey{.scene_id = "main",
                                                           .declaration_index = 2})
                .applied());
    CHECK(selection.selected()->authoring_object_id == 6);

    // Undo of the duplicate: the copy is gone, and a selection of it is
    // cleared rather than moved onto whatever is at its index now.
    REQUIRE(fixture.model.updateSceneTree(idTree(3, nlohmann::json::parse(R"([
        {"id": 1, "name": "Camera"},
        {"id": 2, "name": "Cube"},
        {"id": 3, "name": "Floor"},
        {"id": 4, "name": "Leg", "parent": "Floor"},
        {"id": 5, "name": "Toe", "parent": "Leg"}
    ])"))));
    CHECK(selection.followProject().kind == SelectionUpdateKind::changed);
    CHECK_FALSE(selection.selected());
}

TEST_CASE("OL-1 Outliner edits name their object by authoring id",
          "[devstudio][outliner][ol1]") {
    using PelicanStudio::ReparentPreserve;
    OutlinerEditFixture fixture;
    const auto &cube = fixture.object("Cube");
    const auto &floor = fixture.object("Floor");
    const auto &leg = fixture.object("Leg");

    CHECK(PelicanStudio::duplicateObjectOperation(cube) ==
          nlohmann::json{{"op", "duplicate"}, {"object_id", 2}});
    CHECK(PelicanStudio::destroyObjectOperation(floor) ==
          nlohmann::json{{"op", "destroy"}, {"object_id", 3}});
    CHECK(PelicanStudio::reparentObjectOperation(cube, &floor, ReparentPreserve::World) ==
          nlohmann::json{{"op", "reparent"}, {"object_id", 2}, {"new_parent_id", 3},
                         {"preserve", "world"}});
    CHECK(PelicanStudio::reparentObjectOperation(leg, nullptr, ReparentPreserve::Local) ==
          nlohmann::json{{"op", "reparent"}, {"object_id", 4}, {"new_parent_id", nullptr},
                         {"preserve", "local"}});

    // Delete asks only when it takes others with it.
    CHECK(fixture.model.descendantCount(cube) == 0);
    CHECK(fixture.model.descendantCount(floor) == 2);
    CHECK(fixture.model.descendantCount(leg) == 1);

    // After a delete: the parent, else the sibling declared before, else none.
    CHECK(PelicanStudio::selectionAfterDestroy(fixture.model, fixture.object("Toe")) ==
          leg.key);
    CHECK(PelicanStudio::selectionAfterDestroy(fixture.model, floor) == cube.key);
    CHECK_FALSE(PelicanStudio::selectionAfterDestroy(fixture.model, fixture.object("Camera")));
}

TEST_CASE("OL-1 a drop goes under a row, to the root, or keeps its own transform with Alt",
          "[devstudio][outliner][ol1][drop]") {
    using PelicanStudio::OutlinerDropKind;
    using PelicanStudio::ReparentPreserve;
    using PelicanStudio::planOutlinerDrop;
    OutlinerEditFixture fixture;
    const auto key = [&](std::string_view name) { return fixture.object(name).key; };

    // Onto a row: under it, where it is on screen.
    auto plan = planOutlinerDrop(fixture.model, key("Cube"), key("Floor"), ReparentPreserve::World);
    REQUIRE(plan.kind == OutlinerDropKind::Reparent);
    CHECK(plan.operation == nlohmann::json{{"op", "reparent"}, {"object_id", 2},
                                           {"new_parent_id", 3}, {"preserve", "world"}});
    // Between rows: to the root.
    plan = planOutlinerDrop(fixture.model, key("Leg"), std::nullopt, ReparentPreserve::World);
    REQUIRE(plan.kind == OutlinerDropKind::Reparent);
    CHECK(plan.operation.at("new_parent_id").is_null());
    CHECK(plan.operation.at("preserve") == "world");
    // Alt: its own transform, moving with the new parent.
    plan = planOutlinerDrop(fixture.model, key("Cube"), key("Toe"), ReparentPreserve::Local);
    REQUIRE(plan.kind == OutlinerDropKind::Reparent);
    CHECK(plan.operation.at("new_parent_id") == 5);
    CHECK(plan.operation.at("preserve") == "local");

    // Under its own child or grandchild: refused before the engine would say
    // cycle_detected, by name.
    plan = planOutlinerDrop(fixture.model, key("Floor"), key("Toe"), ReparentPreserve::World);
    CHECK(plan.kind == OutlinerDropKind::Refused);
    CHECK(plan.refusal == "Floor cannot go under Toe: Toe is inside Floor.");
    CHECK(plan.operation.is_null());

    // Where it already is: nothing to send.
    CHECK(planOutlinerDrop(fixture.model, key("Leg"), key("Floor"), ReparentPreserve::World).kind ==
          OutlinerDropKind::Unchanged);
    CHECK(planOutlinerDrop(fixture.model, key("Cube"), std::nullopt, ReparentPreserve::World).kind ==
          OutlinerDropKind::Unchanged);
    CHECK(planOutlinerDrop(fixture.model, key("Cube"), key("Cube"), ReparentPreserve::World).kind ==
          OutlinerDropKind::Unchanged);
}

TEST_CASE("OL-1 the object to select is the one the committed edit created",
          "[devstudio][outliner][ol1]") {
    using PelicanStudio::createdObjectToSelect;
    // A duplicate of Floor copies Floor, Leg and Toe; the one to select is
    // the copy of Floor, whatever order they are listed in.
    const auto created = nlohmann::json::parse(R"([
        {"object_id": 7, "scene_id": "main", "declaration_index": 6, "name": "Leg_copy1",
         "source_object_id": 4},
        {"object_id": 6, "scene_id": "main", "declaration_index": 5, "name": "Floor_copy1",
         "source_object_id": 3},
        {"object_id": 8, "scene_id": "main", "declaration_index": 7, "name": "Toe_copy1",
         "source_object_id": 5}
    ])");
    const auto copy = createdObjectToSelect(created, 3);
    REQUIRE(copy);
    CHECK(*copy == OutlinerObjectKey{.scene_id = "main", .declaration_index = 5,
                                     .authoring_object_id = 6});
    // A spawn, or an undone delete: the first one named.
    CHECK(createdObjectToSelect(created)->authoring_object_id == 7);
    CHECK_FALSE(createdObjectToSelect(nlohmann::json::array()));
    CHECK_FALSE(createdObjectToSelect(created, 99));
}

// MG-1 (GUI). Studio's offline open reads the project's documents before the
// engine starts. A document an earlier engine wrote is named as that -- the
// player's own refusal -- instead of being read as if it were current.
TEST_CASE("MG-1 the offline open does not read a document that needs migrating",
          "[devstudio][outliner][mg1][migration][offline]") {
    ProjectSandbox sandbox;
    sandbox.write("project.json", R"json({
      "schema":"pelican.project","version":1,"name":"migration-offline",
      "basic_config":{"scene_data_json":"scenes/main.scene.json"}
    })json");
    sandbox.write("scenes/main.scene.json", R"json({
      "schema":"pelican.scene","version":2,
      "scenes":{"main":{"objects":[{"name":"Lamp","components":[{"name":"transform"}]}]}}
    })json");

    // A later engine: pelican.scene moved from version 2 to 3.
    Pelican::DocumentMigrationRegistry later;
    later.registerStep(Pelican::DocumentMigrationStep{
        .id = "test.scene_2_to_3",
        .schema = "pelican.scene",
        .from_version = 2,
        .to_version = 3,
        .description = "test step",
        .plan = [](const nlohmann::ordered_json &document,
                   const Pelican::ProjectContext &) {
            return Pelican::StepResult{document, {}};
        },
    });

    std::optional<std::string> refusal;
    std::filesystem::path refused_root;
    try {
        (void)ProjectOutlinerModel::open(sandbox.root, later);
    } catch (const PelicanStudio::ProjectMigrationRequiredError &error) {
        refusal = error.what();
        refused_root = error.projectRoot();
    }
    REQUIRE(refusal.has_value());
    REQUIRE(refused_root == std::filesystem::canonical(sandbox.root));
    // The same text the player writes when it stops at the gate, so the one
    // screen reads both.
    const std::string_view prefix = "project.migration_required@1: ";
    REQUIRE(refusal->starts_with(prefix));
    const auto fields = nlohmann::json::parse(refusal->substr(prefix.size()));
    REQUIRE(fields.at("documents") ==
            nlohmann::json::parse(R"json([{"path":"scenes/main.scene.json",
              "schema":"pelican.scene","from":2,"to":3}])json"));

    // The control: the engine that shipped (no steps) opens the same project.
    const auto current = ProjectOutlinerModel::open(sandbox.root);
    REQUIRE(current.projectName() == std::optional<std::string>{"migration-offline"});
    REQUIRE(ProjectOutlinerModel::open(sandbox.root,
                                       Pelican::DocumentMigrationRegistry{})
                .projectName()
                .has_value());
}

namespace {

// UX-13c: what the engine's scene_outline answers -- ids, declaration order,
// names (left out for an unnamed object) and parents by authoring id.
nlohmann::json sceneOutline(std::uint64_t revision, const nlohmann::json &objects) {
    nlohmann::json result{{"scene_id", "main"},
                          {"scene_revision", revision},
                          {"not_modified", false},
                          {"objects", nlohmann::json::array()}};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto &source = objects.at(index);
        nlohmann::json object{{"authoring_object_id", source.at("id")},
                              {"declaration_index", index},
                              {"parent_authoring_object_id", source.value("parent", nlohmann::json{})}};
        if (source.contains("name")) object["name"] = source.at("name");
        result["objects"].push_back(std::move(object));
    }
    return result;
}

nlohmann::json notModified(std::uint64_t revision) {
    return {{"scene_id", "main"}, {"scene_revision", revision}, {"not_modified", true}};
}

} // namespace

TEST_CASE("UX-13c the Outliner hangs an object under its parent's authoring id",
          "[devstudio][outliner][ux13c]") {
    OutlinerEditFixture fixture;
    // Leg is declared before its parent Floor, an unnamed object hangs under
    // Cube, and Toe under Leg. The reply names no parent: only ids.
    REQUIRE(fixture.model.updateSceneOutline(sceneOutline(2, nlohmann::json::parse(R"([
        {"id": 4, "name": "Leg", "parent": 3},
        {"id": 2, "name": "Cube"},
        {"id": 9, "parent": 2},
        {"id": 5, "name": "Toe", "parent": 4},
        {"id": 3, "name": "Floor"}
    ])"))));
    const auto &scene = requireScene(fixture.model, "main");
    CHECK(scene.root_declaration_indices == std::vector<std::size_t>{1, 4});
    CHECK(scene.objects[0].parent_declaration_index == std::optional<std::size_t>{4});
    CHECK(scene.objects[2].parent_declaration_index == std::optional<std::size_t>{1});
    CHECK(scene.objects[3].parent_declaration_index == std::optional<std::size_t>{0});
    CHECK(scene.objects[4].child_declaration_indices == std::vector<std::size_t>{0});
    CHECK(scene.objects[1].child_declaration_indices == std::vector<std::size_t>{2});
    // The unnamed object keeps the name made for it and has no authored name.
    CHECK(scene.objects[2].name.empty());
    CHECK_FALSE(scene.objects[2].display_name.empty());

    // A parent id the list does not hold, an id of 0 and a missing parent
    // field are refused, and the scene held is kept.
    CHECK_THROWS(fixture.model.updateSceneOutline(sceneOutline(3, nlohmann::json::parse(R"([
        {"id": 2, "name": "Cube", "parent": 77}
    ])"))));
    CHECK_THROWS(fixture.model.updateSceneOutline(sceneOutline(3, nlohmann::json::parse(R"([
        {"id": 0, "name": "Cube"}
    ])"))));
    auto missing_parent = sceneOutline(3, nlohmann::json::parse(R"([{"id": 2}])"));
    missing_parent["objects"][0].erase("parent_authoring_object_id");
    CHECK_THROWS(fixture.model.updateSceneOutline(missing_parent));
    CHECK(fixture.model.sceneRevision() == std::optional<std::uint64_t>{2});
    CHECK(requireScene(fixture.model, "main").objects.size() == 5);
}

TEST_CASE("UX-13c a not_modified poll rebuilds nothing and a new revision rebuilds",
          "[devstudio][outliner][ux13c]") {
    OutlinerEditFixture fixture;
    // A thousand objects: a root every tenth, the nine after it its children.
    nlohmann::json objects = nlohmann::json::array();
    for (std::uint64_t index = 0; index < 1000; ++index) {
        nlohmann::json object{{"id", 100 + index}, {"name", "Object" + std::to_string(index)}};
        if (index % 10 != 0) object["parent"] = 100 + index - index % 10;
        objects.push_back(std::move(object));
    }
    const auto full = sceneOutline(7, objects);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(fixture.model.updateSceneOutline(full));
    const auto projected = std::chrono::steady_clock::now() - started;
    const auto *held = requireScene(fixture.model, "main").objects.data();
    REQUIRE(requireScene(fixture.model, "main").root_declaration_indices.size() == 100);

    // The poll's answer for the revision held: nothing is read or rebuilt.
    const auto unchanged = notModified(7);
    CHECK_FALSE(fixture.model.updateSceneOutline(unchanged));
    CHECK(requireScene(fixture.model, "main").objects.data() == held);
    // A not_modified answer for a revision Studio does not hold is refused.
    CHECK_THROWS(fixture.model.updateSceneOutline(notModified(6)));

    // A new revision is the whole list again, and it is rebuilt.
    auto moved = objects;
    moved[1].erase("parent");
    REQUIRE(fixture.model.updateSceneOutline(sceneOutline(8, moved)));
    CHECK(requireScene(fixture.model, "main").root_declaration_indices.size() == 101);

    // Decision 6: the numbers for the report, not a pass condition.
    std::cout << "UX-13c 1000 objects: scene_outline " << full.dump().size()
              << " bytes, not_modified " << unchanged.dump().size()
              << " bytes, projected in "
              << std::chrono::duration<double, std::milli>(projected).count() << " ms\n";
    CHECK(unchanged.dump().size() < 100);
}
