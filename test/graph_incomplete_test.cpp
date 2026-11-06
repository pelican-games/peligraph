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
    const auto &nodes = body.is_array() ? body : body.at("nodes");
    for (const auto &entry : nodes)
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


namespace {
Json gc2a2ImageType(const std::string &transfer = "linear") {
    return Json{{"semantic", "pelican.render.color_signal@1"},
        {"arguments", {{"reference", "scene"}, {"transfer", transfer}, {"range", "extended"}}}};
}

Json gc2a2ImageDocument() {
    return Json{{"schema", "pelican.graph"}, {"version", 1},
        {"nodes", Json::array({{{"id", "paint"}, {"op", "render.fullscreen"},
            {"parameters", {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "shaders/paint"}}},
                {"ports", {{"source", {{"kind", "image"}, {"access", "read"}, {"binding", "sampled"},
                                          {"type", gc2a2ImageType()}}}}}}},
            {"outputs", Json::array()}}})},
        {"outputs", Json::object()}};
}

std::string gc2a2Refusal(const Json &document) {
    try {
        (void)partitionGraphDocument(document);
    } catch (const std::exception &error) {
        return error.what();
    }
    return {};
}
}

TEST_CASE("[gc2a2] sampled image reads share a produced magenta image by compatible type", "[gc2a2]") {
    auto authored = gc2a2ImageDocument();
    auto &ports = authored["nodes"][0]["parameters"]["ports"];
    ports["again"] = ports.at("source");
    const auto original = authored;
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(result.inputs.size() == 2);
    CHECK(result.inputs[0].substitute == "error_image");
    CHECK(result.inputs[1].substitute == "error_image");
    CHECK(authored == original);
    const auto source = node(result.document, "paint").at("inputs").at("source").at("node").get<std::string>();
    CHECK(node(result.document, "paint").at("inputs").at("again").at("node") == source);
    CHECK(node(result.document, source).at("op") == "render.image");
    CHECK(node(result.document, source).at("parameters").at("width") == 1);
    CHECK(node(result.document, source).at("parameters").at("height") == 1);
    CHECK_FALSE(node(result.document, source).contains("layout"));
    const auto producer = source + "_producer";
    CHECK(node(result.document, producer).at("op") == "render.fullscreen");
    CHECK(node(result.document, producer).at("parameters").at("shader").at("fragment") == "engine://error_magenta");
    CHECK(node(result.document, producer).at("inputs").at("color").at("node") == source);
    CHECK(result.document.at("nodes").size() == 3);

    ports["other"] = ports.at("source");
    ports["other"]["type"]["arguments"]["transfer"] = "output_encoded";
    const auto different = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(different.inputs.size() == 3);
    CHECK(node(different.document, "paint").at("inputs").at("other").at("node") != source);
    CHECK(different.document.at("nodes").size() == 5);
}

TEST_CASE("[gc2a2] definitions and nested instance image cuts use the real consumer type", "[gc2a2]") {
    auto body = gc2a2ImageDocument();
    body["nodes"][0]["inputs"] = {{"source", {{"node", "boundary"}, {"port", "value"}}}};
    body["nodes"].insert(body["nodes"].begin(), Json::object({{"id", "boundary"}, {"op", "graph.input"}}));
    body.erase("outputs");
    body["outputs"] = Json::object();
    Json authored{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"body", body}}}, {"outputs", Json::object()},
        {"nodes", Json::array({{{"id", "instance"}, {"op", "graph.instance"}, {"definition", "body"}}})}};
    const auto original = authored;
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CAPTURE(result.document.dump());
    CAPTURE(graphIncompleteInputsJson(result.inputs).dump());
    REQUIRE(result.inputs.size() == 1);
    CHECK(result.inputs[0].node == "instance");
    CHECK(result.inputs[0].port == "boundary");
    CHECK(result.inputs[0].substitute == "error_image");
    CHECK(authored == original);
    const auto expanded = expandGraphDocument(result.document);
    REQUIRE(expanded.nodes.size() == 3);
    CHECK(node(expanded.nodes, "instance/paint").at("inputs").at("source").at("node") ==
          "gc2_unconnected_image_0");
    authored["nodes"].push_back({{"id", "second"}, {"op", "graph.instance"}, {"definition", "body"}});
    const auto instances = expandGraphDocument(completeUnconnectedInputs(authored, builtinGraphModuleRegistry()).document);
    CHECK(node(instances.nodes, "instance/paint").at("inputs").at("source").at("node") !=
          node(instances.nodes, "second/paint").at("inputs").at("source").at("node"));
    authored["nodes"].erase(std::next(authored["nodes"].begin()));

    authored["definitions"]["outer"] = {
        {"nodes", Json::array({{{"id", "inner"}, {"op", "graph.instance"}, {"definition", "body"}}})},
        {"outputs", Json::object()}};
    authored["nodes"][0]["definition"] = "outer";
    const auto nested = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(nested.inputs.size() == 1);
    CHECK(nested.inputs[0].scope == "outer");
    CHECK(nested.inputs[0].node == "inner");
    CHECK(nested.inputs[0].port == "boundary");
    const auto expanded_nested = expandGraphDocument(nested.document);
    CHECK(node(expanded_nested.nodes, "instance/inner/paint").at("inputs").at("source").at("node") ==
          "instance/gc2_unconnected_image_0");
}

TEST_CASE("[gc2a2] an empty eligible image port still lowers as an ordinary graph", "[gc2a2]") {
    const auto completed = completeUnconnectedInputs(gc2a2ImageDocument(), builtinGraphModuleRegistry());
    const auto expanded = expandGraphDocument(completed.document);
    REQUIRE(expanded.nodes.size() == 3);
    CHECK(node(expanded.nodes, "paint").at("inputs").contains("source"));
    const auto producer_id = node(expanded.nodes, "paint").at("inputs").at("source").at("node").get<std::string>();
    CHECK(node(expanded.nodes, producer_id + "_producer").at("inputs").at("color").at("node") == producer_id);
}


TEST_CASE("[gc2a2] unsupported image and non-image ports keep their existing refusals", "[gc2a2]") {
    const auto refused = [](const Json &document, const std::string &port) {
        const auto completed = completeUnconnectedInputs(document, builtinGraphModuleRegistry());
        CAPTURE(document.dump());
        CHECK(completed.document == document);
        CHECK(completed.inputs.empty());
        CHECK(gc2a2Refusal(document).find("unconnected graph input: paint." + port) != std::string::npos);
    };
    const auto original = gc2a2ImageDocument();
    auto document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["access"] = "write";
    document["nodes"][0]["outputs"] = Json::array({"source"});
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["access"] = "read_write";
    document["nodes"][0]["outputs"] = Json::array({"source"});
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["history"] = true;
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["attachment"] = "color";
    document["nodes"][0]["parameters"]["ports"]["source"]["location"] = 0;
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["binding"] = "storage";
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["binding"] = "input";
    document["nodes"][0]["parameters"]["ports"]["source"]["input_index"] = 0;
    refused(document, "source");
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"]["type"] = {
        {"semantic", "pelican.render.depth@1"},
        {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    refused(document, "source");
    for (const auto *setting : {"view", "sampling", "subresource", "layers", "mip_levels", "sample_count",
                                "samples", "dimension", "usage"}) {
        document = original;
        document["nodes"][0]["parameters"]["ports"]["source"][setting] = Json::object();
        refused(document, "source");
    }
    for (const auto *topology : {"two_d_array", "cube", "volume"}) {
        document = original;
        document["nodes"][0]["parameters"]["ports"]["source"]["type"]["arguments"]["topology"] = topology;
        refused(document, "source");
    }
    document = original;
    document["nodes"][0]["parameters"]["ports"]["source"].erase("kind");
    document["nodes"][0]["parameters"]["ports"]["source"]["element"] = "float";
    refused(document, "source");

    document = original;
    document["nodes"][0]["op"] = "render.compute";
    document["nodes"][0]["parameters"]["shader"] = "shaders/paint.comp";
    document["nodes"][0]["parameters"]["ports"]["source"].erase("binding");
    refused(document, "source");
    document = original;
    document["nodes"][0]["op"] = "render.compute";
    document["nodes"][0]["parameters"]["shader"] = "shaders/paint.comp";
    document["nodes"][0]["parameters"]["dispatch"] = {{"groups_from", {{"port", "source"}}}};
    refused(document, "source");
    document["nodes"][0]["parameters"]["dispatch"] = {{"rays_from", {{"port", "source"}}}};
    refused(document, "source");

    for (const auto *field : {"vertex_source", "index_source", "instance_source"}) {
        document = original;
        document["nodes"][0]["op"] = "render.raster";
        document["nodes"][0]["parameters"]["draw"] = {{"vertex_count", 3}, {field, "source"}};
        refused(document, "source");
    }
    document = original;
    document["nodes"][0]["op"] = "render.raster";
    document["nodes"][0]["parameters"]["draw"] = {{"vertex_count", 3}};
    document["nodes"][0]["parameters"]["gpu_draw_source"] = Json::object();
    refused(document, "source");
    document = original;
    document["nodes"][0]["op"] = "render.material_pass";
    document["nodes"][0]["parameters"]["material_contract"] = "forward_opaque_v1";
    document["nodes"][0]["parameters"] = {{"material_contract", "forward_opaque_v1"}};
    CHECK(completeUnconnectedInputs(document, builtinGraphModuleRegistry()).document == document);
}

TEST_CASE("[gc2a2] synthetic image ids are deterministic and avoid authored collisions", "[gc2a2]") {
    auto authored = gc2a2ImageDocument();
    authored["nodes"].push_back({{"id", "gc2_unconnected_image_0"}, {"op", "value.constant"},
        {"parameters", {{"value", 7.0}}}, {"outputs", Json::array({"value"})}});
    const auto first = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    const auto second = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CHECK(first.document == second.document);
    const auto source = node(first.document, "paint").at("inputs").at("source").at("node").get<std::string>();
    CHECK(source == "gc2_unconnected_image_0_1");
    CHECK(node(first.document, source).at("origin").at("node") == "paint");
    CHECK(node(first.document, source).at("origin").at("port") == "source");
    CHECK(node(authored, "gc2_unconnected_image_0").at("parameters").at("value") == 7.0);
}

TEST_CASE("[gc2a2] no image edits returns an equal document and leaves scalar behavior unchanged", "[gc2a2]") {
    auto authored = gc2a2ImageDocument();
    authored["nodes"][0]["inputs"] = {{"source", {{"node", "image"}, {"port", "resource"}}}};
    authored["nodes"].insert(authored["nodes"].begin(), {{"id", "image"}, {"op", "render.image"},
        {"parameters", {{"type", gc2a2ImageType()}, {"format", "R8G8B8A8_UNORM"}, {"width", 1}, {"height", 1}}},
        {"outputs", Json::array({"resource"})}});
    const auto result = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CHECK(result.document == authored);
    CHECK(result.inputs.empty());
    const auto scalar = completeUnconnectedInputs(document(), builtinGraphModuleRegistry());
    REQUIRE(scalar.inputs.size() == 1);
    CHECK(scalar.inputs[0].substitute == "zero_value");
}

TEST_CASE("[gc2a2] declared compatible image formats drive the synthetic resource", "[gc2a2]") {
    auto authored = gc2a2ImageDocument();
    authored["nodes"][0]["parameters"]["ports"]["source"]["format"] = "R16G16B16A16_SFLOAT";
    authored["nodes"][0]["parameters"]["ports"]["source"]["format_class"] =
        "explicit(R16G16B16A16_SFLOAT)";
    authored["nodes"][0]["parameters"]["ports"]["again"] =
        authored["nodes"][0]["parameters"]["ports"]["source"];
    authored["nodes"][0]["parameters"]["ports"]["other"] =
        authored["nodes"][0]["parameters"]["ports"]["source"];
    authored["nodes"][0]["parameters"]["ports"]["other"].erase("format");
    authored["nodes"][0]["parameters"]["ports"]["other"].erase("format_class");
    authored["nodes"][0]["parameters"]["ports"]["other"]["format_candidates"] =
        {"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT"};
    const auto completed = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(completed.inputs.size() == 3);
    const auto image_format = [&](const std::string &port) {
        const auto &inputs = node(completed.document, "paint").at("inputs");
        const auto id = inputs.at(port).at("node").get<std::string>();
        return node(completed.document, id).at("parameters").at("format").get<std::string>();
    };
    CHECK(image_format("source") == "R16G16B16A16_SFLOAT");
    CHECK(image_format("again") == "R16G16B16A16_SFLOAT");
    CHECK(image_format("other") == "R8G8B8A8_UNORM");
    CHECK(node(completed.document, "paint").at("inputs").at("source").at("node") ==
          node(completed.document, "paint").at("inputs").at("again").at("node"));
    CHECK(node(completed.document, "paint").at("inputs").at("source").at("node") !=
          node(completed.document, "paint").at("inputs").at("other").at("node"));
}

TEST_CASE("[gc2a2] incompatible declared image formats retain their refusal", "[gc2a2]") {
    for (const auto format : {"R8_UNORM", "R8G8_UNORM", "R8G8B8A8_UINT", "R32G32B32A32_SINT",
                              "D32_SFLOAT", "not_a_format"}) {
        auto authored = gc2a2ImageDocument();
        authored["nodes"][0]["parameters"]["ports"]["source"]["format"] = format;
        CAPTURE(format);
        const auto completed = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
        CHECK(completed.document == authored);
        CHECK(completed.inputs.empty());
        CHECK(gc2a2Refusal(authored).find("unconnected graph input: paint.source") != std::string::npos);
    }
    auto authored = gc2a2ImageDocument();
    authored["nodes"][0]["parameters"]["ports"]["source"]["format_class"] = "scene";
    CHECK(completeUnconnectedInputs(authored, builtinGraphModuleRegistry()).document == authored);
}

TEST_CASE("[gc2a2] execution schedules separate otherwise identical synthetic sources", "[gc2a2]") {
    auto authored = gc2a2ImageDocument();
    auto &pass = authored["nodes"][0];
    pass["parameters"]["ports"]["again"] = pass["parameters"]["ports"]["source"];
    pass["parameters"]["schedule"] = "once";
    const auto once = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    CHECK(node(once.document, "gc2_unconnected_image_0_producer").at("parameters").at("schedule") == "once");
    authored["nodes"][1] = pass;
    authored["nodes"][1]["id"] = "later";
    authored["nodes"][1]["parameters"]["schedule"] = "per_frame";
    const auto scheduled = completeUnconnectedInputs(authored, builtinGraphModuleRegistry());
    REQUIRE(scheduled.inputs.size() == 4);
    CHECK(node(scheduled.document, "paint").at("inputs").at("source").at("node") !=
          node(scheduled.document, "later").at("inputs").at("source").at("node"));
}
