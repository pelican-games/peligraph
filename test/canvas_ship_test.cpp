// CV-5: the ship policy without a GPU -- which layers are static, where the
// runs are cut, the document each run is drawn from, the image that replaces
// it (read back through the CPU reference as the player's frame would be),
// and the constants folded into the shader.
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/canvas/canvasship.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;

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

Json image(const std::string &id, const std::string &asset, std::array<double, 2> offset, std::array<double, 2> size,
           Json extra = Json::object()) {
    Json parameters{{"asset", asset}, {"offset", {offset[0], offset[1]}}, {"size", {size[0], size[1]}}};
    parameters.update(extra);
    return {{"id", id}, {"op", "canvas.image"}, {"parameters", parameters}, {"outputs", Json::array({"layer"})}};
}

// An ellipse and its fill `<id>`; `inputs` drive the ellipse's ports.
std::vector<Json> disc(const std::string &id, std::array<double, 2> center, double diameter,
                       std::array<double, 4> rgba, Json inputs = Json::object()) {
    return {{{"id", id + "_shape"},
             {"op", "shape.ellipse"},
             {"parameters", {{"size", {diameter, diameter}}, {"center", {center[0], center[1]}}}},
             {"inputs", inputs},
             {"outputs", Json::array({"geometry"})}},
            {{"id", id},
             {"op", "shape.fill"},
             {"parameters", {{"color", {rgba[0], rgba[1], rgba[2], rgba[3]}}}},
             {"inputs", {{"geometry", {{"node", id + "_shape"}, {"port", "geometry"}}}}},
             {"outputs", Json::array({"layer"})}}};
}

Json timeInput() {
    return {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "time"}}}, {"outputs", Json::array({"value"})}};
}

const Json drive{{"center.x", {{"node", "clock"}, {"port", "value"}}}};

// A stack node: `layers` as authored, each layer's input fed by the node of
// the same name (and each mask by its node).
Json stack(const std::string &id, const Json &layers, Json parameters = Json::object()) {
    Json inputs = Json::object();
    for (const auto &layer : layers) {
        inputs[layer.at("input").get<std::string>()] = {{"node", layer.at("input")}, {"port", "layer"}};
        if (layer.contains("mask")) inputs[layer.at("mask").get<std::string>()] = {{"node", layer.at("mask")}, {"port", "layer"}};
    }
    parameters["layers"] = layers;
    return {{"id", id}, {"op", "canvas.stack"}, {"parameters", parameters}, {"inputs", inputs},
            {"outputs", Json::array({"layer"})}};
}

Json document(Json nodes, const std::string &top = "root") {
    nodes.push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", {{"fit", "stretch"}}},
                     {"inputs", {{"layer", {{"node", top}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}});
    return {{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()}, {"nodes", nodes},
            {"outputs", Json::object()}};
}

Json nodes(std::initializer_list<Json> single, std::initializer_list<std::vector<Json>> pairs = {}) {
    auto result = Json::array();
    for (const auto &pair : pairs)
        for (const auto &node : pair) result.push_back(node);
    for (const auto &node : single) result.push_back(node);
    return result;
}

template <typename Body> std::string refusal(Body body) {
    try {
        body();
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

// The runs as [stack, first, last, reason, layer names].
Json runs(const Pelican::CanvasFlattenPlan &plan) {
    auto result = Json::array();
    for (const auto &run : plan.runs)
        result.push_back({run.stack, run.first, run.last, run.reason, run.layers});
    return result;
}

Pelican::CanvasFlattenPlan plan(const Json &authored, const Pelican::CanvasAssetReader &reader = {}) {
    return Pelican::planCanvasFlatten(authored, reader);
}

Pelican::CanvasReferenceImage solid(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 4> rgba) {
    Pelican::CanvasReferenceImage result{width, height, {}};
    for (std::uint32_t i = 0; i < width * height; ++i) result.rgba.insert(result.rgba.end(), rgba.begin(), rgba.end());
    return result;
}

// What the player writes: the premultiplied linear canvas, sRGB-encoded, alpha kept.
std::vector<int> frameBytes(const Pelican::CanvasImage &image) {
    std::vector<int> bytes;
    for (std::size_t at = 0; at < image.pixels.size(); ++at) {
        const double value = std::clamp(static_cast<double>(image.pixels[at]), 0.0, 1.0);
        const auto encoded = at % 4 == 3 ? value
                             : value <= 0.0031308 ? value * 12.92
                                                  : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
        bytes.push_back(static_cast<int>(std::lround(encoded * 255.0)));
    }
    return bytes;
}

Pelican::CanvasOutput output(const Json &document, const std::string &id) {
    const auto ir = Pelican::buildCanvasIr(document);
    for (const auto &candidate : ir.outputs)
        if (candidate.id == id) return candidate;
    FAIL("no canvas.output " << id);
    throw std::logic_error("unreachable");
}

bool hasCanvasOp(const Json &document) { return Pelican::documentHasCanvasNodes(document); }
} // namespace

TEST_CASE("CV-5 a layer is static when no edge from outside the canvas reaches it", "[canvas][cv5]") {
    // Bottom to top: two images, a disc that moves, an image under an
    // opacity edge, two images, a canvas.from_image, an image.
    auto authored = document(nodes(
        {timeInput(), image("a", "project://a.png", {0, 0}, {8, 8}), image("b", "project://b.png", {2, 2}, {8, 8}),
         image("c", "project://c.png", {0, 0}, {4, 4}), image("d", "project://d.png", {0, 0}, {4, 4}),
         image("e", "project://e.png", {1, 1}, {4, 4}), image("g", "project://g.png", {1, 1}, {4, 4}),
         image("gm", "project://gm.png", {1, 1}, {4, 4}, {{"channel", "luminance"}}),
         {{"id", "pass"}, {"op", "render.image"},
          {"parameters", {{"type", {{"semantic", "pelican.render.color_signal@1"},
                                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
                          {"format", "R16G16B16A16_SFLOAT"}, {"extent_scale", 1.0},
                          {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
          {"outputs", Json::array({"resource"})}},
         {{"id", "f"}, {"op", "canvas.from_image"}, {"parameters", {{"size", {16, 16}}}},
          {"inputs", {{"image", {{"node", "pass"}, {"port", "resource"}}}}}, {"outputs", Json::array({"layer"})}},
         stack("root",
               Json::array({{{"input", "a"}, {"name", "A"}}, {{"input", "b"}, {"name", "B"}},
                            {{"input", "m"}, {"name", "M"}}, {{"input", "c"}, {"name", "C"}},
                            {{"input", "d"}, {"name", "D"}}, {{"input", "e"}, {"name", "E"}},
                            {{"input", "f"}, {"name", "F"}}, {{"input", "g"}, {"name", "G"}, {"mask", "gm"}}}),
               {{"size", {16, 16}}})},
        {disc("m", {8, 8}, 6, {1, 0, 0, 1}, drive)}));
    for (auto &node : authored["nodes"])
        if (node.at("id") == "root") node["inputs"]["c.opacity"] = {{"node", "clock"}, {"port", "value"}};
    const auto result = plan(authored);
    // M moves, C's opacity moves, F is another pass's image: [A B], [D E] and
    // G (masked, so not one image already) are folded.
    REQUIRE(runs(result) == Json::array({{"root", 0, 1, "static_from_bottom", {"A", "B"}},
                                         {"root", 4, 5, "static_above_cut", {"D", "E"}},
                                         {"root", 7, 7, "static_above_cut", {"G"}}}));
    for (const auto &run : result.runs) {
        REQUIRE(run.output == "out");
        REQUIRE_FALSE(run.group);
        REQUIRE(run.pixels == std::array<std::uint32_t, 2>{16, 16});
        REQUIRE(run.digest.size() == 16);
    }
    REQUIRE(result.diagnostics.empty());
}

TEST_CASE("CV-5 the cut: backdrop readers, shapes alone, bake texture, a whole static stack", "[canvas][cv5]") {
    const auto images = [](std::initializer_list<const char *> ids) {
        std::vector<Json> result;
        for (const auto *id : ids) result.push_back(image(id, std::string{"project://"} + id + ".png", {0, 0}, {4, 4}));
        return result;
    };
    const auto build = [&](Json layers, std::vector<Json> extra = {}, Json parameters = {{"size", {4, 4}}}) {
        auto list = Json::array({timeInput()});
        for (auto &node : images({"a", "b", "c", "d", "e"})) list.push_back(node);
        for (auto &node : disc("m", {2, 2}, 2, {1, 0, 0, 1}, drive)) list.push_back(node);
        for (auto &node : disc("s1", {1, 1}, 2, {0, 1, 0, 1})) list.push_back(node);
        for (auto &node : disc("s2", {3, 3}, 2, {0, 0, 1, 1})) list.push_back(node);
        for (auto &node : extra) list.push_back(node);
        list.push_back(stack("root", layers, parameters));
        auto authored = document(list);
        validate(authored);
        return plan(authored);
    };
    SECTION("a blend or an adjustment layer above something that stays cuts the run") {
        const Json adjustment[] = {{{"id", "back"}, {"op", "canvas.backdrop"}, {"outputs", Json::array({"layer"})}},
                                   {{"id", "inv"}, {"op", "filter.invert"},
                                    {"inputs", {{"layer", {{"node", "back"}, {"port", "layer"}}}}},
                                    {"outputs", Json::array({"layer"})}}};
        const auto result = build(Json::array({{{"input", "m"}, {"name", "M"}},
                                               {{"input", "a"}, {"name", "A"}},
                                               {{"input", "b"}, {"name", "B"}, {"blend", "multiply"}},
                                               {{"input", "c"}, {"name", "C"}},
                                               {{"input", "inv"}, {"name", "Adjust"}},
                                               {{"input", "d"}, {"name", "D"}},
                                               {{"input", "e"}, {"name", "E"}}}),
                                  {adjustment[0], adjustment[1]});
        // A and C are one image each already; D and E fold.
        REQUIRE(runs(result) == Json::array({{"root", 5, 6, "static_above_cut", {"D", "E"}}}));
    }
    SECTION("a stack that is static all through is one run, its blends included") {
        const auto result = build(Json::array({{{"input", "a"}, {"name", "A"}},
                                               {{"input", "b"}, {"name", "B"}, {"blend", "multiply"}},
                                               {{"input", "c"}, {"name", "C"}}}));
        REQUIRE(runs(result) == Json::array({{"root", 0, 2, "static_unit", {"A", "B", "C"}}}));
    }
    SECTION("shapes alone stay shapes, unless a layer asks for a texture") {
        const auto shapes = build(Json::array({{{"input", "s1"}, {"name", "S1"}}, {{"input", "s2"}, {"name", "S2"}},
                                               {{"input", "m"}, {"name", "M"}}, {{"input", "a"}, {"name", "A"}}}));
        REQUIRE(runs(shapes).empty());
        const auto texture =
            build(Json::array({{{"input", "s1"}, {"name", "S1"}}, {{"input", "s2"}, {"name", "S2"}, {"bake", "texture"}},
                               {{"input", "m"}, {"name", "M"}}, {{"input", "a"}, {"name", "A"}}}));
        REQUIRE(runs(texture) == Json::array({{"root", 0, 1, "bake_texture", {"S1", "S2"}}}));
        // Shapes beside an image fold with it.
        const auto mixed = build(Json::array({{{"input", "s1"}, {"name", "S1"}}, {{"input", "a"}, {"name", "A"}},
                                              {{"input", "m"}, {"name", "M"}}}));
        REQUIRE(runs(mixed) == Json::array({{"root", 0, 1, "static_from_bottom", {"S1", "A"}}}));
    }
    SECTION("a hidden layer joins the run around it, never ends one") {
        const auto result = build(Json::array({{{"input", "a"}, {"name", "A"}},
                                               {{"input", "s1"}, {"name", "Hidden"}, {"visible", false}},
                                               {{"input", "b"}, {"name", "B"}},
                                               {{"input", "c"}, {"name", "Hidden too"}, {"visible", false}},
                                               {{"input", "m"}, {"name", "M"}}}));
        REQUIRE(runs(result) == Json::array({{"root", 0, 2, "static_from_bottom", {"A", "B"}}}));
    }
    SECTION("a blend inside a pass-through group reads the stack's backdrop") {
        const auto group = stack("group", Json::array({{{"input", "c"}}, {{"input", "d"}, {"blend", "screen"}}}));
        const auto through = build(Json::array({{{"input", "m"}, {"name", "M"}},
                                                {{"input", "group"}, {"name", "G"}, {"pass_through", true}},
                                                {{"input", "a"}, {"name", "A"}}, {{"input", "b"}, {"name", "B"}}}),
                                   {group});
        REQUIRE(runs(through) == Json::array({{"root", 2, 3, "static_above_cut", {"A", "B"}}}));
        // Composited on its own, the group reads its own backdrop alone.
        const auto isolated = build(Json::array({{{"input", "m"}, {"name", "M"}}, {{"input", "group"}, {"name", "G"}},
                                                 {{"input", "a"}, {"name", "A"}}}),
                                    {group});
        REQUIRE(runs(isolated) == Json::array({{"root", 1, 2, "static_above_cut", {"G", "A"}}}));
    }
    SECTION("a clip layer goes with its base") {
        const auto result = build(Json::array({{{"input", "m"}, {"name", "M"}}, {{"input", "a"}, {"name", "A"}},
                                               {{"input", "b"}, {"name", "B"}, {"clip", true}}}));
        REQUIRE(runs(result) == Json::array({{"root", 1, 2, "static_above_cut", {"A", "B"}}}));
        auto moving = Json::array({{{"input", "a"}, {"name", "A"}}, {{"input", "b"}, {"name", "B"}},
                                   {{"input", "m"}, {"name", "M"}, {"clip", true}}});
        REQUIRE(runs(build(moving)).empty());
    }
}

TEST_CASE("CV-5 a nested stack with a size is cut on its own, a placed one is not", "[canvas][cv5]") {
    auto list = Json::array({timeInput(), image("a", "project://a.png", {0, 0}, {4, 4}),
                             image("b", "project://b.png", {0, 0}, {4, 4})});
    for (auto &node : disc("m", {2, 2}, 2, {1, 0, 0, 1}, drive)) list.push_back(node);
    list.push_back(stack("inner", Json::array({{{"input", "a"}, {"name", "A"}}, {{"input", "b"}, {"name", "B"}},
                                               {{"input", "m"}, {"name", "M"}}}),
                         {{"size", {4, 4}}}));
    list.push_back(stack("root", Json::array({{{"input", "inner"}, {"name", "Inner"}}}), {{"size", {4, 4}}}));
    auto authored = document(list);
    validate(authored);
    const auto result = plan(authored);
    REQUIRE(runs(result) == Json::array({{"inner", 0, 1, "static_from_bottom", {"A", "B"}}}));
    REQUIRE(result.runs.front().group);
    REQUIRE(result.runs.front().output == "out");

    // The same stack under a canvas.place is drawn as authored.
    auto placed = authored;
    placed["nodes"].push_back({{"id", "scene"}, {"op", "render.image"},
                               {"parameters", {{"type", {{"semantic", "pelican.render.color_signal@1"},
                                                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                                                         {"range", "extended"}}}}},
                                               {"format", "R16G16B16A16_SFLOAT"}, {"extent_scale", 1.0},
                                               {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
                               {"outputs", Json::array({"resource"})}});
    placed["nodes"].push_back({{"id", "board"}, {"op", "canvas.place"},
                               {"inputs", {{"layer", {{"node", "inner"}, {"port", "layer"}}},
                                           {"color", {{"node", "scene"}, {"port", "resource"}}}}},
                               {"outputs", Json::array({"color"})}});
    const auto placed_plan = plan(placed);
    REQUIRE(placed_plan.runs.empty());
    REQUIRE(placed_plan.diagnostics ==
            Json::array({{{"code", "canvas.flatten_skipped"}, {"reason", "placed"}, {"node", "board"}}}));
}

TEST_CASE("CV-5 a run is drawn from a document of its own, named by its content", "[canvas][cv5]") {
    auto list = Json::array({timeInput(), image("a", "project://a.png", {0, 0}, {6, 4}),
                             image("b", "project://b.png", {1, 1}, {4, 2}),
                             image("bm", "project://bm.png", {0, 0}, {6, 4}, {{"channel", "luminance"}})});
    for (auto &node : disc("m", {2, 2}, 2, {1, 0, 0, 1}, drive)) list.push_back(node);
    list.push_back(stack("root", Json::array({{{"input", "a"}, {"name", "A"}},
                                              {{"input", "b"}, {"name", "B"}, {"mask", "bm"}, {"opacity", 0.5}},
                                              {{"input", "m"}, {"name", "M"}}}),
                         {{"size", {6, 4}}, {"bake_scale", 2.5}, {"blend_space", "srgb"}}));
    const auto authored = document(list);
    validate(authored);
    std::map<std::string, std::vector<std::byte>> files{{"project://a.png", {std::byte{1}}},
                                                        {"project://b.png", {std::byte{2}}}};
    const Pelican::CanvasAssetReader reader = [&](std::string_view reference) {
        const auto found = files.find(std::string{reference});
        return found == files.end() ? nullptr : std::make_shared<const std::vector<std::byte>>(found->second);
    };
    const auto first = plan(authored, reader);
    REQUIRE(first.runs.size() == 1);
    const auto &run = first.runs.front();
    REQUIRE(run.pixels == std::array<std::uint32_t, 2>{15, 10});
    REQUIRE(run.size == std::array<double, 2>{6, 4});
    REQUIRE(run.assets == std::vector<std::string>{"project://a.png", "project://b.png", "project://bm.png"});
    // The run's nodes, its stack with its layers alone, an output (drawn at its pixels).
    std::vector<std::string> ids;
    for (const auto &node : run.document.at("nodes")) ids.push_back(node.at("id").get<std::string>());
    REQUIRE(ids == std::vector<std::string>{"a", "b", "bm", "root", run.bake_output});
    const auto &copy = nodeById(run.document, "root");
    REQUIRE(copy.at("parameters").at("layers").size() == 2);
    REQUIRE(copy.at("parameters").at("blend_space") == "srgb");
    REQUIRE(copy.at("inputs").size() == 3);
    REQUIRE(nodeById(run.document, run.bake_output).at("parameters") == Json{{"fit", "stretch"}});
    REQUIRE(run.document.at("rendering").at("rendering_passes").at(0).at("passes").at(0).at("input") ==
            Json::array({Pelican::canvasImageNodeId(run.bake_output)}));
    validate(run.document);
    const auto lowered = Pelican::lowerCanvasNodes(run.document);
    REQUIRE_FALSE(hasCanvasOp(lowered.document));
    // The digest follows the document and the files' bytes, nothing else.
    REQUIRE(plan(authored, reader).runs.front().digest == run.digest);
    files["project://b.png"] = {std::byte{3}};
    REQUIRE(plan(authored, reader).runs.front().digest != run.digest);
    REQUIRE(Pelican::canvasFlattenImagePath(run) == "canvas_baked/" + run.digest + ".png");
    // Too many pixels a side.
    auto huge = authored;
    for (auto &node : huge["nodes"])
        if (node.at("id") == "root") node["parameters"]["bake_scale"] = 3000;
    REQUIRE(refusal([&] { (void)plan(huge); }) ==
            R"(canvas.flatten_size_exceeded@1 {"limit":16384,"pixels":[18000.0,12000.0],"stack":"root"})");
    for (auto &node : huge["nodes"])
        if (node.at("id") == "root") node["parameters"]["bake_scale"] = -1;
    REQUIRE_THROWS_AS(plan(huge), std::invalid_argument);
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(huge), std::invalid_argument);
}

TEST_CASE("CV-5 the folded run reads back as the picture it replaces", "[canvas][cv5]") {
    // A PSD-like sRGB stack: two images under a disc that moves, a masked
    // image at half opacity and a clipped one above it.
    for (const char *space : {"srgb", "linear"}) {
        CAPTURE(space);
        auto list = Json::array({timeInput(), image("bg", "project://bg.png", {0, 0}, {16, 8}),
                                 image("card", "project://card.png", {2, 1}, {10, 6}),
                                 image("top", "project://top.png", {6, 2}, {8, 5}),
                                 image("top_mask", "project://mask.png", {6, 2}, {8, 5}, {{"channel", "luminance"}}),
                                 image("shine", "project://shine.png", {4, 0}, {6, 8})});
        for (auto &node : disc("m", {8, 4}, 5, {0.9, 0.2, 0.1, 0.8}, drive)) list.push_back(node);
        list.push_back(stack("root",
                             Json::array({{{"input", "bg"}, {"name", "Background"}}, {{"input", "card"}, {"name", "Card"}},
                                          {{"input", "m"}, {"name", "Disc"}},
                                          {{"input", "top"}, {"name", "Top"}, {"mask", "top_mask"}, {"opacity", 0.5}},
                                          {{"input", "shine"}, {"name", "Shine"}, {"clip", true}}}),
                             {{"size", {16, 8}}, {"blend_space", space}}));
        const auto authored = document(list);
        Pelican::CanvasReferenceImages images{
            {"project://bg.png", solid(16, 8, {40, 60, 200, 255})}, {"project://card.png", solid(10, 6, {250, 240, 20, 160})},
            {"project://top.png", solid(8, 5, {20, 200, 90, 200})}, {"project://mask.png", solid(8, 5, {180, 180, 180, 255})},
            {"project://shine.png", solid(6, 8, {255, 255, 255, 90})}};
        const auto result = plan(authored);
        REQUIRE(runs(result) == Json::array({{"root", 0, 1, "static_from_bottom", {"Background", "Card"}},
                                             {"root", 3, 4, "static_above_cut", {"Top", "Shine"}}}));
        // Each run drawn alone, as the player would write it.
        for (const auto &run : result.runs) {
            const auto frame = frameBytes(Pelican::renderCanvasReference(output(run.document, run.bake_output),
                                                                         run.pixels[0], run.pixels[1], {}, images));
            Pelican::CanvasReferenceImage baked{run.pixels[0], run.pixels[1], {}};
            for (const auto byte : frame) baked.rgba.push_back(static_cast<std::uint8_t>(byte));
            images["project://" + Pelican::canvasFlattenImagePath(run)] = std::move(baked);
        }
        const auto folded = Pelican::applyCanvasFlatten(authored, result);
        validate(folded);
        const auto &root = nodeById(folded, "root");
        REQUIRE(root.at("parameters").at("layers") ==
                Json::array({{{"input", "baked0"}, {"name", "Background + Card"}}, {{"input", "m"}, {"name", "Disc"}},
                             {{"input", "baked1"}, {"name", "Top + Shine"}}}));
        REQUIRE(root.at("inputs") == Json{{"baked0", {{"node", "root$baked0"}, {"port", "layer"}}},
                                          {"m", {{"node", "m"}, {"port", "layer"}}},
                                          {"baked1", {{"node", "root$baked1"}, {"port", "layer"}}}});
        REQUIRE(nodeById(folded, "root$baked1").at("parameters") ==
                Json{{"asset", "project://" + Pelican::canvasFlattenImagePath(result.runs[1])},
                     {"offset", {0, 0}},
                     {"size", {16.0, 8.0}},
                     {"alpha", "premultiplied"}});
        // Read through the sRGB decode, premultiplied; an sRGB stack encodes back.
        const auto folded_output = output(folded, "out");
        const auto srgb = std::string{space} == "srgb";
        const auto &items = srgb ? folded_output.groups.at(0).items : folded_output.items;
        REQUIRE(items.size() == 3);
        REQUIRE_FALSE(items[0].texture.linear);
        REQUIRE(items[0].parameters.at(4).value == 1.0);
        REQUIRE(items[0].parameters.at(5).value == (srgb ? 1.0 : 0.0));
        // The same picture at two times, within the 8 bits the frame has.
        for (const double time : {0.0, 12.0}) {
            CAPTURE(time);
            const Pelican::CanvasValues values{{{"clock", "value"}, time}};
            const auto expected = frameBytes(Pelican::renderCanvasReference(output(authored, "out"), 16, 8, values, images));
            const auto shipped = frameBytes(Pelican::renderCanvasReference(folded_output, 16, 8, values, images));
            int worst = 0;
            for (std::size_t at = 0; at < expected.size(); ++at) worst = std::max(worst, std::abs(expected[at] - shipped[at]));
            CHECK(worst <= 2);
        }
        // Lowered for shipping, nothing of the canvas is left.
        Pelican::CanvasLoweringOptions options;
        options.policy = Pelican::CanvasLoweringPolicy::ship;
        REQUIRE_FALSE(hasCanvasOp(Pelican::lowerCanvasNodes(folded, options).document));
    }
}

TEST_CASE("CV-5 a canvas definition's runs are cut where the lowering expands it", "[canvas][cv5]") {
    auto body = Json::array({timeInput(), image("a", "project://a.png", {0, 0}, {4, 4}),
                             image("b", "project://b.png", {1, 1}, {2, 2})});
    for (auto &node : disc("m", {2, 2}, 2, {1, 0, 0, 1}, drive)) body.push_back(node);
    body.push_back(stack("root", Json::array({{{"input", "a"}, {"name", "A"}}, {{"input", "b"}, {"name", "B"}},
                                              {{"input", "m"}, {"name", "M"}}}),
                         {{"size", {4, 4}}}));
    Json authored{{"schema", "pelican.graph"},
                  {"version", 1},
                  {"definitions", {{"art", {{"nodes", body}, {"outputs", {{"layer", {{"node", "root"}, {"port", "layer"}}}}}}}}},
                  {"nodes",
                   {{{"id", "psd"}, {"op", "graph.instance"}, {"definition", "art"}},
                    {{"id", "out"}, {"op", "canvas.output"}, {"inputs", {{"layer", {{"node", "psd"}, {"port", "layer"}}}}},
                     {"outputs", Json::array({"image"})}}}},
                  {"outputs", Json::object()}};
    const auto result = plan(authored);
    REQUIRE(runs(result) == Json::array({{"psd$root", 0, 1, "static_from_bottom", {"A", "B"}}}));
    const auto folded = Pelican::applyCanvasFlatten(authored, result);
    REQUIRE_FALSE(folded.at("definitions").contains("art"));
    REQUIRE(nodeById(folded, "psd$root$baked0").at("op") == "canvas.image");
    // The images only the run read are no longer named by what is drawn.
    Pelican::CanvasLoweringOptions options;
    options.policy = Pelican::CanvasLoweringPolicy::ship;
    const auto text = Pelican::lowerCanvasNodes(folded, options).document.dump();
    REQUIRE(text.find("project://a.png") == std::string::npos);
    REQUIRE(text.find(Pelican::canvasFlattenImagePath(result.runs.front())) != std::string::npos);
    REQUIRE(Pelican::canvasDocumentAssets(authored) == std::vector<std::string>{"project://a.png", "project://b.png"});
}

TEST_CASE("CV-5 ship folds into the shader the numbers every item of a kind shares", "[canvas][cv5]") {
    // Three discs of one colour and size; one moves.
    auto list = Json::array({timeInput()});
    for (auto &node : disc("d0", {4, 4}, 6, {1, 0.5, 0.25, 1})) list.push_back(node);
    for (auto &node : disc("d1", {12, 5}, 6, {1, 0.5, 0.25, 1})) list.push_back(node);
    for (auto &node : disc("d2", {20, 6}, 6, {1, 0.5, 0.25, 1}, drive)) list.push_back(node);
    list.push_back(stack("root", Json::array({{{"input", "d0"}}, {{"input", "d1"}}, {{"input", "d2"}}}),
                         {{"size", {24, 8}}}));
    const auto authored = document(list);
    Pelican::CanvasLoweringOptions options;
    const auto edit = Pelican::lowerCanvasNodes(authored, options);
    options.policy = Pelican::CanvasLoweringPolicy::ship;
    const auto ship = Pelican::lowerCanvasNodes(authored, options);
    REQUIRE(edit.folded_constants == 0);
    // ellipse_fill: size.x size.y center.x center.y rotation r g b a -- all
    // but the centres are shared (center.x is an edge in one of them).
    REQUIRE(ship.folded_constants == 7);
    const auto fragment = [](const Pelican::CanvasLowering &lowering) {
        for (const auto &source : lowering.generated_sources)
            if (source.stage == "frag") return source.text;
        FAIL("no fragment shader");
        return std::string{};
    };
    REQUIRE(fragment(edit).find("canvasShapeColor(vec3(canvasParam(it, 5u), canvasParam(it, 6u), canvasParam(it, 7u)))") !=
            std::string::npos);
    REQUIRE(fragment(ship).find("canvasShapeColor(vec3(1.0, 0.5, 0.25))") != std::string::npos);
    REQUIRE(fragment(ship).find("canvasEllipse(local, half_size)") != std::string::npos);
    REQUIRE(fragment(ship).find("vec2 half_size = 0.5 * abs(vec2(6.0, 6.0));") != std::string::npos);
    // The centres stay in the tables, read as the kept parameters 0 and 1.
    REQUIRE(fragment(ship).find("vec2(canvasParam(it, 0u), canvasParam(it, 1u))") != std::string::npos);
    const auto bake = [](const Pelican::CanvasLowering &lowering) {
        return nodeById(lowering.document, "out$canvas_bake").at("parameters").at("initial_f32").size();
    };
    // Edit: 9 parameters per disc, and the moving one's 9 sources.
    REQUIRE(bake(edit) == 3 * 9 + 9);
    REQUIRE(bake(ship) == 3 * 2 + 2);
    // The moving centre still reaches its frame table.
    REQUIRE(nodeById(ship.document, "out$canvas_set_0").at("inputs").at("value") ==
            Json{{"node", "clock"}, {"port", "value"}});
    validate(ship.document);
    // The edit policy is the default.
    REQUIRE(Pelican::lowerCanvasNodes(authored).document == edit.document);
}

TEST_CASE("CV-5 the ship policy's parameters refuse what they do not take", "[canvas][cv5]") {
    auto authored = document(nodes({image("a", "project://a.png", {0, 0}, {4, 4}, {{"alpha", "opaque"}}),
                                    stack("root", Json::array({{{"input", "a"}}}), {{"size", {4, 4}}})}));
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(authored), std::invalid_argument);
    authored = document(nodes({image("a", "project://a.png", {0, 0}, {4, 4}),
                               stack("root", Json::array({{{"input", "a"}, {"bake", "image"}}}), {{"size", {4, 4}}})}));
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(authored), std::invalid_argument);
    // A mask is coverage, never a premultiplied colour.
    authored = document(nodes({image("a", "project://a.png", {0, 0}, {4, 4}),
                               image("m", "project://m.png", {0, 0}, {4, 4}, {{"alpha", "premultiplied"}}),
                               stack("root", Json::array({{{"input", "a"}, {"mask", "m"}}}), {{"size", {4, 4}}})}));
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(authored), std::invalid_argument);
}
