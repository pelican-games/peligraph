#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/imgui/planviewer.hpp"

#include <nlohmann/json.hpp>

using namespace Pelican;

TEST_CASE("plan viewer model is an exact deterministic projection of frame plan v1",
          "[imgui][plan-viewer]") {
    nlohmann::json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"profile", "runtime"},
        {"graph", "main"},
        {"levels", {{"opaque"}, {"__snapshot_opaque_color"}, {"refract"}}},
        {"nodes",
         nlohmann::json::array({
             {{"name", "refract"}, {"kind", "render"}, {"order", 2}, {"level", 2},
              {"reads", {"opaque_color"}}, {"writes", {"display"}},
              {"shader_resolution", {{"state", "material_owned"}}}},
             {{"name", "opaque"}, {"kind", "render"}, {"order", 0}, {"level", 0},
              {"reads", nlohmann::json::array()}, {"writes", {"display"}},
              {"source", "project"},
              {"shader_resolution", {{"state", "material_owned"}}}},
             {{"name", "__snapshot_opaque_color"}, {"kind", "snapshot_copy"},
              {"order", 1}, {"level", 1}, {"reads", {"display"}},
              {"writes", {"opaque_color"}}, {"snapshot_after", "post_ldr"},
              {"byte_size", 8294400}, {"source", "engine"},
              {"shader_resolution", {{"state", "not_applicable"}}}},
         })},
        {"barriers",
         nlohmann::json::array({
             {{"kind", "raw"}, {"resource", "display"}, {"from", "opaque"},
              {"to", "__snapshot_opaque_color"}},
             {{"kind", "raw"}, {"resource", "opaque_color"},
              {"from", "__snapshot_opaque_color"}, {"to", "refract"}},
         })},
        {"resources",
         nlohmann::json::array({
             {{"name", "display"}, {"kind", "render_target"},
              {"source", "project"}},
             {{"name", "opaque_color"}, {"kind", "render_target"},
              {"source", "feature:refraction"},
             {"provider_feature", "refraction"},
              {"provider_ref", "engine://features/refraction.json"}},
         })},
        {"physical_target_plan",
         {{"attachments",
           nlohmann::json::array({
               {{"node", "opaque"}, {"logical_resource", "display"},
                {"aspect", "color"}, {"load_op", "clear"},
                {"store_op", "store"}},
               {{"node", "refract"}, {"logical_resource", "display"},
                {"aspect", "color"}, {"load_op", "load"},
                {"store_op", "store"}},
           })},
          {"resources",
           nlohmann::json::array({
               {{"logical_resource", "display"},
                {"format", "B8G8R8A8_SRGB"}},
               {{"logical_resource", "opaque_color"},
                {"format", "B8G8R8A8_SRGB"}},
           })}}},
    };
    plan["nodes"][0]["source"] = "feature:refraction";
    plan["nodes"][0]["provider_feature"] = "refraction";
    plan["nodes"][0]["provider_ref"] =
        "engine://features/refraction.json";
    LoweredMaterial material;
    material.name = "glass";
    material.surface = "project://shaders/refract.surface";
    material.screen_inputs = {"opaque_color"};
    material.target_pass = "refract";
    material.render_state.blend = SurfaceBlendMode::blend;
    material.render_state.depth_write = false;

    const auto model = buildPlanViewerModel(plan, {&material, 1});
    REQUIRE(model.graph == "main");
    REQUIRE(model.nodes.size() == 3);
    REQUIRE(model.nodes[0].name == "opaque");
    REQUIRE(model.nodes[0].color_load_op == "clear");
    REQUIRE(model.nodes[0].color_store_op == "store");
    REQUIRE(model.nodes[1].name == "__snapshot_opaque_color");
    REQUIRE(model.nodes[1].kind == "snapshot_copy");
    REQUIRE(model.nodes[1].snapshot_after == "post_ldr");
    REQUIRE(model.nodes[1].byte_size == 8294400);
    REQUIRE(model.nodes[2].feature == "refraction");
    REQUIRE(model.nodes[2].source == "feature:refraction");
    REQUIRE(model.nodes[2].color_load_op == "load");
    REQUIRE(model.edges.size() == 2);
    REQUIRE(model.resources.size() == 2);
    REQUIRE(model.resources[0].format == "B8G8R8A8_SRGB");
    REQUIRE(model.resources[1].name == "opaque_color");
    REQUIRE(model.resources[1].writers == std::vector<std::string>{"__snapshot_opaque_color"});
    REQUIRE(model.resources[1].readers == std::vector<std::string>{"refract"});
    REQUIRE(model.materials.size() == 1);
    REQUIRE(model.materials[0].surface_stem == "refract");
    REQUIRE(model.materials[0].screen_inputs == std::vector<std::string>{"opaque_color"});
    REQUIRE(model.materials[0].target_pass == "refract");
    REQUIRE(model.materials[0].render_state.find("blend=blend") != std::string::npos);
}

TEST_CASE("plan viewer rejects data outside the public frame plan contract",
          "[imgui][plan-viewer]") {
    REQUIRE_THROWS_WITH(buildPlanViewerModel({{"schema", "private.renderer_graph"}}),
                        "plan viewer requires pelican.frame_plan version 1");

    nlohmann::json runtime{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"profile", "runtime"},
        {"graph", "suffix_contract"},
        {"levels", {{"pass"}}},
        {"nodes",
         nlohmann::json::array({
             {{"name", "pass"}, {"kind", "render"}, {"order", 0},
              {"level", 0}, {"reads", nlohmann::json::array()},
              {"writes", nlohmann::json::array()},
              {"shader_resolution",
               {{"state", "resolved"},
                {"stages",
                 nlohmann::json::array({
                     {{"stage", "vertex"},
                      {"declared_ref", "shaders/pass"},
                      {"effective_ref", "shaders/pass"},
                      {"origin", "authored"},
                      {"source_open_ref",
                       "project://shaders/pass.frag"}},
                 })}}}},
         })},
        {"barriers", nlohmann::json::array()},
        {"resources", nlohmann::json::array()},
    };
    REQUIRE_THROWS_WITH(
        buildPlanViewerModel(runtime),
        Catch::Matchers::ContainsSubstring("exact stage source suffix"));

    runtime.erase("profile");
    REQUIRE_THROWS_WITH(
        buildPlanViewerModel(runtime),
        Catch::Matchers::ContainsSubstring(
            "requires frame plan profile 'runtime'"));
}

TEST_CASE("WP367 PlanViewer strictly consumes sparse anyhit slots",
          "[WP367][imgui][plan-viewer][rt2a][shader-resolution][mutation]") {
    nlohmann::json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"profile", "runtime"},
        {"graph", "wp367"},
        {"levels", {{"trace"}}},
        {"nodes", nlohmann::json::array({
             {{"name", "trace"}, {"kind", "compute"}, {"order", 0},
              {"level", 0}, {"reads", nlohmann::json::array()},
              {"writes", nlohmann::json::array()},
              {"shader_resolution",
               {{"state", "resolved"},
                {"stages", nlohmann::json::array({
                     {{"stage", "raygen"},
                      {"declared_ref", "shaders/trace"},
                      {"effective_ref", "shaders/trace"},
                      {"origin", "authored"},
                      {"source_open_ref", "project://shaders/trace.rgen"}},
                 })},
                {"anyhit_slots", nlohmann::json::array({
                     {{"hit_slot", std::size_t{0}}, {"stage", "anyhit"},
                      {"declared_ref", "shaders/filter0"},
                      {"effective_ref", "shaders/filter0"},
                      {"origin", "authored"},
                      {"source_open_ref", "project://shaders/filter0.rahit"}},
                     {{"hit_slot", std::size_t{3}}, {"stage", "anyhit"},
                      {"declared_ref", "shaders/filter3"},
                      {"effective_ref", "shaders/filter3"},
                      {"origin", "authored"},
                      {"source_open_ref", "project://shaders/filter3.rahit"}},
                 })}}}},
         })},
        {"barriers", nlohmann::json::array()},
        {"resources", nlohmann::json::array()},
    };

    const auto model = buildPlanViewerModel(plan);
    REQUIRE(model.nodes.size() == 1);
    REQUIRE(model.nodes.front().shader_resolution.anyhit_slots.size() == 2);
    CHECK(model.nodes.front().shader_resolution.anyhit_slots[0].hit_slot == 0);
    CHECK(model.nodes.front().shader_resolution.anyhit_slots[1].hit_slot == 3);
    CHECK(model.nodes.front()
              .shader_resolution.anyhit_slots[1]
              .projection.source_open_ref ==
          std::optional<std::string>{
              "project://shaders/filter3.rahit"});

    auto &slots =
        plan["nodes"][0]["shader_resolution"]["anyhit_slots"];
    SECTION("unknown projection field is rejected") {
        slots[0]["subset_certified"] = true;
        CHECK_THROWS_WITH(
            buildPlanViewerModel(plan),
            Catch::Matchers::ContainsSubstring("unknown field"));
    }
    SECTION("slot order is strictly increasing") {
        slots[1]["hit_slot"] = std::size_t{0};
        CHECK_THROWS_WITH(
            buildPlanViewerModel(plan),
            Catch::Matchers::ContainsSubstring("strictly increasing"));
    }
    SECTION("slot stage is exactly anyhit") {
        slots[0]["stage"] = "closesthit";
        CHECK_THROWS_WITH(
            buildPlanViewerModel(plan),
            Catch::Matchers::ContainsSubstring("missing a required field"));
    }
    SECTION("slot source suffix is exactly rahit") {
        slots[0]["source_open_ref"] =
            "project://shaders/filter0.rchit";
        CHECK_THROWS_WITH(
            buildPlanViewerModel(plan),
            Catch::Matchers::ContainsSubstring("must end in .rahit"));
    }
    SECTION("empty sparse field must instead be omitted") {
        slots = nlohmann::json::array();
        CHECK_THROWS_WITH(
            buildPlanViewerModel(plan),
            Catch::Matchers::ContainsSubstring("non-empty array"));
    }
}
