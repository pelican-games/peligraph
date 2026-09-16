#include "../src/core/animation/posegraph.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/posegraphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using Json = nlohmann::json;

namespace {
// One owned instance whose definition drives the object's skeletal sink. The
// scalars its pose operations read come from the values side of the very same
// document, so a test can see the one-way feed without a live scene.
Json poseInstanceDocument(Json definition_nodes, Json instance_inputs = Json{},
                          Json extra_root_nodes = Json::array()) {
    Json document = Json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{},
      "nodes":[{"id":"clock","op":"value.input","parameters":{"name":"time"},"outputs":["value"]}],
      "outputs":{}})");
    document["definitions"]["pose"] = {{"nodes", std::move(definition_nodes)},
                                       {"outputs", Json::object()}};
    for (auto &node : extra_root_nodes) document["nodes"].push_back(std::move(node));
    if (instance_inputs.is_null())
        instance_inputs = Json{{"time", {{"node", "clock"}, {"port", "value"}}}};
    document["nodes"].push_back(Json{{"id", "hero"}, {"op", "graph.instance"},
        {"definition", "pose"}, {"owner", {{"scene", "scenes/main"}, {"object", 7}}},
        {"inputs", std::move(instance_inputs)}});
    return document;
}

Json clipSampleChain() {
    return Json::parse(R"([
      {"id":"time","op":"graph.input"},
      {"id":"walk","op":"pose.clip_sample","parameters":{"rig":"models/hero.glb","clip":"Walk"},
       "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]},
      {"id":"out","op":"pose.skeletal","inputs":{"pose":{"node":"walk","port":"pose"}},"outputs":[]}])");
}

Json animGraphDocument() {
    return Json::parse(R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{"speed":0},
      "states":[{"name":"idle","type":"clip","clip":"Turn","loop":true},
                {"name":"walk","type":"clip","clip":"Step","loop":true}],
      "transitions":[{"from":"idle","to":"walk",
        "conditions":[{"parameter":"speed","op":">","value":0.5}]}],
      "initial_state":"idle"})");
}

// The instance the state-machine cases share: a curve on the values side feeds
// the declared `speed` port of the wrapped evaluator.
Json stateMachineDocument(bool escape_the_sink) {
    Json nodes = Json::array();
    nodes.push_back(Json::parse(R"({"id":"time","op":"graph.input"})"));
    nodes.push_back(Json::parse(R"({"id":"speed","op":"graph.input"})"));
    nodes.push_back(Json{{"id", "brain"}, {"op", "pose.state_machine"},
        {"parameters", {{"rig", "models/hero.glb"}, {"graph", animGraphDocument()}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                    {"speed", {{"node", "speed"}, {"port", "value"}}}}},
        {"outputs", {"pose", "state_events"}}});
    nodes.push_back(Json::parse(R"({"id":"out","op":"pose.skeletal",
      "inputs":{"pose":{"node":"brain","port":"pose"}},"outputs":[]})"));
    if (escape_the_sink)
        nodes.push_back(Json::parse(R"({"id":"echo","op":"pose.retarget",
          "parameters":{"profile":"profiles/vrm.json"},
          "inputs":{"pose":{"node":"brain","port":"pose"}},"outputs":["pose"]})"));
    Json gate = Json::array();
    gate.push_back(Json::parse(R"({"id":"gate","op":"value.curve",
      "inputs":{"time":{"node":"clock","port":"value"}},
      "parameters":{"keys":[{"time":0,"value":0},{"time":1,"value":1}]},"outputs":["value"]})"));
    return poseInstanceDocument(std::move(nodes),
        Json{{"time", {{"node", "clock"}, {"port", "value"}}},
             {"speed", {{"node", "gate"}, {"port", "value"}}}},
        std::move(gate));
}
} // namespace

TEST_CASE("the pose module puts a skeletal pose on a common-graph port",
          "[posegraph][graph-module]") {
    using namespace Pelican;
    const auto document = poseInstanceDocument(clipSampleChain());
    // Registration is all the graph learns: the partition routes operations by
    // the domain the module declared, never by their names.
    const auto partitions = partitionGraphDocument(document);
    REQUIRE(partitions.rendering.nodes.empty());
    std::set<std::string> animation_ops;
    for (const auto &node : partitions.animation.nodes)
        animation_ops.insert(node.at("op").get<std::string>());
    REQUIRE(animation_ops == std::set<std::string>{"pose.clip_sample", "pose.skeletal"});
    // Nothing pose-shaped reaches the value backend, and the scalar the sampler
    // reads is published by the values partition: the feed is one-way.
    std::set<std::string> value_ops;
    for (const auto &node : partitions.values.nodes)
        value_ops.insert(node.at("op").get<std::string>());
    REQUIRE(value_ops == std::set<std::string>{"value.input"});
    REQUIRE(partitions.values.outputs.contains(graphAnimationScalarOutput("hero/walk", "time")));
    REQUIRE_NOTHROW(ValueGraph::Graph{document.dump()});
    const Animation::PoseGraph graph{document};
    REQUIRE(graph.sinks().size() == 1);
    REQUIRE(graph.scalarOutputs() ==
            std::vector<std::string>{graphAnimationScalarOutput("hero/walk", "time")});

    // A registry that never registered pelican.pose -- what a build without the
    // animation unit has -- rejects the same document by name.
    const GraphModuleRegistry without_pose;
    REQUIRE(builtinGraphModuleRegistry().contains("pose.clip_sample"));
    REQUIRE_FALSE(without_pose.contains("pose.clip_sample"));
    try {
        (void)without_pose.resolveOperation(
            Json{{"id", "walk"}, {"op", "pose.clip_sample"}, {"parameters", Json::object()}});
        FAIL("an unregistered pose operation resolved");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.operation_unregistered@1");
        REQUIRE(error.detail().at("op") == "pose.clip_sample");
    }
}

TEST_CASE("the pose catalog publishes its own types and sinks", "[posegraph][graph-module]") {
    using namespace Pelican;
    const auto catalog = builtinGraphModuleRegistry().catalog();
    const nlohmann::ordered_json *pose = nullptr;
    for (const auto &module : catalog.at("modules"))
        if (module.at("namespace") == "pelican.pose") pose = &module;
    REQUIRE(pose != nullptr);
    std::set<std::string> types;
    for (const auto &type : pose->at("types")) types.insert(type.at("id").get<std::string>());
    REQUIRE(types == std::set<std::string>{"pelican.pose@1", "pelican.rig@1"});
    std::set<std::string> sinks;
    for (const auto &sink : pose->at("sinks")) sinks.insert(sink.get<std::string>());
    REQUIRE(sinks == std::set<std::string>{"pose.skeletal", "pose.transform"});
    for (const auto &operation : pose->at("operations")) {
        REQUIRE(operation.at("domain") == "animation");
        if (operation.at("op") == "pose.state_machine")
            REQUIRE(operation.at("evaluation") == "replay");
    }
}

TEST_CASE("one object has one pose writer", "[posegraph][graph-module]") {
    using namespace Pelican;
    auto nodes = clipSampleChain();
    nodes.push_back(Json::parse(R"({"id":"second","op":"pose.transform",
      "inputs":{"pose":{"node":"walk","port":"pose"}},"outputs":[]})"));
    const auto document = poseInstanceDocument(nodes);
    // The document is well typed -- two sinks accept a pose -- and is rejected
    // by the backend that would otherwise have to pick a writer.
    REQUIRE_NOTHROW(partitionGraphDocument(document));
    try {
        const Animation::PoseGraph graph{document};
        FAIL("two sinks claimed one object");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.sink_conflict@1");
        REQUIRE(error.detail().at("owner").at("object") == 7);
        REQUIRE(error.detail().at("first") == "hero/out");
        REQUIRE(error.detail().at("second") == "hero/second");
    }
    // One sink on the same object is accepted, so the rejection is about the
    // second writer and not about the shape of the document.
    REQUIRE_NOTHROW(Animation::PoseGraph{poseInstanceDocument(clipSampleChain())});
}

TEST_CASE("a pose cannot be blended across pose layouts", "[posegraph][graph-module]") {
    using namespace Pelican;
    auto nodes = Json::parse(R"([
      {"id":"time","op":"graph.input"},
      {"id":"walk","op":"pose.clip_sample","parameters":{"rig":"models/hero.glb","clip":"Walk"},
       "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]},
      {"id":"other","op":"pose.clip_sample","parameters":{"rig":"models/villain.glb","clip":"Walk"},
       "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]},
      {"id":"mix","op":"pose.blend","inputs":{"a":{"node":"walk","port":"pose"},
       "b":{"node":"other","port":"pose"},"weight":{"node":"time","port":"value"}},
       "outputs":["pose"]},
      {"id":"out","op":"pose.skeletal","inputs":{"pose":{"node":"mix","port":"pose"}},
       "outputs":[]}])");
    REQUIRE_NOTHROW(partitionGraphDocument(poseInstanceDocument(nodes)));
    try {
        const Animation::PoseGraph graph{poseInstanceDocument(nodes)};
        FAIL("two layouts were blended");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.layout_mismatch@1");
        REQUIRE(error.detail().at("node") == "hero/mix");
        REQUIRE(error.detail().at("expected").at("rig") == "models/hero.glb");
        REQUIRE(error.detail().at("observed").at("rig") == "models/villain.glb");
    }
    // The same blend between two clips of one rig is accepted, so the check is
    // about the layout and not about pose.blend itself.
    nodes[2]["parameters"]["rig"] = "models/hero.glb";
    nodes[2]["parameters"]["clip"] = "Run";
    REQUIRE_NOTHROW(Animation::PoseGraph{poseInstanceDocument(nodes)});
}

TEST_CASE("a state machine's parameters become graph input ports", "[posegraph][graph-module]") {
    using namespace Pelican;
    const auto document = stateMachineDocument(false);
    const auto partitions = partitionGraphDocument(document);
    // The declared parameter reaches the node as a port, so a curve on the
    // values side can drive a transition instead of GameContext alone.
    REQUIRE(partitions.values.outputs.contains(graphAnimationScalarOutput("hero/brain", "speed")));
    const Animation::PoseGraph graph{document};
    const auto *state_machine = graph.stateMachineFor(graph.sinks().front());
    REQUIRE(state_machine != nullptr);
    REQUIRE(state_machine->parameter_ports == std::vector<std::string>{"speed"});
    REQUIRE(state_machine->scalar_inputs.at("speed") ==
            graphAnimationScalarOutput("hero/brain", "speed"));

    // The wrapped evaluator owns the writer for its object, so routing its pose
    // anywhere but that object's own sink would make a second writer.
    try {
        const Animation::PoseGraph rejected{stateMachineDocument(true)};
        FAIL("a state machine pose escaped its own sink");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.sink_conflict@1");
        REQUIRE(error.detail().at("first") == "hero/brain");
    }
}
