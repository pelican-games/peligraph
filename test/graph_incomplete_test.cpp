#include "../src/project/authoringlowering.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphincomplete.hpp"
#include "../src/project/graphpartition.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
using namespace Pelican;

Json document() {
    return Json::parse(R"({
        "schema": "pelican.graph", "version": 1,
        "nodes": [
            {"id": "one", "op": "value.constant", "parameters": {"value": 1.0}, "outputs": ["value"]},
            {"id": "sum", "op": "value.add", "inputs": {"b": {"node": "one", "port": "value"}}, "outputs": ["value"]}
        ],
        "outputs": {"value": {"node": "sum", "port": "value"}}
    })");
}

const Json &node(const Json &body, const std::string &id) {
    for (const auto &entry : body.at("nodes"))
        if (entry.at("id") == id) return entry;
    throw std::runtime_error("missing test node");
}
}

TEST_CASE("[gc2a1] required root and definition scalars receive compile-only zeroes", "[gc2a1]") {
    const auto authored = document();
    const auto &modules = builtinGraphModuleRegistry();
    const auto result = completeUnconnectedInputs(authored, modules);
    REQUIRE(result.inputs.size() == 1);
    CHECK(result.inputs[0].scope.empty());
    CHECK(result.inputs[0].node == "sum");
    CHECK(result.inputs[0].port == "a");
    CHECK(result.inputs[0].substitute == "zero_value");
    CHECK(result.document != authored);
    CHECK(node(authored, "sum").at("inputs").size() == 1);
    CHECK(node(result.document, "sum").at("inputs").at("a").at("node") == "gc2_unconnected_sum_a");
    CHECK(node(result.document, "gc2_unconnected_sum_a").at("parameters").at("value") == 0.0);
    CHECK(node(result.document, "gc2_unconnected_sum_a").at("origin").at("node") == "sum");
    REQUIRE_NOTHROW(partitionGraphDocument(authored, modules));

    auto definitions = authored;
    definitions["nodes"] = Json::array();
    definitions["outputs"] = Json::object();
    definitions["definitions"] = {{"body", authored}};
    const auto in_definition = completeUnconnectedInputs(definitions, modules);
    REQUIRE(in_definition.inputs.size() == 1);
    CHECK(in_definition.inputs[0].scope == "body");
    CHECK(node(definitions.at("definitions").at("body"), "sum").at("inputs").size() == 1);
}

TEST_CASE("[gc2a1] instance scalar input uses zero while parameters and defaults win", "[gc2a1]") {
    auto authored = Json::parse(R"({
        "schema": "pelican.graph", "version": 1, "nodes": [], "outputs": {},
        "definitions": {"body": {
            "nodes": [
                {"id": "gain", "op": "graph.input", "outputs": ["value"]},
                {"id": "sum", "op": "value.add", "inputs": {
                    "a": {"node": "gain", "port": "value"}, "b": {"node": "gain", "port": "value"}
                }, "outputs": ["value"]}
            ], "outputs": {"value": {"node": "sum", "port": "value"}}
        }}
    })");
    authored["nodes"].push_back({{"id", "instance"}, {"op", "graph.instance"}, {"definition", "body"}, {"outputs", {"value"}}});
    const auto original = authored;
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(result.inputs.size() == 1);
    CHECK(result.inputs[0].scope.empty());
    CHECK(result.inputs[0].node == "instance");
    CHECK(result.inputs[0].port == "gain");
    CHECK(authored == original);
    REQUIRE_NOTHROW(partitionGraphDocument(original));

    authored["nodes"][0]["parameters"] = {{"gain", 2.0}};
    CHECK(completeUnconnectedInputs(authored, builtinGraphModuleRegistry()).inputs.empty());
    authored["nodes"][0].erase("parameters");
    authored["definitions"]["body"]["nodes"][0]["default"] = 3.0;
    CHECK(completeUnconnectedInputs(authored, builtinGraphModuleRegistry()).inputs.empty());
}

TEST_CASE("[gc2a1] complete documents and optional inputs stay untouched", "[gc2a1]") {
    auto authored = document();
    authored["nodes"][1]["inputs"]["a"] = {{"node", "one"}, {"port", "value"}};
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CHECK(result.document == authored);
    CHECK(result.inputs.empty());

    authored["nodes"][1]["op"] = "value.sample_table";
    authored["nodes"][1]["parameters"] = {{"asset", "missing"}, {"row", "time"}};
    authored["nodes"][1]["inputs"].erase("a");
    CHECK(completeUnconnectedInputs(authored, builtinGraphModuleRegistry()).document == authored);
}

TEST_CASE("[gc2a1] synthetic ids avoid authored collisions deterministically", "[gc2a1]") {
    auto authored = document();
    authored["nodes"].push_back({{"id", "gc2_unconnected_sum_a"}, {"op", "value.constant"},
        {"parameters", {{"value", 9.0}}}, {"outputs", {"value"}}});
    const auto first = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    const auto second = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CHECK(first.document == second.document);
    CHECK(node(first.document, "sum").at("inputs").at("a").at("node") == "gc2_unconnected_sum_a_1");
    CHECK(node(authored, "gc2_unconnected_sum_a").at("parameters").at("value") == 9.0);
}

TEST_CASE("[gc2a1] non-scalars, types and structural failures still refuse", "[gc2a1]") {
    const auto &modules = builtinGraphModuleRegistry();
    auto events = document();
    events["nodes"][1] = {{"id", "clock"}, {"op", "value.event_clock"},
        {"inputs", {{"time", {{"node", "one"}, {"port", "value"}}}}}, {"outputs", {"value"}}};
    events["outputs"] = Json::object();
    CHECK(completeUnconnectedInputs(events, modules).document == events);
    REQUIRE_THROWS_WITH(partitionGraphDocument(events, modules), Catch::Matchers::ContainsSubstring("unconnected graph input: clock.events"));

    auto wrong_type = document();
    wrong_type["nodes"][0] = {{"id", "events"}, {"op", "event.playback"}, {"outputs", {"events"}}};
    wrong_type["nodes"][1]["inputs"] = {
        {"a", {{"node", "events"}, {"port", "events"}}},
        {"b", {{"node", "events"}, {"port", "events"}}}};
    REQUIRE_THROWS_WITH(partitionGraphDocument(wrong_type, modules), Catch::Matchers::ContainsSubstring("graph type mismatch at sum.a"));

    auto missing_source = document();
    missing_source["nodes"][1]["inputs"]["a"] = {{"node", "missing"}, {"port", "value"}};
    REQUIRE_THROWS_WITH(partitionGraphDocument(missing_source, modules), Catch::Matchers::ContainsSubstring("unknown graph source: missing"));
}

TEST_CASE("[gc2a1] authored edit accepts a scalar cut but returns the unfilled document", "[gc2a1]") {
    const auto authored = document();
    const auto edited = editGraphDocument(authored, Json::array({
        {{"op", "set_input"}, {"node", "sum"}, {"port", "a"}, {"source", nullptr}}}));
    CHECK(node(edited, "sum").at("inputs").contains("b"));
    CHECK_FALSE(node(edited, "sum").at("inputs").contains("a"));
    CHECK(edited.at("nodes").size() == authored.at("nodes").size());
    const auto instances = editGraphDocument(authored, Json::array({
        {{"op", "set_input"}, {"node", "sum"}, {"port", "a"}, {"source", nullptr}}}));
    CHECK(instances.at("nodes").size() == authored.at("nodes").size());
}

TEST_CASE("[gc2a1] several missing ports and loop-body inputs complete without aliasing", "[gc2a1]") {
    auto authored = document();
    authored["nodes"][1].erase("inputs");
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(result.inputs.size() == 2);
    CHECK(result.document.at("nodes").size() == 4);
    CHECK(node(result.document, "sum").at("inputs").at("a").at("node") == "gc2_unconnected_sum_a");
    CHECK(node(result.document, "sum").at("inputs").at("b").at("node") == "gc2_unconnected_sum_b");
    REQUIRE_NOTHROW(partitionGraphDocument(authored));

    auto loop = Json::parse(R"({
        "schema": "pelican.graph", "version": 1,
        "nodes": [{"id": "steps", "op": "graph.loop",
            "parameters": {"over": {"range": {"count": 2}}, "branches": [{"body": "body"}]},
            "outputs": ["value"]}],
        "definitions": {"body": {
            "nodes": [
                {"id": "target", "op": "graph.input", "outputs": ["value"]},
                {"id": "sum", "op": "value.add",
                    "inputs": {"a": {"node": "target", "port": "value"}}, "outputs": ["value"]}
            ],
            "outputs": {"value": {"node": "sum", "port": "value"}}
        }}, "outputs": {}
    })");
    const auto completed = completeUnconnectedInputs(loop, builtinGraphModuleRegistry());
    REQUIRE(completed.inputs.size() == 2);
    CHECK(completed.inputs[0].node == "steps");
    CHECK(completed.inputs[0].port == "target");
    CHECK(completed.inputs[1].scope == "body");
    CHECK(completed.inputs[1].port == "b");
    REQUIRE_NOTHROW(partitionGraphDocument(loop));
}


TEST_CASE("[gc2a1] synthetic nodes never become authored edit targets", "[gc2a1]") {
    const auto completed = completeUnconnectedInputs(document(), builtinGraphModuleRegistry());
    CHECK_THROWS_WITH(editGraphDocument(document(), Json::array({
        {{"op", "set_node_parameters"}, {"node", "gc2_unconnected_sum_a"},
         {"parameters", {{"value", 4.0}}}}})),
        Catch::Matchers::ContainsSubstring("graph.node_missing@1"));
    CHECK(node(completed.document, "gc2_unconnected_sum_a").at("parameters").at("value") == 0.0);
}

TEST_CASE("[gc2a1] image, draw stream and numbered render inputs keep their refusals", "[gc2a1]") {
    const auto refusal = [](const Json &authored) {
        try {
            (void)partitionGraphDocument(authored);
        } catch (const GraphRegistryError &error) {
            return std::string{error.code()};
        }
        return std::string{};
    };
    auto image = Json::parse(R"({
        "schema":"pelican.graph", "version":1, "nodes":[{
            "id":"paint", "op":"render.fullscreen",
            "parameters":{"shader":{"vertex":"engine://fullscreen", "fragment":"engine://passthrough"},
                "ports":{"source":{"kind":"image", "access":"read", "binding":"input", "input_index":0,
                    "type":{"semantic":"pelican.render.color_signal@1",
                        "arguments":{"reference":"scene", "transfer":"linear", "range":"extended"}}}}},
            "outputs":[]}
        ], "outputs":{}
    })");
    CHECK_THROWS_WITH(partitionGraphDocument(image),
        Catch::Matchers::ContainsSubstring("unconnected graph input: paint.source"));

    image["nodes"][0]["parameters"]["ports"]["source"]["input_index"] = 1;
    CHECK(refusal(image) == "graph.input_index_invalid@1");

    auto draws = Json::parse(R"({
        "schema":"pelican.graph", "version":1,
        "nodes":[
            {"id":"steps", "op":"graph.loop",
                "parameters":{"over":{"draw_items":{"contract":"forward_opaque_v1"}},
                    "branches":[{"body":"body"}]}, "outputs":["draws"]},
            {"id":"paint", "op":"render.material_pass",
                "parameters":{"material_contract":"forward_opaque_v1"}, "outputs":[]}
        ],
        "definitions":{"body":{
            "nodes":[{"id":"draw", "op":"draw.material", "outputs":["draws"]}],
            "outputs":{"draws":{"node":"draw", "port":"draws"}}
        }}, "outputs":{}
    })");
    CHECK(refusal(draws) == "graph.loop_draws_unconnected@1");
}

TEST_CASE("[gc2a1] render parameters and canvas stay refused by name", "[gc2a1]") {
    const auto refusal = [](const Json &authored) {
        try {
            (void)partitionGraphDocument(authored);
        } catch (const GraphRegistryError &error) {
            return std::string{error.code()};
        }
        return std::string{};
    };
    auto missing = document();
    missing["nodes"][1] = {{"id", "paint"}, {"op", "render.fullscreen"}, {"outputs", Json::array()}};
    CHECK(refusal(missing) == "graph.render_node_parameters_missing@1");

#if PELICAN_WITH_CANVAS
    auto canvas = document();
    canvas["nodes"][1] = {{"id", "out"}, {"op", "canvas.output"}, {"outputs", Json::array({"image"})}};
    try {
        (void)lowerAuthoringDocument(canvas);
        FAIL("an empty canvas output was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(std::string{error.code()} == "canvas.output_unconnected@1");
    }
#endif
}
