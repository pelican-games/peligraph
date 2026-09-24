// GP-3: writing the UI by (document, path), the commit boundary, the gauge
// bar, and the pelican.ui graph sinks, measured without a GPU. The headless
// player run (run_gp3_ui_headless.cmake) covers the same surface through
// ctx.ui() and the render config's preparation.
#include "ui/module.hpp"
#include "userpublic/gamecontext.hpp"
#include "userpublic/valuegraph.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace Pelican;
using namespace Pelican::ui;
using Json = nlohmann::json;

namespace {

// Two parents, each with a child whose id is "name": the ids collide, the
// paths do not.
Json twinDocument() {
    return Json::parse(R"json({
      "schema":"pelican.ui","version":1,"key":"gp3_twins",
      "root":{"id":"root","type":"panel","children":[
        {"id":"left","type":"panel",
         "layout":{"x":{"mode":"fixed","value":80},"y":{"mode":"fixed","value":40},"offsets":[0,0,0,0]},
         "children":[{"id":"name","type":"label","text":"A",
                      "layout":{"x":{"mode":"content"},"y":{"mode":"content"}}}]},
        {"id":"right","type":"panel",
         "layout":{"x":{"mode":"fixed","value":80},"y":{"mode":"fixed","value":40},"offsets":[80,0,0,0]},
         "children":[{"id":"name","type":"label","text":"B",
                      "layout":{"x":{"mode":"content"},"y":{"mode":"content"}}}]},
        {"id":"bar","type":"gauge","value":0.25,
         "layout":{"x":{"mode":"fixed","value":100},"y":{"mode":"fixed","value":6},"offsets":[0,50,0,0]}},
        {"id":"empty","type":"label",
         "layout":{"x":{"mode":"content"},"y":{"mode":"content"},"offsets":[0,70,0,0]}}
      ]}
    })json");
}

std::size_t quadsOf(const DrawBatch &batch, std::string_view path) {
    return static_cast<std::size_t>(std::count_if(batch.quads.begin(), batch.quads.end(),
        [&](const QuadCommand &quad) { return quad.widget_id == path; }));
}

const QuadCommand &onlyQuadOf(const DrawBatch &batch, std::string_view path) {
    const auto found = std::find_if(batch.quads.begin(), batch.quads.end(),
        [&](const QuadCommand &quad) { return quad.widget_id == path; });
    REQUIRE(found != batch.quads.end());
    REQUIRE(quadsOf(batch, path) == 1);
    return *found;
}

const DocumentNode *nodeAt(const DocumentNode &node, std::string_view path) {
    if (node.path == path) return &node;
    for (const auto &child : node.children)
        if (const auto *found = nodeAt(child, path)) return found;
    return nullptr;
}

UiWrite text(std::string value) { return {.property = UiProperty::text, .text = std::move(value)}; }
UiWrite value(double v) { return {.property = UiProperty::value, .value = v}; }
UiWrite visible(bool v) { return {.property = UiProperty::visible, .visible = v}; }

constexpr vk::Extent2D extent{200, 100};

} // namespace

TEST_CASE("GP-3 a path tells apart two nodes that share an id under different parents", "[ui][gp3]") {
    UiModule module{twinDocument()};
    const auto before = module.buildFrame(extent);
    REQUIRE(quadsOf(before, "root/left/name") == 1);
    REQUIRE(quadsOf(before, "root/right/name") == 1);

    REQUIRE(module.queueWrite({}, "root/left/name", text("LEFT")) == UiWriteStatus::ok);
    REQUIRE(module.queueWrite({}, "root/right/name", text("RIGHT!")) == UiWriteStatus::ok);
    module.commitFrameWrites();
    const auto after = module.buildFrame(extent);
    // One glyph quad per character, drawn for the node the path named.
    REQUIRE(quadsOf(after, "root/left/name") == 4);
    REQUIRE(quadsOf(after, "root/right/name") == 6);
    REQUIRE(nodeAt(module.uiDocument().root, "root/left/name")->text == "LEFT");
    REQUIRE(nodeAt(module.uiDocument().root, "root/right/name")->text == "RIGHT!");
    // The bare id names no node: the id is unique only among siblings.
    REQUIRE(module.queueWrite({}, "name", text("X")) == UiWriteStatus::node_unknown);
}

TEST_CASE("GP-3 a write is checked when made and lands only at the commit boundary", "[ui][gp3]") {
    UiModule module{twinDocument()};
    REQUIRE(module.queueWrite({}, "root/left/name", text("LATER")) == UiWriteStatus::ok);
    REQUIRE(module.queueWrite({}, "root/bar", value(1.0)) == UiWriteStatus::ok);
    REQUIRE(module.queueWrite({}, "root/right/name", visible(false)) == UiWriteStatus::ok);
    // Nothing moved before the boundary.
    const auto queued = module.buildFrame(extent);
    REQUIRE(quadsOf(queued, "root/left/name") == 1);
    REQUIRE(quadsOf(queued, "root/right/name") == 1);
    REQUIRE(onlyQuadOf(queued, "root/bar").rect_px.right - onlyQuadOf(queued, "root/bar").rect_px.left == 25);
    const auto status = module.commitFrameWrites();
    REQUIRE(status.applied == 3);
    const auto landed = module.buildFrame(extent);
    REQUIRE(quadsOf(landed, "root/left/name") == 5);
    REQUIRE(quadsOf(landed, "root/right/name") == 0);
    REQUIRE(onlyQuadOf(landed, "root/bar").rect_px.right - onlyQuadOf(landed, "root/bar").rect_px.left == 100);
}

TEST_CASE("GP-3 refused writes name their reason and queue nothing", "[ui][gp3]") {
    UiModule module{twinDocument()};
    // The document key: empty is the project's document, its own key names it
    // too, any other key names no node.
    REQUIRE(module.queueWrite("gp3_twins", "root/left/name", text("K")) == UiWriteStatus::ok);
    REQUIRE(module.queueWrite("other", "root/left/name", text("X")) == UiWriteStatus::node_unknown);
    REQUIRE(module.queueWrite({}, "root/missing", text("X")) == UiWriteStatus::node_unknown);
    // Text belongs to label and button, value to gauge, visibility to all.
    REQUIRE(module.queueWrite({}, "root/bar", text("X")) == UiWriteStatus::property_mismatch);
    REQUIRE(module.queueWrite({}, "root/left/name", value(0.5)) == UiWriteStatus::property_mismatch);
    REQUIRE(module.queueWrite({}, "root/left", text("X")) == UiWriteStatus::property_mismatch);
    REQUIRE(module.queueWrite({}, "root/bar", value(std::numeric_limits<double>::quiet_NaN())) ==
            UiWriteStatus::property_mismatch);
    REQUIRE(module.queueWrite({}, "root/left", visible(false)) == UiWriteStatus::ok);
    // A sink owns (path, property): the same property is refused, another one
    // of the same node is not.
    const UiSinkClaims claims = [](std::string_view path, UiProperty property) {
        return path == "root/bar" && property == UiProperty::value;
    };
    REQUIRE(module.queueWrite({}, "root/bar", value(0.5), claims) == UiWriteStatus::sink_conflict);
    REQUIRE(module.queueWrite({}, "root/bar", visible(false), claims) == UiWriteStatus::ok);
    // Three accepted writes; the refused ones queued nothing.
    REQUIRE(module.commitFrameWrites().applied == 3);
}

TEST_CASE("GP-3 a gauge draws a bar filling its value's share of the width", "[ui][gp3][gauge]") {
    UiModule module{twinDocument()};
    const auto quarter = module.buildFrame(extent);
    const auto &bar = onlyQuadOf(quarter, "root/bar");
    REQUIRE(bar.rect_px == RectI{0, 50, 25, 56});
    REQUIRE(bar.color == std::array<std::uint8_t, 4>{96, 200, 120, 255});
    for (const auto &[written, width] : std::vector<std::pair<double, std::int32_t>>{
             {0.5, 50}, {1.5, 100}, {-2.0, 0}, {0.999, 99}}) {
        REQUIRE(module.queueWrite({}, "root/bar", value(written)) == UiWriteStatus::ok);
        module.commitFrameWrites();
        const auto batch = module.buildFrame(extent);
        if (width == 0) REQUIRE(quadsOf(batch, "root/bar") == 0);
        else REQUIRE(onlyQuadOf(batch, "root/bar").rect_px.right == width);
    }
}

TEST_CASE("GP-3 a label with no text yet can be given some", "[ui][gp3]") {
    UiModule module{twinDocument()};
    // The font page is registered for every node that can show text.
    REQUIRE(std::any_of(module.pages().begin(), module.pages().end(),
                        [](const UiTexturePage &page) { return page.stable_name == "engine:debug_text_font"; }));
    REQUIRE(quadsOf(module.buildFrame(extent), "root/empty") == 0);
    REQUIRE(module.queueWrite({}, "root/empty", text("OK")) == UiWriteStatus::ok);
    module.commitFrameWrites();
    const auto batch = module.buildFrame(extent);
    REQUIRE(quadsOf(batch, "root/empty") == 2);
    // The content-sized label measured its new text.
    REQUIRE(nodeAt(module.uiDocument().root, "root/empty")->intrinsic_size ==
            measureTextIntrinsic("label", "OK"));
}

namespace {

Json sinkGraph(Json sinks) {
    Json nodes = Json::array({
        Json{{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "time"}}},
             {"inputs", Json::object()}, {"outputs", Json::array({"value"})}},
        Json{{"id", "fill"}, {"op", "value.curve"},
             {"parameters", {{"keys", Json::array({Json{{"time", 0}, {"value", 0}},
                                                   Json{{"time", 1}, {"value", 1}}})}}},
             {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}},
             {"outputs", Json::array({"value"})}}});
    for (auto &sink : sinks) nodes.push_back(std::move(sink));
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
                {"nodes", std::move(nodes)}, {"outputs", Json::object()}};
}

Json sink(std::string id, std::string op, std::string path) {
    return Json{{"id", std::move(id)}, {"op", std::move(op)}, {"parameters", {{"node", std::move(path)}}},
                {"inputs", {{"value", {{"node", "fill"}, {"port", "value"}}}}},
                {"outputs", Json::array()}};
}

} // namespace

TEST_CASE("GP-3 pelican.ui registers ui.value and ui.visible as sinks", "[ui][gp3][graph]") {
    const auto &registry = builtinGraphModuleRegistry();
    REQUIRE(registry.isSink("ui.value"));
    REQUIRE(registry.isSink("ui.visible"));
    const auto catalog = registry.catalog();
    const auto module = std::find_if(catalog.at("modules").begin(), catalog.at("modules").end(),
        [](const auto &entry) { return entry.at("namespace") == "pelican.ui"; });
    REQUIRE(module != catalog.at("modules").end());
    REQUIRE(module->at("sinks") == nlohmann::ordered_json::array({"ui.value", "ui.visible"}));
}

TEST_CASE("GP-3 two sinks writing one (path, property) are ui.sink_conflict@1 when the graph compiles",
          "[ui][gp3][graph]") {
    try {
        ValueGraph::Graph graph{sinkGraph(Json::array({sink("a", "ui.value", "root/bar"),
                                                        sink("b", "ui.value", "root/bar")})).dump()};
        FAIL("two ui.value sinks on one node compiled");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "ui.sink_conflict@1");
        REQUIRE(error.detail().at("path") == "root/bar");
        REQUIRE(error.detail().at("property") == "value");
        REQUIRE(error.detail().at("sinks") == Json::array({"a", "b"}));
    }
    // The unit is the property: the value and the visibility of one node are
    // two writers' worth.
    const ValueGraph::Graph split{sinkGraph(Json::array({sink("a", "ui.value", "root/bar"),
                                                          sink("b", "ui.visible", "root/bar")})).dump()};
    REQUIRE(split.uiSinkBindings().size() == 2);
    REQUIRE(split.uiSinkBindings()[0].path == "root/bar");
    REQUIRE(split.uiSinkBindings()[0].property == ValueGraph::UiSinkProperty::value);
    REQUIRE(split.uiSinkBindings()[1].property == ValueGraph::UiSinkProperty::visible);
}

TEST_CASE("GP-3 value.curve feeds ui.value through publish into the gauge", "[ui][gp3][graph]") {
    const ValueGraph::Graph graph{sinkGraph(Json::array({sink("meter", "ui.value", "root/bar")})).dump()};
    UiModule module{twinDocument()};
    const ValueGraph::UiValueWriter writer = [&](std::string_view path, ValueGraph::UiSinkProperty property,
                                                 float written) {
        return module.applySinkValue(path, property == ValueGraph::UiSinkProperty::value
            ? UiProperty::value : UiProperty::visible, written);
    };
    std::vector<std::int32_t> widths;
    for (const double time : {0.25, 0.75}) {
        auto state = graph.createState();
        const auto published = graph.publish(GameContext{}, {{"time", time}}, {}, {}, {}, {}, &state,
                                             {}, {}, {}, writer);
        REQUIRE(published.unavailable.empty());
        REQUIRE(published.applied == 1);
        widths.push_back(onlyQuadOf(module.buildFrame(extent), "root/bar").rect_px.right);
    }
    REQUIRE(widths == std::vector<std::int32_t>{25, 75});
    // Without a writer the sink is reported, not dropped.
    auto state = graph.createState();
    const auto unwritten = graph.publish(GameContext{}, {{"time", 0.5}}, {}, {}, {}, {}, &state);
    REQUIRE(unwritten.unavailable == std::vector<std::string>{"ui_value:root/bar"});
}
