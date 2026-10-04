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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <stb_image.h>
#include <string>
#include <utility>

// MT-2. Two Blank boxes share engine://primitives/cube; one of them is given
// its own material over the engine's RPC (set_object_material) and is drawn
// in it in the next frame while the other keeps the model's; the edit is
// undone, redone, taken back with its `previous`, put on one primitive, and
// saved with the scene, which the next start of the engine draws.

namespace Pelican {
namespace {

using Json = nlohmann::json;

constexpr std::string_view openPbrOpaque = "engine://surfaces/openpbr/opaque_single.surface";
constexpr std::string_view cube = "engine://primitives/cube";
constexpr std::string_view primitiveDefault = "pelican.primitive.default";

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
                   ("pelican_mt2_" + std::string{name} + "_" + std::to_string(suffix))};
}

// The Blank template with one forward material pass (cleared to white) and
// the template's own `present` reading its colour (as MT-1's test).
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

// 128 x 64: the camera at z = 3 (yfov 45 degrees) sees a box at x = -1 left
// of the image's quarter and one at x = +1 right of its three quarters.
void configureHeadless(const std::filesystem::path &root) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{128, 64};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    TestSupport::requireVulkanDevice("MT-2 object materials");
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

bool reddish(const Rgb &rgb) { return rgb.r > rgb.g + 60 && rgb.r > rgb.b + 60; }
// The box in the model's grey default, on the white clear.
bool grey(const Rgb &rgb) { return rgb.r < 230 && !reddish(rgb); }

struct Boxes {
    Rgb left;
    Rgb right;
};

std::ostream &operator<<(std::ostream &out, const Boxes &boxes) {
    return out << "left " << boxes.left << ", right " << boxes.right;
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 8000;

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
    Json refusal(std::string_view method, Json params) {
        const auto response = call(method, std::move(params));
        INFO(method << " -> " << response.dump(2));
        REQUIRE(response.contains("error"));
        REQUIRE(response.at("error").contains("data"));
        return response.at("error").at("data");
    }
    // The two boxes' pixels in the next frame, through the capture RPC.
    Boxes boxes(const std::filesystem::path &capture) {
        (void)result("render_frame");
        std::filesystem::remove(capture);
        (void)result("capture", {{"path", capture.generic_string()}});
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(capture.string().c_str(), &width, &height, &channels, 4);
        REQUIRE(pixels != nullptr);
        const auto at = [&](int x) {
            const auto *pixel = pixels + (static_cast<std::size_t>(height / 2) * width + x) * 4;
            return Rgb{pixel[0], pixel[1], pixel[2]};
        };
        const Boxes boxes{at(width / 4), at(width * 3 / 4)};
        stbi_image_free(pixels);
        return boxes;
    }
    // A ticket the editor accepted, committed at the next frame boundary.
    Json commit(const Json &accepted) {
        INFO(accepted.dump(2));
        REQUIRE(accepted.at("status") == "accepted");
        (void)result("step_frame");
        const auto committed = result("get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO(committed.dump(2));
        REQUIRE(committed.at("status") == "committed");
        return committed;
    }
    Json edit(std::uint64_t actor, Json operations) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        return commit(result("edit", {{"actor_id", actor}, {"base_revision", revision}, {"operations", std::move(operations)}}));
    }
    Json revert(std::string_view method, std::uint64_t actor) {
        const auto revision = result("get_scene_revision").at("scene_revision");
        return commit(result(method, {{"actor_id", actor}, {"base_revision", revision}}));
    }
};

Json spawnBox(std::string_view name, double x) {
    return {{"op", "spawn"},
            {"scene_id", "default_scene"},
            {"object",
             {{"name", name},
              {"components",
               Json::array({{{"name", "transform"},
                             {"pos", {x, 0.0, 0.0}},
                             {"rotation", {0.0, 0.0, 0.0, 1.0}},
                             {"scale", {1.0, 1.0, 1.0}}},
                            {{"name", "simplemodelview"}, {"model", cube}}})}}}};
}

std::uint64_t objectId(Rpc &rpc, std::string_view name) {
    const auto tree = rpc.result("scene_tree");
    for (const auto &object : tree.at("objects"))
        if (object.value("name", std::string{}) == name) return object.at("authoring_object_id").get<std::uint64_t>();
    FAIL("no object named " << name);
    return 0;
}

// The draws the forward pass issued per material in the last frame (MP-1).
std::map<std::string, std::uint64_t> forwardDraws(Rpc &rpc) {
    std::map<std::string, std::uint64_t> draws;
    const auto plan = rpc.result("get_frame_plan");
    for (const auto &node : plan.at("nodes"))
        if (node.value("name", std::string{}) == "forward" && node.contains("drawn_materials"))
            for (const auto &entry : node.at("drawn_materials"))
                draws[entry.value("material", std::string{})] += entry.at("draws").get<std::uint64_t>();
    return draws;
}

} // namespace

TEST_CASE("MT-2 one of two Blank boxes takes its own material over RPC, drawn in the next frame and kept by save",
          "[mt2][material][rpc][gpu]") {
    setupLogger(true);
    const auto dir = makeTempDir("boxes");
    const auto root = makeBlankBoxProject(dir.root);
    const auto capture = dir.root / "capture.png";
    std::uint64_t right = 0;

    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        const auto actor =
            rpc.result("open_editor_session", {{"display_name", "MT-2"}}).at("actor_id").get<std::uint64_t>();
        (void)rpc.edit(actor, Json::array({spawnBox("Left", -1.0), spawnBox("Right", 1.0)}));
        const auto left = objectId(rpc, "Left");
        right = objectId(rpc, "Right");
        const auto camera = objectId(rpc, "MainCamera");
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(grey(boxes.right));
        }

        const Json red{1.0, 0.0, 0.0, 1.0};
        const auto created = rpc.result(
            "create_material",
            {{"path", "materials/paint.material.json"},
             {"surface", openPbrOpaque},
             {"values", {{"base_color", red}, {"emission_color", red}, {"emission_luminance", 1.0}}}});
        REQUIRE(created.at("applied") == "live");

        // Refusals, by name, with nothing written.
        const auto revision = rpc.result("get_scene_revision").at("scene_revision");
        const auto unknown =
            rpc.refusal("set_object_material", {{"actor_id", actor}, {"object", right}, {"material", "nope"}});
        CHECK(unknown.at("error_code") == "material.name_unknown@1");
        CHECK(unknown.at("error_fields").at("name") == "nope");
        const auto no_primitive = rpc.refusal(
            "set_object_material",
            {{"actor_id", actor}, {"object", right}, {"material", "paint"}, {"primitive", {{"mesh", 0}, {"primitive", 3}}}});
        CHECK(no_primitive.at("error_code") == "material.binding_invalid@1");
        CHECK(no_primitive.at("error_fields").at("object") == right);
        const auto no_model =
            rpc.refusal("set_object_material", {{"actor_id", actor}, {"object", camera}, {"material", "paint"}});
        CHECK(no_model.at("error_code") == "material.object_unknown@1");
        CHECK(no_model.at("error_fields").at("reason") == "the object has no simplemodelview");
        // MT-2b: the object is named by its authoring name and its scene.
        CHECK(no_model.at("error_fields").at("name") == "MainCamera");
        CHECK(no_model.at("error_fields").at("scene_id") == "default_scene");
        const auto other_scene = rpc.refusal(
            "set_object_material", {{"actor_id", actor}, {"object", right}, {"scene_id", "other"}, {"material", "paint"}});
        CHECK(other_scene.at("error_code") == "material.object_unknown@1");
        CHECK(other_scene.at("error_fields").at("name") == "Right");
        CHECK(other_scene.at("error_fields").at("scene_id") == "default_scene");
        const auto missing = rpc.refusal("get_object_materials", {{"object", 999999}});
        CHECK(missing.at("error_code") == "material.object_unknown@1");
        CHECK_FALSE(missing.at("error_fields").contains("name"));
        CHECK_FALSE(missing.at("error_fields").contains("scene_id"));
        CHECK(rpc.result("get_scene_revision").at("scene_revision") == revision);

        // The right box alone takes the material, in the next frame.
        const auto set = rpc.result("set_object_material", {{"actor_id", actor}, {"object", right}, {"material", "paint"}});
        CHECK(set.at("changed") == true);
        CHECK(set.at("model") == std::string{cube});
        CHECK(set.at("scene_id") == "default_scene");
        CHECK(set.at("previous") == Json{{"materials", Json::array({{{"material", nullptr}}})}});
        (void)rpc.commit(set);
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(reddish(boxes.right));
        }
        {
            const auto own = rpc.result("get_object_materials", {{"object", right}});
            REQUIRE(own.at("primitives").size() == 1);
            const auto &primitive = own.at("primitives").at(0);
            CHECK(primitive.at("material") == "paint");
            CHECK(primitive.at("source") == "object");
            CHECK(primitive.at("model_material") == std::string{primitiveDefault});
            CHECK(primitive.at("drawn") == "paint");
            REQUIRE(own.at("materials").size() == 1);
            CHECK(own.at("materials").at(0).at("material") == "paint");
            CHECK(own.at("materials").at(0).at("resolved") == true);
            CHECK_FALSE(own.at("materials").at(0).contains("primitive"));
            const auto other = rpc.result("get_object_materials", {{"object", left}}).at("primitives").at(0);
            CHECK(other.at("material") == std::string{primitiveDefault});
            CHECK(other.at("source") == "model");
            CHECK(other.at("drawn") == std::string{primitiveDefault});
            // The model's readers: two boxes, one with its own material.
            const auto bindings = rpc.result("get_material_bindings", {{"model", cube}});
            CHECK(bindings.at("objects") == 2);
            CHECK(bindings.at("object_overrides") == 1);
            // MP-1: the pass drew each box with its own material.
            const auto draws = forwardDraws(rpc);
            INFO(Json(draws).dump());
            CHECK(draws.at("paint") == 1);
            CHECK(draws.at(std::string{primitiveDefault}) == 1);
        }
        // The same material again changes nothing.
        CHECK(rpc.result("set_object_material", {{"actor_id", actor}, {"object", right}, {"material", "paint"}})
                  .at("changed") == false);

        // undo / redo.
        (void)rpc.revert("undo", actor);
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(grey(boxes.right));
        }
        CHECK(rpc.result("get_object_materials", {{"object", right}}).at("primitives").at(0).at("source") == "model");
        (void)rpc.revert("redo", actor);
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(reddish(boxes.right));
        }

        // Taken back with what it answered: null puts the model's back.
        const auto taken = rpc.result("set_object_material",
                                      {{"actor_id", actor}, {"object", right}, {"materials", set.at("previous").at("materials")}});
        CHECK(taken.at("previous") == Json{{"materials", Json::array({{{"material", "paint"}}})}});
        (void)rpc.commit(taken);
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(grey(boxes.right));
        }
        CHECK(rpc.result("get_object_materials", {{"object", right}}).at("materials").empty());
        CHECK(rpc.result("get_material_bindings", {{"model", cube}}).at("object_overrides") == 0);

        // One primitive, then the same primitive back to the model's.
        const Json primitive{{"mesh", 0}, {"primitive", 0}};
        (void)rpc.commit(rpc.result("set_object_material", {{"actor_id", actor},
                                                            {"object", right},
                                                            {"material", "paint"},
                                                            {"primitive", primitive}}));
        CHECK(reddish(rpc.boxes(capture).right));
        CHECK(rpc.result("get_object_materials", {{"object", right}}).at("materials").at(0).at("primitive") ==
              primitive);
        const auto off = rpc.result("set_object_material",
                                    {{"actor_id", actor}, {"object", right}, {"material", nullptr}, {"primitive", primitive}});
        CHECK(off.at("previous") ==
              Json{{"materials", Json::array({{{"material", "paint"}, {"primitive", primitive}}})}});
        (void)rpc.commit(off);
        CHECK(grey(rpc.boxes(capture).right));

        // By the glTF material the primitive carries, kept for the save.
        (void)rpc.commit(rpc.result("set_object_material", {{"actor_id", actor},
                                                            {"object", right},
                                                            {"material", "paint"},
                                                            {"slot", primitiveDefault}}));
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(reddish(boxes.right));
        }
        (void)rpc.result("save_scene");
        GET_MODULE(VulkanManageCore).waitIdle();
    }

    // The scene document holds the row on the right box's simplemodelview.
    const auto scene = Json::parse(readFile(root / "scenes/main.scene.json"));
    Json rows;
    for (const auto &object : scene.at("scenes").at("default_scene").at("objects"))
        if (object.value("name", std::string{}) == "Right")
            for (const auto &component : object.at("components"))
                if (component.value("name", std::string{}) == "simplemodelview") rows = component.at("materials").at("rows");
    INFO(scene.dump(2));
    REQUIRE(rows.size() == 1);
    CHECK(rows.at(0).at("material") == "paint");
    CHECK(rows.at(0).at("mesh") == 0);
    CHECK(rows.at(0).at("primitive") == 0);

    // The next start draws what was saved.
    {
        FastModuleContainer modules;
        configureHeadless(root);
        (void)GET_MODULE(Renderer);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("step_frame");
        {
            const auto boxes = rpc.boxes(capture);
            INFO(boxes);
            CHECK(grey(boxes.left));
            CHECK(reddish(boxes.right));
        }
        const auto own = rpc.result("get_object_materials", {{"object", objectId(rpc, "Right")}});
        CHECK(own.at("primitives").at(0).at("source") == "object");
        CHECK(own.at("primitives").at(0).at("drawn") == "paint");
        GET_MODULE(VulkanManageCore).waitIdle();
    }
}

} // namespace Pelican
