#include "../src/core/userpublic/animation/curve.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"
#include "../src/project/graphparameterschema.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "graph_composition_fixture.hpp"
#include "graphtestopaquemodule.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

using namespace Pelican::Animation;
using Catch::Approx;

TEST_CASE("pose attributes derive a matrix upload without changing their source", "[curve][graph-pose-attribute]") {
    using Json = nlohmann::json;
    const auto document = Json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{"pose":{"nodes":[{"id":"bone","op":"render.attribute","parameters":{
        "element":"mat4","domain":"instance","source":{"asset":"project://actor.gltf","pose_node":2}},
        "outputs":["resource"]}],"outputs":{"matrix":{"node":"bone","port":"resource"}}}},
      "nodes":[{"id":"actor","op":"graph.instance","definition":"pose","owner":{"scene":"main","object":1}}],
      "outputs":{"pose":{"node":"actor","port":"matrix"}}})");
    auto input = document;
    const auto resolved = Pelican::resolveGraphAttributeSources(input,
        [](auto, const Json &parameters) -> std::uint64_t {
            REQUIRE(parameters["source"]["pose_node"] == 2);
            return 1;
        });
    REQUIRE(input == document);
    const auto lowered = Pelican::lowerRenderGraphDocument(resolved);
    REQUIRE(lowered["buffers"][0]["size"] == 64);
    REQUIRE(lowered["buffers"][0]["host_source"] == "runtime_f32");
    auto &parameters = input["definitions"]["pose"]["nodes"][0]["parameters"];
    for (const auto &invalid : Json::array({-1, 0.5, "2", true, std::uint64_t{1} << 32})) {
        parameters["source"]["pose_node"] = invalid;
        REQUIRE_THROWS(Pelican::partitionGraphDocument(input));
    }
    const Json invalid_parameters{{"element", "vec4"}, {"domain", "vertex"}, {"count", 1}, {"host_source", "runtime_f32"}};
    for (const auto &[key, value] : invalid_parameters.items()) {
        auto invalid = document;
        invalid["definitions"]["pose"]["nodes"][0]["parameters"][key] = value;
        REQUIRE_THROWS(Pelican::partitionGraphDocument(invalid));
    }
    input = document;
    input["nodes"][0].erase("owner");
    REQUIRE_THROWS(Pelican::partitionGraphDocument(input));
}

TEST_CASE("deformed attribute sources require owned typed vertex declarations", "[curve][graph-deformed-attribute]") {
    using Json = nlohmann::json;
    auto document = Json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{"geometry":{"nodes":[
        {"id":"position","op":"render.attribute","parameters":{"element":"vec3","domain":"vertex",
          "source":{"asset":"project://character.glb","mesh":0,"primitive":0,"semantic":"POSITION","deformed_node":0}},"outputs":["resource"]}],
        "outputs":{"position":{"node":"position","port":"resource"}}}},
      "nodes":[{"id":"model","op":"graph.instance","definition":"geometry","owner":{"scene":"main","object":1}}],
      "outputs":{"position":{"node":"model","port":"position"}}})");
    const auto original = document;
    const auto parameters = [](Json &value) -> Json & {
        return value["definitions"]["geometry"]["nodes"][0]["parameters"];
    };
    SECTION("resolution derives counts without changing authored source") {
        std::size_t calls = 0;
        const auto resolved = Pelican::resolveGraphAttributeSources(document,
            [&](std::string_view id, const Json &source_parameters) -> std::uint64_t {
                REQUIRE(std::string{id} == "model/position");
                REQUIRE(source_parameters == parameters(document));
                ++calls;
                return 4;
            });
        REQUIRE(calls == 1);
        REQUIRE(document == original);
        const auto &derived = resolved["definitions"]["geometry"]["nodes"][0]["parameters"];
        REQUIRE(derived["count"] == 4);
        REQUIRE_FALSE(derived.contains("source"));
        parameters(document)["source"]["semantic"] = "NORMAL";
        REQUIRE_NOTHROW(Pelican::partitionGraphDocument(document));
        parameters(document)["source"]["deformed_node"] = std::numeric_limits<std::uint32_t>::max();
        REQUIRE_NOTHROW(Pelican::partitionGraphDocument(document));
    }
    SECTION("indices must be uint32 integers") {
        for (const auto &invalid : Json::array({-1, 0.5, "0", true, std::uint64_t{1} << 32})) {
            parameters(document)["source"]["deformed_node"] = invalid;
            REQUIRE_THROWS(Pelican::partitionGraphDocument(document));
        }
    }
    SECTION("only current position and normal vertex vec3 are supported") {
        const Json invalid_parameters{{"element", "vec4"}, {"domain", "instance"}, {"count", 4}};
        for (const auto &[key, value] : invalid_parameters.items()) {
            auto invalid = document;
            parameters(invalid)[key] = value;
            REQUIRE_THROWS(Pelican::partitionGraphDocument(invalid));
        }
        parameters(document)["source"]["semantic"] = "TANGENT";
        REQUIRE_THROWS(Pelican::partitionGraphDocument(document));
        document = original;
        parameters(document)["source"]["morph_target"] = 0;
        REQUIRE_THROWS(Pelican::partitionGraphDocument(document));
    }
    SECTION("an owned instance selects the live model") {
        document["nodes"][0].erase("owner");
        REQUIRE_THROWS(Pelican::partitionGraphDocument(document));
    }
}

TEST_CASE("graph state transfer follows stable nodes dependencies and owners", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"].push_back({{"id", "custom_control"}, {"op", "value.parameter"},
        {"parameters", {{"name", "left/custom"}, {"default", 3}}}, {"outputs", {"value"}}});
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 1}};
    document["nodes"][4]["owner"] = {{"scene", "main"}, {"object", 2}};
    addGraphEventClock(document);
    const Pelican::ValueGraph::Graph previous{document.dump()};
    auto state = previous.createState();
    REQUIRE(previous.emitEvent(state, "left/commands", CurvePlaybackCommand::start, 10));
    REQUIRE(previous.emitEvent(state, "right/commands", CurvePlaybackCommand::start, 10));
    REQUIRE(previous.evaluate({{"time", 10.25}}, {}, {}, &state).at("left")[0] == 0.5);
    REQUIRE(previous.emitEvent(state, "left/commands", CurvePlaybackCommand::pause, 10.5));
    SECTION("node reorder and sibling attachment preserve elapsed time and queued events") {
        auto extra = document["nodes"][4];
        extra["id"] = "extra";
        document["nodes"].insert(document["nodes"].begin(), extra);
        std::reverse(document["nodes"].begin(), document["nodes"].end());
        const Pelican::ValueGraph::Graph next{document.dump()};
        auto transfer = next.transferState(previous, state);
        REQUIRE(transfer.retained_nodes.contains("left/playhead"));
        REQUIRE(transfer.retained_nodes.contains("custom_control"));
        REQUIRE(transfer.retained_parameters.contains("left/custom"));
        REQUIRE_FALSE(transfer.retained_nodes.contains("extra/playhead"));
        const auto output = next.evaluate({{"time", 10.75}}, {}, {}, &transfer.state);
        REQUIRE(output.at("left")[0] == 1);
        REQUIRE(output.at("right")[0] == 3);
        REQUIRE(previous.evaluate({{"time", 10.75}}, {}, {}, &state).at("left")[0] == 1);
    }
    SECTION("changed owner does not inherit its old clock") {
        document["nodes"][3]["owner"]["object"] = 3;
        const Pelican::ValueGraph::Graph next{document.dump()};
        auto transfer = next.transferState(previous, state);
        REQUIRE_FALSE(transfer.retained_nodes.contains("left/playhead"));
        REQUIRE(transfer.retained_nodes.contains("right/playhead"));
        REQUIRE(next.evaluate({{"time", 10.75}}, {}, {}, &transfer.state).at("left")[0] == 0);
    }
    SECTION("changed upstream input invalidates dependent clocks") {
        document["nodes"][0]["parameters"]["name"] = "scene_time";
        const Pelican::ValueGraph::Graph next{document.dump()};
        auto transfer = next.transferState(previous, state);
        REQUIRE_FALSE(transfer.retained_nodes.contains("left/playhead"));
        REQUIRE_FALSE(transfer.retained_nodes.contains("right/playhead"));
        REQUIRE(next.evaluate({{"scene_time", 10.75}}, {}, {}, &transfer.state).at("left")[0] == 0);
    }
}

TEST_CASE("graph binding shapes are available without live input evaluation", "[curve][graph-composition]") {
    const Pelican::ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"live","op":"input","name":"external"},
        {"id":"one","op":"constant","value":1}],
      "outputs":{"pair":["live","one"],"volume":["live"]},
      "bindings":[{"output":"pair","kind":"render_buffer","target":"values"},
        {"output":"volume","kind":"sound_volume","target":"voice"}]})"};
    REQUIRE_THROWS(graph.evaluate({}));
    REQUIRE(graph.inputNames() == std::set<std::string>{"external"});
    const auto bindings = graph.renderBufferBindings();
    REQUIRE(bindings.size() == 1);
    REQUIRE(bindings[0].target == "values");
    REQUIRE(bindings[0].element_count == 2);
    REQUIRE(graph.evaluate({{"external", 3}}).at("pair") == std::vector<double>{3, 1});

    auto document = parameterGraphCompositionFixture();
    addGraphTriggerClock(document);
    const Pelican::ValueGraph::Graph stateful{document.dump()};
    REQUIRE_THROWS(stateful.evaluate({{"time", 0}}));
    const auto lowered = stateful.renderBufferBindings();
    REQUIRE(lowered.size() == 1);
    REQUIRE(lowered[0].target == "render");
    REQUIRE(lowered[0].element_count == 1);
}

// RS-15: render.scalar writes a vec2 .. vec4 given as numbers, at a byte
// offset, and several sinks share one buffer by tiling it from byte 0. The
// sinks of one buffer stage it as one write in offset order; a scalar sink
// without an offset writes as before.
TEST_CASE("RS-15 render.scalar writes a vec4 and a float into one buffer at their offsets",
          "[curve][graph-composition][valuegraph][rs15]") {
    using Json = nlohmann::json;
    const auto document = [](const Json &color_offset, const Json &gain_offset, const std::string &element) {
        return Json{{"schema", "pelican.graph"}, {"version", 1},
            {"nodes", Json::array({
                {{"id", "packed"}, {"op", "render.buffer"},
                 {"parameters", {{"element", element}, {"size", 20}, {"host_source", "runtime_f32"}}},
                 {"outputs", {"resource"}}},
                {{"id", "two"}, {"op", "value.constant"}, {"parameters", {{"value", 2.0}}}, {"outputs", {"value"}}},
                {{"id", "color"}, {"op", "render.scalar"},
                 {"parameters", {{"target", "packed"}, {"value", {0.25, 0.5, 0.75, 1.0}}, {"offset", color_offset}}},
                 {"outputs", Json::array()}},
                {{"id", "gain"}, {"op", "render.scalar"},
                 {"parameters", {{"target", "packed"}, {"offset", gain_offset}}},
                 {"inputs", {{"value", {{"node", "two"}, {"port", "value"}}}}}, {"outputs", Json::array()}}})},
            {"outputs", Json::object()}};
    };
    const Pelican::ValueGraph::Graph graph{document(0, 16, "float").dump()};
    const auto bindings = graph.renderBufferBindings();
    REQUIRE(bindings.size() == 1);
    CHECK(bindings[0].target == "packed");
    CHECK(bindings[0].element_count == 5);
    std::vector<std::pair<std::string, std::vector<float>>> writes;
    const auto published = graph.publish(Pelican::GameContext{}, {}, {},
        [&](std::string_view target, std::span<const float> values) {
            writes.emplace_back(std::string{target}, std::vector<float>(values.begin(), values.end()));
            return true;
        });
    CHECK(published.unavailable.empty());
    CHECK(published.applied == 2);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].first == "packed");
    CHECK(writes[0].second == std::vector<float>{0.25f, 0.5f, 0.75f, 1.0f, 2.0f});

    const auto refusal = [](const Json &authored) {
        try {
            (void)Pelican::ValueGraph::Graph{authored.dump()};
        } catch (const Pelican::GraphRegistryError &error) {
            return std::pair{std::string{error.code()}, error.detail()};
        }
        FAIL("the graph was not refused by name");
        return std::pair{std::string{}, Json{}};
    };
    // The float written over the vec4's last component.
    auto [code, fields] = refusal(document(0, 12, "float"));
    CHECK(code == "graph.render_scalar_layout_invalid@1");
    CHECK(fields == Json{{"target", "packed"}, {"node", "gain"}, {"offset", 12}, {"expected_offset", 16},
                         {"reason", "overlap"}});
    // A hole between them.
    std::tie(code, fields) = refusal(document(0, 20, "float"));
    CHECK(code == "graph.render_scalar_layout_invalid@1");
    CHECK(fields.at("reason") == "gap");
    // An integer buffer cannot take float32 values.
    std::tie(code, fields) = refusal(document(0, 16, "uint"));
    CHECK(code == "graph.render_scalar_type_mismatch@1");
    CHECK(fields == Json{{"node", "color"}, {"target", "packed"}, {"element", "uint"}, {"components", 4}});
    // A vector is 2 to 4 numbers, and an offset a multiple of 4 bytes.
    auto five = document(0, 16, "float");
    five["nodes"][2]["parameters"]["value"] = {1, 2, 3, 4, 5};
    CHECK_THROWS_AS(Pelican::ValueGraph::Graph{five.dump()}, std::invalid_argument);
    CHECK_THROWS_AS(Pelican::ValueGraph::Graph{document(2, 18, "float").dump()}, std::invalid_argument);
}

TEST_CASE("typed playback events queue independently for consumers and preserve timestamp order", "[curve][graph-composition]") {
    auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "nodes":[{"id":"commands","op":"event.playback","outputs":["events"]},
        {"id":"time","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
        {"id":"lag","op":"value.input","parameters":{"name":"lag"},"outputs":["value"]},
        {"id":"a","op":"value.event_clock","inputs":{"time":{"node":"time","port":"value"},
          "events":{"node":"commands","port":"events"}},"outputs":["value"]},
        {"id":"b","op":"value.event_clock","inputs":{"time":{"node":"lag","port":"value"},
          "events":{"node":"commands","port":"events"}},"outputs":["value"]}],
      "outputs":{"a":{"node":"a","port":"value"},"b":{"node":"b","port":"value"}}})");
    const Pelican::ValueGraph::Graph graph{document.dump()};
    auto state = graph.createState();
    using Command = CurvePlaybackCommand;
    REQUIRE(graph.emitEvent(state, "commands", Command::start, 10.125));
    const auto evaluate = [&](double time, double lag) { return graph.evaluate({{"time", time}, {"lag", lag}}, {}, {}, &state); };
    REQUIRE(evaluate(10, 10).at("a")[0] == 0);
    const auto first = evaluate(10.5, 10);
    REQUIRE(first.at("a")[0] == 0.375);
    REQUIRE(first.at("b")[0] == 0);
    REQUIRE(evaluate(10.75, 10.5).at("b")[0] == 0.375);
    REQUIRE(graph.emitEvent(state, "commands", Command::resume, 11));
    REQUIRE(graph.emitEvent(state, "commands", Command::pause, 10.75));
    REQUIRE(evaluate(11.25, 11.25).at("a")[0] == 0.875);
    REQUIRE(evaluate(11.25, 11.25).at("b")[0] == 0.875);
    REQUIRE(graph.emitEvent(state, "commands", Command::reset, 11.25));
    REQUIRE(graph.emitEvent(state, "commands", Command::start, 11.25));
    REQUIRE(evaluate(11.5, 11.5).at("a")[0] == 0.25);
    REQUIRE_FALSE(graph.emitEvent(state, "missing", Command::start, 12));
    REQUIRE_THROWS(graph.emitEvent(state, "commands", Command::start, std::numeric_limits<double>::infinity()));
    document["nodes"][3]["inputs"]["events"] = {{"node", "time"}, {"port", "value"}};
    REQUIRE_THROWS(Pelican::ValueGraph::Graph(document.dump()));
}

TEST_CASE("trigger clocks keep evaluation state separate and reset inactive scopes", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    addGraphTriggerClock(document);
    const Pelican::ValueGraph::Graph graph{document.dump()};
    auto state = graph.createState();
    auto other = graph.createState();
    REQUIRE_THROWS(graph.evaluate({{"time", 10}}));
    const Pelican::ValueGraph::ParameterValues fire{{"left/fire", 1}};
    REQUIRE(graph.evaluate({{"time", 10}}, fire, {}, &state).at("left")[0] == 0);
    REQUIRE(graph.evaluate({{"time", 10.25}}, fire, {}, &state).at("left")[0] == 0.5);
    REQUIRE(graph.evaluate({{"time", 10.25}}, fire, {}, &state).at("left")[0] == 0.5);
    REQUIRE(graph.evaluate({{"time", 10.25}}, fire, {}, &other).at("left")[0] == 0);
    REQUIRE(graph.evaluate({{"time", 10.5}}, fire, {"left"}, &state).at("right")[0] == 0);
    REQUIRE(graph.evaluate({{"time", 10.75}}, fire, {}, &state).at("left")[0] == 0);
    const Pelican::ValueGraph::Graph another{document.dump()};
    REQUIRE_THROWS(another.evaluate({{"time", 11}}, fire, {}, &state));
    REQUIRE(nlohmann::json::parse(graph.serialize()) == document);
}

TEST_CASE("common image ports lower typed compute dependencies", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    addGraphImageStage(document);
    const auto lowered = Pelican::lowerRenderGraphDocument(document);
    REQUIRE(lowered.at("render_targets")[0].at("name") == "intermediate");
    REQUIRE_FALSE(lowered.at("render_targets")[0].contains("type"));
    const auto &tasks = lowered.at("compute_tasks");
    const auto reader = std::find_if(tasks.begin(), tasks.end(), [](const auto &task) { return task.at("name") == "image_probe"; });
    REQUIRE(reader != tasks.end());
    REQUIRE(reader->at("after") == nlohmann::json::array({"probe"}));
    REQUIRE(reader->at("resource_ports").at("source").at("resource") == "intermediate");
    REQUIRE(reader->at("resource_ports").at("source").at("kind") == "image");
    const Pelican::ValueGraph::Graph values{document.dump()};
    REQUIRE(values.evaluate({{"time", 0.25}}).at("left")[0] == 0.5);
    REQUIRE(nlohmann::json::parse(values.serialize()) == document);
    auto mismatch = document;
    mismatch["nodes"].back()["parameters"]["ports"]["source"]["type"]["arguments"]["reference"] = "display";
    REQUIRE_THROWS(Pelican::lowerRenderGraphDocument(mismatch));
    mismatch = document;
    mismatch["nodes"].back()["inputs"]["source"] = {{"node", "render"}, {"port", "resource"}};
    REQUIRE_THROWS(Pelican::lowerRenderGraphDocument(mismatch));
}

TEST_CASE("owner scope dependencies cross CPU GPU boundaries without stopping sibling branches", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    const auto partitions = Pelican::partitionGraphDocument(document);
    const std::set<std::string> left{"left"};
    REQUIRE(partitions.values.node_scopes.at("left/shape") == left);
    REQUIRE(partitions.values.node_scopes.at("$upload/render") == left);
    REQUIRE(partitions.rendering.node_scopes.at("render") == left);
    REQUIRE(partitions.rendering.node_scopes.at("probe") == left);
    REQUIRE(partitions.rendering.output_scopes.at("gpu") == left);
    REQUIRE(partitions.values.node_scopes.at("clock").empty());
    REQUIRE(partitions.values.node_scopes.at("right/scaled").empty());
    REQUIRE(partitions.rendering.node_scopes.at("result").empty());
    const Pelican::ValueGraph::Graph graph{document.dump()};
    const auto values = graph.evaluate({{"time", 0.25}}, {}, {"left"});
    REQUIRE_FALSE(values.contains("left"));
    REQUIRE(values.at("right")[0] == 1);
    REQUIRE_FALSE(graph.nodeActive("probe", {"left"}));
    REQUIRE(graph.nodeActive("probe", {}));
}

TEST_CASE("GPU lowering omits inactive owner dependencies and keeps shared resources", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    auto ordered = document["nodes"][7];
    ordered["id"] = "ordered";
    ordered["inputs"]["source"] = {{"node", "result"}, {"port", "resource"}};
    ordered["parameters"]["after"] = {"probe"};
    document["nodes"].push_back(ordered);
    const auto original = document;
    const auto active = Pelican::lowerRenderGraphDocument(document);
    const auto inactive = Pelican::lowerRenderGraphDocument(document, {"left"});
    const auto named = [](const auto &config, const char *kind, const char *name) {
        const auto &entries = config.at(kind);
        return std::any_of(entries.begin(), entries.end(), [&](const auto &entry) { return entry.at("name") == name; });
    };
    REQUIRE(named(active, "buffers", "render"));
    REQUIRE(named(active, "compute_tasks", "probe"));
    REQUIRE_FALSE(named(inactive, "buffers", "render"));
    REQUIRE_FALSE(named(inactive, "compute_tasks", "probe"));
    REQUIRE(named(inactive, "buffers", "result"));
    const auto &tasks = inactive.at("compute_tasks");
    const auto ordered_task = std::find_if(tasks.begin(), tasks.end(), [](const auto &task) { return task.at("name") == "ordered"; });
    REQUIRE(ordered_task != tasks.end());
    REQUIRE(ordered_task->at("after").empty());
    REQUIRE(Pelican::lowerRenderGraphDocument(document) == active);
    REQUIRE(document == original);
}

TEST_CASE("nested passthrough instances retain lifetime boundaries after alias expansion", "[curve][graph-composition]") {
    const auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{
        "alias":{"nodes":[{"id":"input","op":"graph.input"}],
                 "outputs":{"value":{"node":"input","port":"value"}}},
        "wrapper":{"nodes":[{"id":"input","op":"graph.input"},
          {"id":"child","op":"graph.instance","definition":"alias",
           "owner":{"scene":"main","object":2},
           "inputs":{"input":{"node":"input","port":"value"}}}],
          "outputs":{"value":{"node":"child","port":"value"}}}},
      "nodes":[
        {"id":"source","op":"value.constant","parameters":{"value":3},"outputs":["value"]},
        {"id":"outer","op":"graph.instance","definition":"wrapper",
         "owner":{"scene":"main","object":1},
         "inputs":{"input":{"node":"source","port":"value"}}},
        {"id":"consumer","op":"value.add","inputs":{
          "a":{"node":"outer","port":"value"},"b":{"node":"outer","port":"value"}},"outputs":["value"]}],
      "outputs":{"alias":{"node":"outer","port":"value"},
                 "sum":{"node":"consumer","port":"value"},
                 "direct":{"node":"source","port":"value"}}})");
    const auto graph = Pelican::expandGraphDocument(document);
    const std::set<std::string> owners{"outer", "outer/child"};
    REQUIRE(graph.nodes.size() == 2);
    REQUIRE(graph.node_scopes.at("source").empty());
    REQUIRE(graph.node_scopes.at("consumer") == owners);
    REQUIRE(graph.output_scopes.at("alias") == owners);
    REQUIRE(graph.output_scopes.at("sum") == owners);
    REQUIRE(graph.output_scopes.at("direct").empty());
    REQUIRE(graph.outputs.at("alias") == nlohmann::json{{"node", "source"}, {"port", "value"}});
    const Pelican::ValueGraph::Graph values{document.dump()};
    REQUIRE(values.evaluate({}).at("sum")[0] == 6);
    for (const auto &scope : {"outer", "outer/child"}) {
        const auto inactive = values.evaluate({}, {}, {scope});
        REQUIRE(inactive.size() == 1);
        REQUIRE(inactive.at("direct")[0] == 3);
    }
    REQUIRE(nlohmann::json::parse(values.serialize()) == document);
}

TEST_CASE("inactive owned nodes are not evaluated", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    document["definitions"]["left_only"] = document["definitions"]["scaled_curve"];
    document["nodes"][3]["definition"] = "left_only";
    document["definitions"]["left_only"]["nodes"].push_back({
        {"id", "external"}, {"op", "value.input"},
        {"parameters", {{"name", "not_supplied"}}}, {"outputs", {"value"}}});
    const Pelican::ValueGraph::Graph graph{document.dump()};
    REQUIRE_THROWS_AS(graph.evaluate({{"time", 0.25}}), std::invalid_argument);
    REQUIRE(graph.evaluate({{"time", 0.25}}, {}, {"left"}).at("right")[0] == 1);
}

TEST_CASE("instance parameters preserve defaults and isolate runtime overrides", "[curve][graph-composition]") {
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    const Pelican::ValueGraph::Graph graph{document.dump()};
    REQUIRE(graph.parameterOwner("left/gain") != nullptr);
    REQUIRE(graph.parameterOwner("left/gain")->scene == "main");
    REQUIRE(graph.parameterOwner("left/gain")->object == 42);
    REQUIRE(graph.parameterOwner("right/gain") == nullptr);
    REQUIRE(graph.parameterOwner("left/missing") == nullptr);
    REQUIRE(nlohmann::json::parse(graph.serialize()) == document);
    REQUIRE(graph.parameterDefaults().at("left/gain") == 2);
    REQUIRE(graph.parameterDefaults().at("right/gain") == 4);
    const auto values = graph.evaluate({{"time", 0.25}}, {{"left/gain", 3}});
    REQUIRE(values.at("left")[0] == Approx(0.75));
    REQUIRE(values.at("right")[0] == Approx(1));
    REQUIRE(graph.evaluate({{"time", 0.25}}).at("left")[0] == Approx(0.5));
    REQUIRE_THROWS_AS(graph.evaluate({{"time", 0.25}}, {{"gain", 3}}), std::invalid_argument);
    REQUIRE_THROWS_AS(graph.evaluate({{"time", 0.25}}, {{"left/gain", std::numeric_limits<double>::infinity()}}), std::invalid_argument);
    document["nodes"][3].erase("parameters");
    const Pelican::ValueGraph::Graph fallback{document.dump()};
    REQUIRE(fallback.evaluate({{"time", 0.25}}).at("left")[0] == Approx(0.25));
    document["nodes"][3]["parameters"] = {{"typo", 2}};
    REQUIRE_THROWS_AS(Pelican::ValueGraph::Graph(document.dump()), std::invalid_argument);
    document = parameterGraphCompositionFixture();
    document["nodes"][3]["inputs"]["gain"] = {{"node", "four"}, {"port", "value"}};
    // edge > parameters > default. A stored value under an edge is not a
    // contradiction: it is what the port goes back to, so it is kept rather
    // than refused -- without it an input whose definition declares no default
    // could never be disconnected again.
    const Pelican::ValueGraph::Graph connected{document.dump()};
    REQUIRE(connected.evaluate({{"time", 0.25}}).at("left")[0] == Approx(1.0));
    REQUIRE_FALSE(connected.parameterDefaults().contains("left/gain"));
    REQUIRE(connected.parameterShadowed("left/gain"));
    REQUIRE_FALSE(connected.parameterShadowed("right/gain"));
    REQUIRE(connected.shadowedParameterOwner("right/gain") == nullptr);
    document["nodes"][3]["inputs"].erase("gain");
    REQUIRE(Pelican::ValueGraph::Graph(document.dump()).evaluate({{"time", 0.25}}).at("left")[0] == Approx(0.5));
    // The value under an edge is still checked, so disconnecting cannot land
    // on something the expansion would then refuse.
    document["nodes"][3]["inputs"]["gain"] = {{"node", "four"}, {"port", "value"}};
    document["nodes"][3]["parameters"]["gain"] = "two";
    REQUIRE_THROWS_AS(Pelican::expandGraphDocument(document), std::invalid_argument);
}

TEST_CASE("instance inputs name their supply and the value a publication used",
          "[curve][graph-composition]") {
    using Json = nlohmann::json;
    auto document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    const auto expanded = Pelican::expandGraphDocument(document);
    // Read from the authoring document: a connected port leaves no node behind
    // in the expansion, so it cannot be recovered from the expanded nodes.
    REQUIRE(expanded.instance_inputs.at("left/time").source ==
            Pelican::GraphInstanceInputSource::connected);
    REQUIRE(expanded.instance_inputs.at("left/time").endpoint.at("node") == "clock");
    REQUIRE(expanded.instance_inputs.at("left/gain").source ==
            Pelican::GraphInstanceInputSource::parameter);
    REQUIRE(expanded.instance_inputs.at("left/gain").parameter == "left/gain");
    document["definitions"]["scaled_curve"]["nodes"][1]["default"] = 1;
    document["nodes"][4]["parameters"].erase("gain");
    REQUIRE(Pelican::expandGraphDocument(document).instance_inputs.at("right/gain").source ==
            Pelican::GraphInstanceInputSource::defaulted);

    document = parameterGraphCompositionFixture();
    document["nodes"][3]["owner"] = {{"scene", "main"}, {"object", 42}};
    const Pelican::ValueGraph::Graph graph{document.dump()};
    const auto published = graph.publish(Pelican::GameContext{}, {{"time", 0.25}}, {},
        [](std::string_view, std::span<const float>) { return true; }, {{"left/gain", 3}});
    REQUIRE(published.unavailable.empty());
    const auto &inputs = published.instance_inputs;
    REQUIRE(inputs.at("left/time").source == Pelican::ValueGraph::InstanceInputSource::connected);
    REQUIRE(inputs.at("left/time").node == "clock");
    REQUIRE(inputs.at("left/time").value == Approx(0.25));
    // A run-time override is a fourth origin, reported rather than hidden
    // behind the document's own number.
    REQUIRE(inputs.at("left/gain").source == Pelican::ValueGraph::InstanceInputSource::overridden);
    REQUIRE(inputs.at("left/gain").value == Approx(3));
    REQUIRE(inputs.at("right/gain").source == Pelican::ValueGraph::InstanceInputSource::parameter);
    REQUIRE(inputs.at("right/gain").value == Approx(4));
}

TEST_CASE("graph document ops apply together or not at all", "[curve][graph-composition]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const auto code = [&](const Json &ops) {
        try {
            (void)Pelican::editGraphDocument(document, ops);
        } catch (const Pelican::GraphRegistryError &error) {
            return error.code();
        }
        return std::string{"<accepted>"};
    };
    const Json curve{{"id", "shaper"}, {"op", "value.curve"},
        {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}},
        {"parameters", {{"keys", Json::array({Json{{"time", 0}, {"value", 1}},
                                              Json{{"time", 1}, {"value", 1}}})}}},
        {"outputs", Json::array({"value"})}};
    const auto edited = Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "add_node"}, {"node", curve}},
        Json{{"op", "set_input"}, {"node", "left"}, {"port", "gain"},
             {"source", {{"node", "shaper"}, {"port", "value"}}}}}));
    REQUIRE(edited.at("nodes").back() == curve);
    REQUIRE(edited.at("nodes")[3].at("inputs").at("gain") ==
            Json{{"node", "shaper"}, {"port", "value"}});
    // The stored value survives the edge, so set_input null is the way back.
    REQUIRE(edited.at("nodes")[3].at("parameters").at("gain") == 2);
    const auto restored = Pelican::editGraphDocument(edited, Json::array({
        Json{{"op", "set_input"}, {"node", "left"}, {"port", "gain"}, {"source", nullptr}}}));
    REQUIRE_FALSE(restored.at("nodes")[3].at("inputs").contains("gain"));
    REQUIRE(restored.at("nodes")[3].at("parameters").at("gain") == 2);

    CHECK(code(Json::array({Json{{"op", "add_node"}, {"node", {{"id", "clock"}, {"op", "value.constant"},
        {"parameters", {{"value", 1}}}, {"outputs", Json::array({"value"})}}}}})) == "graph.node_duplicate@1");
    CHECK(code(Json::array({Json{{"op", "remove_node"}, {"node", "absent"}}})) == "graph.node_missing@1");
    CHECK(code(Json::array({Json{{"op", "remove_node"}, {"node", "clock"}}})) == "graph.output_referenced@1");
    CHECK(code(Json::array({Json{{"op", "set_input"}, {"node", "left"}, {"port", "absent"},
        {"source", {{"node", "clock"}, {"port", "value"}}}}})) == "graph.input_unknown@1");
    CHECK(code(Json::array({Json{{"op", "set_node_parameters"}, {"node", "shape"},
        {"parameters", Json::object()}}})) == "graph.definition_immutable@1");
    CHECK(code(Json::array({Json{{"op", "set_input"}, {"node", "left/shape"}, {"port", "time"},
        {"source", {{"node", "clock"}, {"port", "value"}}}}})) == "graph.definition_immutable@1");
    // A valid op ahead of an invalid one writes nothing.
    CHECK(code(Json::array({Json{{"op", "add_node"}, {"node", curve}},
        Json{{"op", "remove_node"}, {"node", "absent"}}})) == "graph.node_missing@1");
}

TEST_CASE("graph topology edits prune placement atomically and their inverse restores it",
          "[curve][graph-composition][graph-undo]") {
    using Json = nlohmann::json;
    const auto source = Json::parse(R"({"schema":"pelican.graph","version":1,
      "nodes":[
        {"id":"source","op":"value.constant","parameters":{"value":2},"outputs":["value"]},
        {"id":"drop","op":"value.multiply","inputs":{"a":{"node":"source","port":"value"},
          "b":{"node":"source","port":"value"}},"outputs":["value"],"label":"Keep on undo"},
        {"id":"sink","op":"value.multiply","inputs":{"a":{"node":"source","port":"value"},
          "b":{"node":"source","port":"value"}},"outputs":["value"]}],
      "outputs":{"value":{"node":"sink","port":"value"}},
      "layout":{
        "nodes":{"source":{"x":0,"y":0},"drop":{"x":100,"y":100},"sink":{"x":100,"y":0}},
        "reroutes":[{"edge":{"node":"drop","port":"a"},"points":[[10,20]]},
          {"edge":{"node":"sink","port":"a"},"points":[[20,30]]},
          {"edge":{"node":"sink","port":"b"},"points":[[40,0],[80,0]]}],
        "ties":[{"x":50,"y":60,"edges":[{"node":"drop","port":"b"},{"node":"sink","port":"b"}]}],
        "frame_plan":{"render":{"nodes":{"unrelated":{"x":10,"y":20}}}}}})");
    for (const bool definition_scope : {false, true}) {
        CAPTURE(definition_scope);
        auto document = source;
        const std::string scope = definition_scope ? "inner" : "root";
        if (definition_scope) {
            auto body = source;
            body.erase("schema");
            body.erase("version");
            document = {{"schema", "pelican.graph"}, {"version", 1},
                {"definitions", {{"inner", body}}},
                {"nodes", Json::array({Json{{"id", "instance"}, {"op", "graph.instance"},
                    {"definition", "inner"}}})},
                {"outputs", {{"value", {{"node", "instance"}, {"port", "value"}}}}}};
            document["layout"]["nodes"]["instance"] = {{"x", 500}, {"y", 600}};
        }
        const auto bodyOf = [&](const Json &value) -> const Json & {
            return definition_scope ? value.at("definitions").at("inner") : value;
        };
        const auto original_node = bodyOf(document).at("nodes").at(1);
        const auto original_layout = bodyOf(document).at("layout");
        const auto removed = Pelican::editGraphDocument(document, Json::array({
            Json{{"op", "remove_node"}, {"scope", scope}, {"node", "drop"}}}));
        const auto &layout = bodyOf(removed).at("layout");
        CHECK_FALSE(layout.at("nodes").contains("drop"));
        REQUIRE(layout.at("reroutes").size() == 2);
        CHECK(layout.at("reroutes").at(0) == original_layout.at("reroutes").at(1));
        CHECK(layout.at("reroutes").at(1).at("points") == Json::array({
            Json::array({40, 0}), Json::array({50, 60}), Json::array({80, 0})}));
        CHECK(layout.at("ties").empty());
        CHECK(layout.at("frame_plan") == original_layout.at("frame_plan"));
        if (definition_scope) CHECK(removed.at("layout") == document.at("layout"));
        // No canvas-generated repair transaction is needed after the removal.
        REQUIRE_NOTHROW(Pelican::editGraphDocument(removed, Json::array({
            Json{{"op", "set_layout"}, {"scope", scope}, {"layout", layout}}})));
        CHECK(Pelican::editGraphDocument(removed, Json::array({
            Json{{"op", "add_node"}, {"scope", scope}, {"node", original_node}, {"index", 1}},
            Json{{"op", "set_layout"}, {"scope", scope}, {"layout", original_layout}}})) == document);

        const auto disconnected = Pelican::editGraphDocument(document, Json::array({
            Json{{"op", "set_input"}, {"scope", scope}, {"node", "drop"},
                {"port", "b"}, {"source", nullptr}}}));
        const auto &disconnected_layout = bodyOf(disconnected).at("layout");
        CHECK(disconnected_layout.at("nodes") == original_layout.at("nodes"));
        CHECK(disconnected_layout.at("ties").empty());
        CHECK(disconnected_layout.at("reroutes").at(2).at("points") ==
            layout.at("reroutes").at(1).at("points"));
        CHECK(Pelican::editGraphDocument(disconnected, Json::array({
            Json{{"op", "replace_node"}, {"scope", scope}, {"node", original_node}},
            Json{{"op", "set_layout"}, {"scope", scope}, {"layout", original_layout}}})) == document);
        auto without_input = original_node;
        without_input["inputs"].erase("b");
        CHECK(Pelican::editGraphDocument(document, Json::array({
            Json{{"op", "replace_node"}, {"scope", scope}, {"node", without_input}}})) == disconnected);
    }
}

TEST_CASE("graph inverse operations validate indexes and replacement topology",
          "[curve][graph-composition][graph-undo]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    auto added = document.at("nodes").at(1);
    added["id"] = "extra";
    for (const auto &index : Json::array({-1, 0.5, "1", true, nullptr,
             Json(document.at("nodes").size() + 1), Json(std::numeric_limits<std::uint64_t>::max())})) {
        CAPTURE(index);
        CHECK_THROWS(Pelican::editGraphDocument(document, Json::array({
            Json{{"op", "add_node"}, {"node", added}, {"index", index}}})));
    }
    CHECK(Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "add_node"}, {"node", added}, {"index", 0}}})).at("nodes").front() == added);
    CHECK(Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "add_node"}, {"node", added}, {"index", document.at("nodes").size()}}}))
        .at("nodes").back() == added);

    const auto original = document.at("nodes").at(3);
    const auto changed = Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "set_node_label"}, {"node", "left"}, {"label", "Edited"}}}));
    CHECK(Pelican::editGraphDocument(changed, Json::array({
        Json{{"op", "replace_node"}, {"node", original}}})) == document);
    for (const auto &invalid : Json::array({
             Json{{"id", "absent"}, {"op", "value.constant"}, {"outputs", {"value"}}},
             Json{{"id", "left/shape"}, {"op", "value.constant"}, {"outputs", {"value"}}},
             Json{{"id", "left"}, {"op", "graph.input"}}})) {
        CAPTURE(invalid);
        CHECK_THROWS(Pelican::editGraphDocument(document, Json::array({
            Json{{"op", "replace_node"}, {"node", invalid}}})));
    }
    auto missing_source = original;
    missing_source["inputs"]["time"]["node"] = "absent";
    CHECK_THROWS(Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "replace_node"}, {"node", missing_source}}})));
    CHECK_THROWS(Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "replace_node"}, {"scope", "scaled_curve"},
             {"node", {{"id", "shape"}, {"op", "graph.instance"}, {"definition", "scaled_curve"}}}}})));
}

namespace {
// The code a refused edit is named by, or "<accepted>". A refusal that is not
// named (std::invalid_argument) escapes, so a case that expects a code fails
// loudly on it.
std::string lp0cEditCode(const nlohmann::json &document, const nlohmann::json &ops) {
    try {
        (void)Pelican::editGraphDocument(document, ops);
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code();
    }
    return "<accepted>";
}

nlohmann::json lp0cEditDetail(const nlohmann::json &document, const nlohmann::json &ops) {
    try {
        (void)Pelican::editGraphDocument(document, ops);
    } catch (const Pelican::GraphRegistryError &error) {
        return error.detail();
    }
    return nullptr;
}
} // namespace

TEST_CASE("LP-0c a definition is added and filled in one op list and saved as written",
          "[curve][graph-composition][graph-definition-edit]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const Json knob{{"id", "knob"}, {"op", "value.constant"}, {"parameters", {{"value", 3}}},
                    {"outputs", Json::array({"value"})}};
    // The body is addressed by `scope` in the very list that creates it.
    const auto edited = Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "add_definition"}, {"name", "echo"}},
        Json{{"op", "add_node"}, {"scope", "echo"}, {"node", knob}},
        Json{{"op", "add_node"}, {"node", {{"id", "echo_use"}, {"op", "graph.instance"},
                                           {"definition", "echo"}}}}}));
    CHECK(edited.at("definitions").at("echo") ==
          Json{{"nodes", Json::array({knob})}, {"outputs", Json::object()}});
    // Nothing else moved: the rest of the document is the one that went in.
    auto without = edited;
    without["definitions"].erase("echo");
    without["nodes"].erase(without["nodes"].size() - 1);
    CHECK(without == document);
    // The body expands under its instance.
    const auto expanded = Pelican::expandGraphDocument(edited);
    CHECK(expanded.origins.contains("echo_use/knob"));
    // Saved and read back, it is the same document.
    CHECK(Json::parse(edited.dump(2)) == edited);
    // An empty definition is the default body; a given body is taken whole.
    const Json body{{"nodes", Json::array({knob})}, {"outputs", {{"value", {{"node", "knob"}, {"port", "value"}}}}},
                    {"layout", {{"nodes", {{"knob", {{"x", 1}, {"y", 2}}}}}}}};
    CHECK(Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "add_definition"}, {"name", "whole"}, {"definition", body}}}))
              .at("definitions").at("whole") == body);
    // The first add creates `definitions` when the document had none.
    const Json bare{{"schema", "pelican.graph"}, {"version", 1},
                    {"nodes", Json::array({knob})}, {"outputs", Json::object()}};
    const auto created = Pelican::editGraphDocument(bare, Json::array({
        Json{{"op", "add_definition"}, {"name", "echo"}}}));
    CHECK(created.at("definitions") ==
          Json{{"echo", {{"nodes", Json::array()}, {"outputs", Json::object()}}}});
}

TEST_CASE("LP-0c renaming a definition rewrites its references and renames back byte for byte",
          "[curve][graph-composition][graph-definition-edit]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const auto renamed = Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "rename_definition"}, {"from", "scaled_curve"}, {"to", "shaped"}}}));
    CHECK_FALSE(renamed.at("definitions").contains("scaled_curve"));
    CHECK(renamed.at("definitions").at("shaped") == document.at("definitions").at("scaled_curve"));
    for (const auto index : {3, 4}) CHECK(renamed.at("nodes").at(index).at("definition") == "shaped");
    const auto expanded = Pelican::expandGraphDocument(renamed);
    CHECK(expanded.origins.at("left/shape").definition == "shaped");
    const auto back = Pelican::editGraphDocument(renamed, Json::array({
        Json{{"op", "rename_definition"}, {"from", "shaped"}, {"to", "scaled_curve"}}}));
    CHECK(back == document);
    // Key order included: the object keeps its keys sorted, so the bytes match.
    CHECK(back.dump(2) == document.dump(2));

    // A check an earlier op recorded follows the rename: the port is declared
    // under the new name, after the instance already supplies it.
    const Json bias{{"id", "bias"}, {"op", "graph.input"}};
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_input"}, {"node", "left"}, {"port", "bias"},
             {"source", {{"node", "two"}, {"port", "value"}}}},
        Json{{"op", "set_input"}, {"node", "right"}, {"port", "bias"},
             {"source", {{"node", "two"}, {"port", "value"}}}},
        Json{{"op", "rename_definition"}, {"from", "scaled_curve"}, {"to", "shaped"}},
        Json{{"op", "add_node"}, {"scope", "shaped"}, {"node", bias}}})) == "<accepted>");
}

TEST_CASE("LP-0c a definition still named is not removed, and a removal comes back as an add",
          "[curve][graph-composition][graph-definition-edit]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const Json remove_scaled = Json::array({
        Json{{"op", "remove_definition"}, {"name", "scaled_curve"}}});
    REQUIRE(lp0cEditCode(document, remove_scaled) == "graph.definition_referenced@1");
    CHECK(lp0cEditDetail(document, remove_scaled) ==
          Json{{"definition", "scaled_curve"}, {"referenced_by", Json::array({"left", "right"})}});

    // A definition nothing names goes, and the add carrying its body restores it.
    auto spare = document;
    spare["definitions"]["spare"] = {
        {"nodes", Json::array({Json{{"id", "k"}, {"op", "value.constant"},
            {"parameters", {{"value", 1}}}, {"outputs", Json::array({"value"})}}})},
        {"outputs", {{"value", {{"node", "k"}, {"port", "value"}}}}},
        {"layout", {{"nodes", {{"k", {{"x", 4}, {"y", 5}}}}}}}};
    const auto body = spare.at("definitions").at("spare");
    const auto removed = Pelican::editGraphDocument(spare, Json::array({
        Json{{"op", "remove_definition"}, {"name", "spare"}}}));
    CHECK_FALSE(removed.at("definitions").contains("spare"));
    const auto restored = Pelican::editGraphDocument(removed, Json::array({
        Json{{"op", "add_definition"}, {"name", "spare"}, {"definition", body}}}));
    CHECK(restored == spare);
    CHECK(restored.dump(2) == spare.dump(2));

    // The reference check waits for the end of the list: the instance that
    // names the definition may be removed after it.
    auto used = spare;
    used["nodes"].push_back({{"id", "user"}, {"op", "graph.instance"}, {"definition", "spare"}});
    CHECK(lp0cEditCode(used, Json::array({
        Json{{"op", "remove_definition"}, {"name", "spare"}},
        Json{{"op", "remove_node"}, {"node", "user"}}})) == "<accepted>");
    const Json alone = Json::array({Json{{"op", "remove_definition"}, {"name", "spare"}}});
    CHECK(lp0cEditCode(used, alone) == "graph.definition_referenced@1");
    CHECK(lp0cEditDetail(used, alone).at("referenced_by") == Json::array({"user"}));
    // And a definition of that name back in its place by the end is no loss.
    CHECK(lp0cEditCode(used, Json::array({
        Json{{"op", "remove_definition"}, {"name", "spare"}},
        Json{{"op", "add_definition"}, {"name", "spare"}, {"definition", body}}})) == "<accepted>");
}

TEST_CASE("LP-0c an instance and its definition's boundary agree after the list, in either order",
          "[curve][graph-composition][graph-definition-edit]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const Json declare{{"op", "add_node"}, {"scope", "scaled_curve"},
                       {"node", {{"id", "bias"}, {"op", "graph.input"}}}};
    const auto supply = [](const char *instance) {
        return Json{{"op", "set_input"}, {"node", instance}, {"port", "bias"},
                    {"source", {{"node", "two"}, {"port", "value"}}}};
    };
    const auto declared_first = Pelican::editGraphDocument(document, Json::array({
        declare, supply("left"), supply("right")}));
    const auto supplied_first = Pelican::editGraphDocument(document, Json::array({
        supply("left"), supply("right"), declare}));
    CHECK(supplied_first == declared_first);
    const auto expanded = Pelican::expandGraphDocument(supplied_first);
    CHECK(expanded.instance_inputs.at("left/bias").source ==
          Pelican::GraphInstanceInputSource::connected);
    // The same for stored values.
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_node_parameters"}, {"node", "left"}, {"parameters", {{"gain", 2}, {"bias", 1}}}},
        Json{{"op", "set_node_parameters"}, {"node", "right"}, {"parameters", {{"gain", 4}, {"bias", 1}}}},
        declare})) == "<accepted>");
    // One half alone is refused as it always was.
    CHECK(lp0cEditCode(document, Json::array({supply("left")})) == "graph.input_unknown@1");
    CHECK(lp0cEditDetail(document, Json::array({supply("left")})) ==
          Json{{"node", "left"}, {"definition", "scaled_curve"}, {"port", "bias"}});
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_node_parameters"}, {"node", "left"}, {"parameters", {{"bias", 1}}}}})) ==
          "graph.input_unknown@1");
    CHECK_THROWS_AS(Pelican::editGraphDocument(document, Json::array({declare})),
                    std::invalid_argument); // an unsupplied input without a default
    // Clearing a port the definition never had is still a typo, named.
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_input"}, {"node", "left"}, {"port", "bias"}, {"source", nullptr}}})) ==
          "graph.input_unknown@1");
    // Taking the port out again, in either order.
    const Json undeclare{{"op", "remove_node"}, {"scope", "scaled_curve"}, {"node", "bias"}};
    const auto unsupply = [](const char *instance) {
        return Json{{"op", "set_input"}, {"node", instance}, {"port", "bias"}, {"source", nullptr}};
    };
    CHECK(Pelican::editGraphDocument(declared_first, Json::array({
        unsupply("left"), unsupply("right"), undeclare})) == document);
    CHECK(Pelican::editGraphDocument(declared_first, Json::array({
        undeclare, unsupply("left"), unsupply("right")})) == document);
    CHECK(lp0cEditCode(declared_first, Json::array({undeclare})) == "graph.input_referenced@1");
    CHECK(lp0cEditDetail(declared_first, Json::array({undeclare})) ==
          Json{{"definition", "scaled_curve"}, {"input", "bias"}, {"instance", "left"}});
    // A port declared again by the end of the list is not lost.
    CHECK(lp0cEditCode(declared_first, Json::array({undeclare, declare})) == "<accepted>");
    // An op refused for its own reason still names the instance first, as it
    // did when the check ran in place: `gain` is also wired inside the body.
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "remove_node"}, {"scope", "scaled_curve"}, {"node", "gain"}}})) ==
          "graph.input_referenced@1");
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_input"}, {"node", "left"}, {"port", "absent"},
             {"source", {{"node", "two"}}}}})) == "graph.input_unknown@1");
}

TEST_CASE("LP-0c definition ops are refused by name",
          "[curve][graph-composition][graph-definition-edit]") {
    using Json = nlohmann::json;
    const auto document = parameterGraphCompositionFixture();
    const auto one = [](Json op) { return Json::array({std::move(op)}); };
    const auto add = one({{"op", "add_definition"}, {"name", "scaled_curve"}});
    CHECK(lp0cEditCode(document, add) == "graph.definition_duplicate@1");
    CHECK(lp0cEditDetail(document, add) == Json{{"definition", "scaled_curve"}});
    auto two = document;
    two["definitions"]["other"] = {{"nodes", Json::array()}, {"outputs", Json::object()}};
    const auto onto = one({{"op", "rename_definition"}, {"from", "other"}, {"to", "scaled_curve"}});
    CHECK(lp0cEditCode(two, onto) == "graph.definition_duplicate@1");
    CHECK(lp0cEditDetail(two, onto) == Json{{"definition", "scaled_curve"}});
    const auto remove = one({{"op", "remove_definition"}, {"name", "absent"}});
    CHECK(lp0cEditCode(document, remove) == "graph.definition_missing@1");
    CHECK(lp0cEditDetail(document, remove) == Json{{"definition", "absent"}});
    const auto rename = one({{"op", "rename_definition"}, {"from", "absent"}, {"to", "x"}});
    CHECK(lp0cEditCode(document, rename) == "graph.definition_missing@1");
    CHECK(lp0cEditDetail(document, rename) == Json{{"definition", "absent"}});
    // definition_referenced is covered with its referrers above.

    // A body that arrives whole obeys the rules its nodes would one at a time.
    CHECK(lp0cEditCode(document, one({{"op", "add_definition"}, {"name", "nest"},
        {"definition", {{"nodes", Json::array({Json{{"id", "inner"}, {"op", "graph.instance"},
            {"definition", "scaled_curve"}}})}, {"outputs", Json::object()}}}})) ==
          "graph.nested_instance@1");
    CHECK(lp0cEditCode(document, one({{"op", "add_definition"}, {"name", "twice"},
        {"definition", {{"nodes", Json::array({Json{{"id", "a"}, {"op", "graph.input"}},
            Json{{"id", "a"}, {"op", "graph.input"}}})}, {"outputs", Json::object()}}}})) ==
          "graph.node_duplicate@1");
    // Names follow the node id rule, and "root" names the root scope.
    for (const auto &name : Json::array({"", "a/b", 3, nullptr, "root"})) {
        CAPTURE(name);
        CHECK_THROWS_AS(Pelican::editGraphDocument(document, one({{"op", "add_definition"},
            {"name", name}})), std::invalid_argument);
        CHECK_THROWS_AS(Pelican::editGraphDocument(document, one({{"op", "rename_definition"},
            {"from", "scaled_curve"}, {"to", name}})), std::invalid_argument);
    }
    for (const auto &body : Json::array({Json::array(), Json{{"nodes", Json::array()}},
             Json{{"outputs", Json::object()}}, Json{{"nodes", Json::object()}, {"outputs", Json::object()}}})) {
        CAPTURE(body);
        CHECK_THROWS_AS(Pelican::editGraphDocument(document, one({{"op", "add_definition"},
            {"name", "shape"}, {"definition", body}})), std::invalid_argument);
    }
    // They act on the document, not inside a body.
    CHECK_THROWS_AS(Pelican::editGraphDocument(document, one({{"op", "add_definition"},
        {"name", "x"}, {"scope", "scaled_curve"}})), std::invalid_argument);
    CHECK(lp0cEditCode(document, one({{"op", "add_definition"}, {"name", "x"}, {"scope", "root"}})) ==
          "<accepted>");
}

TEST_CASE("mixed graph keeps one document for scalar and render execution", "[curve][graph-composition]") {
    auto document = mixedGraphCompositionFixture();
    const Pelican::ValueGraph::Graph graph{document.dump()};
    REQUIRE(nlohmann::json::parse(graph.serialize()) == document);
    const auto values = graph.evaluate({{"time", 0.25}});
    REQUIRE(values.size() == 2);
    REQUIRE(values.at("left")[0] == Approx(0.5));
    const auto rendering = Pelican::lowerRenderGraphDocument(document);
    REQUIRE(rendering.at("buffers")[0].at("name") == "render");
    REQUIRE(rendering.at("buffers")[0].at("size") == 4);
    REQUIRE(rendering.at("buffers")[0].at("host_source") == "runtime_f32");
    document["nodes"][5]["parameters"]["element"] = "uint";
    REQUIRE_THROWS_AS(Pelican::ValueGraph::Graph(document.dump()), std::invalid_argument);
    REQUIRE_THROWS_AS(Pelican::lowerRenderGraphDocument(document), std::invalid_argument);
}

TEST_CASE("common graph contracts check types across instance boundaries", "[curve][graph-composition]") {
    using namespace Pelican;
    auto document = nlohmann::json::parse(graphCompositionFixture);
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto scalar = registry.canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const GraphInputContract input{exactLogicalTypePattern(registry, scalar)};
    const std::map<std::string, LogicalType> outputs{{"value", scalar}};
    GraphOperationContracts contracts{
        {"value.input", {{}, outputs}}, {"value.constant", {{}, outputs}},
        {"value.curve", {{{"time", input}}, outputs}},
        {"value.multiply", {{{"a", input}, {"b", input}}, outputs}},
        {"render.scalar", {{{"value", input}}, {}}},
    };
    REQUIRE(validateGraphPorts(expandGraphDocument(document), registry, contracts).at("left") == scalar);
    // Only one instance receives this producer. Matching port names must not let
    // an image through its scalar input; removing edge type matching loses this check.
    document["nodes"][1]["op"] = "test.image";
    contracts.emplace("test.image", GraphOperationContract{{}, {{"value", sceneLinearHdrV1(registry)}}});
    try {
        validateGraphPorts(expandGraphDocument(document), registry, contracts);
        FAIL("image connected to scalar input was accepted");
    } catch (const std::invalid_argument &error) {
        const std::string message = error.what();
        REQUIRE(message.find("left/scaled.b") != std::string::npos);
        REQUIRE(message.find("two.value") != std::string::npos);
    }
    document = nlohmann::json::parse(graphCompositionFixture);
    document["nodes"][2]["outputs"].push_back("unused");
    REQUIRE_THROWS_AS(ValueGraph::Graph(document.dump()), std::invalid_argument);
    document = nlohmann::json::parse(graphCompositionFixture);
    document["definitions"]["scaled_curve"]["nodes"][3]["inputs"].erase("b");
    REQUIRE_THROWS_AS(ValueGraph::Graph(document.dump()), std::invalid_argument);
    document = nlohmann::json::parse(graphCompositionFixture);
    document["nodes"].push_back({{"id", "play"}, {"op", "value.playback"},
        {"inputs", {{"elapsed", {{"node", "clock"}, {"port", "value"}}},
                    {"duration", {{"node", "two"}, {"port", "value"}}}}},
        {"outputs", {"value"}}});
    document["outputs"]["play"] = {{"node", "play"}, {"port", "value"}};
    REQUIRE(ValueGraph::Graph(document.dump()).evaluate({{"time", 0.5}}).at("play")[0] == Approx(0.25));
    // A parameter string must not create a dependency invisible to port checking.
    document["nodes"].back()["inputs"].erase("duration");
    document["nodes"].back()["parameters"] = {{"duration", "two"}};
    // CT-3: a duration is a number; named from the parameter table.
    REQUIRE_THROWS_WITH(ValueGraph::Graph(document.dump()),
                        Catch::Matchers::ContainsSubstring("graph.render_node_parameter_invalid@1") &&
                            Catch::Matchers::ContainsSubstring("\"parameter\":\"duration\""));
}

TEST_CASE("common graph instances share definitions but preserve scope and connections", "[curve][graph-composition]") {
    const auto document = nlohmann::json::parse(graphCompositionFixture);
    const auto expanded = Pelican::expandGraphDocument(document);
    REQUIRE(expanded.origins.at("left/shape").instance_path == "left/");
    REQUIRE(expanded.origins.at("right/shape").definition == "scaled_curve");
    const Pelican::ValueGraph::Graph graph{document.dump()};
    REQUIRE(nlohmann::json::parse(graph.serialize()) == document);
    const auto output = Pelican::ValueGraph::Graph{graph.serialize()}.evaluate({{"time", 0.25}});
    REQUIRE(output.size() == 2);
    REQUIRE(output.at("left")[0] == Approx(0.5));
    REQUIRE(output.at("right")[0] == Approx(1.0));
    auto edited = document;
    edited["nodes"][1]["parameters"]["value"] = 3;
    const auto changed = Pelican::ValueGraph::Graph{edited.dump()}.evaluate({{"time", 0.25}});
    REQUIRE(changed.at("left")[0] == Approx(0.75));
    REQUIRE(changed.at("right")[0] == Approx(1.0));
    auto invalid = document;
    invalid["nodes"][3]["inputs"].erase("gain");
    REQUIRE_THROWS(Pelican::expandGraphDocument(invalid));
    invalid = document;
    invalid["nodes"][5]["inputs"]["value"]["port"] = "missing";
    REQUIRE_THROWS(Pelican::expandGraphDocument(invalid));
}

namespace {
// Operations and a type that exist only in the test-only module. Expanded nodes
// are visited in id order, so "emit" is the first one a registry resolves.
nlohmann::json opaqueModuleDocument() {
    return nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "nodes":[{"id":"emit","op":"test.emit","outputs":["token"]},
               {"id":"use","op":"test.consume","outputs":[],
                "inputs":{"token":{"node":"emit","port":"token"}}}],
      "outputs":{}})");
}
}

TEST_CASE("a module brings its own type and operations to the common graph",
          "[curve][graph-composition][graph-module]") {
    using namespace Pelican;
    const auto document = opaqueModuleDocument();
    auto registered = makeBuiltinGraphModuleRegistry();
    registerTestOpaqueGraphModule(registered);
    REQUIRE(registered.typeRegistry().contains(parseSemanticTypeId("pelican.test.token@1")));
    const auto partitions = partitionGraphDocument(document, registered);
    REQUIRE(partitions.values.nodes.size() == 2);
    REQUIRE(partitions.rendering.nodes.empty());
    // Purging the module leaves the document rejected by name, not by prose and
    // not silently.
    try {
        partitionGraphDocument(document, makeBuiltinGraphModuleRegistry());
        FAIL("an unregistered operation was accepted");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unregistered@1");
        REQUIRE(error.detail().at("op") == "test.emit");
        REQUIRE(error.detail().at("node") == "emit");
    }
    // Registering the operations without their type is a different name.
    auto without_type = makeBuiltinGraphModuleRegistry();
    registerTestOpaqueGraphModule(without_type, TestOpaqueModuleTypes::omitted);
    try {
        partitionGraphDocument(document, without_type);
        FAIL("an unregistered output type was accepted");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.type_unregistered@1");
        REQUIRE(error.detail().at("type") == "pelican.test.token@1");
        REQUIRE(error.detail().at("node") == "emit");
    }
    // The test module is not global state: production keeps its own registry.
    REQUIRE_NOTHROW(partitionGraphDocument(nlohmann::json::parse(graphCompositionFixture)));
}

TEST_CASE("the registering module decides which domain evaluates an operation",
          "[curve][graph-composition][graph-module]") {
    using namespace Pelican;
    const auto operations = [](const ExpandedGraphDocument &partition) {
        std::set<std::string> result;
        for (const auto &node : partition.nodes) result.insert(node.at("op").get<std::string>());
        return result;
    };
    // The publication sink is a pelican.render operation that runs with the
    // values it uploads, so the partition follows the declaration and not the
    // operation's name.
    const auto scalars = partitionGraphDocument(nlohmann::json::parse(graphCompositionFixture));
    REQUIRE(operations(scalars.values).contains("render.scalar"));
    REQUIRE(scalars.rendering.nodes.empty());
    const auto mixed = partitionGraphDocument(mixedGraphCompositionFixture());
    REQUIRE(operations(mixed.rendering) ==
            std::set<std::string>{"render.buffer", "render.compute"});
    REQUIRE(operations(mixed.values) ==
            std::set<std::string>{"value.constant", "value.curve", "value.input",
                                  "value.multiply", "render.scalar"});
}

TEST_CASE("one operation has exactly one registering module",
          "[curve][graph-composition][graph-module]") {
    using namespace Pelican;
    // Every operation the retired static contract table listed. Reviving that
    // table as a second registration is a named rejection, so the registry
    // cannot end up with two sources of truth for one operation.
    std::vector<std::pair<std::string, std::string>> retired_static_table{
        {"value.input", "pelican.value"}, {"value.constant", "pelican.value"},
        {"value.parameter", "pelican.value"}, {"value.morph_weight", "pelican.value"},
        {"value.trigger_clock", "pelican.value"}, {"event.playback", "pelican.value"},
        {"value.event_clock", "pelican.value"}, {"value.add", "pelican.value"},
        {"value.multiply", "pelican.value"}, {"value.curve", "pelican.value"},
        {"value.playback", "pelican.value"}, {"render.scalar", "pelican.render"}};
#if PELICAN_WITH_AUDIO
    retired_static_table.insert(retired_static_table.end(),
        {{"audio.playback", "pelican.audio"}, {"audio.volume", "pelican.audio"},
         {"audio.pitch", "pelican.audio"}});
#endif
    for (const auto &[op, owner] : retired_static_table) {
        auto registry = makeBuiltinGraphModuleRegistry();
        const GraphModuleManifest revived{"pelican.retired_static_table", 1,
            "the contract table this registry replaced", {},
            {{op, GraphOperationRegistration{}}}, {}};
        try {
            registry.registerModule(revived);
            FAIL("a second module registered " + op);
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "graph.operation_duplicate@1");
            REQUIRE(error.detail().at("op") == op);
            REQUIRE(error.detail().at("first_module") == owner);
            REQUIRE(error.detail().at("second_module") == "pelican.retired_static_table");
        }
        // A rejected manifest leaves no half-registered module behind.
        REQUIRE_FALSE(registry.contains("test.emit"));
        REQUIRE_NOTHROW(registerTestOpaqueGraphModule(registry));
    }
    auto registry = makeBuiltinGraphModuleRegistry();
    registerTestOpaqueGraphModule(registry);
    try {
        registerTestOpaqueGraphModule(registry);
        FAIL("a namespace was registered twice");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.module_duplicate@1");
        REQUIRE(error.detail().at("namespace") == "pelican.test.opaque");
    }
    // Discovery does not depend on registration order: the module registered
    // last still appears in namespace order.
    std::vector<std::string> namespaces;
    const auto catalog = registry.catalog();
    for (const auto &module : catalog.at("modules"))
        namespaces.push_back(module.at("namespace").get<std::string>());
    std::vector<std::string> expected{"pelican.render", "pelican.test.opaque",
                                      "pelican.ui", "pelican.value"};
#if PELICAN_WITH_ANIMATION
    expected.insert(expected.begin(), "pelican.pose");
#endif
#if PELICAN_WITH_AUDIO
    expected.insert(expected.begin(), "pelican.audio");
#endif
    REQUIRE(namespaces == expected);
}

// CT-1: the catalog Studio reads (get_graph_catalog) says what each op cannot
// do without and which names value.input takes, from the tables the engine
// checks with: resolveOperation's required parameters, and the names the
// frame binds (validateGraphInputs).
TEST_CASE("CT-1 the catalog lists each op's required parameters and the names value.input takes",
          "[curve][graph-module][graph-catalog][ct1]") {
    using namespace Pelican;
    using Json = nlohmann::json;
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    const auto operation = [&](std::string_view op) {
        for (const auto &module : catalog.at("modules"))
            for (const auto &entry : module.at("operations"))
                if (entry.at("op").get<std::string>() == std::string{op}) return entry;
        FAIL("the catalog lists no " << op);
        return Json{};
    };
    // A single name, or alternatives of which one is enough; nothing for an
    // op that requires nothing.
    CHECK(operation("render.scalar").at("required_parameters") == Json::array({Json{{"name", "target"}}}));
    CHECK(operation("render.compute").at("required_parameters") ==
          Json::array({Json{{"name", "ports"}}, Json{{"one_of", Json::array({"shader", "ray_tracing"})}}}));
    CHECK(operation("value.add").at("required_parameters") == Json::array());
#if PELICAN_WITH_AUDIO
    CHECK(operation("audio.volume").at("required_parameters") ==
          Json::array({Json{{"one_of", Json::array({"target", "voice"})}}}));
#endif
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations")) {
            INFO(entry.at("op"));
            CHECK(entry.at("required_parameters").is_array());
            CHECK(entry.contains("input_names") == (entry.at("op") == "value.input"));
        }

    // value.input: every name it takes, each a scalar.
    const auto input = operation("value.input");
    CHECK(input.at("required_parameters") == Json::array({Json{{"name", "name"}}}));
    std::vector<std::string> names;
    for (const auto &entry : input.at("input_names")) {
        INFO(entry.dump());
        names.push_back(entry.at("name").get<std::string>());
        CHECK(entry.at("type").at("semantic") == "pelican.value.scalar@1");
        CHECK_FALSE(entry.at("description").get<std::string>().empty());
    }
    CHECK(names == std::vector<std::string>{"time", "delta_time", "scene_time"});
    // Each listed name is one the frame binds; any other is refused as before.
    const auto graph = [](const std::string &name) {
        return ValueGraph::Graph{Json{{"schema", "pelican.graph"}, {"version", 1},
            {"nodes", Json::array({Json{{"id", "clock"}, {"op", "value.input"},
                                        {"parameters", {{"name", name}}}, {"outputs", Json::array({"value"})}}})},
            {"outputs", {{"clock", {{"node", "clock"}, {"port", "value"}}}}}}.dump()};
    };
    for (const auto &name : names) {
        INFO(name);
        CHECK_NOTHROW(validateGraphInputs(graph(name)));
    }
    CHECK_THROWS_WITH(validateGraphInputs(graph("frame_count")),
                      Catch::Matchers::ContainsSubstring("unbound frame graph input: frame_count"));
}

// CT-3: the catalog says what each op's parameters hold -- kind, default,
// enum, range, nested fields -- read off the parser that reads them, and
// resolveOperation refuses a value of the wrong kind by name. The table is
// the one RS-9's required parameters are read from, so required_parameters
// is what it was.
TEST_CASE("CT-3 the catalog lists each op's parameters with their kinds and a wrong value is refused by name",
          "[curve][graph-module][graph-catalog][ct3]") {
    using namespace Pelican;
    using Json = nlohmann::json;
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    const auto operation = [&](std::string_view op) {
        for (const auto &module : catalog.at("modules"))
            for (const auto &entry : module.at("operations"))
                if (entry.at("op").get<std::string>() == std::string{op}) return entry;
        FAIL("the catalog lists no " << op);
        return Json{};
    };
    const auto parameter = [&](std::string_view op, std::string_view name) {
        const auto listed = operation(op);
        for (const auto &entry : listed.at("parameters"))
            if (entry.at("name").get<std::string>() == std::string{name}) return entry;
        FAIL(op << " lists no parameter " << name);
        return Json{};
    };
    const auto names = [&](std::string_view op) {
        std::vector<std::string> result;
        const auto listed = operation(op);
        for (const auto &entry : listed.at("parameters")) result.push_back(entry.at("name").get<std::string>());
        return result;
    };

    // RS-9's table before CT-3, which the schema's required entries replace,
    // and the six the pose and draw.variant parsers required by themselves.
    const std::map<std::string, Json> rs9{
        {"render.fullscreen", Json::array({Json{{"name", "ports"}}, Json{{"name", "shader"}}})},
        {"render.raster", Json::array({Json{{"name", "ports"}}, Json{{"name", "shader"}}, Json{{"name", "draw"}}})},
        {"render.compute", Json::array({Json{{"name", "ports"}},
                                        Json{{"one_of", Json::array({"shader", "ray_tracing"})}}})},
        {"render.material_pass", Json::array({Json{{"name", "material_contract"}}})},
        {"render.snapshot_copy", Json::array({Json{{"name", "snapshot_after"}}})},
        {"render.target_ref", Json::array({Json{{"name", "name"}}, Json{{"name", "type"}}})},
        {"render.image", Json::array({Json{{"name", "type"}}})},
        {"render.buffer", Json::array({Json{{"name", "element"}}})},
        {"render.attribute", Json::array({Json{{"name", "element"}}, Json{{"name", "domain"}},
                                          Json{{"one_of", Json::array({"count", "source"})}}})},
        {"render.scalar", Json::array({Json{{"name", "target"}}})},
        {"value.input", Json::array({Json{{"name", "name"}}})},
        {"value.constant", Json::array({Json{{"name", "value"}}})},
        {"value.parameter", Json::array({Json{{"name", "name"}}, Json{{"name", "default"}}})},
        {"value.morph_weight", Json::array({Json{{"name", "node"}}, Json{{"name", "target"}}})},
        {"value.curve", Json::array({Json{{"name", "keys"}}})},
        // AB-1, after CT-3.
        {"value.sample_table", Json::array({Json{{"name", "asset"}}, Json{{"name", "row"}}})},
        {"audio.playback", Json::array({Json{{"name", "asset"}}})},
        {"audio.volume", Json::array({Json{{"one_of", Json::array({"target", "voice"})}}})},
        {"audio.pitch", Json::array({Json{{"one_of", Json::array({"target", "voice"})}}})},
        {"ui.value", Json::array({Json{{"name", "node"}}})},
        {"ui.visible", Json::array({Json{{"name", "node"}}})},
        {"pose.clip_sample", Json::array({Json{{"name", "rig"}}, Json{{"name", "clip"}}})},
        {"pose.state_machine", Json::array({Json{{"name", "rig"}}, Json{{"name", "graph"}}})},
        {"pose.retarget", Json::array({Json{{"name", "profile"}}})},
        {"draw.variant", Json::array({Json{{"name", "variant"}}})},
    };
    // CT-4 added union.
    const std::set<std::string> kinds{"number", "integer", "bool", "string", "enum", "vec2", "vec3", "vec4",
                                      "color", "array", "object", "asset", "shader", "node_ref", "expression", "json",
                                      "union"};
    std::function<void(const Json &)> checkShape = [&](const Json &entry) {
        INFO(entry.dump());
        CHECK(kinds.contains(entry.at("kind").get<std::string>()));
        CHECK(entry.contains("enum") == (entry.at("kind") == "enum"));
        if (entry.contains("items")) checkShape(entry.at("items"));
        for (const auto &field : entry.value("fields", Json::array())) checkShape(field);
        for (const auto &variant : entry.value("variants", Json::array())) checkShape(variant);
    };
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations")) {
            const auto op = entry.at("op").get<std::string>();
            INFO(op);
            REQUIRE(entry.at("parameters").is_array());
            CHECK(entry.at("parameters_complete").is_boolean());
            // required_parameters is the parameters' required entries, and
            // what RS-9 required before.
            auto required = Json::array();
            for (const auto &listed : entry.at("parameters")) {
                checkShape(listed);
                if (!listed.value("required", false)) {
                    CHECK_FALSE(listed.contains("alternatives"));
                    continue;
                }
                if (!listed.contains("alternatives")) {
                    required.push_back(Json{{"name", listed.at("name")}});
                    continue;
                }
                auto one_of = Json::array({listed.at("name")});
                for (const auto &alternative : listed.at("alternatives")) one_of.push_back(alternative);
                required.push_back(Json{{"one_of", std::move(one_of)}});
            }
            CHECK(entry.at("required_parameters") == required);
            const auto before = rs9.find(op);
            CHECK(entry.at("required_parameters") == (before != rs9.end() ? before->second : Json::array()));
        }

    // The parameters the GUI asked for, with their kinds, enums and defaults.
#if PELICAN_WITH_ANIMATION
    CHECK(names("pose.clip_sample") ==
          std::vector<std::string>{"rig", "clip", "speed", "start_time", "loop", "root_motion", "root_node"});
    CHECK(operation("pose.clip_sample").at("parameters_complete") == true);
    CHECK(parameter("pose.clip_sample", "rig").at("kind") == "asset");
    CHECK(parameter("pose.clip_sample", "rig").at("required") == true);
    CHECK(parameter("pose.clip_sample", "clip").at("kind") == "string");
    CHECK(parameter("pose.clip_sample", "clip").at("required") == true);
    CHECK(parameter("pose.clip_sample", "speed") == Json{{"name", "speed"}, {"kind", "number"}, {"default", 1.0}});
    CHECK(parameter("pose.clip_sample", "start_time").at("default") == 0.0);
    CHECK(parameter("pose.clip_sample", "loop") == Json{{"name", "loop"}, {"kind", "bool"}, {"default", true}});
    CHECK(parameter("pose.clip_sample", "root_motion").at("enum") == Json::array({"none", "extract"}));
    CHECK(parameter("pose.clip_sample", "root_motion").at("default") == "none");
#endif
    CHECK(parameter("render.buffer", "element").at("required") == true);
    CHECK(parameter("render.buffer", "element").at("enum").size() == 13);
    CHECK(parameter("render.buffer", "size").at("kind") == "integer");
    CHECK(parameter("render.buffer", "size").at("min") == 0);
    CHECK(parameter("render.buffer", "lifetime").at("enum") == Json::array({"persistent", "transient"}));
    CHECK(parameter("render.buffer", "lifetime").at("default") == "persistent");
    CHECK(parameter("render.buffer", "host_source").at("enum").front() == "runtime_f32");
    CHECK(parameter("render.attribute", "count").at("alternatives") == Json::array({"source"}));
    CHECK(parameter("render.attribute", "lifetime").at("default") == "persistent");
    CHECK(operation("render.buffer").at("parameters_complete") == true);
    const auto shader = parameter("render.shadow_depth", "shader");
    CHECK(shader.at("kind") == "shader");
    CHECK(shader.at("fields") == Json::array({Json{{"name", "vertex"}, {"kind", "string"}, {"required", true}}}));
    CHECK(shader.at("default") == Json{{"vertex", "engine://shadow_depth"}});
    CHECK(parameter("render.shadow_depth", "ray_query").at("kind") == "union"); // CT-4: was json
    CHECK(parameter("render.shadow_depth", "depth_load_op").at("kind") == "enum");
    CHECK(parameter("render.shadow_depth", "depth_store_op").at("enum").front() == "store");
    CHECK(parameter("render.shadow_depth", "resolution_domain").at("default") == "independent");
    CHECK(parameter("render.shadow_depth", "view_select").at("items").at("expression") == true);
    CHECK(operation("render.shadow_depth").at("parameters_complete") == true);
    CHECK(names("value.trigger_clock") == std::vector<std::string>{"paused", "reset"});
    CHECK(parameter("value.trigger_clock", "paused").at("kind") == "number");
    CHECK(parameter("value.trigger_clock", "reset").at("default") == 0.0);
    // Nested: an image's type, its initial asset; incomplete ops say so.
    const auto type = parameter("render.image", "type");
    CHECK(type.at("kind") == "object");
    CHECK(type.at("fields").at(0).at("name") == "semantic");
    CHECK(type.at("fields").at(1).at("items").at("kind") == "string");
    CHECK(parameter("render.image", "initial").at("fields").at(0) ==
          Json{{"name", "asset"}, {"kind", "asset"}, {"required", true},
               // CT-4.
               {"asset_kind", "image"}, {"extensions", Json::array({".ktx2", ".png", ".jpg"})}});
    CHECK(parameter("render.image", "width").at("expression") == true);
    CHECK(operation("render.image").at("parameters_complete") == false);
    CHECK(operation("render.fullscreen").at("parameters_complete") == false);

    // A value of the wrong kind, outside its enum or its range is refused by
    // name before the op's parser reads it.
    const auto refusal = [](Json node) {
        try {
            (void)builtinGraphModuleRegistry().resolveOperation(node);
        } catch (const GraphRegistryError &error) {
            return std::pair{error.code(), error.detail()};
        }
        return std::pair{std::string{}, Json{}};
    };
    auto [code, detail] = refusal({{"id", "c"}, {"op", "value.constant"}, {"parameters", {{"value", "loud"}}}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail == Json{{"node", "c"}, {"op", "value.constant"}, {"parameter", "value"}, {"expected", "number"},
                         {"reason", "type"}});
    std::tie(code, detail) = refusal(
        {{"id", "data"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}, {"lifetime", "forever"}}}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail.at("parameter") == "lifetime");
    CHECK(detail.at("expected") == "enum");
    CHECK(detail.at("reason") == "enum");
    CHECK(detail.at("allowed") == Json::array({"persistent", "transient"}));
    std::tie(code, detail) = refusal(
        {{"id", "w"}, {"op", "value.morph_weight"}, {"parameters", {{"node", -1}, {"target", 0}}}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail.at("parameter") == "node");
    CHECK(detail.at("reason") == "range");
    CHECK(detail.at("min") == 0);
    std::tie(code, detail) = refusal(
        {{"id", "k"}, {"op", "value.curve"},
         {"parameters", {{"keys", Json::array({Json{{"time", 0}, {"value", 1}, {"interpolation", "smooth"}}})}}}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail.at("parameter") == "keys[0].interpolation");
    CHECK(detail.at("reason") == "enum");
    std::tie(code, detail) = refusal(
        {{"id", "shadow"}, {"op", "render.shadow_depth"}, {"parameters", {{"shader", "shaders/depth.vert"}}}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail.at("parameter") == "shader");
    CHECK(detail.at("expected") == "shader");
    CHECK(detail.at("reason") == "type");
    // RS-9 still names what is missing first.
    std::tie(code, detail) = refusal({{"id", "c"}, {"op", "value.constant"}, {"parameters", Json::object()}});
    CHECK(code == "graph.render_node_parameters_missing@1");
    // What the pose and draw.variant parsers require is RS-9's too.
    std::tie(code, detail) = refusal({{"id", "v"}, {"op", "draw.variant"}});
    CHECK(code == "graph.render_node_parameters_missing@1");
    CHECK(detail == Json{{"node", "v"}, {"op", "draw.variant"}, {"missing", Json::array({"variant"})}});
#if PELICAN_WITH_ANIMATION
    std::tie(code, detail) =
        refusal({{"id", "walk"}, {"op", "pose.clip_sample"}, {"parameters", {{"clip", "Walk"}}}});
    CHECK(code == "graph.render_node_parameters_missing@1");
    CHECK(detail.at("missing") == Json::array({"rig"}));
    std::tie(code, detail) = refusal({{"id", "brain"}, {"op", "pose.state_machine"}});
    CHECK(detail.at("missing") == Json::array({"rig", "graph"}));
    std::tie(code, detail) = refusal({{"id", "profile"}, {"op", "pose.retarget"}, {"parameters", Json::object()}});
    CHECK(detail.at("missing") == Json::array({"profile"}));
#endif
    // A loop body's expression, JSON null and a key the table does not name
    // are left to the op's own parser, as before.
    std::tie(code, detail) = refusal(
        {{"id", "shadow"}, {"op", "render.shadow_depth"},
         {"parameters", {{"view_select", {{"cascade", {{"$expr", "element"}}}}}, {"pass_set", nullptr}}}});
    CHECK(code.empty());
    std::tie(code, detail) = refusal(
        {{"id", "data"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}, {"note", 3}}}});
    CHECK(code.empty());
}

// CT-4: what Studio needs to make a field of each parameter -- where a
// string's value is chosen from, what an asset is, the shapes of a value of
// two kinds, a slider's practical range -- and where an op may stand, which
// the partition enforces by name. Nothing CT-3 checks changes.
TEST_CASE("CT-4 the catalog says where a value is chosen from, an asset's kind, a union's shapes, a slider's range "
          "and where an op may stand",
          "[curve][graph-module][graph-catalog][ct4]") {
    using namespace Pelican;
    using Json = nlohmann::json;
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    const auto operation = [&](std::string_view op) {
        for (const auto &module : catalog.at("modules"))
            for (const auto &entry : module.at("operations"))
                if (entry.at("op").get<std::string>() == std::string{op}) return entry;
        FAIL("the catalog lists no " << op);
        return Json{};
    };
    const auto parameter = [&](std::string_view op, std::string_view name) {
        const auto listed = operation(op);
        for (const auto &entry : listed.at("parameters"))
            if (entry.at("name").get<std::string>() == std::string{name}) return entry;
        FAIL(op << " lists no parameter " << name);
        return Json{};
    };
    const Json context{{"rpc", "get_render_authoring_context"}};
    const auto from = [&](const char *key, const char *field = nullptr) {
        auto result = context;
        result["key"] = key;
        if (field) result["field"] = field;
        return result;
    };

    // 1. Where a string's value is chosen from.
    // CT-5 adds `where` (the CT-5 case below).
    CHECK(parameter("render.scalar", "target").at("choices_from").at("graph_nodes") ==
          Json::array({"render.buffer", "render.attribute"}));
    CHECK(parameter("render.shadow_depth", "view_family").at("choices_from") == from("view_families"));
    CHECK(parameter("render.shadow_depth", "pass_set").at("choices_from") == from("graphs", "name"));
    CHECK(parameter("render.shadow_depth", "canonical_anchor").at("choices_from") == from("canonical_anchors"));
    CHECK(parameter("render.image", "format").at("choices_from") == from("render_target_vocabulary.formats"));
    CHECK(parameter("render.image", "format_class").at("choices_from") ==
          from("render_target_vocabulary.format_classes"));
    CHECK(parameter("render.target_ref", "name").at("choices_from") == from("render_targets", "name"));
    CHECK(parameter("render.fullscreen", "inherit_bindings_from").at("choices_from") ==
          from("graphs.passes", "name"));
    CHECK(parameter("draw.variant", "variant").at("choices_from") == from("materials", "variants"));
#if PELICAN_WITH_AUDIO
    CHECK(parameter("audio.volume", "target").at("choices_from") ==
          Json{{"graph_nodes", Json::array({"audio.playback"})}});
    CHECK(parameter("audio.playback", "asset").at("asset_kind") == "audio");
    CHECK(parameter("audio.playback", "asset").at("extensions") == Json::array({".wav", ".ogg"}));
#endif

    // 2. An asset's kind and the extensions its reader takes.
#if PELICAN_WITH_ANIMATION
    CHECK(parameter("pose.clip_sample", "rig").at("asset_kind") == "model");
    CHECK(parameter("pose.clip_sample", "rig").at("extensions") == Json::array({".glb", ".gltf", ".vrm"}));
#endif
    CHECK(parameter("value.sample_table", "asset").at("asset_kind") == "analysis_table");
    CHECK(parameter("render.attribute", "source").at("fields").at(0).at("extensions") ==
          Json::array({".glb", ".gltf"}));

    // 3. A value of two kinds is a union of its shapes.
    const auto ray_query = parameter("render.shadow_depth", "ray_query");
    CHECK(ray_query.at("kind") == "union");
    CHECK(ray_query.at("default") == false);
    CHECK(ray_query.at("variants") ==
          Json::array({Json{{"kind", "bool"}},
                       Json{{"kind", "object"},
                            {"fields", Json::array({Json{{"name", "acceleration_structure"}, {"kind", "string"},
                                                         {"required", true},
                                                         // CT-5.
                                                         {"choices_from", from("acceleration_structures")}}})}}}));
    const auto mip_levels = parameter("render.image", "mip_levels");
    CHECK(mip_levels.at("kind") == "union");
    CHECK(mip_levels.at("variants").at(0).at("kind") == "integer");
    CHECK(mip_levels.at("variants").at(0).at("min") == 1);
    CHECK(mip_levels.at("variants").at(1) == Json{{"kind", "enum"}, {"enum", Json::array({"full"})}});
    CHECK(parameter("value.sample_table", "row").at("kind") == "union");
    // Listed, not checked: the op's parser judges it, as it did a json value.
    CHECK_NOTHROW(checkGraphNodeParameterTypes(
        "render.shadow_depth",
        Json{{"id", "shadow"}, {"op", "render.shadow_depth"}, {"parameters", {{"ray_query", "yes"}}}}));

    // 5. A slider's range, never checked.
    CHECK(parameter("render.image", "width").at("soft_max") == 8192);
    CHECK(parameter("render.image", "height").at("soft_max") == 8192);
    CHECK(parameter("render.image", "width").at("max") == 4294967295LL);
    CHECK(mip_levels.at("variants").at(0).at("soft_max") == 14);
    CHECK_FALSE(parameter("render.image", "layers").contains("soft_max"));

    // 4. Where an op may stand: every op says so, and the pose ops and the
    // morph weight only inside an owned instance.
    std::set<std::string> owned;
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations")) {
            INFO(entry.at("op"));
            const auto placement = entry.at("placement").get<std::string>();
            CHECK((placement == "anywhere" || placement == "owned_instance"));
            if (placement == "owned_instance") owned.insert(entry.at("op").get<std::string>());
        }
    std::set<std::string> expected{"value.morph_weight"};
#if PELICAN_WITH_ANIMATION
    expected.insert({"pose.clip_sample", "pose.blend", "pose.state_machine", "pose.retarget", "pose.skeletal",
                     "pose.transform"});
#endif
    CHECK(owned == expected);
    const auto refusal = [](const Json &document) {
        try {
            (void)partitionGraphDocument(document);
        } catch (const GraphRegistryError &error) {
            return std::pair{error.code(), error.detail()};
        }
        return std::pair{std::string{}, Json{}};
    };
    // Each document has a clock at the root, which a clip sample's time reads.
    const auto rooted = [](Json node) {
        return Json{{"schema", "pelican.graph"}, {"version", 1},
                    {"nodes", Json::array({Json{{"id", "clock"}, {"op", "value.input"},
                                                {"parameters", {{"name", "time"}}},
                                                {"outputs", Json::array({"value"})}},
                                           std::move(node)})},
                    {"outputs", Json::object()}};
    };
    const Json clock{{"node", "clock"}, {"port", "value"}};
    auto [code, detail] = refusal(rooted({{"id", "weight"}, {"op", "value.morph_weight"},
                                          {"parameters", {{"node", 0}, {"target", 0}}},
                                          {"outputs", Json::array({"value"})}}));
    CHECK(code == "graph.op_requires_owned_instance@1");
    CHECK(detail == Json{{"node", "weight"}, {"op", "value.morph_weight"}});
#if PELICAN_WITH_ANIMATION
    std::tie(code, detail) = refusal(rooted({{"id", "walk"}, {"op", "pose.clip_sample"},
                                             {"parameters", {{"rig", "m.glb"}, {"clip", "Walk"}}},
                                             {"inputs", {{"time", clock}}}, {"outputs", Json::array({"pose"})}}));
    CHECK(code == "graph.op_requires_owned_instance@1");
    CHECK(detail == Json{{"node", "walk"}, {"op", "pose.clip_sample"}});
    // In an instance without an owner, by its expanded id.
    auto unowned = rooted({{"id", "hero"}, {"op", "graph.instance"}, {"definition", "pose"},
                           {"inputs", {{"time", clock}}}});
    unowned["definitions"]["pose"] = {
        {"nodes", Json::array({Json{{"id", "time"}, {"op", "graph.input"}},
                               Json{{"id", "walk"}, {"op", "pose.clip_sample"},
                                    {"parameters", {{"rig", "m.glb"}, {"clip", "Walk"}}},
                                    {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}}}},
                                    {"outputs", Json::array({"pose"})}}})},
        {"outputs", Json::object()}};
    std::tie(code, detail) = refusal(unowned);
    CHECK(code == "graph.op_requires_owned_instance@1");
    CHECK(detail.at("node") == "hero/walk");
    // With an owner it stands.
    unowned["nodes"][1]["owner"] = {{"scene", "scenes/main"}, {"object", 7}};
    std::tie(code, detail) = refusal(unowned);
    CHECK(code.empty());
#endif
}

// CT-5: the rest of the table's string choices -- a condition on the graph's
// nodes a render.scalar may write, the names an asset's manifest lists, the
// catalog's image types, the composed region tags and acceleration
// structures -- and a fullscreen pass's push_constants as the enum its
// parser reads. A render.scalar writing a buffer of the graph that is not
// runtime_f32, or not as large as its sinks, is refused by name when the
// graph is partitioned (every edit and every load).
TEST_CASE("CT-5 the catalog's remaining choices and a render.scalar writing an incompatible buffer",
          "[curve][graph-module][graph-catalog][ct5]") {
    using namespace Pelican;
    using Json = nlohmann::json;
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    const auto parameter = [&](std::string_view op, std::string_view name) {
        for (const auto &module : catalog.at("modules"))
            for (const auto &entry : module.at("operations"))
                if (entry.at("op").get<std::string>() == std::string{op})
                    for (const auto &listed : entry.at("parameters"))
                        if (listed.at("name").get<std::string>() == std::string{name}) return listed;
        FAIL(op << " lists no parameter " << name);
        return Json{};
    };
    const auto context = [](const char *key, const char *field = nullptr) {
        Json result{{"rpc", "get_render_authoring_context"}, {"key", key}};
        if (field) result["field"] = field;
        return result;
    };

    // 1. The buffers a render.scalar can write.
    CHECK(parameter("render.scalar", "target").at("choices_from") ==
          Json{{"graph_nodes", Json::array({"render.buffer", "render.attribute"})},
               {"where", {{"host_source", "runtime_f32"},
                          {"element", Json::array({"float", "vec2", "vec3", "vec4", "mat4"})}}}});
    // 5. Strings whose names a list holds.
    const auto acceleration_structure =
        Json{{"name", "acceleration_structure"}, {"kind", "string"}, {"required", true},
             {"choices_from", context("acceleration_structures")}};
    CHECK(parameter("render.fullscreen", "ray_query").at("variants").at(1).at("fields") ==
          Json::array({acceleration_structure}));
    CHECK(parameter("render.compute", "ray_query").at("fields") == Json::array({acceleration_structure}));
    CHECK(parameter("value.sample_table", "row").at("variants").at(1) ==
          Json{{"kind", "string"},
               {"choices_from", {{"asset_manifest", {{"parameter", "asset"},
                                                     {"manifest", "<asset>.import.json"},
                                                     {"key", "source.metadata.layout.features"}}}}}});
    CHECK(parameter("render.image", "type").at("fields").at(0).at("choices_from") ==
          Json{{"rpc", "get_graph_catalog"}, {"key", "modules.types"}, {"field", "id"},
               {"where", {{"constructor", "image"}}}});
    CHECK(parameter("render.target_ref", "type").at("fields").at(0).at("choices_from").at("key") == "modules.types");
    for (const auto *op : {"render.fullscreen", "render.raster", "render.material_pass", "render.compute"}) {
        INFO(op);
        CHECK(parameter(op, "region_tags").at("items").at("choices_from") ==
              context("graphs.passes.declaration", "regions"));
        CHECK(parameter(op, "regions").at("items").at("choices_from") ==
              context("graphs.passes.declaration", "regions"));
    }
    CHECK(parameter("render.shadow_depth", "region_tags").at("items").at("choices_from") ==
          context("graphs.passes.declaration", "regions"));
    const auto push_constants = parameter("render.fullscreen", "push_constants");
    CHECK(push_constants.at("kind") == "enum");
    CHECK(push_constants.at("enum") == Json::array({"none", "camera_position", "projection_view"}));
    CHECK(push_constants.at("default") == "none");
    // Not listed anywhere the engine reads: left a plain string.
#if PELICAN_WITH_ANIMATION
    CHECK_FALSE(parameter("pose.retarget", "profile").contains("choices_from"));
#endif
    CHECK_FALSE(parameter("draw.override", "defines").at("items").contains("choices_from"));
    CHECK(parameter("render.attribute", "source").at("fields").at(3).at("name") == "semantic");
    CHECK_FALSE(parameter("render.attribute", "source").at("fields").at(3).contains("choices_from"));
    CHECK_THROWS_AS(checkGraphNodeParameterTypes("render.fullscreen",
                        Json{{"id", "post"}, {"op", "render.fullscreen"},
                             {"parameters", {{"push_constants", "camera"}}}}),
                    GraphRegistryError);

    // 1. The refusal: a buffer of the graph a render.scalar writes.
    const auto document = [](Json buffer, Json value, const char *op = "render.buffer") {
        return Json{{"schema", "pelican.graph"}, {"version", 1},
                    {"nodes", Json::array({
                        Json{{"id", "data"}, {"op", op}, {"parameters", std::move(buffer)},
                             {"outputs", Json::array({"resource"})}},
                        Json{{"id", "write"}, {"op", "render.scalar"},
                             {"parameters", {{"target", "data"}, {"value", std::move(value)}}},
                             {"outputs", Json::array()}}})},
                    {"outputs", Json::object()}};
    };
    const auto refusal = [](const Json &authored) {
        try {
            (void)partitionGraphDocument(authored);
        } catch (const GraphRegistryError &error) {
            return std::pair{error.code(), error.detail()};
        }
        return std::pair{std::string{}, Json{}};
    };
    const Json pair = Json::array({1.0, 2.0});
    auto [code, detail] = refusal(document({{"element", "float"}, {"size", 8}}, pair));
    CHECK(code == "graph.render_scalar_target_incompatible@1");
    CHECK(detail == Json{{"node", "write"}, {"target", "data"},
                         {"expected", {{"host_source", "runtime_f32"}, {"size", 8}}},
                         {"actual", {{"host_source", nullptr}, {"size", 8}}}});
    std::tie(code, detail) =
        refusal(document({{"element", "float"}, {"size", 8}, {"host_source", "scene_lights_v2"}}, pair));
    CHECK(detail.at("actual").at("host_source") == "scene_lights_v2");
    std::tie(code, detail) =
        refusal(document({{"element", "vec4"}, {"size", 16}, {"host_source", "runtime_f32"}}, pair));
    CHECK(code == "graph.render_scalar_target_incompatible@1");
    CHECK(detail.at("expected") == Json{{"host_source", "runtime_f32"}, {"size", 8}});
    CHECK(detail.at("actual") == Json{{"host_source", "runtime_f32"}, {"size", 16}});
    // An attribute's size is its count times its element's stride.
    std::tie(code, detail) = refusal(document(
        {{"element", "float"}, {"domain", "vertex"}, {"count", 3}, {"host_source", "runtime_f32"}}, pair,
        "render.attribute"));
    CHECK(detail.at("actual") == Json{{"host_source", "runtime_f32"}, {"size", 12}});
    // As large as its sinks: taken.
    std::tie(code, detail) =
        refusal(document({{"element", "float"}, {"size", 8}, {"host_source", "runtime_f32"}}, pair));
    CHECK(code.empty());
    // An integer buffer stays graph.render_scalar_type_mismatch@1's (the
    // value graph's), and a buffer declared outside the graph the load's.
    std::tie(code, detail) = refusal(document({{"element", "uint"}, {"size", 4}}, pair));
    CHECK(code.empty());
    auto outside = document({{"element", "float"}, {"size", 8}}, pair);
    outside["nodes"][1]["parameters"]["target"] = "bloom.intensity";
    std::tie(code, detail) = refusal(outside);
    CHECK(code.empty());
}

// CT-6: render.compute's ray_tracing and dispatch as the task JSON parser
// reads them (computetaskdefinitionjson.cpp), so Studio makes a form of each:
// their fields, a wrong kind refused by name before the parser reads it, and
// the forms Studio, the engine's pipelines and the RT demos write still taken.
TEST_CASE("CT-6 the catalog lists render.compute's ray_tracing and dispatch as fields and its parameters as complete",
          "[curve][graph-module][graph-catalog][ct6]") {
    using namespace Pelican;
    using Json = nlohmann::json;
    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    Json compute;
    for (const auto &module : catalog.at("modules"))
        for (const auto &entry : module.at("operations"))
            if (entry.at("op") == "render.compute") compute = entry;
    REQUIRE(compute.is_object());
    const auto parameter = [&](std::string_view name) {
        for (const auto &listed : compute.at("parameters"))
            if (listed.at("name").get<std::string>() == std::string{name}) return listed;
        FAIL("render.compute lists no parameter " << name);
        return Json{};
    };
    const auto field = [](const Json &holder, std::string_view name) {
        for (const auto &listed : holder.at("fields"))
            if (listed.at("name").get<std::string>() == std::string{name}) return listed;
        FAIL("no field " << name);
        return Json{};
    };
    const auto names = [](const Json &holder) {
        std::vector<std::string> result;
        for (const auto &listed : holder.at("fields")) result.push_back(listed.at("name").get<std::string>());
        return result;
    };

    // 1. ray_tracing: both of the parser's key sets, and what either reads.
    CHECK(compute.at("parameters_complete") == true);
    const auto ray_tracing = parameter("ray_tracing");
    CHECK(ray_tracing.at("kind") == "object");
    CHECK_FALSE(ray_tracing.contains("required"));
    CHECK(names(ray_tracing) ==
          std::vector<std::string>{"raygen", "miss", "closesthit", "traced_ray_types", "misses", "hit_shaders",
                                   "callable", "callable_record", "callable_records", "max_recursion",
                                   "acceleration_structure"});
    CHECK(field(ray_tracing, "raygen").at("kind") == "shader");
    CHECK(field(ray_tracing, "raygen").at("required") == true);
    for (const auto *list : {"miss", "closesthit", "callable"}) {
        INFO(list);
        CHECK(field(ray_tracing, list).at("kind") == "union");
        CHECK(field(ray_tracing, list).at("variants") ==
              Json::array({Json{{"kind", "shader"}}, Json{{"kind", "array"}, {"items", {{"kind", "shader"}}}}}));
    }
    CHECK(field(ray_tracing, "traced_ray_types").at("items") == Json{{"kind", "string"}});
    CHECK(field(ray_tracing, "misses").at("items") == Json{{"kind", "shader"}});
    const auto group = field(ray_tracing, "hit_shaders").at("items").at("items");
    CHECK(group.at("kind") == "object");
    CHECK(names(group) == std::vector<std::string>{"closesthit", "anyhit", "intersection"});
    CHECK(field(group, "closesthit") == Json{{"name", "closesthit"}, {"kind", "shader"}, {"required", true}});
    const auto member = field(ray_tracing, "callable_record").at("items");
    CHECK(field(member, "type").at("enum") == Json::array({"i32", "u32", "f32", "vec2", "vec3", "vec4"}));
    CHECK(field(ray_tracing, "callable_records").at("items").at("items").at("kind") == "union");
    const auto recursion = field(ray_tracing, "max_recursion");
    CHECK(recursion.at("kind") == "integer");
    CHECK(recursion.at("min") == 1);
    CHECK(recursion.at("max") == 4294967295LL);
    CHECK(recursion.at("default") == 1);
    const auto acceleration_structure = field(ray_tracing, "acceleration_structure");
    CHECK(acceleration_structure.at("kind") == "string");
    CHECK(acceleration_structure.at("default") == "scene");
    CHECK(acceleration_structure.at("choices_from") ==
          Json{{"rpc", "get_render_authoring_context"}, {"key", "acceleration_structures"}});

    // 2. dispatch: a compute task's groups, a ray tracing task's rays, an
    // indirect buffer's command.
    const auto dispatch = parameter("dispatch");
    CHECK(dispatch.at("kind") == "object");
    CHECK(names(dispatch) == std::vector<std::string>{"groups", "groups_from", "rays", "rays_from", "indirect"});
    const auto groups = field(dispatch, "groups");
    CHECK(groups.at("kind") == "union");
    CHECK(groups.at("default") == Json::array({1, 1, 1}));
    CHECK(groups.at("variants").at(0).at("items").at("min") == 1);
    CHECK(names(groups.at("variants").at(1)) == std::vector<std::string>{"x", "y", "z"});
    CHECK(field(groups.at("variants").at(1), "x").at("default") == 1);
    CHECK(field(dispatch, "rays").at("items").at("kind") == "integer");
    CHECK(field(dispatch, "rays").at("items").at("min") == 1);
    for (const auto *from : {"groups_from", "rays_from"}) {
        INFO(from);
        CHECK(names(field(dispatch, from)) == std::vector<std::string>{"port"});
        CHECK(field(field(dispatch, from), "port").at("required") == true);
    }
    const auto indirect = field(dispatch, "indirect");
    CHECK(field(indirect, "buffer").at("choices_from") ==
          Json{{"graph_nodes", Json::array({"render.buffer", "render.attribute"})},
               {"where", {{"command_layout", Json::array({"compute_dispatch", "ray_dispatch"})}}}});
    CHECK(field(indirect, "offset").at("min") == 0);
    CHECK(field(indirect, "offset").at("default") == 0);

    // 3. A wrong kind or range inside them is refused by name.
    const auto resolve = [](Json parameters) {
        parameters["ports"] = Json::object();
        try {
            (void)builtinGraphModuleRegistry().resolveOperation(
                Json{{"id", "trace"}, {"op", "render.compute"}, {"parameters", std::move(parameters)}});
        } catch (const GraphRegistryError &error) {
            return std::pair{error.code(), error.detail()};
        }
        return std::pair{std::string{}, Json{}};
    };
    const Json rays_from{{"rays_from", {{"port", "mask"}}}};
    auto [code, detail] = resolve({{"ray_tracing", {{"raygen", 3}, {"miss", "m"}, {"closesthit", "c"}}},
                                   {"dispatch", rays_from}});
    CHECK(code == "graph.render_node_parameter_invalid@1");
    CHECK(detail == Json{{"node", "trace"}, {"op", "render.compute"}, {"parameter", "ray_tracing.raygen"},
                         {"expected", "shader"}, {"reason", "type"}});
    std::tie(code, detail) = resolve(
        {{"ray_tracing", {{"raygen", "r"}, {"miss", "m"}, {"closesthit", "c"}, {"max_recursion", 0}}},
         {"dispatch", rays_from}});
    CHECK(detail.at("parameter") == "ray_tracing.max_recursion");
    CHECK(detail.at("reason") == "range");
    CHECK(detail.at("min") == 1);
    std::tie(code, detail) =
        resolve({{"ray_tracing", {{"raygen", "r"}, {"traced_ray_types", Json::array({"primary"})},
                                  {"misses", {{"sky", "m"}}},
                                  {"hit_shaders", {{"primary", {{"sphere", {{"closesthit", 7}}}}}}}}},
                 {"dispatch", rays_from}});
    CHECK(detail.at("parameter") == "ray_tracing.hit_shaders.primary.sphere.closesthit");
    CHECK(detail.at("expected") == "shader");
    std::tie(code, detail) = resolve({{"shader", "c"}, {"dispatch", {{"rays", Json::array({64, 0, 1})}}}});
    CHECK(detail.at("parameter") == "dispatch.rays[1]");
    CHECK(detail.at("reason") == "range");
    std::tie(code, detail) = resolve({{"shader", "c"}, {"dispatch", {{"groups_from", {{"port", 2}}}}}});
    CHECK(detail.at("parameter") == "dispatch.groups_from.port");
    CHECK(detail.at("expected") == "string");
    std::tie(code, detail) =
        resolve({{"shader", "c"}, {"dispatch", {{"indirect", {{"buffer", "args"}, {"offset", -4}}}}}});
    CHECK(detail.at("parameter") == "dispatch.indirect.offset");
    CHECK(detail.at("reason") == "range");

    // 4. What is written today is taken: Studio's engine pipeline
    // (graphcanvasmodel.cpp), the legacy lists with callables, the ray group
    // layout's form (projects/pathtrace_demo), and every dispatch form.
    const std::vector<Json> taken{
        {{"ray_tracing", {{"raygen", "engine://rt_shadow_mask_pipeline"},
                          {"miss", "engine://rt_shadow_mask_pipeline"},
                          {"closesthit", "engine://rt_shadow_mask_pipeline"}}},
         {"dispatch", rays_from}},
        {{"ray_tracing", {{"raygen", "project://shaders/a"},
                          {"miss", Json::array({"project://shaders/a", "project://shaders/b"})},
                          {"closesthit", Json::array({"project://shaders/a"})},
                          {"callable", Json::array({"project://shaders/tint"})},
                          {"callable_record", Json::array({Json{{"name", "tint"}, {"type", "vec3"}}})},
                          {"callable_records", Json::array({Json{{"tint", Json::array({1.0, 0.5, 0.0})}}})},
                          {"max_recursion", 2},
                          {"acceleration_structure", "actors"}}},
         {"dispatch", {{"rays", Json::array({64, 32, 1})}}}},
        {{"ray_tracing", Json::parse(R"({
            "raygen": "project://shaders/pt", "traced_ray_types": ["radiance", "shadow"],
            "misses": {"sky": "project://shaders/pt_radiance", "shadow": "project://shaders/pt_shadow"},
            "hit_shaders": {
              "radiance": {"sphere": {"closesthit": "project://shaders/pt", "intersection": "project://shaders/sphere"},
                           "mesh": {"closesthit": "project://shaders/pt_mesh"}},
              "shadow": {"sphere": {"closesthit": "project://shaders/pt_shadow",
                                    "intersection": "project://shaders/sphere"},
                         "mesh": {"closesthit": "project://shaders/pt_shadow"}}},
            "max_recursion": 1})")},
         {"dispatch", {{"indirect", {{"buffer", "rays"}}}}}},
        {{"shader", "project://shaders/c"}, {"dispatch", {{"groups", Json::array({1, 1, 1})}}}},
        {{"shader", "project://shaders/c"}, {"dispatch", {{"groups", {{"x", 8}}}}}},
        {{"shader", "project://shaders/c"}, {"dispatch", {{"groups_from", {{"port", "h0"}}}}}},
        {{"shader", "project://shaders/c"}, {"dispatch", {{"indirect", {{"buffer", "args"}, {"offset", 16}}}}}},
    };
    for (const auto &parameters : taken) {
        INFO(parameters.dump());
        std::tie(code, detail) = resolve(parameters);
        CHECK(code.empty());
    }
}

TEST_CASE("a build without the animation unit rejects a pose document by name",
          "[curve][graph-composition][graph-module]") {
    using namespace Pelican;
    // The same document either way. Whether pelican.pose is registered is the
    // only difference PELICAN_WITH_ANIMATION makes to the common graph, so this
    // is the OFF build's observation and the ON build's contrast.
    const auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{"pose":{"nodes":[
        {"id":"time","op":"graph.input"},
        {"id":"walk","op":"pose.clip_sample","parameters":{"rig":"m.glb","clip":"Walk"},
         "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]},
        {"id":"out","op":"pose.skeletal","inputs":{"pose":{"node":"walk","port":"pose"}},
         "outputs":[]}],"outputs":{}}},
      "nodes":[
        {"id":"clock","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
        {"id":"hero","op":"graph.instance","definition":"pose",
         "owner":{"scene":"scenes/main","object":7},
         "inputs":{"time":{"node":"clock","port":"value"}}}],
      "outputs":{}})");
#if PELICAN_WITH_ANIMATION
    REQUIRE(builtinGraphModuleRegistry().contains("pose.clip_sample"));
    REQUIRE(builtinGraphModuleRegistry().isSink("pose.skeletal"));
    const auto partitions = partitionGraphDocument(document);
    REQUIRE(partitions.animation.nodes.size() == 2);
#else
    REQUIRE_FALSE(builtinGraphModuleRegistry().contains("pose.clip_sample"));
    try {
        partitionGraphDocument(document);
        FAIL("a pose document was accepted without the animation unit");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unregistered@1");
        // Expanded nodes resolve in id order, so the first pose operation the
        // registry has never heard of is the one it names.
        REQUIRE(error.detail().at("op") == "pose.skeletal");
        REQUIRE(error.detail().at("node") == "hero/out");
    }
#endif
    // The value, render and audio sides of the common graph are unaffected
    // either way.
    REQUIRE_NOTHROW(partitionGraphDocument(nlohmann::json::parse(graphCompositionFixture)));
    REQUIRE_NOTHROW(ValueGraph::Graph{graphCompositionFixture});
}

TEST_CASE("PG-1 a build without the audio unit rejects an audio document by name",
          "[curve][graph-composition][graph-module][pg1]") {
    using namespace Pelican;
    // The same document either way: a clock driving a voice and the voice's
    // volume. Whether pelican.audio is registered is the only difference
    // PELICAN_WITH_AUDIO makes to the common graph.
    const auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "nodes":[
        {"id":"clock","op":"value.input","parameters":{"name":"scene_time"},"outputs":["value"]},
        {"id":"cue","op":"event.playback","outputs":["events"]},
        {"id":"sound","op":"audio.playback","parameters":{"asset":"project://tone.wav"},
         "inputs":{"time":{"node":"clock","port":"value"},"events":{"node":"cue","port":"events"}},
         "outputs":["voice"]},
        {"id":"level","op":"value.constant","parameters":{"value":0.5},"outputs":["value"]},
        {"id":"volume","op":"audio.volume","inputs":{"value":{"node":"level","port":"value"},
         "voice":{"node":"sound","port":"voice"}},"outputs":[]}],
      "outputs":{}})");
    // AU-1: the voice's new forms -- a position to follow, a bus, the
    // integrated time map -- are the same operation, so the same refusal.
    const auto au1_document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
      "nodes":[
        {"id":"clock","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
        {"id":"scene","op":"value.input","parameters":{"name":"scene_time"},"outputs":["value"]},
        {"id":"cue","op":"event.playback","outputs":["events"]},
        {"id":"map","op":"value.playback","parameters":{"wrap":"repeat","duration":2},
         "inputs":{"elapsed":{"node":"clock","port":"value"}},"outputs":["value"]},
        {"id":"bgm","op":"audio.playback","parameters":{"asset":"project://bgm.wav","bus":"bgm"},
         "inputs":{"time":{"node":"clock","port":"value"},"position":{"node":"map","port":"value"}},
         "outputs":["voice"]},
        {"id":"loop","op":"audio.playback","parameters":{"asset":"project://bgm.wav","wrap":"repeat",
         "source_begin":0.5,"speed":1},
         "inputs":{"time":{"node":"scene","port":"value"},"events":{"node":"cue","port":"events"}},
         "outputs":["voice"]}],
      "outputs":{}})");
#if PELICAN_WITH_AUDIO
    REQUIRE(builtinGraphModuleRegistry().contains("audio.playback"));
    REQUIRE(builtinGraphModuleRegistry().isSink("audio.volume"));
    REQUIRE_NOTHROW(partitionGraphDocument(document));
    const ValueGraph::Graph graph{document.dump()};
    REQUIRE(graph.audioSources().size() == 1);
    const ValueGraph::Graph au1_graph{au1_document.dump()};
    REQUIRE(au1_graph.audioSources().size() == 2);
#else
    for (const auto &attempt : std::vector<std::function<void()>>{
             [&] { (void)partitionGraphDocument(document); },
             // The runtime's own entry: the value graph a rendering config
             // prepares is built from this constructor.
             [&] { const ValueGraph::Graph graph{document.dump()}; },
             [&] { (void)partitionGraphDocument(au1_document); },
             [&] { const ValueGraph::Graph graph{au1_document.dump()}; }}) {
        try {
            attempt();
            FAIL("an audio document was accepted without the audio unit");
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "graph.operation_unregistered@1");
            REQUIRE(error.detail().at("op").get<std::string>().rfind("audio.", 0) == 0);
            std::cout << "PG1_AUDIO_GRAPH state=OFF code=" << error.code()
                      << " op=" << error.detail().at("op").get<std::string>() << '\n';
        }
    }
    REQUIRE_FALSE(builtinGraphModuleRegistry().contains("audio.playback"));
#endif
    // The value and render sides of the common graph are unaffected.
    REQUIRE_NOTHROW(partitionGraphDocument(nlohmann::json::parse(graphCompositionFixture)));
}

TEST_CASE("a registered operation without a backend is rejected by name",
          "[curve][graph-composition][graph-module]") {
    using namespace Pelican;
    auto registry = makeBuiltinGraphModuleRegistry();
    registerTestOpaqueGraphModule(registry);
    try {
        const ValueGraph::Graph graph{R"({"schema":"pelican.graph","version":1,
            "nodes":[{"id":"emit","op":"test.emit","outputs":["token"]}],"outputs":{}})",
            registry};
        FAIL("an operation with no value backend was compiled");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unexecutable@1");
        REQUIRE(error.detail().at("backend") == "pelican.value_graph");
        REQUIRE(error.detail().at("op") == "emit");
        REQUIRE(error.detail().at("node") == "emit");
    }
    // A saved backend document reaches the same dispatch.
    try {
        const ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
            "nodes":[{"id":"n","op":"opaque"}],"outputs":{}})"};
        FAIL("an unexecutable backend operation was compiled");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unexecutable@1");
        REQUIRE(error.detail().at("op") == "opaque");
        REQUIRE(error.detail().at("node") == "n");
    }
    try {
        lowerRenderGraphDocument(nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
            "nodes":[{"id":"draw","op":"test.draw","parameters":{},"outputs":[]}],"outputs":{}})"),
            {}, registry);
        FAIL("an operation with no render backend was lowered");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unexecutable@1");
        REQUIRE(error.detail().at("backend") == "pelican.render");
        REQUIRE(error.detail().at("op") == "test.draw");
        REQUIRE(error.detail().at("node") == "draw");
    }
}

TEST_CASE("playback nodes map dynamic timing independently of curve shape", "[curve][valuegraph]") {
    const Pelican::ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[
        {"id":"shape","op":"curve","time":"phase","keys":[{"time":2,"value":0},{"time":6,"value":1}]},
        {"id":"phase","op":"playback","elapsed":"clock","source_begin":2,"source_end":6,
          "duration":"duration","speed":"speed","delay":0.25,"sample_interval":0.5,"wrap":"ping_pong"},
        {"id":"clock","op":"input","name":"time"},
        {"id":"duration","op":"input","name":"duration"},
        {"id":"speed","op":"input","name":"speed"}],
      "outputs":{"value":["shape"],"source_time":["phase"]}})"};
    const Pelican::ValueGraph::Graph restored{graph.serialize()};
    const auto evaluate = [&](double time, double duration, double speed) {
        return restored.evaluate({{"time", time}, {"duration", duration}, {"speed", speed}});
    };
    REQUIRE(evaluate(1, 2, 1).at("source_time")[0] == Approx(3));
    REQUIRE(evaluate(1, 2, 1).at("value")[0] == Approx(0.25));
    REQUIRE(evaluate(1, 4, 1).at("value")[0] == Approx(0.125));
    REQUIRE(evaluate(1, 2, -1).at("value")[0] == Approx(0.75));
    REQUIRE(evaluate(0.1, 2, 1).at("value")[0] == Approx(0));
    REQUIRE(evaluate(3.25, 2, 1).at("value")[0] == Approx(0.5));
    REQUIRE_THROWS(evaluate(1, 0, 1));
    REQUIRE_THROWS(evaluate(1, 2, 0));
}

TEST_CASE("saved value graphs compose curves and external parameters", "[curve][valuegraph]") {
    const Pelican::ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[
        {"id":"result","op":"add","a":"scaled","b":"bias"},
        {"id":"scaled","op":"multiply","a":"shape","b":"gain"},
        {"id":"squared","op":"multiply","a":"t","b":"t"},
        {"id":"shape","op":"curve","time":"t","keys":[
          {"time":0,"value":0,"interpolation":"cubic","out_tangent":0},
          {"time":1,"value":1,"in_tangent":2}]},
        {"id":"t","op":"input","name":"clock"},
        {"id":"gain","op":"input","name":"gain"},
        {"id":"bias","op":"constant","value":3}],
      "outputs":{"values":["result","squared"]}})"};
    const Pelican::ValueGraph::Graph restored{graph.serialize()};
    const auto first = restored.evaluate({{"clock", 0.5}, {"gain", 2}});
    REQUIRE(first.at("values")[0] == Approx(3.5));
    REQUIRE(first.at("values")[1] == Approx(0.25));
    REQUIRE(restored.evaluate({{"clock", 0.25}, {"gain", 4}}).at("values")[0] == Approx(3.25));
    REQUIRE_THROWS(restored.evaluate({{"clock", 0.5}}));
    REQUIRE_THROWS(Pelican::ValueGraph::Graph(R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"x","op":"add","a":"x","b":"x"}],"outputs":{"v":["x"]}})"));
    REQUIRE_THROWS(Pelican::ValueGraph::Graph(R"({"schema":"pelican.value_graph","version":1,
      "nodes":[],"outputs":{"v":["missing"]}})"));
    REQUIRE_THROWS(Pelican::ValueGraph::Graph(R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"x","op":"constant","value":1}],"outputs":{"v":["x","x"]},
      "bindings":[{"output":"v","kind":"sound_volume","target":"voice"}]})"));
}

TEST_CASE("scalar curves preserve slope units and step key boundaries", "[animation][curve]") {
    // The Hermite segment represents y = (time - 2)^2 over four seconds.
    const ScalarCurve parabola{{
        {.time = 2, .value = 0, .out_tangent = 0, .interpolation = CurveInterpolation::cubic},
        {.time = 6, .value = 16, .in_tangent = 8},
    }};
    REQUIRE(parabola.sample(3) == Approx(1));
    REQUIRE(parabola.sample(4) == Approx(4));
    REQUIRE(parabola.sample(5) == Approx(9));
    REQUIRE(parabola.sample(-1) == 0);
    REQUIRE(parabola.sample(9) == 16);

    const ScalarCurve steps{{
        {.time = 0, .value = 3, .interpolation = CurveInterpolation::step},
        {.time = 2, .value = 7, .interpolation = CurveInterpolation::step},
        {.time = 5, .value = 11},
    }};
    REQUIRE(steps.sample(std::nextafter(2.0, 0.0)) == 3);
    REQUIRE(steps.sample(2) == 7);
    REQUIRE(steps.sample(std::nextafter(5.0, 0.0)) == 7);
    REQUIRE(steps.sample(5) == 11);
    const ScalarCurve constant{{{.time = 4, .value = 9}}};
    REQUIRE(constant.sample(-100) == 9);
}

TEST_CASE("curve playback reuses shape with source range scale reverse and wrapping", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 10, .value = 100}}};
    CurvePlaybackSettings settings{.source_begin = 2, .source_end = 8, .duration = 4,
                                   .speed = 2, .scale = 0.5, .offset = -3};
    auto sample = sampleCurve(curve, settings, 0.5);
    REQUIRE(sample.source_time == Approx(3.5));
    REQUIRE(sample.value == Approx(14.5));
    REQUIRE(sample.active);
    REQUIRE_FALSE(sample.completed);
    sample = sampleCurve(curve, settings, 2);
    REQUIRE(sample.value == 37);
    REQUIRE(sample.completed);
    REQUIRE_FALSE(sample.active);

    settings.speed = -2;
    REQUIRE(sampleCurve(curve, settings, 0).source_time == 8);
    REQUIRE(sampleCurve(curve, settings, 0.5).source_time == Approx(6.5));
    REQUIRE(sampleCurve(curve, settings, 3).source_time == 2);
    settings.wrap = CurveWrap::repeat;
    REQUIRE(sampleCurve(curve, settings, 2).source_time == 8);
    sample = sampleCurve(curve, settings, 2.5);
    REQUIRE(sample.source_time == Approx(6.5));
    REQUIRE(sample.active);
    REQUIRE_FALSE(sample.completed);
    settings.speed = 2;
    settings.wrap = CurveWrap::ping_pong;
    REQUIRE(sampleCurve(curve, settings, 2).source_time == 8);
    REQUIRE(sampleCurve(curve, settings, 3).source_time == 5);
    REQUIRE(sampleCurve(curve, settings, 4).source_time == 2);
}

TEST_CASE("curve triggers include delay in retrigger lifetime and restart after completion", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 1, .value = 10}}};
    CurveTriggerClock clock{{.duration = 2, .delay = 1}};
    REQUIRE_FALSE(clock.sample(curve, 100).active);
    REQUIRE_FALSE(clock.sample(curve, 100).completed);
    REQUIRE(clock.trigger(10));
    REQUIRE_FALSE(clock.sample(curve, 10.5).active);
    REQUIRE_FALSE(clock.trigger(10.5, CurveRetrigger::ignore_active));
    REQUIRE(clock.sample(curve, 11).active);
    REQUIRE(clock.sample(curve, 12).value == 5);
    REQUIRE_FALSE(clock.trigger(12, CurveRetrigger::ignore_active));
    REQUIRE(clock.trigger(12, CurveRetrigger::restart));
    REQUIRE_FALSE(clock.sample(curve, 12.5).active);
    REQUIRE(clock.sample(curve, 14).value == 5);
    REQUIRE(clock.sample(curve, 15).completed);
    REQUIRE(clock.trigger(15, CurveRetrigger::ignore_active));
    clock.stop();
    REQUIRE_FALSE(clock.sample(curve, 100).active);
    REQUIRE_FALSE(clock.sample(curve, 100).completed);
    REQUIRE(clock.trigger(100, CurveRetrigger::ignore_active));
    REQUIRE(clock.sample(curve, 102).value == 5);
}

TEST_CASE("fixed curve sampling is independent of calls and includes final value and LUT endpoints", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 1, .value = 10}}};
    const CurvePlaybackSettings settings{.duration = 1, .delay = 0.5, .sample_interval = 0.3};
    REQUIRE(sampleCurve(curve, settings, 0.79).value == 0);
    REQUIRE(sampleCurve(curve, settings, 0.99).value == Approx(3));
    REQUIRE(sampleCurve(curve, settings, 1.49).value == Approx(9));
    REQUIRE(sampleCurve(curve, settings, 0.99).value == Approx(3));
    const auto completed = sampleCurve(curve, settings, 1.5);
    REQUIRE(completed.value == 10);
    REQUIRE(completed.completed);
    const auto lut = curve.sampleUniform(0.2, 0.8, 4);
    REQUIRE(lut.size() == 4);
    REQUIRE(lut[0] == Approx(2));
    REQUIRE(lut[1] == Approx(4));
    REQUIRE(lut[2] == Approx(6));
    REQUIRE(lut[3] == Approx(8));
}

TEST_CASE("curve input rejects ambiguous keys and invalid playback settings", "[animation][curve]") {
    REQUIRE_THROWS_AS(ScalarCurve(std::vector<CurveKey>{}), std::invalid_argument);
    const std::vector<CurveKey> duplicates{{.time = 1}, {.time = 1}};
    REQUIRE_THROWS_AS(ScalarCurve(duplicates), std::invalid_argument);
    const ScalarCurve curve{{{.time = 0, .value = 1}}};
    REQUIRE_THROWS_AS(curve.sample(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    REQUIRE_THROWS_AS(curve.sampleUniform(0, 1, 1), std::invalid_argument);
    CurvePlaybackSettings settings;
    settings.duration = 0;
    REQUIRE_THROWS_AS(sampleCurve(curve, settings, 0), std::invalid_argument);
}

TEST_CASE("evaluating a clock without state is refused by name",
          "[curve][graph-composition][offline]") {
    using namespace Pelican;
    // The three operations that carry state between evaluations. Each one names
    // itself and the node, so a caller reading the rejection knows which node of
    // which document it has to give a state to.
    const std::vector<std::pair<std::string, std::string>> clocks{
        {"trigger_clock", "value.trigger_clock"},
        {"event_clock", "value.event_clock"},
        {"audio_playback", "audio.playback"},
    };
    for (const auto &[lowered, op] : clocks) {
        // Spelled in the lowered schema so the document names the node itself.
        nlohmann::json document{{"schema", "pelican.value_graph"}, {"version", 1},
            {"nodes", nlohmann::json::array({
                {{"id", "now"}, {"op", "input"}, {"name", "time"}},
                {{"id", "gate"}, {"op", "constant"}, {"value", 1}},
                {{"id", "commands"}, {"op", "event_input"}}})},
            {"outputs", {{"elapsed", nlohmann::json::array({"clocked"})}}},
            {"bindings", nlohmann::json::array()}};
        if (lowered == "trigger_clock")
            document["nodes"].push_back({{"id", "clocked"}, {"op", "trigger_clock"},
                {"time", "now"}, {"trigger", "gate"}});
        else if (lowered == "event_clock")
            document["nodes"].push_back({{"id", "clocked"}, {"op", "event_clock"},
                {"time", "now"}, {"events", "commands"}});
        else
            document["nodes"].push_back({{"id", "clocked"}, {"op", "audio_playback"},
                {"time", "now"}, {"events", "commands"}, {"asset", "project://tone.wav"}});
        const ValueGraph::Graph graph{document.dump()};
        try {
            graph.evaluate({{"time", 1.0}});
            FAIL("a stateful operation evaluated without state: " + op);
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "graph.evaluation_requires_state@1");
            REQUIRE(error.detail().at("node") == "clocked");
            REQUIRE(error.detail().at("op") == op);
        }
        // The same document, given a state, is not refused: the name marks the
        // one thing the backend cannot do, not the operation itself.
        auto state = graph.createState();
        REQUIRE_NOTHROW(graph.evaluate({{"time", 1.0}}, {}, {}, &state));
    }
}

namespace {
// LP-1a: a root graph.loop whose element 0 runs `first` and every other
// element `paint`. Both bodies draw into an image of their own.
nlohmann::json lp1aEditColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

nlohmann::json lp1aEditBody(const std::string &pass) {
    using Json = nlohmann::json;
    const auto type = lp1aEditColorType();
    return Json{{"nodes", Json::array({
        {{"id", "image"}, {"op", "render.image"},
         {"parameters", {{"type", type}, {"format", "R16G16B16A16_SFLOAT"}, {"extent_scale", 1.0},
                         {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
         {"outputs", {"resource"}}},
        {{"id", pass}, {"op", "render.fullscreen"},
         {"parameters", {{"pass_set", "main_render"},
                         {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                         {"ports", {{"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                               {"attachment", "color"}, {"location", 0}}}}}}},
         {"inputs", {{"color", {{"node", "image"}, {"port", "resource"}}}}},
         {"outputs", {"color"}}}})},
        {"outputs", {{"image", {{"node", pass}, {"port", "color"}}}}}};
}

nlohmann::json lp1aEditDocument() {
    using Json = nlohmann::json;
    return Json{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"first", lp1aEditBody("first_fill")}, {"paint", lp1aEditBody("fill")}}},
        {"nodes", Json::array({
            {{"id", "lit"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "lit_color"}, {"type", lp1aEditColorType()}}}, {"outputs", {"resource"}}},
            {{"id", "steps"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"range", {{"count", 3}}}}},
                             {"branches", Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                                       {{"body", "paint"}}})}}},
             {"outputs", {"image"}}}})},
        {"outputs", Json::object()}};
}
} // namespace

TEST_CASE("LP-1a renaming a loop body renames the loop's branch, and a body in use is not removed",
          "[curve][graph-composition][graph-definition-edit][lp1a]") {
    using Json = nlohmann::json;
    const auto document = lp1aEditDocument();
    REQUIRE_NOTHROW(Pelican::partitionGraphDocument(document));
    const auto renamed = Pelican::editGraphDocument(document, Json::array({
        Json{{"op", "rename_definition"}, {"from", "paint"}, {"to", "brush"}}}));
    CHECK(renamed.at("nodes").at(1).at("parameters").at("branches").at(1).at("body") == "brush");
    CHECK(renamed.at("nodes").at(1).at("parameters").at("branches").at(0).at("body") == "first");
    CHECK(Pelican::expandGraphDocument(renamed).loops.at("steps").branches.at(1)->definition == "brush");
    const auto back = Pelican::editGraphDocument(renamed, Json::array({
        Json{{"op", "rename_definition"}, {"from", "brush"}, {"to", "paint"}}}));
    CHECK(back == document);
    CHECK(back.dump(2) == document.dump(2));

    const Json remove = Json::array({Json{{"op", "remove_definition"}, {"name", "paint"}}});
    CHECK(lp0cEditCode(document, remove) == "graph.definition_referenced@1");
    CHECK(lp0cEditDetail(document, remove) ==
          Json{{"definition", "paint"}, {"referenced_by", Json::array({"steps"})}});
    // With the branch pointed elsewhere first, the body goes.
    auto branches = document.at("nodes").at(1).at("parameters");
    branches["branches"][1]["body"] = "first";
    CHECK(lp0cEditCode(document, Json::array({
        Json{{"op", "set_node_parameters"}, {"node", "steps"}, {"parameters", branches}},
        Json{{"op", "remove_definition"}, {"name", "paint"}}})) == "<accepted>");
    // A loop stands in the root document only.
    const Json nested = Json::array({Json{{"op", "add_node"}, {"scope", "paint"},
        {"node", document.at("nodes").at(1)}}});
    CHECK(lp0cEditCode(document, nested) == "graph.loop_nested@1");
    CHECK(lp0cEditDetail(document, nested) == Json{{"node", "steps"}, {"definition", "paint"}});
    CHECK(lp0cEditCode(document, Json::array({Json{{"op", "add_definition"}, {"name", "holder"},
        {"definition", {{"nodes", Json::array({document.at("nodes").at(1)})}, {"outputs", Json::object()}}}}})) ==
          "graph.loop_nested@1");
}

TEST_CASE("LP-1a a loop's inputs and its bodies' boundary agree after the list, in either order",
          "[curve][graph-composition][graph-definition-edit][lp1a]") {
    using Json = nlohmann::json;
    const auto document = lp1aEditDocument();
    const auto declare = [](const char *body) {
        return Json{{"op", "add_node"}, {"scope", body}, {"node", {{"id", "target"}, {"op", "graph.input"}}}};
    };
    const Json supply{{"op", "set_input"}, {"node", "steps"}, {"port", "target"},
                      {"source", {{"node", "lit"}, {"port", "resource"}}}};
    const auto declared_first = Pelican::editGraphDocument(document, Json::array({
        declare("first"), declare("paint"), supply}));
    const auto supplied_first = Pelican::editGraphDocument(document, Json::array({
        supply, declare("first"), declare("paint")}));
    CHECK(declared_first == supplied_first);
    CHECK(declared_first.at("nodes").at(1).at("inputs") ==
          Json{{"target", {{"node", "lit"}, {"port", "resource"}}}});
    REQUIRE_NOTHROW(Pelican::partitionGraphDocument(declared_first));
    // One half alone.
    CHECK(lp0cEditCode(document, Json::array({supply})) == "graph.input_unknown@1");
    CHECK(lp0cEditDetail(document, Json::array({supply})) ==
          Json{{"node", "steps"}, {"definition", "first"}, {"port", "target"}});
    CHECK_THROWS_AS(Pelican::editGraphDocument(document, Json::array({declare("first"), declare("paint")})),
                    std::invalid_argument); // an unsupplied input without a default
    // Declared in one body only: the other body is named.
    CHECK(lp0cEditDetail(document, Json::array({declare("paint"), supply})) ==
          Json{{"node", "steps"}, {"definition", "first"}, {"port", "target"}});
    // Taking it out again, in either order; a body input the loop still
    // supplies is not removed.
    const auto undeclare = [](const char *body) {
        return Json{{"op", "remove_node"}, {"scope", body}, {"node", "target"}};
    };
    const Json unsupply{{"op", "set_input"}, {"node", "steps"}, {"port", "target"}, {"source", nullptr}};
    auto without_inputs = document;
    CHECK(Pelican::editGraphDocument(declared_first, Json::array({
        unsupply, undeclare("first"), undeclare("paint")})) == without_inputs);
    CHECK(Pelican::editGraphDocument(declared_first, Json::array({
        undeclare("first"), undeclare("paint"), unsupply})) == without_inputs);
    CHECK(lp0cEditCode(declared_first, Json::array({undeclare("paint"), undeclare("first")})) ==
          "graph.input_referenced@1");
    CHECK(lp0cEditDetail(declared_first, Json::array({undeclare("paint"), undeclare("first")})) ==
          Json{{"definition", "paint"}, {"input", "target"}, {"instance", "steps"}});
}

TEST_CASE("LP-1a a document with a loop is saved as written, without its expansion",
          "[curve][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    // Built by edits from an empty root, as an editor would.
    const Json empty{{"schema", "pelican.graph"}, {"version", 1}, {"nodes", Json::array()}, {"outputs", Json::object()}};
    const auto reference = lp1aEditDocument();
    const auto edited = Pelican::editGraphDocument(empty, Json::array({
        Json{{"op", "add_definition"}, {"name", "first"}, {"definition", reference.at("definitions").at("first")}},
        Json{{"op", "add_definition"}, {"name", "paint"}, {"definition", reference.at("definitions").at("paint")}},
        Json{{"op", "add_node"}, {"node", reference.at("nodes").at(0)}},
        Json{{"op", "add_node"}, {"node", reference.at("nodes").at(1)}}}));
    CHECK(edited == reference);
    const auto saved = edited.dump(2);
    CHECK(Json::parse(saved) == edited);
    // The expansion is derived: neither the template nor a copy is saved.
    CHECK(saved.find("steps/") == std::string::npos);
    const auto expanded = Pelican::expandGraphDocument(Json::parse(saved));
    CHECK(expanded.origins.contains("steps/$0/first_fill"));
    CHECK(expanded.origins.at("steps/$1/fill").loop == "steps");
    CHECK(expanded.origins.at("steps/$1/fill").loop_branch == std::optional<std::size_t>{1});
    // The loop itself is no expanded node.
    for (const auto &node : expanded.nodes) CHECK(node.at("id") != "steps");
}

namespace {
// DO-1: the outputs op for one scope.
nlohmann::json do1SetOutputs(const char *scope, nlohmann::json outputs) {
    return {{"op", "set_outputs"}, {"scope", scope}, {"outputs", std::move(outputs)}};
}

nlohmann::json do1Endpoint(const char *node, const char *port) {
    return {{"node", node}, {"port", port}};
}
} // namespace

TEST_CASE("DO-1 set_outputs adds a loop body output that the loop's p and p[] then read, in either order",
          "[curve][graph-composition][graph-definition-edit][do1]") {
    using Json = nlohmann::json;
    const auto document = lp1aEditDocument();
    // Both branch bodies gain `again`, the loop declares it as `p` and `p[]`,
    // and the root reads `p`.
    const Json bodies = Json::array({
        do1SetOutputs("first", {{"image", do1Endpoint("first_fill", "color")},
                                {"again", do1Endpoint("first_fill", "color")}}),
        do1SetOutputs("paint", {{"image", do1Endpoint("fill", "color")},
                                {"again", do1Endpoint("fill", "color")}})});
    auto loop = document.at("nodes").at(1);
    loop["outputs"] = Json::array({"image", "again", "again[]"});
    const Json wiring = Json::array({
        Json{{"op", "replace_node"}, {"node", loop}},
        do1SetOutputs("root", {{"final", do1Endpoint("steps", "again")}})});
    auto bodies_first = bodies;
    bodies_first.insert(bodies_first.end(), wiring.begin(), wiring.end());
    auto wired_first = wiring;
    wired_first.insert(wired_first.end(), bodies.begin(), bodies.end());
    const auto edited = Pelican::editGraphDocument(document, bodies_first);
    CHECK(Pelican::editGraphDocument(document, wired_first) == edited);
    CHECK(edited.at("definitions").at("paint").at("outputs").at("again") == do1Endpoint("fill", "color"));
    CHECK(edited.at("outputs") == Json{{"final", do1Endpoint("steps", "again")}});
    const auto expanded = Pelican::expandGraphDocument(edited);
    CHECK(expanded.outputs.contains("final"));
    // Nothing else of the document moved.
    auto rest = edited;
    rest["definitions"]["first"]["outputs"].erase("again");
    rest["definitions"]["paint"]["outputs"].erase("again");
    rest["nodes"][1]["outputs"] = Json::array({"image"});
    rest["outputs"] = Json::object();
    CHECK(rest == document);

    // The inverse carries what each scope held before, in either order too.
    const Json unbodies = Json::array({
        do1SetOutputs("first", document.at("definitions").at("first").at("outputs")),
        do1SetOutputs("paint", document.at("definitions").at("paint").at("outputs"))});
    const Json unwiring = Json::array({
        Json{{"op", "replace_node"}, {"node", document.at("nodes").at(1)}},
        do1SetOutputs("root", document.at("outputs"))});
    auto unwired_first = unwiring;
    unwired_first.insert(unwired_first.end(), unbodies.begin(), unbodies.end());
    auto unbodies_first = unbodies;
    unbodies_first.insert(unbodies_first.end(), unwiring.begin(), unwiring.end());
    CHECK(Pelican::editGraphDocument(edited, unwired_first) == document);
    CHECK(Pelican::editGraphDocument(edited, unbodies_first) == document);
    CHECK(Pelican::editGraphDocument(edited, unbodies_first).dump(2) == document.dump(2));
}

TEST_CASE("DO-1 an output still read from outside its body is not removed, and the reader is named",
          "[curve][graph-composition][graph-definition-edit][do1]") {
    using Json = nlohmann::json;
    // An instance's port: read by an edge, then by the root's outputs.
    const auto curves = parameterGraphCompositionFixture();
    const Json drop = Json::array({do1SetOutputs("scaled_curve", Json::object())});
    CHECK(lp0cEditCode(curves, drop) == "graph.output_referenced@1");
    CHECK(lp0cEditDetail(curves, drop) ==
          Json{{"definition", "scaled_curve"}, {"output", "result"}, {"node", "left"},
               {"consumer", "render"}, {"port", "value"}});
    const Json rewire{{"op", "set_input"}, {"node", "render"}, {"port", "value"},
                      {"source", do1Endpoint("two", "value")}};
    const Json after_rewire = Json::array({drop.at(0), rewire});
    CHECK(lp0cEditDetail(curves, after_rewire) ==
          Json{{"definition", "scaled_curve"}, {"output", "result"}, {"node", "left"},
               {"scope_output", "left"}});
    // Every reader taken off in the same list, in any order: the output goes.
    const Json root_outputs = do1SetOutputs("root", {{"gpu", curves.at("outputs").at("gpu")}});
    CHECK(lp0cEditCode(curves, Json::array({drop.at(0), rewire, root_outputs})) == "<accepted>");
    CHECK(lp0cEditCode(curves, Json::array({root_outputs, rewire, drop.at(0)})) == "<accepted>");
    // Declared again by the end of the list, it was never lost.
    CHECK(lp0cEditCode(curves, Json::array({drop.at(0),
        do1SetOutputs("scaled_curve", curves.at("definitions").at("scaled_curve").at("outputs"))})) ==
          "<accepted>");

    // A loop's: its declared `p`, its carry, an edge reading `p[]`.
    const auto document = lp1aEditDocument();
    const Json empty_bodies = Json::array({do1SetOutputs("first", Json::object()),
                                           do1SetOutputs("paint", Json::object())});
    CHECK(lp0cEditCode(document, empty_bodies) == "graph.output_referenced@1");
    CHECK(lp0cEditDetail(document, empty_bodies) ==
          Json{{"definition", "first"}, {"output", "image"}, {"node", "steps"}, {"field", "outputs"}});
    auto undeclared = document;
    undeclared["nodes"][1]["outputs"] = Json::array();
    CHECK(lp0cEditCode(undeclared, empty_bodies) == "<accepted>");
    auto carried = undeclared;
    carried["nodes"][1]["parameters"]["carry"] = Json::array({{{"from", "image"}, {"to", "target"}}});
    CHECK(lp0cEditDetail(carried, empty_bodies) ==
          Json{{"definition", "first"}, {"output", "image"}, {"node", "steps"}, {"field", "carry"}});
    auto per_element = undeclared;
    per_element["nodes"].push_back({{"id", "reader"}, {"op", "value.add"},
        {"inputs", {{"a", {{"node", "steps"}, {"port", "image[]"}, {"element", 0}}}}},
        {"outputs", Json::array({"value"})}});
    CHECK(lp0cEditDetail(per_element, empty_bodies) ==
          Json{{"definition", "first"}, {"output", "image"}, {"node", "steps"},
               {"consumer", "reader"}, {"port", "a"}});
}

TEST_CASE("DO-1 a loop's branch bodies change their outputs together, in one list",
          "[curve][graph-composition][graph-definition-edit][do1]") {
    using Json = nlohmann::json;
    const auto document = lp1aEditDocument();
    // One body alone: the bodies disagree, and the other body is named.
    const Json first_only = Json::array({do1SetOutputs("first",
        {{"image", do1Endpoint("first_fill", "color")}, {"again", do1Endpoint("first_fill", "color")}})});
    CHECK(lp0cEditCode(document, first_only) == "graph.loop_branch_interface@1");
    CHECK(lp0cEditDetail(document, first_only) ==
          Json{{"node", "steps"}, {"branch", 1}, {"definition", "paint"}, {"port", "again"}});
    // Removing from one body only is the same disagreement, not a reference:
    // the other body still has the output the loop declares.
    const Json drop_first = Json::array({do1SetOutputs("first", Json::object())});
    CHECK(lp0cEditCode(document, drop_first) == "graph.loop_branch_interface@1");
    CHECK(lp0cEditDetail(document, drop_first) ==
          Json{{"node", "steps"}, {"branch", 1}, {"definition", "paint"}, {"port", "image"}});
    // Both in one list are aligned.
    auto both = first_only;
    both.push_back(do1SetOutputs("paint",
        {{"image", do1Endpoint("fill", "color")}, {"again", do1Endpoint("fill", "color")}}));
    CHECK(lp0cEditCode(document, both) == "<accepted>");
}

TEST_CASE("DO-1 set_outputs refuses a malformed outputs map",
          "[curve][graph-composition][graph-definition-edit][do1]") {
    using Json = nlohmann::json;
    const auto document = lp1aEditDocument();
    const auto one = [](const char *port, Json endpoint) { return Json{{port, std::move(endpoint)}}; };
    // A node the scope does not hold is named with the scope.
    const Json absent = Json::array({do1SetOutputs("paint", one("image", do1Endpoint("first_fill", "color")))});
    CHECK(lp0cEditCode(document, absent) == "graph.node_missing@1");
    CHECK(lp0cEditDetail(document, absent) == Json{{"node", "first_fill"}, {"scope", "paint"}});
    // The rest is a malformed request.
    const auto malformed = [&](Json outputs) {
        CAPTURE(outputs);
        CHECK_THROWS_AS(Pelican::editGraphDocument(document,
            Json::array({do1SetOutputs("paint", std::move(outputs))})), std::invalid_argument);
    };
    malformed(Json::array());
    malformed(one("image", do1Endpoint("fill", "absent")));  // a port the node lacks
    malformed(one("image", do1Endpoint("image", "value")));
    malformed(one("image", Json{{"node", "fill"}}));
    malformed(one("image", Json{{"node", "fill"}, {"port", "color"}, {"element", 0}}));
    malformed(one("image", "fill.color"));
    malformed(one("", do1Endpoint("fill", "color")));
    malformed(one("a/b", do1Endpoint("fill", "color")));
    malformed(one("image[]", do1Endpoint("fill", "color")));
    CHECK_THROWS_AS(Pelican::editGraphDocument(document, Json::array({Json{{"op", "set_outputs"},
        {"scope", "paint"}}})), std::invalid_argument);
}
