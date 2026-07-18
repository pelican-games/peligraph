#include <catch2/catch_test_macros.hpp>

#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/imgui/assetbrowser.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <utility>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios_base::binary};
    if (!input.is_open()) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(input);
}

template <class Invoke> EditorCommandErrorCode errorCode(Invoke &&invoke) {
    std::optional<EditorCommandErrorCode> result;
    try {
        std::forward<Invoke>(invoke)();
    } catch (const EditorCommandError &error) {
        result = error.code();
    }
    REQUIRE(result.has_value());
    return *result;
}

} // namespace

TEST_CASE("asset browser UI and RPC adapters preserve list_assets results and errors",
          "[imgui][asset-browser][editor-command][wp159]") {
    const auto fixture = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                         "fixtures" / "authoring_scene" / "multi_scene_roundtrip.json";
    const auto document = AuthoringSceneDocument::load(readJson(fixture).dump(), SceneRevision{51});
    bool inject_error = false;
    std::uint64_t provider_calls = 0;
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .assets = [&] {
            ++provider_calls;
            if (inject_error) {
                throw EditorCommandError{EditorCommandErrorCode::InvalidParams,
                                         "injected list_assets failure"};
            }
            return std::vector<EditorAssetQueryResult>{
                {.id = "hero", .kind = "model", .path = "assets/hero.glb",
                 .store = "project", .status = "loaded"},
                {.id = "mask", .kind = "texture", .path = "assets/mask.png",
                 .store = "project", .status = "missing"},
                {.id = "shared", .kind = "model", .path = "shared/model.glb",
                 .store = "shared", .status = "loaded"},
            };
        },
    }};
    const EditorCommandRpcAdapter rpc{service};
    AssetBrowserPanelTrace trace;
    const AssetBrowserQueryAdapter ui{service, trace};

    const EditorListAssetsRequest typed_request{.store = "project"};
    const auto rpc_result = rpc.listAssets({{"store", "project"}});
    const auto ui_result = ui.listAssets(typed_request);
    REQUIRE(rpc_result.dump() == editorQueryJson(ui_result).dump());
    REQUIRE(ui_result.assets.size() == 2);
    REQUIRE(ui_result.assets[0].id == "hero");
    REQUIRE(ui_result.assets[1].status == "missing");
    REQUIRE(trace.query_calls == 1);
    REQUIRE(trace.edit_enqueue_calls == 0);
    REQUIRE(provider_calls == 2);

    inject_error = true;
    const auto rpc_error = errorCode([&] { (void)rpc.listAssets({{"store", "project"}}); });
    const auto ui_error = errorCode([&] { (void)ui.listAssets(typed_request); });
    REQUIRE(rpc_error == EditorCommandErrorCode::InvalidParams);
    REQUIRE(ui_error == rpc_error);
    REQUIRE(trace.query_calls == 2);
    REQUIRE(trace.edit_enqueue_calls == 0);
}

TEST_CASE("deterministic drivers execute zero asset browser callbacks and queries",
          "[imgui][asset-browser][isolation][wp159]") {
    const auto fixture = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                         "fixtures" / "authoring_scene" / "multi_scene_roundtrip.json";
    const auto document = AuthoringSceneDocument::load(readJson(fixture).dump(), SceneRevision{52});
    std::uint64_t provider_calls = 0;
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & { return document; },
        .current_scene_id = [] { return std::string{"main"}; },
        .assets = [&] {
            ++provider_calls;
            return std::vector<EditorAssetQueryResult>{};
        },
    }};

    std::vector<EngineLaunchConfig> isolated(5);
    isolated[0].headless = true;
    isolated[1].rpc = true;
    isolated[2].input_replay = true;
    isolated[3].golden_mode = true;
    isolated[4].xr_active = true;

    for (const auto &config : isolated) {
        AssetBrowserPanelTrace trace;
        const AssetBrowserQueryAdapter ui{service, trace};
        REQUIRE_FALSE(invokeAssetBrowserPanelCallback(config, trace, [&] {
            (void)ui.listAssets();
        }));
        REQUIRE(trace.panel_callback_calls == 0);
        REQUIRE(trace.query_calls == 0);
        REQUIRE(trace.edit_enqueue_calls == 0);
    }
    REQUIRE(provider_calls == 0);

    EngineLaunchConfig interactive;
    AssetBrowserPanelTrace trace;
    const AssetBrowserQueryAdapter ui{service, trace};
    REQUIRE(invokeAssetBrowserPanelCallback(interactive, trace, [&] {
        (void)ui.listAssets();
    }));
    REQUIRE(trace.panel_callback_calls == 1);
    REQUIRE(trace.query_calls == 1);
    REQUIRE(trace.edit_enqueue_calls == 0);
    REQUIRE(provider_calls == 1);
}

} // namespace Pelican
