#include "../src/core/loader/editorpreviewprojection.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json previewFixture() {
    return Json::parse(R"json({
      "schema":"pelican.scene",
      "version":1,
      "scenes":{
        "main":{
          "objects":[
            {"name":"Root","components":[
              {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"light","type":"directional","direction":[0,-1,0],"intensity":1,"color":[1,1,1]}
            ]},
            {"name":"Middle","parent":"Root","components":[
              {"name":"transform","pos":[1,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}
            ]},
            {"name":"Leaf","parent":"Middle","components":[
              {"name":"transform","pos":[0,2,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"collider","shape":"sphere","radius":0.5}
            ]},
            {"name":"Camera","components":[
              {"name":"transform","pos":[0,0,-5],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"camera","type":"perspective","yfov":0.8,"znear":0.1,"zfar":100}
            ]}
          ]
        }
      }
    })json");
}

struct SharedStateFixture {
    std::string authored_semantic_bytes;
    std::uint64_t scene_revision = 0;
    std::vector<std::string> journal{"stable"};
    std::vector<int> ecs_values{1, 2, 3};
    std::vector<std::uint64_t> ecs_versions{7, 8, 9};
    std::uint64_t global_tick = 11;
    std::vector<std::uint32_t> entity_free_list{4, 2};
    std::uint64_t light_binding = 13;
    std::uint64_t phys_binding = 17;
    std::uint64_t phys_identity_counter = 19;
    std::uint64_t renderer_handle = 23;
    std::uint64_t renderer_history_epoch = 29;
    std::vector<std::string> behavior_event_trace{"unchanged"};
    double engine_time = 2.5;
    std::uint64_t reload_generation = 31;

    bool operator==(const SharedStateFixture &) const = default;
};

} // namespace

TEST_CASE("WP172 PreparedProjection evaluates overrides and explicit staged queries",
          "[wp172][editor][eval-preview]") {
    const auto base = AuthoringSceneDocument::load(
        previewFixture().dump(), SceneRevision{41});
    SharedStateFixture live{
        .authored_semantic_bytes =
            test_support::authoring(base).encodeSemantic(),
        .scene_revision = base.revision().value,
    };
    const auto before = live;

    const Json overrides = Json::array({
        {{"op", "set_component_value"}, {"object_id", 1},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {2, 0, 0}}},
        {{"op", "set_component_value"}, {"object_id", 1},
         {"component_slot", "light"}, {"field_path", "/intensity"},
         {"value", 4.0}},
        {{"op", "set_component_value"}, {"object_id", 3},
         {"component_slot", "collider"}, {"field_path", "/radius"},
         {"value", 0.75}},
    });
    const auto prepared = prepareEditorPreviewProjection(base, overrides);
    const EditorPreviewEvaluationContext context{prepared};
    const auto results = context.evaluate(Json::array({
        {{"kind", "component"}, {"object_id", 1},
         {"component_slot", "light"}},
        {{"kind", "descendant_world"}, {"object_id", 1}},
        {{"kind", "raycast"},
         {"ray", {{"origin", {3, 2, -5}}, {"direction", {0, 0, 1}},
                  {"max_distance", 10}}}},
        {{"kind", "overlap"},
         {"shape", {{"type", "sphere"}, {"center", {3, 2, 0}},
                    {"radius", 0.2}}}},
        {{"kind", "camera"}, {"object_id", 4},
         {"width", 320}, {"height", 180}},
    }));

    REQUIRE(results.size() == 5);
    REQUIRE(results.at(0).at("data").at("intensity") == 4.0f);
    REQUIRE(results.at(0).at("data").at("linear_color") ==
            Json::array({1.0f, 1.0f, 1.0f}));

    const auto &closure = results.at(1).at("data");
    REQUIRE(closure.size() == 3);
    REQUIRE(closure.at(0).at("world_trs").at("pos") ==
            Json::array({2.0f, 0.0f, 0.0f}));
    REQUIRE(closure.at(1).at("world_trs").at("pos") ==
            Json::array({3.0f, 0.0f, 0.0f}));
    REQUIRE(closure.at(2).at("world_trs").at("pos") ==
            Json::array({3.0f, 2.0f, 0.0f}));

    REQUIRE(results.at(2).at("data").size() == 1);
    REQUIRE(results.at(2).at("data").at(0).at("name") == "Leaf");
    REQUIRE_THAT(results.at(2).at("data").at(0).at("distance").get<float>(),
                 Catch::Matchers::WithinAbs(4.25f, 1.0e-4f));
    REQUIRE(results.at(3).at("data").size() == 1);
    REQUIRE(results.at(3).at("data").at(0).at("name") == "Leaf");
    REQUIRE(results.at(4).at("data").at("position") ==
            Json::array({0.0f, 0.0f, -5.0f}));

    REQUIRE(base.revision().value == 41);
    REQUIRE(test_support::authoring(base).encodeSemantic() ==
            before.authored_semantic_bytes);
    REQUIRE(live == before);
}

TEST_CASE("WP172 eval_preview prepare and query faults discard request-local state",
          "[wp172][editor][eval-preview][fault]") {
    const auto base = AuthoringSceneDocument::load(
        previewFixture().dump(), SceneRevision{9});
    SharedStateFixture live{
        .authored_semantic_bytes =
            test_support::authoring(base).encodeSemantic(),
        .scene_revision = base.revision().value,
    };
    const auto before = live;
    const Json overrides = Json::array({
        {{"op", "set_component_value"}, {"object_id", 1},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {9, 0, 0}}},
    });

    REQUIRE_THROWS_AS(prepareEditorPreviewProjection(
                          base, overrides,
                          [](std::string_view phase, std::size_t) {
                              if (phase == "prepare") {
                                  throw std::runtime_error{"injected prepare fault"};
                              }
                          }),
                      std::runtime_error);
    REQUIRE(live == before);
    REQUIRE(test_support::authoring(base).encodeSemantic() ==
            before.authored_semantic_bytes);

    const auto prepared = prepareEditorPreviewProjection(base, overrides);
    const EditorPreviewEvaluationContext context{prepared};
    REQUIRE_THROWS_AS(context.evaluate(
                          Json::array({{{"kind", "component"}, {"object_id", 1},
                                        {"component_slot", "transform"}}}),
                          [](std::string_view phase, std::size_t) {
                              if (phase == "query") {
                                  throw std::runtime_error{"injected query fault"};
                              }
                          }),
                      std::runtime_error);
    REQUIRE(live == before);
    REQUIRE(test_support::authoring(base).encodeSemantic() ==
            before.authored_semantic_bytes);
}

TEST_CASE("WP172 descendant_world remains scoped to the root scene",
          "[wp172][editor][eval-preview][multi-scene]") {
    auto fixture = previewFixture();
    fixture["scenes"]["other"] = {
        {"objects",
         Json::array({
             {{"name", "Root"},
              {"components",
               Json::array({{{"name", "transform"},
                             {"pos", {100, 0, 0}},
                             {"rotation", {0, 0, 0, 1}},
                             {"scale", {1, 1, 1}}}})}},
             {{"name", "OtherLeaf"},
              {"parent", "Root"},
              {"components",
               Json::array({{{"name", "transform"},
                             {"pos", {1, 0, 0}},
                             {"rotation", {0, 0, 0, 1}},
                             {"scale", {1, 1, 1}}}})}}})}};
    const auto base = AuthoringSceneDocument::load(
        fixture.dump(), SceneRevision{12});
    const auto prepared =
        prepareEditorPreviewProjection(base, Json::array());
    const auto result = EditorPreviewEvaluationContext{prepared}.evaluate(
        Json::array({{{"kind", "descendant_world"}, {"object_id", 1}}}));

    const auto &closure = result.at(0).at("data");
    REQUIRE(closure.size() == 3);
    REQUIRE(closure.at(0).at("object_id") == 1);
    REQUIRE(closure.at(1).at("object_id") == 2);
    REQUIRE(closure.at(2).at("object_id") == 3);
}

TEST_CASE("WP172 eval_preview rejects unsupported prepared fields without publication",
          "[wp172][editor][eval-preview][negative]") {
    auto fixture = previewFixture();
    fixture.at("scenes").at("main").at("objects").at(0).at("components")
        .push_back({{"name", "custom_unprepared"}, {"value", 1}});
    const auto base = AuthoringSceneDocument::load(fixture.dump(), SceneRevision{1});
    const auto bytes = test_support::authoring(base).encodeSemantic();

    try {
        (void)prepareEditorPreviewProjection(
            base, Json::array({
                      {{"op", "set_component_value"}, {"object_id", 1},
                       {"component_slot", "custom_unprepared"},
                       {"field_path", "/value"}, {"value", 2}},
                  }));
        FAIL("method_unavailable was not raised");
    } catch (const EditorPreviewProjectionError &error) {
        REQUIRE(error.code() ==
                EditorPreviewProjectionErrorCode::method_unavailable);
        REQUIRE(error.adapter() == "custom_unprepared");
    }
    REQUIRE(test_support::authoring(base).encodeSemantic() == bytes);
    REQUIRE(base.revision().value == 1);

    const auto prepared = prepareEditorPreviewProjection(base, Json::array());
    try {
        (void)EditorPreviewEvaluationContext{prepared}.evaluate(Json::array({
            {{"kind", "raycast"},
             {"ray", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}},
                      {"max_distance", 1.0}}}},
        }));
        FAIL("degenerate ray was accepted");
    } catch (const EditorPreviewProjectionError &error) {
        REQUIRE(error.code() ==
                EditorPreviewProjectionErrorCode::schema_violation);
    }
    REQUIRE(test_support::authoring(base).encodeSemantic() == bytes);
}

} // namespace Pelican
