// UX-6b: what the frame costs and what the last edit did to it, read from
// get_status's gpu_timing and get_frame_plan, kept per runtime generation. No
// Qt and no engine: the replies are written the way the engine writes them
// (rendertiming.cpp statusJson, frameplanner's plan).

#include "planperfmodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

namespace {

using PelicanStudio::PlanPerfModel;
using Json = nlohmann::json;

// get_status, as far as gpu_timing goes: the window began at `generation`
// and holds `samples` frames; each node's samples are its barriers and body.
Json status(std::uint64_t generation, std::size_t samples, double average, double min,
            double max, const Json &nodes = Json::array()) {
    Json averages = Json::array();
    if (samples > 0)
        averages.push_back({{"graph_variant", "main"},
                            {"frame_count", samples},
                            {"average_total_ms", average},
                            {"min_total_ms", min},
                            {"max_total_ms", max}});
    return {{"frame", 900},
            {"gpu_timing",
             {{"schema_version", 2},
              {"enabled", true},
              {"supported", true},
              {"reason", "enabled"},
              {"generation", generation},
              {"samples", samples},
              {"logical_frame_averages", averages},
              {"nodes", nodes}}}};
}

Json sample(const char *node, const char *subrange, double ms) {
    return {{"node_name", node}, {"subrange", subrange}, {"supported", true}, {"ms", ms}};
}

Json plan(std::uint64_t generation, std::size_t passes, std::size_t barriers,
          std::size_t alias_groups) {
    Json nodes = Json::array();
    for (std::size_t index = 0; index < passes; ++index)
        nodes.push_back({{"name", "pass_" + std::to_string(index)}});
    Json barrier_list = Json::array();
    for (std::size_t index = 0; index < barriers; ++index)
        barrier_list.push_back({{"from", "a"}, {"to", "b"}});
    Json groups = Json::array();
    for (std::size_t index = 0; index < alias_groups; ++index)
        groups.push_back({{"id", "alias_" + std::to_string(index)}});
    return {{"graph", "main_render"},
            {"runtime_generation", generation},
            {"nodes", nodes},
            {"barriers", barrier_list},
            {"physical_target_plan", {{"alias_groups", groups}}}};
}

} // namespace

TEST_CASE("UX-6b decision 1 and 2: the status line says the frame's GPU time and the plan's "
          "size, and each node its own time",
          "[studio][plan-perf]") {
    PlanPerfModel model;
    // Nothing read: no line.
    CHECK(model.summaryLine().empty());
    REQUIRE(model.readPlan(plan(4, 24, 20, 2)));
    CHECK(model.summaryLine() ==
          "GPU measuring\xE2\x80\xA6 \xC2\xB7 24 passes \xC2\xB7 20 barriers \xC2\xB7 2 alias groups");
    // Fewer than 30 frames in the window: still measuring.
    REQUIRE(model.readStatus(status(4, 29, 0.2, 0.1, 0.3)));
    CHECK(model.summaryLine().starts_with("GPU measuring"));
    // Two nodes over two frames: each node's frames, averaged.
    REQUIRE(model.readStatus(status(4, 30, 0.1349, 0.11, 0.16,
                                    Json::array({sample("lighting", "barriers", 0.01),
                                                 sample("lighting", "body", 0.05),
                                                 sample("lighting", "barriers", 0.01),
                                                 sample("lighting", "body", 0.03),
                                                 sample("present", "body", 0.06)}))));
    CHECK(model.summaryLine() ==
          "GPU 0.13 ms \xC2\xB7 24 passes \xC2\xB7 20 barriers \xC2\xB7 2 alias groups");
    REQUIRE(model.nodeMs().contains("lighting"));
    CHECK(std::abs(model.nodeMs().at("lighting") - 0.1 / 30.0) < 1e-9);
    CHECK(PlanPerfModel::milliseconds(0.0213) == "0.02 ms");
    CHECK(PlanPerfModel::milliseconds(0.004) == "0.004 ms");
    // get_gpu_timing's node averages, once read, are what the line and the
    // nodes say: the frame is the sum of its nodes (a body the node does not
    // time is not counted).
    const auto timing = [](std::size_t frames) {
        return Json{{"schema", "pelican.gpu_timing_node_averages"},
                    {"version", 1},
                    {"enabled", true},
                    {"supported", true},
                    {"reason", "enabled"},
                    {"window_size", 30},
                    {"frame_count", frames},
                    {"nodes", Json::array({{{"node_name", "lighting"},
                                            {"barriers_ms", 0.01},
                                            {"body_ms", 0.02},
                                            {"body_supported", true}},
                                           {{"node_name", "present"},
                                            {"barriers_ms", 0.001},
                                            {"body_ms", 5.0},
                                            {"body_supported", false}}})}};
    };
    auto reading = PlanPerfModel::timingReading(timing(12));
    REQUIRE(reading);
    model.read(*reading);
    CHECK(model.summaryLine().starts_with("GPU measuring"));
    reading = PlanPerfModel::timingReading(timing(30));
    REQUIRE(reading);
    model.read(*reading);
    CHECK(model.summaryLine().starts_with("GPU 0.03 ms \xC2\xB7 24 passes"));
    CHECK(std::abs(model.nodeMs().at("lighting") - 0.03) < 1e-9);
    CHECK(std::abs(model.nodeMs().at("present") - 0.001) < 1e-9);
    // One of each is said once.
    REQUIRE(model.readPlan(plan(4, 1, 1, 1)));
    CHECK(model.summaryLine() == "GPU 0.03 ms \xC2\xB7 1 pass \xC2\xB7 1 barrier \xC2\xB7 1 alias group");
    // A project that did not turn timing on.
    auto off = timing(0);
    off["enabled"] = false;
    reading = PlanPerfModel::timingReading(off);
    REQUIRE(reading);
    model.read(*reading);
    CHECK(model.summaryLine().starts_with("GPU timing off"));
}

TEST_CASE("UX-6b decision 3: an edit's before and after, read once its generation has 60 frames",
          "[studio][plan-perf]") {
    PlanPerfModel model;
    // What the edits are called.
    CHECK(PlanPerfModel::editLabel("edit_rendering_keys",
                                   {{"set", {{"draw_sort", "state_batched"}}}}) == "draw_sort");
    const Json count{{"ops", Json::array({{{"op", "set_node_parameters"},
                                           {"scope", "root"},
                                           {"node", "loop"},
                                           {"parameters", {{"count", 3}}}}})}};
    CHECK(PlanPerfModel::editLabel("edit_graph_document", count) == "loop.count");
    // A placement moves nothing the engine draws: not an edit here.
    CHECK(PlanPerfModel::editLabel(
              "edit_graph_document",
              {{"ops", Json::array({{{"op", "set_layout"}, {"layout", Json::object()}}})}})
              .empty());
    CHECK(PlanPerfModel::editLabel("get_frame_plan", Json::object()).empty());

    REQUIRE(model.readStatus(status(4, 120, 0.13, 0.11, 0.16)));
    REQUIRE(model.readPlan(plan(4, 24, 20, 0)));
    model.noteEdit(PlanPerfModel::editLabel("edit_rendering_keys",
                                            {{"set", {{"draw_sort", "state_batched"}}}}));
    REQUIRE(model.waiting());
    // The new generation's plan, and a window not yet 60 frames long.
    REQUIRE(model.readPlan(plan(5, 24, 20, 0)));
    REQUIRE(model.readStatus(status(5, 59, 0.12, 0.10, 0.14)));
    CHECK(model.deltaLine().empty());
    REQUIRE(model.readStatus(status(5, 60, 0.12, 0.10, 0.14)));
    CHECK(model.deltaLine() ==
          "no change in the plan \xC2\xB7 after draw_sort: passes 24\xE2\x86\x92" "24, "
          "barriers 20\xE2\x86\x92" "20, alias 0\xE2\x86\x92" "0, GPU 0.13\xE2\x86\x92" "0.12 ms "
          "\xC2\xB7 GPU difference is within noise \xC2\xB7 generation 4\xE2\x86\x92" "5");
    CHECK_FALSE(model.waiting());

    // The loop runs once fewer. No panel read the new generation's plan: the
    // owner is asked to, once.
    model.noteEdit(PlanPerfModel::editLabel("edit_graph_document", count));
    REQUIRE(model.readStatus(status(6, 60, 0.30, 0.29, 0.31)));
    CHECK(model.deltaLine().starts_with("no change in the plan \xC2\xB7 after draw_sort"));
    REQUIRE(model.planWanted() == std::optional<std::uint64_t>{6});
    REQUIRE(model.readPlan(plan(6, 23, 19, 0)));
    CHECK_FALSE(model.planWanted());
    CHECK(model.deltaLine() == "after loop.count: passes 24\xE2\x86\x92" "23, barriers 20\xE2\x86\x92"
                               "19, alias 0\xE2\x86\x92" "0, GPU 0.12\xE2\x86\x92" "0.30 ms "
                               "\xC2\xB7 generation 5\xE2\x86\x92" "6");

    // A restart in between: the before is kept, the new engine's first
    // generation is the after even though it is numbered lower.
    model.noteEdit("multisampling");
    model.engineStopped();
    CHECK(model.summaryLine().empty());
    REQUIRE(model.readPlan(plan(1, 23, 19, 0)));
    REQUIRE(model.readStatus(status(1, 60, 0.50, 0.495, 0.505)));
    CHECK(model.deltaLine() ==
          "no change in the plan \xC2\xB7 after multisampling: passes 23\xE2\x86\x92" "23, "
          "barriers 19\xE2\x86\x92" "19, alias 0\xE2\x86\x92" "0, GPU 0.30\xE2\x86\x92" "0.50 ms "
          "\xC2\xB7 generation 6\xE2\x86\x92" "1");

    // A new generation's first frames are its pipelines' first compiles: the
    // time and its spread (10th to 90th percentile) leave them out while the
    // window still begins there.
    model.noteEdit("draw_sort");
    auto settling = status(2, 60, 1.35, 0.50, 3.0);
    Json history = Json::array();
    for (int frame = 0; frame < 60; ++frame)
        history.push_back({{"graph_variant", "main"},
                           {"total_ms", frame < 20 ? 3.0 : (frame % 2 == 0 ? 0.50 : 0.54)}});
    settling["gpu_timing"]["logical_frame_history"] = history;
    settling["gpu_timing"]["history_capacity"] = 120;
    REQUIRE(model.readPlan(plan(2, 23, 19, 0)));
    REQUIRE(model.readStatus(settling));
    CHECK(model.deltaLine() ==
          "no change in the plan \xC2\xB7 after draw_sort: passes 23\xE2\x86\x92" "23, "
          "barriers 19\xE2\x86\x92" "19, alias 0\xE2\x86\x92" "0, GPU 0.50\xE2\x86\x92" "0.52 ms "
          "\xC2\xB7 generation 1\xE2\x86\x92" "2");
}

TEST_CASE("UX-6b decision 2 (engine ST-1): get_status's node_averages are each node's time, "
          "in place of summing every sample of the window",
          "[studio][plan-perf]") {
    // ST-1: `nodes` is the last frame's samples only; `node_averages` is the
    // window's average per node, in get_gpu_timing's row shape.
    auto reply = status(7, 120, 0.2, 0.1, 0.3,
                        Json::array({sample("lighting", "barriers", 9.0),
                                     sample("lighting", "body", 9.0)}));
    reply["gpu_timing"]["node_averages"] =
        Json::array({{{"view_index", 0}, {"node_kind", "render"}, {"node_name", "lighting"},
                      {"barriers_ms", 0.004}, {"body_ms", 0.016}, {"body_supported", true},
                      {"sample_count", 120}},
                     {{"view_index", 1}, {"node_kind", "render"}, {"node_name", "lighting"},
                      {"barriers_ms", 0.001}, {"body_ms", 0.009}, {"body_supported", true},
                      {"sample_count", 120}},
                     {{"view_index", 0}, {"node_kind", "anchor"}, {"node_name", "post"},
                      {"barriers_ms", 0.002}, {"body_ms", 0.0}, {"body_supported", false},
                      {"sample_count", 120}}});
    const auto reading = PlanPerfModel::statusReading(reply);
    REQUIRE(reading);
    REQUIRE(reading->node_ms.size() == 2);
    // Both views, barriers and body; not the last frame's samples / 120.
    CHECK(std::abs(reading->node_ms.at("lighting") - 0.03) < 1e-9);
    CHECK(std::abs(reading->node_ms.at("post") - 0.002) < 1e-9);
    // The window's figures are read as before.
    REQUIRE(reading->generation == std::optional<std::uint64_t>{7});
    REQUIRE(reading->gpu);
    CHECK(reading->gpu->samples == 120);
    // Without node_averages (an engine before ST-1): every sample summed.
    reply["gpu_timing"].erase("node_averages");
    const auto summed = PlanPerfModel::statusReading(reply);
    REQUIRE(summed);
    CHECK(std::abs(summed->node_ms.at("lighting") - 18.0 / 120.0) < 1e-9);
}
