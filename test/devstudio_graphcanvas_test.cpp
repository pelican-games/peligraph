#include "audioanalysistables.hpp"
#include "graphcanvasmodel.hpp"
#include "graphcomposition.hpp"
#include "graphmodule.hpp"
#include "graphpartition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using PelicanStudio::GraphCanvasModel;
using PelicanStudio::GraphCanvasNode;
using PelicanStudio::InspectorNoticeKind;

// The real shipped-form document, not a hand-written miniature: the canvas has
// to read what the engine actually publishes for a `pelican.graph` project.
Json fixtureDocument() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                      "fixtures" / "graph_instance_inputs" / "passes" /
                      "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return Json::parse(buffer.str());
}

// What get_graph_instances answers for that document: the root nodes, the
// definition bodies, and the instances among the root nodes.
Json context(bool with_edge = true) {
    const auto document = fixtureDocument();
    auto nodes = document.at("nodes");
    if (with_edge) {
        // The fixture's `left` stores both values and connects neither. One
        // authored edge from the scene clock makes the root scope a graph with
        // an edge rather than two loose nodes.
        for (auto &node : nodes)
            if (node.at("id") == "left")
                node["inputs"] = {{"gain", {{"node", "clock"}, {"port", "value"}}}};
    }
    auto instances = Json::array();
    for (const auto &node : nodes)
        if (node.at("op") == "graph.instance") instances.push_back(node);
    return {{"source_reference", "fixture"},
            {"source_digest", {{"hex", "source-a"}}},
            {"instances", std::move(instances)},
            {"nodes", std::move(nodes)},
            {"definitions", document.at("definitions")}};
}

Json operation(const std::string &op, const std::string &domain,
               const Json &inputs, const Json &outputs, bool sink = false) {
    return {{"op", op}, {"domain", domain}, {"evaluation", "direct"},
            {"sink", sink}, {"ports", "declared"},
            {"inputs", inputs}, {"outputs", outputs}};
}

// The shapes the engine really publishes: a logical type pattern and a logical
// type both name the semantic type under "semantic".
Json declaredInput(const std::string &port, bool required = true,
                   const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port}, {"required", required},
            {"accepted_type", {{"semantic", type},
                               {"required_traits", Json::array()},
                               {"predicates", Json::array()}}}};
}

Json declaredOutput(const std::string &port,
                    const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port},
            {"type", {{"schema", "pelican.logical_type"}, {"version", 1},
                      {"constructor", "value"}, {"semantic", type},
                      {"arguments", Json::array()}}}};
}

Json typeSchema(const std::string &id, const Json &traits,
                const Json &display = Json{}) {
    Json schema{{"id", id}, {"constructor", "value"}, {"traits", traits},
                {"parameters", Json::array()}};
    if (!display.is_null()) schema["display"] = display;
    return schema;
}

// The catalog the builtin registry publishes for the operations the fixture
// uses. graph.instance is deliberately parameter_dependent: its ports follow
// the definition, so the catalog cannot name them.
Json catalog() {
    return {{"schema", "pelican.graph_catalog"}, {"version", 1},
        {"modules", Json::array({
            {{"namespace", "value"}, {"version", 1}, {"description", ""},
             {"types", Json::array({
                 typeSchema("pelican.value.scalar@1", Json::array({"numeric"}))})},
             {"sinks", Json::array()},
             {"operations", Json::array({
                 operation("value.input", "values", Json::array(),
                           Json::array({declaredOutput("value")})),
                 operation("value.add", "values",
                           Json::array({declaredInput("a"), declaredInput("b")}),
                           Json::array({declaredOutput("value")})),
                 {{"op", "graph.input"}, {"domain", "values"},
                  {"evaluation", "direct"}, {"sink", false},
                  {"ports", "parameter_dependent"}},
                 {{"op", "graph.instance"}, {"domain", "values"},
                  {"evaluation", "direct"}, {"sink", false},
                  {"ports", "parameter_dependent"}}})}},
            {{"namespace", "render"}, {"version", 1}, {"description", ""},
             {"types", Json::array({
                 typeSchema("pelican.render.storage_buffer@1",
                            Json::array({"shader_storage"}))})},
             {"sinks", Json::array()},
             {"operations", Json::array({
                 operation("render.buffer", "rendering",
                           Json::array({declaredInput("value")}),
                           Json::array({declaredOutput(
                               "resource", "pelican.render.storage_buffer@1")}),
                           true)})}}})}};
}

struct Harness {
    GraphCanvasModel model;
    // What get_graph_catalog answers on open().
    Json catalog_reply = catalog();

    PelicanStudio::InspectorRpcRequest take(std::string_view method) {
        auto requests = model.takeRpcRequests();
        const auto found = std::ranges::find(requests, std::string{method},
                                             &PelicanStudio::InspectorRpcRequest::method);
        REQUIRE(found != requests.end());
        return *found;
    }
    void reply(const PelicanStudio::InspectorRpcRequest &request, const Json &value) {
        model.receiveRpcResult(request.request_id, value.dump());
    }
    // Connecting asks for the document and the catalog on separate slots.
    void open(const Json &document = context(), bool with_catalog = true) {
        model.setConnected(true);
        auto requests = model.takeRpcRequests();
        REQUIRE(requests.size() == 2);
        for (const auto &request : requests) {
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, document.dump());
            else if (request.method == "get_graph_catalog" && with_catalog)
                model.receiveRpcResult(request.request_id, catalog_reply.dump());
        }
    }
    const GraphCanvasNode &node(const std::string &id) {
        static GraphCanvasNode empty;
        nodes_ = model.nodes();
        const auto found = std::ranges::find(nodes_, id, &GraphCanvasNode::id);
        REQUIRE(found != nodes_.end());
        return *found;
    }
    std::vector<GraphCanvasNode> nodes_;
};

// GL-1, decision 1: a document read is followed by the plan read, whatever
// the document holds. Nothing else is asked.
bool onlyPlanRead(GraphCanvasModel &model) {
    const auto requests = model.takeRpcRequests();
    return std::ranges::all_of(requests, [](const PelicanStudio::InspectorRpcRequest &request) {
        return request.method == "get_frame_plan";
    });
}

const PelicanStudio::GraphCanvasPort &port(const GraphCanvasNode &node,
                                           const std::string &name) {
    const auto found = std::ranges::find(node.inputs, name,
                                         &PelicanStudio::GraphCanvasPort::port);
    REQUIRE(found != node.inputs.end());
    return *found;
}
} // namespace

TEST_CASE("The graph canvas reads root and definition scopes out of one document",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();

    // Only the three reads the canvas can justify ever leave it (and, GL-1,
    // the plan its folded nodes are drawn from).
    REQUIRE(onlyPlanRead(h.model));

    auto scopes = h.model.scopes();
    REQUIRE(scopes.size() == 2);
    REQUIRE(scopes[0].root);
    REQUIRE(scopes[0].label == "root");
    REQUIRE(scopes[1].id == "gain_curve");

    REQUIRE(h.model.currentScope().empty());
    REQUIRE(h.model.nodes().size() == 2);
    REQUIRE(h.model.edges().size() == 1);
    const auto root_edge = h.model.edges().front();
    REQUIRE(root_edge.from_node == "clock");
    REQUIRE(root_edge.from_port == "value");
    REQUIRE(root_edge.to_node == "left");
    REQUIRE(root_edge.to_port == "gain");

    // The catalog's declared contract supplies the ports of an ordinary node.
    const auto &clock = h.node("clock");
    REQUIRE(clock.domain == "values");
    REQUIRE(clock.evaluation == "direct");
    REQUIRE_FALSE(clock.sink);
    REQUIRE(clock.outputs.size() == 1);
    REQUIRE(clock.outputs[0].port == "value");
    // The socket's type is the catalog's, read off the declared contract.
    REQUIRE(clock.outputs[0].type == "pelican.value.scalar@1");
    // A node with no authored label is headed by its id, not by an empty band.
    REQUIRE(clock.label.empty());
    REQUIRE(clock.heading() == "clock");

    // graph.instance is parameter_dependent, so its ports come from the node
    // and its definition, and they are not labelled as declared.
    const auto &left = h.node("left");
    REQUIRE(left.instance);
    REQUIRE(left.definition == "gain_curve");
    REQUIRE(left.inputs.size() == 2);
    REQUIRE(left.inputs[0].port == "gain");
    REQUIRE(left.inputs[1].port == "bias");
    REQUIRE_FALSE(left.inputs[0].declared);
    REQUIRE(left.inputs[0].connected);
    REQUIRE(left.inputs[0].source_node == "clock");
    REQUIRE(left.inputs[1].has_parameter);
    REQUIRE(left.inputs[1].parameter == 5);
    REQUIRE(left.outputs.size() == 1);
    REQUIRE(left.outputs[0].port == "value");
    // An instance's ports are the definition's boundary: its inputs are the
    // graph.input scalars, and its output follows the definition's own
    // outputs map down to the node that really produces it.
    REQUIRE(left.inputs[0].type == "pelican.value.scalar@1");
    REQUIRE(left.outputs[0].type == "pelican.value.scalar@1");

    // Descending into the instance shows the definition body.
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE(h.model.currentScope() == "gain_curve");
    REQUIRE(h.model.breadcrumb() == std::vector<std::string>{"", "gain_curve"});
    REQUIRE(h.model.nodes().size() == 4);
    REQUIRE(h.model.edges().size() == 3);

    const auto &sum = h.node("sum");
    REQUIRE(sum.inputs.size() == 2);
    REQUIRE(sum.inputs[0].port == "a");
    REQUIRE(sum.inputs[0].declared);
    REQUIRE(sum.inputs[0].connected);
    REQUIRE(sum.inputs[0].source_node == "gain");
    REQUIRE(sum.label.empty());
    REQUIRE(sum.heading() == "sum");
    REQUIRE(sum.op == "value.add");
    // graph.input is structural: no module registers it, so its scalar output
    // comes from what composition expands it into, not from the catalog.
    const auto &gain_input = h.node("gain");
    REQUIRE(gain_input.outputs.size() == 1);
    REQUIRE(gain_input.outputs[0].port == "value");
    REQUIRE(gain_input.outputs[0].type == "pelican.value.scalar@1");
    const auto &buffer = h.node("buffer");
    REQUIRE(buffer.domain == "rendering");
    // The document's own `label` is what the heading shows; the type name is
    // still the op, so a renamed node never hides what it is.
    REQUIRE(buffer.label == "pulse buffer");
    REQUIRE(buffer.heading() == "pulse buffer");
    REQUIRE(buffer.op == "render.buffer");
    REQUIRE(buffer.sink);
    REQUIRE(port(buffer, "value").declared);
    REQUIRE(buffer.outputs.size() == 1);
    REQUIRE(buffer.outputs[0].port == "resource");
    REQUIRE(buffer.outputs[0].type == "pelican.render.storage_buffer@1");
    // The catalog's type schemas came across with the operations.
    REQUIRE(h.model.typeSchema("pelican.render.storage_buffer@1") != nullptr);
    REQUIRE(h.model.typeSchema("pelican.pose@1") == nullptr);

    REQUIRE(h.model.leaveScope());
    REQUIRE(h.model.currentScope().empty());
    REQUIRE_FALSE(h.model.leaveScope());

    REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);
}

// P-2b acceptance 4. The shipped learning project's own rendering config, read
// the way Studio reads one: a canned `get_graph_instances` reply. The canvas
// has to find the definition scope and the owned instance in it, because that
// is what a reader opens the demo to look at.
TEST_CASE("The graph canvas lists the posegraph_demo scope and its instance",
          "[studio][graph-canvas][posegraph-demo]") {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
                      "posegraph_demo" / "passes" / "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const auto document = Json::parse(buffer.str());
    auto instances = Json::array();
    for (const auto &node : document.at("nodes"))
        if (node.at("op") == "graph.instance") instances.push_back(node);
    const Json demo_context{{"source_reference", "projects/posegraph_demo/passes/main.json"},
                            {"source_digest", {{"hex", "posegraph-demo"}}},
                            {"instances", instances},
                            {"nodes", document.at("nodes")},
                            {"definitions", document.at("definitions")}};

    Harness h;
    h.open(demo_context);
    auto scopes = h.model.scopes();
    REQUIRE(scopes.size() == 2);
    REQUIRE(scopes[0].root);
    REQUIRE(scopes[1].id == "walk_run");

    // The values side of the root: a clock, the playback that wraps it, the
    // curve, and the instance the curve drives.
    REQUIRE(h.model.nodes().size() == 4);
    REQUIRE(h.model.edges().size() == 3);
    const auto &hero = h.node("hero_walk_run");
    REQUIRE(hero.instance);
    REQUIRE(hero.definition == "walk_run");
    REQUIRE(hero.inputs.size() == 1);
    REQUIRE(hero.inputs[0].port == "weight");
    REQUIRE(hero.inputs[0].connected);
    REQUIRE(hero.inputs[0].source_node == "blend_curve");
    // The parameter under the edge is the value the port returns to when the
    // edge is taken away, which is the exercise the README sets.
    REQUIRE(hero.inputs[0].has_parameter);
    REQUIRE(hero.inputs[0].parameter == 0.0);

    // Descending into the instance shows the pose chain itself.
    REQUIRE(h.model.enterInstance("hero_walk_run"));
    REQUIRE(h.model.currentScope() == "walk_run");
    REQUIRE(h.model.breadcrumb() == std::vector<std::string>{"", "walk_run"});
    REQUIRE(h.model.nodes().size() == 6);
    const auto &mix = h.node("mix");
    REQUIRE(port(mix, "a").source_node == "walk");
    REQUIRE(port(mix, "b").source_node == "run");
    REQUIRE(port(mix, "weight").source_node == "weight");
    const auto &sink = h.node("out");
    REQUIRE(port(sink, "pose").source_node == "mix");
}

TEST_CASE("A document without definitions leaves the root scope standing",
          "[studio][graph-canvas]") {
    Harness h;
    // The control for the scope list: remove `definitions` from the reply.
    auto document = context();
    document.erase("definitions");
    h.open(document);

    REQUIRE(h.model.scopes().size() == 1);
    REQUIRE(h.model.scopes().front().root);
    // The canvas does not fall over: the root nodes and the edge are still there.
    REQUIRE(h.model.nodes().size() == 2);
    REQUIRE(h.model.edges().size() == 1);
    // And an instance whose definition body is unknown cannot be descended into.
    REQUIRE_FALSE(h.model.enterInstance("left"));
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
    REQUIRE(h.model.notice().message.find("no definitions") != std::string::npos);

    // The ports of the instance are still readable, from the node's own keys.
    const auto &left = h.node("left");
    REQUIRE(left.inputs.size() == 2);
    REQUIRE_FALSE(left.inputs[0].declared);
}

TEST_CASE("Without the catalog the canvas draws the document's own keys and says so",
          "[studio][graph-canvas]") {
    Harness h;
    // The control for the declared ports: never answer get_graph_catalog.
    h.open(context(), false);

    REQUIRE_FALSE(h.model.catalogReceived());
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
    REQUIRE(h.model.notice().message.find("catalog not fetched") != std::string::npos);

    // No contract: no domain, no evaluation, and the ports are only what the
    // document itself names.
    REQUIRE(h.model.enterInstance("left"));
    const auto &sum = h.node("sum");
    REQUIRE_FALSE(sum.contract_known);
    REQUIRE(sum.domain.empty());
    REQUIRE(sum.inputs.size() == 2);
    REQUIRE_FALSE(sum.inputs[0].declared);
    REQUIRE_FALSE(sum.inputs[1].declared);
    REQUIRE(sum.outputs.size() == 1);
    REQUIRE(sum.outputs[0].port == "value");
    // Without a contract there is no type to put on the socket, and the canvas
    // says so rather than picking one.
    REQUIRE(sum.outputs[0].type.empty());
    REQUIRE(sum.inputs[0].type.empty());
    // `gain` declares no inputs in the document, so without a contract it
    // draws with none rather than invented ones. Its single output is the one
    // thing a graph.input always has, which is a fact about composition and
    // not about the catalog.
    const auto &gain = h.node("gain");
    REQUIRE(gain.inputs.empty());
    REQUIRE(gain.outputs.size() == 1);
    REQUIRE(gain.outputs[0].port == "value");
    // The edges are the document's, so the shape of the graph is unchanged.
    REQUIRE(h.model.edges().size() == 3);
}

TEST_CASE("Published values land on the instance's input ports",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();

    REQUIRE(h.model.valueInstances() == std::vector<std::string>{"left"});
    h.model.watchInstance("left");
    const auto request = h.take("get_graph_values");
    REQUIRE(request.params == Json{{"instance", "left"}});
    h.reply(request, {{"source_digest", {{"hex", "source-a"}}},
        {"instance", "left"}, {"published", true}, {"generation", 3}, {"frame", 9},
        {"inputs", Json::array({
            {{"port", "gain"}, {"value", 1.25}, {"source", "connected"},
             {"connected", {{"node", "clock"}, {"port", "value"}}}},
            {{"port", "bias"}, {"value", 7.0}, {"source", "override"},
             {"parameter", "left/bias"}}})}});

    REQUIRE(h.model.valuesPublished());
    const auto &left = h.node("left");
    REQUIRE(port(left, "gain").has_value);
    REQUIRE(port(left, "gain").value == 1.25);
    REQUIRE(port(left, "gain").value_source == "connected");
    REQUIRE(port(left, "gain").value_connected_node == "clock");
    REQUIRE(port(left, "bias").value == 7.0);
    REQUIRE(port(left, "bias").value_source == "override");
    // The document's own stored value is still what the port returns to.
    REQUIRE(port(left, "bias").has_parameter);
    REQUIRE(port(left, "bias").parameter == 5);
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);

    // A node inside a definition never takes the overlay: get_graph_values
    // answers for an instance, not for the nodes of its body (AG-3).
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE_FALSE(port(h.node("sum"), "a").has_value);
}

TEST_CASE("An unpublished graph and a failed value read both leave the canvas standing",
          "[studio][graph-canvas]") {
    SECTION("published: false") {
        Harness h;
        h.open();
        h.model.watchInstance("left");
        h.reply(h.take("get_graph_values"),
                {{"instance", "left"}, {"published", false}, {"generation", 0},
                 {"frame", 0}, {"inputs", Json::array()}});
        REQUIRE_FALSE(h.model.valuesPublished());
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
        REQUIRE(h.model.notice().message.find("not published") != std::string::npos);
        REQUIRE(h.model.nodes().size() == 2);
        REQUIRE_FALSE(port(h.node("left"), "gain").has_value);
    }
    SECTION("the read fails") {
        Harness h;
        h.open();
        h.model.watchInstance("left");
        const auto request = h.take("get_graph_values");
        h.model.receiveRpcFailure(request.request_id, "engine is gone");
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Error);
        REQUIRE(h.model.notice().message == "engine is gone");
        // The canvas is the document's, so it survives a missing report.
        REQUIRE(h.model.nodes().size() == 2);
        REQUIRE(h.model.edges().size() == 1);
        REQUIRE(h.model.scopes().size() == 2);
    }
}

TEST_CASE("A refresh keeps the scope trail only while the definition is still there",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE(h.model.currentScope() == "gain_curve");

    // Same document: the trail survives a refresh.
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), context());
    REQUIRE(h.model.currentScope() == "gain_curve");

    // A document that dropped the definition cannot keep showing its body.
    auto without = context();
    without["definitions"] = Json::object();
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), without);
    REQUIRE(h.model.currentScope().empty());
    REQUIRE(h.model.scopes().size() == 1);
    // allNodeKeys is what the scene prunes retained positions against.
    REQUIRE(h.model.allNodeKeys().size() == 2);
}

// --------------------------------------------------------------------------
// AG-3b: the node values overlay, in both scopes.

namespace {
// What AG-3a answers. `instance` is echoed back: the empty string is the root
// scope's question, and `node` is the id inside that scope while `expanded` is
// the whole path.
Json nodeValues(const std::string &instance, const Json &nodes,
                bool published = true, double frame = 12) {
    return {{"instance", instance}, {"generation", 4}, {"frame", frame},
            {"published", published}, {"source", "rendering"},
            {"nodes", nodes}};
}

Json reportedNode(const std::string &node, const std::string &expanded,
                  const Json &outputs) {
    return {{"node", node}, {"expanded", expanded}, {"outputs", outputs}};
}

// Answer whatever observation pair is in the air, so the next thing the test
// asks for is asked rather than skipped: a question already outstanding is not
// asked twice.
void answerObservation(Harness &h,
                       const std::vector<PelicanStudio::InspectorRpcRequest> &requests) {
    for (const auto &request : requests) {
        if (request.method == "get_status")
            h.reply(request, {{"time", 0.0}, {"frame", 0},
                              {"animation", {{"available", false}}}});
        else if (request.method == "get_graph_node_values")
            h.reply(request, {{"instance", request.params.value("instance", std::string{})},
                              {"generation", 0}, {"frame", 0},
                              {"published", false}, {"source", "rendering"},
                              {"nodes", Json::array()}});
    }
}

void settleObservation(Harness &h) {
    answerObservation(h, h.model.takeRpcRequests());
}

const PelicanStudio::GraphCanvasOutput &output(const GraphCanvasNode &node,
                                               const std::string &name) {
    const auto found = std::ranges::find(
        node.outputs, name, &PelicanStudio::GraphCanvasOutput::port);
    REQUIRE(found != node.outputs.end());
    return *found;
}
} // namespace

TEST_CASE("The node values overlay reaches a definition body and the root scope",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    // Naming the instance is what turns the overlay on. Nothing is read for a
    // canvas nobody asked to observe (GL-1: but the plan, with the document).
    REQUIRE(onlyPlanRead(h.model));
    h.model.watchInstance("left");

    SECTION("a definition body, read through the instance that owns it") {
        // The root scope's question went out on the selection. Descending asks
        // a different one, and only the newest answer of a kind is kept.
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        const auto request = h.take("get_graph_node_values");
        // The scope decides the question: a definition body is read through
        // the instance, the root document through no instance at all.
        REQUIRE(request.params == Json{{"instance", "left"}});
        h.reply(request,
                nodeValues("left", Json::array({
                    reportedNode("gain", "left/gain", {{"value", 2.5}}),
                    reportedNode("bias", "left/bias", {{"value", 5.0}}),
                    reportedNode("sum", "left/sum", {{"value", 7.5}}),
                    // A rendering-domain node has no value of its own. It is
                    // listed with no outputs, which is not the same as absent.
                    reportedNode("buffer", "left/buffer", Json::object())})));

        REQUIRE(h.model.nodeValuesPublished());
        REQUIRE(output(h.node("sum"), "value").has_value);
        REQUIRE(output(h.node("sum"), "value").value == 7.5);
        REQUIRE(output(h.node("gain"), "value").value == 2.5);
        // outputs: {} is a node with no band, not a node with a zero.
        REQUIRE_FALSE(output(h.node("buffer"), "resource").has_value);
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);
    }

    SECTION("the root scope, read with no instance at all") {
        const auto request = h.take("get_graph_node_values");
        REQUIRE(request.params == Json::object());
        h.reply(request,
                nodeValues("", Json::array({
                    // A root node answers under its own id: node == expanded.
                    reportedNode("clock", "clock", {{"value", 0.5}}),
                    // The instance body is in the same reply, under the path
                    // that reaches it. No canvas node is called that, so
                    // nothing of the body leaks onto the root drawing.
                    reportedNode("left/sum", "left/sum", {{"value", 7.5}})})));

        REQUIRE(output(h.node("clock"), "value").has_value);
        REQUIRE(output(h.node("clock"), "value").value == 0.5);
        // `left` is a graph.instance: the expansion has no node by that name,
        // so its declared outputs carry no band.
        REQUIRE_FALSE(output(h.node("left"), "value").has_value);
    }

    SECTION("published: false says so and draws no band") {
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        h.reply(h.take("get_graph_node_values"),
                nodeValues("left", Json::array(), false));
        REQUIRE_FALSE(h.model.nodeValuesPublished());
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
        REQUIRE(h.model.notice().message.find("not published") != std::string::npos);
        REQUIRE_FALSE(output(h.node("sum"), "value").has_value);
    }

    SECTION("an answer about another scope is not drawn against these nodes") {
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        // The reply names the question it answered, and it is the root's.
        h.reply(h.take("get_graph_node_values"),
                nodeValues("", Json::array({
                    reportedNode("sum", "sum", {{"value", 99.0}})})));
        // The reply answers the root's question; the canvas is showing a
        // definition body. A number drawn against the wrong node list is worse
        // than no number.
        REQUIRE_FALSE(output(h.node("sum"), "value").has_value);
    }
}

TEST_CASE("Live reads on a clock and off it the overlay follows the document",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    settleObservation(h);

    // Off: polling asks for nothing. The catalog is already in hand and no
    // edit is outstanding, so the slot stays empty.
    h.model.poll();
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());

    // Every tick of the fake clock is one pair of reads, and the pair has to
    // be answered before the next one goes out: a question already in the air
    // is not asked again, so a slow engine cannot be made to queue reads.
    const auto takePair = [&] {
        auto requests = h.model.takeRpcRequests();
        REQUIRE(requests.size() == 2);
        REQUIRE(requests[0].method == "get_status");
        REQUIRE(requests[1].method == "get_graph_node_values");
        answerObservation(h, requests);
    };
    h.model.setLive(true);
    takePair();
    for (int tick = 0; tick < 3; ++tick) {
        h.model.poll();
        takePair();
    }

    // A tick while the previous pair is still unanswered asks nothing.
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().size() == 2);
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());
    settleObservation(h);

    h.model.setLive(false);
    (void)h.model.takeRpcRequests();
    h.model.poll();
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());
}

TEST_CASE("The Timeline buffers what Live watched go by",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    REQUIRE(h.model.selectScope("gain_curve"));
    h.model.setTimelineNode("sum");
    settleObservation(h);
    h.model.setLive(true);

    const auto tick = [&](double time, double value) {
        for (const auto &request : h.model.takeRpcRequests()) {
            if (request.method == "get_status")
                h.reply(request, {{"time", time}, {"frame", 3},
                                  {"animation", {{"available", false}}}});
            else if (request.method == "get_graph_node_values")
                h.reply(request, nodeValues("left", Json::array({
                    reportedNode("sum", "left/sum", {{"value", value}})})));
        }
        h.model.poll();
    };
    tick(0.25, 7.5);
    tick(0.50, 8.0);
    tick(0.75, 8.5);

    REQUIRE(h.model.timelinePorts() == std::vector<std::string>{"value"});
    const auto &samples = h.model.timelineSamples();
    REQUIRE(samples.size() == 3);
    REQUIRE(samples[0].time == 0.25);
    REQUIRE(samples[0].values == std::vector<double>{7.5});
    REQUIRE(samples[2].time == 0.75);
    REQUIRE(samples[2].values == std::vector<double>{8.5});
    REQUIRE_FALSE(samples[2].has_phase);

    // A different node is a different strip: the lines would otherwise change
    // what they meant halfway along.
    h.model.setTimelineNode("gain");
    REQUIRE(h.model.timelineSamples().empty());
    REQUIRE(h.model.timelinePorts().empty());
}

TEST_CASE("A scrub is set_time and one drawn frame, in that order",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    settleObservation(h);

    REQUIRE(h.model.scrub(1.5));
    // A second scrub while the first is in the air would race it.
    REQUIRE_FALSE(h.model.scrub(2.0));
    auto requests = h.model.takeRpcRequests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].method == "set_time");
    REQUIRE(requests[0].params == Json{{"t", 1.5}});
    REQUIRE(requests[1].method == "render_frame");
    REQUIRE(requests[1].params == Json::object());
    // The scrub does not take the edit slot: a drag on the Timeline must not
    // make the canvas look busy to an edit trying to land.
    REQUIRE(h.model.canEdit());

    // The frame that was asked for has been drawn; the values are read back.
    h.model.receiveRpcResult(requests[1].request_id,
                             Json{{"frame", 90}, {"time", 1.5}}.dump());
    REQUIRE_FALSE(h.model.scrubbing());
    auto after = h.model.takeRpcRequests();
    REQUIRE(after.size() == 2);
    REQUIRE(after[0].method == "get_status");
    REQUIRE(after[1].method == "get_graph_node_values");
}

TEST_CASE("The animation band is drawn only for the watched instance owner",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    // The fixture's `left` is owned by main/41.
    const Json sinks = Json::array({
        Json{{"owner", {{"scene", "main"}, {"object", 41}}},
             {"node", "left/pose"}, {"source", "pose_graph_clip"},
             {"cursors", Json::array({Json{{"clip", "Turn"},
                                           {"time_seconds", 0.5},
                                           {"normalized_phase", 0.25}}})},
             {"transition", {{"active", true}, {"target", "walk"},
                             {"progress", 0.4}}}},
        Json{{"owner", {{"scene", "main"}, {"object", 99}}},
             {"node", "other/pose"}, {"source", "component_clip"},
             {"cursors", Json::array()}}});
    for (const auto &request : h.model.takeRpcRequests())
        if (request.method == "get_status")
            h.reply(request, {{"time", 0.5}, {"frame", 30},
                              {"animation", {{"available", true},
                                             {"sinks", sinks}}}});

    REQUIRE(h.model.animationAvailable());
    const auto owned = h.model.animationSinks();
    REQUIRE(owned.size() == 1);
    REQUIRE(owned[0].node == "left/pose");
    REQUIRE(owned[0].source == "pose_graph_clip");
    REQUIRE(owned[0].cursors.size() == 1);
    REQUIRE(owned[0].cursors[0].clip == "Turn");
    REQUIRE(owned[0].cursors[0].normalized_phase == 0.25);
    REQUIRE(owned[0].transition_active);
    REQUIRE(owned[0].transition_progress == 0.4);

    // Watching an instance the status has no sink for draws no band at all --
    // which is not the same as "the engine reported none".
    h.model.watchInstance("");
    REQUIRE(h.model.animationSinks().empty());
}

// Q-2. The canvas's 250 ms clock keeps running -- it is also what flushes the
// model's request queue after a scope change -- so what matters is that a tick
// asks the engine for something only when one of the three things that read on
// a clock is outstanding: the catalog, Live, or an edit ticket. That is this
// predicate, and it is the reason the canvas costs zero RPC per second while
// idle.
TEST_CASE("The graph canvas polls only for the catalog, Live, or an edit ticket",
          "[studio][graph-canvas][poll][idle][negative-contrast][q2]") {
    Harness h;
    // Disconnected: there is no one to ask.
    REQUIRE_FALSE(h.model.pollWanted());

    // Connecting asks for the document and the catalog itself, on their own
    // slots -- the clock is not what fetches them.
    h.model.setConnected(true);
    auto requests = h.model.takeRpcRequests();
    REQUIRE(requests.size() == 2);
    std::uint64_t catalog_request = 0;
    for (const auto &request : requests) {
        if (request.method == "get_graph_instances")
            h.model.receiveRpcResult(request.request_id, context().dump());
        else if (request.method == "get_graph_catalog")
            catalog_request = request.request_id;
    }
    REQUIRE(catalog_request != 0);

    // A catalog that did not arrive is the one thing a clock has to retry.
    h.model.receiveRpcFailure(catalog_request, "the engine dropped it");
    REQUIRE(h.model.pollWanted());
    h.model.poll();
    h.model.receiveRpcResult(h.take("get_graph_catalog").request_id,
                             catalog().dump());

    // Catalog in hand, Live off, nothing edited: nothing left to ask, and the
    // poll issues nothing even when it is called.
    REQUIRE_FALSE(h.model.pollWanted());
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());

    // Live reads on a clock by definition.
    h.model.setLive(true);
    REQUIRE(h.model.pollWanted());
    h.model.setLive(false);
    REQUIRE_FALSE(h.model.pollWanted());
}

namespace {
// Run emitted edits through the production document editor. This exercises
// Undo's data restoration as well as the asynchronous ticket/CAS boundary.
struct GraphHistoryHarness {
    Harness client;
    Json document = fixtureDocument();
    int generation = 0;

    std::string digest() const { return "revision-" + std::to_string(generation); }
    Json snapshot() const {
        return {{"source_reference", "fixture"}, {"root_schema", "pelican.graph"},
                {"source_digest", {{"hex", digest()}}},
                {"nodes", document.at("nodes")},
                {"definitions", document.at("definitions")},
                {"layout", document.value("layout", Json::object())}};
    }
    void open() { client.open(snapshot()); }
    void refresh() {
        client.reply(client.take("get_graph_instances"), snapshot());
    }
    // LP-4c: the op list the last commit() sent (GL-1d: and all it sent).
    Json last_ops;
    Json last_params;
    void commit() {
        const auto submit = client.take("edit_graph_document");
        REQUIRE(submit.params.at("base_source_digest") == digest());
        last_ops = submit.params.at("ops");
        last_params = submit.params;
        auto edited = Pelican::editGraphDocument(document, submit.params.at("ops"));
        client.reply(submit, {{"status", "accepted"}, {"ticket", "edit"}});
        REQUIRE_FALSE(client.model.canUndo());
        const auto poll = client.take("get_edit_result");
        document = std::move(edited);
        ++generation;
        client.reply(poll, {{"status", "committed"}, {"source_digest", {{"hex", digest()}}}});
        refresh();
    }
};
}

// Q-13. A placement is a commit like any other, and so is every other panel's
// edit: the digest a debounced set_layout carries can be one revision behind.
// The placement is the canvas's own, so it goes once more against the next
// read; a gesture built from the old nodes does not.
TEST_CASE("Q-13 a placement refused for a moved document is written once more "
          "against the revision read next",
          "[studio][graph-canvas][q13]") {
    GraphHistoryHarness h;
    h.open();
    const Json moved{{"status", "rejected"},
                     {"committed", false},
                     {"error",
                      {{"code", "external_modification"},
                       {"message", "render config base digest no longer "
                                   "matches the engine-owned document"}}}};
    const auto edits = [&h] {
        auto requests = h.client.model.takeRpcRequests();
        return std::ranges::count(requests, std::string{"edit_graph_document"},
                                  &PelicanStudio::InspectorRpcRequest::method);
    };

    SECTION("a placement") {
        const Json placement{{"nodes", {{"left", {{"x", 70}, {"y", 80}}}}}};
        REQUIRE(h.client.model.setLayout("", placement));
        const auto first = h.client.take("edit_graph_document");
        CHECK(first.params.at("base_source_digest") == h.digest());
        // Another panel's commit landed first.
        ++h.generation;
        h.client.reply(first, moved);
        h.refresh();
        const auto again = h.client.take("edit_graph_document");
        CHECK(again.params.at("base_source_digest") == h.digest());
        CHECK(again.params.at("ops") == first.params.at("ops"));
        // Refused a second time: once is all it goes again.
        ++h.generation;
        h.client.reply(again, moved);
        h.refresh();
        CHECK(edits() == 0);
    }

    SECTION("a node edit is dropped, as it always was") {
        REQUIRE(h.client.model.setNodeLabel("clock", "Scene clock"));
        const auto first = h.client.take("edit_graph_document");
        ++h.generation;
        h.client.reply(first, moved);
        h.refresh();
        CHECK(edits() == 0);
    }
}

TEST_CASE("Graph Undo restores deleted nodes, order and placement together",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.document["layout"] = {{"nodes", {
        {"clock", {{"x", 11}, {"y", 12}}},
        {"left", {{"x", 40}, {"y", 50}}}}}};
    const auto before = h.document;
    h.open();
    REQUIRE(h.client.model.removeNode("clock"));
    h.commit();
    REQUIRE_FALSE(h.document.at("layout").at("nodes").contains("clock"));
    const auto removed = h.document;
    REQUIRE(h.client.model.canUndo());
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.document == before);
    REQUIRE_FALSE(h.client.model.canUndo());
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.redo());
    h.commit();
    REQUIRE(h.document == removed);
}

TEST_CASE("Graph Undo preserves complete nodes and groups seeded additions",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    const auto before = h.document;
    SECTION("parameter and label edits restore absent fields") {
        REQUIRE(h.client.model.setNodeLabel("clock", "Scene clock"));
        h.commit();
        REQUIRE(h.client.model.setNodeParameter("clock", "name", "other_time"));
        h.commit();
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document.at("nodes")[0].at("label") == "Scene clock");
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
    }
    SECTION("one add gesture includes its seed constants") {
        REQUIRE(h.client.model.addNode("value.add"));
        h.commit();
        REQUIRE(h.document.at("nodes").size() == before.at("nodes").size() + 3);
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
        REQUIRE(h.client.model.redo());
        h.commit();
        REQUIRE(h.document.at("nodes").size() == before.at("nodes").size() + 3);
    }
    SECTION("definition edits stay in their own scope") {
        REQUIRE(h.client.model.selectScope("gain_curve"));
        REQUIRE(h.client.model.setNodeLabel("sum", "Sum"));
        h.commit();
        REQUIRE(h.client.model.selectScope(""));
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
    }
}

TEST_CASE("Graph Undo restores disconnected edges and their reroutes",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.document["nodes"][1]["inputs"]["gain"] = {{"node", "clock"}, {"port", "value"}};
    h.document["layout"] = {{"reroutes", Json::array({
        {{"edge", {{"node", "left"}, {"port", "gain"}}},
         {"points", Json::array({Json::array({10, 20})})}}})}};
    const auto before = h.document;
    h.open();
    REQUIRE(h.client.model.disconnectInput("left", "gain"));
    h.commit();
    REQUIRE(h.document.at("layout").at("reroutes").empty());
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.document == before);
}

TEST_CASE("Graph history never replays across another writer or a reconnect",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    h.commit();
    REQUIRE(h.client.model.canUndo());
    SECTION("external change invalidates history") {
        ++h.generation;
        h.document["nodes"][0]["label"] = "Another writer";
        h.client.model.refresh();
        h.refresh();
    }
    SECTION("a reconnect to another revision starts a new history") {
        h.client.model.setConnected(false);
        ++h.generation;
        h.open();
    }
    REQUIRE_FALSE(h.client.model.canUndo());
    REQUIRE_FALSE(h.client.model.canRedo());
    REQUIRE_FALSE(h.client.model.undo());
    REQUIRE(onlyPlanRead(h.client.model));
}

// OT-1, decision 3: an engine restart (RS-1 asks for one after a graph edit
// the live materials cannot take) reconnects Studio. The history is about the
// document, not the connection: the same digest read back keeps Undo, another
// one clears it, and another project clears it whatever the digest.
TEST_CASE("OT-1 an engine restart keeps the graph history for the same document",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    const auto before = h.document;
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    h.commit();
    REQUIRE(h.client.model.canUndo());
    h.client.model.setConnected(false);
    REQUIRE_FALSE(h.client.model.canUndo());
    SECTION("the same document read back offers Undo, and it applies") {
        h.open();
        REQUIRE(h.client.model.canUndo());
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
        REQUIRE(h.client.model.canRedo());
    }
    SECTION("another digest read back clears it") {
        ++h.generation;
        h.open();
        REQUIRE_FALSE(h.client.model.canUndo());
        REQUIRE_FALSE(h.client.model.canRedo());
    }
    SECTION("another project clears it, even with the same digest") {
        h.client.model.forgetHistory();
        h.open();
        REQUIRE_FALSE(h.client.model.canUndo());
        REQUIRE_FALSE(h.client.model.canRedo());
    }
}

TEST_CASE("Q-10 a sound refused while a graph edit is prepared reads as one sentence",
          "[studio][graph-canvas][q10]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    const auto submitted = h.client.take("edit_graph_document");
    h.client.reply(submitted, {{"status", "accepted"}, {"ticket", "edit"}});
    const auto poll = h.client.take("get_edit_result");
    // What the render pipeline answers when Audio::prepareSound refuses the
    // asset: its own code, and AU-2's code at the head of the message.
    h.client.reply(poll, {{"status", "rejected"}, {"committed", false},
                          {"error", {{"code", "render_pipeline_preflight_failed"},
                                     {"message", R"(audio.format_unsupported@1 {"asset":"project://a.ogg",)"
                                                 R"("extension":".ogg","signature":"wav"})"},
                                     {"ticket", "edit"}}}});
    const auto notice = h.client.model.notice();
    CHECK(notice.kind == PelicanStudio::InspectorNoticeKind::Error);
    CHECK(notice.message ==
          "Sound extension and contents disagree (WAV or Ogg Vorbis) (audio.format_unsupported@1)");

    // The control: a refusal Studio has no sentence for keeps its own words.
    h.refresh();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock again"));
    h.client.reply(h.client.take("edit_graph_document"),
                   {{"error", {{"code", "invalid_graph_document"}, {"message", "Rejected edit"}}}});
    CHECK(h.client.model.notice().message == "Rejected edit (invalid_graph_document)");
}

TEST_CASE("Rejected graph edits retain history but successful branches discard Redo",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    h.commit();
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.setNodeLabel("clock", "Rejected"));
    const auto submitted = h.client.take("edit_graph_document");
    h.client.reply(submitted, {{"error", {{"message", "Rejected edit"}}}});
    h.refresh();
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.setNodeLabel("clock", "New branch"));
    h.commit();
    REQUIRE_FALSE(h.client.model.canRedo());
    REQUIRE(h.client.model.canUndo());
}

// --- LP-4c: graph.loop on the Graph tab.

namespace {

Json lp4cColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// A loop body: a boundary port `target`, and a passthrough drawing into an
// image of its own, which is the body's one output.
Json lp4cBody(const std::string &pass) {
    const auto type = lp4cColorType();
    return Json{{"nodes", Json::array({
        {{"id", "target"}, {"op", "graph.input"}},
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

// Element 0 runs `first`, every other element `paint`; `spare` is a body
// nothing runs, with another boundary.
Json lp4cDocument() {
    return Json{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"first", lp4cBody("first_fill")}, {"paint", lp4cBody("fill")},
                         {"spare", {{"nodes", Json::array({{{"id", "gain"}, {"op", "graph.input"},
                                                            {"default", 1.0}}})},
                                    {"outputs", Json::object()}}}}},
        {"nodes", Json::array({
            {{"id", "lit"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "lit_color"}, {"type", lp4cColorType()}}}, {"outputs", {"resource"}}},
            {{"id", "steps"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"range", {{"count", 3}}}}},
                             {"branches", Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                                       {{"body", "paint"}}})}}},
             {"inputs", {{"target", {{"node", "lit"}, {"port", "resource"}}}}},
             {"outputs", {"image", "image[]"}}}})},
        {"outputs", Json::object()}};
}

// The catalog with the render operations, which declare no ports of their
// own (parameter_dependent), as the engine publishes them.
Json lp4cCatalog() {
    auto reply = catalog();
    auto &render = reply["modules"][1]["operations"];
    for (const char *op : {"render.image", "render.target_ref", "render.fullscreen"})
        render.push_back({{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"},
                          {"sink", false}, {"ports", "parameter_dependent"}});
    return reply;
}

// get_frame_plan's answer for the loop: element 0 on branch 0 and the others
// on branch 1.
Json lp4cFramePlan(std::int64_t count = 3) {
    auto elements = Json::array();
    for (std::int64_t element = 0; element < count; ++element)
        elements.push_back({{"element", element}, {"index", element}, {"branch", element == 0 ? 0 : 1}});
    return {{"schema", "pelican.frame_plan"}, {"version", 1},
            {"loops", Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", count},
                                    {"elements", elements},
                                    {"unmatched_branches", Json::array()}}})}};
}

struct LoopHarness : GraphHistoryHarness {
    LoopHarness() {
        document = lp4cDocument();
        client.catalog_reply = lp4cCatalog();
    }
    // The loop report the model asks for once the document holds a loop.
    void report(const Json &plan = lp4cFramePlan()) {
        client.reply(client.take("get_frame_plan"), plan);
    }
    // One gesture's op list, refused, so the next gesture starts clean.
    Json refuse(const Json &error = Json{{"code", "invalid_graph_document"},
                                         {"message", "refused in the test"}}) {
        const auto submit = client.take("edit_graph_document");
        const auto ops = submit.params.at("ops");
        client.reply(submit, {{"status", "rejected"}, {"committed", false}, {"error", error}});
        refresh();
        return ops;
    }
    Json parameters(const Json &ops) {
        REQUIRE(ops.size() == 1);
        REQUIRE(ops[0].at("op") == "set_node_parameters");
        CHECK(ops[0].at("scope") == "root");
        CHECK(ops[0].at("node") == "steps");
        return ops[0].at("parameters");
    }
};

} // namespace

TEST_CASE("LP-4c a loop's ports are its body's boundary and its parameters are not ports",
          "[studio][graph-canvas][lp4c]") {
    REQUIRE_NOTHROW(Pelican::editGraphDocument(lp4cDocument(), Json::array()));
    LoopHarness h;
    h.open();
    const auto loop = h.client.node("steps");
    CHECK(loop.loop);
    CHECK_FALSE(loop.instance);
    // The input ports are the first body's graph.input nodes, and nothing else:
    // over and branches are the Loop fields.
    REQUIRE(loop.inputs.size() == 1);
    CHECK(loop.inputs[0].port == "target");
    CHECK(loop.inputs[0].connected);
    CHECK(loop.inputs[0].source_node == "lit");
    CHECK_FALSE(loop.inputs[0].has_parameter);
    CHECK(std::ranges::none_of(loop.inputs, [](const auto &port) {
        return port.port == "over" || port.port == "branches";
    }));
    // The outputs are the p and p[] it declares; p[] is a bundle.
    REQUIRE(loop.outputs.size() == 2);
    CHECK_FALSE(output(loop, "image").bundle);
    CHECK(output(loop, "image[]").bundle);
    // A descent opens the first body.
    CHECK(loop.definition == "first");
    CHECK(h.client.model.canEnterInstance("steps"));
    // Before the report the heading says loop; after it, the tier and count.
    CHECK(loop.badge == "↻ loop");
    h.report();
    CHECK(h.client.node("steps").badge == "↻ expand ×3");

    const auto read = h.client.model.loop("steps");
    REQUIRE(read);
    CHECK(read->count == 3);
    CHECK_FALSE(read->count_expression);
    CHECK(read->body == "first");
    CHECK(read->body_inputs == std::vector<std::string>{"target"});
    CHECK(read->body_outputs == std::vector<std::string>{"image"});
    REQUIRE(read->branches.size() == 2);
    CHECK(read->branches[0].when == "element");
    CHECK(read->branches[0].values == std::vector<std::int64_t>{0});
    CHECK(read->branches[0].matched == std::vector<std::int64_t>{0});
    CHECK(read->branches[1].when == "else");
    CHECK(read->branches[1].matched == std::vector<std::int64_t>{1, 2});
    CHECK(read->reported_count == 3);
    // Only the boundary-sharing definitions are a branch's choices.
    CHECK(h.client.model.loopBodyChoices("steps") == std::vector<std::string>{"first", "paint"});
}

TEST_CASE("LP-4c Add loop sends the new body and the loop in one list",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    CHECK(h.client.model.suggestDefinitionName() == "loop_body");
    SECTION("over a new, empty definition") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        CHECK(h.last_ops == Json::array({
            {{"op", "add_definition"}, {"name", "loop_body"}},
            {{"op", "add_node"}, {"scope", "root"},
             {"node", {{"id", "loop"}, {"op", "graph.loop"},
                       {"parameters", {{"over", {{"range", {{"count", 1}}}}},
                                       {"branches", Json::array({{{"body", "loop_body"}}})}}},
                       {"outputs", Json::array()}}}}}));
        CHECK(h.document.at("definitions").at("loop_body") ==
              Json{{"nodes", Json::array()}, {"outputs", Json::object()}});
        CHECK(h.client.model.suggestDefinitionName() == "loop_body_2");
    }
    SECTION("over an existing body, declaring its outputs as p") {
        REQUIRE(h.client.model.addLoop("paint"));
        // Its boundary port has no loop input yet, which the engine refuses:
        // the list is what is looked at here.
        const auto ops = h.refuse();
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("op") == "add_node");
        CHECK(ops[0].at("node").at("parameters").at("branches") ==
              Json::array({{{"body", "paint"}}}));
        CHECK(ops[0].at("node").at("outputs") == Json::array({"image"}));
    }
    SECTION("RU-1: the first pass put in the new body leaves the loop unplaced (LP-1e)") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        REQUIRE(h.client.model.enterInstance("loop"));
        REQUIRE(h.client.model.currentScope() == "loop_body");
        // A fullscreen pass arrives whole: an image of its own, the pass
        // drawing the engine's passthrough into it.
        REQUIRE(h.client.model.addNode("render.fullscreen"));
        h.commit();
        // The image and the pass, and nothing about the loop: the engine
        // expands an unplaced loop after the scene passes (LP-1e).
        REQUIRE(h.last_ops.size() == 2);
        CHECK(h.last_ops[0].at("node").at("op") == "render.image");
        CHECK(h.last_ops[1].at("node").at("op") == "render.fullscreen");
        CHECK(h.last_ops[1].at("node").at("inputs").at("color").at("node") ==
              h.last_ops[0].at("node").at("id"));
        // Nor a family: a pass that names none joins the first one.
        CHECK_FALSE(h.last_ops[1].at("node").at("parameters").contains("pass_set"));
        CHECK(h.document.at("definitions").at("loop_body").at("nodes").size() == 2);
        // A second pass is the same two nodes.
        REQUIRE(h.client.model.addNode("render.fullscreen"));
        h.commit();
        CHECK(h.last_ops.size() == 2);
        // The Place field is still there: it places the loop, and an empty
        // choice takes the place away.
        REQUIRE(h.client.model.selectScope(""));
        const auto read = h.client.model.loop("loop");
        REQUIRE(read);
        CHECK(read->body_draws);
        CHECK(read->anchor.empty());
        CHECK_FALSE(h.document.at("nodes").back().at("parameters").contains("canonical_anchor"));
        REQUIRE(h.client.model.setLoopAnchor("loop", "post_ldr"));
        h.commit();
        CHECK(h.document.at("nodes").back().at("parameters").at("canonical_anchor") == "post_ldr");
        CHECK_FALSE(h.client.model.setLoopAnchor("loop", "nowhere"));
    }
    // A loop stands in the root only; the palette says so.
    const auto root_palette = h.client.model.paletteOperations("graph.loop");
    CHECK(std::ranges::any_of(root_palette, [](const auto &entry) { return entry.op == "graph.loop"; }));
    REQUIRE(h.client.model.selectScope("paint"));
    CHECK(h.client.model.addLoopOps("paint").empty());
    CHECK(h.client.model.paletteOperations("graph.loop").empty());
}

TEST_CASE("LP-4c the Loop fields each send the loop's parameters with one member changed",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    const auto base = h.document.at("nodes").at(1).at("parameters");
    const auto with = [&](const char *key, const Json &value) {
        auto expected = base;
        expected[key] = value;
        return expected;
    };
    REQUIRE(h.client.model.setLoopCount("steps", 5));
    CHECK(h.parameters(h.refuse()) == with("over", {{"range", {{"count", 5}}}}));
    REQUIRE(h.client.model.setLoopCountExpression("steps", "2 + 1"));
    CHECK(h.parameters(h.refuse()) == with("over", {{"range", {{"count", {{"$expr", "2 + 1"}}}}}}));
    REQUIRE(h.client.model.setLoopDescending("steps", true));
    CHECK(h.parameters(h.refuse()) == with("order", {{"by", "element"}, {"direction", "descending"}}));
    REQUIRE(h.client.model.setLoopLimit("steps", 2));
    CHECK(h.parameters(h.refuse()) == with("limit", 2));
    REQUIRE(h.client.model.setLoopTier("steps", "draw_loop"));
    CHECK(h.parameters(h.refuse()) == with("tier", "draw_loop"));
    REQUIRE(h.client.model.setLoopWhere("steps", "element_not_in", {1, 4}));
    CHECK(h.parameters(h.refuse()) == with("where", {{"element_not_in", {1, 4}}}));
    REQUIRE(h.client.model.setLoopWhere("steps", "element_in", {2}));
    CHECK(h.parameters(h.refuse()) == with("where", {{"element_in", {2}}}));
    REQUIRE(h.client.model.setLoopCarry("steps", {{"image", "target"}}));
    CHECK(h.parameters(h.refuse()) ==
          with("carry", Json::array({{{"from", "image"}, {"to", "target"}}})));
    // Setting what is already there sends nothing.
    CHECK_FALSE(h.client.model.setLoopDescending("steps", false));
    CHECK_FALSE(h.client.model.setLoopLimit("steps", std::nullopt));
    CHECK_FALSE(h.client.model.setLoopTier("steps", ""));
    // The outputs are the node's own member: one replace_node.
    REQUIRE(h.client.model.setLoopOutputs("steps", {"image[]"}));
    const auto replaced = h.refuse();
    REQUIRE(replaced.size() == 1);
    CHECK(replaced[0].at("op") == "replace_node");
    auto expected = h.document.at("nodes").at(1);
    expected["outputs"] = Json::array({"image[]"});
    CHECK(replaced[0].at("node") == expected);
    // And the ones the engine takes commit.
    REQUIRE(h.client.model.setLoopCount("steps", 2));
    h.commit();
    REQUIRE(h.client.model.setLoopDescending("steps", true));
    h.commit();
    REQUIRE(h.client.model.setLoopLimit("steps", 1));
    h.commit();
    REQUIRE(h.client.model.setLoopWhere("steps", "element_not_in", {1}));
    h.commit();
    const auto read = h.client.model.loop("steps");
    REQUIRE(read);
    CHECK(read->count == 2);
    CHECK(read->descending);
    CHECK(read->limit == std::optional<std::int64_t>{1});
    CHECK(read->where == "element_not_in");
    CHECK(read->where_values == std::vector<std::int64_t>{1});
}

TEST_CASE("LP-4c the branch table adds, removes, orders and conditions branches",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    const auto branches = [&] {
        return h.document.at("nodes").at(1).at("parameters").at("branches");
    };
    // Added in front of the else, running a copy of the else's body (UX-1,
    // decision 4): a definition of its own, in the same list.
    REQUIRE(h.client.model.addLoopBranch("steps"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0] == Json{{"op", "add_definition"}, {"name", "steps_body"},
                                {"definition", lp4cBody("fill")}});
    CHECK(branches() == Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                     {{"when", {{"element", 0}}}, {"body", "steps_body"}},
                                     {{"body", "paint"}}}));
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element_in", {1, 2}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element_in", {1, 2}}});
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element_not_in", {0}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element_not_in", {0}}});
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element", {2}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element", 2}});
    REQUIRE(h.client.model.moveLoopBranch("steps", 1, -1));
    h.commit();
    CHECK(branches()[0].at("when") == Json{{"element", 2}});
    CHECK(branches()[1].at("body") == "first");
    // (nothing) is the null body.
    REQUIRE(h.client.model.setLoopBranchBody("steps", 0, std::nullopt));
    h.commit();
    CHECK(branches()[0].at("body").is_null());
    REQUIRE(h.client.model.setLoopBranchBody("steps", 0, std::string{"paint"}));
    h.commit();
    CHECK(branches()[0].at("body") == "paint");
    REQUIRE(h.client.model.removeLoopBranch("steps", 0));
    h.commit();
    CHECK(branches().size() == 2);
    // The else is the last row: never removed, moved or conditioned.
    CHECK_FALSE(h.client.model.removeLoopBranch("steps", 1));
    CHECK_FALSE(h.client.model.moveLoopBranch("steps", 0, 1));
    CHECK_FALSE(h.client.model.setLoopBranchWhen("steps", 1, "element", {3}));
    CHECK(std::ranges::none_of(h.client.model.takeRpcRequests(), [](const auto &request) {
        return request.method == "edit_graph_document";
    }));

    // New definition...: the loop's boundary in a definition of its own, and
    // the branch running it, in one list. The body's output is made by a
    // pass, so the body is copied whole.
    REQUIRE(h.client.model.newLoopBranchDefinition("steps", 0, "first_copy"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0] == Json{{"op", "add_definition"}, {"name", "first_copy"},
                                {"definition", lp4cBody("first_fill")}});
    CHECK(h.last_ops[1].at("op") == "set_node_parameters");
    CHECK(branches()[0].at("body") == "first_copy");
    // A body whose outputs come off its boundary is copied empty: only its
    // graph.input nodes.
    auto passthrough = lp4cDocument();
    passthrough["definitions"]["first"] = Json{
        {"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}}})},
        {"outputs", {{"image", {{"node", "target"}, {"port", "value"}}}}}};
    passthrough["definitions"]["paint"] = passthrough["definitions"]["first"];
    LoopHarness empty;
    empty.document = passthrough;
    empty.open();
    REQUIRE(empty.client.model.newLoopBranchDefinition("steps", 1, "blank"));
    const auto ops = empty.refuse();
    CHECK(ops[0].at("definition") == passthrough["definitions"]["first"]);
}

TEST_CASE("LP-4c Per iteration reads one element of another loop's p[]",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    auto reader = h.document.at("nodes").at(1);
    reader["id"] = "reader";
    reader["outputs"] = Json::array();
    h.document["nodes"].push_back(reader);
    h.open();
    const auto sources = h.client.model.perIterationSources("reader");
    CHECK(sources == std::vector<std::pair<std::string, std::string>>{{"steps", "image[]"}});
    // A loop does not read its own p[].
    CHECK(h.client.model.perIterationSources("steps").empty());
    REQUIRE(h.client.model.connectPerIteration("reader", "target", "steps", "image[]"));
    CHECK(h.refuse() == Json::array({{{"op", "set_input"}, {"scope", "root"}, {"node", "reader"},
                                      {"port", "target"},
                                      {"source", {{"node", "steps"}, {"port", "image[]"},
                                                  {"element", {{"$expr", "element"}}}}}}}));
    REQUIRE(h.client.model.connectPerIteration("reader", "target", "steps", "image[]",
                                               "element - 1"));
    CHECK(h.refuse()[0].at("source").at("element") == Json{{"$expr", "element - 1"}});
    CHECK_FALSE(h.client.model.connectPerIteration("reader", "target", "lit", "resource"));
}

TEST_CASE("LP-4c Undo inverts each op against what the ops before it left",
          "[studio][graph-canvas][lp4c][graph-history]") {
    LoopHarness h;
    h.document["nodes"].push_back(
        {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
         {"outputs", {"value"}}});
    h.open();
    const auto before = h.document;
    const auto roundTrip = [&](const Json &after) {
        REQUIRE(h.client.model.canUndo());
        REQUIRE(h.client.model.undo());
        h.commit();
        CHECK(h.document == before);
        CHECK(h.document.dump(2) == before.dump(2));
        REQUIRE(h.client.model.redo());
        h.commit();
        CHECK(h.document == after);
        CHECK(h.document.dump(2) == after.dump(2));
    };
    SECTION("add_definition, then a loop over it") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        roundTrip(h.document);
    }
    SECTION("add_node, then set_input naming the node just added") {
        REQUIRE(h.client.model.submitEdit(Json::array({
            {{"op", "add_node"}, {"scope", "root"},
             {"node", {{"id", "sum"}, {"op", "value.add"},
                       {"inputs", {{"b", {{"node", "clock"}, {"port", "value"}}}}},
                       {"outputs", {"value"}}}}},
            {{"op", "set_input"}, {"scope", "root"}, {"node", "sum"}, {"port", "a"},
             {"source", {{"node", "clock"}, {"port", "value"}}}}})));
        h.commit();
        CHECK(h.document.at("nodes").back().at("inputs").at("a").at("node") == "clock");
        roundTrip(h.document);
    }
    SECTION("rename_definition, with the loop's branch following it") {
        REQUIRE(h.client.model.renameDefinition("paint", "brush"));
        h.commit();
        CHECK(h.document.at("nodes").at(1).at("parameters").at("branches").at(1).at("body") == "brush");
        roundTrip(h.document);
    }
    SECTION("remove_definition comes back with its body") {
        REQUIRE(h.client.model.removeDefinition("spare"));
        h.commit();
        CHECK_FALSE(h.document.at("definitions").contains("spare"));
        CHECK(h.client.model.undo());
        h.commit();
        CHECK(h.document == before);
        CHECK(h.document.dump(2) == before.dump(2));
    }
}

TEST_CASE("LP-4c loop and definition refusals read as sentences and mark where they point",
          "[studio][graph-canvas][lp4c]") {
    using PelicanStudio::graphLoopRefusalDescription;
    // The engine lays the fields beside the code.
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_branch_else_missing@1"}, {"node", "steps"}}) ==
          "Loop steps has no else: its last branch must have no condition "
          "(graph.loop_branch_else_missing@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_branch_interface@1"}, {"node", "steps"},
                                       {"branch", 1}, {"definition", "spare"}, {"port", "gain"}}) ==
          "Branch 1 of loop steps runs spare, whose inputs and outputs differ from the loop's at gain "
          "(graph.loop_branch_interface@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_expression_invalid@1"}, {"node", "steps"},
                                       {"pointer", "/nodes/1/parameters/over/range/count"},
                                       {"reason", "syntax"}}) ==
          "An expression of loop steps is not valid: syntax at /nodes/1/parameters/over/range/count "
          "(graph.loop_expression_invalid@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_element_unknown@1"}, {"node", "reader"},
                                       {"port", "target"}, {"element", 7}}) ==
          "Input target of reader reads element 7, which the loop it reads does not have "
          "(graph.loop_element_unknown@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_expansion_limit@1"}, {"node", "steps"},
                                       {"count", 100}, {"limit", 64}}) ==
          "Loop steps would make 100 copies; the limit is 64 (graph.loop_expansion_limit@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.definition_referenced@1"}, {"definition", "paint"},
                                       {"referenced_by", {"steps", "other"}}}) ==
          "Definition paint is still used by steps, other (graph.definition_referenced@1).");
    // Quoted in a message, as a GraphRegistryError spells itself.
    CHECK(graphLoopRefusalDescription(
              {{"code", "render_pipeline_preflight_failed"},
               {"message", "graph.loop_expansion_limit@1 {\"count\":70,\"limit\":64,\"node\":\"steps\"}"}}) ==
          "Loop steps would make 70 copies; the limit is 64 (graph.loop_expansion_limit@1).");
    CHECK(graphLoopRefusalDescription({{"code", "invalid_graph_document"}, {"message", "x"}}).empty());

    LoopHarness h;
    h.open();
    SECTION("a loop refused by name is marked") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse({{"code", "graph.loop_expansion_limit@1"}, {"message", "limit"},
                  {"node", "steps"}, {"count", 100}, {"limit", 64}});
        CHECK(h.client.model.notice().message ==
              "Loop steps would make 100 copies; the limit is 64 (graph.loop_expansion_limit@1).");
        CHECK(h.client.node("steps").refused);
        CHECK_FALSE(h.client.node("lit").refused);
        // An edit that commits takes the mark away.
        REQUIRE(h.client.model.setLoopCount("steps", 2));
        h.commit();
        CHECK_FALSE(h.client.node("steps").refused);
    }
    SECTION("an element's copy marks the loop and the body node of that element's branch") {
        h.report();
        REQUIRE(h.client.model.setLoopCount("steps", 4));
        h.refuse({{"code", "graph.loop_expression_invalid@1"}, {"message", "refused"},
                  {"node", "steps/2/fill"}, {"pointer", "/definitions/paint/nodes/2"},
                  {"reason", "not_static"},
                  {"loop_origin", {{"loop", "steps"}, {"element", 2}, {"node", "fill"}}}});
        CHECK(h.client.model.refusalMarks().size() == 2);
        CHECK(h.client.node("steps").refused);
        REQUIRE(h.client.model.selectScope("paint"));
        CHECK(h.client.node("fill").refused);
        REQUIRE(h.client.model.selectScope("first"));
        CHECK_FALSE(h.client.node("first_fill").refused);
    }
    SECTION("a graph variant that refused it is named first") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse({{"code", "graph.loop_expansion_limit@1"}, {"variant", "preview"},
                  {"node", "steps"}, {"count", 100}, {"limit", 64}});
        CHECK(h.client.model.notice().message ==
              "The preview graph refused it: Loop steps would make 100 copies; the limit is 64 "
              "(graph.loop_expansion_limit@1).");
    }
}

TEST_CASE("LP-4c a descent into a loop opens its chosen branch, and a copy's origin opens its body",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    REQUIRE(h.client.model.enterInstance("steps"));
    CHECK(h.client.model.currentScope() == "first");
    CHECK(h.client.model.enteredLoop() == "steps");
    CHECK(h.client.model.definitionUseCount("first") == 1);
    // The branch combo swaps the body in place.
    REQUIRE(h.client.model.selectLoopBranch(1));
    CHECK(h.client.model.currentScope() == "paint");
    CHECK(h.client.model.breadcrumb() == std::vector<std::string>{"", "paint"});
    REQUIRE(h.client.model.leaveScope());
    CHECK(h.client.model.enteredLoop().empty());
    // The branch chosen is the one opened next time.
    REQUIRE(h.client.model.enterInstance("steps"));
    CHECK(h.client.model.currentScope() == "paint");
    REQUIRE(h.client.model.leaveScope());
    // Decision 10: element 0 ran `first`, element 2 `paint` (the report).
    h.report();
    REQUIRE(h.client.model.enterLoopOrigin("steps", 2, "fill"));
    CHECK(h.client.model.currentScope() == "paint");
    REQUIRE(h.client.model.enterLoopOrigin("steps", 0, "first_fill"));
    CHECK(h.client.model.currentScope() == "first");
    CHECK_FALSE(h.client.model.enterLoopOrigin("steps", 0, "fill"));
    // A definition two loops run is shared.
    auto shared = lp4cDocument();
    auto second = shared.at("nodes").at(1);
    second["id"] = "again";
    shared["nodes"].push_back(second);
    LoopHarness twice;
    twice.document = shared;
    twice.open();
    CHECK(twice.client.model.definitionUseCount("paint") == 2);
}

// --- LP-4e: the draw_loop stage, render.material_pass and draw.*.

namespace {

using PelicanStudio::GraphCanvasDrawWhen;
using PelicanStudio::GraphCanvasTagFilter;

Json lp4eDepthType() {
    return {{"semantic", "pelican.render.depth@1"},
            {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
}

// What decompose_material_pass (LP-3b) leaves of a forward pass: its
// attachments through target_refs, a draw_loop over its materials whose one
// (else) branch draws each as it was, and the pass itself, reading the
// loop's draws.
Json lp4eDocument() {
    return Json{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"forward_opaque_draw",
            {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"},
                                     {"parameters", Json::object()}, {"outputs", {"draws"}}}})},
             {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}}},
        {"nodes", Json::array({
            {{"id", "scene_color_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_color"}, {"type", lp4cColorType()}}},
             {"outputs", {"resource"}}},
            {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_depth"}, {"type", lp4eDepthType()}}},
             {"outputs", {"resource"}}},
            {{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                             {"order", {{"by", "phase_default"}}},
                             {"branches", Json::array({{{"body", "forward_opaque_draw"}}})}}},
             {"outputs", {"draws"}}},
            {{"id", "forward_opaque"}, {"op", "render.material_pass"},
             {"parameters", {{"material_contract", "forward_opaque_v1"}, {"pass_set", "main_render"},
                             {"insert", {{"before", "present"}, {"declared", true}}}}},
             {"inputs", {{"color", {{"node", "scene_color_ref"}, {"port", "resource"}}},
                         {"depth", {{"node", "scene_depth_ref"}, {"port", "resource"}}},
                         {"draws", {{"node", "forward_opaque_draws"}, {"port", "draws"}}}}},
             {"outputs", Json::array()}}})},
        {"outputs", Json::object()}};
}

// The catalog with the material pass and the draw steps, parameter_dependent
// as the engine publishes every render operation.
Json lp4eCatalog() {
    auto reply = lp4cCatalog();
    auto &render = reply["modules"][1]["operations"];
    for (const char *op : {"render.material_pass", "draw.material", "draw.variant", "draw.override"})
        render.push_back({{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"},
                          {"sink", false}, {"ports", "parameter_dependent"}});
    return reply;
}

std::string lp4eMaterial(std::size_t index) {
    return "project://materials/m" + std::to_string(index) + ".material.json#m" +
           std::to_string(index);
}

// get_frame_plan's draw_loop entry: `total` materials, of which the page
// holds [offset, offset + held), all on the else.
Json lp4eFramePlan(std::size_t total, std::size_t offset, std::size_t held) {
    auto elements = Json::array();
    for (std::size_t index = offset; index < offset + held && index < total; ++index)
        elements.push_back({{"element", lp4eMaterial(index)}, {"index", index}, {"branch", 0}});
    return {{"schema", "pelican.frame_plan"}, {"version", 1},
            {"loops", Json::array({{{"node", "forward_opaque_draws"}, {"tier", "draw_loop"},
                                    {"pass", "forward_opaque"}, {"count", total},
                                    {"elements", elements}, {"unmatched_branches", Json::array()},
                                    {"total", total}, {"offset", offset}, {"limit", 256}}})}};
}

struct DrawLoopHarness : LoopHarness {
    DrawLoopHarness() {
        document = lp4eDocument();
        client.catalog_reply = lp4eCatalog();
    }
    Json loopParameters(const Json &ops) {
        REQUIRE(ops.size() == 1);
        REQUIRE(ops[0].at("op") == "set_node_parameters");
        CHECK(ops[0].at("scope") == "root");
        CHECK(ops[0].at("node") == "forward_opaque_draws");
        return ops[0].at("parameters");
    }
};

std::vector<std::string> portNames(const GraphCanvasNode &node) {
    std::vector<std::string> names;
    for (const auto &port : node.inputs) names.push_back(port.port);
    return names;
}

bool offered(const GraphCanvasModel &model, const std::string &op) {
    return std::ranges::any_of(model.paletteOperations(), [&](const auto &entry) {
        return entry.op == op;
    });
}

} // namespace

TEST_CASE("LP-4e a material pass's sockets come from its contract, and the palette places one",
          "[studio][graph-canvas][lp4e]") {
    REQUIRE_NOTHROW(Pelican::editGraphDocument(lp4eDocument(), Json::array()));
    DrawLoopHarness h;
    // A G-buffer pass beside the forward one, with nothing connected.
    auto deferred = lp4eDocument();
    deferred["nodes"].push_back({{"id", "gbuffer"}, {"op", "render.material_pass"},
                                 {"parameters", {{"material_contract", "deferred_geometry_v1"}}},
                                 {"outputs", Json::array()}});
    h.document = deferred;
    h.open();

    const auto forward = h.client.node("forward_opaque");
    // The contract's sockets, then depth, then the draws a draw_loop hands
    // it; the parameters are not ports.
    CHECK(portNames(forward) == std::vector<std::string>{"color", "depth", "draws"});
    CHECK(port(forward, "color").required);
    CHECK(port(forward, "color").connected);
    CHECK(port(forward, "color").source_node == "scene_color_ref");
    CHECK(port(forward, "depth").required);
    CHECK_FALSE(port(forward, "draws").required);
    CHECK(port(forward, "draws").type == "pelican.render.draw_stream@1");
    CHECK(port(forward, "draws").source_node == "forward_opaque_draws");
    // GL-1f (RS-4): what it wrote is handed on, a port per attachment.
    REQUIRE(forward.outputs.size() == 2);
    CHECK(forward.outputs[0].port == "color");
    CHECK(forward.outputs[1].port == "depth");
    CHECK(portNames(h.client.node("gbuffer")) ==
          std::vector<std::string>{"albedo", "normal", "material", "worldpos", "emissive", "depth",
                                   "draws"});
    CHECK(GraphCanvasModel::materialPassColorSockets(
              {{"material_outputs", {{"schema", "pelican.material_outputs"},
                                     {"outputs", Json::array({{{"name", "tint"}}, {{"name", "mask"}}})}}}}) ==
          std::vector<std::string>{"tint", "mask"});
    // A draw step follows the one on `after` and hands the list on.
    REQUIRE(h.client.model.selectScope("forward_opaque_draw"));
    const auto step = h.client.node("step");
    CHECK(portNames(step) == std::vector<std::string>{"after"});
    REQUIRE(step.outputs.size() == 1);
    CHECK(step.outputs[0].port == "draws");
    CHECK(step.outputs[0].type == "pelican.render.draw_stream@1");

    // The palette offers the material pass in the root, and the draw steps
    // in a body a draw_loop runs -- each on its own side only.
    CHECK(offered(h.client.model, "draw.material"));
    CHECK(offered(h.client.model, "draw.variant"));
    CHECK(offered(h.client.model, "draw.override"));
    CHECK_FALSE(offered(h.client.model, "render.material_pass"));
    REQUIRE(h.client.model.selectScope({}));
    CHECK(offered(h.client.model, "render.material_pass"));
    CHECK_FALSE(offered(h.client.model, "draw.material"));

    SECTION("a G-buffer pass arrives writing an image per socket and a depth") {
        REQUIRE(h.client.model.addNode("render.material_pass", "deferred_geometry_v1"));
        h.commit();
        // GL-1d, decision 1: its body and its draw_loop first, then the
        // images, then the pass.
        REQUIRE(h.last_ops.size() == 9);
        const auto &pass = h.last_ops.back().at("node");
        CHECK(pass.at("op") == "render.material_pass");
        CHECK(pass.at("parameters") ==
              Json{{"material_contract", "deferred_geometry_v1"}, {"pass_set", "main_render"}});
        CHECK(pass.at("inputs").size() == 7);
        CHECK(pass.at("inputs").at("worldpos") ==
              Json{{"node", pass.at("id").get<std::string>() + "__worldpos"}, {"port", "resource"}});
        const auto &depth = h.last_ops[7].at("node");
        CHECK(depth.at("parameters").at("format") == "D32_SFLOAT");
        CHECK(depth.at("parameters").at("type") == lp4eDepthType());
        CHECK(portNames(h.client.node(pass.at("id").get<std::string>())).front() == "albedo");
        // A deferred pass needs no route default.
        CHECK_FALSE(h.last_params.contains("material_route_default"));
    }
    SECTION("the palette's contract decides the sockets, and changing it is one field") {
        REQUIRE(h.client.model.setMaterialPassContract("gbuffer", "forward_transparent_v1"));
        const auto ops = h.refuse();
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("parameters").at("material_contract") == "forward_transparent_v1");
        CHECK_FALSE(h.client.model.setMaterialPassContract("gbuffer", "legacy_gbuffer_v1"));
    }
}

// GL-1d, decision 1: the palette's material pass arrives drawing: the draw_loop
// decompose_material_pass would write, its body's draw.material and the edge
// into the pass's draws, all in the one list one Undo takes back.
TEST_CASE("GL-1d the palette's material pass arrives with its draw loop, body and draws edge",
          "[studio][graph-canvas][gl1d]") {
    DrawLoopHarness h;
    h.open();
    REQUIRE(h.client.model.addNode("render.material_pass", "deferred_geometry_v1"));
    h.commit();
    const auto &pass = h.last_ops.back().at("node");
    const auto id = pass.at("id").get<std::string>();
    CHECK(id == "render_material_pass");
    CHECK(h.last_ops[0] ==
          Json{{"op", "add_definition"}, {"name", id + "_draw"},
               {"definition",
                {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"},
                                         {"parameters", Json::object()},
                                         {"outputs", {"draws"}}}})},
                 {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}});
    CHECK(h.last_ops[1] ==
          Json{{"op", "add_node"}, {"scope", "root"},
               {"node", {{"id", id + "_draws"}, {"op", "graph.loop"},
                         {"parameters",
                          {{"over", {{"draw_items", {{"contract", "deferred_geometry_v1"}}}}},
                           {"tier", "draw_loop"},
                           {"order", {{"by", "phase_default"}}},
                           {"branches", Json::array({{{"body", id + "_draw"}}})}}},
                         {"outputs", {"draws"}}}}});
    CHECK(pass.at("inputs").at("draws") == Json{{"node", id + "_draws"}, {"port", "draws"}});
    // The canvas reads it back as a draw_loop the pass's draws come from,
    // whose body draws each material.
    const auto loop = h.client.model.loop(id + "_draws");
    REQUIRE(loop);
    CHECK(loop->draw_items);
    CHECK(loop->contract == "deferred_geometry_v1");
    CHECK(loop->pass == id);
    CHECK(loop->body == id + "_draw");
    CHECK(port(h.client.node(id), "draws").source_node == id + "_draws");
    CHECK(h.client.model.drawBodyScope(id + "_draw"));
    // One Undo takes the whole list back.
    REQUIRE(h.client.model.canUndo());
    REQUIRE(h.client.model.undo());
    const auto submit = h.client.take("edit_graph_document");
    const auto &inverse = submit.params.at("ops");
    CHECK(std::ranges::any_of(inverse, [&](const Json &op) {
        return op.value("op", std::string{}) == "remove_definition" &&
               op.value("name", std::string{}) == id + "_draw";
    }));
    const auto undone = Pelican::editGraphDocument(h.document, inverse);
    CHECK(undone.at("nodes").size() == lp4eDocument().at("nodes").size());
    CHECK_FALSE(undone.at("definitions").contains(id + "_draw"));

    SECTION("a second one takes names its loop and body are free under") {
        auto document = lp4eDocument();
        document["nodes"].push_back({{"id", "render_material_pass_draws"}, {"op", "value.constant"},
                                     {"parameters", {{"value", 0.0}}}, {"outputs", {"value"}}});
        DrawLoopHarness second;
        second.document = document;
        second.open();
        REQUIRE(second.client.model.addNode("render.material_pass", "forward_opaque_v1"));
        second.commit();
        CHECK(second.last_ops.back().at("node").at("id") == "render_material_pass_2");
        CHECK(second.last_ops[1].at("node").at("id") == "render_material_pass_2_draws");
    }
}

// MC-1s, decisions 1 and 3: the palette's material pass makes its images with
// the formats of the contract's table the Frame Plan's form makes its targets
// with, in the column the authoring context's enabled features pick. A Blank
// pipeline (no "hdr") gets B8G8R8A8_SRGB albedo and emissive -- the contract
// the live materials were registered with, so the engine does not restart --
// and one with "hdr" gets R16G16B16A16_SFLOAT. Forward colour is wide in both.
TEST_CASE("MC-1s the palette's material pass images take the form's formats for the "
          "pipeline's HDR switch",
          "[studio][graph-canvas][mc1s]") {
    // The image op a pass's socket is connected to, by the id its input names.
    const auto imageOf = [](const Json &ops, const Json &pass, const std::string &socket) {
        const auto id = pass.at("inputs").at(socket).at("node").get<std::string>();
        for (const auto &op : ops)
            if (op.at("op") == "add_node" && op.at("node").at("id") == id) return op.at("node");
        FAIL("no image op for " << socket);
        return Json{};
    };
    const auto check = [&](const Json &context, bool hdr) {
        CHECK(PelicanStudio::materialPassHdr(context) == hdr);
        DrawLoopHarness h;
        h.open();
        // The context the palette asks for when it opens.
        h.client.model.readRenderAuthoringAgain();
        h.client.reply(h.client.take("get_render_authoring_context"), context);
        for (const std::string contract :
             {"deferred_geometry_v1", "forward_opaque_v1", "forward_transparent_v1"}) {
            CAPTURE(contract, hdr);
            REQUIRE(h.client.model.addNode("render.material_pass", contract));
            h.commit();
            const auto &pass = h.last_ops.back().at("node");
            REQUIRE(pass.at("op") == "render.material_pass");
            // The form's slots: the colour attachments in location order,
            // then depth, against the pass's sockets in the same order.
            const auto slots = PelicanStudio::materialPassSlots(contract, hdr);
            auto sockets = GraphCanvasModel::materialPassColorSockets(pass.at("parameters"));
            sockets.push_back("depth");
            REQUIRE(sockets.size() == slots.size());
            for (std::size_t index = 0; index < slots.size(); ++index) {
                CAPTURE(sockets[index]);
                const auto image = imageOf(h.last_ops, pass, sockets[index]);
                CHECK(image.at("op") == "render.image");
                CHECK(image.at("parameters").at("format") == slots[index].format);
                CHECK(image.at("parameters").at("usage") == Json(slots[index].usage));
            }
            // The table itself, read as the engine's live materials have it.
            const std::string scene = hdr ? "R16G16B16A16_SFLOAT" : "B8G8R8A8_SRGB";
            if (contract == "deferred_geometry_v1") {
                CHECK(imageOf(h.last_ops, pass, "albedo").at("parameters").at("format") == scene);
                CHECK(imageOf(h.last_ops, pass, "emissive").at("parameters").at("format") == scene);
                CHECK(imageOf(h.last_ops, pass, "normal").at("parameters").at("format") ==
                      "R16G16B16A16_SFLOAT");
                CHECK(imageOf(h.last_ops, pass, "worldpos").at("parameters").at("format") ==
                      "R16G16B16A16_SFLOAT");
                CHECK(imageOf(h.last_ops, pass, "material").at("parameters").at("format") ==
                      "R8G8B8A8_UNORM");
            } else {
                CHECK(imageOf(h.last_ops, pass, "color").at("parameters").at("format") ==
                      "R16G16B16A16_SFLOAT");
            }
            CHECK(imageOf(h.last_ops, pass, "depth").at("parameters").at("format") == "D32_SFLOAT");
        }
    };
    SECTION("a Blank pipeline: no hdr feature") {
        check(Json{{"enabled_feature_names", Json::array()}}, false);
    }
    SECTION("a pipeline with the hdr feature") {
        check(Json{{"enabled_feature_names", Json::array({"bloom", "hdr"})}}, true);
    }
    SECTION("a context that does not say is read as HDR, as the form reads it") {
        CHECK(PelicanStudio::materialPassHdr(Json::object()));
        CHECK(PelicanStudio::materialPassHdr(false, {}));
        CHECK_FALSE(PelicanStudio::materialPassHdr(true, {"bloom"}));
    }
}

// GL-1d, decisions 3 and 4: a Forward opaque pass where no pass draws deferred
// materials carries material_route_default "forward" in the same ticket (the
// Frame Plan's judgment); an engine that does not take the field is sent the
// list again without it, and its material_route_unserved@1 reads as the Frame
// Plan's sentence.
TEST_CASE("GL-1d a forward material pass sets the route default, and falls back without it",
          "[studio][graph-canvas][gl1d]") {
    DrawLoopHarness h;
    // forward_opaque is the only material pass: nothing draws deferred.
    h.open();
    CHECK_FALSE(h.client.model.pipelineHasDeferredMaterialPass());
    CHECK(h.client.model.addNodeParams({}, "render.material_pass", "forward_opaque_v1") ==
          Json{{"material_route_default", "forward"}});
    CHECK(h.client.model.addNodeParams({}, "render.material_pass", "deferred_geometry_v1").empty());
    CHECK(h.client.model.addNodeParams({}, "render.material_pass", "forward_transparent_v1").empty());
    CHECK(h.client.model.addNodeParams({}, "value.constant").empty());

    REQUIRE(h.client.model.addNode("render.material_pass", "forward_opaque_v1"));
    const auto first = h.client.take("edit_graph_document");
    CHECK(first.params.at("material_route_default") == "forward");
    const auto ops = first.params.at("ops");

    SECTION("an engine that takes it commits the pass and the default together") {
        auto edited = Pelican::editGraphDocument(h.document, ops);
        h.client.reply(first, {{"status", "accepted"}, {"ticket", "edit"}});
        const auto poll = h.client.take("get_edit_result");
        h.document = std::move(edited);
        ++h.generation;
        h.client.reply(poll, {{"status", "committed"}, {"source_digest", {{"hex", h.digest()}}}});
        h.refresh();
        // Its Undo does not send it: the default is the project's.
        REQUIRE(h.client.model.undo());
        CHECK_FALSE(h.client.take("edit_graph_document").params.contains("material_route_default"));
    }
    SECTION("an engine that does not know the field is asked again without it") {
        h.client.model.receiveRpcFailure(
            first.request_id,
            "edit_graph_document contains unsupported field 'material_route_default'");
        const auto again = h.client.take("edit_graph_document");
        CHECK_FALSE(again.params.contains("material_route_default"));
        CHECK(again.params.at("ops") == ops);
        // Decision 4: its refusal reads as the Frame Plan's sentence.
        const Json unserved{
            {"code", "material_route_unserved@1"},
            {"message", "material_route_unserved@1: the engine's message"},
            {"unserved",
             Json::array({{{"route", "deferred_geometry"},
                           {"materials", Json::array({{{"name", "pelican.builtin.opaque"},
                                                       {"origin", "builtin"}}})},
                           {"hint", "set material_route_default to \"forward\""}}})}};
        h.client.reply(again, {{"status", "rejected"}, {"committed", false}, {"error", unserved}});
        CHECK(h.client.model.editNotice().kind == PelicanStudio::InspectorNoticeKind::Error);
        CHECK(h.client.model.editNotice().message == PelicanStudio::materialRouteUnservedNotice(unserved));
        CHECK(h.client.model.editNotice().message.find(
                  "No pass draws the deferred_geometry route, which pelican.builtin.opaque "
                  "(engine built-in) takes") != std::string::npos);
        h.refresh();
        // And the next addition does not offer the field again.
        CHECK(h.client.model.addNodeParams({}, "render.material_pass", "forward_opaque_v1").empty());
    }
    SECTION("any other failure is not retried") {
        h.client.model.receiveRpcFailure(first.request_id, "the engine went away");
        CHECK(std::ranges::none_of(h.client.model.takeRpcRequests(), [](const auto &request) {
            return request.method == "edit_graph_document";
        }));
    }
}

// GL-1d: a deferred material pass in the root, or one the frame plan routes
// deferred materials to, is a pass for them: no default is offered.
TEST_CASE("GL-1d a pipeline with a deferred material pass offers no route default",
          "[studio][graph-canvas][gl1d]") {
    auto document = lp4eDocument();
    document["nodes"].push_back({{"id", "gbuffer"}, {"op", "render.material_pass"},
                                 {"parameters", {{"material_contract", "deferred_geometry_v1"}}},
                                 {"outputs", Json::array()}});
    DrawLoopHarness h;
    h.document = document;
    h.open();
    CHECK(h.client.model.pipelineHasDeferredMaterialPass());
    CHECK(h.client.model.addNodeParams({}, "render.material_pass", "forward_opaque_v1").empty());
    // The shared judgment, as the Frame Plan's form asks it.
    CHECK(PelicanStudio::materialRouteOffersForwardDefault("forward_opaque_v1", false));
    CHECK_FALSE(PelicanStudio::materialRouteOffersForwardDefault("forward_opaque_v1", true));
    CHECK_FALSE(PelicanStudio::materialRouteOffersForwardDefault("deferred_geometry_v1", false));
    CHECK(PelicanStudio::materialContractTakesDeferredRoute("legacy_gbuffer_v1"));
    CHECK(PelicanStudio::materialContractTakesDeferredRoute(""));
    CHECK_FALSE(PelicanStudio::materialContractTakesDeferredRoute("forward_transparent_v1"));
}

// GL-1d, decision 2: Over's Draw items turns a loop a material pass reads into
// that pass's draw_loop.
TEST_CASE("GL-1d a loop a material pass reads can run over its draw items",
          "[studio][graph-canvas][gl1d]") {
    auto document = lp4eDocument();
    for (auto &node : document["nodes"])
        if (node.at("id") == "forward_opaque_draws")
            node["parameters"] = {{"over", {{"range", {{"count", 2}}}}},
                                  {"limit", 1},
                                  {"branches", Json::array({{{"body", "forward_opaque_draw"}}})}};
    document["nodes"].push_back({{"id", "unread"}, {"op", "graph.loop"},
                                 {"parameters", {{"over", {{"range", {{"count", 1}}}}},
                                                 {"branches", Json::array({{{"body", "forward_opaque_draw"}}})}}},
                                 {"outputs", {"draws"}}});
    DrawLoopHarness h;
    h.document = document;
    h.open();
    CHECK(h.client.model.loopDrawItemsContract("forward_opaque_draws") == "forward_opaque_v1");
    CHECK(h.client.model.loopDrawItemsContract("unread").empty());
    CHECK_FALSE(h.client.model.setLoopDrawItems("unread"));
    REQUIRE(h.client.model.setLoopDrawItems("forward_opaque_draws"));
    const auto parameters = h.loopParameters(h.refuse());
    CHECK(parameters == Json{{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                             {"tier", "draw_loop"},
                             {"order", {{"by", "phase_default"}}},
                             {"branches", Json::array({{{"body", "forward_opaque_draw"}}})}});
}

TEST_CASE("LP-4e a draw step arrives at the head of its body's list and its fields are one "
          "gesture each",
          "[studio][graph-canvas][lp4e]") {
    DrawLoopHarness h;
    h.open();
    REQUIRE(h.client.model.selectScope("forward_opaque_draw"));
    // Nothing drawn differently would be refused: an override arrives
    // drawing both faces.
    REQUIRE(h.client.model.addNode("draw.override"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("node").at("parameters") == Json{{"render_state", {{"cull", "none"}}}});
    const auto added = h.last_ops[0].at("node").at("id").get<std::string>();
    // Drawn first: the step that makes `draws` now follows it.
    CHECK(h.last_ops[1] == Json{{"op", "set_input"}, {"scope", "forward_opaque_draw"},
                                {"node", "step"}, {"port", "after"},
                                {"source", {{"node", added}, {"port", "draws"}}}});
    CHECK(h.document.at("definitions").at("forward_opaque_draw").at("outputs").at("draws") ==
          Json{{"node", "step"}, {"port", "draws"}});

    REQUIRE(h.client.model.setDrawOverride(added, {{"cull", "front"}, {"depth_write", false}},
                                           {"MASKED", "LEVEL=2"}));
    auto ops = h.refuse();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("parameters") ==
          Json{{"render_state", {{"cull", "front"}, {"depth_write", false}}},
               {"defines", {"MASKED", "LEVEL=2"}}});
    // An override of nothing is not sent.
    CHECK_FALSE(h.client.model.setDrawOverride(added, Json::object(), {}));

    // A named variant: the pass's material_variant is proposed, else "variant".
    CHECK(h.client.model.suggestDrawVariant() == "variant");
    REQUIRE(h.client.model.addNode("draw.variant"));
    h.commit();
    const auto variant = h.last_ops[0].at("node").at("id").get<std::string>();
    CHECK(h.last_ops[0].at("node").at("parameters") == Json{{"variant", "variant"}});
    REQUIRE(h.client.model.setDrawVariant(variant, "shadow"));
    ops = h.refuse();
    CHECK(ops[0].at("parameters") == Json{{"variant", "shadow"}});
    CHECK_FALSE(h.client.model.setDrawVariant(variant, ""));
}

TEST_CASE("LP-4e a draw_loop's fields: tags where, material branches, and every page of "
          "materials",
          "[studio][graph-canvas][lp4e]") {
    DrawLoopHarness h;
    h.open();
    const auto read = h.client.model.loop("forward_opaque_draws");
    REQUIRE(read);
    CHECK(read->draw_items);
    CHECK(read->contract == "forward_opaque_v1");
    CHECK(read->pass == "forward_opaque");
    REQUIRE(read->branches.size() == 1);
    CHECK(read->branches[0].when == "else");
    CHECK(read->body_outputs == std::vector<std::string>{"draws"});

    // The report holds the first 256 of 300 materials; the rest are asked for
    // from where the page ends, and nothing more once all are held.
    h.report(lp4eFramePlan(300, 0, 256));
    auto page = h.client.take("get_frame_plan");
    CHECK(page.params == Json{{"loop_elements", {{"node", "forward_opaque_draws"},
                                                 {"offset", 256}, {"limit", 4096}}}});
    h.client.reply(page, lp4eFramePlan(300, 256, 4096));
    CHECK(h.client.model.takeRpcRequests().empty());
    const auto materials = h.client.model.loopMaterials("forward_opaque_draws");
    REQUIRE(materials.size() == 300);
    CHECK(materials.front() == lp4eMaterial(0));
    CHECK(materials.back() == lp4eMaterial(299));
    const auto reported = h.client.model.loop("forward_opaque_draws");
    CHECK(reported->reported_total == 300);
    CHECK(reported->branches[0].matched_materials.size() == 300);
    CHECK(h.client.node("forward_opaque_draws").badge == "↻ draw_loop ×300");

    SECTION("where: the tags, without an empty list") {
        REQUIRE(h.client.model.setLoopWhereTags("forward_opaque_draws", {{"metal"}, {}, {"hero"}}));
        const auto parameters = h.loopParameters(h.refuse());
        CHECK(parameters.at("where") ==
              Json{{"tags", {{"include", {"metal"}}, {"require_all", {"hero"}}}}});
        CHECK(parameters.at("order") == Json{{"by", "phase_default"}});
    }
    SECTION("a new branch is for the first material the report names") {
        REQUIRE(h.client.model.addLoopBranch("forward_opaque_draws"));
        // UX-1, decision 4: over a copy of the else's body.
        auto ops = h.refuse();
        REQUIRE(ops.size() == 2);
        CHECK(ops[0].at("op") == "add_definition");
        CHECK(ops[0].at("name") == "forward_opaque_draws_body");
        ops.erase(ops.begin());
        const auto parameters = h.loopParameters(ops);
        CHECK(parameters.at("branches") ==
              Json::array({{{"when", {{"material", lp4eMaterial(0)}}},
                            {"body", "forward_opaque_draws_body"}},
                           {{"body", "forward_opaque_draw"}}}));
    }
    SECTION("each attribute of a branch's condition, and all of them ANDed") {
        auto branched = lp4eDocument();
        branched["nodes"][2]["parameters"]["branches"] =
            Json::array({{{"when", {{"material", lp4eMaterial(1)}}}, {"body", nullptr}},
                         {{"body", "forward_opaque_draw"}}});
        DrawLoopHarness b;
        b.document = branched;
        b.open();
        const auto table = b.client.model.loop("forward_opaque_draws");
        REQUIRE(table->branches.size() == 2);
        CHECK(table->branches[0].when == "draw");
        CHECK(table->branches[0].draw_when.material == lp4eMaterial(1));
        CHECK(table->branches[0].nothing);
        const auto when = [&](const GraphCanvasDrawWhen &condition) {
            REQUIRE(b.client.model.setLoopBranchDrawWhen("forward_opaque_draws", 0, condition));
            return b.loopParameters(b.refuse()).at("branches").at(0).at("when");
        };
        GraphCanvasDrawWhen material;
        material.material = lp4eMaterial(7);
        CHECK(when(material) == Json{{"material", lp4eMaterial(7)}});
        GraphCanvasDrawWhen tags;
        tags.tags = GraphCanvasTagFilter{{"glass"}, {"decal"}, {}};
        CHECK(when(tags) == Json{{"tags", {{"include", {"glass"}}, {"exclude", {"decal"}}}}});
        GraphCanvasDrawWhen contract;
        contract.contract = "gbuffer_v1";
        CHECK(when(contract) == Json{{"contract", "gbuffer_v1"}});
        GraphCanvasDrawWhen skinned;
        skinned.skinned = false;
        CHECK(when(skinned) == Json{{"skinned", false}});
        GraphCanvasDrawWhen variant;
        variant.has_variant = true;
        CHECK(when(variant) == Json{{"has_variant", true}});
        GraphCanvasDrawWhen all = material;
        all.skinned = true;
        all.has_variant = false;
        CHECK(when(all) == Json{{"material", lp4eMaterial(7)}, {"skinned", true}, {"has_variant", false}});
        // Nothing to test is the else, which only the last row is.
        CHECK_FALSE(b.client.model.setLoopBranchDrawWhen("forward_opaque_draws", 0, {}));
        GraphCanvasDrawWhen unknown;
        unknown.contract = "forward_opaque_v1";
        CHECK_FALSE(b.client.model.setLoopBranchDrawWhen("forward_opaque_draws", 0, unknown));
        CHECK_FALSE(b.client.model.setLoopBranchDrawWhen("forward_opaque_draws", 1, material));
    }
}

TEST_CASE("LP-4e a draw_loop's refusals read as sentences that say what to change",
          "[studio][graph-canvas][lp4e]") {
    const auto sentence = [](Json error) {
        return PelicanStudio::graphLoopRefusalDescription(error);
    };
    CHECK(sentence({{"code", "graph.loop_tier_unsupported@1"}, {"node", "draws"},
                    {"reason", "carry_in_draw_loop"}}) ==
          "Loop draws draws inside one material pass, where no material hands a value to the "
          "next: remove its carry (graph.loop_tier_unsupported@1).");
    CHECK(sentence({{"code", "graph.loop_tier_unsupported@1"}, {"node", "draws"},
                    {"reason", "limit_in_draw_loop"}}) ==
          "Loop draws draws every material of its pass: remove its limit "
          "(graph.loop_tier_unsupported@1).");
    CHECK(sentence({{"code", "graph.loop_tier_unsupported@1"}, {"node", "draws"},
                    {"reason", "resource_in_draw_body"}}) ==
          "A body of loop draws makes a resource, which a draw_loop body cannot: it holds draw "
          "steps only (graph.loop_tier_unsupported@1).");
    CHECK(sentence({{"code", "graph.loop_tier_unsupported@1"}, {"node", "draws"},
                    {"reason", "mixed_body"}}) ==
          "The bodies of loop draws mix draw steps with passes: a body holds draw steps only "
          "(draw_loop) or passes only (expand) (graph.loop_tier_unsupported@1).");
    // Another reason keeps the code's own sentence.
    CHECK(sentence({{"code", "graph.loop_tier_unsupported@1"}, {"node", "steps"},
                    {"reason", "nested"}}) ==
          "Loop steps has no stage it can run in (nested) (graph.loop_tier_unsupported@1).");
    CHECK(sentence({{"code", "graph.loop_draws_unconnected@1"}, {"node", "draws"}, {"count", 0}}) ==
          "The draws of loop draws reach 0 inputs: they must feed the draws of exactly one "
          "render.material_pass (graph.loop_draws_unconnected@1).");
    CHECK(sentence({{"code", "graph.definition_role@1"}, {"definition", "forward_opaque_draw"},
                    {"node", "misuse"}}) ==
          "Definition forward_opaque_draw holds draw steps, so only a draw_loop runs it; the "
          "instance misuse cannot (graph.definition_role@1).");
    CHECK(sentence({{"code", "graph.loop_contract_unsupported@1"}, {"node", "draws"},
                    {"contract", "legacy_gbuffer_v1"}}) ==
          "Loop draws draws with legacy_gbuffer_v1, which its stage does not take "
          "(graph.loop_contract_unsupported@1).");
}

namespace {

// LP-2 (engine 992225fb): a draw-loop refusal as the render edit service
// returns it -- its own code, its fields beside ticket and participant, and a
// message that is still "<code>: <fields JSON>". In a graph variant's
// preflight `variant` beside the code is the graph variant. (LP-4d2: since
// engine 5e246063 no refusal's own field is called `variant`;
// graph.loop_variant_missing@1 names the material's `material_variant`.)
Json lp2DrawLoopRefusal(const std::string &code, const Json &fields,
                        const std::string &graph_variant = {}) {
    Json error{{"participant", "pelican.render_pipeline"}, {"ticket", "t-lp2"}};
    if (!graph_variant.empty()) error["variant"] = graph_variant;
    for (const auto &[key, value] : fields.items())
        if (!error.contains(key)) error[key] = value;
    error["code"] = code;
    error["message"] = code + ": " + fields.dump();
    return error;
}

} // namespace

TEST_CASE("LP-2 the draw-loop refusals the engine names read as sentences, a graph variant "
          "first, and mark the loop",
          "[studio][graph-canvas][lp2][lp4e]") {
    const auto sentence = [](const Json &error) {
        return PelicanStudio::graphLoopRefusalDescription(error);
    };
    const Json variant_missing{{"node", "draws"}, {"material", "box"},
                               {"material_variant", "outline"}};
    CHECK(sentence(lp2DrawLoopRefusal("graph.loop_phase_crossing@1",
                                      {{"node", "draws"}, {"material", "box"}})) ==
          "Loop draws: material box draws in another phase (graph.loop_phase_crossing@1).");
    CHECK(sentence(lp2DrawLoopRefusal("graph.loop_variant_missing@1", variant_missing)) ==
          "Loop draws: material box has no outline variant (graph.loop_variant_missing@1).");
    CHECK(sentence(lp2DrawLoopRefusal("graph.loop_override_interface@1",
                                      {{"node", "draws"}, {"material", "box"},
                                       {"binding", "set 1 binding 0"}})) ==
          "Loop draws: material box has no binding set 1 binding 0 "
          "(graph.loop_override_interface@1).");
    CHECK(sentence(lp2DrawLoopRefusal("graph.loop_override_needs_compiler@1",
                                      {{"node", "draws"}})) ==
          "Loop draws changes shader defines, which needs the runtime shader compiler "
          "(graph.loop_override_needs_compiler@1).");
    CHECK(sentence(lp2DrawLoopRefusal("graph.loop_override_compile_limit@1",
                                      {{"node", "draws"}, {"count", 17}, {"limit", 16}})) ==
          "Loop draws needs 17 shader compiles; the limit is 16 "
          "(graph.loop_override_compile_limit@1).");
    // In the preview graph's preflight: the material variant is the
    // refusal's material_variant, never the `variant` the graph variant
    // took (LP-4d2, decision 8; a sentence that read `variant` would say
    // "preview" here, and "?" in the main graph's).
    const auto in_preview =
        lp2DrawLoopRefusal("graph.loop_variant_missing@1", variant_missing, "preview");
    REQUIRE(in_preview.at("variant") == "preview");
    CHECK(sentence(in_preview) ==
          "Loop draws: material box has no outline variant (graph.loop_variant_missing@1).");
    // The same when the fields travel as error_fields, the message says
    // nothing, and `variant` is the graph's.
    const Json relayed{{"code", "graph.loop_variant_missing@1"},
                       {"variant", "xr"},
                       {"error_fields", variant_missing}};
    CHECK(sentence(relayed) ==
          "Loop draws: material box has no outline variant (graph.loop_variant_missing@1).");
    // `variant` beside the code is the graph variant, whatever the message
    // quotes -- even a message that quotes a `variant` of its own.
    CHECK(PelicanStudio::graphRefusalVariant(
              lp2DrawLoopRefusal("graph.loop_variant_missing@1", variant_missing)).empty());
    CHECK(PelicanStudio::graphRefusalVariant(in_preview) == "preview");
    Json quoting = in_preview;
    quoting["message"] = std::string{"graph.loop_variant_missing@1: "} +
                         Json{{"node", "draws"}, {"variant", "preview"}}.dump();
    CHECK(PelicanStudio::graphRefusalVariant(quoting) == "preview");
    CHECK(PelicanStudio::graphRefusalVariant(relayed) == "xr");
    CHECK(PelicanStudio::graphRefusalVariant(lp2DrawLoopRefusal(
              "graph.loop_phase_crossing@1", {{"node", "draws"}, {"material", "box"}}, "xr")) ==
          "xr");

    LoopHarness h;
    h.open();
    SECTION("the main graph's refusal: the sentence alone, and the loop is marked") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse(lp2DrawLoopRefusal("graph.loop_variant_missing@1",
                                    {{"node", "steps"}, {"material", "box"},
                                     {"material_variant", "outline"}}));
        CHECK(h.client.model.notice().message ==
              "Loop steps: material box has no outline variant (graph.loop_variant_missing@1).");
        CHECK(h.client.node("steps").refused);
    }
    SECTION("a graph variant's: named first, and the loop is marked the same way") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse(lp2DrawLoopRefusal("graph.loop_override_compile_limit@1",
                                    {{"node", "steps"}, {"count", 17}, {"limit", 16}}, "preview"));
        CHECK(h.client.model.notice().message ==
              "The preview graph refused it: Loop steps needs 17 shader compiles; the limit is 16 "
              "(graph.loop_override_compile_limit@1).");
        CHECK(h.client.node("steps").refused);
    }
}

// --- Q-19: a render node's input sockets, lines into them, and a
// definition renamed or deleted from the Graph tab.

namespace {

// The LP-4c document with a root pass `post` drawing with the passthrough
// into an image of its own, and one more image a line can come from.
Json q19Document() {
    auto document = lp4cDocument();
    const auto type = lp4cColorType();
    const auto image = [&](const char *id) {
        return Json{{"id", id}, {"op", "render.image"},
                    {"parameters", {{"type", type}, {"format", "R16G16B16A16_SFLOAT"},
                                    {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
                    {"outputs", {"resource"}}};
    };
    document["nodes"].push_back(image("post_image"));
    document["nodes"].push_back(image("extra"));
    document["nodes"].push_back(
        {{"id", "post"}, {"op", "render.fullscreen"},
         {"parameters", {{"pass_set", "main_render"},
                         {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                         {"ports", {{"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                               {"attachment", "color"}, {"location", 0}}}}}}},
         {"inputs", {{"color", {{"node", "post_image"}, {"port", "resource"}}}}},
         {"outputs", {"color"}}});
    return document;
}

Json q19PassInputs(const Json &declarations, const Json &range = nullptr) {
    return {{"state", "declared"}, {"fragment", "engine://passthrough"},
            {"declarations", declarations}, {"variadic_range", range}};
}

// The plan's word on the two passes: `post` draws with the passthrough, whose
// inputs are all one variadic group; element 1's copy of `fill` (its branch
// runs `paint`) with two fixed images and a buffer. No copy of `first_fill`
// is listed.
Json q19Plan(std::size_t max = 8) {
    auto plan = lp4cFramePlan();
    plan["nodes"] = Json::array({
        {{"name", "post"}, {"kind", "fullscreen"},
         {"pass_inputs", q19PassInputs(Json::array({{{"binding", 0}, {"type", "sampler2D"},
                                                     {"name", "pelican_inputs"}, {"variadic", true}}}),
                                       {{"first", 0}, {"max", max}})}},
        {{"name", "steps/1/fill"}, {"kind", "fullscreen"},
         {"loop_origin", {{"loop", "steps"}, {"element", 1}, {"node", "fill"}}},
         {"pass_inputs", q19PassInputs(Json::array({
             {{"binding", 0}, {"type", "sampler2D"}, {"name", "inputTexture"}},
             {{"binding", 1}, {"type", "sampler2D"}, {"name", "bloomTexture"}},
             {{"binding", 8}, {"type", "uniform buffer"}, {"name", "settings"}}}))}}});
    return plan;
}

Json q19Port(std::int64_t index) {
    return {{"kind", "image"}, {"type", lp4cColorType()}, {"access", "read"},
            {"binding", "input"}, {"input_index", index}};
}

// UX-7, decision 1: a line onto a fullscreen pass's socket makes the port
// clamped at the image's edge (the engine's default is repeat).
Json q19Connected(std::int64_t index) {
    auto port = q19Port(index);
    port["sampling"] = {{"address", "clamp_to_edge"}};
    return port;
}

struct Q19Harness : LoopHarness {
    Q19Harness() { document = q19Document(); }
    // Every edit reads the document again, and that read asks for the plan.
    void commitAndReport(const Json &plan = q19Plan()) {
        commit();
        report(plan);
    }
    // Whether anything asked the engine to edit since the last look.
    bool sentEdit() {
        const auto requests = client.model.takeRpcRequests();
        return std::ranges::any_of(requests, [](const auto &request) {
            return request.method == "edit_graph_document";
        });
    }
    Json &node(const std::string &scope, const std::string &id) {
        auto &nodes = scope.empty() ? document.at("nodes")
                                    : document.at("definitions").at(scope).at("nodes");
        const auto found = std::ranges::find_if(nodes, [&](const Json &each) {
            return each.at("id") == id;
        });
        REQUIRE(found != nodes.end());
        return *found;
    }
};

} // namespace

TEST_CASE("Q-19 a render node's sockets come from its pass's declarations; without them it keeps "
          "its rows",
          "[studio][graph-canvas][q19]") {
    using Kind = PelicanStudio::GraphCanvasInputSocket::Kind;
    Q19Harness h;
    // post already reads lit as its input 0.
    auto &post = h.node({}, "post");
    post["parameters"]["ports"]["lit"] = q19Port(0);
    post["inputs"]["lit"] = {{"node", "lit"}, {"port", "resource"}};
    REQUIRE_NOTHROW(Pelican::editGraphDocument(h.document, Json::array()));
    h.open();
    const auto hasRow = [](const GraphCanvasNode &node, const std::string &port) {
        return std::ranges::any_of(node.inputs, [&](const auto &row) { return row.port == port; });
    };
    // Before the plan says anything, the numbered port is a row like any other.
    CHECK_FALSE(h.client.node("post").declared_sockets);
    CHECK(hasRow(h.client.node("post"), "lit"));

    h.report(q19Plan());
    const auto drawn = h.client.node("post");
    REQUIRE(drawn.declared_sockets);
    REQUIRE(drawn.input_sockets.size() == 2);
    // The variadic band: the member connected at binding 0, then its end.
    CHECK(drawn.input_sockets[0].kind == Kind::variadic);
    CHECK(drawn.input_sockets[0].binding == 0);
    CHECK(drawn.input_sockets[0].key() == "in@0");
    CHECK(drawn.input_sockets[0].port == "lit");
    CHECK(drawn.input_sockets[0].source_node == "lit");
    CHECK(drawn.input_sockets[0].label == "← lit.resource");
    CHECK(drawn.input_sockets[1].kind == Kind::variadic_drop);
    CHECK(drawn.input_sockets[1].binding == 1);
    CHECK(drawn.input_sockets[1].key() == "in:variadic");
    CHECK_FALSE(drawn.input_sockets[1].connected());
    // The numbered port is the socket's now; the write port stays a row.
    CHECK_FALSE(hasRow(drawn, "lit"));
    CHECK(hasRow(drawn, "color"));

    // A body node's sockets are read off the copy a loop made of it: element
    // 1's branch runs paint.
    REQUIRE(h.client.model.selectScope("paint"));
    const auto fill = h.client.node("fill");
    REQUIRE(fill.declared_sockets);
    REQUIRE(fill.input_sockets.size() == 3);
    CHECK(fill.input_sockets[0].kind == Kind::declared);
    CHECK(fill.input_sockets[0].declaration == "inputTexture");
    CHECK(fill.input_sockets[0].key() == "in@0");
    CHECK(fill.input_sockets[0].label == "inputTexture");
    CHECK_FALSE(fill.input_sockets[0].connected());
    CHECK(fill.input_sockets[1].declaration == "bloomTexture");
    CHECK(fill.input_sockets[1].key() == "in@1");
    CHECK(fill.input_sockets[2].buffer);
    CHECK(fill.input_sockets[2].key() == "in@8");
    // The control: no copy of first's pass is in the plan, so it keeps the
    // rows it had.
    REQUIRE(h.client.model.selectScope("first"));
    CHECK_FALSE(h.client.node("first_fill").declared_sockets);
    CHECK(h.client.node("first_fill").input_sockets.empty());
    CHECK(hasRow(h.client.node("first_fill"), "color"));

    // A plan that says the pass has no pipeline: the rows again.
    auto unavailable = q19Plan();
    unavailable["nodes"][0]["pass_inputs"] = {{"state", "unavailable"}, {"reason", "no pipeline"}};
    REQUIRE(h.client.model.selectScope({}));
    h.client.model.refresh();
    h.refresh();
    h.report(unavailable);
    CHECK_FALSE(h.client.node("post").declared_sockets);
    CHECK(hasRow(h.client.node("post"), "lit"));
}

TEST_CASE("Q-19 a line on a socket is a numbered port and its edge in one list; taking it away "
          "moves the later ones down",
          "[studio][graph-canvas][q19]") {
    Q19Harness h;
    h.open();
    h.report(q19Plan());
    const auto post_parameters = h.node({}, "post").at("parameters");
    const auto edge = [](const char *node) { return Json{{"node", node}, {"port", "resource"}}; };

    // Onto the band's end: a port named after the source's port, at the next
    // binding, and the edge into it -- one edit.
    REQUIRE(h.client.model.connectSocket({}, "post", "in:variadic", "lit", "resource"));
    h.commitAndReport();
    auto expected = post_parameters;
    expected["ports"]["resource"] = q19Connected(0);
    CHECK(h.last_ops == Json::array({
        {{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "post"}, {"parameters", expected}},
        {{"op", "set_input"}, {"scope", "root"}, {"node", "post"}, {"port", "resource"},
         {"source", edge("lit")}}}));
    CHECK(h.node({}, "post").at("inputs").at("resource") == edge("lit"));
    // Drawn as the band's member at binding 0.
    CHECK(h.client.node("post").input_sockets.at(0).port == "resource");

    // A second one takes binding 1 and a name of its own.
    REQUIRE(h.client.model.connectSocket({}, "post", "in:variadic", "extra", "resource"));
    h.commitAndReport();
    CHECK(h.node({}, "post").at("parameters").at("ports").at("resource_2") == q19Connected(1));
    CHECK(h.node({}, "post").at("inputs").at("resource_2") == edge("extra"));

    // Onto the connected socket at binding 0: the edge moves, nothing else.
    REQUIRE(h.client.model.connectSocket({}, "post", "in@0", "extra", "resource"));
    h.commitAndReport();
    CHECK(h.last_ops == Json::array({{{"op", "set_input"}, {"scope", "root"}, {"node", "post"},
                                      {"port", "resource"}, {"source", edge("extra")}}}));
    const auto replaced = h.document;
    // The same line again is no edit at all.
    CHECK_FALSE(h.client.model.connectSocket({}, "post", "in@0", "extra", "resource"));
    CHECK_FALSE(h.sentEdit());

    // Taking binding 0's line away takes its port, and binding 1 becomes 0.
    REQUIRE(h.client.model.disconnectInput({}, "post", "resource"));
    h.commitAndReport();
    auto renumbered = post_parameters;
    renumbered["ports"]["resource_2"] = q19Connected(0);
    CHECK(h.last_ops == Json::array({
        {{"op", "set_input"}, {"scope", "root"}, {"node", "post"}, {"port", "resource"},
         {"source", nullptr}},
        {{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "post"},
         {"parameters", renumbered}}}));
    CHECK_FALSE(h.node({}, "post").at("inputs").contains("resource"));
    CHECK(h.node({}, "post").at("parameters") == renumbered);

    // Undo and Redo come back byte for byte.
    const auto after = h.document;
    REQUIRE(h.client.model.undo());
    h.commitAndReport();
    CHECK(h.document.dump(2) == replaced.dump(2));
    REQUIRE(h.client.model.redo());
    h.commitAndReport();
    CHECK(h.document.dump(2) == after.dump(2));
}

TEST_CASE("Q-19 a line on a socket past the next binding, on a buffer or on a full band is "
          "refused in one line, and a body's pass reads its boundary the same way",
          "[studio][graph-canvas][q19]") {
    Q19Harness h;
    h.open();
    h.report(q19Plan(1));

    // In a body: binding 1 before binding 0 is a skipped binding.
    CHECK_FALSE(h.client.model.connectSocket("paint", "fill", "in@1", "target", "value"));
    CHECK_FALSE(h.sentEdit());
    CHECK(h.client.model.notice().message ==
          "Connect binding 0 of fill first: a pass's inputs fill the bindings in order.");
    CHECK_FALSE(h.client.model.connectSocket("paint", "fill", "in@8", "target", "value"));
    CHECK_FALSE(h.sentEdit());
    // UX-8, decision 2: a buffer binding takes a named buffer port, but a
    // uniform buffer is none a pass can hand over.
    CHECK(h.client.model.notice().message ==
          "Binding 8 of fill is a uniform buffer: a pass hands its shader storage buffers "
          "(declare it as a `readonly buffer` block).");

    // Binding 0 is the next: the boundary port target, named by its id and
    // typed by what the loop is fed there (lit), in the body's scope.
    REQUIRE(h.client.model.connectSocket("paint", "fill", "in@0", "target", "value"));
    h.commitAndReport(q19Plan(1));
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("scope") == "paint");
    CHECK(h.last_ops[0].at("parameters").at("ports").at("target") == q19Connected(0));
    CHECK(h.last_ops[1] == Json{{"op", "set_input"}, {"scope", "paint"}, {"node", "fill"},
                                {"port", "target"},
                                {"source", {{"node", "target"}, {"port", "value"}}}});
    // Now binding 1 is the next one, and the same source a second time is
    // a port of its own.
    REQUIRE(h.client.model.connectSocket("paint", "fill", "in@1", "target", "value"));
    h.commitAndReport(q19Plan(1));
    CHECK(h.node("paint", "fill").at("parameters").at("ports").at("target_2") == q19Connected(1));

    // The root pass's band holds one input at most here.
    REQUIRE(h.client.model.connectSocket({}, "post", "in:variadic", "lit", "resource"));
    h.commitAndReport(q19Plan(1));
    CHECK_FALSE(h.client.model.connectSocket({}, "post", "in:variadic", "extra", "resource"));
    CHECK_FALSE(h.sentEdit());
    CHECK(h.client.model.notice().message ==
          "post's shader takes at most 1 inputs in its variadic group.");
}

TEST_CASE("Q-19 a definition is renamed and deleted from the Graph tab, checked before it is "
          "sent, and both come back byte for byte",
          "[studio][graph-canvas][q19]") {
    LoopHarness h;
    h.open();
    h.report();
    const auto before = h.document;
    const auto sentEdit = [&] {
        const auto requests = h.client.model.takeRpcRequests();
        return std::ranges::any_of(requests, [](const auto &request) {
            return request.method == "edit_graph_document";
        });
    };
    // The node id rule and "root", checked before anything is sent.
    const std::vector<std::pair<std::string, std::string>> refused{
        {"", "A definition needs a name."},
        {"a/b", "A definition's name cannot hold a slash: a/b."},
        {"root", "A definition cannot be named root: that name is the root document's."},
        {"first", "A definition named first already exists."}};
    for (const auto &[name, line] : refused) {
        CHECK(h.client.model.definitionNameRefusal("paint", name) == line);
        CHECK_FALSE(h.client.model.renameDefinition("paint", name));
        CHECK_FALSE(sentEdit());
        CHECK(h.client.model.notice().message == line);
    }
    CHECK(h.client.model.definitionNameRefusal("paint", "paint").empty());

    // Rename is the one op; the engine moves the loop's branch with it, and
    // the trail standing in it follows it.
    REQUIRE(h.client.model.selectScope("paint"));
    REQUIRE(h.client.model.renameDefinition("paint", "brush"));
    h.commit();
    CHECK(h.last_ops == Json::array({{{"op", "rename_definition"}, {"from", "paint"}, {"to", "brush"}}}));
    CHECK(h.document.at("nodes").at(1).at("parameters").at("branches").at(1).at("body") == "brush");
    CHECK(h.client.model.currentScope() == "brush");
    const auto renamed = h.document;
    REQUIRE(h.client.model.undo());
    h.commit();
    CHECK(h.document.dump(2) == before.dump(2));
    CHECK(h.client.model.currentScope() == "paint");
    REQUIRE(h.client.model.redo());
    h.commit();
    CHECK(h.document.dump(2) == renamed.dump(2));
    CHECK(h.client.model.currentScope() == "brush");

    // Delete: a definition something runs is not sent, and the line says what.
    CHECK(h.client.model.definitionReferrers("brush") == std::vector<std::string>{"steps"});
    CHECK_FALSE(h.client.model.removeDefinition("brush"));
    CHECK_FALSE(sentEdit());
    CHECK(h.client.model.notice().message ==
          "Definition brush is used by steps: make them run another definition first.");
    // One nothing runs goes, and comes back with its body.
    CHECK(h.client.model.definitionReferrers("spare").empty());
    REQUIRE(h.client.model.removeDefinition("spare"));
    h.commit();
    CHECK(h.last_ops == Json::array({{{"op", "remove_definition"}, {"name", "spare"}}}));
    CHECK_FALSE(h.document.at("definitions").contains("spare"));
    REQUIRE(h.client.model.undo());
    h.commit();
    CHECK(h.document.dump(2) == renamed.dump(2));
    REQUIRE(h.client.model.redo());
    h.commit();
    CHECK_FALSE(h.document.at("definitions").contains("spare"));
}

TEST_CASE("Q-19 graph.input_index_invalid@1 reads as a sentence for each reason",
          "[studio][graph-canvas][q19]") {
    using PelicanStudio::graphLoopRefusalDescription;
    CHECK(graphLoopRefusalDescription({{"code", "graph.input_index_invalid@1"}, {"node", "post"},
                                       {"input_index", 1}, {"reason", "missing"},
                                       {"ports", Json::array()}}) ==
          "No port of post reads input 1: its numbered inputs run from 0 without a gap "
          "(graph.input_index_invalid@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.input_index_invalid@1"}, {"node", "post"},
                                       {"input_index", 0}, {"reason", "duplicate"},
                                       {"ports", {"a", "b"}}}) ==
          "Ports a, b of post all read input 0: each number is one port's "
          "(graph.input_index_invalid@1).");
    // As the engine quotes it in a message.
    CHECK(graphLoopRefusalDescription(
              {{"code", "invalid_graph_document"},
               {"message", "graph.input_index_invalid@1 "
                           "{\"input_index\":2,\"node\":\"post\",\"ports\":[],\"reason\":\"missing\"}"}}) ==
          "No port of post reads input 2: its numbered inputs run from 0 without a gap "
          "(graph.input_index_invalid@1).");
}

// GL-1 (design_graph_layers 3.3, 3.4). A frame plan read from nothing but
// JSON: a preset's material pass and lighting pass, a feature of three
// passes, an anchor, a pass the graph's own render node made, and a present
// of the rendering section. The root holds that render node, the target_ref
// it reads through, and a value node feeding it.
namespace {
Json gl1Document() {
    Json blur_inputs = Json::object();
    blur_inputs["source"] = Json{{"node", "bloomed"}, {"port", "resource"}};
    blur_inputs["strength"] = Json{{"node", "gain"}, {"port", "value"}};
    return {{"source_reference", "fixture"},
            {"root_schema", "pelican.graph"},
            {"source_digest", {{"hex", "gl1"}}},
            {"nodes", Json::array({
                 {{"id", "gain"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
                  {"outputs", Json::array({"value"})}},
                 {{"id", "bloomed"}, {"op", "render.target_ref"},
                  {"parameters", {{"name", "scene_color_bloomed"}}},
                  {"outputs", Json::array({"resource"})}},
                 {{"id", "blur"}, {"op", "render.fullscreen"}, {"parameters", Json::object()},
                  {"inputs", blur_inputs}, {"outputs", Json::array({"color"})}}})},
            {"definitions", Json::object()}};
}

Json gl1Pass(const std::string &name, std::size_t order, const std::string &source,
             const Json &reads, const Json &writes, const std::string &feature = {}) {
    Json node{{"name", name},       {"kind", "render"}, {"order", order},  {"level", order},
              {"source", source},   {"reads", reads},   {"writes", writes}, {"reads_history", Json::array()},
              {"declaration_index", order}};
    if (!feature.empty()) node["provider_feature"] = feature;
    return node;
}

Json gl1Plan() {
    const std::string bloom = "engine://features/bloom.json";
    auto gbuffer = gl1Pass("gbuffer", 0, "engine", Json::array(), Json::array({"g_albedo", "g_depth"}));
    gbuffer["shader_resolution"] = {{"state", "material_owned"}};
    auto anchor = gl1Pass("post", 5, "engine", Json::array(), Json::array());
    anchor["kind"] = "anchor";
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"material_routing",
             {{"routes", {{"deferred", {{"pass", "gbuffer"}, {"contract", "gbuffer_v1"}}}}}}},
            {"loops", Json::array()},
            {"nodes", Json::array({
                 gbuffer,
                 gl1Pass("lighting", 1, "engine", Json::array({"g_albedo", "g_depth"}),
                         Json::array({"scene_color"})),
                 gl1Pass("bloom_down", 2, "feature:bloom", Json::array({"scene_color"}),
                         Json::array({"bloom_0"}), bloom),
                 gl1Pass("bloom_up", 3, "feature:bloom", Json::array({"bloom_0"}),
                         Json::array({"bloom_1"}), bloom),
                 gl1Pass("bloom_mix", 4, "feature:bloom", Json::array({"scene_color", "bloom_1"}),
                         Json::array({"scene_color_bloomed"}), bloom),
                 anchor,
                 // The graph's own render node made this one: its name is the
                 // node's id, and its source says nothing about that.
                 gl1Pass("blur", 6, "engine", Json::array({"scene_color_bloomed"}),
                         Json::array({"blur_image"})),
                 gl1Pass("present", 7, "project", Json::array({"blur_image"}),
                         Json::array({"display"}))})}};
}

Json gl1Templates() {
    Json body{{"nodes", Json::array({{{"id", "down"}, {"op", "render.fullscreen"}}})},
              {"outputs", Json::object()}};
    Json loop{{"id", "bloom_steps"}, {"op", "graph.loop"},
              {"parameters", {{"over", {{"range", {{"count", 3}}}}},
                              {"branches", Json::array({{{"body", "bloom_step"}}})}}}};
    return {{"templates", Json::array({
                {{"reference", "engine://graphs/bloom_loop.json"},
                 {"title", "Bloom as a loop"},
                 {"decomposes", {{"feature", "engine://features/bloom.json"}}},
                 {"document", {{"nodes", Json::array({loop})},
                               {"definitions", {{"bloom_step", body}}}}}}})}};
}

const PelicanStudio::GraphCanvasFoldedNode &folded(
    const std::vector<PelicanStudio::GraphCanvasFoldedNode> &nodes, const std::string &id) {
    const auto found = std::ranges::find(nodes, id, &PelicanStudio::GraphCanvasFoldedNode::id);
    REQUIRE(found != nodes.end());
    return *found;
}

bool asked(GraphCanvasModel &model, std::string_view method) {
    const auto requests = model.takeRpcRequests();
    return std::ranges::any_of(requests, [&](const PelicanStudio::InspectorRpcRequest &request) {
        return request.method == method;
    });
}
} // namespace

TEST_CASE("GL-1 the plan's passes no graph node made are folded nodes, read only, "
          "with the lines the plan implies and the domain filter's cut ends",
          "[studio][graph-canvas][gl1]") {
    Harness h;
    h.open(gl1Document());
    // Decision 1: the plan is read with the document, loop or no loop.
    h.reply(h.take("get_frame_plan"), gl1Plan());
    // A plan that names a feature asks, once, which have a decomposed version.
    const auto templates = h.take("get_graph_templates");
    auto &model = h.model;
    CHECK(model.planGraph() == "main_render");

    // One node per feature, one per other pass, in the plan's order; neither
    // the anchor nor the pass the graph's own node made.
    auto nodes = model.foldedNodes();
    std::vector<std::string> ids;
    for (const auto &node : nodes) ids.push_back(node.id);
    CHECK(ids == std::vector<std::string>{"gbuffer", "lighting", "feature:bloom", "present"});
    const auto &bloom = folded(nodes, "feature:bloom");
    CHECK(bloom.feature_group);
    CHECK(bloom.label == "bloom");
    CHECK(bloom.type == "feature");
    CHECK(bloom.passes == std::vector<std::string>{"bloom_down", "bloom_up", "bloom_mix"});
    // What it reads and hands on; bloom_0 and bloom_1 stay inside.
    CHECK(bloom.reads == std::vector<std::string>{"scene_color"});
    CHECK(bloom.writes == std::vector<std::string>{"scene_color_bloomed"});
    // GL-5a, decision 1: a feature's fold is read only; the way to edit it is
    // to expand it.
    CHECK_FALSE(bloom.rendering_section);
    CHECK(bloom.tooltip() == "From feature bloom: read only here. Expand to edit\xE2\x80\xA6");
    const auto &gbuffer = folded(nodes, "gbuffer");
    CHECK(gbuffer.material);
    CHECK(gbuffer.type == "material");
    CHECK(folded(nodes, "lighting").type == "render");
    CHECK_FALSE(folded(nodes, "lighting").enterable());

    // Decision 2: the lines, one per pair of nodes, the resources listed. A
    // graph node reads through the target_ref that names the resource.
    const auto edges = model.derivedEdges();
    using Edge = PelicanStudio::GraphCanvasDerivedEdge;
    CHECK(edges == std::vector<Edge>{
                       {"gbuffer", "lighting", {"g_albedo", "g_depth"}},
                       {"lighting", "feature:bloom", {"scene_color"}},
                       {"feature:bloom", "bloomed", {"scene_color_bloomed"}},
                       {"blur", "present", {"blur_image"}}});

    // Decision 6: where each pass is drawn on the root canvas.
    CHECK(model.canvasNodeOfPass("bloom_up") == "feature:bloom");
    CHECK(model.canvasNodeOfPass("blur") == "blur");
    CHECK(model.canvasNodeOfPass("gbuffer") == "gbuffer");
    CHECK(model.canvasNodeOfPass("post").empty());

    // Decision 5: Render only. The value node is not drawn; the render node's
    // input it fed says where the line went.
    for (const auto &domain : GraphCanvasModel::domainNames())
        model.setDomainShown(domain, domain == "render");
    const auto root = model.nodes();
    const auto gain = std::ranges::find(root, std::string{"gain"}, &GraphCanvasNode::id);
    REQUIRE(gain != root.end());
    CHECK_FALSE(model.nodeShown({}, *gain));
    const auto stubs = model.domainStubs();
    REQUIRE(stubs.size() == 1);
    CHECK(stubs[0].node == "blur");
    CHECK(stubs[0].port == "strength");
    CHECK(stubs[0].input);
    CHECK(stubs[0].count == 1);
    CHECK(stubs[0].text() == "\xE2\x86\x90 value (1)");
    // Without Render, no folded node either.
    model.setDomainShown("render", false);
    CHECK(model.foldedNodes().empty());
    model.setDomainShown("render", true);

    // Decision 3: inside the material pass, Studio's own drawing of it -- a
    // render.material_pass taking an else-only draw_loop whose body draws
    // each material.
    REQUIRE(model.enterFolded("gbuffer"));
    CHECK(model.foldedView());
    CHECK(model.displayTrail() == std::vector<std::string>{"root", "gbuffer"});
    CHECK(model.foldedNodes().empty());
    auto inside = model.nodes();
    REQUIRE(inside.size() == 2);
    const auto pass = std::ranges::find(inside, std::string{"render.material_pass"}, &GraphCanvasNode::op);
    REQUIRE(pass != inside.end());
    CHECK(pass->id == "gbuffer");
    const auto zone = model.loop("gbuffer_draws");
    REQUIRE(zone);
    CHECK(zone->draw_items);
    CHECK(zone->contract == "gbuffer_v1");
    CHECK(zone->tier == "draw_loop");
    CHECK(zone->reported_tier == "draw_loop");
    CHECK(zone->branches.size() == 1);
    CHECK(zone->branches[0].when == "else");
    CHECK(model.loopZoneBody("gbuffer_draws") == "gbuffer_draw");
    const auto body = model.nodes("gbuffer_draw");
    REQUIRE(body.size() == 1);
    CHECK(body[0].op == "draw.material");

    // Decision 4: a drag says nothing; an edit sends nothing and says so.
    CHECK_FALSE(model.setLayout({}, Json::object()));
    CHECK_FALSE(model.foldedEditRefused());
    CHECK_FALSE(model.removeNode("gbuffer"));
    CHECK(model.foldedEditRefused());
    CHECK_FALSE(asked(model, "edit_graph_document"));
    const auto expand = model.foldedExpand();
    CHECK(expand.kind == "decompose");
    CHECK(expand.graph == "main_render");
    CHECK(expand.pass == "gbuffer");

    // A document read while inside is the real one's; the view stands.
    model.refresh();
    h.reply(h.take("get_graph_instances"), gl1Document());
    CHECK(model.foldedView());
    CHECK(model.nodes().size() == 2);
    CHECK(model.foldedEditRefused());
    // The breadcrumb comes back out, to the real root.
    REQUIRE(model.leaveScope());
    CHECK_FALSE(model.foldedView());
    CHECK_FALSE(model.foldedEditRefused());
    CHECK(model.nodes().size() == 3);

    // A feature's fold: its passes one by one, and the lines between them.
    REQUIRE(model.enterFolded("feature:bloom"));
    std::vector<std::string> passes;
    for (const auto &node : model.foldedNodes()) passes.push_back(node.id);
    CHECK(passes == std::vector<std::string>{"bloom_down", "bloom_up", "bloom_mix"});
    CHECK(model.derivedEdges() == std::vector<Edge>{{"bloom_down", "bloom_up", {"bloom_0"}},
                                                     {"bloom_up", "bloom_mix", {"bloom_1"}}});
    // No template read yet: no decomposed version, and no link.
    CHECK_FALSE(model.canShowDecomposed());
    CHECK(model.foldedExpand().kind.empty());
    h.reply(templates, gl1Templates());
    REQUIRE(model.canShowDecomposed());
    REQUIRE(model.showDecomposed());
    CHECK(model.displayTrail() ==
          std::vector<std::string>{"root", "bloom", "bloom (decomposed)"});
    const auto decomposed = model.nodes();
    REQUIRE(decomposed.size() == 1);
    CHECK(decomposed[0].loop);
    CHECK(model.loopZoneBody("bloom_steps") == "bloom_step");
    CHECK(model.foldedExpand().kind == "replace");
    CHECK(model.foldedExpand().feature == "engine://features/bloom.json");
    REQUIRE(model.leaveScope());
    CHECK(model.displayTrail() == std::vector<std::string>{"root", "bloom"});
    REQUIRE(model.leaveScope());
    CHECK_FALSE(model.foldedView());
    CHECK(folded(model.foldedNodes(), "feature:bloom").decomposable);
}

// GL-2b (design_graph_layers 3.2, 4, 5.1). A frame plan from an engine that
// publishes origin (GL-2a): a root pass, an instance's pass, a loop's three
// copies (the first on a branch of its own) that take the place of a
// feature's pass, the preset's and two features' passes -- one of those
// named like a root node the feature switched off -- and the physical plan's
// elements naming the plan nodes they concern.
namespace {
Json gl2bDocument() {
    const Json fullscreen{{"op", "render.fullscreen"}, {"parameters", Json::object()},
                          {"outputs", Json::array({"color"})}};
    auto tint = fullscreen;
    tint["id"] = "tint";
    auto sky = fullscreen;
    sky["id"] = "sky";
    auto fill = fullscreen;
    fill["id"] = "fill";
    auto depth = Json{{"id", "depth"}, {"op", "render.shadow_depth"},
                      {"parameters", Json::object()}, {"outputs", Json::array()}};
    const Json loop{{"id", "cascades"},
                    {"op", "graph.loop"},
                    {"parameters",
                     {{"over", {{"range", {{"count", 3}}}}},
                      {"branches", Json::array({{{"when", {{"element", 0}}}, {"body", "near"}},
                                                {{"body", "cascade"}}})}}},
                    {"outputs", Json::array()}};
    return {{"source_reference", "passes/main.json"},
            {"root_schema", "pelican.graph"},
            {"source_digest", {{"hex", "gl2b"}}},
            {"nodes", Json::array({tint, sky,
                                   {{"id", "left"}, {"op", "graph.instance"}, {"definition", "paint"}},
                                   loop})},
            {"definitions",
             {{"paint", {{"nodes", Json::array({fill})}, {"outputs", Json::object()}}},
              {"near", {{"nodes", Json::array({depth})}, {"outputs", Json::object()}}},
              {"cascade", {{"nodes", Json::array({depth})}, {"outputs", Json::object()}}}}}};
}

Json gl2bPass(const std::string &name, std::size_t order, const std::string &source,
              const Json &reads, const Json &writes) {
    return gl1Pass(name, order, source, reads, writes);
}

Json gl2bPlan() {
    const std::string graph = "passes/main.json";
    const std::string shadow = "engine://features/shadow_directional.json";
    auto gbuffer = gl2bPass("gbuffer", 0, "engine", Json::array(), Json::array({"g_albedo"}));
    gbuffer["shader_resolution"] = {{"state", "material_owned"}};
    Json nodes = Json::array({gbuffer});
    for (std::int64_t element = 0; element < 3; ++element) {
        const auto name = "cascades/" + std::to_string(element) + "/depth";
        auto copy = gl2bPass(name, static_cast<std::size_t>(element) + 1, "engine", Json::array(),
                             Json::array({"shadow_map"}));
        copy["loop_origin"] = {{"loop", "cascades"}, {"element", element}, {"node", "depth"}};
        copy["origin"] = {{"graph", graph},        {"node", "depth"},
                          {"definition", element == 0 ? "near" : "cascade"},
                          {"loop", "cascades"}, {"loop_branch", element == 0 ? 0 : 1},
                          {"element", element}};
        copy["replaces"] = Json::array({{{"feature", "shadow_directional"}, {"pass", "shadow_depth"}}});
        nodes.push_back(copy);
    }
    auto resolve = gl2bPass("shadow_resolve", 4, "feature:shadow_directional",
                            Json::array({"shadow_map"}), Json::array({"shadow_mask"}));
    resolve["provider_feature"] = shadow;
    nodes.push_back(resolve);
    nodes.push_back(gl2bPass("lighting", 5, "engine", Json::array({"g_albedo", "shadow_mask"}),
                             Json::array({"scene_color"})));
    auto tint = gl2bPass("tint", 6, "engine", Json::array({"scene_color"}), Json::array({"tint_image"}));
    tint["origin"] = {{"graph", graph}, {"node", "tint"}};
    nodes.push_back(tint);
    auto fill = gl2bPass("left/fill", 7, "engine", Json::array(), Json::array({"canvas"}));
    fill["origin"] = {{"graph", graph}, {"node", "fill"}, {"definition", "paint"},
                      {"instance_path", "left"}};
    nodes.push_back(fill);
    // The feature's pass named like the root node it switched off: no origin.
    auto sky = gl2bPass("sky", 8, "feature:sky", Json::array(), Json::array({"sky_image"}));
    sky["provider_feature"] = "engine://features/sky.json";
    nodes.push_back(sky);
    nodes.push_back(gl2bPass("present", 9, "project", Json::array({"tint_image"}),
                             Json::array({"display"})));
    const Json physical{
        {"resources",
         Json::array({{{"logical_resource", "shadow_map"}, {"format", "D32_SFLOAT"},
                       {"from_nodes", Json::array({"cascades/0/depth", "cascades/1/depth",
                                                   "cascades/2/depth", "shadow_resolve"})}},
                      {{"logical_resource", "tint_image"}, {"format", "R8G8B8A8_UNORM"},
                       {"from_nodes", Json::array({"tint", "present"})}},
                      {{"logical_resource", "canvas"}, {"format", "R8G8B8A8_UNORM"},
                       {"from_nodes", Json::array({"left/fill"})}},
                      {{"logical_resource", "scene_color"}, {"format", "R16G16B16A16_SFLOAT"},
                       {"from_nodes", Json::array({"lighting", "tint"})}}})},
        {"alias_groups", Json::array({{{"id", "alias_0"},
                                       {"resources", Json::array({"tint_image", "canvas"})},
                                       {"from_nodes", Json::array({"tint", "left/fill", "present"})}}})},
        {"scopes", Json::array({{{"id", "scope_0"}, {"kind", "render_pass"},
                                 {"nodes", Json::array({"tint"})}}})},
        {"decisions",
         Json::array({{{"id", "pelican.plan.serial"}, {"subject", "tint"}, {"selected", "serial"},
                       {"detail", ""}, {"from_nodes", Json::array({"tint"})}},
                      {{"id", "pelican.plan.profile"}, {"subject", "main_render"},
                       {"selected", "default"}, {"detail", ""}, {"from_nodes", Json::array()}}})},
        {"backend_selection",
         {{"selected_candidate", "vulkan_raster"},
          {"decisions", Json::array()},
          {"candidates", Json::array({{{"candidate", "vulkan_raster"}, {"endpoint", "vulkan"},
                                       {"feasible", true}, {"from_nodes", Json::array()}}})}}}};
    const Json execution{
        {"nodes", Json::array({{{"name", "lighting"}, {"selected_endpoint", "vulkan"},
                                {"resource_uses", Json::array({{{"resource", "scene_color"},
                                                                {"access", "write"}}})}},
                               {{"name", "tint"}, {"selected_endpoint", "vulkan"},
                                {"resource_uses", Json::array({{{"resource", "scene_color"},
                                                                {"access", "read"}}})}}})}};
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"loops", Json::array()},
            {"nodes", nodes},
            {"barriers",
             Json::array({{{"from", "cascades/0/depth"}, {"to", "cascades/1/depth"},
                           {"resource", "shadow_map"}, {"kind", "write_after_write"}},
                          {{"from", "cascades/2/depth"}, {"to", "shadow_resolve"},
                           {"resource", "shadow_map"}, {"kind", "read_after_write"}},
                          {{"from", "lighting"}, {"to", "tint"},
                           {"resource", "scene_color"}, {"kind", "read_after_write"}}})},
            {"physical_target_plan", physical},
            {"execution_plan", execution},
            {"diagnostics",
             Json::array({{{"kind", "pipeline_preset_resolved"},
                           {"subject", "engine://render_pipelines/hybrid_v1.json"},
                           {"detail", "preset"}},
                          {{"kind", "graph_node_deactivated"}, {"subject", "sky"},
                           {"detail", "target_ref"}}})},
            {"deactivated_graph_nodes",
             Json::array({{{"node", "sky"}, {"feature", "sky"}, {"reason", "target_ref"}}})}};
}
} // namespace

TEST_CASE("GL-2b the plan's origin says which graph node made a pass; bands, Layers "
          "and Open in Graph read it",
          "[studio][graph-canvas][gl2b]") {
    using PelicanStudio::GraphCanvasLayerRow;
    using PelicanStudio::GraphCanvasPlanTarget;
    Harness h;
    h.open(gl2bDocument());
    h.reply(h.take("get_frame_plan"), gl2bPlan());
    auto &model = h.model;
    REQUIRE(model.planOriginAware());

    // Decision 1: origin, not the name, says whose a pass is. The feature's
    // `sky` stays folded though a root node is called sky; the instance's and
    // the loop's passes are drawn under their root nodes.
    std::vector<std::string> ids;
    for (const auto &node : model.foldedNodes()) ids.push_back(node.id);
    CHECK(ids == std::vector<std::string>{"gbuffer", "feature:shadow_directional", "lighting",
                                          "feature:sky", "present"});
    CHECK(model.canvasNodeOfPass("sky") == "feature:sky");
    CHECK(model.canvasNodeOfPass("cascades/1/depth") == "cascades");
    CHECK(model.canvasNodeOfPass("left/fill") == "left");
    CHECK(model.canvasNodeOfPass("tint") == "tint");
    CHECK(model.planNodesOf("cascades") ==
          std::vector<std::string>{"cascades/0/depth", "cascades/1/depth", "cascades/2/depth"});

    // Decision 4: where Open in Graph lands for each kind of plan node.
    CHECK(model.planNodeTarget("cascades/1/depth") ==
          GraphCanvasPlanTarget{"loop", "depth", "cascade", "", "cascades", 1, false});
    CHECK(model.planNodeTarget("left/fill") ==
          GraphCanvasPlanTarget{"instance", "fill", "paint", "left", "", 0, false});
    CHECK(model.planNodeTarget("tint") == GraphCanvasPlanTarget{"root", "tint", "", "", "", 0, false});
    CHECK(model.planNodeTarget("lighting") ==
          GraphCanvasPlanTarget{"root", "lighting", "", "", "", 0, true});
    CHECK(model.planNodeTarget("shadow_resolve") ==
          GraphCanvasPlanTarget{"root", "feature:shadow_directional", "", "", "", 0, true});
    CHECK(model.planNodeTarget("nothing").kind.empty());

    // Decision 2: bands only with the filter on one domain (Render).
    CHECK(model.layerBand("cascades").empty());
    for (const auto &domain : GraphCanvasModel::domainNames())
        model.setDomainShown(domain, domain == "render");
    REQUIRE(model.layersShown());
    // The zone's bottom row says how many copies; the band leaves them out.
    CHECK(model.layerBand("cascades") == "2 barriers");
    CHECK(model.layerBand("tint") == "1 barrier \xC2\xB7 aliased");
    CHECK(model.layerBand("left") == "aliased");
    CHECK(model.layerBand("gbuffer").empty());
    CHECK(model.layerBand("sky") == "deactivated: target_ref (sky)");
    REQUIRE(model.deactivatedNode("sky") != nullptr);
    CHECK(model.deactivatedNode("tint") == nullptr);

    // Decision 3: the Layers section of one node, L1 to L3.
    CHECK(model.layerRows("tint") ==
          std::vector<GraphCanvasLayerRow>{
              {"L1", "pass", "tint \xC2\xB7 render", "tint"},
              {"L2", "resource", "resource tint_image \xC2\xB7 R8G8B8A8_UNORM \xC2\xB7 alias alias_0", ""},
              {"L2", "resource", "resource scene_color \xC2\xB7 R16G16B16A16_SFLOAT", ""},
              {"L2", "alias", "alias group alias_0: tint_image, canvas", ""},
              {"L2", "scope", "scope scope_0 (render_pass)", ""},
              {"L2", "barrier",
               "lighting \xE2\x86\x92 tint \xC2\xB7 scene_color \xC2\xB7 read_after_write \xC2\xB7 "
               "write \xE2\x86\x92 read",
               ""},
              {"L3", "decision", "pelican.plan.serial \xC2\xB7 tint \xE2\x86\x92 serial", ""}});
    const auto loop_rows = model.layerRows("cascades");
    REQUIRE(loop_rows.size() >= 2);
    CHECK(loop_rows[0] == GraphCanvasLayerRow{"L1", "pass",
                                              "cascades/0/depth \xC2\xB7 render \xC2\xB7 loop cascades [0]",
                                              "cascades/0/depth"});
    CHECK(loop_rows[1] == GraphCanvasLayerRow{"L1", "replaces",
                                              "cascades/0/depth replaces pass shadow_depth of "
                                              "shadow_directional",
                                              "cascades/0/depth"});
    // replaces ties the loop to the feature it stands in for, on the
    // feature's folded node too -- once, not once a copy.
    const auto feature_rows = model.layerRows("feature:shadow_directional");
    CHECK(std::ranges::count(feature_rows,
                             GraphCanvasLayerRow{"L1", "replaced",
                                                 "pass shadow_depth is replaced by graph node cascades",
                                                 "cascades/0/depth"}) == 1);
    CHECK(model.layerRows("left").front() ==
          GraphCanvasLayerRow{"L1", "pass", "left/fill \xC2\xB7 render \xC2\xB7 instance left",
                              "left/fill"});
    // With nothing picked: what concerns the whole graph.
    CHECK(model.graphLayerRows() ==
          std::vector<GraphCanvasLayerRow>{
              {"L3", "decision", "pelican.plan.profile \xC2\xB7 main_render \xE2\x86\x92 default", ""},
              {"L3", "backend", "candidate vulkan_raster \xC2\xB7 vulkan \xC2\xB7 selected", ""}});
    CHECK(model.planDiagnostics().size() == 2);
    // Two domains: no band, no Layers.
    model.setDomainShown("value", true);
    CHECK(model.layerBand("cascades").empty());
    CHECK(model.layerRows("tint").empty());

    // An engine that predates origin: GL-1's name match, in its one place.
    auto old = gl2bPlan();
    old.erase("diagnostics");
    old.erase("deactivated_graph_nodes");
    for (auto &node : old["nodes"]) {
        node.erase("origin");
        node.erase("replaces");
    }
    model.refresh();
    h.reply(h.take("get_graph_instances"), gl2bDocument());
    h.reply(h.take("get_frame_plan"), old);
    CHECK_FALSE(model.planOriginAware());
    CHECK(model.canvasNodeOfPass("sky") == "sky");
    CHECK(model.planNodeTarget("cascades/2/depth") ==
          GraphCanvasPlanTarget{"loop", "depth", "", "", "cascades", 2, false});
}

// UX-15b, decision 5 (GL-2b's hole): a feature whose passes graph nodes took
// the place of, every one, has no folded node to say "is replaced by" on; the
// graph's own Layers rows (nothing picked) say it instead.
TEST_CASE("UX-15b a feature every pass of which a graph node replaced says so in the graph's "
          "own Layers rows",
          "[studio][graph-canvas][ux15b]") {
    using PelicanStudio::GraphCanvasLayerRow;
    Harness h;
    h.open(gl2bDocument());
    auto plan = gl2bPlan();
    // shadow_resolve is no feature pass any more: no pass of
    // shadow_directional is left, only the loop's copies in its place.
    for (auto &node : plan["nodes"])
        if (node.at("name") == "shadow_resolve") {
            node.erase("provider_feature");
            node["source"] = "engine";
        }
    h.reply(h.take("get_frame_plan"), plan);
    auto &model = h.model;
    for (const auto &domain : GraphCanvasModel::domainNames())
        model.setDomainShown(domain, domain == "render");
    REQUIRE(model.layersShown());
    for (const auto &node : model.foldedNodes()) CHECK(node.id != "feature:shadow_directional");
    const GraphCanvasLayerRow replaced{
        "L1", "replaced",
        "pass shadow_depth of shadow_directional is replaced by graph node cascades",
        "cascades/0/depth"};
    // Once, not once a copy.
    CHECK(std::ranges::count(model.graphLayerRows(), replaced) == 1);

    // With the feature's folded node there, the row stays on that node.
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), gl2bDocument());
    h.reply(h.take("get_frame_plan"), gl2bPlan());
    CHECK(std::ranges::count(model.graphLayerRows(), replaced) == 0);
}

// UX-6b, decision 4 (engine PO-1): an output nothing reads and a binding the
// shader never reads are said on the node that made the pass -- the band's
// words and one sentence each, once for all of a loop's copies.
TEST_CASE("UX-6b unread outputs and unused bindings are said on the graph node that made the pass",
          "[studio][graph-canvas][ux6b]") {
    using PelicanStudio::GraphCanvasPlanNote;
    Harness h;
    h.open(gl2bDocument());
    auto plan = gl2bPlan();
    plan["diagnostics"].push_back(
        {{"kind", "unread_output"}, {"subject", "tint"}, {"detail", {{"resource", "tint_image"}}}});
    for (const char *copy : {"cascades/0/depth", "cascades/1/depth"}) {
        const std::string resource = std::string{copy}.substr(0, 11) + "shadow_scratch";
        plan["diagnostics"].push_back({{"kind", "unused_binding"},
                                       {"subject", copy},
                                       {"detail", {{"binding", 2}, {"resource", resource}}}});
    }
    h.reply(h.take("get_frame_plan"), plan);
    auto &model = h.model;
    for (const auto &domain : GraphCanvasModel::domainNames())
        model.setDomainShown(domain, domain == "render");
    REQUIRE(model.layersShown());
    CHECK(model.planNotes("tint") ==
          std::vector<GraphCanvasPlanNote>{
              {"output unread", "tint_image is written but nothing reads it this frame "
                                "\xE2\x80\x94 the pass may be removable"}});
    CHECK(model.planNotes("cascades") ==
          std::vector<GraphCanvasPlanNote>{
              {"binding 2 unused",
               "binding 2 (shadow_scratch) is declared but the shader never reads it"}});
    CHECK(model.planNotes("left").empty());
    // They stay out of the diagnostics line; the others are still there.
    const auto &diagnostics = model.planDiagnostics();
    CHECK(std::ranges::count_if(diagnostics, GraphCanvasModel::nodeNoteDiagnostic) == 3);
    CHECK(std::ranges::count_if(diagnostics, [](const auto &diagnostic) {
              return !GraphCanvasModel::nodeNoteDiagnostic(diagnostic);
          }) == 2);
    // The GL-2b band is not changed by them; the canvas adds the words.
    CHECK(model.layerBand("tint") == "1 barrier \xC2\xB7 aliased");
    // Two domains: no bands, no notes.
    model.setDomainShown("value", true);
    CHECK(model.planNotes("tint").empty());
}

// --- GL-1f (engine RS-4): a material pass, a shadow_depth and a
// snapshot_copy hand on the images they wrote as output ports of their input
// sockets' names, typed as the image on that socket (decision 1). A line from
// one is a set_input and the writer's `outputs` gaining the port, in one list
// and one Undo (decision 2); taking the last reader away takes the port out.

namespace {

Json gl1fCatalog() {
    auto reply = lp4eCatalog();
    auto &render = reply["modules"][1]["operations"];
    const Json any_image{{"semantic", "pelican.render.color_signal@1"}};
    render.push_back({{"op", "render.shadow_depth"}, {"domain", "rendering"},
                      {"evaluation", "direct"}, {"sink", false}, {"ports", "declared"},
                      {"inputs", Json::array({{{"port", "depth"}, {"required", true},
                                               {"accepted_type", lp4eDepthType()}}})},
                      {"outputs", Json::array({{{"port", "depth"}, {"type_from_input", "depth"}}})}});
    render.push_back({{"op", "render.snapshot_copy"}, {"domain", "rendering"},
                      {"evaluation", "direct"}, {"sink", false}, {"ports", "declared"},
                      {"inputs", Json::array({{{"port", "source"}, {"required", true},
                                               {"accepted_type", any_image}},
                                              {{"port", "destination"}, {"required", true},
                                               {"accepted_type", any_image}}})},
                      {"outputs", Json::array({{{"port", "destination"},
                                                {"type_from_input", "destination"}}})}});
    return reply;
}

// lp4e's forward pass, a shadow_depth on the scene depth, and two copies:
// `snap` into a history image, `copy` with nothing on its source yet.
Json gl1fDocument() {
    auto document = lp4eDocument();
    auto &nodes = document["nodes"];
    nodes.push_back({{"id", "history_ref"}, {"op", "render.target_ref"},
                     {"parameters", {{"name", "history"}, {"type", lp4cColorType()}}},
                     {"outputs", {"resource"}}});
    nodes.push_back({{"id", "shadow"}, {"op", "render.shadow_depth"},
                     {"parameters", Json::object()},
                     {"inputs", {{"depth", {{"node", "scene_depth_ref"}, {"port", "resource"}}}}},
                     {"outputs", Json::array()}});
    nodes.push_back({{"id", "snap"}, {"op", "render.snapshot_copy"},
                     {"parameters", Json::object()},
                     {"inputs", {{"source", {{"node", "scene_color_ref"}, {"port", "resource"}}},
                                 {"destination", {{"node", "history_ref"}, {"port", "resource"}}}}},
                     {"outputs", Json::array()}});
    nodes.push_back({{"id", "copy"}, {"op", "render.snapshot_copy"},
                     {"parameters", Json::object()},
                     {"inputs", {{"destination", {{"node", "history_ref"}, {"port", "resource"}}}}},
                     {"outputs", Json::array()}});
    return document;
}

std::vector<std::string> outputNames(const GraphCanvasNode &node) {
    std::vector<std::string> names;
    for (const auto &each : node.outputs) names.push_back(each.port);
    return names;
}

Json nodeIn(const Json &document, const std::string &id) {
    for (const auto &node : document.at("nodes"))
        if (node.at("id") == id) return node;
    FAIL("no node " << id);
    return {};
}

} // namespace

TEST_CASE("GL-1f a material pass, a shadow_depth and a snapshot_copy hand on what they wrote; "
          "a line from one lists the port in the writer's outputs, and the last one gone takes "
          "it out",
          "[studio][graph-canvas][gl1f]") {
    DrawLoopHarness h;
    h.document = gl1fDocument();
    h.client.catalog_reply = gl1fCatalog();
    h.open();

    // Decision 1: the three kinds, typed as the image on the socket.
    const auto forward = h.client.node("forward_opaque");
    CHECK(outputNames(forward) == std::vector<std::string>{"color", "depth"});
    CHECK(output(forward, "color").type == "pelican.render.color_signal@1");
    CHECK(output(forward, "depth").type == "pelican.render.depth@1");
    const auto shadow = h.client.node("shadow");
    CHECK(outputNames(shadow) == std::vector<std::string>{"depth"});
    CHECK(output(shadow, "depth").type == "pelican.render.depth@1");
    const auto snap = h.client.node("snap");
    CHECK(outputNames(snap) == std::vector<std::string>{"destination"});
    CHECK(output(snap, "destination").type == "pelican.render.color_signal@1");
    CHECK(h.client.model.canConnect({}, "forward_opaque", "color", "copy", "source"));
    // A type the reader does not take is not a line.
    CHECK_FALSE(h.client.model.canConnect({}, "forward_opaque", "depth", "copy", "source"));

    // Decision 2: the line, and the port listed before it, in one list.
    REQUIRE(h.client.model.connectInput({}, "copy", "source", "forward_opaque", "color"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("op") == "replace_node");
    CHECK(h.last_ops[0].at("node").at("id") == "forward_opaque");
    CHECK(h.last_ops[0].at("node").at("outputs") == Json::array({"color"}));
    CHECK(h.last_ops[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "copy"},
                                {"port", "source"},
                                {"source", {{"node", "forward_opaque"}, {"port", "color"}}}});
    CHECK(nodeIn(h.document, "forward_opaque").at("outputs") == Json::array({"color"}));
    // Through the pass: the copy's destination is a history image, typed.
    CHECK(output(h.client.node("copy"), "destination").type == "pelican.render.color_signal@1");

    // One Undo takes both back.
    REQUIRE(h.client.model.canUndo());
    REQUIRE(h.client.model.undo());
    h.commit();
    CHECK(nodeIn(h.document, "forward_opaque").at("outputs") == Json::array());
    CHECK_FALSE(nodeIn(h.document, "copy").at("inputs").contains("source"));

    // A second reader of a port already listed adds nothing to the list.
    REQUIRE(h.client.model.connectInput({}, "copy", "source", "forward_opaque", "color"));
    h.commit();
    REQUIRE(h.client.model.connectInput({}, "snap", "source", "forward_opaque", "color"));
    h.commit();
    REQUIRE(h.last_ops.size() == 1);
    CHECK(h.last_ops[0].at("op") == "set_input");

    // Taking one line away leaves the port while the other reads it ...
    REQUIRE(h.client.model.disconnectInput({}, "snap", "source"));
    h.commit();
    REQUIRE(h.last_ops.size() == 1);
    CHECK(nodeIn(h.document, "forward_opaque").at("outputs") == Json::array({"color"}));
    // ... and the last one takes it out, after the line, in the same list.
    REQUIRE(h.client.model.disconnectInput({}, "copy", "source"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("op") == "set_input");
    CHECK(h.last_ops[1].at("op") == "replace_node");
    CHECK(nodeIn(h.document, "forward_opaque").at("outputs") == Json::array());

    // shadow_depth's depth, the same way.
    REQUIRE(h.client.model.connectInput({}, "forward_opaque", "depth", "shadow", "depth"));
    h.commit();
    CHECK(nodeIn(h.document, "shadow").at("outputs") == Json::array({"depth"}));
}

// UX-5c, decision 2: RS-13's refusal of a <loop>.<output> a pass names reads
// as one sentence per reason; RS-9's names the node, its op and what it lacks.
TEST_CASE("UX-5c a loop output reference and a render node's missing parameters read as sentences",
          "[studio][graph-canvas][ux5c][refusal]") {
    using PelicanStudio::graphLoopRefusalDescription;
    using PelicanStudio::knownRefusalDescription;
    const auto reference = [](const char *reason) {
        return Json{{"code", "graph.loop_output_reference_unknown@1"},
                    {"pass", "present"}, {"reference", "loop.color"}, {"reason", reason}};
    };
    CHECK(graphLoopRefusalDescription(reference("loop_unknown")) ==
          "present reads loop.color, but no loop, target or buffer has that name "
          "(graph.loop_output_reference_unknown@1).");
    CHECK(graphLoopRefusalDescription(reference("not_a_loop")) ==
          "present reads loop.color, but the node before the dot is not a loop "
          "(graph.loop_output_reference_unknown@1).");
    CHECK(graphLoopRefusalDescription(reference("output_unknown")) ==
          "present reads loop.color, but the loop has no output of that name "
          "(graph.loop_output_reference_unknown@1).");
    CHECK(graphLoopRefusalDescription(reference("per_element")) ==
          "present reads loop.color, an output with one value per element: a pass reads a "
          "loop's single output (the last element's) (graph.loop_output_reference_unknown@1).");
    CHECK(graphLoopRefusalDescription(reference("not_a_resource")) ==
          "present reads loop.color, which is a value, not an image or a buffer "
          "(graph.loop_output_reference_unknown@1).");
    CHECK(graphLoopRefusalDescription(reference("loop_inactive")) ==
          "present reads loop.color, but this graph variant leaves that loop out "
          "(graph.loop_output_reference_unknown@1).");
    // A reason Studio has no sentence for reads as the code's own.
    CHECK(graphLoopRefusalDescription(reference("later")) ==
          "present reads loop.color, which is no loop output "
          "(graph.loop_output_reference_unknown@1).");
    // Quoted in the message of the refusal that carried it, as the engine
    // spells a GraphRegistryError -- the line the Graph's pass edit shows.
    CHECK(knownRefusalDescription(
              "render_pipeline_preflight_failed",
              "graph.loop_output_reference_unknown@1 {\"pass\":\"present\",\"reason\":"
              "\"per_element\",\"reference\":\"loop.color[]\"}") ==
          "present reads loop.color[], an output with one value per element: a pass reads a "
          "loop's single output (the last element's) (graph.loop_output_reference_unknown@1).");

    CHECK(graphLoopRefusalDescription({{"code", "graph.render_node_parameters_missing@1"},
                                       {"node", "bright"}, {"op", "render.fullscreen"},
                                       {"missing", {"ports", "shader"}}}) ==
          "The render.fullscreen node bright needs ports, shader "
          "(graph.render_node_parameters_missing@1).");
    CHECK(knownRefusalDescription(
              "invalid_graph_document",
              "graph.render_node_parameters_missing@1 {\"missing\":[\"material_contract\"],"
              "\"node\":\"mp\",\"op\":\"render.material_pass\"}") ==
          "The render.material_pass node mp needs material_contract "
          "(graph.render_node_parameters_missing@1).");
}

// LC-1s, decision 2 (engine LC-1): get_frame_plan's reads_loop_index on the
// passes a node made says whether its shader reads the loop element -- on a
// body node, from its copies; on a root pass, outside any loop (element 0).
TEST_CASE("LC-1s a pass whose shader reads the loop element is known by its body node",
          "[studio][graph-canvas][lc1s]") {
    using PelicanStudio::GraphCanvasLoopElementRead;
    Harness h;
    h.open(gl2bDocument());
    auto plan = gl2bPlan();
    for (auto &node : plan["nodes"])
        if (node.at("name") == "cascades/1/depth" || node.at("name") == "cascades/2/depth" ||
            node.at("name") == "tint")
            node["reads_loop_index"] = true;
    h.reply(h.take("get_frame_plan"), plan);
    auto &model = h.model;
    // Decision 1: a loop's bodies (a branch's too), not an instance's.
    CHECK(model.isLoopBodyScope("cascade"));
    CHECK(model.isLoopBodyScope("near"));
    CHECK_FALSE(model.isLoopBodyScope("paint"));
    CHECK_FALSE(model.isLoopBodyScope(""));
    // The body that runs elements 1 and 2 reads it; branch 0's does not.
    CHECK(model.loopElementRead("cascade", "depth") == GraphCanvasLoopElementRead::in_loop);
    CHECK(model.loopElementRead("near", "depth") == GraphCanvasLoopElementRead::none);
    CHECK(model.loopElementRead("paint", "fill") == GraphCanvasLoopElementRead::none);
    // A root pass that reads it is outside every loop: element 0.
    CHECK(model.loopElementRead("", "tint") == GraphCanvasLoopElementRead::outside_loop);
    CHECK(model.loopElementRead("", "sky") == GraphCanvasLoopElementRead::none);
}

// LC-1s, decision 3: a stage's push-constant range over part of the loop
// element reads as one sentence that says what to write instead.
TEST_CASE("LC-1s shader.loop_push_overlap@1 reads as a sentence",
          "[studio][graph-canvas][lc1s][refusal]") {
    using PelicanStudio::graphLoopRefusalDescription;
    using PelicanStudio::knownRefusalDescription;
    const std::string sentence =
        "project://shaders/fft declares a push constant that overlaps the loop element at "
        "96..116 in the fragment stage \xE2\x80\x94 use PELICAN_LOOP_CUSTOM_PUSH and "
        "PELICAN_LOOP_PUSH_FIELDS (shader.loop_push_overlap@1).";
    // As the render config editor relays it: the fields beside the code.
    CHECK(graphLoopRefusalDescription({{"code", "shader.loop_push_overlap@1"},
                                       {"shader", "project://shaders/fft"},
                                       {"stage", "fragment"},
                                       {"range", {{"offset", 96}, {"size", 20}}}}) == sentence);
    // As the pipeline build throws it, quoted in a broader refusal.
    CHECK(knownRefusalDescription(
              "render_pipeline_preflight_failed",
              "shader.loop_push_overlap@1: {\"shader\":\"project://shaders/fft\",\"stage\":"
              "\"fragment\",\"range\":{\"offset\":96,\"size\":20}}") == sentence);
}

// --- UX-8: a curve onto a shader's value, the curve's keys, the sound nodes.

namespace {

Json ux8Catalog() {
    return Json::parse(Pelican::makeBuiltinGraphModuleRegistry().catalog().dump());
}

Json ux8ColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// A float buffer, a fullscreen pass drawing into an image of its own, and
// what the palette puts beside them.
Json ux8Document() {
    return {{"schema", "pelican.graph"},
            {"version", 1},
            {"definitions", Json::object()},
            {"outputs", Json::object()},
            {"nodes",
             Json::array(
                 {{{"id", "level"}, {"op", "render.buffer"},
                   {"parameters", {{"element", "float"}, {"size", 4}}},
                   {"outputs", Json::array({"resource"})}},
                  {{"id", "shade__color"}, {"op", "render.image"},
                   {"parameters", {{"type", ux8ColorType()}, {"format", "R16G16B16A16_SFLOAT"},
                                   {"extent_scale", 1.0},
                                   {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
                   {"outputs", Json::array({"resource"})}},
                  {{"id", "shade"}, {"op", "render.fullscreen"},
                   {"parameters",
                    {{"shader", {{"vertex", "engine://fullscreen"},
                                 {"fragment", "project://shaders/level"}}},
                     {"ports", {{"color", {{"kind", "image"}, {"type", ux8ColorType()},
                                           {"access", "write"}, {"attachment", "color"},
                                           {"location", 0}}}}}}},
                   {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                   {"outputs", Json::array({"color"})}}})}};
}

// The plan's word on `shade`: its shader declares one storage buffer.
Json ux8Plan(const char *type = "storage_buffer", const char *name = "pelican_resource_level") {
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"nodes", Json::array({{{"name", "shade"},
                                    {"kind", "fullscreen"},
                                    {"pass_inputs",
                                     {{"state", "declared"},
                                      {"fragment", "project://shaders/level"},
                                      {"declarations", Json::array({{{"binding", 0},
                                                                     {"type", type},
                                                                     {"name", name},
                                                                     {"variadic", false}}})},
                                      {"variadic_range", nullptr}}}}})}};
}

struct Ux8Harness : GraphHistoryHarness {
    Ux8Harness() {
        document = ux8Document();
        client.catalog_reply = ux8Catalog();
    }
    void report(const Json &plan = ux8Plan()) { client.reply(client.take("get_frame_plan"), plan); }
    void commitAndReport() {
        commit();
        report();
    }
    // What the engine composes from the document: it has to take it.
    void partitions() const {
        CHECK_NOTHROW(Pelican::partitionGraphDocument(document,
                                                      Pelican::makeBuiltinGraphModuleRegistry()));
    }
    Json &node(const std::string &id) {
        auto &nodes = document.at("nodes");
        const auto found =
            std::ranges::find_if(nodes, [&](const Json &each) { return each.at("id") == id; });
        REQUIRE(found != nodes.end());
        return *found;
    }
    // The document changed under the canvas (a hand edit): read it again.
    void reread() {
        client.model.refresh();
        refresh();
        report();
    }
    bool sentEdit() {
        const auto requests = client.model.takeRpcRequests();
        return std::ranges::any_of(requests, [](const auto &request) {
            return request.method == "edit_graph_document";
        });
    }
};

} // namespace

TEST_CASE("UX-8 decisions 1 and 3: a curve from the palette is a ramp on the scene clock, and its "
          "line lands on a float buffer's value socket",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    h.open();
    h.report();

    // Decision 1: the buffer's first row is a value socket, typed as the
    // curve's output is.
    const auto level = h.client.node("level");
    REQUIRE_FALSE(level.inputs.empty());
    CHECK(level.inputs[0].port == "value");
    CHECK(level.inputs[0].type == "pelican.value.scalar@1");
    CHECK_FALSE(level.inputs[0].connected);

    // Decision 3: the palette's curve is a ramp from 0 to 1, read on the
    // scope's scene clock (a new one), in one list.
    const auto ops = h.client.model.addNodeOpsIn({}, "value.curve");
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("node").at("op") == "value.input");
    CHECK(ops[0].at("node").at("parameters") == Json{{"name", "scene_time"}});
    const auto &curve = ops[1].at("node");
    CHECK(curve.at("parameters").at("keys") ==
          Json::array({{{"time", 0.0}, {"value", 0.0}}, {{"time", 1.0}, {"value", 1.0}}}));
    CHECK(curve.at("inputs").at("time") ==
          Json{{"node", ops[0].at("node").at("id")}, {"port", "value"}});
    REQUIRE(h.client.model.addNodeIn({}, "value.curve"));
    h.commitAndReport();
    h.partitions();
    // A second curve reads the clock that is there.
    CHECK(h.client.model.addNodeOpsIn({}, "value.curve").size() == 1);
    // Nothing to ask for: the palette places it as it is.
    CHECK(h.client.model.requiredParametersFor("value.curve").empty());
    CHECK(h.client.model.requiredParametersFor("render.buffer").empty());

    CHECK(h.client.model.canConnect({}, "value_curve", "value", "level", "value"));
    REQUIRE(h.client.model.connectInput({}, "level", "value", "value_curve", "value"));
    h.commitAndReport();
    CHECK(h.last_ops == Json::array({{{"op", "set_input"}, {"scope", "root"}, {"node", "level"},
                                      {"port", "value"},
                                      {"source", {{"node", "value_curve"}, {"port", "value"}}}}}));
    h.partitions();
    CHECK(h.client.node("level").inputs[0].connected);

    // A buffer of another size is made one float in the same list; one of
    // another element is refused by name.
    h.node("level")["parameters"]["size"] = 16;
    h.node("level").erase("inputs");
    h.reread();
    REQUIRE(h.client.model.connectInput({}, "level", "value", "value_curve", "value"));
    h.commitAndReport();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("parameters") == Json{{"element", "float"}, {"size", 4}});
    h.partitions();
    h.node("level")["parameters"] = {{"element", "vec4"}, {"size", 16}};
    h.node("level").erase("inputs");
    h.reread();
    CHECK_FALSE(h.client.model.connectInput({}, "level", "value", "value_curve", "value"));
    CHECK_FALSE(h.sentEdit());
    CHECK(h.client.model.notice().message ==
          "level holds vec4: a value's line writes one float. For two to four numbers, write "
          "them with a render.scalar.");
}

TEST_CASE("UX-8 decision 2: a buffer binding of the shader is a named buffer port, added with its "
          "line and its element in one list; taking the line away takes the port",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    h.open();
    h.report();
    const auto shade = h.client.node("shade");
    REQUIRE(shade.declared_sockets);
    REQUIRE(shade.input_sockets.size() == 1);
    CHECK(shade.input_sockets[0].buffer);
    CHECK(shade.input_sockets[0].key() == "in@0");
    CHECK_FALSE(shade.input_sockets[0].connected());

    // Something that hands on no buffer is refused in one line.
    CHECK_FALSE(h.client.model.connectSocket({}, "shade", "in@0", "shade__color", "resource"));
    CHECK_FALSE(h.sentEdit());
    CHECK(h.client.model.notice().message ==
          "shade__color hands on no buffer: a buffer port reads a render.buffer or a "
          "render.attribute.");

    const auto before = h.document;
    REQUIRE(h.client.model.connectSocket({}, "shade", "in@0", "level", "resource"));
    h.commitAndReport();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("op") == "set_node_parameters");
    CHECK(h.last_ops[0].at("parameters").at("ports").at("level") ==
          Json{{"kind", "buffer"}, {"access", "read"}, {"element", "float"}});
    CHECK(h.last_ops[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "shade"},
                                {"port", "level"},
                                {"source", {{"node", "level"}, {"port", "resource"}}}});
    h.partitions();
    // The socket shows it; it is no row of its own.
    const auto connected = h.client.node("shade");
    CHECK(connected.input_sockets[0].port == "level");
    CHECK(connected.input_sockets[0].source_node == "level");
    CHECK(std::ranges::none_of(connected.inputs, [](const auto &row) { return row.port == "level"; }));
    CHECK(h.client.model.bufferPorts({}, "shade") == std::vector<std::string>{"level"});

    // + Buffer port, for a port the reflection cannot show: by name.
    REQUIRE(h.client.model.addBufferPort({}, "shade", "gain", "level", "resource"));
    h.commitAndReport();
    CHECK(h.node("shade").at("parameters").at("ports").at("gain").at("element") == "float");
    CHECK(h.node("shade").at("inputs").at("gain") == Json{{"node", "level"}, {"port", "resource"}});

    // The line taken away takes the port: one without a line is not bound.
    REQUIRE(h.client.model.disconnectInput({}, "shade", "gain"));
    h.commitAndReport();
    REQUIRE(h.client.model.disconnectInput({}, "shade", "level"));
    h.commitAndReport();
    CHECK(h.document.at("nodes") == before.at("nodes"));
    // A block of another name is no port's: the engine binds a port to
    // pelican_resource_<port> only.
    h.client.model.refresh();
    h.refresh();
    h.report(ux8Plan("storage_buffer", "Level"));
    CHECK_FALSE(h.client.model.connectSocket({}, "shade", "in@0", "level", "resource"));
    CHECK(h.client.model.notice().message ==
          "Binding 0 of shade is Level: the engine reads a buffer port through a variable named "
          "pelican_resource_<port>. Name the block's variable so, or read it through "
          "pelican_resource_ports.glsl and + Buffer port.");
    // A uniform buffer is none a pass hands over.
    h.client.model.refresh();
    h.refresh();
    h.report(ux8Plan("uniform_buffer"));
    CHECK_FALSE(h.client.model.connectSocket({}, "shade", "in@0", "level", "resource"));
    CHECK(h.client.model.notice().message ==
          "Binding 0 of shade is a uniform_buffer: a pass hands its shader storage buffers "
          "(declare it as a `readonly buffer` block).");
}

TEST_CASE("UX-8 decision 4: a curve's keys are edited a row or a drag at a time, held in order, "
          "and each change is one Undo step",
          "[studio][graph-canvas][ux8]") {
    using Key = PelicanStudio::GraphCanvasCurveKey;
    CHECK(GraphCanvasModel::curveInterpolations() ==
          std::vector<std::string>{"linear", "step", "cubic"});
    const std::vector<Key> keys{{0.0, 0.0}, {1.0, 1.0}};
    // A drag past a neighbour stops just short of it; the value goes along.
    const auto moved = GraphCanvasModel::curveKeysWithMoved(keys, 0, 5.0, 0.25);
    CHECK(moved[0].time < 1.0);
    CHECK(moved[0].time > 0.99);
    CHECK(moved[0].value == 0.25);
    // A key added halfway takes the curve's value there.
    const auto added = GraphCanvasModel::curveKeysWithAdded(keys, 0);
    REQUIRE(added.size() == 3);
    CHECK(added[1].time == 0.5);
    CHECK(added[1].value == 0.5);
    CHECK(GraphCanvasModel::curveKeysWithAdded(keys, 1)[2].time == 2.0);
    CHECK(GraphCanvasModel::curveKeysRefusal({{1.0, 0.0}, {1.0, 1.0}}) ==
          "The keys' times have to rise: key 2 is not later than key 1.");
    CHECK(GraphCanvasModel::curveKeysRefusal({}) == "A curve needs at least one key.");

    Ux8Harness h;
    h.open();
    h.report();
    REQUIRE(h.client.model.addNodeIn({}, "value.curve"));
    h.commitAndReport();
    const auto ramp = h.document;
    REQUIRE(h.client.model.curveKeys({}, "value_curve") == keys);

    // One row: 0.1 at 0, 0.9 at 1, the second one a step.
    REQUIRE(h.client.model.setCurveKeys({}, "value_curve", {{0.0, 0.1}, {1.0, 0.9, "step"}}));
    h.commitAndReport();
    CHECK(h.last_ops.size() == 1);
    CHECK(h.node("value_curve").at("parameters").at("keys") ==
          Json::array({{{"time", 0.0}, {"value", 0.1}},
                       {{"time", 1.0}, {"value", 0.9}, {"interpolation", "step"}}}));
    h.partitions();
    // A cubic key's tangents ride along untouched.
    h.node("value_curve")["parameters"]["keys"][0]["interpolation"] = "cubic";
    h.node("value_curve")["parameters"]["keys"][0]["out_tangent"] = 2.0;
    h.reread();
    auto cubic = h.client.model.curveKeys({}, "value_curve");
    CHECK(cubic[0].rest == Json{{"out_tangent", 2.0}});
    cubic[1].value = 0.5;
    REQUIRE(h.client.model.setCurveKeys({}, "value_curve", cubic));
    h.commitAndReport();
    CHECK(h.node("value_curve").at("parameters").at("keys")[0].at("out_tangent") == 2.0);

    // Times that do not rise are not sent.
    CHECK_FALSE(h.client.model.setCurveKeys({}, "value_curve", {{1.0, 0.0}, {0.5, 1.0}}));
    CHECK_FALSE(h.sentEdit());
    CHECK(h.client.model.notice().message ==
          "The keys' times have to rise: key 2 is not later than key 1.");

    // Undo takes back one change at a time: the cubic change, then the row.
    REQUIRE(h.client.model.undo());
    h.commitAndReport();
    CHECK(h.node("value_curve").at("parameters").at("keys")[1].at("value") == 0.9);
}

TEST_CASE("UX-8 decision 5: a playback's wrap and numbers are written one at a time; a number an "
          "edge drives is not",
          "[studio][graph-canvas][ux8]") {
    CHECK(GraphCanvasModel::playbackWraps() ==
          std::vector<std::string>{"once", "repeat", "ping_pong"});
    Ux8Harness h;
    h.open();
    h.report();
    REQUIRE(h.client.model.addNodeIn({}, "value.playback"));
    h.commitAndReport();
    // It plays on the scene clock it arrived on.
    CHECK(h.node("value_playback").at("inputs").at("elapsed").at("node") == "scene_time");
    CHECK(h.client.model.playbackNumber({}, "value_playback", "duration") == 1.0);
    REQUIRE(h.client.model.setPlaybackField({}, "value_playback", "wrap", "repeat"));
    h.commitAndReport();
    REQUIRE(h.client.model.setPlaybackField({}, "value_playback", "duration", 2.5));
    h.commitAndReport();
    CHECK(h.node("value_playback").at("parameters") ==
          Json{{"wrap", "repeat"}, {"duration", 2.5}});
    h.partitions();
    CHECK_FALSE(h.client.model.setPlaybackField({}, "value_playback", "wrap", "bounce"));
    CHECK_FALSE(h.client.model.setPlaybackField({}, "value_playback", "duration", 0.0));
    CHECK(h.client.model.notice().message == "A playback's duration has to be above 0.");
    // speed on an edge: its field is the edge's.
    REQUIRE(h.client.model.connectInput({}, "value_playback", "speed", "scene_time", "value"));
    h.commitAndReport();
    CHECK(h.client.model.playbackDriven({}, "value_playback", "speed"));
    CHECK_FALSE(h.client.model.setPlaybackField({}, "value_playback", "speed", 2.0));
    CHECK_FALSE(h.sentEdit());
}

TEST_CASE("UX-8 decision 6: the Timeline reads the root's nodes without an instance while Live is "
          "on, and says why when it cannot",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    h.open();
    h.report();
    h.client.model.setTimelineNode("level");
    CHECK(h.client.model.timelineReason() ==
          "Turn Live on to read the values while the engine runs.");
    // Live on, no instance named: the root's question is the empty one.
    h.client.model.setLive(true);
    const auto requests = h.client.model.takeRpcRequests();
    const auto values = std::ranges::find(requests, std::string{"get_graph_node_values"},
                                          &PelicanStudio::InspectorRpcRequest::method);
    REQUIRE(values != requests.end());
    CHECK(values->params == Json::object());
    CHECK(std::ranges::count(requests, std::string{"get_status"},
                             &PelicanStudio::InspectorRpcRequest::method) == 1);
    CHECK(h.client.model.timelineReason().empty());
    // Off, nothing is read on the clock.
    h.client.model.setLive(false);
    h.client.model.poll();
    CHECK(std::ranges::none_of(h.client.model.takeRpcRequests(), [](const auto &request) {
        return request.method == "get_graph_node_values";
    }));
}

TEST_CASE("UX-8 decisions 7 and 13: a sound sink is placed driving the voice the dialog picked, at "
          "full volume; the dialog asks what the catalog lists",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    h.document["nodes"].push_back({{"id", "voice"}, {"op", "audio.playback"},
                                   {"parameters", {{"asset", "project://assets/audio/pulse.wav"}}},
                                   {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}},
                                   {"outputs", Json::array({"voice"})}});
    h.document["nodes"].push_back({{"id", "clock"}, {"op", "value.input"},
                                   {"parameters", {{"name", "scene_time"}}},
                                   {"outputs", Json::array({"value"})}});
    // CT-1's catalog: what each op cannot be placed without.
    auto catalog = ux8Catalog();
    for (auto &module : catalog.at("modules"))
        for (auto &operation : module.at("operations")) {
            const auto op = operation.at("op").get<std::string>();
            if (op == "audio.volume" || op == "audio.pitch")
                operation["required_parameters"] =
                    Json::array({{{"one_of", {"target", "voice"}}}});
            if (op == "value.curve")
                operation["required_parameters"] = Json::array({{{"name", "keys"}}});
            if (op == "render.scalar")
                operation["required_parameters"] = Json::array({{{"name", "target"}}});
            if (op == "value.input") {
                operation["required_parameters"] = Json::array({{{"name", "name"}}});
                operation["input_names"] =
                    Json::array({{{"name", "scene_time"}}, {{"name", "time"}}});
            }
        }
    h.client.catalog_reply = catalog;
    h.open();
    h.report();
    const auto kinds = [&](const char *op) {
        std::vector<std::string> result;
        for (const auto &each : h.client.model.requiredParametersFor(op))
            result.push_back(each.name + ":" + each.kind);
        return result;
    };
    // UX-12a, decision 2: the table's entry -- a target, or the voice edge.
    CHECK(kinds("audio.volume") == std::vector<std::string>{"target:string"});
    CHECK(h.client.model.requiredParametersFor("audio.volume").front().edge_alternatives ==
          std::vector<std::string>{"voice"});
    CHECK(kinds("value.curve").empty());
    CHECK(kinds("render.scalar") == std::vector<std::string>{"target:buffer"});
    CHECK(kinds("value.input") == std::vector<std::string>{"name:input"});
    CHECK(h.client.model.valueInputNamesFor() == std::vector<std::string>{"scene_time", "time"});
    CHECK(h.client.model.voiceChoices({}) == std::vector<std::string>{"voice"});

    REQUIRE(h.client.model.addNodeIn(
        {}, "audio.volume", {},
        Json{{"parameters", Json::object()},
             {"inputs", {{"voice", {{"node", "voice"}, {"port", "voice"}}}}}}));
    h.commitAndReport();
    const auto volume = h.node("audio_volume");
    CHECK(volume.at("inputs").at("voice") == Json{{"node", "voice"}, {"port", "voice"}});
    CHECK(h.node(volume.at("inputs").at("value").at("node").get<std::string>())
              .at("parameters")
              .at("value") == 1.0);
    h.partitions();
    // A curve fades it: its line onto the volume's value.
    REQUIRE(h.client.model.addNodeIn({}, "value.curve"));
    h.commitAndReport();
    REQUIRE(h.client.model.connectInput({}, "audio_volume", "value", "value_curve", "value"));
    h.commitAndReport();
    h.partitions();
}

TEST_CASE("UX-8 decision 10: New definition adds an empty definition by a free name",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    h.open();
    h.report();
    const auto name = h.client.model.suggestDefinitionName("definition");
    CHECK(name == "definition");
    REQUIRE(h.client.model.addDefinition(name));
    h.commitAndReport();
    CHECK(h.document.at("definitions").at("definition") ==
          Json{{"nodes", Json::array()}, {"outputs", Json::object()}});
    CHECK(h.client.model.suggestDefinitionName("definition") == "definition_2");
    CHECK_FALSE(h.client.model.addDefinition("definition"));
    CHECK(h.client.model.notice().message == "A definition named definition already exists.");
    CHECK_FALSE(h.client.model.addDefinition("a/b"));
}

TEST_CASE("UX-8 decision 12: render.scalar's numbers are packed with the other sinks on the "
          "buffer, and the engine's layout and port refusals read as sentences",
          "[studio][graph-canvas][ux8]") {
    Ux8Harness h;
    // CT-5: what a render.scalar writes is a runtime_f32 buffer as large as
    // its sinks (a's vec4 and b's vec2 once a grows: 24 bytes).
    h.document["nodes"].push_back({{"id", "params"}, {"op", "render.buffer"},
                                   {"parameters", {{"element", "vec4"}, {"size", 24},
                                                   {"host_source", "runtime_f32"}}},
                                   {"outputs", Json::array({"resource"})}});
    for (const char *id : {"a", "b"})
        h.document["nodes"].push_back(
            {{"id", id}, {"op", "render.scalar"},
             {"parameters", {{"target", "params"}, {"value", {0.0, 0.0}}}},
             {"outputs", Json::array()}});
    h.node("b")["parameters"]["offset"] = 8;
    h.open();
    h.report();
    CHECK(h.client.model.scalarValue({}, "a") == std::vector<double>{0.0, 0.0});
    // a grows to a vec4: b moves on to byte 16, in the same list.
    REQUIRE(h.client.model.setScalarValue({}, "a", {1.0, 2.0, 3.0, 4.0}));
    h.commitAndReport();
    CHECK(h.last_ops.size() == 2);
    CHECK(h.node("a").at("parameters") ==
          Json{{"target", "params"}, {"value", {1.0, 2.0, 3.0, 4.0}}});
    CHECK(h.node("b").at("parameters").at("offset") == 16);
    CHECK(h.client.model.scalarLayout({}, "params") ==
          std::vector<std::pair<std::string, std::int64_t>>{{"a", 0}, {"b", 16}});
    h.partitions();
    // a moves to level: b goes back to 0.
    REQUIRE(h.client.model.setScalarTarget({}, "a", "level"));
    h.commitAndReport();
    CHECK(h.node("a").at("parameters").at("target") == "level");
    CHECK_FALSE(h.node("b").at("parameters").contains("offset"));
    const auto targets = h.client.model.scalarTargets({});
    CHECK(std::ranges::find(targets, "params") != targets.end());

    using PelicanStudio::graphLoopRefusalDescription;
    CHECK(graphLoopRefusalDescription({{"code", "graph.render_scalar_layout_invalid@1"},
                                       {"target", "params"}, {"node", "b"}, {"offset", 4},
                                       {"expected_offset", 8}, {"reason", "overlap"}}) ==
          "The render.scalar b writes params from byte 4, over the sink before it (which ends "
          "at byte 8) (graph.render_scalar_layout_invalid@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.render_scalar_layout_invalid@1"},
                                       {"target", "params"}, {"node", "b"}, {"offset", 12},
                                       {"expected_offset", 8}, {"reason", "gap"}}) ==
          "The render.scalar b writes params from byte 12, leaving bytes 8 up to 12 unwritten "
          "(graph.render_scalar_layout_invalid@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.render_scalar_type_mismatch@1"},
                                       {"node", "a"}, {"target", "counts"}, {"element", "uint"},
                                       {"components", 1}}) ==
          "The render.scalar a writes 1 float(s) into counts, whose elements are uint "
          "(graph.render_scalar_type_mismatch@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.port_field_missing@1"}, {"node", "shade"},
                                       {"port", "level"}, {"field", "element"}}) ==
          "Port level of shade needs element (graph.port_field_missing@1).");
    // Decision 13 (c): the line the engine stopped on, read as a sentence.
    CHECK(PelicanStudio::engineStoppedSentence(
              "12:00:00 [1] pelican_core.cpp:166 LOG_ERROR pelican Pelican fatal error : "
              "invalid graph document C:/project/passes/main.json: graph.port_field_missing@1 "
              "{\"field\":\"element\",\"node\":\"shade\",\"port\":\"level\"}") ==
          "The engine stopped: Port level of shade needs element (graph.port_field_missing@1) "
          "in C:/project/passes/main.json. Fix the file (or Reload from disk) to start it again.");
    CHECK(PelicanStudio::engineStoppedSentence("Pelican fatal error : out of memory").empty());
    // UX-9, decision 5: the line the running engine writes (CT-1) names no
    // file; the render document the project names stands in.
    CHECK(PelicanStudio::engineStoppedSentence(
              "13:59:17.707507130 [76388] pelican_core.cpp:166 LOG_ERROR pelican Pelican fatal "
              "error : graph.port_field_missing@1 {\"field\":\"element\",\"node\":\"sim\",\"port\":\"data\"}",
              "passes/main.json") ==
          "The engine stopped: Port data of sim needs element (graph.port_field_missing@1) in "
          "passes/main.json. Fix the file (or Reload from disk) to start it again.");
}

// --- UX-13b (engine CT-5): a render.scalar's buffer made one it can write.

TEST_CASE("UX-13b decision 2: a render.scalar's target offers the buffers it can write once the same "
          "edit makes them runtime_f32 and as large as their sinks, and each edit of the sinks keeps "
          "them so",
          "[studio][graph-canvas][ux13b]") {
    Ux8Harness h;
    // level (ux8Document's): a float buffer nothing writes from the host;
    // counts: uint elements, which no edit of it makes one; params: one
    // already.
    h.document["nodes"].push_back({{"id", "counts"}, {"op", "render.buffer"},
                                   {"parameters", {{"element", "uint"}, {"size", 4}}},
                                   {"outputs", Json::array({"resource"})}});
    h.document["nodes"].push_back({{"id", "params"}, {"op", "render.buffer"},
                                   {"parameters", {{"element", "float"}, {"size", 4},
                                                   {"host_source", "runtime_f32"}}},
                                   {"outputs", Json::array({"resource"})}});
    h.open();
    h.report();
    using PelicanStudio::GraphCanvasScalarTarget;
    CHECK(h.client.model.scalarTargetChoices({}) ==
          std::vector<GraphCanvasScalarTarget>{{"level", "runtime_f32"}, {"params", ""}});
    CHECK(GraphCanvasModel::scalarTargetLabel({"level", "runtime_f32"}) == "level (will be set to runtime_f32)");
    CHECK(GraphCanvasModel::scalarTargetLabel({"params", ""}) == "params");
    // Placed on level: the buffer is made one it can write, in the same list.
    const Json seed{{"parameters", {{"target", "level"}}}};
    const auto ops = h.client.model.addNodeOpsIn({}, "render.scalar", {}, seed);
    REQUIRE(ops.size() >= 2);
    CHECK(ops[ops.size() - 2].at("node").at("op") == "render.scalar");
    CHECK(ops.back() == Json{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "level"},
                             {"parameters", {{"element", "float"}, {"size", 4}, {"host_source", "runtime_f32"}}}});
    REQUIRE(h.client.model.addNodeIn({}, "render.scalar", {}, seed));
    h.commitAndReport();
    h.partitions();
    // It writes a vec3: level grows to 12 bytes in the same edit.
    REQUIRE(h.client.model.setScalarValue({}, "render_scalar", {1.0, 2.0, 3.0}));
    h.commitAndReport();
    CHECK(h.node("level").at("parameters").at("size") == 12);
    h.partitions();
    // A second sink on it joins after the first; level takes both.
    CHECK(h.client.model.scalarTargetChoices({}) ==
          std::vector<GraphCanvasScalarTarget>{{"level", "16 bytes"}, {"params", ""}});
    REQUIRE(h.client.model.addNodeIn({}, "render.scalar", {}, seed));
    h.commitAndReport();
    CHECK(h.node("render_scalar_2").at("parameters").at("offset") == 12);
    CHECK(h.node("level").at("parameters").at("size") == 16);
    h.partitions();
    // Moved to params: level is left as large as the sink still on it.
    REQUIRE(h.client.model.setScalarTarget({}, "render_scalar_2", "params"));
    h.commitAndReport();
    CHECK(h.node("render_scalar_2").at("parameters").at("target") == "params");
    CHECK(h.node("params").at("parameters").at("size") == 4);
    CHECK(h.node("level").at("parameters").at("size") == 12);
    h.partitions();
}

TEST_CASE("UX-13b decision 3: graph.render_scalar_target_incompatible@1 reads as one sentence with its "
          "Fix; a graph's buffer is fixed in one edit, one declared outside the graph is only said",
          "[studio][graph-canvas][ux13b]") {
    using PelicanStudio::graphLoopRefusalDescription;
    const Json in_graph{{"code", "graph.render_scalar_target_incompatible@1"},
                        {"node", "render_scalar"},
                        {"target", "level"},
                        {"expected", {{"host_source", "runtime_f32"}, {"size", 16}}},
                        {"actual", {{"host_source", nullptr}, {"size", 4}}},
                        {"document", "project://passes/main.json"}};
    CHECK(graphLoopRefusalDescription(in_graph) ==
          "The render.scalar render_scalar writes level (no host_source, 4 bytes), which must be "
          "runtime_f32, 16 bytes \xE2\x80\x94 Fix: make level runtime_f32, 16 bytes (in passes/main.json, "
          "graph.render_scalar_target_incompatible@1).");
    const Json outside{{"code", "graph.render_scalar_target_incompatible@1"},
                       {"target", "bloom.intensity"},
                       {"expected", {{"host_source", "runtime_f32"}, {"size", 4}}},
                       {"actual", {{"host_source", "runtime_f32"}, {"size", 8}}}};
    CHECK(graphLoopRefusalDescription(outside) ==
          "A render.scalar writes bloom.intensity (runtime_f32, 8 bytes), declared outside the graph, "
          "which must be runtime_f32, 4 bytes \xE2\x80\x94 Fix: make bloom.intensity runtime_f32, 4 bytes "
          "where it is declared (graph.render_scalar_target_incompatible@1).");

    Ux8Harness h;
    h.open();
    h.report();
    const auto refuse = [&](const Json &error) {
        REQUIRE(h.client.model.setNodeParameter({}, "level", "size", 8));
        const auto submit = h.client.take("edit_graph_document");
        h.client.reply(submit, {{"status", "rejected"}, {"committed", false}, {"error", error}});
        h.refresh();
    };
    refuse(in_graph);
    REQUIRE(h.client.model.scalarTargetFix().has_value());
    CHECK(h.client.model.scalarTargetFix()->in_graph);
    CHECK(GraphCanvasModel::scalarTargetFixLabel(*h.client.model.scalarTargetFix()) ==
          "Fix: make level runtime_f32, 16 bytes");
    CHECK(h.client.model.notice().message.find("Fix: make level runtime_f32, 16 bytes") != std::string::npos);
    // The Fix: one edit of the buffer, as the engine asked.
    REQUIRE(h.client.model.fixScalarTarget());
    h.commitAndReport();
    CHECK(h.last_ops == Json::array({{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "level"},
                                      {"parameters", {{"element", "float"}, {"size", 16},
                                                      {"host_source", "runtime_f32"}}}}}));
    CHECK_FALSE(h.client.model.scalarTargetFix().has_value());
    // Declared outside the graph: said, nothing to apply here.
    refuse(outside);
    REQUIRE(h.client.model.scalarTargetFix().has_value());
    CHECK_FALSE(h.client.model.scalarTargetFix()->in_graph);
    CHECK_FALSE(h.client.model.fixScalarTarget());
    CHECK(h.client.model.notice().message.find("where it is declared") != std::string::npos);
}

TEST_CASE("UX-8 decision 9: sounds are copied into assets/audio, _2 beside a name already there",
          "[studio][graph-canvas][ux8]") {
    const auto root = std::filesystem::temp_directory_path() / "pelican_ux8_import";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "outside");
    for (const char *name : {"pulse.wav", "note.txt"}) {
        std::ofstream file{root / "outside" / name, std::ios::binary};
        file << "RIFF";
    }
    const auto project = root / "project";
    const auto first = PelicanStudio::importProjectAudio(project, {root / "outside" / "pulse.wav"});
    CHECK(first.error.empty());
    CHECK(first.references == std::vector<std::string>{"project://assets/audio/pulse.wav"});
    const auto second = PelicanStudio::importProjectAudio(
        project, {root / "outside" / "pulse.wav", root / "outside" / "note.txt"});
    CHECK(second.references == std::vector<std::string>{"project://assets/audio/pulse_2.wav"});
    CHECK(second.error == "note.txt is not a .wav or .ogg file.");
    CHECK(std::filesystem::exists(project / "assets" / "audio" / "pulse_2.wav"));
    CHECK_FALSE(std::filesystem::exists(project / "assets" / "audio" / "note.txt"));
    // What the Sound list then offers.
    const auto listed = PelicanStudio::readProjectAudioAssets(project);
    CHECK(listed.assets == std::vector<std::string>{"project://assets/audio/pulse.wav",
                                                    "project://assets/audio/pulse_2.wav"});
    std::filesystem::remove_all(root);
}

// --- PS-1s (engine PS-1): passes that run once, images that are kept.

namespace {

// A loop over a body whose one pass draws into an image of the body's own.
struct Ps1sLoopHarness : GraphHistoryHarness {
    Ps1sLoopHarness() {
        client.catalog_reply = ux8Catalog();
        auto image = ux8Document().at("nodes").at(1);
        image["id"] = "blur__color";
        auto pass = ux8Document().at("nodes").at(2);
        pass["id"] = "blur";
        pass["inputs"]["color"]["node"] = "blur__color";
        document = {{"schema", "pelican.graph"},
                    {"version", 1},
                    {"outputs", Json::object()},
                    {"nodes", Json::array({{{"id", "blur_loop"},
                                            {"op", "graph.loop"},
                                            {"parameters",
                                             {{"over", {{"range", {{"count", 2}}}}},
                                              {"branches", Json::array({{{"body", "blur_body"}}})}}},
                                            {"outputs", Json::array()}}})},
                    {"definitions",
                     {{"blur_body",
                       {{"nodes", Json::array({image, pass})}, {"outputs", Json::object()}}}}}};
    }
};

} // namespace

// Decisions 1 and 2: Schedule = Once is one edit with the images the pass
// writes made Persistent, said in one line; back to Every frame takes only
// the schedule out. A loop's "Run the body once" is the same over its body.
TEST_CASE("PS-1s Once keeps what the pass writes in the same edit, for a pass and a loop's body",
          "[studio][graph-canvas][ps1s]") {
    SECTION("a pass") {
        Ux8Harness h;
        h.open();
        h.report();
        auto &model = h.client.model;
        CHECK(model.nodeSchedule({}, "shade") == "per_frame");
        CHECK(model.nodeSchedule({}, "shade__color").empty());
        CHECK(model.transientImagesWrittenBy({}, "shade") == std::vector<std::string>{"shade__color"});
        // Every view is a task's.
        CHECK_FALSE(model.setNodeSchedule({}, "shade", "per_view"));
        REQUIRE(model.setNodeSchedule({}, "shade", "once"));
        h.commitAndReport();
        REQUIRE(h.last_ops.size() == 2);
        CHECK(h.last_ops[0].at("node") == "shade");
        CHECK(h.last_ops[0].at("parameters").at("schedule") == "once");
        CHECK(h.last_ops[1].at("node") == "shade__color");
        CHECK(h.last_ops[1].at("parameters").at("lifetime") == "persistent");
        CHECK(model.notice().message ==
              "Saved; shade__color made Persistent: what a once pass writes is kept.");
        CHECK(model.persistentImage({}, "shade__color"));
        CHECK(model.transientImagesWrittenBy({}, "shade").empty());
        h.partitions();
        // Every frame again: the schedule goes, the image stays as it is.
        REQUIRE(model.setNodeSchedule({}, "shade", "per_frame"));
        h.commitAndReport();
        REQUIRE(h.last_ops.size() == 1);
        CHECK_FALSE(h.last_ops[0].at("parameters").contains("schedule"));
        CHECK(model.notice().message == "Graph change saved.");
        CHECK(model.persistentImage({}, "shade__color"));
    }
    SECTION("a loop's body") {
        Ps1sLoopHarness h;
        h.open();
        auto &model = h.client.model;
        CHECK(model.loopBodyOnce("blur_loop") == std::optional<bool>{false});
        REQUIRE(model.setLoopBodyOnce("blur_loop", true));
        h.commit();
        REQUIRE(h.last_ops.size() == 2);
        CHECK(h.last_ops[0].at("scope") == "blur_body");
        CHECK(h.last_ops[0].at("parameters").at("schedule") == "once");
        CHECK(h.last_ops[1].at("node") == "blur__color");
        CHECK(h.last_ops[1].at("parameters").at("lifetime") == "persistent");
        CHECK(model.loopBodyOnce("blur_loop") == std::optional<bool>{true});
        CHECK(model.persistentImage("blur_body", "blur__color"));
        REQUIRE(model.setLoopBodyOnce("blur_loop", false));
        h.commit();
        REQUIRE(h.last_ops.size() == 1);
        CHECK_FALSE(h.last_ops[0].at("parameters").contains("schedule"));
        CHECK(model.loopBodyOnce("blur_loop") == std::optional<bool>{false});
    }
}

// Decisions 3 and 4: a once node's band says the generation it last ran in
// (a body node's: its copies'), Recompute asks rerun_once for it (a body
// node: for its loop; nothing named: every once node), and the plan is read
// again until it says the pass ran after the press.
TEST_CASE("PS-1s a once node says when it last ran, and Recompute asks rerun_once and follows it",
          "[studio][graph-canvas][ps1s]") {
    Harness h;
    auto document = gl2bDocument();
    document["nodes"][0]["parameters"]["schedule"] = "once";
    // Every render node has its pass in the plan (sky is switched off): the
    // plan is not read again for one missing.
    document["nodes"].erase(1);
    document["definitions"]["cascade"]["nodes"][0] = {
        {"id", "depth"}, {"op", "render.fullscreen"},
        {"parameters", {{"schedule", "once"}}}, {"outputs", Json::array({"color"})}};
    h.open(document);
    const auto ran = [](Json plan, const std::string &name, Json generation, Json frame) {
        for (auto &node : plan["nodes"])
            if (node.at("name") == name) {
                node["schedule"] = "once";
                node["last_run_generation"] = generation;
                node["last_run_frame"] = frame;
            }
        return plan;
    };
    auto plan = ran(gl2bPlan(), "tint", nullptr, nullptr);
    plan = ran(plan, "cascades/1/depth", 3, 118);
    plan = ran(plan, "cascades/2/depth", 3, 119);
    // Every pass says what its shader declares: the plan is not read again
    // for a pass that has not said it yet.
    for (auto &node : plan["nodes"])
        node["pass_inputs"] = {{"state", "declared"}, {"declarations", Json::array()}};
    h.reply(h.take("get_frame_plan"), plan);
    auto &model = h.model;
    CHECK(model.onceBand({}, "tint") == "once \xC2\xB7 not run yet");
    CHECK(model.onceBand({}, "left").empty());
    const auto body = model.onceRun("cascade", "depth");
    REQUIRE(body);
    CHECK(body->passes == 2);
    CHECK(body->generation == std::optional<std::uint64_t>{3});
    CHECK(body->frame == std::optional<std::uint64_t>{119});
    CHECK(model.planHasOncePasses());
    plan = ran(plan, "tint", 3, 120);
    model.refresh();
    h.reply(h.take("get_graph_instances"), document);
    h.reply(h.take("get_frame_plan"), plan);
    CHECK(model.onceBand({}, "tint") == "once \xC2\xB7 ran gen 3");
    CHECK(model.onceRun({}, "tint")->frame == std::optional<std::uint64_t>{120});

    REQUIRE(model.rerunOnce({}, "tint"));
    auto rerun = h.take("rerun_once");
    CHECK(rerun.params == Json{{"node", "tint"}});
    h.reply(rerun, {{"scheduled", Json::array({"tint"})}});
    CHECK(model.notice().message == "Recompute: 1 once pass runs again on the next frame.");
    // The plan is read on the clock; one that has not run it yet is read
    // again, one that has ends it.
    REQUIRE(model.pollWanted());
    model.poll();
    h.reply(h.take("get_frame_plan"), plan);
    model.poll();
    h.reply(h.take("get_frame_plan"), ran(plan, "tint", 3, 124));
    CHECK(model.onceRun({}, "tint")->frame == std::optional<std::uint64_t>{124});
    CHECK_FALSE(model.pollWanted());

    // A body node is no node of the plan: its loop is asked for.
    REQUIRE(model.rerunOnce("cascade", "depth"));
    CHECK(h.take("rerun_once").params == Json{{"node", "cascades"}});
    REQUIRE(model.rerunOnce({}, {}));
    CHECK(h.take("rerun_once").params == Json::object());
    // A node the plan does not have: the engine's invalid params, said as a
    // sentence.
    REQUIRE(model.rerunOnce({}, "ghost"));
    model.receiveRpcFailure(
        h.take("rerun_once").request_id,
        "RPC -32602: rerun_once node names no node of the frame plan: ghost");
    CHECK(model.notice().kind == InspectorNoticeKind::Error);
    CHECK(model.notice().message ==
          "ghost is not in the frame plan: there is nothing of it to recompute (node_unknown).");
}

// Decision 5: the engine's two refusals of a once pass, as sentences.
TEST_CASE("PS-1s a once pass's refusals read as sentences", "[studio][graph-canvas][ps1s][refusal]") {
    using PelicanStudio::graphLoopRefusalDescription;
    using PelicanStudio::knownRefusalDescription;
    const std::string writes =
        "h0 runs once but writes h0__color, which is not kept between frames \xE2\x80\x94 make it "
        "Persistent (graph.once_writes_transient@1).";
    CHECK(graphLoopRefusalDescription({{"code", "graph.once_writes_transient@1"},
                                       {"node", "h0"}, {"resource", "h0__color"}}) == writes);
    CHECK(knownRefusalDescription("invalid_graph_document",
                                  "graph.once_writes_transient@1 {\"node\":\"h0\",\"resource\":"
                                  "\"h0__color\"}") == writes);
    CHECK(graphLoopRefusalDescription({{"code", "graph.once_reads_per_frame@1"},
                                       {"node", "h0"}, {"input", "input0"}}) ==
          "h0 runs once but reads input0, which changes every frame \xE2\x80\x94 it would freeze "
          "at the first value (graph.once_reads_per_frame@1).");
}

// --- IA-1s.

namespace {

// The start of a KTX2 file: its identifier, vkFormat (109,
// R32G32B32A32_SFLOAT), typeSize, pixelWidth and pixelHeight -- what
// readImageAssetExtent reads -- and zeros.
void ia1sWriteKtx2Header(const std::filesystem::path &path, std::uint32_t width,
                         std::uint32_t height) {
    std::filesystem::create_directories(path.parent_path());
    std::string bytes{"\xABKTX 20\xBB\r\n\x1A\n", 12};
    const auto word = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    };
    word(109);
    word(4);
    word(width);
    word(height);
    bytes.resize(64, '\0');
    std::ofstream{path, std::ios::binary} << bytes;
}

} // namespace

TEST_CASE("IA-1s decision 2: an image's initial-contents refusals read as sentences, a mismatch "
          "in the words of what differs",
          "[studio][graph-canvas][ia1s][refusal]") {
    using PelicanStudio::graphLoopRefusalDescription;
    using PelicanStudio::knownRefusalDescription;
    CHECK(graphLoopRefusalDescription({{"code", "graph.image_initial_requires_persistent@1"},
                                       {"node", "ltc_1"}}) ==
          "ltc_1 starts from an image but is not kept between frames \xE2\x80\x94 make it "
          "Persistent (graph.image_initial_requires_persistent@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.image_initial_extent_conflict@1"},
                                       {"node", "ltc_1"}}) ==
          "ltc_1 takes its size from its image and from the screen at once \xE2\x80\x94 check From "
          "the image, or give it a fixed size (graph.image_initial_extent_conflict@1).");
    const auto mismatch = [](const char *what, const nlohmann::json &expected,
                             const nlohmann::json &actual) {
        return nlohmann::json{{"node", "ltc_1"},
                              {"asset", "assets/images/ltc_1.ktx2"},
                              {"what", what},
                              {"expected", expected},
                              {"actual", actual}};
    };
    const nlohmann::json lut{{"width", 64}, {"height", 64}, {"format", "R32G32B32A32_SFLOAT"}};
    auto extent = mismatch("extent", {{"width", 256}, {"height", 256}, {"format", "R32G32B32A32_SFLOAT"}}, lut);
    extent["code"] = "graph.image_initial_mismatch@1";
    CHECK(graphLoopRefusalDescription(extent) ==
          "assets/images/ltc_1.ktx2 is 64×64 but ltc_1 is 256×256 \xE2\x80\x94 bake the image to "
          "match (pelican_cli image ktx2) or change the image's size "
          "(graph.image_initial_mismatch@1).");
    // As a broader refusal's message carries it.
    const auto format = mismatch("format", {{"width", 64}, {"height", 64}, {"format", "R16G16B16A16_SFLOAT"}}, lut);
    CHECK(knownRefusalDescription("render_pipeline_preflight_failed",
                                  "graph.image_initial_mismatch@1 " + format.dump()) ==
          "assets/images/ltc_1.ktx2 is R32G32B32A32_SFLOAT but ltc_1 is R16G16B16A16_SFLOAT "
          "\xE2\x80\x94 bake the image to match (pelican_cli image ktx2) or change the image's "
          "format (graph.image_initial_mismatch@1).");
    auto space = mismatch("color_space", {{"width", 4}, {"height", 4}, {"format", "R8G8B8A8_UNORM"}},
                          {{"width", 4}, {"height", 4}, {"format", "R8G8B8A8_SRGB"}});
    space["code"] = "graph.image_initial_mismatch@1";
    CHECK(graphLoopRefusalDescription(space) ==
          "assets/images/ltc_1.ktx2 is sRGB (R8G8B8A8_SRGB) but ltc_1 is linear (R8G8B8A8_UNORM) "
          "\xE2\x80\x94 bake the image to match (pelican_cli image ktx2) or change the image's "
          "format to the other color space (graph.image_initial_mismatch@1).");
}

TEST_CASE("IA-1s decision 4: images are copied into assets/images, a .hdr to be baked into a "
          ".ktx2 beside it, and the .ktx2 and .png are what Initial contents lists",
          "[studio][graph-canvas][ia1s]") {
    const auto root = std::filesystem::temp_directory_path() / "pelican_ia1s_import";
    std::filesystem::remove_all(root);
    ia1sWriteKtx2Header(root / "outside" / "ltc_1.ktx2", 64, 32);
    for (const char *name : {"sky.hdr", "notes.txt"})
        std::ofstream{root / "outside" / name, std::ios::binary} << "#?RADIANCE\n";
    const auto project = root / "project";
    const auto first = PelicanStudio::importProjectImage(project, root / "outside" / "ltc_1.ktx2");
    CHECK(first.error.empty());
    CHECK(first.reference == "assets/images/ltc_1.ktx2");
    CHECK_FALSE(first.convert.has_value());
    const auto again = PelicanStudio::importProjectImage(project, root / "outside" / "ltc_1.ktx2");
    CHECK(again.reference == "assets/images/ltc_1_2.ktx2");
    // A .hdr is copied, and its .ktx2 is the one to pick once it is baked.
    const auto sky = PelicanStudio::importProjectImage(project, root / "outside" / "sky.hdr");
    CHECK(sky.error.empty());
    CHECK(sky.reference == "assets/images/sky.ktx2");
    REQUIRE(sky.convert.has_value());
    CHECK(sky.convert->hdr == project / "assets" / "images" / "sky.hdr");
    CHECK(sky.convert->ktx2 == project / "assets" / "images" / "sky.ktx2");
    CHECK(std::filesystem::exists(sky.convert->hdr));
    const auto text = PelicanStudio::importProjectImage(project, root / "outside" / "notes.txt");
    CHECK(text.error == "notes.txt is not a .ktx2, .png or .hdr file.");
    CHECK(text.reference.empty());
    const auto listed = PelicanStudio::readProjectImageAssets(project);
    CHECK(listed.assets == std::vector<std::string>{"assets/images/ltc_1.ktx2",
                                                    "assets/images/ltc_1_2.ktx2"});
    CHECK(PelicanStudio::readImageAssetExtent(project / "assets" / "images" / "ltc_1.ktx2") ==
          PelicanStudio::GraphCanvasImageExtent{64, 32});
    CHECK_FALSE(PelicanStudio::readImageAssetExtent(sky.convert->hdr).has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("IA-1s decision 6: a graph refusal names the document and the definition the engine "
          "says its node was written in (CT-2), Studio's guess only without them",
          "[studio][graph-canvas][ia1s][refusal]") {
    using PelicanStudio::engineStoppedSentence;
    using PelicanStudio::graphLoopRefusalDescription;
    const nlohmann::json root_node{{"code", "graph.port_field_missing@1"},
                                   {"node", "shade"},
                                   {"port", "level"},
                                   {"field", "element"},
                                   {"document", "project://passes/main.json"}};
    CHECK(graphLoopRefusalDescription(root_node) ==
          "Port level of shade needs element (in passes/main.json, "
          "graph.port_field_missing@1).");
    auto body_node = root_node;
    body_node["definition"] = "blur_step";
    CHECK(graphLoopRefusalDescription(body_node) ==
          "Port level of shade needs element (in definition blur_step of "
          "passes/main.json, graph.port_field_missing@1).");
    // A sentence about a definition keeps its own.
    CHECK(graphLoopRefusalDescription({{"code", "graph.definition_referenced@1"},
                                       {"definition", "blur_step"},
                                       {"referenced_by", nlohmann::json::array({"blur"})},
                                       {"document", "project://passes/main.json"}}) ==
          "Definition blur_step is still used by blur (in passes/main.json, "
          "graph.definition_referenced@1).");
    // The engine's fatal line: its document, not the project's guess.
    CHECK(engineStoppedSentence(
              "13:59:17.707507130 [76388] pelican_core.cpp:181 LOG_ERROR pelican Pelican fatal "
              "error : graph.port_field_missing@1 {\"definition\":\"blur_step\",\"document\":"
              "\"project://passes/graph.json\",\"field\":\"element\",\"node\":\"sim\",\"port\":"
              "\"data\"}",
              "passes/main.json") ==
          "The engine stopped: Port data of sim needs element (graph.port_field_missing@1) in "
          "definition blur_step of passes/graph.json. Fix the file (or Reload from "
          "disk) to start it again.");
    CHECK(engineStoppedSentence(
              "Pelican fatal error : graph.port_field_missing@1 {\"field\":\"element\",\"node\":"
              "\"sim\",\"port\":\"data\"}",
              "passes/main.json") ==
          "The engine stopped: Port data of sim needs element (graph.port_field_missing@1) in "
          "passes/main.json. Fix the file (or Reload from disk) to start it again.");
}

// --- AB-1s: a sound's analysis baked into a table (engine AB-1).

namespace {

// ux8Document's pass, whose shader the plan says declares a sampled image at
// binding 0 and the layout's buffer at binding 1.
Json ab1sPlan(bool image = true) {
    auto declarations = Json::array();
    if (image)
        declarations.push_back(
            {{"binding", 0}, {"type", "sampled_image"}, {"name", "tone_table"}, {"variadic", false}});
    declarations.push_back({{"binding", image ? 1 : 0},
                            {"type", "storage_buffer"},
                            {"name", "pelican_resource_tone_layout"},
                            {"variadic", false}});
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"nodes", Json::array({{{"name", "shade"},
                                    {"kind", "fullscreen"},
                                    {"pass_inputs",
                                     {{"state", "declared"},
                                      {"fragment", "project://shaders/level"},
                                      {"declarations", declarations},
                                      {"variadic_range", nullptr}}}}})}};
}

// A bake's manifest as the engine writes it (audioanalysisformat.cpp), and
// one of another tool's beside it.
std::filesystem::path ab1sProject() {
    const auto root = std::filesystem::temp_directory_path() / "pelican_ab1s_tables";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets" / "analysis");
    std::filesystem::create_directories(root / "assets" / "audio");
    std::ofstream{root / "assets" / "analysis" / "tone.ktx2.import.json"}
        << Json{{"schema", "pelican.import"},
                {"version", 1},
                {"tool", {{"name", "pelican.audio_analysis"}, {"version", "1"}}},
                {"source",
                 {{"file", "../audio/tone.wav"},
                  {"metadata",
                   {{"sha256", "0"},
                    {"settings", {{"features", {"rms", "bands"}}, {"bands", 2}, {"hop_ms", 10.0},
                                  {"window_ms", 20.0},
                                  {"smoothing", {{"attack_ms", 0.0}, {"release_ms", 0.0}}},
                                  {"channels", "mix"}}},
                    {"settings_sha256", "0"},
                    {"layout", {{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 101},
                                {"hop_seconds", 0.01}, {"features", {"rms", "band:0", "band:1"}}}}}}}},
                {"outputs", Json::array({{{"file", "tone.ktx2"},
                                          {"schema", "pelican.audio_analysis_table"},
                                          {"version", 1},
                                          {"sha256", "0"}}})}}
               .dump(2);
    std::ofstream{root / "assets" / "analysis" / "other.png.import.json"}
        << Json{{"schema", "pelican.import"},
                {"version", 1},
                {"tool", {{"name", "someone.else"}, {"version", "1"}}},
                {"source", {{"file", "x.psd"}}},
                {"outputs", Json::array({{{"file", "other.png"}}})}}
               .dump();
    return root;
}

} // namespace

TEST_CASE("AB-1s decision 3: Use in a shader places the table's image, its layout buffer and the "
          "two sinks that fill it in one edit, on the pass's sampled image and its "
          "pelican_resource_ buffer",
          "[studio][graph-canvas][ab1s]") {
    Ux8Harness h;
    h.open();
    h.report(ab1sPlan());
    GraphCanvasModel::SampleTableShaderPlacement placed;
    REQUIRE(h.client.model.addSampleTableShader({}, "project://assets/analysis/tone.ktx2", 1, 101, 0.01,
                                                "shade", &placed));
    h.commit();
    CHECK(placed.image == "tone_table");
    CHECK(placed.buffer == "tone_layout");
    CHECK(placed.values == "tone_layout_values");
    CHECK(placed.time == "tone_layout_time");
    CHECK(placed.image_port == "tone_table");
    CHECK(placed.buffer_port == "tone_layout");
    CHECK(placed.note.empty());
    const auto &ops = h.last_ops;
    REQUIRE(ops.size() == 8);
    // The table: persistent, from the file (its size the file's), read only.
    CHECK(ops[0].at("node").at("parameters") ==
          Json{{"type", ux8ColorType()},
               {"format", "R32_SFLOAT"},
               {"usage", Json::array({"SAMPLED"})},
               {"lifetime", "persistent"},
               {"initial", {{"asset", "assets/analysis/tone.ktx2"}}}});
    CHECK(ops[1].at("node").at("parameters") ==
          Json{{"element", "vec4"}, {"size", 16}, {"host_source", "runtime_f32"}});
    CHECK(ops[2].at("node").at("parameters") ==
          Json{{"target", "tone_layout"}, {"value", {1.0, 101.0, 0.01}}, {"offset", 0}});
    // The document had no scene clock: one arrives with them.
    CHECK(ops[3].at("node").at("op") == "value.input");
    CHECK(ops[4].at("node").at("parameters") == Json{{"target", "tone_layout"}, {"offset", 12}});
    CHECK(ops[4].at("node").at("inputs").at("value") == Json{{"node", "scene_time"}, {"port", "value"}});
    const auto ports = h.node("shade").at("parameters").at("ports");
    CHECK(ports.at("tone_table").at("input_index") == 0);
    CHECK(ports.at("tone_table").at("binding") == "input");
    CHECK(ports.at("tone_layout") == Json{{"kind", "buffer"}, {"access", "read"}, {"element", "vec4"}});
    CHECK(h.node("shade").at("inputs").at("tone_table") == Json{{"node", "tone_table"}, {"port", "resource"}});
    CHECK(h.node("shade").at("inputs").at("tone_layout") == Json{{"node", "tone_layout"}, {"port", "resource"}});
    h.partitions();

    // Again, with no pass picked: new names, the same clock, no lines.
    h.report(ab1sPlan());
    REQUIRE(h.client.model.addSampleTableShader({}, "assets/analysis/tone.ktx2", 1, 101, 0.01, {}, &placed));
    h.commit();
    CHECK(placed.image == "tone_table_2");
    CHECK(placed.image_port.empty());
    REQUIRE(h.last_ops.size() == 4);
    CHECK(h.last_ops[3].at("node").at("inputs").at("value").at("node") == "scene_time");

    // A shader with no image binding after the one the pass reads: the
    // buffer's line only, and why.
    h.report(ab1sPlan(false));
    REQUIRE(h.client.model.addSampleTableShader({}, "assets/analysis/tone.ktx2", 1, 101, 0.01, "shade",
                                                &placed));
    h.commit();
    CHECK(placed.image_port.empty());
    CHECK(placed.buffer_port == "tone_layout");
    CHECK(placed.note == "shade's shader declares no image binding 1; connect it by hand.");
    CHECK(h.node("shade").at("inputs").at("tone_layout").at("node") == "tone_layout_3");
}

TEST_CASE("AB-1s decision 4: list_derived_assets reads as the stale band; Re-bake asks for the "
          "manifest's own sound and settings (UX-13a: and edits nothing after it)",
          "[studio][graph-canvas][ab1s]") {
    const auto root = ab1sProject();
    // The tables Studio finds: the bake's, not another tool's.
    const auto tables = PelicanStudio::readProjectAnalysisTables(root);
    REQUIRE(tables.size() == 1);
    CHECK(tables[0].asset == "assets/analysis/tone.ktx2");
    CHECK(tables[0].source == "assets/audio/tone.wav");
    CHECK(tables[0].layout.features == std::vector<std::string>{"rms", "band:0", "band:1"});
    CHECK(PelicanStudio::defaultAnalysisOut("project://assets/audio/tone.wav") == "assets/analysis/tone.ktx2");
    CHECK(PelicanStudio::rebakeParams(root, "project://assets/analysis/tone.ktx2") ==
          Json{{"asset", "project://assets/audio/tone.wav"},
               {"out", "assets/analysis/tone.ktx2"},
               {"settings", tables[0].settings}});
    CHECK(PelicanStudio::rebakeParams(root, "assets/analysis/none.ktx2").is_null());

    const auto derived = PelicanStudio::parseDerivedAssets(Json::array(
        {{{"asset", "assets/analysis/tone.ktx2"},
          {"manifest", "assets/analysis/tone.ktx2.import.json"},
          {"tool", {{"name", "pelican.audio_analysis"}, {"version", "1"}}},
          {"source", "assets/audio/tone.wav"},
          {"stale", true},
          {"stale_reasons", {"source_changed", "settings_changed"}}},
         "not an entry"}));
    REQUIRE(derived.size() == 1);
    CHECK(derived[0].stale);
    CHECK(PelicanStudio::staleBandText(derived[0]) == "stale: source_changed, settings_changed");

    Ux8Harness h;
    auto &nodes = h.document.at("nodes");
    nodes.push_back({{"id", "loudness"}, {"op", "value.sample_table"},
                     {"parameters", {{"asset", "project://assets/analysis/tone.ktx2"}, {"row", "rms"}}},
                     {"outputs", Json::array({"value"})}});
    nodes.push_back({{"id", "other"}, {"op", "value.sample_table"},
                     {"parameters", {{"asset", "project://assets/analysis/kick.ktx2"}, {"row", 0}}},
                     {"outputs", Json::array({"value"})}});
    h.open();
    h.report();
    auto &model = h.client.model;
    CHECK(model.sampleTableAssetOf({}, "loudness") == "assets/analysis/tone.ktx2");
    CHECK(model.sampleTableAssetOf({}, "shade").empty());
    CHECK(model.sampleTableReaders("assets/analysis/tone.ktx2") == std::vector<std::string>{"loudness"});
    std::filesystem::remove_all(root);
}

// --- UX-13a: buffer bindings by the engine's rule, the include form, and no
// edit after a Re-bake (engine IA-3).

namespace {

// `shade`'s shader: `declarations` at set 1, its buffers from `base` on.
Json ux13aPlan(const Json &declarations, std::size_t base) {
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"nodes", Json::array({{{"name", "shade"},
                                    {"kind", "fullscreen"},
                                    {"pass_inputs",
                                     {{"state", "declared"},
                                      {"fragment", "project://shaders/level"},
                                      {"declarations", declarations},
                                      {"variadic_range", nullptr},
                                      {"buffer_binding_base", base}}}}})}};
}

Json ux13aBuffer(std::size_t binding, const char *name) {
    return {{"binding", binding}, {"type", "storage_buffer"}, {"name", name}, {"variadic", false}};
}

} // namespace

TEST_CASE("UX-13a decision 1: a buffer binding is the read the engine binds there -- after the "
          "images, in the order of the buffers the ports read, not of the ports' names; a "
          "pelican_resource_<port> block is its port's wherever it sits",
          "[studio][graph-canvas][ux13a]") {
    Ux8Harness h;
    auto &nodes = h.document.at("nodes");
    // a_buf sorts before level; the ports read them the other way round by
    // their own names (a_second reads level, z_first reads a_buf).
    nodes.push_back({{"id", "a_buf"}, {"op", "render.buffer"},
                     {"parameters", {{"element", "float"}, {"size", 4}}},
                     {"outputs", Json::array({"resource"})}});
    auto &shade = h.node("shade");
    shade["parameters"]["ports"]["z_first"] = {{"kind", "buffer"}, {"access", "read"}, {"element", "float"}};
    shade["parameters"]["ports"]["a_second"] = {{"kind", "buffer"}, {"access", "read"}, {"element", "float"}};
    shade["inputs"]["z_first"] = {{"node", "a_buf"}, {"port", "resource"}};
    shade["inputs"]["a_second"] = {{"node", "level"}, {"port", "resource"}};
    h.open();
    CHECK(h.client.model.bufferPorts({}, "shade") == std::vector<std::string>{"z_first", "a_second"});
    const auto reads = h.client.model.bufferPortReads({}, "shade");
    REQUIRE(reads.size() == 2);
    CHECK(reads[0].resource == "a_buf");
    CHECK(reads[1].resource == "level");

    // One image at 0, so the buffers from 1: binding 1 reads a_buf, 2 level.
    h.report(ux13aPlan(Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", "tint"},
                                     {"variadic", false}},
                                    ux13aBuffer(1, "First"), ux13aBuffer(2, "Second")}),
                       1));
    auto sockets = h.client.node("shade").input_sockets;
    REQUIRE(sockets.size() == 3);
    CHECK(sockets[1].key() == "in@1");
    CHECK(sockets[1].port == "z_first");
    CHECK(sockets[1].source_node == "a_buf");
    CHECK(sockets[2].port == "a_second");
    CHECK(sockets[2].source_node == "level");

    // Named blocks bind by name, wherever they sit; one no port is named
    // for reads nothing yet.
    h.client.model.refresh();
    h.refresh();
    h.report(ux13aPlan(Json::array({ux13aBuffer(0, "pelican_resource_a_second"),
                                    ux13aBuffer(1, "pelican_resource_nobody")}),
                       0));
    sockets = h.client.node("shade").input_sockets;
    REQUIRE(sockets.size() == 2);
    CHECK(sockets[0].port == "a_second");
    CHECK_FALSE(sockets[1].connected());
}

TEST_CASE("UX-13a decision 2: Use in a shader connects the layout by the name the shader reads "
          "through pelican_resource_ports.glsl, which no reflection shows before the port is "
          "there; the line to copy reads it through the include",
          "[studio][graph-canvas][ux13a]") {
    Ux8Harness h;
    h.open();
    // The image is declared; the buffers are read through the include only.
    h.report(ux13aPlan(Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", "tone_table"},
                                     {"variadic", false}}}),
                       1));
    GraphCanvasModel::SampleTableShaderPlacement placed;
    REQUIRE(h.client.model.addSampleTableShader({}, "project://assets/analysis/tone.ktx2", 1, 101, 0.01,
                                                "shade", &placed, {"value_level", "tone_layout"}));
    h.commit();
    CHECK(placed.image_port == "tone_table");
    // Named as the layout's buffer, before the first name the text reads.
    CHECK(placed.buffer_port == "tone_layout");
    CHECK(placed.note.empty());
    const auto ports = h.node("shade").at("parameters").at("ports");
    CHECK(ports.at("tone_layout") == Json{{"kind", "buffer"}, {"access", "read"}, {"element", "vec4"}});
    CHECK_FALSE(ports.contains("value_level"));
    CHECK(h.node("shade").at("inputs").at("tone_layout") == Json{{"node", "tone_layout"}, {"port", "resource"}});
    h.partitions();

    // Nothing named so: the first name no port takes.
    h.report(ux13aPlan(Json::array(), 0));
    REQUIRE(h.client.model.addSampleTableShader({}, "assets/analysis/tone.ktx2", 1, 101, 0.01, "shade",
                                                &placed, {"tone_layout", "gain"}));
    h.commit();
    CHECK(placed.buffer_port == "gain");

    const auto line = PelicanStudio::sampleTableShaderLine("tone_table", "tone_layout", 1);
    CHECK(line.find("#include \"pelican_resource_ports.glsl\"") != std::string::npos);
    CHECK(line.find("pelican_resource_tone_layout.values[0]") != std::string::npos);
    CHECK(line.find("binding") == std::string::npos);
}
