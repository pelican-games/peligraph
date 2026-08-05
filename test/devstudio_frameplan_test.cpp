#include "frameplanmodel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace PelicanStudio;
using Catch::Matchers::ContainsSubstring;

namespace {

nlohmann::json responseFixture() {
    using Json = nlohmann::json;
    return Json{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "main_render"},
        {"runtime_generation", 42},
        {"nodes",
         Json::array({
             {{"name", "present"},
              {"kind", "render"},
              {"declaration_index", 2},
              {"order", 2},
              {"level", 2},
              {"reads", {"scene_color"}},
              {"reads_history", Json::array()},
              {"writes", {"display"}},
              {"color_load_op", "load"}},
             {{"name", "cull"},
              {"kind", "compute"},
              {"declaration_index", 0},
              {"order", 0},
              {"level", 0},
              {"reads", Json::array()},
              {"reads_history", Json::array()},
              {"writes", {"indirect_args"}}},
             {{"name", "opaque"},
              {"kind", "render"},
              {"declaration_index", 1},
              {"order", 1},
              {"level", 1},
              {"reads", {"indirect_args"}},
              {"reads_history", {"history_color"}},
              {"writes", {"scene_color", "scene_depth"}},
              {"material_variant", "opaque"},
              {"material_filter",
               {{"include", {"opaque"}},
                {"exclude", {"transparent"}},
                {"filter_id", "filter-1"},
                {"resolved_draw_count", 12},
                {"resolution_state", "resolved"}}}},
         })},
        {"barriers",
         Json::array({
             {{"kind", "read_after_write"},
              {"resource", "indirect_args"},
              {"from", "cull"},
              {"to", "opaque"}},
             {{"kind", "read_after_write"},
              {"resource", "scene_color"},
              {"from", "opaque"},
              {"to", "present"}},
         })},
        {"execution_plan",
         {{"schema", "pelican.frame_execution_plan"},
          {"schema_version", 1},
          {"nodes",
           Json::array({
               {{"name", "cull"},
                {"semantic_dialect", "pelican.logical.compute@1"},
                {"selected_implementation", "pelican.execution.frame_compute@1"},
                {"selected_endpoint", "gpu"},
                {"required_capabilities", {"pelican.execution.compute@1"}},
                {"resource_uses",
                 Json::array({
                     {{"resource", "indirect_args"},
                      {"epoch", "current"},
                      {"access", "write"},
                      {"intent", "storage"},
                      {"footprint", {{"kind", "none"}}}},
                 })}},
               {{"name", "opaque"},
                {"semantic_dialect", "pelican.logical.render@1"},
                {"selected_implementation", "pelican.execution.frame_render@1"},
                {"selected_endpoint", "gpu"},
                {"required_capabilities", {"pelican.execution.graphics@1"}},
                {"resource_uses", Json::array()}},
               {{"name", "present"},
                {"semantic_dialect", "pelican.logical.render@1"},
                {"selected_implementation", "pelican.execution.frame_render@1"},
                {"selected_endpoint", "gpu"},
                {"required_capabilities", {"pelican.execution.graphics@1"}},
                {"resource_uses", Json::array()}},
           })}}},
        {"physical_target_plan",
         {{"schema", "pelican.vulkan_target_plan"},
          {"version", 1},
          {"attachments",
           Json::array({
               {{"node", "opaque"},
                {"logical_resource", "scene_color"},
                {"aspect", "color"},
                {"load_op", "clear"},
                {"store_op", "store"}},
               {{"node", "opaque"},
                {"logical_resource", "scene_depth"},
                {"aspect", "depth"},
                {"load_op", "clear"},
                {"store_op", "dont_care"}},
               {{"node", "present"},
                {"logical_resource", "display"},
                {"aspect", "color"},
                {"load_op", "discard"},
                {"store_op", "store"}},
           })},
          {"resources",
           Json::array({
               {{"logical_resource", "scene_color"},
                {"format", "R16G16B16A16Sfloat"},
                {"dimension", "2d"},
                {"extent", {{"width", 1280}, {"height", 720}}}},
               {{"logical_resource", "scene_depth"},
                {"format", "D32Sfloat"},
                {"dimension", "2d"},
                {"extent", {{"width", 1280}, {"height", 720}}}},
               {{"logical_resource", "display"},
                {"format", "B8G8R8A8Srgb"},
                {"dimension", "2d"},
                {"extent", {{"width", 1280}, {"height", 720}}}},
           })},
          // This deliberately large compiled-only subtree must not leak into
          // the compact Studio model.
          {"ejectable_complete_physical_plan",
           {{"private_rows", std::vector<std::string>(256, "compiled detail")}}}}},
        {"surface_resource_contracts",
         Json::array({
             {{"resource", "history_color"},
              {"provider_feature", "taa"},
              {"provider_ref", "engine://features/taa.json"},
              {"sampling", "sampled"},
              {"view_policy", "family_array"},
              {"fallback", "black"},
              {"material_consumers", {"opaque"}},
              {"fullscreen_consumers", Json::array()}},
         })},
        {"material_routing",
         {{"policy", "hybrid_v1"},
          {"routes",
           {{"forward_opaque",
             {{"pass", "opaque"},
              {"contract", "forward_opaque_v1"},
              {"shader_contract", "forward_scene_color_v1"},
              {"phase", "opaque"}}}}}}},
    };
}

const FramePlanResource &resource(const FramePlanModel &model,
                                  const std::string &name) {
    const auto found = std::find_if(
        model.resources.begin(), model.resources.end(),
        [&](const FramePlanResource &candidate) { return candidate.name == name; });
    REQUIRE(found != model.resources.end());
    return *found;
}

} // namespace

TEST_CASE("Devstudio frame plan model builds a compact ordered projection of get_frame_plan",
          "[devstudio][frame-plan]") {
    const std::string response = responseFixture().dump();
    const FramePlanModel model = buildFramePlanModel(response);

    REQUIRE(model.graph == "main_render");
    REQUIRE(model.runtime_generation == 42);
    REQUIRE(model.response_bytes == response.size());
    REQUIRE(model.nodes.size() == 3);
    REQUIRE(model.nodes[0].name == "cull");
    REQUIRE(model.nodes[0].kind == "compute");
    REQUIRE(model.nodes[0].semantic_dialect == "pelican.logical.compute@1");
    REQUIRE(model.nodes[0].resource_uses.size() == 1);
    REQUIRE(model.nodes[0].resource_uses[0].access == "write");

    const FramePlanNode &opaque = model.nodes[1];
    REQUIRE(opaque.name == "opaque");
    REQUIRE(opaque.reads == std::vector<std::string>{"indirect_args"});
    REQUIRE(opaque.history_reads == std::vector<std::string>{"history_color"});
    REQUIRE(opaque.writes ==
            std::vector<std::string>{"scene_color", "scene_depth"});
    REQUIRE(opaque.attachments.size() == 2);
    REQUIRE(opaque.color_load_op == "clear");
    REQUIRE(opaque.color_store_op == "store");
    REQUIRE(opaque.depth_load_op == "clear");
    REQUIRE(opaque.depth_store_op == "dont_care");
    REQUIRE(opaque.material_filter.has_value());
    REQUIRE(opaque.material_filter->resolved_draw_count == 12);
    REQUIRE(opaque.incoming_barriers == std::vector<std::size_t>{0});
    REQUIRE(opaque.outgoing_barriers == std::vector<std::size_t>{1});

    // A directly published high-level operation wins over the lower-level
    // attachment spelling while the per-resource operation remains available.
    REQUIRE(model.nodes[2].color_load_op == "load");
    REQUIRE(model.nodes[2].attachments[0].load_op == "discard");

    REQUIRE(model.barriers.size() == 2);
    REQUIRE(model.barriers[1].resource == "scene_color");
    REQUIRE(model.material_routes.size() == 1);
    REQUIRE(model.material_routes[0].route == "forward_opaque");
    REQUIRE(model.material_routes[0].pass == "opaque");

    const FramePlanResource &scene_color = resource(model, "scene_color");
    REQUIRE(scene_color.format == "R16G16B16A16Sfloat");
    REQUIRE(scene_color.width == 1280);
    REQUIRE(scene_color.height == 720);
    REQUIRE(scene_color.writers == std::vector<std::string>{"opaque"});
    REQUIRE(scene_color.readers == std::vector<std::string>{"present"});

    const FramePlanResource &history = resource(model, "history_color");
    REQUIRE(history.history_readers == std::vector<std::string>{"opaque"});
    REQUIRE(history.provider_feature == "taa");
    REQUIRE(history.material_consumers == std::vector<std::string>{"opaque"});
}

TEST_CASE("Devstudio frame plan model accepts only the current public contract",
          "[devstudio][frame-plan]") {
    auto wrong_version = responseFixture();
    wrong_version["version"] = 2;
    REQUIRE_THROWS_WITH(
        buildFramePlanModel(wrong_version.dump()),
        ContainsSubstring("requires pelican.frame_plan version 1"));

    auto duplicate_order = responseFixture();
    duplicate_order["nodes"][1]["order"] = 2;
    REQUIRE_THROWS_WITH(buildFramePlanModel(duplicate_order.dump()),
                        ContainsSubstring("duplicate node order"));

    auto unknown_barrier_node = responseFixture();
    unknown_barrier_node["barriers"][0]["from"] = "missing";
    REQUIRE_THROWS_WITH(buildFramePlanModel(unknown_barrier_node.dump()),
                        ContainsSubstring("references an unknown node"));

    REQUIRE_THROWS_WITH(buildFramePlanModel("not json"),
                        ContainsSubstring("is not valid JSON"));
}
