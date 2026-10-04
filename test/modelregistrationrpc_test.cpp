#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/project/projecttemplate.hpp"
#include "render_graph_root_test_support.hpp"
#include "vrm_xr_demo_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stb_image.h>
#include <string>

// MI-1. A glb the editor has put in a Blank project is registered over the
// engine's RPC (register_model), drawn by an object an edit adds in the next
// frame of the same engine, drawn again by the next start from what was
// written, and taken back (unregister_model) once nothing draws it.

namespace Pelican {
namespace {

using Json = nlohmann::json;

constexpr std::string_view checkerBox = "test/fixtures/studio_material_pass_authoring/checker_box.glb";

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << contents;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void copyFixture(const std::filesystem::path &to) {
    std::filesystem::create_directories(to.parent_path());
    std::filesystem::copy_file(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / checkerBox, to,
                               std::filesystem::copy_options::overwrite_existing);
}

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TempDir makeTempDir(std::string_view name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return TempDir{std::filesystem::temp_directory_path() /
                   ("pelican_mi1_" + std::string{name} + "_" + std::to_string(suffix))};
}

// The Blank template with one forward material pass cleared to white and the
// template's own `present` (as MT-1's makeBlankBoxProject).
std::filesystem::path makeBlankProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    const Json rendering{
        {"material_route_default", "forward"},
        {"render_targets",
         Json::array({{{"name", "scene_color"},
                       {"extent_scale", 1.0},
                       {"format", "R16G16B16A16_SFLOAT"},
                       {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                       {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
                      {{"name", "scene_depth"},
                       {"extent_scale", 1.0},
                       {"format", "D32_SFLOAT"},
                       {"format_class", "data"},
                       {"usage", {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}}}})},
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes",
                        Json::array({{{"name", "forward"},
                                      {"type", "material"},
                                      {"material_contract", "forward_opaque_v1"},
                                      {"clear_color", {1.0, 1.0, 1.0, 1.0}},
                                      {"output", {{"color", {"scene_color"}}, {"depth", "scene_depth"}}}},
                                     {{"name", "present"},
                                      {"type", "fullscreen"},
                                      {"input", {"scene_color"}},
                                      {"output", {{"color", {"display"}}, {"depth", nullptr}}},
                                      {"shader",
                                       {{"vertex", "engine://fullscreen"},
                                        {"fragment", "engine://passthrough"}}}}})}}})},
    };
    writeFile(root / "passes/main_rendering_config.json", TestSupport::renderGraphRoot(rendering).dump(2));
    return root;
}

void configureHeadless(const std::filesystem::path &root) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    TestSupport::requireVulkanDevice("MI-1 model registration");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9100;

    Json call(std::string_view method, Json params = Json::object()) {
        return Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}}
                .dump()));
    }
    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(method << " -> " << response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
    // The refusal's {error_code, error_fields}, and its message.
    Json refusal(std::string_view method, Json params) {
        const auto response = call(method, std::move(params));
        INFO(method << " -> " << response.dump(2));
        REQUIRE(response.contains("error"));
        REQUIRE(response.at("error").contains("data"));
        auto data = response.at("error").at("data");
        data["message"] = response.at("error").at("message");
        return data;
    }
    // How many pixels of the next frame are not the white clear.
    int drawnPixels(const std::filesystem::path &capture) {
        (void)result("render_frame");
        std::filesystem::remove(capture);
        (void)result("capture", {{"path", capture.generic_string()}});
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
        REQUIRE(pixels != nullptr);
        int drawn = 0;
        for (int index = 0; index < width * height; ++index) {
            const auto *pixel = pixels + static_cast<std::size_t>(index) * 4;
            if (pixel[0] < 200 || pixel[1] < 200 || pixel[2] < 200) ++drawn;
        }
        stbi_image_free(pixels);
        return drawn;
    }
    Json edit(std::uint64_t actor, Json operations) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        const auto accepted = result("edit", {{"actor_id", actor},
                                              {"base_revision", revision},
                                              {"operations", std::move(operations)}});
        REQUIRE(accepted.at("status") == "accepted");
        (void)result("step_frame");
        return result("get_edit_result", {{"ticket", accepted.at("ticket")}});
    }
    // The list_assets entry with that id (null: none).
    Json asset(std::string_view id) {
        auto listed = result("list_assets");
        if (listed.is_object() && listed.contains("assets")) listed = listed.at("assets");
        for (const auto &entry : listed)
            if (entry.value("id", std::string{}) == id) return entry;
        return nullptr;
    }
};

Json spawn(std::string_view name, std::string_view model) {
    return Json::array({{{"op", "spawn"},
                         {"scene_id", "default_scene"},
                         {"object",
                          {{"name", name},
                           {"components",
                            Json::array({{{"name", "transform"},
                                          {"pos", {0.0, 0.0, 0.0}},
                                          {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                          {"scale", {1.0, 1.0, 1.0}}},
                                         {{"name", "simplemodelview"}, {"model", model}}})}}}}});
}

std::uint64_t objectId(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects"))
        if (object.value("name", std::string{}) == name) return object.at("authoring_object_id").get<std::uint64_t>();
    FAIL("no object named " << name);
    return 0;
}

} // namespace

TEST_CASE("MI-1 a glb put in a Blank project is registered over RPC, drawn live and by the next start, and "
          "taken back",
          "[mi1][model][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("register");
    const auto root = makeBlankProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const auto asset_data_path = root / "assets/asset_data.json";
    copyFixture(root / "assets/models/checker_box.glb");
    copyFixture(root / "assets/models/box2.glb");
    copyFixture(root / "assets/other/checker_box.glb");
    writeFile(root / "assets/models/thing.fbx", "not a model");
    writeFile(root / "assets/models/broken.glb", "glTF this is not a binary glTF");
    TestVrmXrDemoFixture::writeGlb(root / "assets/models/character.vrm");
    const std::string path = "assets/models/checker_box.glb";

    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "MI-1"}}).at("actor_id").get<std::uint64_t>();
        CHECK(rpc.drawnPixels(capture) == 0);
        CHECK(rpc.asset("checker_box").is_null());

        // Refusals, each by name, with asset_data left as it was.
        const auto untouched = readFile(asset_data_path);
        {
            const auto missing = rpc.refusal("register_model", {{"path", "assets/models/none.glb"}});
            INFO(missing.dump(2));
            CHECK(missing.at("error_code") == "model.file_missing@1");
            CHECK(missing.at("error_fields") == Json{{"path", "assets/models/none.glb"}});
            const auto fbx = rpc.refusal("register_model", {{"path", "assets/models/thing.fbx"}});
            INFO(fbx.dump(2));
            CHECK(fbx.at("error_code") == "model.format_unsupported@1");
            CHECK(fbx.at("error_fields") == Json{{"path", "assets/models/thing.fbx"},
                                                 {"extension", ".fbx"},
                                                 {"supported", {".glb", ".gltf", ".vrm"}}});
            const auto fragment = rpc.refusal("register_model", {{"path", path + "#scene/0"}});
            INFO(fragment.dump(2));
            CHECK(fragment.at("error_code") == "model.path_invalid@1");
            CHECK(fragment.at("error_fields").at("reason") == "fragment");
            CHECK(rpc.refusal("register_model", {{"path", "../outside.glb"}}).at("error_fields").at("reason") ==
                  "dot_segment");
            CHECK(rpc.refusal("register_model", {{"path", "assets\\models\\checker_box.glb"}})
                      .at("error_fields")
                      .at("reason") == "backslash");
            const auto broken = rpc.refusal("register_model", {{"path", "assets/models/broken.glb"}});
            INFO(broken.dump(2));
            CHECK(broken.at("error_code") == "model.load_failed@1");
            CHECK(broken.at("error_fields").at("path") == "assets/models/broken.glb");
            CHECK_FALSE(broken.at("error_fields").at("message").get<std::string>().empty());
            const auto engine_name =
                rpc.refusal("register_model", {{"path", path}, {"name", "engine://primitives/box"}});
            CHECK(engine_name.at("error_code") == "model.name_invalid@1");
            CHECK(engine_name.at("error_fields").at("reason") == "engine_reference");
            CHECK(rpc.refusal("register_model", {{"path", path}, {"name", ""}}).at("error_code") ==
                  "model.name_invalid@1");
        }
        CHECK(readFile(asset_data_path) == untouched);

        // An edit naming a model that is not registered fails by that name.
        {
            const auto unknown = rpc.edit(actor, spawn("Ghost", "checker_box"));
            INFO(unknown.dump(2));
            CHECK(unknown.at("status") != "committed");
            CHECK(unknown.at("error").at("code") == "method_unavailable");
            CHECK(unknown.at("error").at("payload").at("model") == "checker_box");
            CHECK(unknown.at("error").at("payload").at("reason") == "model_unknown");
        }

        // register_model: written, published and registered live.
        const auto digest_before = rpc.result("get_procedural_assets").at("source_digest");
        const auto registered = rpc.result("register_model", {{"path", "project://" + path}});
        INFO(registered.dump(2));
        CHECK(registered.at("committed") == true);
        CHECK(registered.at("name") == "checker_box");
        CHECK(registered.at("path") == path);
        CHECK(registered.at("applied") == "live");
        CHECK(registered.at("restart_required").is_null());
        CHECK(registered.at("asset_data_digest") != registered.at("previous_asset_data_digest"));
        {
            const auto written = Json::parse(readFile(asset_data_path)).at("models");
            REQUIRE(written.size() == 1);
            CHECK(written.at(0) == Json{{"name", "checker_box"}, {"path", path}});
        }
        {
            const auto listed = rpc.asset("checker_box");
            INFO(listed.dump(2));
            REQUIRE(listed.is_object());
            CHECK(listed.at("kind") == "model");
            CHECK(listed.at("status") == "loaded");
            // EB-1: the path as asset_data writes it, the resolved file
            // beside it.
            CHECK(listed.at("path") == path);
            REQUIRE(listed.contains("resolved_path"));
            const std::filesystem::path resolved{listed.at("resolved_path").get<std::string>()};
            CHECK(resolved.is_absolute());
            CHECK(std::filesystem::equivalent(resolved, root / path));
        }

        // Conflicts: the same path (its name is given back), the same name.
        {
            const auto again = rpc.refusal("register_model", {{"path", path}, {"name", "again"}});
            INFO(again.dump(2));
            CHECK(again.at("error_code") == "model.path_conflict@1");
            CHECK(again.at("error_fields") == Json{{"path", path}, {"name", "checker_box"}});
            const auto named = rpc.refusal("register_model", {{"path", "assets/models/box2.glb"}, {"name", "checker_box"}});
            CHECK(named.at("error_code") == "model.name_conflict@1");
            CHECK(named.at("error_fields") == Json{{"name", "checker_box"}});
            const auto stem = rpc.refusal("register_model", {{"path", "assets/other/checker_box.glb"}});
            CHECK(stem.at("error_code") == "model.name_conflict@1");
            CHECK(stem.at("error_fields") == Json{{"name", "checker_box"}});
        }

        // MI-1 (C2): the procedural asset editor takes the asset_data
        // register_model wrote and published as its own source.
        {
            const auto procedural = rpc.result("get_procedural_assets");
            INFO(procedural.dump(2));
            CHECK(procedural.at("external_modification") == false);
            CHECK(procedural.at("source_digest") != digest_before);
            const auto edited = rpc.result("edit_procedural_asset",
                                           {{"operation", "delete"},
                                            {"asset_name", "not_there"},
                                            {"base_source_digest", procedural.at("source_digest").at("hex")}});
            INFO(edited.dump(2));
            CHECK(edited.at("status") == "rejected");
            if (procedural.at("editable") == true) CHECK(edited.at("error").at("code") == "asset_not_found");
            CHECK(edited.at("error").at("code") != "external_modification");
        }

        // An object drawing it is added by an edit and drawn in the next frame.
        const auto spawned = rpc.edit(actor, spawn("Crate", "checker_box"));
        INFO(spawned.dump(2));
        REQUIRE(spawned.at("status") == "committed");
        CHECK(rpc.drawnPixels(capture) > 20);

        // unregister_model while an object draws it is refused, by name.
        {
            const auto in_use = rpc.refusal("unregister_model", {{"name", "checker_box"}});
            INFO(in_use.dump(2));
            CHECK(in_use.at("error_code") == "model.in_use@1");
            CHECK(in_use.at("error_fields") == Json{{"name", "checker_box"}, {"count", 1}, {"objects", {"Crate"}}});
            CHECK(rpc.refusal("unregister_model", {{"name", "nothing"}}).at("error_code") == "model.unknown@1");
            const auto primitive = rpc.refusal("unregister_model", {{"name", "engine://primitives/cube"}});
            CHECK(primitive.at("error_code") == "model.name_invalid@1");
            CHECK(primitive.at("error_fields").at("reason") == "engine_primitive");
        }

        // A VRM: registered live; its rig keeps it in the engine when it is
        // taken off, and the same file registered again is that model.
        {
            const auto vrm = rpc.result("register_model", {{"path", "assets/models/character.vrm"}});
            INFO(vrm.dump(2));
            CHECK(vrm.at("name") == "character");
            CHECK(vrm.at("applied") == "live");
            CHECK(rpc.asset("character").is_object());
            const auto taken = rpc.result("unregister_model", {{"name", "character"}});
            INFO(taken.dump(2));
            CHECK(taken.at("committed") == true);
            CHECK(taken.at("removed") == Json{{"name", "character"}, {"path", "assets/models/character.vrm"}});
            CHECK(taken.at("applied") == "restart_required");
            CHECK(taken.at("restart_required").at("reason").get<std::string>().find("animation rig") !=
                  std::string::npos);
            CHECK(rpc.asset("character").is_null());
            const auto back = rpc.result("register_model", {{"path", "assets/models/character.vrm"}});
            INFO(back.dump(2));
            CHECK(back.at("applied") == "live");
        }

        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The next start reads the entry written and draws the same object.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        CHECK(rpc.drawnPixels(capture) > 20);
        CHECK(rpc.asset("checker_box").is_object());
        CHECK(rpc.asset("character").is_object());

        // Nothing draws it once the object is gone: taken off, live.
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "MI-1"}}).at("actor_id").get<std::uint64_t>();
        const auto destroyed =
            rpc.edit(actor, Json::array({{{"op", "destroy"}, {"object_id", objectId(rpc, "Crate")}}}));
        INFO(destroyed.dump(2));
        REQUIRE(destroyed.at("status") == "committed");
        const auto taken = rpc.result("unregister_model", {{"name", "checker_box"}});
        INFO(taken.dump(2));
        CHECK(taken.at("committed") == true);
        CHECK(taken.at("removed") == Json{{"name", "checker_box"}, {"path", path}});
        CHECK(taken.at("applied") == "live");
        CHECK(taken.at("restart_required").is_null());
        CHECK(rpc.asset("checker_box").is_null());
        CHECK(std::filesystem::exists(root / path));
        {
            const auto written = Json::parse(readFile(asset_data_path)).at("models");
            REQUIRE(written.size() == 1);
            CHECK(written.at(0).at("name") == "character");
        }
        // Released, so the same name is registered afresh, live.
        const auto again = rpc.result("register_model", {{"path", path}});
        INFO(again.dump(2));
        CHECK(again.at("applied") == "live");
        CHECK(rpc.edit(actor, spawn("Crate", "checker_box")).at("status") == "committed");
        CHECK(rpc.drawnPixels(capture) > 20);
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

} // namespace Pelican
