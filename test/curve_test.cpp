#include "../src/core/userpublic/animation/curve.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "graph_composition_fixture.hpp"
#include "graphtestopaquemodule.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

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
    REQUIRE_THROWS_AS(ValueGraph::Graph(document.dump()), std::invalid_argument);
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
        {"id":"clock","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
        {"id":"cue","op":"event.playback","outputs":["events"]},
        {"id":"sound","op":"audio.playback","parameters":{"asset":"project://tone.wav"},
         "inputs":{"time":{"node":"clock","port":"value"},"events":{"node":"cue","port":"events"}},
         "outputs":["voice"]},
        {"id":"level","op":"value.constant","parameters":{"value":0.5},"outputs":["value"]},
        {"id":"volume","op":"audio.volume","inputs":{"value":{"node":"level","port":"value"},
         "voice":{"node":"sound","port":"voice"}},"outputs":[]}],
      "outputs":{}})");
#if PELICAN_WITH_AUDIO
    REQUIRE(builtinGraphModuleRegistry().contains("audio.playback"));
    REQUIRE(builtinGraphModuleRegistry().isSink("audio.volume"));
    REQUIRE_NOTHROW(partitionGraphDocument(document));
    const ValueGraph::Graph graph{document.dump()};
    REQUIRE(graph.audioSources().size() == 1);
#else
    for (const auto &attempt : std::vector<std::function<void()>>{
             [&] { (void)partitionGraphDocument(document); },
             // The runtime's own entry: the value graph a rendering config
             // prepares is built from this constructor.
             [&] { const ValueGraph::Graph graph{document.dump()}; }}) {
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
