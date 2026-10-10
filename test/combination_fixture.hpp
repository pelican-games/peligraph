#pragma once
#include "skeletal_fixture.hpp"
#include "vrm_xr_demo_fixture.hpp"
#include "render_graph_root_test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace Pelican::TestSupport::CB1 {
using Json = nlohmann::json;
inline void cbWrite(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    REQUIRE(out.good());
}

inline const std::string cbSurface = R"(//! pelican.surface v1
//! language: glsl
void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(0.2, 0.6, 0.8, 1.0);
    surface.emissive = vec3(0.2, 0.6, 0.8);
}
vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, in PelicanSurfaceInputV1 input_data) {
    return surface.base_color.rgb;
}
)";

struct CombinationProject {
    int row;
    std::filesystem::path root;
    Json project;
    explicit CombinationProject(int number, bool contract_only = false) : row(number) {
        const auto *directory = std::getenv("PELICAN_CB1_ARTIFACT_DIR");
        root = (directory ? std::filesystem::path{directory} :
                           std::filesystem::current_path() / "cb1_artifacts");
        if (contract_only) root /= "fixture_contract";
        root /= std::to_string(row);
        std::filesystem::create_directories(root);
        TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin",
                                      {.morph_target = row == 6});
        auto gltf = Json::parse(std::ifstream{root / "character.gltf"});
        if (row == 3) gltf["materials"][0]["emissiveFactor"] = {0.2, 0.6, 0.8};
        if (row == 6 || row == 8 || row == 11 || row == 12) {
            gltf["nodes"][0].erase("skin");
            gltf["meshes"][0]["primitives"][0]["attributes"].erase("JOINTS_0");
            gltf["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
            gltf.erase("skins");
        }
        if (row == 2 || row == 4 || row == 5) {
            std::ifstream input{root / "character.bin", std::ios::binary};
            std::string binary{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            const auto appendAccessor = [&](const auto &data, Json accessor) {
                const auto view = gltf["bufferViews"].size();
                gltf["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", binary.size()}, {"byteLength", sizeof(data)}});
                binary.append(reinterpret_cast<const char *>(data.data()), sizeof(data));
                accessor["bufferView"] = view; accessor["count"] = data.size();
                gltf["accessors"].push_back(accessor);
                return gltf["accessors"].size() - 1;
            };
            if (row == 2) {
                // Back-facing triangles make the front-cull outline produce
                // pixels; a one-sided strip would only test a vacuous draw.
                const std::array<std::uint16_t, 12> indices{0,1,2,2,1,3,2,1,0,3,1,2};
                gltf["meshes"][0]["primitives"][0]["indices"] = appendAccessor(indices, gltf["accessors"][5]);
            } else {
                gltf["meshes"].push_back(gltf["meshes"][0]);
                const std::array<glm::vec3, 4> positions{glm::vec3{0.7F,0,0}, {1.3F,0,0}, {0.7F,2,0}, {1.3F,2,0}};
                gltf["meshes"][1]["primitives"][0]["attributes"]["POSITION"] = appendAccessor(positions, gltf["accessors"][0]);
                gltf["nodes"].push_back({{"name", "Second"}, {"mesh", 1}, {"skin", 0}});
                gltf["scenes"][0]["nodes"].push_back(3);
            }
            gltf["buffers"][0]["byteLength"] = binary.size();
            cbWrite(root / "character.bin", binary);
        }
        cbWrite(root / "character.gltf", gltf.dump(2));
        if (row == 7) TestVrmXrDemoFixture::writeGlb(root / "character.vrm");
        const auto model_path = row == 7 ? "character.vrm" : "character.gltf";
        cbWrite(root / "paint.surface", cbSurface);
        cbWrite(root / "outline.surface", R"(//! pelican.surface v1
//! language: glsl
//! render_state: { blend: opaque, cull: front, depth: read_only, depth_compare: less_equal }
void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) { vertex.position += vertex.normal * 0.08; }
vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, in PelicanSurfaceInputV1 input_data) { return vec3(1,0,0); }
)");
        cbWrite(root / "glass.surface", R"(//! pelican.surface v1
//! language: glsl
//! render_state: { blend: blend, cull: back, depth: read_only }
void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, inout PelicanSurfaceV1 surface) { surface.base_color = vec4(0.2,0.6,0.8,0.5); }
vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, in PelicanSurfaceInputV1 input_data) { return vec3(0.2,0.6,0.8); }
)");
        Json paint{{"name", "Paint"}, {"surface", "project://paint.surface"},
                   {"routing", {{"alpha_mode", "opaque"}, {"double_sided", row == 7}}}};
        if (row == 7) cbWrite(root / "paint.surface", std::string{
            "//! pelican.surface v1\n//! language: glsl\n//! render_state: { cull: none }\n"} +
            cbSurface.substr(cbSurface.find("void ")));
        if (row == 2) {
            paint["render_path"] = "forward";
            paint["variants"]["outline"] = {{"surface", "project://outline.surface"}, {"render_path", "forward"}};
        }
        Json materials = Json::array({paint});
        if (row == 11) materials[0]["surface"] = "engine://surfaces/openpbr/opaque_single.surface";
        if (row == 10) { auto second = paint; second["name"] = "SecondPaint"; materials.push_back(second); }
        if (row == 3) materials.push_back({{"name", "Glass"}, {"surface", "project://glass.surface"},
            {"render_path", "forward"}, {"routing", {{"alpha_mode", "blend"}, {"double_sided", false}}}});
        cbWrite(root / "paint.material.json", Json{{"schema", "pelican.material"}, {"version", 1}, {"materials", materials}}.dump(2));
        Json binding{{"schema", "pelican.material_bindings"}, {"version", 1}, {"model", model_path},
            {"bindings", Json::array({{{"usd_path", "/Body"}, {"mesh", 0}, {"primitive", 0}, {"material", "Paint"},
                {"routing", {{"alpha_mode", "opaque"}, {"double_sided", row == 7}}}}})}};
        cbWrite(root / "binding.json", binding.dump(2));
        cbWrite(root / "assets.json", Json{{"schema", "pelican.asset_data"}, {"version", 2},
            {"models", Json::array({{{"name", "character"}, {"path", model_path}},
                {{"name", "painted"}, {"path", model_path}, {"material_bindings", "binding.json"}}})},
            {"materials", Json::array({{{"path", "paint.material.json"}}})}}.dump(2));
        // Rows 4 and 5 have two primitives, so its binding document must cover both.
        if (row == 4 || row == 5) {
            binding["bindings"].push_back(binding["bindings"][0]);
            binding["bindings"][1]["mesh"] = 1;
            binding["bindings"][1]["usd_path"] = "/Second";
            cbWrite(root / "binding.json", binding.dump(2));
        }
        const auto object = [](bool overridden, bool partial) {
            Json view{{"name", "simplemodelview"}, {"model", "character"}};
            if (overridden) {
                Json entry{{"$row", {{"id", 1}, {"v", 1}}}, {"material", "Paint"}};
                if (partial) { entry["mesh"] = 0; entry["primitive"] = 0; }
                view["materials"] = {{"$list", {{"v", 1}, {"next_id", 2}}}, {"rows", Json::array({entry})}};
            }
            return Json{{"name", "Subject"}, {"components", Json::array({
                {{"name", "transform"}, {"pos", {0,0,0}}, {"rotation", {0,0,0,1}}, {"scale", {1,1,1}}}, view})}};
        };
        Json scene{{"schema", "pelican.scene"}, {"version", 2}};
        scene["scenes"]["default_scene"]["objects"] = Json::array();
        scene["scenes"]["baseline"]["objects"] = Json::array({object(false, false)});
        scene["scenes"]["combined"]["objects"] = Json::array({object(true, row == 5)});
        cbWrite(root / "scene.json", scene.dump(2));
        project = {{"basic_config", {{"window_size", {{"width", 128}, {"height", 128}}},
            {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"},
            {"default_scene_id", "default_scene"}, {"rendering_config_json", "hybrid.json"},
            {"default_rendering_pass", "main_render"}}}};
        cbWrite(root / "project.json", project.dump(2));
        cbWrite(root / "reproduction.json", Json{{"row", row}, {"camera", {0,1,4}},
            {"pose_degrees", 35}, {"morph_weight", row == 6 ? 0.35 : 0.0},
            {"expression", row == 7 ? "happy=1" : "none"},
            {"test", "CB1 " + std::to_string(row)}, {"extent", {128,128}}}.dump(2));
        writeRendering();
    }

    void writeRendering();
};

inline void CombinationProject::writeRendering() {
    auto config = nlohmann::json::parse(R"({
      "render_targets":[
        {"name":"gbuffer_albedo","extent_scale":1.0,"format":"B8G8R8A8_UNORM","format_class":"scene","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_normal","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT","format_class":"data","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_material","extent_scale":1.0,"format":"R8G8B8A8_UNORM","format_class":"data","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_worldpos","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT","format_class":"data","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"g_emissive","extent_scale":1.0,"format":"B8G8R8A8_UNORM","format_class":"scene","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"scene_depth","extent_scale":1.0,"format":"D32_SFLOAT","format_class":"data","usage":["DEPTH_STENCIL_ATTACHMENT","TRANSFER_SRC"]}],
      "rendering_passes":[{"name":"main_render","passes":[
        {"name":"deferred_geometry","type":"material","material_contract":"deferred_geometry_v1","output":{"color":["gbuffer_albedo","gbuffer_normal","gbuffer_material","gbuffer_worldpos","g_emissive"],"depth":"scene_depth"},"depth_store_op":"store"},
        {"name":"present","type":"fullscreen","output":{"color":"swapchain","depth":null},"shader":{"vertex":"engine://fullscreen","fragment":"engine://shader_lab_hello"}}]}]})");
    config["render_targets"].push_back({{"name", "lit_color"}, {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
        {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}});
    auto &passes = config["rendering_passes"][0]["passes"];
    passes.insert(passes.begin()+1, nlohmann::json{{"name", "forward_opaque"}, {"type", "material"},
        {"material_contract", "forward_opaque_v1"}, {"after", {"deferred_geometry"}},
        {"output", {{"color", {"lit_color"}}, {"depth", "scene_depth"}}},
        {"depth_load_op", "load"}, {"depth_store_op", "store"}});

    if (row == 2 || row == 3) {
        if (row == 3) config["material_route_default"] = "forward";
        for (auto &target : config["render_targets"]) if (target["name"] == "lit_color")
            target["usage"].push_back("TRANSFER_SRC");
    }
    if (row == 3) passes.insert(passes.end()-1, Json{
        {"name", "forward_transparent"}, {"type", "material"}, {"material_contract", "forward_transparent_v1"},
        {"output", {{"color", {"lit_color"}}, {"depth", "scene_depth"}}},
        {"color_load_op", "load"}, {"depth_load_op", "load"}, {"depth_store_op", "store"}});
    const auto addDepth = [&](const char *name) {
        config["render_targets"].push_back({{"name", name}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
            {"format_class", "data"}, {"usage", {"DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC"}}});
    };
    if (row == 1 || row == 6 || row == 8) {
        addDepth("cb_shadow");
        // Deliberately use the main view for both projections. This tests the
        // shadow-depth geometry consumer without comparing two different views.
        passes.insert(passes.begin(), Json{{"name", "cb_shadow_pass"}, {"type", "shadow_depth"},
            {"output", {{"color", nullptr}, {"depth", "cb_shadow"}}}, {"depth_store_op", "store"}});
    }
    if (row == 8) {
        for (const auto *kind : {"velocity", "picking"}) {
            const auto name = "cb_" + std::string{kind};
            const auto depth_name = name + "_depth";
            addDepth(depth_name.c_str());
            config["render_targets"].push_back({{"name", name}, {"extent_scale", 1.0},
                {"format", std::string_view{kind} == "velocity" ? "R16G16_SFLOAT" : "R32_UINT"},
                {"format_class", "data"}, {"usage", {"COLOR_ATTACHMENT", "TRANSFER_SRC"}}});
            passes.insert(passes.end()-1, Json{{"name", name + "_pass"}, {"type", kind},
                {"output", {{"color", name}, {"depth", depth_name}}}, {"clear_color", {0,0,0,0}},
                {"color_store_op", "store"}, {"depth_store_op", "store"}});
        }
    }
    if (row == 11) config = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", {"engine://features/rt_shadow_mask.json"}}};
    cbWrite(root / "hybrid.json", TestSupport::renderGraphRootWithTaskFeature(config, root).dump(2));
}



} // namespace Pelican::TestSupport::CB1
