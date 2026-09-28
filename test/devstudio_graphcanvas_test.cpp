#include "graphcanvasmodel.hpp"
#include "graphcomposition.hpp"

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
    // LP-4c: the op list the last commit() sent.
    Json last_ops;
    void commit() {
        const auto submit = client.take("edit_graph_document");
        REQUIRE(submit.params.at("base_source_digest") == digest());
        last_ops = submit.params.at("ops");
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
    // Added in front of the else, running the else's body.
    REQUIRE(h.client.model.addLoopBranch("steps"));
    h.commit();
    CHECK(branches() == Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                     {{"when", {{"element", 0}}}, {"body", "paint"}},
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
    CHECK(forward.outputs.empty());
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
        REQUIRE(h.last_ops.size() == 7);
        const auto &pass = h.last_ops.back().at("node");
        CHECK(pass.at("op") == "render.material_pass");
        CHECK(pass.at("parameters") ==
              Json{{"material_contract", "deferred_geometry_v1"}, {"pass_set", "main_render"}});
        CHECK(pass.at("inputs").size() == 6);
        CHECK(pass.at("inputs").at("worldpos") ==
              Json{{"node", pass.at("id").get<std::string>() + "__worldpos"}, {"port", "resource"}});
        const auto &depth = h.last_ops[5].at("node");
        CHECK(depth.at("parameters").at("format") == "D32_SFLOAT");
        CHECK(depth.at("parameters").at("type") == lp4eDepthType());
        CHECK(portNames(h.client.node(pass.at("id").get<std::string>())).front() == "albedo");
    }
    SECTION("the palette's contract decides the sockets, and changing it is one field") {
        REQUIRE(h.client.model.setMaterialPassContract("gbuffer", "forward_transparent_v1"));
        const auto ops = h.refuse();
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("parameters").at("material_contract") == "forward_transparent_v1");
        CHECK_FALSE(h.client.model.setMaterialPassContract("gbuffer", "legacy_gbuffer_v1"));
    }
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
        const auto parameters = h.loopParameters(h.refuse());
        CHECK(parameters.at("branches") ==
              Json::array({{{"when", {{"material", lp4eMaterial(0)}}}, {"body", "forward_opaque_draw"}},
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
    expected["ports"]["resource"] = q19Port(0);
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
    CHECK(h.node({}, "post").at("parameters").at("ports").at("resource_2") == q19Port(1));
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
    renumbered["ports"]["resource_2"] = q19Port(0);
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
    CHECK(h.client.model.notice().message ==
          "Binding 8 of fill takes a buffer: a graph pass reads only images by number.");

    // Binding 0 is the next: the boundary port target, named by its id and
    // typed by what the loop is fed there (lit), in the body's scope.
    REQUIRE(h.client.model.connectSocket("paint", "fill", "in@0", "target", "value"));
    h.commitAndReport(q19Plan(1));
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0].at("scope") == "paint");
    CHECK(h.last_ops[0].at("parameters").at("ports").at("target") == q19Port(0));
    CHECK(h.last_ops[1] == Json{{"op", "set_input"}, {"scope", "paint"}, {"node", "fill"},
                                {"port", "target"},
                                {"source", {{"node", "target"}, {"port", "value"}}}});
    // Now binding 1 is the next one, and the same source a second time is
    // a port of its own.
    REQUIRE(h.client.model.connectSocket("paint", "fill", "in@1", "target", "value"));
    h.commitAndReport(q19Plan(1));
    CHECK(h.node("paint", "fill").at("parameters").at("ports").at("target_2") == q19Port(1));

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
    CHECK(bloom.tooltip() == "From the rendering section (feature:bloom): read only here. "
                             "Frame Plan and Features edit it.");
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
