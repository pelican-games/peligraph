// LP-2: the draw-step table of a material pass, on the GPU. Each case builds
// a hybrid project, registers its materials after the render generation is
// published (the way a glTF load or a spawn does), and looks at both the
// pixels and the binds and draws the material renderer recorded.

#include "../src/core/appflow/enginetime.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/materialdrawlooptable.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/model/modeltemplate.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/materialrender.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/materialdrawloop.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/materiallowering.hpp"
#include "../src/project/surfaceformat.hpp"
#include "gltf_fragment_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {
namespace {

#if PELICAN_RUNTIME_SHADER_COMPILER

constexpr std::string_view loopSurfaceReference =
    "project://shaders/loop.surface";
constexpr std::string_view loopMaterialDocument =
    "materials/loop.material.json";

// Faces that look at the camera take the tint; faces that look away take
// the tint with red and blue swapped. With the blue default, a front face is
// blue and a back face is red. LP2_MARK paints everything yellow.
constexpr std::string_view loopSurfaceSource =
    "//! pelican.surface v1\n"
    "//! language: glsl\n"
    "//! params:\n"
    "//!   - { name: tint, type: color, default: [0.02, 0.08, 1.0, 1.0], "
    "encoding: linear }\n\n"
    "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
    "inout PelicanSurfaceV1 surface) {\n"
    "    vec4 tint = pelican_param_tint();\n"
    "    vec3 color = dot(input_data.normal, input_data.view_direction) "
    ">= 0.0 ? tint.rgb : tint.bgr;\n"
    "#ifdef LP2_MARK\n"
    "    color = vec3(1.0, 1.0, 0.02);\n"
    "#endif\n"
    "    surface.base_color = vec4(color, 1.0);\n"
    "    surface.emissive = color;\n"
    "    surface.roughness = 0.8;\n"
    "}\n";

// A binding the surface declares only under LP2_EXTRA_BINDING, so the
// define changes the material set layout. (A declared binding the shader
// merely stops sampling stays in the reflected layout.)
constexpr std::string_view maskedSurfaceReference =
    "project://shaders/masked.surface";
constexpr std::string_view maskedSurfaceSource =
    "//! pelican.surface v1\n"
    "//! language: glsl\n\n"
    "#ifdef LP2_EXTRA_BINDING\n"
    "layout(set = 1, binding = 31) uniform sampler2D lp2_extra_texture;\n"
    "#endif\n"
    "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
    "inout PelicanSurfaceV1 surface) {\n"
    "    vec4 color = vec4(0.3, 0.3, 0.3, 1.0);\n"
    "#ifdef LP2_EXTRA_BINDING\n"
    "    color *= texture(lp2_extra_texture, input_data.uv);\n"
    "#endif\n"
    "    surface.base_color = color;\n"
    "    surface.emissive = color.rgb;\n"
    "}\n";

std::filesystem::path makeDrawLoopProjectDir() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               ("pelican_lp2_draw_loop_" + std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeText(const std::filesystem::path &path,
               std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(contents.data(),
               static_cast<std::streamsize>(contents.size()));
    if (!file) throw std::runtime_error("cannot write " + path.string());
}

using StepsFor = std::function<std::optional<MaterialDrawStepList>(
    const MaterialDrawSubject &)>;

// The same steps for every material pass of the generation.
std::shared_ptr<const MaterialDrawLoopRules> rulesForEveryMaterialPass(
    const RendererRuntimeGeneration &generation, const StepsFor &steps) {
    auto rules = std::make_shared<MaterialDrawLoopRules>();
    for (const auto id : generation.rendering_pass_ids) {
        const auto *program = generation.find(id);
        if (program == nullptr) continue;
        for (const auto &compiled : program->rendering_pass.passes) {
            if (!compiled.definition.isMaterial()) continue;
            rules->passes.insert_or_assign(
                compiled.definition.name,
                MaterialDrawLoopPassRule{.node = "test_loop",
                                         .rule = steps});
        }
    }
    return rules;
}

MaterialDrawLoopTable &liveTable() {
    const auto snapshot =
        GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->material_draw_loops != nullptr);
    return *snapshot->material_draw_loops;
}

void installLive(const StepsFor &steps) {
    const auto snapshot =
        GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    REQUIRE(snapshot != nullptr);
    snapshot->material_draw_loops->install(
        rulesForEveryMaterialPass(*snapshot, steps));
}

std::size_t liveTableCount() {
    std::size_t count = 0;
    MaterialDrawLoopTable::forEachLive(
        [&](MaterialDrawLoopTable &) { ++count; });
    return count;
}

CommonPolygonVertData cubeAt(glm::vec3 center, float h) {
    CommonPolygonVertData data;
    const auto add_face = [&](glm::vec3 normal,
                              std::array<glm::vec3, 4> positions) {
        const auto first = static_cast<std::uint32_t>(data.pos.size());
        for (const auto &position : positions) {
            data.pos.push_back(center + position);
        }
        data.normal.insert(data.normal.end(), 4, normal);
        data.texcoord.insert(data.texcoord.end(),
                             {{0.0f, 0.0f}, {1.0f, 0.0f},
                              {1.0f, 1.0f}, {0.0f, 1.0f}});
        data.color.insert(data.color.end(), 4, glm::vec4{1.0f});
        data.indices.insert(data.indices.end(),
                            {first, first + 1, first + 2, first,
                             first + 2, first + 3});
    };
    add_face({0, 0, 1}, {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}});
    add_face({0, 0, -1},
             {{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}});
    add_face({1, 0, 0}, {{{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}}});
    add_face({-1, 0, 0},
             {{{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}});
    add_face({0, 1, 0}, {{{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}}});
    add_face({0, -1, 0},
             {{{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}});
    return data;
}

// One quad whose front face looks away from a camera on +z: back-face
// culling hides it, a cull-front pipeline shows its back.
CommonPolygonVertData awayQuadAt(glm::vec3 center, float h) {
    CommonPolygonVertData data;
    for (const auto &position :
         {glm::vec3{h, -h, 0}, glm::vec3{-h, -h, 0}, glm::vec3{-h, h, 0},
          glm::vec3{h, h, 0}}) {
        data.pos.push_back(center + position);
    }
    data.normal.insert(data.normal.end(), 4, glm::vec3{0, 0, -1});
    data.texcoord = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},
                     {0.0f, 1.0f}};
    data.color.insert(data.color.end(), 4, glm::vec4{1.0f});
    data.indices = {0, 1, 2, 0, 2, 3};
    return data;
}

struct ColorCounts {
    std::size_t blue = 0;
    std::size_t red = 0;
    std::size_t green = 0;
    std::size_t yellow = 0;
    std::size_t lit = 0;
};

ColorCounts countColors(const std::vector<std::uint8_t> &pixels) {
    ColorCounts counts;
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        const int r = pixels[offset];
        const int g = pixels[offset + 1];
        const int b = pixels[offset + 2];
        if (r > 150 && g > 150 && b + 80 < std::min(r, g)) ++counts.yellow;
        else if (b > r + 20 && b > g + 20) ++counts.blue;
        else if (r > b + 20 && r > g + 20) ++counts.red;
        else if (g > r + 20 && g > b + 20) ++counts.green;
        if (r + g + b > 30) ++counts.lit;
    }
    return counts;
}

struct DrawLoopScene {
    std::filesystem::path temp_dir;
    GlobalMaterialId material;
    std::shared_ptr<StepsFor> steps = std::make_shared<StepsFor>();
    std::vector<MaterialDrawRecord> records;

    ~DrawLoopScene() {
        if (FastModuleContainer::isInitialized<MaterialRenderer>()) {
            GET_MODULE(MaterialRenderer)
                .recordMaterialDrawsForTesting(nullptr);
        }
    }

    std::vector<std::uint8_t> render() {
        records.clear();
        GET_MODULE(Renderer).render();
        GET_MODULE(VulkanManageCore).waitIdle();
        return GET_MODULE(RenderTarget).readbackLastFrameRGBA8();
    }

    std::vector<MaterialDrawRecord>
    materialDraws(GlobalMaterialId of, std::string_view kind =
                                          "material") const {
        std::vector<MaterialDrawRecord> result;
        for (const auto &record : records) {
            if (record.kind == kind && record.material == of.value) {
                result.push_back(record);
            }
        }
        return result;
    }
};

LoweredMaterial lowerProjectMaterial(std::string_view surface_reference,
                                     const SurfaceFormatDocument &surface,
                                     const nlohmann::json &material_json) {
    const MaterialSurfaceCatalog surfaces{
        {std::string{surface_reference}, surface}};
    const auto document = parseMaterialFormatJson(material_json, surfaces);
    REQUIRE(document.materials.size() == 1);
    return lowerMaterial(document.materials.front(), surface);
}

nlohmann::json loopMaterialJson(std::vector<double> tint) {
    return nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials",
         nlohmann::json::array({{
             {"name", "loop_cube"},
             {"tags", {"loop"}},
             {"surface", std::string{loopSurfaceReference}},
             {"values", {{"tint", tint}}},
         }})},
    };
}

GlobalMaterialId registerSurfaceMaterial(
    std::string_view reference, std::string_view source,
    const nlohmann::json &material_json, std::string name) {
    const auto surface =
        parseSurfaceFormat(std::string{source}, std::string{reference});
    const auto lowered =
        lowerProjectMaterial(reference, surface, material_json);
    const auto main_render_id =
        GET_MODULE(RenderingPassContainer)
            .getRenderingPassIdByName("main_render");
    const auto execution =
        GET_MODULE(FrameGraphRuntimeContainer).find(main_render_id);
    REQUIRE(execution != nullptr);
    const auto shaders =
        GET_MODULE(ShaderLibrary)
            .loadFromSurfaceForMaterial(
                surface, reference, lowered,
                execution->render_pipeline->shader_defines);
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = shaders.vertex,
        .frag_shader = shaders.fragment,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture =
            standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
        .occlusion_texture = standard.occlusionDefaultTexture(),
    };
    applyLoweredMaterialForRoute(info, lowered);
    info.name = std::move(name);
    info.source_document = std::string{loopMaterialDocument};
    info.source_asset = std::string{loopMaterialDocument};
    return GET_MODULE(MaterialContainer).registerMaterial(std::move(info));
}

// Writes the project, installs the rule source before the renderer
// publishes its first generation, then registers the loop material and
// places a cube (left) and an away-facing quad (right) that use it.
void setUpDrawLoopScene(
    DrawLoopScene &scene,
    nlohmann::json features = nlohmann::json::array(),
    const std::vector<std::pair<std::string, std::string>> &extra_files = {}) {
    setupLogger();
    scene.temp_dir = makeDrawLoopProjectDir();
    const auto &dir = scene.temp_dir;
    for (const auto &[relative, contents] : extra_files) {
        writeText(dir / relative, contents);
    }
    writeText(dir / "scene.json",
              R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeText(dir / "assets.json",
              R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writeText(dir / "shaders" / "loop.surface", loopSurfaceSource);
    writeText(dir / "shaders" / "masked.surface", maskedSurfaceSource);
    writeText(dir / "materials" / "loop.material.json",
              loopMaterialJson({0.02, 0.08, 1.0, 1.0}).dump(2));
    writeText(dir / "hybrid.json",
              TestSupport::renderGraphRootWithTaskFeature(
                  nlohmann::json{
                      {"pipeline",
                       {{"preset",
                         "engine://render_pipelines/hybrid_v1.json"}}},
                      {"features", std::move(features)},
                  },
                  dir)
                  .dump(2));
    nlohmann::json project{
        {"basic_config",
         {
             {"window_size", {{"width", 32}, {"height", 32}}},
             {"scene_data_json", "scene.json"},
             {"asset_data_json", "assets.json"},
             {"default_scene_id", "default_scene"},
             {"rendering_config_json", "hybrid.json"},
             {"default_rendering_pass", "main_render"},
         }},
    };
    GET_MODULE(ProjectSource).setSourceByData(project.dump());
    GET_MODULE(PathResolver).setup(dir, false);
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{32, 32};
    launch.headless_frames = 1;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

    GET_MODULE(FrameGraphRuntimeContainer)
        .setMaterialDrawLoopRuleSource(
            [steps = scene.steps](const RendererRuntimeGeneration &candidate)
                -> std::shared_ptr<const MaterialDrawLoopRules> {
                if (!*steps) return nullptr;
                return rulesForEveryMaterialPass(candidate, *steps);
            });
    GET_MODULE(MaterialRenderer).recordMaterialDrawsForTesting(&scene.records);
    (void)GET_MODULE(Renderer);

    scene.material = registerSurfaceMaterial(
        loopSurfaceReference, loopSurfaceSource,
        loopMaterialJson({0.02, 0.08, 1.0, 1.0}), "loop_cube");

    auto &geometry = GET_MODULE(VertBufContainer);
    ModelTemplate model;
    model.asset_id = ModelAssetId{2002};
    model.material_primitives = {ModelTemplate::MaterialPrimitives{
        .material = scene.material,
        .primitives = {geometry.addPrimitiveEntry(
                           cubeAt({-0.35f, 0.0f, 0.0f}, 0.25f)),
                       geometry.addPrimitiveEntry(
                           awayQuadAt({0.4f, 0.0f, 0.0f}, 0.2f))},
        .source_material_index = 0,
    }};
    const auto instance =
        GET_MODULE(PolygonInstanceContainer).placeModelInstance(model);
    REQUIRE(GET_MODULE(PolygonInstanceContainer)
                .isModelInstanceAlive(instance));
    auto &camera = GET_MODULE(Camera);
    camera.setPos({0.0f, 0.0f, 2.0f});
    camera.setDir({0.0f, 0.0f, -1.0f});
    camera.setUp({0.0f, 1.0f, 0.0f});
}

MaterialDrawStepList twoDraws() {
    MaterialDrawRenderStateOverride cull_front;
    cull_front.cull = SurfaceCullMode::front;
    return {MaterialDrawStep::overridden(cull_front),
            MaterialDrawStep::material()};
}

StepsFor stepsForKey(std::string key, MaterialDrawStepList steps) {
    return [key = std::move(key), steps = std::move(steps)](
               const MaterialDrawSubject &subject)
               -> std::optional<MaterialDrawStepList> {
        if (subject.key != key) return std::nullopt;
        return steps;
    };
}

std::string loopKey() {
    return std::string{loopMaterialDocument} + "#loop_cube";
}

constexpr std::string_view silhouetteSurfaceReference =
    "project://shaders/silhouette.surface";
constexpr std::string_view silhouetteSurfaceSource =
    "//! pelican.surface v1\n"
    "//! language: glsl\n"
    "//! params:\n"
    "//!   - { name: width, type: float, default: 0.06, min: 0.0, max: 0.5 }\n"
    "//!   - { name: color, type: color, default: [0.02, 1.0, 0.02, 1.0], "
    "encoding: linear }\n"
    "//! render_state: { blend: opaque, cull: front, depth: read_only, "
    "depth_compare: less_equal }\n\n"
    "void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {\n"
    "    vertex.position += vertex.normal * pelican_param_width();\n"
    "}\n"
    "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
    "in PelicanSurfaceInputV1 input_data) {\n"
    "    return pelican_param_color().rgb;\n"
    "}\n";

std::string silhouetteFeatureJson() {
    return nlohmann::json{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "silhouette"},
        {"passes",
         nlohmann::json::array({{
             {"insert", "after:forward_opaque"},
             {"pass",
              {
                  {"name", "silhouette_overlay"},
                  {"type", "material"},
                  {"material_contract", "forward_opaque_v1"},
                  {"material_filter", {{"include", {"loop"}}}},
                  {"material_variant", "silhouette"},
                  {"output",
                   {{"color", {"lit_color"}}, {"depth", "scene_depth"}}},
                  {"color_load_op", "load"},
                  {"depth_load_op", "load"},
                  {"depth_store_op", "store"},
              }},
         }})},
    }
        .dump(2);
}

void registerSilhouetteVariant(GlobalMaterialId base) {
    const auto surface =
        parseSurfaceFormat(std::string{silhouetteSurfaceSource},
                           std::string{silhouetteSurfaceReference});
    const auto lowered = lowerProjectMaterial(
        silhouetteSurfaceReference, surface,
        nlohmann::json{
            {"schema", "pelican.material"},
            {"version", 1},
            {"materials",
             nlohmann::json::array({{
                 {"name", "silhouette"},
                 {"surface", std::string{silhouetteSurfaceReference}},
                 {"render_path", "forward"},
             }})},
        });
    REQUIRE(lowered.route == MaterialRouteClass::forward_opaque);
    const auto main_render_id =
        GET_MODULE(RenderingPassContainer)
            .getRenderingPassIdByName("main_render");
    const auto execution =
        GET_MODULE(FrameGraphRuntimeContainer).find(main_render_id);
    REQUIRE(execution != nullptr);
    const auto shaders =
        GET_MODULE(ShaderLibrary)
            .loadFromSurfaceForMaterial(
                surface, silhouetteSurfaceReference, lowered,
                execution->render_pipeline->shader_defines);
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = shaders.vertex,
        .frag_shader = shaders.fragment,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture =
            standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
        .occlusion_texture = standard.occlusionDefaultTexture(),
    };
    applyLoweredMaterialForRoute(info, lowered);
    GET_MODULE(MaterialContainer)
        .registerMaterialVariants(base, {{"silhouette", std::move(info)}});
}

// A second material on the loop material's own shaders: same pipeline, its
// own descriptor set and GPU slot. Placed as a small cube behind the scene.
GlobalMaterialId placeSharedPipelineMaterial(GlobalMaterialId base) {
    auto &materials = GET_MODULE(MaterialContainer);
    const auto desc = GET_MODULE(PipelineFactory)
                          .graphicsDesc(materials.materialPipeline(base));
    const auto surface = parseSurfaceFormat(
        std::string{loopSurfaceSource}, std::string{loopSurfaceReference});
    auto json = loopMaterialJson({0.02, 0.08, 1.0, 1.0});
    json["materials"][0]["name"] = "loop_cube_b";
    json["materials"][0]["tags"] = {"loop_b"};
    const auto lowered =
        lowerProjectMaterial(loopSurfaceReference, surface, json);
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = desc.vert,
        .frag_shader = *desc.frag,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture =
            standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
        .occlusion_texture = standard.occlusionDefaultTexture(),
    };
    applyLoweredMaterialForRoute(info, lowered);
    info.name = "loop_cube_b";
    info.source_document = std::string{loopMaterialDocument};
    const auto material = materials.registerMaterial(std::move(info));
    REQUIRE(materials.materialPipeline(material) ==
            materials.materialPipeline(base));

    auto &geometry = GET_MODULE(VertBufContainer);
    ModelTemplate model;
    model.asset_id = ModelAssetId{2003};
    model.material_primitives = {ModelTemplate::MaterialPrimitives{
        .material = material,
        .primitives = {geometry.addPrimitiveEntry(
            cubeAt({0.0f, 0.4f, -0.5f}, 0.15f))},
        .source_material_index = 0,
    }};
    REQUIRE(GET_MODULE(PolygonInstanceContainer)
                .isModelInstanceAlive(
                    GET_MODULE(PolygonInstanceContainer)
                        .placeModelInstance(model)));
    return material;
}

// What each material pass draws with no rule, spelled out: the variant the
// pass names (when the material has it), else the material itself.
std::shared_ptr<const MaterialDrawLoopRules> spelledOutRules(
    const RendererRuntimeGeneration &generation) {
    auto rules = std::make_shared<MaterialDrawLoopRules>();
    for (const auto id : generation.rendering_pass_ids) {
        const auto *program = generation.find(id);
        if (program == nullptr) continue;
        for (const auto &compiled : program->rendering_pass.passes) {
            const auto &pass = compiled.definition;
            if (!pass.isMaterial()) continue;
            const auto variant = pass.materialInfo().material_variant;
            rules->passes.insert_or_assign(
                pass.name,
                MaterialDrawLoopPassRule{
                    .node = "spelled_out",
                    .rule = [variant](const MaterialDrawSubject &subject)
                        -> std::optional<MaterialDrawStepList> {
                        if (!variant) {
                            return MaterialDrawStepList{
                                MaterialDrawStep::material()};
                        }
                        if (!std::binary_search(subject.variants.begin(),
                                                subject.variants.end(),
                                                *variant)) {
                            return std::nullopt;
                        }
                        return MaterialDrawStepList{
                            MaterialDrawStep::named(*variant)};
                    },
                });
        }
    }
    return rules;
}

template <typename Body>
void runDrawLoopCase(std::string_view purpose, Body &&body) {
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        DrawLoopScene scene;
        body(scene);
        temp_dir = scene.temp_dir;
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(error,
                                                   std::string{purpose});
        throw;
    }
    if (!temp_dir.empty()) std::filesystem::remove_all(temp_dir);
}

#endif

} // namespace

TEST_CASE("LP-2 a variant pass draws identically with its step spelled out",
          "[headless][render][lp2][draw-loop][material-variant]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(
            scene,
            nlohmann::json::array({"project://features/silhouette.json"}),
            {{"features/silhouette.json", silhouetteFeatureJson()},
             {"shaders/silhouette.surface",
              std::string{silhouetteSurfaceSource}}});
        registerSilhouetteVariant(scene.material);
        const auto shared = placeSharedPipelineMaterial(scene.material);
        const auto variant = GET_MODULE(MaterialContainer)
                                 .materialVariantResource(scene.material,
                                                          "silhouette");

        const auto today_pixels = scene.render();
        const auto today = scene.records;
        const auto today_counts = countColors(today_pixels);
        REQUIRE(today_counts.blue > 0);
        REQUIRE(today_counts.green > 0);
        std::size_t variant_draws = 0;
        for (const auto &record : today) {
            if (record.pass == "silhouette_overlay") {
                CHECK(record.effective_material == variant.value);
                ++variant_draws;
            }
        }
        REQUIRE(variant_draws > 0);
        // Not vacuous: two ranges share a pipeline, so one draw skips the
        // pipeline bind and binds only its own descriptor set.
        const auto skipped = std::count_if(
            today.begin(), today.end(), [&](const auto &record) {
                return record.pass == "deferred_geometry" &&
                       (record.material == shared.value ||
                        record.material == scene.material.value) &&
                       !record.pipeline_bound &&
                       record.descriptor_set_bound;
            });
        REQUIRE(skipped > 0);

        const auto snapshot =
            GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        snapshot->material_draw_loops->install(spelledOutRules(*snapshot));
        const auto *overlay = liveTable().find("silhouette_overlay",
                                               scene.material);
        REQUIRE(overlay != nullptr);
        REQUIRE(overlay->size() == 1);
        CHECK(overlay->front().material == variant);
        CHECK_FALSE(overlay->front().pipeline.has_value());
        REQUIRE(liveTable().find("deferred_geometry", scene.material) !=
                nullptr);

        const auto spelled_pixels = scene.render();
        CHECK(spelled_pixels == today_pixels);
        CHECK(scene.records == today);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 an override step draws a range again with its own pipeline",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(scene);
        auto &materials = GET_MODULE(MaterialContainer);
        REQUIRE(materials.drawLoopSubject(scene.material).key == loopKey());

        // No rule: the cube's front faces are blue and the away quad is
        // culled.
        const auto today = countColors(scene.render());
        REQUIRE(today.blue > 0);
        REQUIRE(today.red == 0);
        const auto today_draws = scene.materialDraws(scene.material);
        REQUIRE_FALSE(today_draws.empty());

        installLive(stepsForKey(loopKey(), twoDraws()));
        auto &table = liveTable();
        REQUIRE(table.overridePipelineCount() == 1);
        const auto twice = countColors(scene.render());
        // The override shows the quad's back (red); the material step then
        // draws the cube's front over its own back faces, so the cube is
        // exactly as blue as before.
        REQUIRE(twice.red > 0);
        REQUIRE(twice.blue == today.blue);

        const auto draws = scene.materialDraws(scene.material);
        REQUIRE(draws.size() == 2 * today_draws.size());
        const auto own = materials.materialPipeline(scene.material).value;
        const auto half = today_draws.size();
        for (std::size_t index = 0; index < half; ++index) {
            CAPTURE(index);
            const auto &override_draw = draws[index];
            const auto &material_draw = draws[half + index];
            CHECK(override_draw.pipeline != own);
            CHECK(material_draw.pipeline == own);
            // Both steps read the same material record.
            CHECK(override_draw.descriptor_set == material_draw.descriptor_set);
            CHECK(override_draw.gpu_material_index ==
                  static_cast<std::uint32_t>(scene.material.value));
            CHECK(material_draw.gpu_material_index ==
                  override_draw.gpu_material_index);
            CHECK(override_draw.offset == today_draws[index].offset);
            CHECK(material_draw.offset == today_draws[index].offset);
        }
        // Two pipelines are bound for the range: the override, then the
        // material's own again.
        CHECK(draws.front().pipeline_bound);
        CHECK(draws[half].pipeline_bound);
        CHECK_FALSE(draws[half].descriptor_set_bound);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 a value-only reload reaches the override draw",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(scene);
        installLive(stepsForKey(loopKey(), twoDraws()));
        const auto blue = countColors(scene.render());
        REQUIRE(blue.red > 0);
        REQUIRE(blue.blue > 0);

        // Green tint: the front stays tint, the back is tint.bgr -- both
        // green. The quad is only ever drawn by the override pipeline.
        const auto surface = parseSurfaceFormat(
            std::string{loopSurfaceSource}, std::string{loopSurfaceReference});
        const auto green = lowerProjectMaterial(
            loopSurfaceReference, surface,
            loopMaterialJson({0.02, 1.0, 0.08, 1.0}));
        GET_MODULE(VulkanManageCore).waitIdle();
        GET_MODULE(MaterialContainer)
            .updateMaterialValues(scene.material, green.values_layout,
                                  green.values);
        const auto after = countColors(scene.render());
        CHECK(after.red == 0);
        CHECK(after.blue == 0);
        CHECK(after.green >= blue.red + blue.blue);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 an empty step list draws nothing in the material pass only",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(
            scene, nlohmann::json::array(
                       {"engine://features/shadow_directional.json",
                        "engine://features/velocity.json"}));
        const auto today = countColors(scene.render());
        REQUIRE(today.blue > 0);
        REQUIRE_FALSE(scene.materialDraws(scene.material).empty());
        const auto shadow_draws =
            scene.materialDraws(scene.material, "shadow_depth").size();
        const auto velocity_draws =
            scene.materialDraws(scene.material, "velocity").size();
        REQUIRE(shadow_draws > 0);
        REQUIRE(velocity_draws > 0);

        installLive(stepsForKey(loopKey(), {}));
        const auto *steps =
            liveTable().find("deferred_geometry", scene.material);
        REQUIRE(steps != nullptr);
        REQUIRE(steps->empty());
        const auto none = countColors(scene.render());
        CHECK(none.blue == 0);
        CHECK(none.red == 0);
        CHECK(scene.materialDraws(scene.material).empty());
        // Known limit (design 4.3): shadow and velocity still draw it.
        CHECK(scene.materialDraws(scene.material, "shadow_depth").size() ==
              shadow_draws);
        CHECK(scene.materialDraws(scene.material, "velocity").size() ==
              velocity_draws);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 a define override compiles its own shaders and shares the "
          "material record",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(scene);
        const auto base_count =
            GET_MODULE(PipelineFactory).registrationCount();
        const auto started = std::chrono::steady_clock::now();
        installLive(stepsForKey(
            loopKey(), {MaterialDrawStep::overridden({}, {"LP2_MARK"})}));
        const auto elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        WARN("LP-2 one define override (vertex + fragment compile and "
             "pipeline) took "
             << elapsed << " ms");
        REQUIRE(liveTable().defineCompileCount() == 1);
        REQUIRE(GET_MODULE(PipelineFactory).registrationCount() ==
                base_count + 1);
        const auto marked_pixels = scene.render();
        const auto marked = countColors(marked_pixels);
        CHECK(marked.yellow > 0);
        CHECK(marked.blue == 0);
        const auto draws = scene.materialDraws(scene.material);
        REQUIRE_FALSE(draws.empty());
        CHECK(draws.front().pipeline !=
              GET_MODULE(MaterialContainer)
                  .materialPipeline(scene.material)
                  .value);
        CHECK(draws.front().gpu_material_index ==
              static_cast<std::uint32_t>(scene.material.value));

        // Taking the rule away retires the override and its shaders.
        installLive([](const MaterialDrawSubject &)
                        -> std::optional<MaterialDrawStepList> {
            return std::nullopt;
        });
        CHECK(liveTable().defineCompileCount() == 0);
        CHECK(GET_MODULE(PipelineFactory).registrationCount() == base_count);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 a surface reload rebuilds the define override too",
          "[headless][render][lp2][draw-loop][hot-reload]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(scene);
        installLive(stepsForKey(
            loopKey(), {MaterialDrawStep::overridden({}, {"LP2_MARK"})}));
        const auto marked = countColors(scene.render());
        REQUIRE(marked.yellow > 0);
        const auto override_pipeline =
            scene.materialDraws(scene.material).front().pipeline;

        // The surface now marks in green. Only the override compiled the
        // marked branch, so only a rebuilt override can show it.
        auto source = std::string{loopSurfaceSource};
        const std::string yellow = "color = vec3(1.0, 1.0, 0.02);";
        const auto at = source.find(yellow);
        REQUIRE(at != std::string::npos);
        source.replace(at, yellow.size(), "color = vec3(0.02, 1.0, 0.02);");
        writeText(scene.temp_dir / "shaders" / "loop.surface", source);
        auto &reload = GET_MODULE(watch::ReloadService);
        REQUIRE(reload.applyRequestForTesting(
            {watch::makeAssetKey("shaders/loop.surface"),
             watch::ReloadKind::modified, {}, 1}));
        const auto reloaded = countColors(scene.render());
        CHECK(reloaded.yellow == 0);
        CHECK(reloaded.green >= marked.yellow);
        // Rebuilt in place: the table still names the same pipeline.
        CHECK(scene.materialDraws(scene.material).front().pipeline ==
              override_pipeline);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 overrides live and die with their generation",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        // The rule is in place before the first generation is published;
        // the material registered afterwards is judged by it at once.
        *scene.steps = stepsForKey(loopKey(), twoDraws());
        setUpDrawLoopScene(scene);
        REQUIRE(liveTable().overridePipelineCount() == 1);
        REQUIRE(liveTable().find("deferred_geometry", scene.material) !=
                nullptr);
        REQUIRE(countColors(scene.render()).red > 0);
        REQUIRE(liveTableCount() == 1);
        auto &factory = GET_MODULE(PipelineFactory);
        const auto published_count = factory.registrationCount();

        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        auto snapshot = runtime.snapshot();
        REQUIRE(snapshot != nullptr);
        const auto prepare_candidate = [&] {
            std::vector<RenderPipelineProgramPreparation> programs;
            for (const auto id : snapshot->rendering_pass_ids) {
                const auto &program = *snapshot->find(id);
                const auto &graph = program.frame_graph;
                programs.push_back(RenderPipelineProgramPreparation{
                    .owner_scope = program.owner_scope,
                    .rendering_pass = program.rendering_pass,
                    .frame_plan = graph.plan,
                    .execution_plan = graph.execution_plan,
                    .render_pipeline = graph.render_pipeline,
                    .target_plan = graph.target_plan,
                    .native_scopes = graph.native_scopes,
                    .render_target_bindings = graph.render_target_bindings,
                    .buffer_bindings = graph.buffer_bindings,
                    .rendering_pass_id = id,
                    .value_graph = graph.value_graph,
                    .inactive_graph_scopes = graph.inactive_graph_scopes,
                    .graph_owner_objects = graph.graph_owner_objects,
                    .source_documents = graph.source_documents,
                });
            }
            return runtime.prepareGeneration(
                std::move(programs), snapshot->enabled_feature_names);
        };

        {
            // A candidate builds overrides of its own ...
            auto candidate = prepare_candidate();
            REQUIRE(candidate.valid());
            REQUIRE(candidate.candidate().material_draw_loops
                        ->overridePipelineCount() == 1);
            REQUIRE(factory.registrationCount() == published_count + 1);
            REQUIRE(liveTableCount() == 2);
            // ... and a refused one takes them with it.
            runtime.rollbackPreparedGeneration(candidate);
        }
        GET_MODULE(VulkanManageCore).waitIdle();
        CHECK(factory.registrationCount() == published_count);
        CHECK(liveTableCount() == 1);

        // A published candidate replaces the old generation; once no frame
        // holds the old one, its overrides are gone too.
        auto accepted = prepare_candidate();
        snapshot.reset();
        REQUIRE(factory.registrationCount() == published_count + 1);
        runtime.publishPreparedGeneration(std::move(accepted));
        for (int frame = 0; frame < 4; ++frame) {
            CHECK(countColors(scene.render()).red > 0);
        }
        CHECK(liveTableCount() == 1);
        CHECK(factory.registrationCount() == published_count);
        CHECK(liveTable().generation() == runtime.activeGeneration());
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 a generation reload that rebuilds the base rebuilds its "
          "overrides from the base's candidate description",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        *scene.steps = stepsForKey(loopKey(), twoDraws());
        setUpDrawLoopScene(scene);
        auto &materials = GET_MODULE(MaterialContainer);
        auto &factory = GET_MODULE(PipelineFactory);
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        const auto base = materials.materialPipeline(scene.material);
        const auto *steps = liveTable().find("deferred_geometry",
                                             scene.material);
        REQUIRE(steps != nullptr);
        REQUIRE(steps->size() == 2);
        REQUIRE(steps->front().pipeline.has_value());
        const auto live_override = *steps->front().pipeline;
        REQUIRE_FALSE(factory.graphicsDesc(base).allow_ray_query);
        REQUIRE_FALSE(factory.graphicsDesc(live_override).allow_ray_query);

        // A candidate whose material passes declare ray queries: the base
        // pipeline is rebuilt in place with the ray-query frame set allowed.
        auto snapshot = runtime.snapshot();
        REQUIRE(snapshot != nullptr);
        std::vector<RenderPipelineProgramPreparation> programs;
        for (const auto id : snapshot->rendering_pass_ids) {
            const auto &program = *snapshot->find(id);
            const auto &graph = program.frame_graph;
            auto rendering_pass = program.rendering_pass;
            for (auto &compiled : rendering_pass.passes) {
                if (compiled.definition.isMaterial()) {
                    compiled.definition.ray_query = true;
                }
            }
            programs.push_back(RenderPipelineProgramPreparation{
                .owner_scope = program.owner_scope,
                .rendering_pass = std::move(rendering_pass),
                .frame_plan = graph.plan,
                .execution_plan = graph.execution_plan,
                .render_pipeline = graph.render_pipeline,
                .target_plan = graph.target_plan,
                .native_scopes = graph.native_scopes,
                .render_target_bindings = graph.render_target_bindings,
                .buffer_bindings = graph.buffer_bindings,
                .rendering_pass_id = id,
                .value_graph = graph.value_graph,
                .inactive_graph_scopes = graph.inactive_graph_scopes,
                .graph_owner_objects = graph.graph_owner_objects,
                .source_documents = graph.source_documents,
            });
        }
        auto candidate = runtime.prepareGeneration(
            std::move(programs), snapshot->enabled_feature_names);
        REQUIRE(candidate.valid());
        const auto *candidate_steps =
            candidate.candidate().material_draw_loops->find(
                "deferred_geometry", scene.material);
        REQUIRE(candidate_steps != nullptr);
        REQUIRE(candidate_steps->front().pipeline.has_value());
        const auto candidate_override = *candidate_steps->front().pipeline;
        REQUIRE(candidate_override != live_override);

        const auto plan =
            materials.prepareRuntimeGenerationReload(candidate.candidate());
        const auto desc_of = [&](PipelineHandle handle)
            -> std::optional<GraphicsPipelineDesc> {
            for (const auto &entry : plan.pipeline_overrides) {
                if (entry.handle == handle) return entry.desc;
            }
            return std::nullopt;
        };
        const auto base_desc = desc_of(base);
        REQUIRE(base_desc.has_value());
        CHECK(base_desc->allow_ray_query);
        // Both tables' overrides of the base follow it, and keep their own
        // render state.
        for (const auto handle : {live_override, candidate_override}) {
            CAPTURE(handle.value);
            const auto desc = desc_of(handle);
            REQUIRE(desc.has_value());
            CHECK(desc->allow_ray_query);
            CHECK(desc->cull_mode == vk::CullModeFlagBits::eFront);
            auto expected = *base_desc;
            expected.cull_mode = vk::CullModeFlagBits::eFront;
            CHECK(*desc == expected);
        }
        runtime.rollbackPreparedGeneration(candidate);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 a glTF loaded later is judged by the live rules",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        // Material 0 of the glTF draws nothing; material 1 is left alone.
        // glTF materials are matched by asset and index, not by name.
        *scene.steps = [](const MaterialDrawSubject &subject)
            -> std::optional<MaterialDrawStepList> {
            if (subject.key == "models/tri.glb#material[0]") {
                return MaterialDrawStepList{};
            }
            return std::nullopt;
        };
        setUpDrawLoopScene(scene);
        const auto path = scene.temp_dir / "models" / "tri.glb";
        std::filesystem::create_directories(path.parent_path());
        TestGltfFragmentFixture::writeGlb(path);
        auto &gltf = GET_MODULE(GltfLoader);
        auto model = gltf.commit(gltf.prepareGltfBinary(path.string()),
                                 GltfAuthoredAssetReference{"models/tri.glb"});
        GlobalMaterialId first{-1};
        GlobalMaterialId second{-1};
        for (const auto &group : model.material_primitives) {
            if (group.source_material_index == 0u) first = group.material;
            if (group.source_material_index == 1u) second = group.material;
        }
        REQUIRE(first.value >= 0);
        REQUIRE(second.value >= 0);
        auto &materials = GET_MODULE(MaterialContainer);
        CHECK(materials.drawLoopSubject(first).key ==
              "models/tri.glb#material[0]");
        CHECK(materials.drawLoopSubject(second).key ==
              "models/tri.glb#material[1]");
        const auto *steps = liveTable().find("deferred_geometry", first);
        REQUIRE(steps != nullptr);
        CHECK(steps->empty());
        CHECK(liveTable().find("deferred_geometry", second) == nullptr);

        const auto instance =
            GET_MODULE(PolygonInstanceContainer).placeModelInstance(model);
        REQUIRE(GET_MODULE(PolygonInstanceContainer)
                    .isModelInstanceAlive(instance));
        (void)scene.render();
        CHECK(scene.materialDraws(first).empty());
        CHECK_FALSE(scene.materialDraws(second).empty());
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE("LP-2 refuses overrides it cannot draw, by name",
          "[headless][render][lp2][draw-loop]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    runDrawLoopCase("Vulkan LP-2 draw loop unavailable", [](DrawLoopScene &scene) {
        setUpDrawLoopScene(scene);
        installLive(stepsForKey(loopKey(), twoDraws()));
        auto &factory = GET_MODULE(PipelineFactory);
        const auto count = factory.registrationCount();
        const auto *before = liveTable().find("deferred_geometry",
                                              scene.material);
        REQUIRE(before != nullptr);
        const auto before_steps = *before;

        // Blending an opaque material moves it to the transparent phase.
        MaterialDrawRenderStateOverride blend;
        blend.blend = SurfaceBlendMode::blend;
        try {
            installLive(stepsForKey(
                loopKey(), {MaterialDrawStep::overridden(blend)}));
            FAIL("phase crossing was accepted");
        } catch (const std::runtime_error &error) {
            CHECK_THAT(error.what(),
                       Catch::Matchers::StartsWith(
                           "graph.loop_phase_crossing@1: "));
            CHECK_THAT(error.what(),
                       Catch::Matchers::ContainsSubstring(
                           "\"material\":\"" + loopKey() + "\""));
        }
        // A refused install leaves the table as it was.
        CHECK(*liveTable().find("deferred_geometry", scene.material) ==
              before_steps);
        CHECK(factory.registrationCount() == count);

        // A define that declares another binding changes the layout.
        const auto masked = registerSurfaceMaterial(
            maskedSurfaceReference, maskedSurfaceSource,
            nlohmann::json{
                {"schema", "pelican.material"},
                {"version", 1},
                {"materials",
                 nlohmann::json::array({{
                     {"name", "masked"},
                     {"surface", std::string{maskedSurfaceReference}},
                 }})},
            },
            "masked");
        const auto masked_key =
            GET_MODULE(MaterialContainer).drawLoopSubject(masked).key;
        const auto shader_count =
            GET_MODULE(ShaderLibrary).registrationCount();
        const auto masked_count = factory.registrationCount();
        try {
            installLive(stepsForKey(
                masked_key,
                {MaterialDrawStep::overridden({}, {"LP2_EXTRA_BINDING"})}));
            FAIL("an override with another layout was accepted");
        } catch (const std::runtime_error &error) {
            WARN(error.what());
            CHECK_THAT(error.what(),
                       Catch::Matchers::StartsWith(
                           "graph.loop_override_interface@1: "));
            CHECK_THAT(error.what(),
                       Catch::Matchers::ContainsSubstring(
                           "lp2_extra_texture (set 1, binding 31)"));
        }
        CHECK(factory.registrationCount() == masked_count);
        CHECK(GET_MODULE(ShaderLibrary).registrationCount() == shader_count);

        // More define overrides than one generation may compile.
        MaterialDrawStepList many;
        for (std::size_t index = 0;
             index <= materialDrawOverrideCompileLimit; ++index) {
            many.push_back(MaterialDrawStep::overridden(
                {}, {"LP2_VARIANT_" + std::to_string(index)}));
        }
        const auto started = std::chrono::steady_clock::now();
        try {
            installLive(stepsForKey(loopKey(), many));
            FAIL("the compile limit was not enforced");
        } catch (const std::runtime_error &error) {
            CHECK_THAT(error.what(),
                       Catch::Matchers::StartsWith(
                           "graph.loop_override_compile_limit@1: "));
            CHECK_THAT(error.what(),
                       Catch::Matchers::ContainsSubstring(
                           "\"limit\":" +
                           std::to_string(materialDrawOverrideCompileLimit)));
        }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        WARN("LP-2 " << materialDrawOverrideCompileLimit
                     << " define overrides compiled in " << elapsed
                     << " ms before the limit refused the next one");
        CHECK(factory.registrationCount() == masked_count);
        CHECK(GET_MODULE(ShaderLibrary).registrationCount() == shader_count);
    });
#else
    SUCCEED("LP-2 GPU acceptance is compiler-ON only");
#endif
}

} // namespace Pelican
