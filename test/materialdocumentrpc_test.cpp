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
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <vector>

// MT-1. A Blank project's box takes a material made, bound and recoloured over
// the engine's RPC (create_material -> bind_material -> edit_material), drawn
// in the next frame of the same engine; the documents written are what the
// next start of the engine draws.

namespace Pelican {
namespace {

using Json = nlohmann::json;

constexpr std::string_view openPbrOpaque = "engine://surfaces/openpbr/opaque_single.surface";
constexpr std::string_view cube = "engine://primitives/cube";

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << contents;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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
                   ("pelican_mt1_" + std::string{name} + "_" + std::to_string(suffix))};
}

// The Blank template with the one material pass a box needs: forward_opaque
// (cleared to white) under material_route_default forward, and the
// template's own `present` (engine://passthrough) reading its colour.
std::filesystem::path makeBlankBoxProject(const std::filesystem::path &dir) {
    const auto root = dir / "project";
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    Json rendering{
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
    TestSupport::requireVulkanDevice("MT-1 material documents");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

std::ostream &operator<<(std::ostream &out, const Rgb &rgb) {
    return out << "rgb(" << rgb.r << ", " << rgb.g << ", " << rgb.b << ")";
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 7000;

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
    // The refusal's {error_code, error_fields}.
    Json refusal(std::string_view method, Json params) {
        const auto response = call(method, std::move(params));
        INFO(method << " -> " << response.dump(2));
        REQUIRE(response.contains("error"));
        REQUIRE(response.at("error").contains("data"));
        return response.at("error").at("data");
    }
    // The centre pixel of the next frame, through the capture RPC.
    Rgb centre(const std::filesystem::path &capture) {
        (void)result("render_frame");
        std::filesystem::remove(capture);
        (void)result("capture", {{"path", capture.generic_string()}});
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
        REQUIRE(pixels != nullptr);
        const auto *pixel = pixels + (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
        const Rgb rgb{pixel[0], pixel[1], pixel[2]};
        stbi_image_free(pixels);
        return rgb;
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
};

bool reddish(const Rgb &rgb) {
    return rgb.r > rgb.g + 60 && rgb.r > rgb.b + 60;
}
bool bluish(const Rgb &rgb) {
    return rgb.b > rgb.r + 60 && rgb.b > rgb.g + 60;
}

Json spawnCube() {
    return Json::array({{{"op", "spawn"},
                         {"scene_id", "default_scene"},
                         {"object",
                          {{"name", "Box"},
                           {"components",
                            Json::array({{{"name", "transform"},
                                          {"pos", {0.0, 0.0, 0.0}},
                                          {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                          {"scale", {1.0, 1.0, 1.0}}},
                                         {{"name", "simplemodelview"}, {"model", cube}}})}}}}});
}

} // namespace

TEST_CASE("MT-1 a Blank box is given a material made, bound and recoloured over RPC without a restart",
          "[mt1][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("box");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";

    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "MT-1"}}).at("actor_id").get<std::uint64_t>();
        const auto spawned = rpc.edit(actor, spawnCube());
        INFO(spawned.dump(2));
        REQUIRE(spawned.at("status") == "committed");
        // The box is drawn in its grey default on the white clear.
        const auto grey = rpc.centre(capture);
        INFO("centre pixel " << grey);
        CHECK(grey.r < 230);
        CHECK_FALSE(reddish(grey));
        CHECK_FALSE(bluish(grey));

        // Nothing is listed yet; the box carries its glTF default.
        CHECK(rpc.result("list_materials").at("documents").empty());
        const auto unbound = rpc.result("get_material_bindings", {{"model", cube}});
        CHECK(unbound.at("material_bindings").is_null());
        REQUIRE(unbound.at("primitives").size() == 1);
        CHECK(unbound.at("primitives").at(0).at("slot") == "pelican.primitive.default");
        CHECK(unbound.at("primitives").at(0).at("material") == "pelican.primitive.default");

        // The schema is the surface's own declaration.
        const auto schema = rpc.result("get_surface_schema", {{"surface", openPbrOpaque}});
        Json base_color;
        for (const auto &param : schema.at("params"))
            if (param.at("name") == "base_color") base_color = param;
        REQUIRE(base_color.is_object());
        CHECK(base_color.at("type") == "color");
        CHECK(base_color.at("min") == 0.0);
        CHECK(base_color.at("max") == 1.0);
        CHECK(base_color.at("default") == Json::array({0.8, 0.8, 0.8, 1.0}));
        bool color_map = false;
        for (const auto &texture : schema.at("textures"))
            if (texture.at("name") == "base_color_map")
                color_map = texture.at("color_space") == "srgb" && texture.at("role") == "color";
        CHECK(color_map);

        // create_material: the document and its asset_data entry, registered live.
        const Json red{1.0, 0.0, 0.0, 1.0};
        const Json blue{0.0, 0.0, 1.0, 1.0};
        const auto created = rpc.result(
            "create_material",
            {{"path", path},
             {"surface", openPbrOpaque},
             {"values", {{"base_color", red}, {"emission_color", red}, {"emission_luminance", 1.0}}}});
        CHECK(created.at("committed") == true);
        CHECK(created.at("name") == "paint");
        CHECK(created.at("applied") == "live");
        CHECK(created.at("restart_required").is_null());
        const auto listed = rpc.result("list_materials").at("documents");
        REQUIRE(listed.size() == 1);
        CHECK(listed.at(0).at("path") == path);
        CHECK(listed.at(0).at("status") == "loaded");
        REQUIRE(listed.at(0).at("materials").size() == 1);
        CHECK(listed.at(0).at("materials").at(0).at("name") == "paint");
        CHECK(listed.at(0).at("materials").at(0).at("registered") == true);
        // The same name again is refused by name; so is the same path.
        CHECK(rpc.refusal("create_material", {{"path", "materials/other.material.json"},
                                              {"surface", openPbrOpaque},
                                              {"name", "paint"}})
                  .at("error_code") == "material.name_conflict@1");
        CHECK(rpc.refusal("create_material", {{"path", path}, {"surface", openPbrOpaque}, {"name", "paint2"}})
                  .at("error_code") == "material.path_conflict@1");

        // bind_material: the Blank box's glTF default slot takes it, live.
        const auto bound = rpc.result("bind_material",
                                      {{"model", cube}, {"slot", "pelican.primitive.default"}, {"material", "paint"}});
        CHECK(bound.at("applied") == "live");
        CHECK(bound.at("previous") ==
              Json{{"bindings", Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}})}});
        {
            const auto pixel = rpc.centre(capture);
            INFO("centre pixel " << pixel);
            CHECK(reddish(pixel));
        }

        // edit_material: refusals first, by name, with nothing written.
        auto read = rpc.result("get_material", {{"path", path}});
        CHECK(read.at("name") == "paint");
        CHECK(read.at("material").at("values").at("base_color") == red);
        const auto digest = read.at("material_digest").get<std::string>();
        const auto out_of_range = rpc.refusal(
            "edit_material",
            {{"base_material_digest", digest}, {"path", path}, {"values", {{"base_color", {2.0, 0.0, 0.0, 1.0}}}}});
        CHECK(out_of_range.at("error_code") == "material.value_invalid@1");
        CHECK(out_of_range.at("error_fields").at("path") == path);
        CHECK(out_of_range.at("error_fields").at("param") == "base_color");
        CHECK(out_of_range.at("error_fields").at("reason").get<std::string>().find("max") != std::string::npos);
        const auto unknown = rpc.refusal(
            "edit_material", {{"base_material_digest", digest}, {"path", path}, {"values", {{"tint", 1.0}}}});
        CHECK(unknown.at("error_code") == "material.param_unknown@1");
        CHECK(unknown.at("error_fields").at("param") == "tint");
        const auto stale = rpc.refusal(
            "edit_material",
            {{"base_material_digest", std::string(64, '0')}, {"path", path}, {"values", {{"base_color", blue}}}});
        CHECK(stale.at("error_code") == "external_modification");
        CHECK(rpc.result("get_material", {{"path", path}}).at("material_digest") == digest);

        // The colour changes in the next frame of the same engine.
        const auto edited = rpc.result(
            "edit_material",
            {{"base_material_digest", digest}, {"path", path}, {"values", {{"base_color", blue}, {"emission_color", blue}}}});
        CHECK(edited.at("applied") == "live");
        CHECK(edited.at("previous_material_digest") == digest);
        CHECK(edited.at("previous") == Json{{"values", {{"base_color", red}, {"emission_color", red}}}});
        {
            const auto pixel = rpc.centre(capture);
            INFO("centre pixel " << pixel);
            CHECK(bluish(pixel));
        }

        // Taken back with what it answered.
        const auto undone = rpc.result("edit_material", {{"base_material_digest", edited.at("material_digest")},
                                                         {"path", path},
                                                         {"values", edited.at("previous").at("values")}});
        CHECK(undone.at("applied") == "live");
        CHECK(undone.at("material_digest") == digest);
        {
            const auto pixel = rpc.centre(capture);
            INFO("centre pixel " << pixel);
            CHECK(reddish(pixel));
        }

        // A key outside values is written for the next start.
        const auto tagged = rpc.result("edit_material", {{"base_material_digest", digest},
                                                         {"path", path},
                                                         {"tags", Json::array({"paint"})}});
        CHECK(tagged.at("applied") == "restart_required");
        CHECK(tagged.at("previous") == Json{{"tags", nullptr}});
        {
            const auto pixel = rpc.centre(capture);
            INFO("centre pixel " << pixel);
            CHECK(reddish(pixel));
        }

        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    const auto assets = Json::parse(readFile(root / "assets/asset_data.json"));
    CHECK(assets.at("materials") == Json::array({{{"path", path}}}));
    REQUIRE(assets.at("models").size() == 1);
    CHECK(assets.at("models").at(0).at("name") == std::string{cube});
    CHECK(assets.at("models").at(0).at("path") == std::string{cube});
    const auto bindings_reference = assets.at("models").at(0).at("material_bindings").get<std::string>();
    const auto bindings = Json::parse(readFile(root / bindings_reference));
    CHECK(bindings.at("model") == std::string{cube});
    CHECK(bindings.at("bindings").at(0).at("material") == "paint");
    CHECK(readFile(root / "scenes/main.scene.json").find("\"Box\"") != std::string::npos);
    const auto document = Json::parse(readFile(root / path));
    CHECK(document.at("materials").at(0).at("tags") == Json::array({"paint"}));

    // The next start draws what was written.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        // As the player boots (appflow/loop.cpp): the renderer first, which
        // snapshots material_route_default before asset_data's materials are
        // lowered; the endpoint below makes the model container.
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        CHECK(rpc.result("scene_tree").dump().find("Box") != std::string::npos);
        CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") == "paint");
        {
            const auto pixel = rpc.centre(capture);
            INFO("centre pixel " << pixel);
            CHECK(reddish(pixel));
        }
        const auto listed = rpc.result("list_materials").at("documents");
        REQUIRE(listed.size() == 1);
        CHECK(listed.at(0).at("materials").at(0).at("tags") == Json::array({"paint"}));
        CHECK(listed.at(0).at("materials").at(0).at("registered") == true);
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

} // namespace Pelican
