#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/imgui/planviewer.hpp"

#include <nlohmann/json.hpp>

using namespace Pelican;

TEST_CASE("plan viewer model is an exact deterministic projection of frame plan v1",
          "[imgui][plan-viewer]") {
    const nlohmann::json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "main"},
        {"levels", {{"opaque"}, {"__snapshot_opaque_color"}, {"refract"}}},
        {"nodes",
         nlohmann::json::array({
             {{"name", "refract"}, {"kind", "render"}, {"order", 2}, {"level", 2},
              {"reads", {"opaque_color"}}, {"writes", {"display"}}},
             {{"name", "opaque"}, {"kind", "render"}, {"order", 0}, {"level", 0},
              {"reads", nlohmann::json::array()}, {"writes", {"display"}}},
             {{"name", "__snapshot_opaque_color"}, {"kind", "snapshot_copy"},
              {"order", 1}, {"level", 1}, {"reads", {"display"}},
              {"writes", {"opaque_color"}}, {"snapshot_after", "post_ldr"},
              {"byte_size", 8294400}},
         })},
        {"barriers",
         nlohmann::json::array({
             {{"kind", "raw"}, {"resource", "display"}, {"from", "opaque"},
              {"to", "__snapshot_opaque_color"}},
             {{"kind", "raw"}, {"resource", "opaque_color"},
              {"from", "__snapshot_opaque_color"}, {"to", "refract"}},
         })},
    };
    const nlohmann::json annotations{
        {"nodes",
         {{"opaque", {{"source", "project"}, {"anchor", "post_ldr"},
                       {"color_load_op", "clear"}, {"color_store_op", "store"}}},
          {"refract", {{"source", "feature"}, {"feature", "refraction"},
                        {"anchor", "post_ldr"}, {"color_load_op", "load"},
                        {"color_store_op", "store"}}}}},
        {"resources",
         {{"display", {{"format", "B8G8R8A8_SRGB"}, {"format_class", "display"}}},
          {"opaque_color", {{"format", "B8G8R8A8_SRGB"},
                             {"format_class", "display"}}}}},
    };
    LoweredMaterial material;
    material.name = "glass";
    material.surface = "project://shaders/refract.surface";
    material.screen_inputs = {"opaque_color"};
    material.target_pass = "refract";
    material.render_state.blend = SurfaceBlendMode::blend;
    material.render_state.depth_write = false;

    const auto model = buildPlanViewerModel(plan, annotations, {&material, 1});
    REQUIRE(model.graph == "main");
    REQUIRE(model.nodes.size() == 3);
    REQUIRE(model.nodes[0].name == "opaque");
    REQUIRE(model.nodes[1].name == "__snapshot_opaque_color");
    REQUIRE(model.nodes[1].kind == "snapshot_copy");
    REQUIRE(model.nodes[1].snapshot_after == "post_ldr");
    REQUIRE(model.nodes[1].byte_size == 8294400);
    REQUIRE(model.nodes[2].feature == "refraction");
    REQUIRE(model.nodes[2].color_load_op == "load");
    REQUIRE(model.edges.size() == 2);
    REQUIRE(model.resources.size() == 2);
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
    REQUIRE_THROWS_WITH(buildPlanViewerModel({{"schema", "private.renderer_graph"}}, {}),
                        "plan viewer requires pelican.frame_plan version 1");
}
