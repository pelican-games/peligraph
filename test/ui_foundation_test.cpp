#include "ui/commandbuffer.hpp"
#include "ui/atlas.hpp"
#include "ui/document.hpp"
#include "ui/drawcommands.hpp"
#include "ui/inputrouter.hpp"
#include "ui/layout.hpp"
#include "ui/semanticfixture.hpp"
#include "ui/types.hpp"
#include "ui/widgetarena.hpp"
#include "ui/module.hpp"
#include "renderer/uicontainer.hpp"
#include "renderer/uirenderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

using namespace Pelican;
using namespace Pelican::ui;
using Json = nlohmann::json;

namespace {
Json readJson(const std::filesystem::path &path) {
    std::ifstream stream(path);
    REQUIRE(stream.good());
    return Json::parse(stream);
}
const std::filesystem::path root{PELICAN_TEST_SOURCE_DIR};
}

TEST_CASE("UI binary64 half-up vectors and viewport edge rounding are normative", "[ui][layout]") {
    REQUIRE(anchorEdge(0, 1, -1.5) == -1);
    REQUIRE(anchorEdge(0, 1, -0.5) == 0);
    REQUIRE(anchorEdge(0, 1, 0.5) == 1);
    REQUIRE(anchorEdge(0, 1, 1.5) == 2);
    REQUIRE(anchorEdge(0, 101, 0.5) == 51);
    REQUIRE(anchorEdge(0, 1, std::nextafter(0.5, 0.0)) == 1);
    REQUIRE(anchorEdge(0, 1, -std::nextafter(0.5, 0.0)) == 0);
    REQUIRE(anchorEdge(INT64_C(1) << 26, 1, 0.5) == (INT64_C(1) << 26) + 1);

    const auto viewport = ViewportTransform::letterboxed({152, 101}, 1.5);
    REQUIRE(viewport.content_rect_ui == RectI{0, 0, 101, 67});
    REQUIRE(viewport.uiToPx({0, 0, 100, 66}) == RectI{0, 0, 150, 99});

    const auto saved = std::fegetround();
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    REQUIRE(std::fegetround() == FE_DOWNWARD); // the production function has a debug assertion for this state
    REQUIRE(std::fesetround(saved) == 0);
}

TEST_CASE("Widget arena rejects stale generations and command commit is frame-batched", "[ui][arena]") {
    WidgetArena arena;
    const auto first = arena.create({.stable_id = "root", .type = "panel"});
    UiCommandBuffer commands;
    commands.push(SetVisibility{first, false});
    REQUIRE(arena.resolve(first)->visible);
    const auto status = commands.commit(arena);
    REQUIRE(status.applied == 1);
    REQUIRE_FALSE(arena.resolve(first)->visible);
    REQUIRE(arena.erase(first));
    const auto second = arena.create({.stable_id = "root2", .type = "panel"});
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);
    commands.push(SetEnabled{first, false});
    REQUIRE(commands.commit(arena).stale == 1);
}

TEST_CASE("Draw commands are stably sorted and only adjacent equal keys merge", "[ui][draw]") {
    DrawKey a{"rgba_straight", Sampler::Nearest, "white", {0, 0, 100, 100}};
    DrawKey b{"rgba_straight", Sampler::Linear, "white", {0, 0, 100, 100}};
    auto batch = buildDrawBatch({{b, {}, 1, 2}, {a, {}, 0, 0}, {a, {}, 1, 1}, {b, {}, 1, 3}});
    REQUIRE(batch.total_index_count == 24);
    REQUIRE(batch.runs.size() == 2);
    REQUIRE(batch.runs[0].first_index == 0);
    REQUIRE(batch.runs[0].index_count == 12);
    REQUIRE(batch.runs[1].first_index == 12);
    REQUIRE(batch.runs[1].index_count == 12);
}

TEST_CASE("U1 QuadVertex ABI, indexed expansion, alternating pages, clips, and limits are normative",
          "[ui][u1][draw]") {
    STATIC_REQUIRE(sizeof(QuadVertex) == 20);
    STATIC_REQUIRE(alignof(QuadVertex) == 4);
    STATIC_REQUIRE(offsetof(QuadVertex, position) == 0);
    STATIC_REQUIRE(offsetof(QuadVertex, uv) == 8);
    STATIC_REQUIRE(offsetof(QuadVertex, color) == 16);

    DrawKey a{"rgba_straight", Sampler::Nearest, "atlas:a/page:0", {1, 2, 30, 40}, 1, 4};
    DrawKey b{"rgba_straight", Sampler::Linear, "atlas:b/page:0", {1, 2, 30, 40}, 2, 4};
    QuadCommand first{a, {10, 20, 30, 40}, 0, 0, {0.1f, 0.2f, 0.3f, 0.4f}, {1, 2, 3, 4}, "root/a"};
    const auto batch = buildDrawBatch({first, {b, {30, 20, 50, 40}, 0, 1},
                                       {a, {50, 20, 70, 40}, 0, 2}}, "u1");
    REQUIRE(batch.runs.size() == 3); // A/B/A never merges across painter order.
    REQUIRE(batch.vertices.size() == 12);
    REQUIRE(batch.indices == std::vector<std::uint16_t>{0,1,2,0,2,3,4,5,6,4,6,7,8,9,10,8,10,11});
    REQUIRE(batch.vertices[0].position == std::array<float, 2>{10.0f, 20.0f});
    REQUIRE(batch.vertices[0].uv == std::array<float, 2>{0.1f, 0.2f});
    REQUIRE(batch.vertices[0].color == std::array<std::uint8_t, 4>{1, 2, 3, 4});

    std::vector<QuadCommand> too_many_clips;
    for (std::uint16_t i = 0; i <= maxUniqueClips; ++i) {
        DrawKey key{"rgba_straight", Sampler::Nearest, "white", {0, 0, 100, 100}, 0, i};
        too_many_clips.push_back({std::move(key), {0, 0, 1, 1}, 0, i});
    }
    REQUIRE_THROWS_WITH(buildDrawBatch(std::move(too_many_clips), "limits"),
                        Catch::Matchers::ContainsSubstring("limit_exceeded") &&
                        Catch::Matchers::ContainsSubstring("257 unique clips"));
}

TEST_CASE("pelican.atlas v1 resolves right-exclusive sprites and rejects invalid bounds", "[ui][u1][atlas]") {
    const auto source = root / "test/fixtures/ui_atlas/atlas.json";
    const auto json = Json::parse(R"json({
      "schema":"pelican.atlas","version":1,
      "pages":[{"image":"page.png","size":[16,8]}],
      "sprites":{"button":{"page":0,"rect":[2,1,14,7]}}
    })json");
    const auto atlas = parseAtlasV1(json, source);
    REQUIRE(atlas.pages[0].image_path == source.parent_path() / "page.png");
    REQUIRE(findAtlasSprite(atlas, "button").rect == RectI{2, 1, 14, 7});
    auto invalid = json;
    invalid["sprites"]["button"]["rect"] = {2, 1, 17, 7};
    REQUIRE_THROWS_WITH(parseAtlasV1(invalid, source), Catch::Matchers::ContainsSubstring("outside its page"));
}

TEST_CASE("U1 nested overflow clips become integer scissor ids", "[ui][u1][clip]") {
    const auto json = Json::parse(R"json({
      "schema":"pelican.ui","version":1,"key":"clip_u1",
      "root":{"id":"root","type":"panel","children":[
        {"id":"outer","type":"panel","color":[255,0,0,255],"overflow":"clip",
         "layout":{"x":{"mode":"fixed","value":80},"y":{"mode":"fixed","value":80},"offsets":[10,10,0,0]},"children":[
          {"id":"inner","type":"panel","color":[0,255,0,255],
           "layout":{"x":{"mode":"fixed","value":105},"y":{"mode":"fixed","value":105},"offsets":[-5,-5,0,0]}}
        ]}
      ]}
    })json");
    UiModule module{json};
    const auto batch = module.buildFrame({100, 100});
    REQUIRE(batch.quads.size() == 2);
    REQUIRE(batch.quads[0].key.scissor_px == RectI{0, 0, 100, 100});
    REQUIRE(batch.quads[1].key.scissor_px == RectI{10, 10, 90, 90});
    REQUIRE(batch.quads[0].key.clip_id != batch.quads[1].key.clip_id);
}

TEST_CASE("UI runtime and GPU modules remain wholly absent unless requested", "[ui][u1][purge]") {
    REQUIRE_FALSE(FastModuleContainer::isInitialized<UiModule>());
    REQUIRE_FALSE(FastModuleContainer::isInitialized<UIContainer>());
    REQUIRE_FALSE(FastModuleContainer::isInitialized<UiRenderer>());
}

TEST_CASE("U1 migration rejects the legacy images overlay instead of accepting two formats", "[ui][u1][document]") {
    const auto legacy = parseUiDocument(Json::parse(R"json({
      "images":[{"name":"ui_test","file":"assets/textures/Frame84.png"}]
    })json"));
    REQUIRE_FALSE(legacy);
    REQUIRE(std::any_of(legacy.errors.begin(), legacy.errors.end(), [](const UiError &error) {
        return error.path == "/schema" || error.path == "/images";
    }));
}

TEST_CASE("Document parser validates emit descriptors and stack layout distributes integer remainder", "[ui][document]") {
    static const std::array fields{
        PayloadFieldSchema{"source", PayloadFieldType::String, std::monostate{}},
        PayloadFieldSchema{"delta", PayloadFieldType::Vec2, std::monostate{}},
    };
    static const EventPayloadSchema schema{fields, true};
    const auto lookup = [](std::string_view name) {
        return name == "MenuOpened" ? EventSchemaLookup{EventSchemaLookup::State::Typed, &schema}
                                    : EventSchemaLookup{};
    };
    const Json document = Json::parse(R"json({
      "schema":"pelican.ui","version":1,"key":"hud",
      "root":{"id":"root","type":"stack","layout":{"stack":"horizontal"},"children":[
        {"id":"fixed","type":"button","layout":{"x":{"mode":"fixed","value":20},"y":{"mode":"fixed","value":10}},
         "emit":{"on_click":{"event":"MenuOpened","fields":[
           {"name":"source","from":"stable_id"},{"name":"delta","from":"drag_delta_ui"}
         ]}}},
        {"id":"fill","type":"panel","layout":{"x":{"mode":"fill","min":10},"y":{"mode":"fixed","value":10}}}
      ]}
    })json");
    const auto parsed = parseUiDocument(document, lookup);
    if (!parsed.errors.empty()) INFO(parsed.errors.front().message);
    REQUIRE(parsed);
    const auto layout = solveLayout(*parsed.document, {0, 0, 101, 20});
    REQUIRE(layout);
    REQUIRE(layout.widgets.size() == 3);
    REQUIRE(layout.widgets[1].rect_ui == RectI{0, 0, 20, 10});
    REQUIRE(layout.widgets[2].rect_ui == RectI{20, 0, 101, 10});
}

TEST_CASE("Document emit error fixture covers all EventPayloadSchema lookup failures", "[ui][document]") {
    static const std::array fields{
        PayloadFieldSchema{"source", PayloadFieldType::String, std::monostate{}},
        PayloadFieldSchema{"amount", PayloadFieldType::I32, std::pair<std::int64_t, std::int64_t>{0, 9}},
    };
    static const EventPayloadSchema typed_schema{fields, true};
    const auto lookup = [](std::string_view name) {
        if (name == "Typed") return EventSchemaLookup{EventSchemaLookup::State::Typed, &typed_schema};
        if (name == "Payloadless") return EventSchemaLookup{EventSchemaLookup::State::Payloadless, nullptr};
        return EventSchemaLookup{};
    };
    const auto fixture = readJson(root / "test/fixtures/ui_semantic/document_emit_errors.json");
    for (const auto &test_case : fixture["cases"]) {
        CAPTURE(test_case["name"]);
        const auto parsed = parseUiDocument(test_case["input"], lookup);
        REQUIRE_FALSE(parsed.errors.empty());
        const auto &expected = test_case["error"];
        const auto found = std::ranges::any_of(parsed.errors, [&](const UiError &value) {
            return toString(value.phase) == expected["phase"].get<std::string>() &&
                   toString(value.code) == expected["code"].get<std::string>() &&
                   value.path == expected["path"].get<std::string>();
        });
        REQUIRE(found);
    }
}

TEST_CASE("Ordered router keeps per-pointer captures independent", "[ui][input]") {
    WidgetArena arena;
    const auto a = arena.create({.stable_id="root/a", .type="button", .rect_ui={0,0,40,40}, .clip_ui={0,0,100,100}, .decl_seq=1});
    const auto b = arena.create({.stable_id="root/b", .type="button", .rect_ui={50,0,90,40}, .clip_ui={0,0,100,100}, .decl_seq=2});
    const std::array traversal{a, b};
    const auto viewport = ViewportTransform::letterboxed({100, 100}, 1);
    const std::array events{
        UiPointerEvent{1, PointerKind::Down, 0, PointerButton::Left, {10,10}},
        UiPointerEvent{2, PointerKind::Down, 1, PointerButton::Right, {60,10}},
        UiPointerEvent{3, PointerKind::Cancel, 1, std::nullopt, {60,10}},
        UiPointerEvent{4, PointerKind::Up, 0, PointerButton::Left, {10,10}},
    };
    InputRouter router;
    const auto routed = router.route(events, arena, traversal, viewport);
    REQUIRE(routed.events[0].effects == std::vector{PointerEffect::Capture});
    REQUIRE(routed.events[1].effects == std::vector{PointerEffect::Capture});
    REQUIRE(routed.events[2].effects == std::vector{PointerEffect::Cancel, PointerEffect::ReleaseCapture});
    REQUIRE(routed.events[3].effects == std::vector{PointerEffect::ReleaseCapture, PointerEffect::Click});
    REQUIRE(routed.events[3].target == a);
}

TEST_CASE("Semantic fixture three-stage gate executes every coverage witness", "[ui][semantic]") {
    const auto gate = runSemanticCoverageGate(root);
    if (!gate.failures.empty()) INFO(gate.failures.front());
    REQUIRE(gate.failures.empty());
    REQUIRE(gate.checked_entries >= 99);
    REQUIRE(gate.checked_fixtures >= 40);

    const auto expected = readJson(root / "test/fixtures/ui_semantic/normative/valid/viewport_rounding.json");
    Json actual = expected;
    REQUIRE(semanticFixtureEqual(actual, expected));
    REQUIRE(validateSemanticFixtureAll(actual).semanticValid());
    REQUIRE(Json::parse(writeSemanticFixture(actual)) == expected);
}
