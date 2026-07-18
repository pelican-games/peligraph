#include "../src/core/communication/editorpreviewservice.hpp"
#include "../src/core/renderingpass/previewgraph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

Json sceneFixture() {
    return Json::parse(R"json({
      "schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[
        {"name":"Root","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"light","type":"directional","direction":[0,-1,0],"intensity":1,"color":[1,1,1]}
        ]},
        {"name":"Leaf","parent":"Root","components":[
          {"name":"transform","pos":[0,1,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"collider","shape":"sphere","radius":0.5}
        ]},
        {"name":"Model","components":[
          {"name":"transform","pos":[1,0,1],"rotation":[0,0,0,1],"scale":[1,1,1]}
        ]},
        {"name":"Camera","components":[
          {"name":"transform","pos":[0,0,-5],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"camera","type":"perspective","yfov":0.8,"znear":0.1,"zfar":100}
        ]}
      ]}}}
    )json");
}

PreviewGraphProgram graphFixture() {
    const auto config = R"json({
      "features":["fixture://ui"],
      "render_targets":[],
      "rendering_passes":[{"name":"main_render","passes":[
        {"name":"main_scene","type":"material",
         "output":{"color":["swapchain"],"depth":null}}
      ]}]
    })json";
    return precompilePreviewGraph(
        config,
        [](std::string_view ref) {
            if (ref != "fixture://ui") throw std::runtime_error{"unknown fixture feature"};
            return std::string{R"json({
              "schema":"pelican.render_feature","version":1,"name":"ui",
              "passes":[{"insert":"after:pelican_ui","pass":{
                "name":"pelican_ui","type":"ui",
                "output":{"color":"swapchain","depth":null}
              }}]
            })json"};
        }, true);
}

OrderedJson exactSharedStateFixture() {
    return {
        {"authored_semantic_bytes", "byte-exact"}, {"scene_revision", 72},
        {"journal", {{"entries", 4}, {"head", 3}}},
        {"ecs", {{"values", {1, 2, 3}}, {"versions", {9, 10, 11}},
                 {"global_tick", 12}, {"entities", {{0, 7}, {2, 4}}},
                 {"free_list", {3, 1}}}},
        {"light", {{"bindings", {"sun"}}, {"identity", 15}}},
        {"phys", {{"bindings", {"Leaf"}}, {"identity_counter", 16}}},
        {"renderer", {{"handles", {21, 22}}, {"flat_history", {1, 2}},
                      {"xr_history", {3, 4}}, {"last_snapshot", {5, 6}},
                      {"reset", false}, {"observed_revision", 24},
                      {"history_epoch", 25}}},
        {"polygon", {{"previous_palette", {1, 3, 5}},
                     {"previous_model", {2, 4, 6}}}},
        {"camera", {{"history", {7, 8}}, {"snapshot", {9, 10}}}},
        {"deletion", {{"logical_epoch", 29}, {"pending", 2}}},
        {"timing", {{"allocator", 30}, {"pending", {31}},
                    {"published", {32}}, {"ring", {33, 34}}}},
        {"behavior_event", {{"attachments", {35}}, {"events", {36}}}},
        {"engine_time", {{"now", 2.5}, {"delta", 0.016}, {"frame", 90}}},
        {"reload_generation", 38},
    };
}

Json renderParams(const PreviewGraphProgram &graph, std::uint32_t width = 96,
                  std::uint32_t height = 64, std::string encoding = "png",
                  std::size_t max_bytes = 200000) {
    return {
        {"overrides", Json::array({
            {{"op", "set_component_value"}, {"object_id", 1},
             {"component_slot", "transform"}, {"field_path", "/pos"},
             {"value", {0.5, 0, 0}}}
        })},
        {"capture", {
            {"width", width}, {"height", height},
            {"pixel_encoding", encoding}, {"camera", {{"object_id", 4}}},
            {"graph_generation", graph.generation}, {"max_bytes", max_bytes}
        }},
    };
}

EditorPreviewErrorCode errorCode(const std::function<void()> &invoke) {
    try {
        invoke();
        FAIL("EditorPreviewError was not raised");
    } catch (const EditorPreviewError &error) {
        return error.code();
    }
    return EditorPreviewErrorCode::state_changed;
}

EditorPreviewGateSnapshot openGate(std::uint64_t epoch = 7) {
    return {.can_preview = true, .epoch = epoch};
}

} // namespace

TEST_CASE("WP172 preview graph is a third startup program with an exclusion policy",
          "[wp172][preview-graph]") {
    const auto first = graphFixture();
    const auto second = graphFixture();
    REQUIRE(first.name == "preview");
    REQUIRE(first.generation != 0);
    REQUIRE(first.generation == second.generation);
    REQUIRE(first.excluded_feature_names == std::vector<std::string>{"ui"});
    REQUIRE(first.composed_config.dump().find("swapchain") == std::string::npos);
    REQUIRE(first.composed_config.dump().find("preview_capture") != std::string::npos);
    REQUIRE(first.pass_names ==
            std::vector<std::string>{"main_scene", "output_transform"});

    const Json ui_feature{{"passes", Json::array({
        {{"insert", "after:pelican_ui"},
         {"pass", {{"name", "editor_overlay"}, {"type", "ui"}}}}
    })}};
    const Json taa_feature{{"projection_jitter", {{"pattern", "halton23"}}}};
    const Json velocity_feature{{"render_targets", Json::array({
        {{"name", "motion_vectors"}}
    })}};
    REQUIRE_FALSE(includeFeatureInPreviewGraph("ui", ui_feature));
    REQUIRE_FALSE(includeFeatureInPreviewGraph("custom_taa", taa_feature));
    REQUIRE_FALSE(includeFeatureInPreviewGraph("custom_motion", velocity_feature));

    auto retained_terminal = first.composed_config;
    retained_terminal.at("rendering_passes").at(0).at("passes").push_back(
        {{"name", "taa_present"}, {"type", "fullscreen"},
         {"output", {{"color", "preview_capture"}, {"depth", nullptr}}}});
    REQUIRE_NOTHROW(validatePreviewGraphConfig(retained_terminal));

    auto unsafe = first.composed_config;
    unsafe.at("rendering_passes").at(0).at("passes").push_back(
        {{"name", "authored_velocity"}, {"type", "velocity"}});
    try {
        validatePreviewGraphConfig(unsafe);
        FAIL("unsafe authored pass was accepted");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string{error.what()}.find("main_render") != std::string::npos);
        REQUIRE(std::string{error.what()}.find("authored_velocity") != std::string::npos);
    }

    const auto inventory = previewStateInventory();
    REQUIRE(inventory.size() == 12);
    const std::array<std::string_view, 12> expected{
        "DeletionQueue(logical epoch)",
        "shared FrameResources(beginLogicalFrame / slot select / uniform update)",
        "RenderTiming allocator/pending/published; shared ring",
        "RenderTargetContainer history", "PolygonInstanceContainer previous state",
        "Renderer flat/xr histories+last snapshots+reset/observed revision",
        "camera history/snapshot", "internal_render_extent/resize/RT rebind",
        "layout tracker", "EngineTime/frame index",
        "flat/xr graph/selectGraphVariant", "swapchain/XR mirror/present"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE(inventory.at(index).at("state") == std::string{expected[index]});
    }
}

TEST_CASE("WP172 typed eval_preview consumes the gate twice and never publishes",
          "[wp172][eval-preview][service]") {
    const auto base = AuthoringSceneDocument::load(sceneFixture().dump(), SceneRevision{72});
    const auto graph = graphFixture();
    auto shared = exactSharedStateFixture();
    const auto before = shared;
    std::size_t gate_calls = 0;
    EditorPreviewService service{{
        .document = [&]() -> const AuthoringSceneDocument & { return base; },
        .preview_graph = [&]() -> const PreviewGraphProgram & { return graph; },
        .xr_active = [] { return false; },
        .engine_time = [] { return PreviewEngineTimeSnapshot{}; },
        .shared_state_snapshot = [&] { return shared; },
    }};
    const auto result = service.evalPreview(
        {{"overrides", Json::array({
             {{"op", "set_component_value"}, {"object_id", 1},
              {"component_slot", "light"}, {"field_path", "/intensity"},
              {"value", 5.0}}
         })},
         {"queries", Json::array({
             {{"kind", "component"}, {"object_id", 1},
              {"component_slot", "light"}},
             {{"kind", "descendant_world"}, {"object_id", 1}},
             {{"kind", "raycast"},
              {"ray", {{"origin", {0, 1, -3}}, {"direction", {0, 0, 1}},
                       {"max_distance", 10}}}},
             {{"kind", "overlap"},
              {"shape", {{"type", "sphere"}, {"center", {0, 1, 0}},
                         {"radius", 0.1}}}},
             {{"kind", "camera"}, {"object_id", 4}, {"width", 96}, {"height", 64}}
         })}},
        [&] { ++gate_calls; return openGate(); });
    REQUIRE(result.at("status") == "evaluated");
    REQUIRE(result.at("results").size() == 5);
    REQUIRE(result.at("results").at(0).at("data").at("intensity") == 5.0f);
    REQUIRE(gate_calls == 2);
    REQUIRE(shared == before);
    REQUIRE(base.revision().value == 72);

    gate_calls = 0;
    REQUIRE(errorCode([&] {
        service.evalPreview(Json::object(), [&] {
            ++gate_calls;
            return gate_calls == 1 ? openGate(7) : openGate(8);
        });
    }) == EditorPreviewErrorCode::gate_closed);
    REQUIRE(gate_calls == 2);
    REQUIRE(shared == before);
}

TEST_CASE("WP172 render_preview validates before invocation and captures request-local bytes",
          "[wp172][render-preview][capture]") {
    const auto base = AuthoringSceneDocument::load(sceneFixture().dump(), SceneRevision{72});
    const auto graph = graphFixture();
    auto shared = exactSharedStateFixture();
    const auto before = shared;
    std::size_t executor_invocations = 0;
    bool xr = false;
    EditorPreviewService service{{
        .document = [&]() -> const AuthoringSceneDocument & { return base; },
        .preview_graph = [&]() -> const PreviewGraphProgram & { return graph; },
        .xr_active = [&] { return xr; },
        .engine_time = [] { return PreviewEngineTimeSnapshot{2.5, 0.016, 90}; },
        .shared_state_snapshot = [&] { return shared; },
        .execution_fault_hook = [&](std::string_view method) {
            REQUIRE(std::string{method} == "render_preview");
            ++executor_invocations;
        },
    }};
    std::size_t gates = 0;
    const auto result = service.renderPreview(renderParams(graph), [&] {
        ++gates;
        return openGate();
    });
    REQUIRE(result.at("status") == "rendered");
    REQUIRE(result.at("graph") == "preview");
    REQUIRE(result.at("capture").at("pixel_encoding") == "png");
    REQUIRE(result.at("capture").at("data_base64").get<std::string>().starts_with("iVBORw0KGgo"));
    REQUIRE(result.at("capture").at("byte_count").get<std::size_t>() > 100);
    REQUIRE(result.at("timing").at("namespace").get<std::string>().starts_with("preview_request_id:"));
    REQUIRE_FALSE(result.at("timing").at("published_to_shared_ring").get<bool>());
    REQUIRE(result.at("state_inventory").size() == 12);
    REQUIRE(gates == 2);
    REQUIRE(executor_invocations == 1);
    REQUIRE(shared == before);

    auto explicit_camera = renderParams(graph, 32, 32, "rgba8_srgb", 4096);
    explicit_camera["capture"]["camera"] = {
        {"position", {0, 0, -5}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}},
        {"projection", {{"kind", "perspective"}, {"yfov", 0.8},
                        {"znear", 0.1}, {"zfar", 100.0}}}
    };
    const auto explicit_result = service.renderPreview(
        explicit_camera, [] { return openGate(); });
    REQUIRE(explicit_result.at("capture").at("pixel_encoding") == "rgba8_srgb");
    REQUIRE(explicit_result.at("capture").at("byte_count") == 4096);
    REQUIRE(executor_invocations == 2);

    auto bad_dimension = renderParams(graph);
    bad_dimension["capture"]["width"] = 0;
    REQUIRE(errorCode([&] { service.renderPreview(bad_dimension, [] { return openGate(); }); }) ==
            EditorPreviewErrorCode::capture_schema_violation);
    REQUIRE(executor_invocations == 2);

    auto parallel_camera = renderParams(graph);
    parallel_camera["capture"]["camera"] = {
        {"position", {0, 0, -5}}, {"target", {0, 0, 0}}, {"up", {0, 0, 1}},
        {"projection", {{"kind", "perspective"}, {"yfov", 0.8},
                        {"znear", 0.1}, {"zfar", 100.0}}}
    };
    REQUIRE(errorCode([&] {
        service.renderPreview(parallel_camera, [] { return openGate(); });
    }) == EditorPreviewErrorCode::capture_schema_violation);
    REQUIRE(executor_invocations == 2);

    auto too_large = renderParams(graph, 96, 64, "png", 100);
    REQUIRE(errorCode([&] { service.renderPreview(too_large, [] { return openGate(); }); }) ==
            EditorPreviewErrorCode::capture_too_large);
    REQUIRE(executor_invocations == 2);

    auto stale = renderParams(graph);
    stale["capture"]["graph_generation"] = graph.generation + 1;
    REQUIRE(errorCode([&] { service.renderPreview(stale, [] { return openGate(); }); }) ==
            EditorPreviewErrorCode::capture_schema_violation);
    REQUIRE(executor_invocations == 2);

    try {
        service.renderPreview(renderParams(graph), [] {
            return EditorPreviewGateSnapshot{.can_preview = false,
                                             .epoch = 11,
                                             .reasons = {"reload_scene_transition"}};
        });
        FAIL("closed gate was accepted");
    } catch (const EditorPreviewError &error) {
        REQUIRE(error.code() == EditorPreviewErrorCode::gate_closed);
        REQUIRE(error.payload().at("method") == "render_preview");
        REQUIRE(error.payload().at("reason") == "reload_scene_transition");
        REQUIRE(error.payload().at("epoch") == 11);
    }
    REQUIRE(executor_invocations == 2);

    xr = true;
    try {
        service.renderPreview(renderParams(graph), [] { return openGate(); });
        FAIL("XR preview was accepted");
    } catch (const EditorPreviewError &error) {
        REQUIRE(error.code() == EditorPreviewErrorCode::xr_active_unsupported);
        REQUIRE(error.payload().at("method") == "render_preview");
        REQUIRE(error.payload().at("reason") == "xr_active");
    }
    REQUIRE(executor_invocations == 2);
    REQUIRE(shared == before);
}

TEST_CASE("WP172 preview insertion leaves the next flat and XR normal capture identical",
          "[wp172][render-preview][control]") {
    const auto base = AuthoringSceneDocument::load(sceneFixture().dump(), SceneRevision{72});
    const auto graph = graphFixture();
    const auto initial = exactSharedStateFixture();
    const auto normal_frame = [](OrderedJson &state, std::string_view variant) {
        state["deletion"]["logical_epoch"] =
            state["deletion"]["logical_epoch"].get<std::uint64_t>() + 1;
        state["timing"]["published"] = Json::array({variant, "normal-complete"});
        state["renderer"][std::string{variant} + "_history"] = Json::array({41, 42});
        state["polygon"]["previous_palette"] = Json::array({8, 13, 21});
        state["polygon"]["previous_model"] = Json::array({5, 8, 13});
        return state.dump();
    };

    for (const auto variant : {std::string_view{"flat"}, std::string_view{"xr"}}) {
        auto control_state = initial;
        const auto control_capture = normal_frame(control_state, variant);

        auto preview_state = initial;
        EditorPreviewService service{{
            .document = [&]() -> const AuthoringSceneDocument & { return base; },
            .preview_graph = [&]() -> const PreviewGraphProgram & { return graph; },
            .xr_active = [] { return false; },
            .engine_time = [] { return PreviewEngineTimeSnapshot{2.5, 0.016, 90}; },
            .shared_state_snapshot = [&] { return preview_state; },
        }};
        (void)service.renderPreview(renderParams(graph), [] { return openGate(); });
        REQUIRE(preview_state == initial);
        const auto preview_then_normal_capture = normal_frame(preview_state, variant);
        REQUIRE(preview_state == control_state);
        REQUIRE(preview_then_normal_capture == control_capture);
    }
}

} // namespace Pelican
