#include "../src/core/userpublic/animation/curve.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "graph_composition_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace Pelican::Animation;
using Catch::Approx;

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
    document["nodes"][3]["inputs"]["gain"] = {{"node", "two"}, {"port", "value"}};
    REQUIRE_THROWS_AS(Pelican::ValueGraph::Graph(document.dump()), std::invalid_argument);
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
