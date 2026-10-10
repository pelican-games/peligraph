#include "../src/core/animation/posegraph.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/posegraphruntime.hpp"
#include "../src/core/model/skeletalanimation.hpp"
#include "../src/core/watch/userdataguard.hpp"
#include "../src/project/graphtemplate.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/posegraphmodule.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
        {"outputs", {"pose"}}});
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
        std::set<std::string> inputs;
        for (const auto &input : operation.value("inputs", nlohmann::ordered_json::array()))
            inputs.insert(input.at("port").get<std::string>());
        std::set<std::string> outputs;
        for (const auto &output : operation.value("outputs", nlohmann::ordered_json::array()))
            outputs.insert(output.at("port").get<std::string>());
        if (operation.at("op") == "pose.state_machine") {
            REQUIRE(operation.at("evaluation") == "replay");
            // No `events` input and no `state_events` output: nothing can emit
            // an event out of a pose operation, so the catalog does not offer a
            // port that cannot be connected.
            REQUIRE_FALSE(inputs.contains("events"));
            REQUIRE(outputs == std::set<std::string>{"pose"});
        }
    }
    // The pose.state_machine node used above resolves to the same port set the
    // catalog declares -- and refuses an `events` edge because the port is not
    // there, rather than accepting one and dropping it.
    const auto resolved = builtinGraphModuleRegistry().resolveOperation(
        Json{{"id", "brain"}, {"op", "pose.state_machine"},
             {"parameters", {{"rig", "models/hero.glb"}, {"graph", animGraphDocument()}}}});
    REQUIRE_FALSE(resolved.inputs.contains("events"));
    REQUIRE(resolved.inputs.contains("time"));
    REQUIRE(resolved.inputs.contains("speed"));
    REQUIRE(resolved.outputs.size() == 1);
    REQUIRE(resolved.outputs.contains("pose"));
}

TEST_CASE("a clip sample carries the component's playback parameters",
          "[posegraph][graph-module]") {
    using namespace Pelican;
    // speed / start_time / loop are node parameters of pose.clip_sample, with
    // the same defaults the animation component has. The contract validates
    // them, so an unusable value is named at composition instead of reaching a
    // frame.
    const auto node = [](Json parameters) {
        return Json{{"id", "walk"}, {"op", "pose.clip_sample"}, {"parameters", std::move(parameters)}};
    };
    const Json base{{"rig", "models/hero.glb"}, {"clip", "Walk"}};
    REQUIRE_NOTHROW(builtinGraphModuleRegistry().resolveOperation(node(base)));
    auto full = base;
    full["speed"] = 2.0;
    full["start_time"] = 0.5;
    full["loop"] = false;
    REQUIRE_NOTHROW(builtinGraphModuleRegistry().resolveOperation(node(full)));
    // CT-3: a value of the wrong kind is refused by name, from the
    // parameter table the catalog lists.
    const auto invalid = [&](const Json &parameters, const char *parameter, const char *reason) {
        try {
            (void)builtinGraphModuleRegistry().resolveOperation(node(parameters));
            FAIL("accepted " << parameters.dump());
        } catch (const GraphRegistryError &error) {
            REQUIRE(error.code() == "graph.render_node_parameter_invalid@1");
            REQUIRE(error.detail().at("parameter") == parameter);
            REQUIRE(error.detail().at("reason") == reason);
        }
    };
    auto bad_loop = base;
    bad_loop["loop"] = 1;
    invalid(bad_loop, "loop", "type");
    auto bad_speed = base;
    bad_speed["speed"] = "fast";
    invalid(bad_speed, "speed", "type");

    // The backend reads the same three through the same two functions, so a
    // node cannot mean one thing to the catalog and another to the evaluator.
    auto nodes = clipSampleChain();
    nodes[1]["parameters"]["speed"] = 2.0;
    nodes[1]["parameters"]["start_time"] = 0.25;
    nodes[1]["parameters"]["loop"] = false;
    const Animation::PoseGraph graph{poseInstanceDocument(nodes)};
    const Animation::PoseGraphNode *sampler = nullptr;
    for (const auto &candidate : graph.nodes())
        if (candidate.op == Animation::PoseGraphOp::clip_sample) sampler = &candidate;
    REQUIRE(sampler != nullptr);
    REQUIRE(sampler->speed == 2.0);
    REQUIRE(sampler->start_time == 0.25);
    REQUIRE_FALSE(sampler->loop);
    // The defaults live in one place: an unparameterised node reports them.
    const Animation::PoseGraph plain{poseInstanceDocument(clipSampleChain())};
    for (const auto &candidate : plain.nodes())
        if (candidate.op == Animation::PoseGraphOp::clip_sample) {
            REQUIRE(candidate.speed == 1.0);
            REQUIRE(candidate.start_time == 0.0);
            REQUIRE(candidate.loop);
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

// AG-2. Root motion is two node parameters on pose.clip_sample, and they change
// what the node costs to evaluate: a sampler that reports how far the root moved
// since the previous frame carries that frame with it, so replaying it means
// replaying the frames that led here.
TEST_CASE("root motion is a clip_sample parameter with its own classification",
          "[posegraph][graph-module][root-motion]") {
    using namespace Pelican;
    const auto node = [](Json parameters) {
        return Json{{"id", "walk"}, {"op", "pose.clip_sample"}, {"parameters", std::move(parameters)}};
    };
    const Json base{{"rig", "models/hero.glb"}, {"clip", "Advance"}};
    // The default is unchanged in every respect a document can observe.
    REQUIRE(builtinGraphModuleRegistry().resolveOperation(node(base)).evaluation ==
            GraphOperationEvaluation::direct);
    auto none = base;
    none["root_motion"] = "none";
    REQUIRE(builtinGraphModuleRegistry().resolveOperation(node(none)).evaluation ==
            GraphOperationEvaluation::direct);
    auto extract = base;
    extract["root_motion"] = "extract";
    extract["root_node"] = 0;
    const auto resolved = builtinGraphModuleRegistry().resolveOperation(node(extract));
    REQUIRE(resolved.evaluation == GraphOperationEvaluation::replay);
    // The ports are the ones the module declared; only the classification moved.
    REQUIRE(resolved.inputs.size() == 1);
    REQUIRE(resolved.inputs.contains("time"));
    REQUIRE(resolved.outputs.size() == 1);
    REQUIRE(resolved.outputs.contains("pose"));
    REQUIRE(resolved.parameter_change == GraphParameterChange::rebuild);
    REQUIRE(resolved.domain == GraphExecutionDomain::animation);

    // A factory may raise its node's classification and may never relax it. The
    // operation that would notice is the one whose registration already declares
    // more than its factory does.
    REQUIRE(builtinGraphModuleRegistry().resolveOperation(
                Json{{"id", "brain"}, {"op", "pose.state_machine"},
                     {"parameters", {{"rig", "models/hero.glb"}, {"graph", animGraphDocument()}}}})
                .evaluation == GraphOperationEvaluation::replay);

    auto unusable = base;
    unusable["root_motion"] = "sideways";
    try {
        (void)builtinGraphModuleRegistry().resolveOperation(node(unusable));
        FAIL("root motion sideways was accepted");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "graph.render_node_parameter_invalid@1");
        REQUIRE(error.detail().at("parameter") == "root_motion");
        REQUIRE(error.detail().at("reason") == "enum");
    }
    // Extracting without saying from which node is the same thing to the author
    // as naming a node the rig does not have, so it is the same name.
    auto nameless = base;
    nameless["root_motion"] = "extract";
    try {
        (void)builtinGraphModuleRegistry().resolveOperation(node(nameless));
        FAIL("root motion was extracted from no node at all");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.root_node_unknown@1");
        REQUIRE(error.detail().at("node") == "walk");
    }
    auto negative = nameless;
    negative["root_node"] = -1;
    REQUIRE_THROWS_AS(builtinGraphModuleRegistry().resolveOperation(node(negative)),
                      GraphRegistryError);

    // The backend reads the same two through the same two functions.
    auto nodes = clipSampleChain();
    nodes[1]["parameters"]["root_motion"] = "extract";
    nodes[1]["parameters"]["root_node"] = 3;
    const Animation::PoseGraph graph{poseInstanceDocument(nodes)};
    const Animation::PoseGraphNode *sampler = nullptr;
    for (const auto &candidate : graph.nodes())
        if (candidate.op == Animation::PoseGraphOp::clip_sample) sampler = &candidate;
    REQUIRE(sampler != nullptr);
    REQUIRE(sampler->root_motion);
    REQUIRE(sampler->root_node == 3);
}

// AG-2. One clip's root moves one object. The compile point is where that is
// decidable: the extracting node's pose has to arrive at exactly one skeletal
// sink, and nothing on the way may mix another pose into it.
TEST_CASE("root motion reaches exactly one skeletal sink",
          "[posegraph][graph-module][root-motion]") {
    using namespace Pelican;
    const auto extracting = [](Json nodes) {
        nodes[1]["parameters"]["root_motion"] = "extract";
        nodes[1]["parameters"]["root_node"] = 0;
        return nodes;
    };
    // The plain chain, and the same chain with a pass-through retarget, are the
    // two shapes that work.
    REQUIRE_NOTHROW(Animation::PoseGraph{poseInstanceDocument(extracting(clipSampleChain()))});
    auto retargeted = extracting(clipSampleChain());
    retargeted[2]["inputs"]["pose"] = {{"node", "profile"}, {"port", "pose"}};
    retargeted.push_back(Json::parse(R"({"id":"profile","op":"pose.retarget",
      "parameters":{"profile":"profiles/humanoid.json"},
      "inputs":{"pose":{"node":"walk","port":"pose"}},"outputs":["pose"]})"));
    REQUIRE_NOTHROW(Animation::PoseGraph{poseInstanceDocument(retargeted)});

    // A blend produces a pose whose root came from somewhere other than the
    // extracted delta, so the individual and its mesh would disagree.
    auto blended = extracting(clipSampleChain());
    blended[2]["inputs"]["pose"] = {{"node", "mix"}, {"port", "pose"}};
    blended.push_back(Json::parse(R"({"id":"idle","op":"pose.clip_sample",
      "parameters":{"rig":"models/hero.glb","clip":"Turn"},
      "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]})"));
    blended.push_back(Json::parse(R"({"id":"mix","op":"pose.blend",
      "inputs":{"a":{"node":"walk","port":"pose"},"b":{"node":"idle","port":"pose"},
                "weight":{"node":"time","port":"value"}},"outputs":["pose"]})"));
    try {
        const Animation::PoseGraph graph{poseInstanceDocument(blended)};
        FAIL("root motion was blended");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.root_motion_unsupported@1");
        REQUIRE(error.detail().at("node") == "hero/walk");
        REQUIRE(error.detail().at("detail").at("consumer") == "hero/mix");
    }

    // Two consumers: one clip's root would move more than one object, and
    // nothing says which. The same shape is what a second owner's sink reading
    // this node produces.
    auto shared = extracting(clipSampleChain());
    shared.push_back(Json::parse(R"({"id":"profile","op":"pose.retarget",
      "parameters":{"profile":"profiles/humanoid.json"},
      "inputs":{"pose":{"node":"walk","port":"pose"}},"outputs":["pose"]})"));
    try {
        const Animation::PoseGraph graph{poseInstanceDocument(shared)};
        FAIL("one clip root motion was shared");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.root_motion_shared@1");
        REQUIRE(error.detail().at("node") == "hero/walk");
    }

    // A chain that ends before it reaches a sink has no object to move.
    auto open = extracting(clipSampleChain());
    open[2]["inputs"]["pose"] = {{"node", "idle"}, {"port", "pose"}};
    open.push_back(Json::parse(R"({"id":"idle","op":"pose.clip_sample",
      "parameters":{"rig":"models/hero.glb","clip":"Turn"},
      "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]})"));
    open.push_back(Json::parse(R"({"id":"profile","op":"pose.retarget",
      "parameters":{"profile":"profiles/humanoid.json"},
      "inputs":{"pose":{"node":"walk","port":"pose"}},"outputs":["pose"]})"));
    try {
        const Animation::PoseGraph graph{poseInstanceDocument(open)};
        FAIL("root motion reached no sink");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "pose.root_motion_unsupported@1");
        REQUIRE(error.detail().at("detail").at("open") == "hero/profile");
    }

    // The same document without root motion compiles, so every rejection above
    // is about the extraction and not about the shape.
    REQUIRE_NOTHROW(Animation::PoseGraph{poseInstanceDocument(clipSampleChain())});
}

TEST_CASE("RX-1b pose graph recovers only marked user data failures",
          "[rx1][posegraph][animation][problems]") {
    using namespace Pelican;
    using namespace Pelican::Animation;
    enum class Failure { conflict, unsupported, contract, direct_contract, other_contract, data, engine };
    const auto failure = GENERATE(Failure::conflict, Failure::unsupported, Failure::contract,
        Failure::direct_contract, Failure::other_contract, Failure::data, Failure::engine);
    CAPTURE(failure);

    FastModuleContainer lifetime;
    auto &service = animationServiceRuntime();
    service.reset();
    struct ResetService {
        AnimationServiceRuntime &service;
        ~ResetService() { service.reset(); }
    } reset{service};
    SkeletalModelData model;
    model.nodes = {{.parent = -1, .name = "Root"}};
    model.joint_nodes = {0};
    model.inverse_bind_matrices = {glm::mat4{1.0f}};
    model.skin_bindings = {{"Body", 0, 1}};
    SkeletalAnimationClip clip;
    clip.name = "Walk";
    clip.start = 0.0f;
    clip.end = failure == Failure::unsupported ? 0.0f : 1.0f;
    clip.channels.push_back({.node = 0, .path = AnimationPath::translation,
        .times = {0.0f, 1.0f}, .values = {{0, 0, 0, 0}, {1, 0, 0, 0}}});
    model.clips.push_back(std::move(clip));
    service.registerObject("Hero", model);
    PoseGraphRuntime runtime;
    auto nodes = clipSampleChain();
    nodes[1]["parameters"]["root_motion"] = "extract";
    nodes[1]["parameters"]["root_node"] = 0;
    const auto graph = std::make_shared<const PoseGraph>(poseInstanceDocument(nodes));
    PoseGraphFrame frame;
    frame.frame_revision = 1;
    frame.object_name = [](const GraphInstanceOwner &) { return std::optional<std::string>{"Hero"}; };
    frame.component_writer = [&](const GraphInstanceOwner &) { return failure == Failure::conflict; };
    frame.root_transform_object = [](const GraphInstanceOwner &) { return std::optional<std::uint64_t>{7}; };
    frame.apply_root_translation = [](std::uint64_t, const double *) { return false; };
    for (const auto &scalar : graph->scalarOutputs()) frame.scalars[scalar] = 0.0;

    const auto problems = [&] { return GET_MODULE(ProblemList).problemsJson(); };
    const auto require_contract = [&](auto &&operation, const std::string &code, const Json &detail) {
        try {
            operation();
            FAIL("a coded pose contract rejection was swallowed");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == code);
            CHECK(error.detail() == detail);
            if (failure == Failure::contract || failure == Failure::direct_contract)
                CHECK(std::string{error.what()}.find("pose graph evaluation failed for Hero") != std::string::npos);
        }
        CHECK(problems().at("problems").empty());
    };
    if (failure == Failure::conflict || failure == Failure::unsupported) {
        const auto code = failure == Failure::conflict ? "pose.sink_conflict@1" : "pose.root_motion_unsupported@1";
        const auto detail = failure == Failure::conflict
            ? Json{{"owner", {{"scene", "scenes/main"}, {"object", 7}}},
                   {"first", "animation component"}, {"second", "hero/out"}}
            : Json{{"node", "hero/walk"}, {"reason", "root motion needs a clip with a duration"},
                   {"detail", {{"clip", "Walk"}}}};
        // A contract refusal must not poison the scope and disappear on retry.
        for (int attempt = 0; attempt < 2; ++attempt)
            require_contract([&] { runtime.synchronize(graph, frame); }, code, detail);
        return;
    }
    if (failure == Failure::data)
        frame.apply_root_translation = [](std::uint64_t, const double *) -> bool {
            throw UserDataError("authored pose evaluation failed");
        };
    if (failure == Failure::engine)
        frame.apply_root_translation = [](std::uint64_t, const double *) -> bool {
            throw std::logic_error("pose engine invariant");
        };
    if (failure == Failure::other_contract)
        frame.apply_root_translation = [](std::uint64_t, const double *) -> bool {
            throw GraphTemplateError("graph_template_invalid", {{"reason", "contract probe"}}, "coded refusal");
        };
    REQUIRE_NOTHROW(runtime.synchronize(graph, frame));
    REQUIRE(service.runAllPhases(frame.frame_revision) == Status::callback_failed);
    if (failure == Failure::data) {
        CHECK(runtime.recoverPendingUserDataFailures());
        const auto recorded = problems();
        REQUIRE(recorded.at("problems").size() == 1);
        CHECK(recorded.at("problems")[0].at("code") == "runtime.user_data@1");
        CHECK(recorded.at("problems")[0].at("message") ==
              "pose graph evaluation failed for Hero: authored pose evaluation failed");
        ++frame.frame_revision;
        REQUIRE_NOTHROW(runtime.synchronize(graph, frame));
        CHECK(service.runAllPhases(frame.frame_revision) == Status::ok);
        CHECK(problems() == recorded);
        runtime.retainOnly({});
        CHECK(problems().at("problems").empty());
    } else if (failure == Failure::engine) {
        CHECK_THROWS_AS(runtime.recoverPendingUserDataFailures(), std::logic_error);
        CHECK(problems().at("problems").empty());
    } else if (failure == Failure::other_contract) {
        try {
            (void)runtime.recoverPendingUserDataFailures();
            FAIL("another coded contract rejection was swallowed");
        } catch (const GraphTemplateError &error) {
            CHECK(error.code() == "graph_template_invalid");
            CHECK(error.fields() == nlohmann::ordered_json{{"reason", "contract probe"}});
            CHECK(std::string{error.what()} == "coded refusal");
        }
        CHECK(problems().at("problems").empty());
    } else {
        require_contract([&] {
            if (failure == Failure::direct_contract) runtime.rethrowPendingFailure();
            else (void)runtime.recoverPendingUserDataFailures();
        }, "pose.root_motion_no_transform@1",
           {{"owner", {{"scene", "scenes/main"}, {"object", 7}}}, {"node", "hero/out"}, {"object", "Hero"}});
    }
    CHECK_FALSE(runtime.recoverPendingUserDataFailures());
    CHECK(runtime.trace().empty());
}
