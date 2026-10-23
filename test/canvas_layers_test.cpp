// CV-2a: image, color and transform layers, masks, clips, groups and blend
// spaces in the pelican.canvas lowering and its CPU reference, without a GPU.
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

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

std::set<std::string> ids(const Json &document) {
    std::set<std::string> result;
    for (const auto &node : document.at("nodes")) result.insert(node.at("id").get<std::string>());
    return result;
}

Json image(const std::string &id, const std::string &asset, std::array<double, 2> offset, std::array<double, 2> size,
           Json extra = Json::object()) {
    Json parameters{{"asset", asset}, {"offset", {offset[0], offset[1]}}, {"size", {size[0], size[1]}}};
    parameters.update(extra);
    return {{"id", id}, {"op", "canvas.image"}, {"parameters", parameters}, {"outputs", Json::array({"layer"})}};
}

Json color(const std::string &id, std::array<double, 4> rgba) {
    return {{"id", id}, {"op", "canvas.color"}, {"parameters", {{"color", {rgba[0], rgba[1], rgba[2], rgba[3]}}}},
            {"outputs", Json::array({"layer"})}};
}

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

Json document(Json nodes, const std::string &top, Json output_parameters = Json::object()) {
    nodes.push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", output_parameters},
                     {"inputs", {{"layer", {{"node", top}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}});
    return {{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()}, {"nodes", nodes},
            {"outputs", Json::object()}};
}

template <typename Body> std::string refusal(Body body) {
    try {
        body();
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

// The row of item `instance` of an item table.
std::vector<double> row(const Json &document, const std::string &table, std::size_t instance) {
    const auto &values = nodeById(document, table).at("parameters").at("initial_f32");
    std::vector<double> result;
    for (std::size_t i = 0; i < 16; ++i) result.push_back(values.at(instance * 16 + i).get<double>());
    return result;
}

Pelican::CanvasOutput onlyOutput(const Json &document) {
    auto ir = Pelican::buildCanvasIr(document);
    REQUIRE(ir.outputs.size() == 1);
    return ir.outputs.front();
}

Pelican::CanvasReferenceImage solid(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 4> rgba) {
    Pelican::CanvasReferenceImage result{width, height, {}};
    for (std::uint32_t i = 0; i < width * height; ++i) result.rgba.insert(result.rgba.end(), rgba.begin(), rgba.end());
    return result;
}

const float *pixel(const Pelican::CanvasImage &image, std::uint32_t x, std::uint32_t y) {
    return &image.pixels[(std::size_t{y} * image.width + x) * 4];
}

double srgbToLinear(double c) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92; }
double linearToSrgb(double c) { return c > 0.0031308 ? 1.055 * std::pow(c, 1.0 / 2.4) - 0.055 : c * 12.92; }

bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }
} // namespace

TEST_CASE("CV-2a image and color layers lower to sampled ports of the raster", "[canvas][cv2a]") {
    const auto authored = document(
        Json::array({color("paper", {1.0, 1.0, 1.0, 1.0}),
                     image("logo", "project://assets/logo.png", {2, 2}, {8, 4}),
                     image("logo_again", "project://assets/logo.png", {4, 6}, {8, 4}, {{"sampler", "nearest"}}),
                     image("fade", "project://assets/fade.png", {0, 0}, {16, 8}, {{"channel", "luminance"}, {"outside", 0.25}}),
                     stack("root", Json::array({{{"input", "paper"}}, {{"input", "logo"}},
                                                {{"input", "logo_again"}, {"mask", "fade"}}}),
                           {{"size", {16, 8}}})}),
        "root");
    validate(authored);
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    const auto &lowered = lowering.document;
    validate(lowered);
    // One render.image per file and decode: the logo twice through the sRGB
    // decode, the mask's bytes as they are.
    REQUIRE(ids(lowered) == std::set<std::string>{"out$canvas_image", "out$canvas_items", "out$canvas_bake",
                                                   "out$canvas_asset0", "out$canvas_asset1", "out"});
    REQUIRE(nodeById(lowered, "out$canvas_asset0").at("parameters").at("source") ==
            Json{{"asset", "project://assets/logo.png"}, {"color_space", "srgb"}, {"alpha", "straight"}, {"mips", "none"}});
    REQUIRE(nodeById(lowered, "out$canvas_asset1").at("parameters").at("source").at("color_space") == "linear");
    const auto &raster = nodeById(lowered, "out");
    const auto &ports = raster.at("parameters").at("ports");
    // Slots by first use: the logo (linear filter), the logo (nearest), the mask.
    REQUIRE(ports.at("t0").at("sampling") == Json{{"filter", "linear"}, {"address", "clamp_to_edge"}});
    REQUIRE(ports.at("t1").at("sampling").at("filter") == "nearest");
    REQUIRE(raster.at("inputs").at("t0") == Json{{"node", "out$canvas_asset0"}, {"port", "resource"}});
    REQUIRE(raster.at("inputs").at("t1") == Json{{"node", "out$canvas_asset0"}, {"port", "resource"}});
    REQUIRE(raster.at("inputs").at("t2") == Json{{"node", "out$canvas_asset1"}, {"port", "resource"}});
    REQUIRE_FALSE(ports.contains("t3"));
    // Kinds 5 (color), 4 (image), 4; slots -1, 0, 1; the last one masked.
    REQUIRE(row(lowered, "out$canvas_items", 0)[0] == 5);
    REQUIRE(row(lowered, "out$canvas_items", 0)[14] == -1);
    REQUIRE(row(lowered, "out$canvas_items", 1)[0] == 4);
    REQUIRE(row(lowered, "out$canvas_items", 1)[14] == 0);
    REQUIRE(row(lowered, "out$canvas_items", 1)[6] == -1);
    const auto masked = row(lowered, "out$canvas_items", 2);
    REQUIRE(masked[14] == 1);
    REQUIRE(masked[7] == 1);
    const auto &bake = nodeById(lowered, "out$canvas_bake").at("parameters").at("initial_f32");
    const auto mask_at = static_cast<std::size_t>(masked[6]);
    // slot, luminance, outside, rectangle, then the identity map.
    REQUIRE(Json(std::vector<double>(bake.begin() + mask_at, bake.begin() + mask_at + 13)) ==
            Json(std::vector<double>{2, 1, 0.25, 0, 0, 16, 8, 1, 0, 0, 1, 0, 0}));
    REQUIRE(lowering.items.at(1) ==
            Json{{"output", "out"}, {"instance", 1}, {"kind", "image_rgba"}, {"nodes", {"logo", "root"}}});
    const auto &fragment = lowering.generated_sources.at(1).text;
    REQUIRE(fragment.find("pelican_sample_lod_t2") != std::string::npos);
    REQUIRE(fragment.find("canvasMask") != std::string::npos);
    REQUIRE(fragment.find("case 0u: { // rect_fill") == std::string::npos);
}

TEST_CASE("CV-2a a clip layer is cut by its base, and a base below full opacity is composited first",
          "[canvas][cv2a]") {
    const auto layers = [](Json base) {
        return Json::array({base, {{"input", "shine"}, {"clip", true}}, {{"input", "dot"}, {"clip", true}}});
    };
    const auto nodes = [&](Json base) {
        return Json::array({image("card", "project://card.png", {2, 2}, {4, 4}),
                            image("shine", "project://shine.png", {0, 0}, {8, 3}),
                            image("dot", "project://dot.png", {5, 5}, {2, 2}),
                            stack("root", layers(base), {{"size", {8, 8}}})});
    };
    SECTION("a base and its clip layers make a group whose second draw blends atop") {
        const auto lowering = Pelican::lowerCanvasNodes(document(nodes({{"input", "card"}}), "root"));
        const auto &lowered = lowering.document;
        validate(lowered);
        REQUIRE(nodeById(lowered, "out$canvas_g0_draw0_items").at("parameters").at("count") == 1);
        REQUIRE(nodeById(lowered, "out$canvas_g0_items").at("parameters").at("count") == 2);
        REQUIRE(row(lowered, "out$canvas_g0_draw0_items", 0)[15] == -1);
        REQUIRE(row(lowered, "out$canvas_g0_items", 0)[15] == 0);
        REQUIRE(row(lowered, "out$canvas_g0_items", 1)[15] == 0);
        const auto &base = nodeById(lowered, "out$canvas_g0_draw0");
        const auto &clips = nodeById(lowered, "out$canvas_g0");
        REQUIRE(base.at("parameters").at("raster_state").at("color_attachments").at(0).at("blend").at("color").at("src") ==
                "one");
        REQUIRE(clips.at("parameters").at("raster_state").at("color_attachments").at(0).at("blend") ==
                Json{{"color", {{"src", "dst_alpha"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}}},
                     {"alpha", {{"src", "zero"}, {"dst", "one"}, {"op", "add"}}}});
        REQUIRE(clips.at("inputs").at("image") == Json{{"node", "out$canvas_g0_draw0"}, {"port", "image"}});
        REQUIRE(row(lowered, "out$canvas_items", 0)[0] == 6);
        // A split for the clip layers is no image-limit split.
        REQUIRE(lowering.diagnostics.empty());
    }
    SECTION("a base at half opacity fades the group") {
        const auto lowering = Pelican::lowerCanvasNodes(document(nodes({{"input", "card"}, {"opacity", 0.5}}), "root"));
        REQUIRE(ids(lowering.document).contains("out$canvas_g0"));
        REQUIRE(ids(lowering.document).contains("out$canvas_g0_image"));
        REQUIRE(nodeById(lowering.document, "out$canvas_g0_items").at("parameters").at("count") == 2);
        const auto top = row(lowering.document, "out$canvas_items", 0);
        REQUIRE(top[0] == 6);
        REQUIRE(top[3] == 0.5);
        REQUIRE(nodeById(lowering.document, "out").at("inputs").at("t0") ==
                Json{{"node", "out$canvas_g0"}, {"port", "image"}});
        validate(lowering.document);
    }
    SECTION("a hidden base hides its clip layers") {
        const auto output = onlyOutput(document(nodes({{"input", "card"}, {"visible", false}}), "root"));
        REQUIRE(output.items.empty());
    }
    SECTION("a clip layer with nothing below it is refused") {
        auto authored = document(nodes({{"input", "card"}}), "root");
        auto &first = authored.at("nodes").at(3).at("parameters").at("layers").at(0);
        first["clip"] = true;
        first["name"] = "Card";
        REQUIRE(refusal([&] { (void)Pelican::lowerCanvasNodes(authored); }) ==
                R"(canvas.clip_base_missing@1 {"layer":"Card","node":"root"})");
    }
    SECTION("the reference cuts the clip by the base and fades the two as one") {
        Pelican::CanvasReferenceImages images{{"project://card.png", solid(4, 4, {255, 0, 0, 255})},
                                              {"project://shine.png", solid(8, 3, {0, 0, 255, 255})},
                                              {"project://dot.png", solid(2, 2, {0, 255, 0, 255})}};
        const auto draw = [&](Json base) {
            return Pelican::renderCanvasReference(onlyOutput(document(nodes(base), "root", {{"fit", "stretch"}})), 8, 8,
                                                  {}, images);
        };
        const auto opaque = draw({{"input", "card"}});
        // Outside the base nothing of the clip layers shows.
        REQUIRE(pixel(opaque, 0, 0)[3] == 0.0f);
        REQUIRE(pixel(opaque, 6, 6)[3] == 0.0f);
        // Inside it they cover the card.
        REQUIRE(pixel(opaque, 3, 2)[2] == 1.0f);
        REQUIRE(pixel(opaque, 3, 4)[0] == 1.0f);
        REQUIRE(pixel(opaque, 5, 5)[1] == 1.0f);
        // At half opacity the card and its shine fade as one: the shine,
        // opaque over the card, shows at half, with no red under it.
        const auto faded = draw({{"input", "card"}, {"opacity", 0.5}});
        REQUIRE(near(pixel(faded, 3, 2)[2], 0.5));
        REQUIRE(near(pixel(faded, 3, 2)[0], 0.0));
        REQUIRE(near(pixel(faded, 3, 2)[3], 0.5));
        // Over a half-covered base the opaque shine replaces the colour and
        // keeps the base's alpha (atop), with no red left under it.
        images["project://card.png"] = solid(4, 4, {255, 0, 0, 128});
        const auto half = draw({{"input", "card"}});
        const auto a = 128.0 / 255.0;
        REQUIRE(near(pixel(half, 3, 2)[3], a));
        REQUIRE(near(pixel(half, 3, 2)[2], a));
        REQUIRE(near(pixel(half, 3, 2)[0], 0.0));
    }
}

TEST_CASE("CV-2a groups: pass through, composited, and the blend space", "[canvas][cv2a]") {
    // Two overlapping half-transparent squares in a group at half opacity.
    const auto authored = [](Json group_layer, Json group_parameters = Json::object(),
                             Json root_parameters = Json::object()) {
        root_parameters["size"] = {4, 4};
        return document(
            Json::array({image("a", "project://a.png", {0, 0}, {3, 3}), image("b", "project://b.png", {1, 1}, {3, 3}),
                         stack("group", Json::array({{{"input", "a"}}, {{"input", "b"}}}), group_parameters),
                         stack("root", Json::array({group_layer}), root_parameters)}),
            "root", {{"fit", "stretch"}});
    };
    const Pelican::CanvasReferenceImages images{{"project://a.png", solid(3, 3, {255, 255, 255, 128})},
                                                {"project://b.png", solid(3, 3, {255, 255, 255, 128})}};
    const auto draw = [&](const Json &document) {
        return Pelican::renderCanvasReference(onlyOutput(document), 4, 4, {}, images);
    };
    const auto a = 128.0 / 255.0;
    SECTION("pass through multiplies the opacity into each layer") {
        const auto document = authored({{"input", "group"}, {"opacity", 0.5}, {"pass_through", true}});
        const auto output = onlyOutput(document);
        REQUIRE(output.items.size() == 2);
        REQUIRE(output.groups.empty());
        const auto image = draw(document);
        const auto each = a * 0.5;
        REQUIRE(near(pixel(image, 2, 2)[3], each + each * (1 - each)));
    }
    SECTION("a composited group takes its opacity as one picture") {
        const auto document = authored({{"input", "group"}, {"opacity", 0.5}});
        const auto output = onlyOutput(document);
        REQUIRE(output.items.size() == 1);
        REQUIRE(output.groups.size() == 1);
        REQUIRE(Pelican::canvasItemKind(output.items[0]) == Pelican::canvas_kind_group);
        const auto image = draw(document);
        REQUIRE(near(pixel(image, 2, 2)[3], (a + a * (1 - a)) * 0.5));
        // Different from the pass-through picture where the squares overlap.
        REQUIRE_FALSE(near(pixel(image, 2, 2)[3],
                           pixel(draw(authored({{"input", "group"}, {"opacity", 0.5}, {"pass_through", true}})), 2, 2)[3]));
        const auto lowering = Pelican::lowerCanvasNodes(document);
        REQUIRE(lowering.diagnostics.empty());
        validate(lowering.document);
    }
    SECTION("a group nothing is applied to is flattened, which draws the same") {
        const auto output = onlyOutput(authored({{"input", "group"}}));
        REQUIRE(output.items.size() == 2);
        REQUIRE(output.groups.empty());
    }
    SECTION("a blend space boundary composites even a pass-through group") {
        const auto document = authored({{"input", "group"}, {"pass_through", true}}, {{"blend_space", "srgb"}});
        const auto output = onlyOutput(document);
        REQUIRE(output.groups.size() == 1);
        REQUIRE(output.groups[0].srgb);
        // Read back into the linear output with the sRGB decode.
        REQUIRE(output.items[0].parameters.at(0).value == 2.0);
        // The sRGB stack reads its files' bytes as they are.
        const auto lowering = Pelican::lowerCanvasNodes(document);
        REQUIRE(nodeById(lowering.document, "out$canvas_asset0").at("parameters").at("source").at("color_space") ==
                "linear");
    }
    SECTION("srgb and linear differ by where the colours mix") {
        // Grey at half alpha over black.
        const auto mix = [&](const char *space) {
            const auto authored_mix = document(
                Json::array({color("black", {0, 0, 0, 1}), color("grey", {1, 1, 1, 0.5}),
                             stack("root", Json::array({{{"input", "black"}}, {{"input", "grey"}}}),
                                   {{"size", {2, 2}}, {"blend_space", space}})}),
                "root", {{"fit", "stretch"}});
            return pixel(Pelican::renderCanvasReference(onlyOutput(authored_mix), 2, 2), 0, 0)[0];
        };
        // Linear: half the light. sRGB: half the code value, decoded.
        REQUIRE(near(mix("linear"), 0.5, 1e-5));
        REQUIRE(near(mix("srgb"), srgbToLinear(0.5), 1e-5));
        REQUIRE(near(linearToSrgb(mix("srgb")), 0.5, 1e-5));
    }
}

TEST_CASE("CV-2a a mask reads its outside value past its rectangle", "[canvas][cv2a]") {
    const auto authored = document(
        Json::array({color("fill", {1, 1, 1, 1}),
                     image("hole", "project://m.png", {1, 1}, {2, 2}, {{"channel", "luminance"}, {"outside", 0.25}}),
                     stack("root", Json::array({{{"input", "fill"}, {"mask", "hole"}}}), {{"size", {4, 4}}})}),
        "root", {{"fit", "stretch"}});
    const Pelican::CanvasReferenceImages images{{"project://m.png", solid(2, 2, {128, 128, 128, 255})}};
    const auto image = Pelican::renderCanvasReference(onlyOutput(authored), 4, 4, {}, images);
    REQUIRE(near(pixel(image, 0, 0)[3], 0.25));
    REQUIRE(near(pixel(image, 3, 3)[3], 0.25));
    // Inside: the byte's luminance, not decoded.
    REQUIRE(near(pixel(image, 1, 1)[3], 128.0 / 255.0));
    REQUIRE(near(pixel(image, 2, 2)[3], 128.0 / 255.0));
}

TEST_CASE("CV-2a a transform moves a layer and its mask stays with the stack", "[canvas][cv2a]") {
    const auto authored = document(
        Json::array({image("dot", "project://dot.png", {0, 0}, {1, 1}),
                     {{"id", "moved"}, {"op", "canvas.transform"}, {"parameters", {{"translate", {2, 1}}}},
                      {"inputs", {{"layer", {{"node", "dot"}, {"port", "layer"}}}}}, {"outputs", Json::array({"layer"})}},
                     {{"id", "turned"}, {"op", "canvas.transform"},
                      {"parameters", {{"rotate", 90}, {"pivot", {2, 2}}}},
                      {"inputs", {{"layer", {{"node", "dot"}, {"port", "layer"}}}}}, {"outputs", Json::array({"layer"})}},
                     stack("root", Json::array({{{"input", "moved"}}, {{"input", "turned"}}}), {{"size", {4, 4}}})}),
        "root", {{"fit", "stretch"}});
    const auto output = onlyOutput(authored);
    REQUIRE(output.items.size() == 2);
    REQUIRE(output.items[0].affine == std::array<double, 6>{1, 0, 0, 1, 2, 1});
    const Pelican::CanvasReferenceImages images{{"project://dot.png", solid(1, 1, {255, 255, 255, 255})}};
    const auto image = Pelican::renderCanvasReference(output, 4, 4, {}, images);
    REQUIRE(pixel(image, 2, 1)[3] == 1.0f);
    REQUIRE(pixel(image, 0, 0)[3] == 0.0f);
    // (0.5, 0.5) about (2, 2) by 90 degrees lands at (3.5, 0.5).
    REQUIRE(near(pixel(image, 3, 0)[3], 1.0));
    REQUIRE(Pelican::lowerCanvasNodes(authored).changed);
    // A value edge into a transform waits for a later WP.
    auto driven = authored;
    driven.at("nodes").push_back({{"id", "k"}, {"op", "value.constant"}, {"parameters", {{"value", 1}}},
                                  {"inputs", Json::object()}, {"outputs", Json::array({"value"})}});
    driven.at("nodes").at(1)["inputs"]["rotate"] = {{"node", "k"}, {"port", "value"}};
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(driven), std::invalid_argument);
}

TEST_CASE("CV-2a a canvas definition is expanded by the lowering", "[canvas][cv2a]") {
    // The shape of psd-canvas's output: one definition whose output is a layer.
    Json authored{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()}};
    authored["definitions"]["psd_card"] = {
        {"nodes", Json::array({image("l0", "project://imports/card/layers/0.png", {0, 0}, {4, 4}),
                               stack("root", Json::array({{{"input", "l0"}}}), {{"size", {4, 4}}, {"blend_space", "srgb"}})})},
        {"outputs", {{"layer", {{"node", "root"}, {"port", "layer"}}}}}};
    authored["nodes"] = Json::array(
        {{{"id", "card"}, {"op", "graph.instance"}, {"definition", "psd_card"}},
         {{"id", "out"}, {"op", "canvas.output"}, {"parameters", {{"size", {4u, 4u}}}},
          {"inputs", {{"layer", {{"node", "card"}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}}});
    validate(authored);
    const auto expanded = Pelican::expandCanvasInstances(authored);
    REQUIRE(ids(expanded) == std::set<std::string>{"card$l0", "card$root", "out"});
    REQUIRE(nodeById(expanded, "out").at("inputs").at("layer") == Json{{"node", "card$root"}, {"port", "layer"}});
    REQUIRE_FALSE(expanded.at("definitions").contains("psd_card"));
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    validate(lowering.document);
    REQUIRE_FALSE(lowering.document.at("definitions").contains("psd_card"));
    // The sRGB root is a group read into the linear output.
    REQUIRE(lowering.items.at(0) == Json{{"output", "out"}, {"instance", 0}, {"kind", "image_rgba"},
                                         {"nodes", {"card$l0", "card$root"}}, {"group", 0}});
    REQUIRE(lowering.items.at(1).at("kind") == "group");

    auto with_input = authored;
    with_input["definitions"]["psd_card"]["nodes"].push_back({{"id", "x"}, {"op", "graph.input"}});
    REQUIRE(refusal([&] { (void)Pelican::lowerCanvasNodes(with_input); }) ==
            R"(canvas.instance_inputs_unsupported@1 {"definition":"psd_card","node":"card"})");
}

TEST_CASE("CV-2a past the image limit the items are split across draws of one image", "[canvas][cv2a]") {
    auto nodes = Json::array();
    auto layers = Json::array();
    for (int i = 0; i < 20; ++i) {
        const auto id = "i" + std::to_string(i);
        nodes.push_back(image(id, "project://img/" + std::to_string(i) + ".png", {0, 0}, {2, 2}));
        layers.push_back({{"input", id}});
    }
    nodes.push_back(stack("root", layers, {{"size", {4, 4}}}));
    const auto lowering = Pelican::lowerCanvasNodes(document(nodes, "root"));
    const auto &lowered = lowering.document;
    validate(lowered);
    REQUIRE(lowering.diagnostics ==
            Json::array({{{"code", "canvas.draw_split"}, {"output", "out"}, {"reason", "image_limit"}, {"count", 2}}}));
    REQUIRE(nodeById(lowered, "out$canvas_draw0_items").at("parameters").at("count") == 16);
    REQUIRE(nodeById(lowered, "out$canvas_items").at("parameters").at("count") == 4);
    const auto &first = nodeById(lowered, "out$canvas_draw0");
    REQUIRE(first.at("inputs").at("image") == Json{{"node", "out$canvas_image"}, {"port", "resource"}});
    REQUIRE(first.at("parameters").contains("clear_color"));
    REQUIRE(first.at("parameters").at("ports").contains("t15"));
    // The second draw keeps the first's picture and the output's name.
    const auto &last = nodeById(lowered, "out");
    REQUIRE(last.at("inputs").at("image") == Json{{"node", "out$canvas_draw0"}, {"port", "image"}});
    REQUIRE(last.at("parameters").at("color_load_op") == "load");
    REQUIRE_FALSE(last.at("parameters").contains("clear_color"));
    REQUIRE(lowering.items.at(16).at("draw") == 1);
    REQUIRE(lowering.items.at(16).at("instance") == 0);
    REQUIRE(Pelican::canvas_draw_image_limit == 16);
}

TEST_CASE("CV-2a canvas lowering refuses by name", "[canvas][cv2a]") {
    const auto base = [] {
        return document(Json::array({image("a", "project://a.png", {0, 0}, {2, 2}),
                                     color("c", {1, 1, 1, 1}),
                                     stack("root", Json::array({{{"input", "a"}, {"name", "A"}}}), {{"size", {4, 4}}})}),
                        "root");
    };
    REQUIRE(refusal([&] {
                auto authored = base();
                // CV-3a draws filter.blur; a filter it does not know stays refused.
                authored.at("nodes").push_back({{"id", "bd"}, {"op", "filter.radial_blur"}, {"outputs", Json::array({"layer"})}});
                (void)Pelican::lowerCanvasNodes(authored);
            }) == R"(canvas.op_unsupported@1 {"node":"bd","op":"filter.radial_blur"})");
    REQUIRE(refusal([&] {
                auto authored = base();
                authored.at("nodes").push_back({{"id", "inv"}, {"op", "filter.unsharp_mask"}, {"outputs", Json::array({"layer"})}});
                (void)Pelican::lowerCanvasNodes(authored);
            }) == R"(canvas.op_unsupported@1 {"node":"inv","op":"filter.unsharp_mask"})");
    REQUIRE(refusal([&] {
                auto authored = base();
                auto &root = authored.at("nodes").at(2);
                root.at("parameters").at("layers").at(0)["mask"] = "c";
                root.at("inputs")["c"] = {{"node", "c"}, {"port", "layer"}};
                (void)Pelican::lowerCanvasNodes(authored);
            }) == R"(canvas.mask_unsupported@1 {"layer":"A","node":"root","op":"canvas.color"})");
    REQUIRE(refusal([&] {
                auto authored = base();
                authored.at("nodes").at(2).at("parameters").at("layers").at(0)["blend"] = "dissolve";
                (void)Pelican::lowerCanvasNodes(authored);
            }) == R"(canvas.blend_unsupported@1 {"blend":"dissolve","layer":"A","node":"root"})");
    // A luminance image is a mask, not a layer; pass_through belongs to a stack.
    auto luminance = base();
    luminance.at("nodes").at(0).at("parameters")["channel"] = "luminance";
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(luminance), std::invalid_argument);
    auto through = base();
    through.at("nodes").at(2).at("parameters").at("layers").at(0)["pass_through"] = true;
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(through), std::invalid_argument);
}
