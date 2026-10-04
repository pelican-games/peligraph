#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/material/projectmaterialasset.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/projecttemplate.hpp"
#include "render_graph_root_test_support.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <string>
#include <thread>
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
// MT-3 `transparent`: a forward_transparent pass after it, loading its colour
// and depth.
std::filesystem::path makeBlankBoxProject(const std::filesystem::path &dir, bool transparent = false) {
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
    if (transparent) {
        auto &passes = rendering.at("rendering_passes").at(0).at("passes");
        passes.insert(passes.begin() + 1, Json{{"name", "transparent"},
                                               {"type", "material"},
                                               {"material_contract", "forward_transparent_v1"},
                                               {"output", {{"color", {"scene_color"}}, {"depth", "scene_depth"}}},
                                               {"color_load_op", "load"},
                                               {"depth_load_op", "load"},
                                               {"depth_store_op", "store"}});
    }
    writeFile(root / "passes/main_rendering_config.json", TestSupport::renderGraphRoot(rendering).dump(2));
    return root;
}

// EB-2 `watched`: the resolver reads project.json's hot_reload and the engine
// asks for the watch (as a windowed one, Studio's, does).
void configureHeadless(const std::filesystem::path &root, bool watched = false) {
    if (watched) GET_MODULE(PathResolver).setup(root, false, readFile(root / "project.json"));
    else GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.shader_hot_reload = watched;
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

        // A key outside values registers the material again (MT-3) and
        // rebuilds the box that draws it.
        const auto tagged = rpc.result("edit_material", {{"base_material_digest", digest},
                                                         {"path", path},
                                                         {"tags", Json::array({"paint"})}});
        CHECK(tagged.at("applied") == "rebuilt");
        CHECK(tagged.at("restart_required").is_null());
        CHECK(tagged.at("rebuilt_models") == Json::array({std::string{cube}}));
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

// MT-1b. list_surfaces names what New material can be made on: the engine's
// OpenPBR surfaces and the project's own `.surface` files, wherever they are
// in the project, each counted as get_surface_schema reads it; one that does
// not parse is listed with its error.
TEST_CASE("MT-1b list_surfaces lists the engine's OpenPBR surfaces and the project's own over RPC",
          "[mt1][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("surfaces");
    const auto root = makeBlankBoxProject(dir.root);
    writeFile(root / "shaders/tint.surface",
              "//! pelican.surface v1\n"
              "//! language: glsl\n"
              "//! params:\n"
              "//!   - { name: tint, type: color, default: [1.0, 0.5, 0.25, 1.0] }\n"
              "//! textures:\n"
              "//!   - { name: detail, default: \"engine://textures/white\", color_space: srgb, role: color }\n"
              "//! render_state: { blend: blend, cull: back, depth: read_only, depth_compare: less_equal }\n"
              "\n"
              "void pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n");
    writeFile(root / "materials/deep/plain.surface",
              "//! pelican.surface v1\n"
              "//! language: glsl\n"
              "\n"
              "void pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n");
    writeFile(root / "shaders/broken.surface", "//! pelican.surface v1\n//! language: cobol\n");
    writeFile(root / ".hidden/skipped.surface",
              "//! pelican.surface v1\n//! language: glsl\n\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, "
              "inout PelicanSurfaceV1 s) {}\n");

    FastModuleContainer modules;
    configureHeadless(root);
    (void)GET_MODULE(Renderer);
    Rpc rpc;

    const auto surfaces = rpc.result("list_surfaces").at("surfaces");
    INFO(surfaces.dump(2));
    const auto find = [&](std::string_view reference) {
        for (const auto &entry : surfaces)
            if (entry.at("reference").get<std::string>() == reference) return entry;
        return Json{};
    };

    // The engine's six OpenPBR surfaces, first, named by their manifest.
    std::size_t engine_count = 0;
    bool engine_first = true;
    bool seen_project = false;
    for (const auto &entry : surfaces) {
        if (entry.at("source") == "engine") {
            ++engine_count;
            engine_first = engine_first && !seen_project;
        } else {
            CHECK(entry.at("source") == "project");
            seen_project = true;
        }
    }
    CHECK(engine_count >= 6);
    CHECK(engine_first);

    const auto opaque = find(openPbrOpaque);
    REQUIRE(opaque.is_object());
    CHECK(opaque.at("source") == "engine");
    CHECK(opaque.at("name") == "opaque_single_sided");
    // The counts are get_surface_schema's own.
    const auto schema = rpc.result("get_surface_schema", {{"surface", openPbrOpaque}});
    CHECK(opaque.at("params") == schema.at("params").size());
    CHECK(opaque.at("textures") == schema.at("textures").size());
    CHECK(opaque.at("params").get<std::size_t>() > 10);
    // routing is a material's, as create_material takes it; render_state the
    // header's declaration.
    CHECK(opaque.at("routing") == Json{{"alpha_mode", "opaque"}, {"double_sided", false}});
    CHECK(opaque.at("render_state") == Json{{"blend", "opaque"},
                                            {"cull", "back"},
                                            {"depth_test", true},
                                            {"depth_write", true},
                                            {"depth_compare", "less"}});
    const auto blend = find("engine://surfaces/openpbr/blend_double.surface");
    REQUIRE(blend.is_object());
    CHECK(blend.at("name") == "blend_double_sided");
    CHECK(blend.at("routing") == Json{{"alpha_mode", "blend"}, {"double_sided", true}});
    CHECK(blend.at("render_state") == Json{{"blend", "blend"},
                                           {"cull", "none"},
                                           {"depth_test", true},
                                           {"depth_write", false},
                                           {"depth_compare", "less"}});
    CHECK(find("engine://surfaces/openpbr/mask_single.surface").at("routing") ==
          Json{{"alpha_mode", "mask"}, {"double_sided", false}});

    // The project's, as project:// references, sorted, wherever they are.
    std::vector<std::string> project;
    for (const auto &entry : surfaces)
        if (entry.at("source") == "project") project.push_back(entry.at("reference").get<std::string>());
    CHECK(project == std::vector<std::string>{"project://materials/deep/plain.surface",
                                              "project://shaders/broken.surface",
                                              "project://shaders/tint.surface"});

    const auto tint = find("project://shaders/tint.surface");
    CHECK_FALSE(tint.contains("name"));
    CHECK_FALSE(tint.contains("error"));
    CHECK(tint.at("params") == 1);
    CHECK(tint.at("textures") == 1);
    // No manifest names it: no routing; its render_state as declared.
    CHECK_FALSE(tint.contains("routing"));
    CHECK(tint.at("render_state") == Json{{"blend", "blend"},
                                          {"cull", "back"},
                                          {"depth_test", true},
                                          {"depth_write", false},
                                          {"depth_compare", "less_equal"}});
    // A listed reference is one get_surface_schema reads.
    const auto tint_schema = rpc.result("get_surface_schema", {{"surface", "project://shaders/tint.surface"}});
    CHECK(tint_schema.at("params").at(0).at("name") == "tint");

    // No render_state declared: none listed.
    const auto plain = find("project://materials/deep/plain.surface");
    CHECK(plain.at("params") == 0);
    CHECK(plain.at("textures") == 0);
    CHECK_FALSE(plain.contains("routing"));
    CHECK_FALSE(plain.contains("render_state"));

    // One that does not parse is listed with its error, not dropped.
    const auto broken = find("project://shaders/broken.surface");
    REQUIRE(broken.contains("error"));
    CHECK(broken.at("error").get<std::string>().find("cobol") != std::string::npos);
    CHECK_FALSE(broken.contains("params"));

    // It takes no parameters.
    const auto refused = rpc.call("list_surfaces", {{"filter", "x"}});
    CHECK(refused.contains("error"));

    GET_MODULE(VulkanManageCore).waitIdle();
}

// MT-2b. The previous a bind_material answers, sent back as `bindings`, takes
// an engine primitive back to its own default: the box is grey again and
// asset_data is what it was before the bind (no models[] entry, no bindings
// document), so a later bind makes the same document again.
TEST_CASE("MT-2b bind_material's previous takes a Blank box back to the primitive's default over RPC",
          "[mt2b][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("unbind");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";

    FastModuleContainer modules;
    configureHeadless(root);
    (void)GET_MODULE(Renderer);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto actor =
        rpc.result("open_editor_session", {{"display_name", "MT-2b"}}).at("actor_id").get<std::uint64_t>();
    REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");
    const auto grey = rpc.centre(capture);
    INFO("grey " << grey);
    CHECK(grey.r < 230);
    CHECK_FALSE(reddish(grey));

    const Json red{1.0, 0.0, 0.0, 1.0};
    REQUIRE(rpc.result("create_material", {{"path", "materials/paint.material.json"},
                                           {"surface", openPbrOpaque},
                                           {"values", {{"base_color", red}, {"emission_color", red}}}})
                .at("applied") == "live");
    const auto asset_data_before = readFile(root / "assets/asset_data.json");
    const auto unbound = rpc.result("get_material_bindings", {{"model", cube}});
    CHECK(unbound.at("material_bindings").is_null());

    const auto bound = rpc.result("bind_material", {{"model", cube}, {"material", "paint"}});
    REQUIRE(bound.at("applied") == "live");
    const auto reference = bound.at("material_bindings").get<std::string>();
    CHECK(std::filesystem::exists(root / reference));
    {
        const auto pixel = rpc.centre(capture);
        INFO("bound " << pixel);
        CHECK(reddish(pixel));
    }

    // previous sent back as it was answered.
    const auto previous = bound.at("previous").at("bindings");
    CHECK(previous ==
          Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}}));
    const auto undone = rpc.result("bind_material", {{"model", cube}, {"bindings", previous}});
    CHECK(undone.at("applied") == "live");
    CHECK(undone.at("changed") == true);
    CHECK(undone.at("material_bindings").is_null());
    CHECK(undone.at("previous") ==
          Json{{"bindings", Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "paint"}}})}});
    {
        const auto pixel = rpc.centre(capture);
        INFO("undone " << pixel << " grey " << grey);
        CHECK(std::abs(pixel.r - grey.r) <= 2);
        CHECK(std::abs(pixel.g - grey.g) <= 2);
        CHECK(std::abs(pixel.b - grey.b) <= 2);
    }
    // The documents are what they were before the bind.
    CHECK(readFile(root / "assets/asset_data.json") == asset_data_before);
    CHECK_FALSE(std::filesystem::exists(root / reference));
    const auto after = rpc.result("get_material_bindings", {{"model", cube}});
    CHECK(after.at("material_bindings").is_null());
    CHECK(after.at("primitives") == unbound.at("primitives"));

    // Sending it again changes nothing; a new bind makes the same document.
    CHECK(rpc.result("bind_material", {{"model", cube}, {"bindings", previous}}).at("changed") == false);
    const auto again = rpc.result("bind_material", {{"model", cube}, {"material", "paint"}});
    CHECK(again.at("material_bindings") == reference);
    {
        const auto pixel = rpc.centre(capture);
        INFO("bound again " << pixel);
        CHECK(reddish(pixel));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

// MT-3. What edit_material changes outside values is applied without a
// restart: the box's material is registered again and the box rebuilt with it
// ("rebuilt"). A routing change is written into the bindings that name the
// material in the same transaction, so the next start loads the box; a
// routing taken off a bound material is refused by name, nothing written.
TEST_CASE("MT-3 a Blank box's material takes a new texture and blend routing over RPC without a restart",
          "[mt3][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("rebuilt");
    const auto root = makeBlankBoxProject(dir.root, true);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";
    {
        std::vector<unsigned char> blue_pixels;
        for (int index = 0; index < 16; ++index) blue_pixels.insert(blue_pixels.end(), {0, 0, 255, 255});
        std::filesystem::create_directories(root / "textures");
        REQUIRE(stbi_write_png((root / "textures/blue.png").string().c_str(), 4, 4, 4, blue_pixels.data(), 16) != 0);
    }
    const auto magentaish = [](const Rgb &rgb) { return rgb.r > rgb.g + 60 && rgb.b > rgb.g + 60; };
    // Half of the blue box over the white clear: the white shows through.
    const auto seeThroughBlue = [](const Rgb &rgb) { return rgb.r > 60 && rgb.g > 60 && rgb.b > rgb.r + 30; };
    std::string bindings_reference;

    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "MT-3"}}).at("actor_id").get<std::uint64_t>();
        REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");

        REQUIRE(rpc.result("create_material", {{"path", path},
                                               {"surface", openPbrOpaque},
                                               {"values",
                                                {{"base_color", {0.0, 0.0, 0.0, 1.0}},
                                                 {"emission_color", {1.0, 0.0, 1.0, 1.0}},
                                                 {"emission_luminance", 1.0}}}})
                    .at("applied") == "live");
        const auto bound = rpc.result("bind_material", {{"model", cube}, {"material", "paint"}});
        REQUIRE(bound.at("applied") == "live");
        bindings_reference = bound.at("material_bindings").get<std::string>();
        {
            const auto pixel = rpc.centre(capture);
            INFO("bound " << pixel);
            CHECK(magentaish(pixel));
        }

        // Stage 1: the bound material's routing taken off is refused by name;
        // the material and its bindings stay as they were.
        const auto digest = rpc.result("get_material", {{"path", path}}).at("material_digest").get<std::string>();
        const auto bindings_before = readFile(root / bindings_reference);
        const auto conflict =
            rpc.refusal("edit_material", {{"base_material_digest", digest}, {"path", path}, {"routing", nullptr}});
        INFO(conflict.dump(2));
        CHECK(conflict.at("error_code") == "material.routing_conflict@1");
        CHECK(conflict.at("error_fields") ==
              Json{{"path", path},
                   {"material", "paint"},
                   {"routing", nullptr},
                   {"bound_by",
                    Json::array({{{"model", cube}, {"routing", {{"alpha_mode", "opaque"}, {"double_sided", false}}}}})}});
        CHECK(rpc.result("get_material", {{"path", path}}).at("material_digest") == digest);
        CHECK(readFile(root / bindings_reference) == bindings_before);

        // (a) A texture: the box takes it in the next frame.
        const auto textured = rpc.result(
            "edit_material",
            {{"base_material_digest", digest},
             {"path", path},
             {"textures", {{"emission_color_map", "project://textures/blue.png"}}}});
        INFO(textured.dump(2));
        CHECK(textured.at("applied") == "rebuilt");
        CHECK(textured.at("restart_required").is_null());
        CHECK(textured.at("rebuilt_models") == Json::array({std::string{cube}}));
        CHECK(textured.at("rewritten_bindings") == Json::array());
        CHECK(textured.at("previous") == Json{{"textures", {{"emission_color_map", nullptr}}}});
        {
            const auto pixel = rpc.centre(capture);
            INFO("textured " << pixel);
            CHECK(bluish(pixel));
            CHECK(pixel.r < 60);
        }

        // (b) opaque -> blend (OpenPBR's blend surface with it), half opaque:
        // the binding takes the routing in the same write and the box is drawn
        // see-through in the next frame.
        const auto blended = rpc.result("edit_material",
                                        {{"base_material_digest", textured.at("material_digest")},
                                         {"path", path},
                                         {"surface", "engine://surfaces/openpbr/blend_single.surface"},
                                         {"routing", {{"alpha_mode", "blend"}, {"double_sided", false}}},
                                         {"values", {{"geometry_opacity", 0.5}}}});
        INFO(blended.dump(2));
        CHECK(blended.at("applied") == "rebuilt");
        CHECK(blended.at("rebuilt_models") == Json::array({std::string{cube}}));
        REQUIRE(blended.at("rewritten_bindings").size() == 1);
        const auto &rewritten = blended.at("rewritten_bindings").at(0);
        CHECK(rewritten.at("model") == std::string{cube});
        CHECK(rewritten.at("material_bindings") == bindings_reference);
        CHECK(rewritten.at("bindings_digest") ==
              rpc.result("get_material_bindings", {{"model", cube}}).at("bindings_digest"));
        CHECK(blended.at("previous").at("routing") == Json{{"alpha_mode", "opaque"}, {"double_sided", false}});
        const auto bindings = Json::parse(readFile(root / bindings_reference));
        CHECK(bindings.at("bindings").at(0).at("routing") == Json{{"alpha_mode", "blend"}, {"double_sided", false}});
        {
            const auto pixel = rpc.centre(capture);
            INFO("blended " << pixel);
            CHECK(seeThroughBlue(pixel));
        }
        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The next start loads the box with the rewritten binding and draws it
    // the same.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") == "paint");
        {
            const auto pixel = rpc.centre(capture);
            INFO("next start " << pixel);
            CHECK(seeThroughBlue(pixel));
        }
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

// VR-1. Studio's material dock sends the surface and the routing each on its
// own. An OpenPBR material's surface variant and its routing are one choice
// (the surface manifest pairs them), so the half not sent follows the half
// sent, in the same write: the bindings take the routing (MT-3) and the box is
// rebuilt. Both sent and not a pair is refused by name, nothing written.
TEST_CASE("VR-1 a Blank box's OpenPBR material changes variant from the surface or the routing alone over RPC",
          "[vr1][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("variant");
    const auto root = makeBlankBoxProject(dir.root, true);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";
    constexpr std::string_view openPbrBlend = "engine://surfaces/openpbr/blend_single.surface";
    constexpr std::string_view openPbrOpaqueDouble = "engine://surfaces/openpbr/opaque_double.surface";
    const Json opaqueSingle{{"alpha_mode", "opaque"}, {"double_sided", false}};
    const Json blendSingle{{"alpha_mode", "blend"}, {"double_sided", false}};
    const Json opaqueDouble{{"alpha_mode", "opaque"}, {"double_sided", true}};
    // The magenta box drawn solid, and half of it over the white clear.
    const auto solidMagenta = [](const Rgb &rgb) { return rgb.r > rgb.g + 120 && rgb.b > rgb.g + 120; };
    const auto seeThroughMagenta = [](const Rgb &rgb) {
        return rgb.g > 60 && rgb.r > rgb.g + 30 && rgb.b > rgb.g + 30;
    };
    std::string bindings_reference;

    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "VR-1"}}).at("actor_id").get<std::uint64_t>();
        REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");
        // As Studio's New material makes it: the surface with list_surfaces'
        // routing for it, half opaque (an opaque surface draws it solid).
        REQUIRE(rpc.result("create_material", {{"path", path},
                                               {"surface", openPbrOpaque},
                                               {"routing", opaqueSingle},
                                               {"values",
                                                {{"base_color", {0.0, 0.0, 0.0, 1.0}},
                                                 {"emission_color", {1.0, 0.0, 1.0, 1.0}},
                                                 {"emission_luminance", 1.0},
                                                 {"geometry_opacity", 0.5}}}})
                    .at("applied") == "live");
        const auto bound = rpc.result("bind_material", {{"model", cube}, {"material", "paint"}});
        REQUIRE(bound.at("applied") == "live");
        bindings_reference = bound.at("material_bindings").get<std::string>();
        {
            const auto pixel = rpc.centre(capture);
            INFO("bound " << pixel);
            CHECK(solidMagenta(pixel));
        }

        // (a) The surface alone, as the dock's Surface box sends it: the
        // routing follows the blend surface, and the binding with it.
        const auto digest = rpc.result("get_material", {{"path", path}}).at("material_digest").get<std::string>();
        const auto blended = rpc.result(
            "edit_material", {{"base_material_digest", digest}, {"path", path}, {"surface", openPbrBlend}});
        INFO(blended.dump(2));
        CHECK(blended.at("committed") == true);
        CHECK(blended.at("applied") == "rebuilt");
        CHECK(blended.at("restart_required").is_null());
        CHECK(blended.at("rebuilt_models") == Json::array({std::string{cube}}));
        CHECK(blended.at("previous") == Json{{"surface", openPbrOpaque}, {"routing", opaqueSingle}});
        REQUIRE(blended.at("rewritten_bindings").size() == 1);
        CHECK(blended.at("rewritten_bindings").at(0).at("material_bindings") == bindings_reference);
        {
            const auto read = rpc.result("get_material", {{"path", path}});
            CHECK(read.at("material_digest") == blended.at("material_digest"));
            CHECK(read.at("material").at("surface") == std::string{openPbrBlend});
            CHECK(read.at("material").at("routing") == blendSingle);
            const auto bindings = Json::parse(readFile(root / bindings_reference));
            CHECK(bindings.at("bindings").at(0).at("routing") == blendSingle);
            const auto pixel = rpc.centre(capture);
            INFO("surface alone " << pixel);
            CHECK(seeThroughMagenta(pixel));
        }

        // (b) Both sent and not a pair: refused by name, nothing written.
        const auto bindings_before = readFile(root / bindings_reference);
        const auto mismatch = rpc.refusal("edit_material", {{"base_material_digest", blended.at("material_digest")},
                                                            {"path", path},
                                                            {"surface", openPbrBlend},
                                                            {"routing", opaqueSingle}});
        INFO(mismatch.dump(2));
        CHECK(mismatch.at("error_code") == "material.document_invalid@1");
        CHECK(mismatch.at("error_fields").at("path") == path);
        CHECK(mismatch.at("error_fields").at("reason").get<std::string>().find("does not match surface") !=
              std::string::npos);
        CHECK(rpc.result("get_material", {{"path", path}}).at("material_digest") == blended.at("material_digest"));
        CHECK(readFile(root / bindings_reference) == bindings_before);

        // (c) The routing alone, as the dock's Alpha mode row sends it: the
        // surface follows to the OpenPBR variant of that routing.
        const auto doubled = rpc.result("edit_material", {{"base_material_digest", blended.at("material_digest")},
                                                          {"path", path},
                                                          {"routing", opaqueDouble}});
        INFO(doubled.dump(2));
        CHECK(doubled.at("applied") == "rebuilt");
        CHECK(doubled.at("rebuilt_models") == Json::array({std::string{cube}}));
        CHECK(doubled.at("previous") == Json{{"surface", openPbrBlend}, {"routing", blendSingle}});
        REQUIRE(doubled.at("rewritten_bindings").size() == 1);
        {
            const auto read = rpc.result("get_material", {{"path", path}});
            CHECK(read.at("material").at("surface") == std::string{openPbrOpaqueDouble});
            CHECK(read.at("material").at("routing") == opaqueDouble);
            const auto bindings = Json::parse(readFile(root / bindings_reference));
            CHECK(bindings.at("bindings").at(0).at("routing") == opaqueDouble);
            const auto pixel = rpc.centre(capture);
            INFO("routing alone " << pixel);
            CHECK(solidMagenta(pixel));
        }
        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The next start loads the box with the material and binding written.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") == "paint");
        CHECK(rpc.result("get_material", {{"path", path}}).at("material").at("surface") ==
              std::string{openPbrOpaqueDouble});
        {
            const auto pixel = rpc.centre(capture);
            INFO("next start " << pixel);
            CHECK(solidMagenta(pixel));
        }
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

// EB-1. A Blank box's material is renamed while it is bound (the bindings
// follow in the same transaction and the box keeps drawing it), refused
// deletion while anything names it, deleted once nothing does (list_assets
// and list_materials stop listing it, the document stays on disk), and put
// back with register_material.
TEST_CASE("EB-1 a Blank box's material is renamed while bound, deleted once unbound and put back over RPC",
          "[eb1][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("eb1");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";

    FastModuleContainer modules;
    configureHeadless(root);
    (void)GET_MODULE(Renderer);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto actor =
        rpc.result("open_editor_session", {{"display_name", "EB-1"}}).at("actor_id").get<std::uint64_t>();
    REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");
    const auto grey = rpc.centre(capture);
    INFO("grey " << grey);
    CHECK_FALSE(reddish(grey));

    const Json red{1.0, 0.0, 0.0, 1.0};
    REQUIRE(rpc.result("create_material", {{"path", path},
                                           {"surface", openPbrOpaque},
                                           {"values", {{"base_color", red}, {"emission_color", red}}}})
                .at("applied") == "live");
    REQUIRE(rpc.result("bind_material", {{"model", cube}, {"material", "paint"}}).at("applied") == "live");
    {
        const auto pixel = rpc.centre(capture);
        INFO("bound " << pixel);
        CHECK(reddish(pixel));
    }

    const auto materialAssets = [&] {
        auto found = Json::array();
        const auto listed = rpc.result("list_assets");
        for (const auto &asset : listed.at("assets"))
            if (asset.at("kind") == "material") found.push_back(asset);
        return found;
    };
    const auto resolved = [&](const std::string &relative) {
        return std::filesystem::weakly_canonical(root / relative).generic_string();
    };
    // list_assets lists the material by its name (EB-1 #2): the path as
    // asset_data writes it, the file it resolved beside it.
    {
        const auto listed = materialAssets();
        REQUIRE(listed.size() == 1);
        CHECK(listed.at(0).at("id") == "paint");
        CHECK(listed.at(0).at("path") == path);
        CHECK(listed.at(0).at("store") == "project");
        CHECK(listed.at(0).at("status") == "loaded");
        CHECK(std::filesystem::weakly_canonical(listed.at(0).at("resolved_path").get<std::string>()).generic_string() ==
              resolved(path));
    }

    // Deleting a bound material is refused by name; nothing is written.
    const auto asset_data_bound = readFile(root / "assets/asset_data.json");
    const auto in_use = rpc.refusal("delete_material", {{"name", "paint"}});
    CHECK(in_use.at("error_code") == "material.in_use@1");
    CHECK(in_use.at("error_fields") == Json{{"name", "paint"},
                                            {"bindings", Json::array({std::string{cube}})},
                                            {"objects", Json::array()},
                                            {"count", 0},
                                            {"render_documents", Json::array()}});
    CHECK(readFile(root / "assets/asset_data.json") == asset_data_bound);
    CHECK(rpc.refusal("delete_material", {{"name", "pelican.primitive.default"}}).at("error_fields") ==
          Json{{"name", "pelican.primitive.default"}, {"reason", "engine_material"}});
    CHECK(rpc.refusal("delete_material", {{"name", "nothing"}}).at("error_code") == "material.name_unknown@1");

    // rename_material: the document and the box's bindings in one go; the box
    // keeps drawing it.
    const auto bindings_reference =
        rpc.result("get_material_bindings", {{"model", cube}}).at("material_bindings").get<std::string>();
    const auto renamed = rpc.result("rename_material", {{"name", "paint"}, {"new_name", "coat"}});
    CHECK(renamed.at("committed") == true);
    CHECK(renamed.at("changed") == true);
    CHECK(renamed.at("name") == "coat");
    CHECK(renamed.at("previous_name") == "paint");
    CHECK(renamed.at("path") == path);
    CHECK(renamed.at("applied") == "rebuilt");
    CHECK(renamed.at("restart_required").is_null());
    CHECK(renamed.at("rebuilt_models") == Json::array({std::string{cube}}));
    REQUIRE(renamed.at("rewritten_bindings").size() == 1);
    CHECK(renamed.at("rewritten_bindings").at(0).at("model") == std::string{cube});
    CHECK(renamed.at("rewritten_bindings").at(0).at("material_bindings") == bindings_reference);
    CHECK(Json::parse(readFile(root / path)).at("materials").at(0).at("name") == "coat");
    CHECK(Json::parse(readFile(root / bindings_reference)).at("bindings").at(0).at("material") == "coat");
    CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") == "coat");
    CHECK(rpc.result("list_materials").at("documents").at(0).at("materials").at(0).at("registered") == true);
    {
        const auto pixel = rpc.centre(capture);
        INFO("renamed " << pixel);
        CHECK(reddish(pixel));
    }
    CHECK(materialAssets().at(0).at("id") == "coat");
    // The old name is gone; a name held already and a name that is not one
    // are refused by name.
    CHECK(rpc.refusal("rename_material", {{"name", "paint"}, {"new_name", "other"}}).at("error_code") ==
          "material.name_unknown@1");
    CHECK(rpc.refusal("rename_material", {{"name", "coat"}, {"new_name", "pelican.x"}}).at("error_code") ==
          "material.name_invalid@1");
    REQUIRE(rpc.result("create_material", {{"path", "materials/spare.material.json"}, {"surface", openPbrOpaque}})
                .at("applied") == "live");
    const auto conflict = rpc.refusal("rename_material", {{"name", "coat"}, {"new_name", "spare"}});
    CHECK(conflict.at("error_code") == "material.name_conflict@1");
    CHECK(conflict.at("error_fields") == Json{{"name", "spare"}, {"path", "materials/spare.material.json"}});
    CHECK(rpc.result("rename_material", {{"name", "coat"}, {"new_name", "coat"}}).at("changed") == false);

    // A render document that names the material by its matching key holds it
    // too (a rename would not rewrite it).
    writeFile(root / "passes/eb1_reference.json",
              Json{{"schema", "pelican.graph"},
                   {"version", 1},
                   {"nodes", Json::array({{{"id", "loop"},
                                           {"op", "graph.loop"},
                                           {"parameters", {{"when", {{"material", path + "#coat"}}}}}}})}}
                  .dump(2));
    const auto named = rpc.refusal("rename_material", {{"name", "coat"}, {"new_name", "lacquer"}});
    CHECK(named.at("error_code") == "material.in_use@1");
    CHECK(named.at("error_fields").at("render_documents") == Json::array({"passes/eb1_reference.json"}));
    std::filesystem::remove(root / "passes/eb1_reference.json");

    // Unbound (MT-2b), it is deleted: off asset_data and the engine, not off
    // the disk.
    REQUIRE(rpc.result("bind_material", {{"model", cube},
                                         {"slot", "pelican.primitive.default"},
                                         {"material", "pelican.primitive.default"}})
                .at("material_bindings")
                .is_null());
    const auto deleted = rpc.result("delete_material", {{"name", "coat"}});
    INFO(deleted.dump(2));
    CHECK(deleted.at("committed") == true);
    CHECK(deleted.at("name") == "coat");
    CHECK(deleted.at("removed") == Json{{"path", path}});
    CHECK(deleted.at("applied") == "live");
    CHECK(deleted.at("restart_required").is_null());
    CHECK(deleted.at("asset_data_digest") != deleted.at("previous_asset_data_digest"));
    CHECK(std::filesystem::exists(root / path));
    {
        const auto documents = rpc.result("list_materials");
        for (const auto &document : documents.at("documents")) CHECK(document.at("path") != path);
    }
    {
        const auto listed = materialAssets();
        REQUIRE(listed.size() == 1);
        CHECK(listed.at(0).at("id") == "spare");
    }
    CHECK(rpc.refusal("bind_material", {{"model", cube}, {"material", "coat"}}).at("error_code") ==
          "material.name_unknown@1");
    {
        const auto pixel = rpc.centre(capture);
        INFO("deleted " << pixel << " grey " << grey);
        CHECK_FALSE(reddish(pixel));
    }

    // Put back from what the delete answered: listed, registered and bound
    // again.
    const auto restored = rpc.result("register_material", {{"path", deleted.at("removed").at("path")}});
    CHECK(restored.at("committed") == true);
    CHECK(restored.at("names") == Json::array({"coat"}));
    CHECK(restored.at("applied") == "live");
    CHECK(restored.at("asset_data_digest") != restored.at("previous_asset_data_digest"));
    CHECK(rpc.refusal("register_material", {{"path", path}}).at("error_code") == "material.path_conflict@1");
    CHECK(materialAssets().size() == 2);
    REQUIRE(rpc.result("bind_material", {{"model", cube}, {"material", "coat"}}).at("applied") == "live");
    {
        const auto pixel = rpc.centre(capture);
        INFO("restored " << pixel);
        CHECK(reddish(pixel));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

// FO-1. A material document asset_data lists that does not read does not stop
// the engine: it starts without that document's materials, lists it in the
// problem list (and list_materials says so), and the box bound to its material
// draws with its own. --strict-assets keeps the start failing.
TEST_CASE("FO-1 a Blank box whose material document is broken starts, is listed in get_problems and draws with "
          "its own material",
          "[fo1][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("fo1");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";

    // A red material bound to the box, written as MT-1 writes it.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "FO-1"}}).at("actor_id").get<std::uint64_t>();
        REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");
        const Json red{1.0, 0.0, 0.0, 1.0};
        REQUIRE(rpc.result("create_material", {{"path", path},
                                               {"surface", openPbrOpaque},
                                               {"values", {{"base_color", red}, {"emission_color", red}}}})
                    .at("applied") == "live");
        REQUIRE(rpc.result("bind_material", {{"model", cube}, {"material", "paint"}}).at("applied") == "live");
        const auto pixel = rpc.centre(capture);
        INFO("bound " << pixel);
        REQUIRE(reddish(pixel));
        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }
    const auto document = readFile(root / path);
    const auto problemsOf = [](Rpc &rpc, std::string_view code) {
        auto found = Json::array();
        const auto listed = rpc.result("get_problems");
        for (const auto &problem : listed.at("problems"))
            if (problem.at("code") == std::string{code}) found.push_back(problem);
        return found;
    };
    // Started on the documents as they are: the box draws grey, with the
    // primitive's own material, and the problem list has the document and the
    // binding that could not be kept.
    const auto startBroken = [&](std::string_view status, std::string_view code) {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        CHECK(rpc.result("scene_tree").dump().find("Box") != std::string::npos);
        {
            const auto pixel = rpc.centre(capture);
            INFO("broken " << pixel);
            CHECK_FALSE(reddish(pixel));
            CHECK(pixel.r < 230);
        }
        // The bindings document is kept as it is written.
        CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") ==
              "paint");

        const auto refused = problemsOf(rpc, code);
        INFO(rpc.result("get_problems").dump(2));
        REQUIRE(refused.size() == 1);
        const auto &problem = refused.at(0);
        CHECK(problem.at("severity") == "error");
        CHECK(problem.at("source") == "startup");
        CHECK(problem.at("document") == "project://" + path);
        CHECK(problem.at("document_kind") == "material");
        CHECK(problem.at("fields").at("path") == path);
        CHECK_FALSE(problem.at("fields").at("reason").get<std::string>().empty());
        const auto binding = problemsOf(rpc, "material.binding_unresolved@1");
        REQUIRE(binding.size() == 1);
        CHECK(binding.at(0).at("severity") == "warning");
        CHECK(binding.at(0).at("document_kind") == "material");
        CHECK(binding.at(0).at("fields").at("model") == std::string{cube});
        CHECK(binding.at(0).at("fields").at("materials") == Json::array({"paint"}));
        const auto summary = rpc.result("get_status").at("problems");
        CHECK(summary.at("errors") == 1);
        CHECK(summary.at("warnings") == 1);

        const auto listed = rpc.result("list_materials").at("documents");
        REQUIRE(listed.size() == 1);
        CHECK(listed.at(0).at("path") == path);
        CHECK(listed.at(0).at("status") == std::string{status});
        CHECK(listed.at(0).at("reason") == problem.at("fields").at("reason"));
        GET_MODULE(VulkanManageCore).waitIdle();
        return problem;
    };

    SECTION("not JSON: invalid, with the line and column") {
        writeFile(root / path, document.substr(0, document.size() / 2));
        const auto problem = startBroken("invalid", "material.document_invalid@1");
        CHECK(problem.at("line").get<int>() >= 1);
        CHECK(problem.at("column").get<int>() >= 1);
    }
    SECTION("a surface that does not load: invalid, with the registration's reason") {
        auto broken = Json::parse(document);
        broken.at("materials").at(0)["surface"] = "engine://surfaces/openpbr/nothing_here.surface";
        writeFile(root / path, broken.dump(2));
        const auto problem = startBroken("invalid", "material.document_invalid@1");
        CHECK_FALSE(problem.contains("line"));
        CHECK(problem.at("fields").at("reason").get<std::string>().find("nothing_here") != std::string::npos);
    }
    SECTION("gone: missing") {
        std::filesystem::remove(root / path);
        (void)startBroken("missing", "material.document_unknown@1");
    }
    SECTION("--strict-assets: the start fails as before") {
        writeFile(root / path, document.substr(0, document.size() / 2));
        FastModuleContainer modules;
        configureHeadless(root);
        GET_MODULE(EngineLaunchConfig).strict_assets = true;
        (void)GET_MODULE(Renderer);
        CHECK_THROWS((void)GET_MODULE(ProjectMaterialAssetContainer));
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

namespace {

// EB-2: a Blank box bound to a red material (as FO-1 makes it), saved; the
// document's bytes as written.
std::string makeRedBoxProject(const std::filesystem::path &root, const std::filesystem::path &capture,
                              const std::string &path) {
    FastModuleContainer modules;
    configureHeadless(root);
    (void)GET_MODULE(Renderer);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    const auto actor =
        rpc.result("open_editor_session", {{"display_name", "EB-2"}}).at("actor_id").get<std::uint64_t>();
    REQUIRE(rpc.edit(actor, spawnCube()).at("status") == "committed");
    const Json red{1.0, 0.0, 0.0, 1.0};
    REQUIRE(rpc.result("create_material",
                       {{"path", path},
                        {"surface", openPbrOpaque},
                        {"values", {{"base_color", red}, {"emission_color", red}, {"emission_luminance", 1.0}}}})
                .at("applied") == "live");
    REQUIRE(rpc.result("bind_material", {{"model", cube}, {"material", "paint"}}).at("applied") == "live");
    const auto pixel = rpc.centre(capture);
    INFO("bound " << pixel);
    REQUIRE(reddish(pixel));
    (void)rpc.result("save_scene");
    GET_MODULE(VulkanManageCore).waitIdle();
    return readFile(root / path);
}

Json problemsWithCode(Rpc &rpc, std::string_view code) {
    auto found = Json::array();
    const auto listed = rpc.result("get_problems");
    for (const auto &problem : listed.at("problems"))
        if (problem.at("code") == std::string{code}) found.push_back(problem);
    return found;
}

// The engine started on a broken document: the box draws its own grey, the
// document and the binding are listed, list_materials says invalid.
void requireStartedBroken(Rpc &rpc, const std::filesystem::path &capture, const std::string &path) {
    const auto pixel = rpc.centre(capture);
    INFO("broken " << pixel);
    CHECK_FALSE(reddish(pixel));
    CHECK(pixel.r < 230);
    INFO(rpc.result("get_problems").dump(2));
    REQUIRE(problemsWithCode(rpc, "material.document_invalid@1").size() == 1);
    REQUIRE(problemsWithCode(rpc, "material.binding_unresolved@1").size() == 1);
    const auto listed = rpc.result("list_materials").at("documents");
    REQUIRE(listed.size() == 1);
    CHECK(listed.at(0).at("path") == path);
    CHECK(listed.at(0).at("status") == "invalid");
}

// Taken in: list_materials loaded, the problem list empty, the box red.
void requireRecovered(Rpc &rpc, const std::filesystem::path &capture) {
    const auto listed = rpc.result("list_materials").at("documents");
    REQUIRE(listed.size() == 1);
    CHECK(listed.at(0).at("status") == "loaded");
    REQUIRE(listed.at(0).at("materials").size() == 1);
    CHECK(listed.at(0).at("materials").at(0).at("registered") == true);
    const auto problems = rpc.result("get_problems");
    INFO(problems.dump(2));
    CHECK(problems.at("problems").empty());
    const auto summary = rpc.result("get_status").at("problems");
    CHECK(summary.at("errors") == 0);
    CHECK(summary.at("warnings") == 0);
    const auto pixel = rpc.centre(capture);
    INFO("taken in " << pixel);
    CHECK(reddish(pixel));
}

} // namespace

// EB-2. A material document refused at start is taken in by register_material
// once the file is fixed, without a restart: the problem and the binding's
// warning leave the list and the box draws the material. Called on the file
// still broken, it is refused by the parser's code and nothing changes.
TEST_CASE("EB-2 a material document refused at start is taken in by register_material once fixed, without a restart",
          "[eb2][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("eb2");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";
    const auto document = makeRedBoxProject(root, capture, path);
    const auto asset_data = readFile(root / "assets/asset_data.json");
    writeFile(root / path, document.substr(0, document.size() / 2));

    FastModuleContainer modules;
    configureHeadless(root);
    (void)GET_MODULE(Renderer);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    requireStartedBroken(rpc, capture, path);

    // Still broken: refused by the parser's code, the list and the box as they were.
    const auto refused = rpc.refusal("register_material", {{"path", path}});
    INFO(refused.dump(2));
    CHECK(refused.at("error_code") == "material.document_invalid@1");
    CHECK(refused.at("error_fields").at("path") == path);
    CHECK_FALSE(refused.at("error_fields").at("reason").get<std::string>().empty());
    requireStartedBroken(rpc, capture, path);

    // Fixed on disk, taken in live.
    writeFile(root / path, document);
    const auto recovered = rpc.result("register_material", {{"path", path}});
    INFO(recovered.dump(2));
    CHECK(recovered.at("recovered") == true);
    CHECK(recovered.at("committed") == true);
    CHECK(recovered.at("applied") == "live");
    CHECK(recovered.at("restart_required").is_null());
    CHECK(recovered.at("path") == path);
    CHECK(recovered.at("names") == Json::array({"paint"}));
    CHECK(recovered.at("rebuilt_models") == Json::array({std::string{cube}}));
    CHECK(recovered.at("asset_data_digest") == recovered.at("previous_asset_data_digest"));
    requireRecovered(rpc, capture);
    CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("primitives").at(0).at("material") == "paint");
    // asset_data is not written; the document is listed already now.
    CHECK(readFile(root / "assets/asset_data.json") == asset_data);
    CHECK(rpc.refusal("register_material", {{"path", path}}).at("error_code") == "material.path_conflict@1");
    // The material edits as any registered one.
    const auto digest = rpc.result("get_material", {{"path", path}}).at("material_digest");
    const Json blue{0.0, 0.0, 1.0, 1.0};
    CHECK(rpc.result("edit_material", {{"base_material_digest", digest},
                                       {"path", path},
                                       {"values", {{"base_color", blue}, {"emission_color", blue}}}})
              .at("applied") == "live");
    {
        const auto pixel = rpc.centre(capture);
        INFO("edited " << pixel);
        CHECK(bluish(pixel));
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

// EB-2. The same through the file watch: a project whose hot_reload covers
// the material folder (Studio's --rpc engine watches what is declared, HW-1)
// takes the fixed document in when the file is saved. A registered document
// rewritten after start into one that does not read is refused by the values
// reload -- the box keeps the values read before -- and listed until a save
// that reads.
TEST_CASE("EB-2 a declared watch takes a fixed material document in, and lists one broken after start",
          "[eb2][material][rpc][hw1][gpu]") {
#if !PELICAN_WITH_HOT_RELOAD
    SKIP("PELICAN_WITH_HOT_RELOAD=OFF: there is no file watch; register_material takes a fixed document in (the EB-2 case above)");
#else
    using namespace std::chrono_literals;
    setupLogger(true);
    const auto dir = makeTempDir("eb2_watch");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    const std::string path = "materials/paint.material.json";
    const auto document = makeRedBoxProject(root, capture, path);
    auto project = Json::parse(readFile(root / "project.json"));
    project["hot_reload"] = Json::array({"materials"});
    writeFile(root / "project.json", project.dump(2));
    writeFile(root / path, document.substr(0, document.size() / 2));
    // Older than any write below, so the watch's first inventory is not
    // confused with them.
    std::filesystem::last_write_time(root / path,
                                     std::filesystem::file_time_type::clock::now() - std::chrono::minutes{60});

    FastModuleContainer modules;
    // The declaration opens the watch under --rpc.
    configureHeadless(root, true);
    GET_MODULE(watch::ReloadGate).configureFromLaunch(GET_MODULE(EngineLaunchConfig));
    (void)GET_MODULE(Renderer);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("step_frame");
    // As the player does once the startup resources are loaded.
    (void)GET_MODULE(ProjectMaterialAssetContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    reload.setup(GET_MODULE(PathResolver));
    const auto wait = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            (void)rpc.result("step_frame");
            if (predicate()) return true;
            std::this_thread::sleep_for(50ms);
        }
        return predicate();
    };
    REQUIRE(wait([&] { return reload.statusJson().at("state") == "watching"; }));
    CHECK(reload.statusJson().at("watch") == Json::array({"materials"}));
    requireStartedBroken(rpc, capture, path);

    // Saved fixed: the watch takes it in.
    writeFile(root / path, document);
    REQUIRE(wait([&] {
        return rpc.result("list_materials").at("documents").at(0).at("status") == "loaded";
    }));
    requireRecovered(rpc, capture);

    // Broken after start: the values reload refuses it, the box keeps red,
    // and the problem list has it (source hot_reload).
    writeFile(root / path, document.substr(0, document.size() / 2));
    REQUIRE(wait([&] { return !problemsWithCode(rpc, "material.document_invalid@1").empty(); }));
    {
        const auto problems = problemsWithCode(rpc, "material.document_invalid@1");
        INFO(problems.dump(2));
        REQUIRE(problems.size() == 1);
        CHECK(problems.at(0).at("source") == "hot_reload");
        CHECK(problems.at(0).at("severity") == "error");
        CHECK(problems.at(0).at("document") == "project://" + path);
        CHECK(problems.at(0).at("document_kind") == "material");
        CHECK(problems.at(0).at("line").get<int>() >= 1);
        CHECK(problems.at(0).at("fields").at("path") == path);
        CHECK(problemsWithCode(rpc, "material.binding_unresolved@1").empty());
        const auto pixel = rpc.centre(capture);
        INFO("broken after start " << pixel);
        CHECK(reddish(pixel));
    }

    // Saved again in a form that reads (blue): taken in, the problem gone.
    auto blue = Json::parse(document);
    blue.at("materials").at(0).at("values")["base_color"] = Json{0.0, 0.0, 1.0, 1.0};
    blue.at("materials").at(0).at("values")["emission_color"] = Json{0.0, 0.0, 1.0, 1.0};
    writeFile(root / path, blue.dump(2));
    REQUIRE(wait([&] { return problemsWithCode(rpc, "material.document_invalid@1").empty(); }));
    CHECK(rpc.result("get_problems").at("problems").empty());
    {
        const auto pixel = rpc.centre(capture);
        INFO("fixed again " << pixel);
        CHECK(bluish(pixel));
    }
    reload.watcherForTesting()->stop();
    GET_MODULE(VulkanManageCore).waitIdle();
#endif
}

} // namespace Pelican
