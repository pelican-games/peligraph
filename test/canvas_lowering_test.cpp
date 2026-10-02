// CV-1: the pelican.canvas lowering and its CPU reference, without a GPU.
#include "../src/project/authoringlowering.hpp"
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <set>
#include <string>

namespace {
using Json = nlohmann::json;

// Rounded card (corner_radius on a value.curve edge), a circle, a rotated
// ellipse outline, and a nested stack at half opacity holding an inner rect
// outline and a hidden layer.
Json authoredDocument() {
    return Json::parse(R"json({
  "schema": "pelican.graph", "version": 1, "definitions": {},
  "nodes": [
    {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
    {"id": "radius_curve", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 0}, {"time": 1, "value": 60}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "card", "op": "shape.rect", "parameters": {"size": [200, 140], "center": [100, 90], "corner_radius": 0},
     "inputs": {"corner_radius": {"node": "radius_curve", "port": "value"}}, "outputs": ["geometry"]},
    {"id": "card_fill", "op": "shape.fill", "parameters": {"color": [0.2, 0.4, 0.9, 1.0]},
     "inputs": {"geometry": {"node": "card", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "dot", "op": "shape.ellipse", "parameters": {"size": [100, 100], "center": [160, 160]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "dot_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.5, 0.1, 1.0]},
     "inputs": {"geometry": {"node": "dot", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "ring", "op": "shape.ellipse", "parameters": {"size": [160, 90], "center": [128, 200], "rotation": 30},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "ring_stroke", "op": "shape.stroke", "parameters": {"color": [1.0, 1.0, 1.0, 1.0], "width": 6},
     "inputs": {"geometry": {"node": "ring", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "frame", "op": "shape.rect", "parameters": {"size": [80, 60], "center": [190, 60]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "frame_stroke", "op": "shape.stroke",
     "parameters": {"color": [0.0, 1.0, 0.0, 1.0], "width": 4, "align": "inner"},
     "inputs": {"geometry": {"node": "frame", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "hidden", "op": "shape.rect", "parameters": {"size": [256, 256], "center": [128, 128]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "hidden_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "hidden", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "group", "op": "canvas.stack",
     "parameters": {"size": [256, 256],
                    "layers": [{"input": "frame", "name": "frame"}, {"input": "hidden", "visible": false}]},
     "inputs": {"frame": {"node": "frame_stroke", "port": "layer"}, "hidden": {"node": "hidden_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "root", "op": "canvas.stack",
     "parameters": {"size": [256, 256], "layers": [
       {"input": "card", "name": "card"}, {"input": "dot", "name": "dot"}, {"input": "ring", "name": "ring"},
       {"input": "group", "name": "group", "opacity": 0.5}]},
     "inputs": {"card": {"node": "card_fill", "port": "layer"}, "dot": {"node": "dot_fill", "port": "layer"},
                "ring": {"node": "ring_stroke", "port": "layer"}, "group": {"node": "group", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "out", "op": "canvas.output", "parameters": {"fit": "contain"},
     "inputs": {"layer": {"node": "root", "port": "layer"}}, "outputs": ["image"]},
    {"id": "after", "op": "render.fullscreen",
     "parameters": {"shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"},
                    "ports": {"source": {"kind": "image", "access": "read", "binding": "input", "input_index": 0,
                                         "type": {"semantic": "pelican.render.color_signal@1",
                                                  "arguments": {"reference": "scene", "transfer": "linear",
                                                                "range": "extended"}}}}},
     "inputs": {"source": {"node": "out", "port": "image"}}, "outputs": []}
  ],
  "outputs": {}
})json");
}

void validate(const Json &document) {
    const auto &registry = Pelican::builtinGraphModuleRegistry();
    (void)Pelican::validateGraphPorts(Pelican::expandGraphDocument(document), registry.typeRegistry(),
                                      Pelican::GraphOperationContractResolver{[&](const Json &node) {
                                          return registry.resolveOperation(node);
                                      }});
}

const Json &nodeById(const Json &document, const std::string &id) {
    for (const auto &node : document.at("nodes"))
        if (node.at("id") == id) return node;
    FAIL("no node " << id);
    throw std::logic_error("unreachable");
}

template <typename Edit> std::string refusal(Edit edit) {
    auto document = authoredDocument();
    edit(document);
    try {
        (void)Pelican::lowerCanvasNodes(document);
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

Json &mutableNode(Json &document, const std::string &id) {
    for (auto &node : document.at("nodes"))
        if (node.at("id") == id) return node;
    throw std::logic_error("no node " + id);
}

// One stack of `size` holding the given layers (paint node, shape node).
Json singleCanvas(std::array<double, 2> size, const Json &shapes, const Json &extra_output = Json::object()) {
    Json document{{"schema", "pelican.graph"}, {"version", 1}, {"nodes", Json::array()}, {"outputs", Json::object()}};
    Json layers = Json::array();
    Json inputs = Json::object();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const auto name = "l" + std::to_string(i);
        auto shape = shapes[i].at(0);
        shape["id"] = name + "_shape";
        shape["inputs"] = Json::object();
        shape["outputs"] = Json::array({"geometry"});
        auto paint = shapes[i].at(1);
        paint["id"] = name + "_paint";
        paint["inputs"] = {{"geometry", {{"node", name + "_shape"}, {"port", "geometry"}}}};
        paint["outputs"] = Json::array({"layer"});
        document["nodes"].push_back(shape);
        document["nodes"].push_back(paint);
        layers.push_back({{"input", name}});
        inputs[name] = {{"node", name + "_paint"}, {"port", "layer"}};
    }
    document["nodes"].push_back({{"id", "stack"}, {"op", "canvas.stack"},
                                 {"parameters", {{"size", {size[0], size[1]}}, {"layers", layers}}},
                                 {"inputs", inputs}, {"outputs", Json::array({"layer"})}});
    document["nodes"].push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", extra_output},
                                 {"inputs", {{"layer", {{"node", "stack"}, {"port", "layer"}}}}},
                                 {"outputs", Json::array({"image"})}});
    return document;
}

double alphaSum(const Pelican::CanvasImage &image) {
    double sum = 0.0;
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) sum += image.pixels[i];
    return sum;
}
} // namespace

TEST_CASE("CV-1 canvas nodes lower to the core render vocabulary", "[canvas][cv1]") {
    const auto authored = authoredDocument();
    // The authored document is a valid graph under the module's contracts.
    validate(authored);
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    REQUIRE(lowering.changed);
    const auto &document = lowering.document;

    std::set<std::string> ids;
    for (const auto &node : document.at("nodes")) ids.insert(node.at("id").get<std::string>());
    REQUIRE(ids == std::set<std::string>{"clock", "radius_curve", "after", "out$canvas_image", "out$canvas_items",
                                         "out$canvas_bake", "out$canvas_frame", "out$canvas_set_0", "out"});
    // The lowered document is a valid graph too: the raster pass keeps the
    // output's id and `image` port, so `after` still reads it.
    validate(document);

    const auto &raster = nodeById(document, "out");
    REQUIRE(raster.at("op") == "render.raster");
    const auto &parameters = raster.at("parameters");
    REQUIRE(parameters.at("draw") == Json{{"vertex_count", 6}, {"instance_source", "items"}});
    std::set<std::string> ports;
    for (const auto &[port, declaration] : parameters.at("ports").items()) ports.insert(port);
    REQUIRE(ports == std::set<std::string>{"bake", "frame", "image", "items"});
    REQUIRE(raster.at("outputs") == Json::array({"image"}));
    REQUIRE(raster.at("inputs").at("image") == Json{{"node", "out$canvas_image"}, {"port", "resource"}});
    REQUIRE(parameters.at("raster_state").at("color_attachments").at(0).at("blend").at("color") ==
            Json{{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}});
    REQUIRE(lowering.generated_sources.size() == 2);
    const auto &stem = lowering.generated_sources.at(0).stem;
    REQUIRE(stem.starts_with("canvas_out_"));
    REQUIRE(stem.size() == std::string{"canvas_out_"}.size() + 8);
    REQUIRE(parameters.at("shader").at("vertex") == "project://shaders/generated/" + stem);
    REQUIRE(lowering.generated_sources.at(1).stage == "frag");
    // All four kinds are drawn, so all four cases are generated.
    for (const auto *kind : {"case 0u", "case 1u", "case 2u", "case 3u"})
        REQUIRE(lowering.generated_sources.at(1).text.find(kind) != std::string::npos);

    // Four items: the hidden layer draws nothing.
    const auto &items = nodeById(document, "out$canvas_items").at("parameters");
    REQUIRE(items.at("element") == "mat4");
    REQUIRE(items.at("domain") == "instance");
    REQUIRE(items.at("count") == 4);
    REQUIRE(items.at("host_source") == "runtime_f32");
    const auto &table = items.at("initial_f32");
    REQUIRE(table.size() == 64);
    REQUIRE(table.at(0 * 16) == 0);  // card: rect_fill
    REQUIRE(table.at(1 * 16) == 2);  // dot: ellipse_fill
    REQUIRE(table.at(2 * 16) == 3);  // ring: ellipse_stroke
    REQUIRE(table.at(3 * 16) == 1);  // frame: rect_stroke
    REQUIRE(table.at(3 * 16 + 3) == 0.5); // the nested stack's opacity, multiplied in
    // The card reads corner_radius (its parameter 4) from frame table entry 0.
    const auto &bake = nodeById(document, "out$canvas_bake").at("parameters").at("initial_f32");
    const auto source_offset = table.at(2).get<double>();
    REQUIRE(source_offset >= 0.0);
    REQUIRE(bake.at(static_cast<std::size_t>(source_offset) + 4) == 1);
    REQUIRE(table.at(16 + 2) == -1); // the dot reads the bake table only

    const auto &sink = nodeById(document, "out$canvas_set_0");
    REQUIRE(sink.at("op") == "render.scalar");
    REQUIRE(sink.at("parameters") == Json{{"target", "out$canvas_frame"}, {"offset", 0}});
    REQUIRE(sink.at("inputs").at("value") == Json{{"node", "radius_curve"}, {"port", "value"}});
    REQUIRE(nodeById(document, "out$canvas_frame").at("parameters").at("size") == 4);

    REQUIRE(lowering.items.size() == 4);
    REQUIRE(lowering.items.at(3) == Json{{"output", "out"}, {"instance", 3}, {"kind", "rect_stroke"},
                                         {"nodes", {"frame_stroke", "frame", "group", "root"}}});
    REQUIRE(lowering.diagnostics ==
            Json::array({{{"code", "canvas.group_opacity_flattened"}, {"node", "group"}}}));

    // Deterministic: the same document lowers to the same bytes.
    const auto again = Pelican::lowerCanvasNodes(authored);
    REQUIRE(again.document.dump() == document.dump());
    REQUIRE(again.generated_sources.at(0).text == lowering.generated_sources.at(0).text);
    REQUIRE(again.generated_sources.at(1).text == lowering.generated_sources.at(1).text);

    // A document without canvas ops comes back as it was.
    Json plain{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
               {"nodes", Json::array({nodeById(authored, "clock")})}};
    const auto untouched = Pelican::lowerCanvasNodes(plain);
    REQUIRE_FALSE(untouched.changed);
    REQUIRE(untouched.document.dump() == plain.dump());
    REQUIRE(untouched.generated_sources.empty());

    // Fills only: no frame table, no stroke cases.
    const auto fills = Pelican::lowerCanvasNodes(singleCanvas(
        {100, 100}, Json::array({Json::array({{{"op", "shape.rect"}, {"parameters", {{"size", {10, 10}}, {"center", {50, 50}}}}},
                                              {{"op", "shape.fill"}, {"parameters", {{"color", {1, 1, 1, 1}}}}}})})));
    REQUIRE(fills.generated_sources.at(1).text.find("case 1u") == std::string::npos);
    REQUIRE(fills.generated_sources.at(1).text.find("pelican_load_frame") == std::string::npos);
    std::set<std::string> fill_ids;
    for (const auto &node : fills.document.at("nodes")) fill_ids.insert(node.at("id").get<std::string>());
    REQUIRE(fill_ids == std::set<std::string>{"out$canvas_image", "out$canvas_items", "out$canvas_bake", "out"});
}

TEST_CASE("CV-1 canvas lowering refuses by name", "[canvas][cv1]") {
    REQUIRE(refusal([](Json &document) {
                mutableNode(document, "root").at("parameters").at("layers").at(1)["blend"] = "multiply";
            }) == R"(canvas.blend_unsupported@1 {"blend":"multiply","layer":"dot","node":"root"})");
    REQUIRE(refusal([](Json &document) { mutableNode(document, "root").at("inputs").erase("ring"); }) ==
            R"(canvas.layer_input_missing@1 {"input":"ring","layer":"ring","node":"root"})");
    REQUIRE(refusal([](Json &document) { mutableNode(document, "out").at("inputs").erase("layer"); }) ==
            R"(canvas.output_unconnected@1 {"node":"out"})");
    REQUIRE(refusal([](Json &document) {
                document.at("nodes").push_back({{"id", "out$canvas_items"}, {"op", "value.constant"},
                                                {"parameters", {{"value", 1}}}, {"inputs", Json::object()},
                                                {"outputs", Json::array({"value"})}});
            }) == R"(canvas.reserved_id@1 {"id":"out$canvas_items"})");
    REQUIRE(refusal([](Json &document) {
                // The nested stack takes the root back as a layer.
                auto &group = mutableNode(document, "group");
                group.at("parameters").at("layers").push_back({{"input", "back"}});
                group.at("inputs")["back"] = {{"node", "root"}, {"port", "layer"}};
            }) == R"(canvas.stack_cycle@1 {"cycle":["root","group","root"],"node":"root"})");
    REQUIRE(refusal([](Json &document) {
                document["definitions"]["body"] = {
                    {"nodes", Json::array({{{"id", "r"}, {"op", "shape.rect"},
                                            {"parameters", {{"size", {1, 1}}, {"center", {0, 0}}}},
                                            {"inputs", Json::object()}, {"outputs", Json::array({"geometry"})}}})},
                    {"outputs", Json::object()}};
                document.at("nodes").push_back({{"id", "loop"}, {"op", "graph.loop"},
                                                {"parameters", {{"over", {{"count", 2}}},
                                                                {"branches", Json::array({{{"body", "body"}}})}}},
                                                {"inputs", Json::object()}, {"outputs", Json::array()}});
            }) == R"(canvas.in_loop_body@1 {"node":"r"})");
}

TEST_CASE("CV-1b the authoring hook lowers in place, idempotently, and names the output's image for the "
          "rendering section",
          "[canvas][cv1b]") {
    auto authored = authoredDocument();
    authored["rendering"] = Json::parse(R"json({
      "render_targets": [{"name": "dot"}],
      "rendering_passes": [{"name": "main_render", "passes": [
        {"name": "present", "type": "fullscreen", "input": ["out", "dot", "clock"]},
        {"name": "trail", "type": "fullscreen", "input": "out@history"},
        {"name": "kept", "type": "fullscreen", "input": ["out$canvas_image"]}]}]})json");
    // Only a canvas.output's id moves; other names, and the lowered name
    // written out (as CV-1 had to), stay.
    const auto lowering = Pelican::lowerAuthoringDocument(authored);
    REQUIRE(lowering.changed);
    const auto &passes = lowering.document.at("rendering").at("rendering_passes").at(0).at("passes");
    CHECK(passes.at(0).at("input") == Json::array({"out$canvas_image", "dot", "clock"}));
    CHECK(passes.at(1).at("input") == "out$canvas_image@history");
    CHECK(passes.at(2).at("input") == Json::array({"out$canvas_image"}));
    // The section's own target of the output's name keeps the name.
    auto shadowed = authored;
    shadowed["rendering"]["render_targets"].push_back({{"name", "out"}});
    CHECK(Pelican::lowerAuthoringDocument(shadowed).document.at("rendering").at("rendering_passes").at(0).at(
              "passes").at(0).at("input") == Json::array({"out", "dot", "clock"}));

    // The generated pair lives in a directory of its own under .pelican, and
    // the pass reads it from there.
    REQUIRE(lowering.generated_sources.size() == 2);
    const auto &report = lowering.reports.at("canvas");
    REQUIRE(report.at("outputs").size() == 1);
    const auto &output = report.at("outputs").at(0);
    const auto stem = output.at("generated").at(0).at("stem").get<std::string>();
    CHECK(lowering.generated_sources.at(0).path == ".pelican/generated/canvas/" + stem + "/" + stem + ".vert");
    CHECK(lowering.generated_sources.at(1).path == ".pelican/generated/canvas/" + stem + "/" + stem + ".frag");
    CHECK(nodeById(lowering.document, "out").at("parameters").at("shader").at("fragment") ==
          "project://.pelican/generated/canvas/" + stem + "/" + stem);
    CHECK(output.at("id") == "out");
    CHECK(output.at("kinds") == Json::array({"rect_fill", "rect_stroke", "ellipse_fill", "ellipse_stroke"}));
    CHECK(output.at("items").size() == 4);
    CHECK(output.at("items").at(3) ==
          Json{{"instance", 3}, {"kind", "rect_stroke"}, {"nodes", {"frame_stroke", "frame", "group", "root"}}});
    CHECK(output.at("generated").at(1).at("text") == lowering.generated_sources.at(1).text);
    CHECK(lowering.diagnostics ==
          Json::array({{{"code", "canvas.group_opacity_flattened"}, {"node", "group"}, {"lowering", "canvas"}}}));
    // The same shaders as the cli's lowering: only where they live differs.
    CHECK(Pelican::lowerCanvasNodes(authored).generated_sources.at(1).text == lowering.generated_sources.at(1).text);

    // Idempotent: the lowered document and a document without canvas ops come
    // back byte for byte.
    const auto again = Pelican::lowerAuthoringDocument(lowering.document);
    CHECK_FALSE(again.changed);
    CHECK(again.document.dump() == lowering.document.dump());
    CHECK(again.generated_sources.empty());
    CHECK(again.reports.at("canvas").at("outputs") == Json::array());
    const Json plain{{"schema", "pelican.graph"}, {"version", 1}, {"nodes", Json::array()}, {"outputs", Json::object()},
                     {"rendering", {{"rendering_passes", Json::array({{{"name", "main"}, {"passes", Json::array({
                         {{"name", "present"}, {"type", "fullscreen"}, {"input", Json::array({"out"})}}})}}})}}}};
    CHECK(Pelican::lowerAuthoringDocument(plain).document.dump() == plain.dump());

    // A named failure reaches the caller as it is.
    auto refused = authored;
    mutableNode(refused, "root").at("parameters").at("layers").at(0)["blend"] = "screen";
    CHECK_THROWS_AS(Pelican::lowerAuthoringDocument(refused), Pelican::GraphRegistryError);

    // Written once; the same text is left alone.
    const auto root = std::filesystem::temp_directory_path() / "pelican_cv1b_generated";
    std::filesystem::remove_all(root);
    Pelican::writeAuthoringGeneratedSources(lowering, root);
    const auto written = root / lowering.generated_sources.at(1).path;
    REQUIRE(std::filesystem::is_regular_file(written));
    const auto first_time = std::filesystem::last_write_time(written);
    Pelican::writeAuthoringGeneratedSources(lowering, root);
    CHECK(std::filesystem::last_write_time(written) == first_time);
    std::ifstream in(written, std::ios::binary);
    CHECK(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()} ==
          lowering.generated_sources.at(1).text);
    in.close();
    std::filesystem::remove_all(root);
}

TEST_CASE("CV-1 the CPU reference covers the analytic areas", "[canvas][cv1]") {
    const auto white = Json{{"op", "shape.fill"}, {"parameters", {{"color", {1, 1, 1, 1}}}}};
    const auto draw = [](const Json &document, std::uint32_t width, std::uint32_t height) {
        const auto ir = Pelican::buildCanvasIr(document);
        REQUIRE(ir.outputs.size() == 1);
        return Pelican::renderCanvasReference(ir.outputs.at(0), width, height);
    };
    SECTION("circle") {
        const auto image = draw(singleCanvas({256, 256}, Json::array({Json::array(
            {{{"op", "shape.ellipse"}, {"parameters", {{"size", {80, 80}}, {"center", {128.3, 127.6}}}}}, white})})),
                                256, 256);
        const auto area = std::numbers::pi * 40.0 * 40.0;
        REQUIRE(std::abs(alphaSum(image) - area) / area < 0.005);
    }
    SECTION("rounded and rotated rectangles") {
        const auto rect = [&](double radius, double rotation) {
            return draw(singleCanvas({256, 256}, Json::array({Json::array(
                {{{"op", "shape.rect"}, {"parameters", {{"size", {100, 60}}, {"center", {128, 128}},
                                                        {"corner_radius", radius}, {"rotation", rotation}}}},
                 white})})),
                        256, 256);
        };
        REQUIRE(std::abs(alphaSum(rect(0, 0)) - 6000.0) / 6000.0 < 0.005);
        const auto rounded = 6000.0 - (4.0 - std::numbers::pi) * 400.0;
        REQUIRE(std::abs(alphaSum(rect(20, 0)) - rounded) / rounded < 0.005);
        REQUIRE(std::abs(alphaSum(rect(0, 30)) - 6000.0) / 6000.0 < 0.005);
    }
    SECTION("stroke") {
        // A centred 4-unit outline of a 100 x 60 rectangle: 104 x 64 less 96 x 56.
        const auto image = draw(singleCanvas({256, 256}, Json::array({Json::array(
            {{{"op", "shape.rect"}, {"parameters", {{"size", {100, 60}}, {"center", {128, 128}}}}},
             {{"op", "shape.stroke"}, {"parameters", {{"color", {1, 1, 1, 1}}, {"width", 4}}}}})})),
                                256, 256);
        const auto area = 104.0 * 64.0 - 96.0 * 56.0;
        REQUIRE(std::abs(alphaSum(image) - area) / area < 0.01);
    }
    SECTION("the upper layer wins") {
        const auto image = draw(singleCanvas({100, 100}, Json::array({
            Json::array({{{"op", "shape.rect"}, {"parameters", {{"size", {60, 60}}, {"center", {40, 40}}}}},
                         {{"op", "shape.fill"}, {"parameters", {{"color", {1, 0, 0, 1}}}}}}),
            Json::array({{{"op", "shape.rect"}, {"parameters", {{"size", {60, 60}}, {"center", {60, 60}}}}},
                         {{"op", "shape.fill"}, {"parameters", {{"color", {0, 0, 1, 1}}}}}})})),
                                100, 100);
        const auto pixel = [&](std::size_t x, std::size_t y) { return &image.pixels[(y * 100 + x) * 4]; };
        // The overlap is the upper (blue) layer's; the lower one shows outside it.
        REQUIRE(pixel(50, 50)[0] == 0.0f);
        REQUIRE(pixel(50, 50)[2] == 1.0f);
        REQUIRE(pixel(50, 50)[3] == 1.0f);
        REQUIRE(pixel(20, 20)[0] == 1.0f);
        REQUIRE(pixel(20, 20)[2] == 0.0f);
    }
    SECTION("contain keeps the aspect, centred") {
        const auto image = draw(singleCanvas({200, 100}, Json::array({Json::array(
            {{{"op", "shape.rect"}, {"parameters", {{"size", {200, 100}}, {"center", {100, 50}}}}}, white})})),
                                256, 256);
        REQUIRE(std::abs(alphaSum(image) - 256.0 * 128.0) / (256.0 * 128.0) < 0.005);
        REQUIRE(image.pixels[(10 * 256 + 128) * 4 + 3] == 0.0f);  // above the canvas
        REQUIRE(image.pixels[(128 * 256 + 128) * 4 + 3] == 1.0f); // inside it
    }
    SECTION("an edge-driven parameter reads the values it is given") {
        auto document = singleCanvas({100, 100}, Json::array({Json::array(
            {{{"op", "shape.ellipse"}, {"parameters", {{"size", {10, 10}}, {"center", {50, 50}}}}}, white})}));
        document.at("nodes").push_back({{"id", "k"}, {"op", "value.constant"}, {"parameters", {{"value", 40}}},
                                        {"inputs", Json::object()}, {"outputs", Json::array({"value"})}});
        document.at("nodes").at(0)["inputs"]["size.x"] = {{"node", "k"}, {"port", "value"}};
        const auto ir = Pelican::buildCanvasIr(document);
        REQUIRE_THROWS_AS(Pelican::renderCanvasReference(ir.outputs.at(0), 100, 100), std::invalid_argument);
        const auto image = Pelican::renderCanvasReference(ir.outputs.at(0), 100, 100, {{{"k", "value"}, 40.0}});
        const auto area = std::numbers::pi * 20.0 * 5.0;
        REQUIRE(std::abs(alphaSum(image) - area) / area < 0.02);
    }
}
