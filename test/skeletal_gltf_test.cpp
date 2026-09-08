#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"
#include "../src/core/userpublic/components/animation.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/animation/curve.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/scene.hpp"
#include "scene_load_test_support.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/graphcomposition.hpp"
#include "skeletal_fixture.hpp"
#include "graph_composition_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {
void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary}; file << value;
}
void writeRenderingConfig(const std::filesystem::path &root) {
    std::filesystem::create_directories(root / "passes");
    writeText(root / "passes/main_rendering_config.json",
              R"({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"}})");
}
struct TempDir { std::filesystem::path path; ~TempDir() { std::filesystem::remove_all(path); } };
AssetFragmentRef animationFragment(std::string name) {
    return {.kind = "animation", .path = std::move(name),
            .address_kind = AssetFragmentAddressKind::name};
}
} // namespace

TEST_CASE("prepared glTF attributes use shader array layout without GPU allocation", "[gltf][skeletal][graph-attribute]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_attribute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    FastModuleContainer modules;
    auto &loader = GET_MODULE(GltfLoader);
    const auto prepared = loader.prepareGltfBinary((root / "character.glb").string());
    REQUIRE(prepared.dataFiles() == std::vector<std::filesystem::path>{
        std::filesystem::weakly_canonical(root / "character.glb")});
    using Element = ShaderResourceBufferElement;
    const auto positions = loader.readAttribute(prepared, 0, 0, "POSITION", Element::vec3);
    REQUIRE(positions.element == Element::vec3);
    REQUIRE(positions.count == 4);
    REQUIRE(positions.bytes.size() == 64);
    std::array<float, 16> values;
    std::memcpy(values.data(), positions.bytes.data(), positions.bytes.size());
    REQUIRE(values[0] == -0.3f);
    REQUIRE(values[4] == 0.3f);
    REQUIRE(values[3] == 0.0f);
    REQUIRE(values[7] == 0.0f);
    REQUIRE(values[11] == 0.0f);
    REQUIRE(values[15] == 0.0f);
    const auto joints = loader.readAttribute(prepared, 0, 0, "JOINTS_0", Element::uvec4);
    REQUIRE(joints.count == 4);
    REQUIRE(joints.bytes.size() == 64);
    std::array<std::uint32_t, 16> joint_values;
    std::memcpy(joint_values.data(), joints.bytes.data(), joints.bytes.size());
    REQUIRE(joint_values[0] == 0);
    REQUIRE(joint_values[8] == 1);
    const auto indices = loader.readIndices(prepared, 0, 0);
    REQUIRE(indices.element == Element::unsigned_integer);
    REQUIRE(indices.count == 6);
    REQUIRE(indices.bytes.size() == 24);
    std::array<std::uint32_t, 6> index_values;
    std::memcpy(index_values.data(), indices.bytes.data(), indices.bytes.size());
    REQUIRE(index_values == std::array<std::uint32_t, 6>{0, 1, 2, 2, 1, 3});
    REQUIRE(indices.source_files == positions.source_files);
    REQUIRE_THROWS(loader.readIndices(PreparedGltf{}, 0, 0));
    REQUIRE_THROWS(loader.readIndices(prepared, 1, 0));
    TestSkeletalFixture::writeGltf(root / "morph.gltf", "morph.bin", {.morph_target = true});
    const auto morph = loader.prepareGltf((root / "morph.gltf").string());
    const auto delta = loader.readAttribute(morph, 0, 0, "POSITION", Element::vec3, 0);
    REQUIRE(delta.count == positions.count);
    REQUIRE(delta.bytes.size() == positions.bytes.size());
    std::memcpy(values.data(), delta.bytes.data(), delta.bytes.size());
    REQUIRE(values[0] == 2.0f);
    REQUIRE(values[3] == 0.0f);
    REQUIRE_THROWS(loader.readAttribute(morph, 0, 0, "POSITION", Element::vec3, 1));
    REQUIRE_THROWS(loader.readAttribute(morph, 0, 0, "POSITION", Element::vec4, 0));
    auto malformed_morph = nlohmann::json::parse(std::ifstream{root / "morph.gltf"});
    const auto delta_accessor = malformed_morph["meshes"][0]["primitives"][0]["targets"][0]["POSITION"].get<int>();
    malformed_morph["accessors"][delta_accessor]["count"] = 3;
    writeText(root / "morph.gltf", malformed_morph.dump());
    REQUIRE_THROWS(loader.readAttribute(loader.prepareGltf((root / "morph.gltf").string()),
        0, 0, "POSITION", Element::vec3, 0));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "POSITION", Element::vec2));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "POSITION", Element::ivec3));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "JOINTS_0", Element::ivec4));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "MISSING", Element::vec3));
    REQUIRE_THROWS(loader.readAttribute(prepared, 1, 0, "POSITION", Element::vec3));
    REQUIRE_THROWS(loader.readAttribute(PreparedGltf{}, 0, 0, "POSITION", Element::vec3));
    REQUIRE(FastModuleContainer::tryGet<VulkanManageCore>() == nullptr);
    REQUIRE(FastModuleContainer::tryGet<StandardMaterialResource>() == nullptr);
}

TEST_CASE("prepared glTF dependencies retain resolved external buffer files", "[skeletal][graph-attribute]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_gltf_sources_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    const std::array<float, 6> positions{-1, 0, 0, 2, 0, 0};
    {
        std::ofstream binary{root / "positions data.bin", std::ios::binary};
        binary.write(reinterpret_cast<const char *>(positions.data()), sizeof(positions));
    }
    writeText(root / "unused.bin", "data");
    writeText(root / "mesh.gltf", R"({"asset":{"version":"2.0"},
      "buffers":[{"uri":"positions%20data.bin","byteLength":24},{"uri":"unused.bin","byteLength":4}],
      "bufferViews":[{"buffer":0,"byteLength":24}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"VEC3"}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})");
    FastModuleContainer modules;
    const auto &loader = GET_MODULE(GltfLoader);
    const auto prepared = loader.prepareGltf((root / "mesh.gltf").string());
    REQUIRE(prepared.dataFiles() == std::vector<std::filesystem::path>{
        std::filesystem::weakly_canonical(root / "mesh.gltf"),
        std::filesystem::weakly_canonical(root / "positions data.bin"),
        std::filesystem::weakly_canonical(root / "unused.bin")});
    const auto attribute = loader.readAttribute(prepared, 0, 0, "POSITION", ShaderResourceBufferElement::vec3);
    REQUIRE(attribute.source_files == std::vector<std::filesystem::path>{
        std::filesystem::weakly_canonical(root / "mesh.gltf"),
        std::filesystem::weakly_canonical(root / "positions data.bin")});
    REQUIRE(attribute.count == 2);
    std::array<float, 8> values{};
    REQUIRE(attribute.bytes.size() == sizeof(values));
    std::memcpy(values.data(), attribute.bytes.data(), attribute.bytes.size());
    REQUIRE(values[0] == -1);
    REQUIRE(values[4] == 2);
    REQUIRE_THROWS(loader.readIndices(prepared, 0, 0));
    auto indexed_document = nlohmann::json::parse(std::ifstream{root / "mesh.gltf"});
    indexed_document["bufferViews"].push_back({{"buffer", 1}, {"byteLength", 4}});
    indexed_document["accessors"].push_back({{"bufferView", 1}, {"componentType", 5121},
        {"count", 3}, {"type", "SCALAR"}});
    indexed_document["meshes"][0]["primitives"][0]["indices"] = 1;
    writeText(root / "mesh.gltf", indexed_document.dump());
    writeText(root / "unused.bin", std::string{"\0\1\2\0", 4});
    REQUIRE_THROWS(loader.readIndices(loader.prepareGltf((root / "mesh.gltf").string()), 0, 0));
    writeText(root / "unused.bin", std::string{"\0\1\0\0", 4});
    const auto indices = loader.readIndices(loader.prepareGltf((root / "mesh.gltf").string()), 0, 0);
    REQUIRE(indices.count == 3);
    REQUIRE(indices.bytes.size() == 12);
    std::array<std::uint32_t, 3> index_values;
    std::memcpy(index_values.data(), indices.bytes.data(), indices.bytes.size());
    REQUIRE(index_values == std::array<std::uint32_t, 3>{0, 1, 0});
    REQUIRE(indices.source_files == std::vector<std::filesystem::path>{
        std::filesystem::weakly_canonical(root / "mesh.gltf"),
        std::filesystem::weakly_canonical(root / "unused.bin")});
    REQUIRE_THROWS(PreparedGltf{}.dataFiles());
    REQUIRE(FastModuleContainer::tryGet<VulkanManageCore>() == nullptr);
}

TEST_CASE("common graph glTF source supplies counted GPU shader attributes", "[skeletal][graph-attribute-runtime]") {
    bool shared_model = false;
    bool external_buffer = false;
    SECTION("graph source only") {}
    SECTION("shared model and graph source") { shared_model = true; }
    SECTION("shared model and external buffer") { shared_model = true; external_buffer = true; }
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_source_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::create_directories(root / "passes");
    const TempDir guard{root};
    const std::string source_file = external_buffer ? "character.gltf" : "character.glb";
    const auto write_source = [&](TestSkeletalFixture::Options options = {}) {
        if (external_buffer) TestSkeletalFixture::writeGltf(root / source_file, "character.bin", options);
        else TestSkeletalFixture::writeGlb(root / source_file, options);
    };
    write_source();
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    pelican_store_destination(0, float(pelican_count_source()) +
        pelican_load_source(1).x - pelican_load_source(0).x);
})");
    auto document = parameterGraphCompositionFixture();
    auto &attribute = document["nodes"][5];
    attribute["op"] = "render.attribute";
    attribute.erase("inputs");
    attribute["parameters"] = {{"element", "vec3"}, {"domain", "vertex"},
        {"source", {{"asset", "project://" + source_file}, {"mesh", 0}, {"primitive", 0}, {"semantic", "POSITION"}}}};
    document["nodes"][7]["parameters"]["ports"]["source"] = {
        {"element", "vec3"}, {"domain", "vertex"}, {"access", "read"}};
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "assets.json", nlohmann::json{{"schema", "pelican.asset_data"}, {"version", 1},
        {"models", shared_model ? nlohmann::json::array({{{"name", "character"}, {"path", source_file}}})
                                : nlohmann::json::array()}}.dump());
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
    auto &renderer = GET_MODULE(Renderer);
    renderer.render();
    auto &vkcore = GET_MODULE(VulkanManageCore);
    vkcore.waitIdle();
    const auto bytes = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float));
    float value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    REQUIRE(std::abs(value - 4.6f) < 0.00001f);
    const auto generation = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(generation);
    REQUIRE_FALSE(generation->programs.empty());
    const auto &source_files = generation->programs.begin()->second.frame_graph.attribute_source_files;
    std::vector<std::filesystem::path> expected_sources{std::filesystem::weakly_canonical(root / source_file)};
    if (external_buffer) expected_sources.push_back(std::filesystem::weakly_canonical(root / "character.bin"));
    std::sort(expected_sources.begin(), expected_sources.end());
    REQUIRE(source_files == expected_sources);
    const auto graph = generation->programs.begin()->second.frame_graph.value_graph;
    REQUIRE(graph);
    REQUIRE(nlohmann::json::parse(graph->serialize()) == document);
    auto bad = document;
    bad["nodes"][5]["parameters"]["source"]["semantic"] = "MISSING";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(bad.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == generation);
    renderer.render();
    vkcore.waitIdle();
    const auto retained = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float));
    std::memcpy(&value, retained.data(), sizeof(value));
    REQUIRE(std::abs(value - 4.6f) < 0.00001f);
    auto &reload = GET_MODULE(watch::ReloadService);
    auto &models = GET_MODULE(ModelAssetContainer);
    const auto revision = shared_model ? models.contentRevisionForTesting("character") : 0;
    const watch::ReloadRequest changed{.key = watch::makeAssetKey(external_buffer ? "character.bin" : source_file)};
    write_source(TestSkeletalFixture::Options{.position_scale = 2});
    REQUIRE(reload.applyRequestForTesting(changed));
    const auto updated = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(updated != generation);
    const auto rendered = [&] {
        renderer.render();
        vkcore.waitIdle();
        const auto data = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float));
        float result{};
        std::memcpy(&result, data.data(), sizeof(result));
        return result;
    };
    REQUIRE(std::abs(rendered() - 5.2f) < 0.00001f);
    if (shared_model) REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    write_source(TestSkeletalFixture::Options{.position_scale = 3});
    {
        watch::ScopedRenderPipelineCompanionReloadObserver fail_after_model_prepare{
            [](auto) { throw std::runtime_error("reject coordinated candidate"); }};
        REQUIRE_FALSE(reload.applyRequestForTesting(changed));
    }
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == updated);
    REQUIRE(std::abs(rendered() - 5.2f) < 0.00001f);
    if (shared_model) {
        REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
        write_source(TestSkeletalFixture::Options{
            .invalid_inverse_bind_buffer_view = true, .position_scale = 3});
        REQUIRE_FALSE(reload.applyRequestForTesting(changed));
        REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == updated);
        REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    }
    write_source(TestSkeletalFixture::Options{.position_scale = 4});
    REQUIRE(reload.applyRequestForTesting(changed));
    REQUIRE(std::abs(rendered() - 6.4f) < 0.00001f);
    if (shared_model) REQUIRE(models.contentRevisionForTesting("character") == revision + 2);
    if (external_buffer) {
        REQUIRE(models.handlesReload(changed.key));
        TestSkeletalFixture::writeGltf(root / source_file, "replacement.bin", {.position_scale = 5});
        const auto before_switch = GET_MODULE(RenderingPassContainer).snapshot();
        {
            watch::ScopedRenderPipelineCompanionReloadObserver reject_new_dependencies{
                [](auto) { throw std::runtime_error("reject buffer source switch"); }};
            REQUIRE_FALSE(reload.applyRequestForTesting({.key = watch::makeAssetKey(source_file)}));
        }
        REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == before_switch);
        REQUIRE(models.contentRevisionForTesting("character") == revision + 2);
        REQUIRE(models.handlesReload(changed.key));
        REQUIRE_FALSE(models.handlesReload(watch::makeAssetKey("replacement.bin")));
        REQUIRE(std::abs(rendered() - 6.4f) < 0.00001f);
        std::filesystem::remove(root / "character.bin");
        const std::array changes{
            watch::ReloadRequest{.key = watch::makeAssetKey(source_file)},
            watch::ReloadRequest{.key = changed.key, .kind = watch::ReloadKind::removed}};
        REQUIRE(reload.applyRequestsForTesting(changes) == std::vector<bool>{true, true});
        REQUIRE(std::abs(rendered() - 7.0f) < 0.00001f);
        REQUIRE(models.contentRevisionForTesting("character") == revision + 3);
        REQUIRE_FALSE(models.handlesReload(changed.key));
        const watch::ReloadRequest replacement_change{.key = watch::makeAssetKey("replacement.bin")};
        REQUIRE(models.handlesReload(replacement_change.key));
        const auto switched = GET_MODULE(RenderingPassContainer).snapshot();
        REQUIRE(reload.applyRequestForTesting(changed)); // Former dependency is no longer claimed.
        REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == switched);
        std::filesystem::remove(root / "replacement.bin");
        REQUIRE_FALSE(reload.applyRequestForTesting({.key = replacement_change.key, .kind = watch::ReloadKind::removed}));
        REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == switched);
        REQUIRE(models.contentRevisionForTesting("character") == revision + 3);
        REQUIRE(models.handlesReload(replacement_change.key));
        TestSkeletalFixture::writeGltf(root / source_file, "replacement.bin", {.position_scale = 6});
        REQUIRE(reload.applyRequestForTesting(replacement_change));
        REQUIRE(std::abs(rendered() - 7.6f) < 0.00001f);
        REQUIRE(models.contentRevisionForTesting("character") == revision + 4);
    }
    TestSkeletalFixture::writeGlb(root / "replacement.glb");
    auto replacement = document;
    replacement["nodes"][5]["parameters"]["source"]["asset"] = "project://replacement.glb";
    REQUIRE_NOTHROW(loadRenderGraphVariantsFromConfigData(replacement.dump()));
    const auto next = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(next != generation);
    REQUIRE_FALSE(next->programs.empty());
    REQUIRE(next->programs.begin()->second.frame_graph.attribute_source_files ==
        std::vector<std::filesystem::path>{std::filesystem::weakly_canonical(root / "replacement.glb")});
    REQUIRE(source_files == expected_sources);
    replacement["nodes"][5]["parameters"] = {{"element", "uint"}, {"domain", "index"},
        {"source", {{"asset", "project://" + source_file}, {"mesh", 0}, {"primitive", 0}}}};
    replacement["nodes"][7]["parameters"]["ports"]["source"] = {
        {"element", "uint"}, {"domain", "index"}, {"access", "read"}};
    writeText(root / "shaders/index_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    pelican_store_destination(0, float(pelican_count_source() + pelican_load_source(5)));
})");
    replacement["nodes"][7]["parameters"]["shader"] = "shaders/index_probe";
    REQUIRE_NOTHROW(loadRenderGraphVariantsFromConfigData(replacement.dump()));
    REQUIRE(std::abs(rendered() - 9.0f) < 0.00001f);
    const auto indexed = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(indexed);
    REQUIRE_FALSE(indexed->programs.empty());
    const auto &indexed_graph = indexed->programs.begin()->second.frame_graph;
    REQUIRE(indexed_graph.value_graph);
    REQUIRE(nlohmann::json::parse(indexed_graph.value_graph->serialize()) == replacement);
    auto invalid_index = replacement;
    invalid_index["nodes"][5]["parameters"]["element"] = "float";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid_index.dump()));
    invalid_index = replacement;
    invalid_index["nodes"][7]["parameters"]["ports"]["source"]["domain"] = "vertex";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid_index.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == indexed);
    TestSkeletalFixture::writeGltf(root / "morph.gltf", "morph.bin", {.morph_target = true});
    auto morphed = document;
    morphed["nodes"][5]["parameters"]["source"]["asset"] = "project://morph.gltf";
    auto delta = morphed["nodes"][5];
    delta["id"] = "morph_delta";
    delta["parameters"]["source"]["morph_target"] = 0;
    morphed["nodes"].push_back(delta);
    morphed["nodes"].push_back({{"id", "morph_weight"}, {"op", "value.constant"},
        {"parameters", {{"value", 0.5}}}, {"outputs", {"value"}}});
    morphed["nodes"].push_back({{"id", "weight_buffer"}, {"op", "render.buffer"},
        {"parameters", {{"element", "float"}}},
        {"inputs", {{"value", {{"node", "morph_weight"}, {"port", "value"}}}}}, {"outputs", {"resource"}}});
    auto &morph_compute = morphed["nodes"][7];
    morph_compute["parameters"]["shader"] = "shaders/morph_probe";
    morph_compute["parameters"]["ports"]["delta"] = {{"element", "vec3"}, {"domain", "vertex"}, {"access", "read"}};
    morph_compute["parameters"]["ports"]["weight"] = {{"element", "float"}, {"access", "read"}};
    morph_compute["inputs"]["delta"] = {{"node", "morph_delta"}, {"port", "resource"}};
    morph_compute["inputs"]["weight"] = {{"node", "weight_buffer"}, {"port", "resource"}};
    writeText(root / "shaders/morph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    vec3 position = pelican_load_source(0) + pelican_load_delta(0) * pelican_load_weight(0);
    pelican_store_destination(0, position.x);
})");
    REQUIRE_NOTHROW(loadRenderGraphVariantsFromConfigData(morphed.dump()));
    REQUIRE(std::abs(rendered() - 0.7f) < 0.00001f);
    const auto morph_generation = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(morph_generation);
    REQUIRE_FALSE(morph_generation->programs.empty());
    REQUIRE(morph_generation->programs.begin()->second.frame_graph.value_graph);
    REQUIRE(nlohmann::json::parse(morph_generation->programs.begin()->second.frame_graph.value_graph->serialize()) == morphed);
    for (auto &node : morphed["nodes"])
        if (node["id"] == "morph_weight") node["parameters"]["value"] = 2.0;
    REQUIRE_NOTHROW(loadRenderGraphVariantsFromConfigData(morphed.dump()));
    REQUIRE(std::abs(rendered() - 3.7f) < 0.00001f);
    const auto retained_morph = GET_MODULE(RenderingPassContainer).snapshot();
    for (auto &node : morphed["nodes"])
        if (node["id"] == "morph_delta") node["parameters"]["source"]["morph_target"] = 1;
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(morphed.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == retained_morph);
    REQUIRE(std::abs(rendered() - 3.7f) < 0.00001f);
}

TEST_CASE("owned common graph reads live model morph weights", "[skeletal][graph-live-morph]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_live_morph_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb", {.morph_target = true});
    auto document = parameterGraphCompositionFixture();
    document["definitions"]["live_weight"] = {
        {"nodes", {{{"id", "weight"}, {"op", "value.morph_weight"},
            {"parameters", {{"node", 0}, {"target", 0}}}, {"outputs", {"value"}}}}},
        {"outputs", {{"value", {{"node", "weight"}, {"port", "value"}}}}}};
    for (const auto id : {"base", "delta"}) {
        nlohmann::json source{{"asset", "project://character.glb"}, {"mesh", 0}, {"primitive", 0}, {"semantic", "POSITION"}};
        if (std::string_view{id} == "delta") source["morph_target"] = 0;
        document["nodes"].push_back({{"id", id}, {"op", "render.attribute"},
            {"parameters", {{"element", "vec3"}, {"domain", "vertex"}, {"source", source}}}, {"outputs", {"resource"}}});
        document["nodes"][7]["parameters"]["ports"][id] = {{"element", "vec3"}, {"domain", "vertex"}, {"access", "read"}};
        document["nodes"][7]["inputs"][id] = {{"node", id}, {"port", "resource"}};
    }
    document["nodes"].push_back({{"id", "model"}, {"op", "graph.instance"}, {"definition", "live_weight"},
        {"owner", {{"scene", "default_scene"}, {"object", 1}}}});
    document["nodes"][5]["inputs"]["value"] = {{"node", "model"}, {"port", "value"}};
    REQUIRE(ValueGraph::Graph{document.dump()}.morphSources().size() == 1);
    auto invalid_source = document;
    invalid_source["nodes"].back().erase("owner");
    REQUIRE_THROWS(ValueGraph::Graph{invalid_source.dump()});
    invalid_source = document;
    invalid_source["definitions"]["live_weight"]["nodes"][0]["parameters"]["target"] = -1;
    REQUIRE_THROWS(ValueGraph::Graph{invalid_source.dump()});
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    vec3 position = pelican_load_base(0) + pelican_load_delta(0) * pelican_load_source(0);
    pelican_store_destination(0, position.x);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":1,"models":[{"name":"character","path":"character.glb"}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
      {"name":"Character","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]}
    ]}}})");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16,16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    GET_MODULE(ECSCore).update();
    const auto object = scene.objectId("Character");
    REQUIRE(object);
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object) document["nodes"].back()["owner"]["object"] = binding.authoring_object_id.value;
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    const auto instance_for = [&](GameObjectId id) {
        const auto *model = ecs.tryComponent<SimpleModelViewComponent>(id);
        REQUIRE(model);
        REQUIRE(model->model_instance_id);
        return *model->model_instance_id;
    };
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto initial = instance_for(*object);
    const auto weight = [&](ModelInstanceId id, float value, std::uint64_t revision) {
        REQUIRE(instances.publishMorphWeightFrame(id, PublishMorphWeightFrameDescV1{
            .layout_generation = instances.morphLayoutGenerationForTesting(id),
            .frame_revision = revision, .weights = &value, .weight_count = 1}) == Animation::Status::ok);
    };
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float));
        float value;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    };
    weight(initial, 0.5f, 1);
    REQUIRE_FALSE(instances.morphWeight(initial, 0, 1));
    REQUIRE(std::abs(render() - 0.7f) < 0.00001f);
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    auto invalid_binding = document;
    invalid_binding["nodes"].push_back({{"id", "bad_sink"}, {"op", "render.scalar"},
        {"inputs", {{"value", {{"node", "model"}, {"port", "value"}}}}},
        {"parameters", {{"target", "missing_buffer"}}}, {"outputs", nlohmann::json::array()}});
    REQUIRE_THROWS_WITH(loadRenderGraphVariantsFromConfigData(invalid_binding.dump()),
        Catch::Matchers::ContainsSubstring("unbound frame graph output: missing_buffer"));
    invalid_binding["nodes"].push_back({{"id", "missing_buffer"}, {"op", "render.buffer"},
        {"parameters", {{"size", 8}, {"element", "float"}, {"host_source", "runtime_f32"}}},
        {"outputs", {"resource"}}});
    REQUIRE_THROWS_WITH(loadRenderGraphVariantsFromConfigData(invalid_binding.dump()),
        Catch::Matchers::ContainsSubstring("incompatible frame graph output buffer: missing_buffer"));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    REQUIRE(std::abs(render() - 0.7f) < 0.00001f);
    weight(initial, 0.75f, 2);
    REQUIRE(std::abs(render() - 1.2f) < 0.00001f);
    GameObjects{}.remove(*object);
    REQUIRE_FALSE(instances.morphWeight(initial, 0, 0));
    REQUIRE_NOTHROW(render());
    test_support::loadScene(scene, "default_scene");
    GET_MODULE(ECSCore).update();
    const auto reloaded = scene.objectId("Character");
    REQUIRE(reloaded);
    const auto replacement = instance_for(*reloaded);
    REQUIRE(replacement != initial);
    REQUIRE(std::abs(render() + 0.3f) < 0.00001f);
    weight(replacement, 2.0f, 1);
    REQUIRE(std::abs(render() - 3.7f) < 0.00001f);
}

TEST_CASE("glTF skin and clips connect to animation component without ECS bones", "[gltf][skeletal]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp38_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    TestSkeletalFixture::writeGlb(root / "cubic.glb", true);
    writeRenderingConfig(root);
    TestSkeletalFixture::writeGlb(
        root / "no_scene.glb", TestSkeletalFixture::Options{.omit_scenes = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_accessor.glb",
        TestSkeletalFixture::Options{.invalid_animation_accessor = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_buffer_view.glb",
        TestSkeletalFixture::Options{.invalid_inverse_bind_buffer_view = true});
    TestSkeletalFixture::writeGlb(
        root / "truncated_accessor.glb",
        TestSkeletalFixture::Options{.truncated_inverse_bind_view = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_default_scene.glb",
        TestSkeletalFixture::Options{.invalid_default_scene = true});
    writeText(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[{"name":"character","path":"character.glb"}]})json");
    writeText(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{
        "name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","clip":"character.glb#animation/Turn","speed":1.0,"loop":true,"start_time":0.0}
        ]}]}}})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true; launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &loader = GET_MODULE(GltfLoader);
    const auto loaded = loader.loadGltfBinary((root / "character.glb").string());
    REQUIRE(loaded.skeletal);
    REQUIRE(loaded.skeletal->joint_nodes.size() == 2);
    REQUIRE(loaded.skeletal->nodes[1].name == "RootJoint");
    REQUIRE(loaded.skeletal->nodes[2].name == "TipJoint");
    REQUIRE(loaded.skeletal->skin_bindings.size() == 1);
    REQUIRE(loaded.skeletal->clips.size() == 2);
    REQUIRE(loaded.material_primitives.size() == 1);
    REQUIRE(loaded.material_primitives.front().primitives.front().skinned);
    const auto clip_only = loader.loadGltfBinary((root / "character.glb").string(),
                                                 animationFragment("Turn"));
    REQUIRE(clip_only.material_primitives.empty());
    REQUIRE(clip_only.skeletal);
    REQUIRE_THROWS_WITH(loader.loadGltfBinary((root / "cubic.glb").string()),
                        Catch::Matchers::ContainsSubstring("animation 'Turn'") &&
                        Catch::Matchers::ContainsSubstring("CUBICSPLINE"));
    REQUIRE(loader.loadGltfBinary((root / "no_scene.glb").string())
                .material_primitives.size() == 1);
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_accessor.glb").string()),
        Catch::Matchers::ContainsSubstring("accessor 17") &&
            Catch::Matchers::ContainsSubstring("out of range"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_buffer_view.glb").string()),
        Catch::Matchers::ContainsSubstring("no valid bufferView"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "truncated_accessor.glb").string()),
        Catch::Matchers::ContainsSubstring("data exceeds its bufferView"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_default_scene.glb").string()),
        Catch::Matchers::ContainsSubstring("default scene index is out of range"));

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.setTime(0.5);
    GET_MODULE(ECSPredefinedRegistration).reg();
    test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
    time.advance();
    GET_MODULE(ECSCore).update();
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.modelInstanceIdForTesting(0);
    const auto first_revision = instances.currentAnimationRevisionForTesting(instance);
    REQUIRE(first_revision != 0);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.advanceTemporalHistoryAfterRender();
    time.setTime(0.75);
    time.advance();
    GET_MODULE(ECSCore).update();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) > first_revision);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.triggerUpdate();
    REQUIRE(instances.getDrawCalls().size() == 1);
    REQUIRE(instances.getDrawCalls().front().skinned);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("animation component rejects competing clip and graph sources", "[skeletal][scene]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp38_graph_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    writeText(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
    writeText(root / "scene.json", R"json({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[{"name":"Bad","components":[
        {"name":"animation","clip":"x.glb#animation/Walk","graph":"walk.anim.json"}
      ]}]}}})json");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    REQUIRE_THROWS_WITH(test_support::loadScene(GET_MODULE(SceneLoader), "default_scene"),
                        Catch::Matchers::ContainsSubstring("exactly one of clip or graph"));
}

TEST_CASE("authored animation graphs publish and release real scene model poses",
          "[gltf][skeletal][animation-graph-component]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_component_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    writeText(root / "assets.json",
              R"({"schema":"pelican.asset_data","version":1,"models":[{"name":"character","path":"character.glb"}]})");
    writeRenderingConfig(root);
    writeText(root / "turn.json", R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0},"initial_state":"turn",
      "states":[{"name":"turn","type":"clip","clip":"Turn","loop":false},
                {"name":"step","type":"clip","clip":"Step","loop":false}],
      "transitions":[{"from":"turn","to":"step","duration":0,
                      "conditions":[{"parameter":"go","op":">","value":0}]}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[
        {"name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","graph":"turn.json"}]},
        {"components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","graph":"turn.json","parameters":{
            "$list":{"v":1,"next_id":2},
            "rows":[{"$row":{"id":1,"v":1},"parameter":"go","value":1}]}}]}
      ]}}})");
    auto &service = Animation::animationServiceRuntime();
    service.reset();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto frame = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto named = scene.objectId("Character");
    REQUIRE(named);
    frame();
    const auto instance = *ecs.tryComponent<SimpleModelViewComponent>(*named)->model_instance_id;
    const auto first_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(first_palette.size() == 2);
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != first_palette);
    auto unnamed = invalidEntityId;
    for (const auto entity : ecs.liveEntities()) {
        if (entity != *named && ecs.tryComponent<AnimationComponent>(entity)) unnamed = entity;
    }
    REQUIRE(unnamed != invalidEntityId);
    REQUIRE_FALSE(scene.objectName(unnamed));
    const auto unnamed_instance = *ecs.tryComponent<SimpleModelViewComponent>(unnamed)->model_instance_id;
    REQUIRE(instances.currentAnimationRevisionForTesting(unnamed_instance) == time.frameIndex());
    // Shared graph, different authored initial values: only the named actor turns.
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(unnamed_instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) > 0.1f);

    GameContext context;
    const auto turn_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(std::abs(turn_palette[0][0][1]) > 0.1f);
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 1) == Animation::Status::ok);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != turn_palette);
    // Step translates the tip only; a continuing Turn would still rotate the root.
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][0] - 1.0f) < 0.0001f);
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 0) == Animation::Status::ok);
    const auto step_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::ok);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != step_palette);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) > 0.1f);

    // Switching to a clip removes the graph writer before phase evaluation.
    auto *animation = ecs.tryComponent<AnimationComponent>(*named);
    animation->graph.clear();
    animation->clip = "character.glb#animation/Turn";
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 1) == Animation::Status::not_found);
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::not_found);
    animation->clip.clear();
    animation->graph = "turn.json";
    frame();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    // Removing the unnamed actor retires both its renderer slot and writer.
    REQUIRE(ecs.remove(unnamed));
    REQUIRE(context.forceAnimationGraphState(unnamed, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE_THROWS(instances.currentAnimationRevisionForTesting(unnamed_instance));
    REQUIRE(context.forceAnimationGraphState(unnamed, "turn") == Animation::Status::not_found);

    test_support::loadScene(scene, "default_scene");
    frame();
    const auto reloaded = scene.objectId("Character");
    REQUIRE(reloaded);
    REQUIRE(*reloaded != *named);
    const auto new_instance = *ecs.tryComponent<SimpleModelViewComponent>(*reloaded)->model_instance_id;
    REQUIRE(new_instance != instance);
    REQUIRE(instances.currentAnimationRevisionForTesting(new_instance) == time.frameIndex());
    const auto reloaded_palette = instances.currentSkinPaletteForTesting(new_instance);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(new_instance) != reloaded_palette);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("curve samples publish to a runtime float render buffer", "[runtime-render-values][skeletal]") {
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto declaration = nlohmann::json::parse(R"({"buffers":[
        {"name":"curve_values","size":8,"host_source":"runtime_f32"}]})");
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(declaration));
    resources.uploadRuntimeValues();
    const auto read = [&] {
        const auto bytes = vkcore.readBuf(resources.buffer("curve_values"), 8);
        std::array<float, 2> values;
        std::memcpy(values.data(), bytes.data(), bytes.size());
        return values;
    };
    REQUIRE(read() == std::array<float, 2>{0.0f, 0.0f});
    const ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"t","op":"input","name":"time"},
        {"id":"shape","op":"curve","time":"t","keys":[{"time":0,"value":0},{"time":1,"value":1}]},
        {"id":"gain","op":"constant","value":2},
        {"id":"scaled","op":"multiply","a":"shape","b":"gain"},
        {"id":"one","op":"constant","value":1},
        {"id":"offset","op":"add","a":"scaled","b":"one"}],
      "outputs":{"values":["scaled","offset"]},
      "bindings":[{"output":"values","kind":"render_buffer","target":"curve_values"}]})"};
    const ValueGraph::Graph restored{graph.serialize()};
    GameContext context;
    ValueGraph::Inputs inputs{{"time", 0.25}};
    const auto published = restored.publish(context, inputs);
    REQUIRE(published.applied == 1);
    REQUIRE(published.unavailable.empty());
    inputs["time"] = 99; // Already published values are a snapshot.
    REQUIRE(read() == std::array<float, 2>{0.0f, 0.0f});
    resources.uploadRuntimeValues();
    REQUIRE(read() == std::array<float, 2>{0.5f, 1.5f});
    std::array<float, 2> values{2.0f, 0.0f};
    REQUIRE_FALSE(context.setRenderValues("missing", values));
    REQUIRE_THROWS(context.setRenderValues("curve_values", std::span{values}.first(1)));
    REQUIRE(context.setRenderValues("curve_values", values));
    resources.uploadRuntimeValues();
    REQUIRE(read() == values);
    auto invalid = declaration;
    invalid["buffers"][0]["size"] = 6;
    REQUIRE_THROWS(parseFrameGraphBufferDefinitionsFromJson(invalid));
    vkcore.waitIdle();
}

TEST_CASE("common graph public ports reach a render buffer", "[skeletal][graph-composition]") {
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(nlohmann::json::parse(
        R"({"buffers":[{"name":"scoped_value","size":4,"host_source":"runtime_f32"}]})")));
    const ValueGraph::Graph graph{graphCompositionFixture};
    const ValueGraph::Graph restored{graph.serialize()};
    REQUIRE(restored.publish(GameContext{}, {{"time", 0.25}}).applied == 1);
    resources.uploadRuntimeValues();
    const auto bytes = vkcore.readBuf(resources.buffer("scoped_value"), sizeof(float));
    float value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    REQUIRE(value == 0.5f);
    vkcore.waitIdle();
}

TEST_CASE("mixed graph publishes scalar values consumed by a compute shader", "[skeletal][graph-composition]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_compute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, pelican_load_source(0) * 3.0 + 1.0); }
)");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    const ValueGraph::Graph authored{parameterGraphCompositionFixture().dump()};
    const ValueGraph::Graph graph{authored.serialize()};
    const auto config = composeRenderFeatureConfig(nlohmann::json::parse(graph.serialize())).config;
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(config));
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto definitions = parseComputeTaskDefinitionsFromConfigJson(config);
    REQUIRE(definitions.size() == 1);
    const auto task = tasks.registerComputeTask(definitions[0], ComputeTaskRuntimeDependencies{
        GET_MODULE(ShaderLibrary), GET_MODULE(PathResolver), GET_MODULE(RenderTargetContainer), resources});
    auto &frames = GET_MODULE(FrameResources);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto run = [&](double time, const ValueGraph::ParameterValues &parameters = ValueGraph::ParameterValues{}) {
        REQUIRE(graph.publish(GameContext{}, {{"time", time}}, {}, {}, parameters).applied == 1);
        resources.uploadRuntimeValues();
        GET_MODULE(VulkanUtils).executeOneTimeCmd([&](vk::CommandBuffer cmd) {
            tasks.dispatch(cmd, task, frames);
            const vk::MemoryBarrier barrier{vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                                {}, barrier, {}, {});
        }, true);
        const auto bytes = vkcore.readBuf(resources.buffer("result"), sizeof(float));
        float value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    };
    REQUIRE(run(0.25) == 2.5f);
    REQUIRE(run(0.75) == 5.5f);
    REQUIRE(run(0.25, {{"left/gain", 4}}) == 4.0f);
    REQUIRE(run(0.25) == 2.5f);
    vkcore.waitIdle();
}

TEST_CASE("renderer evaluates the common graph using committed scene time", "[skeletal][graph-composition]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_frame_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::create_directories(root / "passes");
    const TempDir guard{root};
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, pelican_load_source(0) * 3.0 + 1.0); }
)");
    auto document = parameterGraphCompositionFixture();
    auto right_buffer = document["nodes"][5];
    right_buffer["id"] = "right_render";
    right_buffer["inputs"]["value"]["node"] = "right";
    document["nodes"].push_back(right_buffer);
    auto right_result = document["nodes"][6];
    right_result["id"] = "right_result";
    document["nodes"].push_back(right_result);
    auto right_probe = document["nodes"][7];
    right_probe["id"] = "right_probe";
    right_probe["inputs"]["source"]["node"] = "right_render";
    right_probe["inputs"]["destination"]["node"] = "right_result";
    document["nodes"].push_back(right_probe);
    bool graphics_path = false;
    SECTION("buffer compute path") {}
    SECTION("image compute path") {
        addGraphImageStage(document);
        std::string image_load = "imageLoad(pelican_resource_source, ivec2(0)).x";
        SECTION("storage image read") {}
        SECTION("sampled image read") {
            document["nodes"].back()["parameters"]["ports"]["source"]["binding"] = "sampled";
            image_load = "texelFetch(pelican_resource_source, ivec2(0), 0).x";
        }
        SECTION("graphics attachment write") {
            graphics_path = true;
            document["nodes"][5]["op"] = "render.attribute";
            document["nodes"][5]["parameters"]["domain"] = "instance";
            document["nodes"][5]["parameters"]["count"] = 1;
            auto &writer = document["nodes"][7];
            writer["parameters"]["ports"]["source"]["domain"] = "instance";
            writer["op"] = "render.fullscreen";
            writer["parameters"].erase("dispatch");
            writer["parameters"]["pass_set"] = "main_render";
            writer["parameters"]["shader"] = {{"vertex", "engine://fullscreen"},
                                                 {"fragment", "shaders/graph_image_write"}};
            writer["parameters"]["ports"]["destination"]["attachment"] = "color";
            writer["parameters"]["ports"]["destination"]["location"] = 0;
            writer["parameters"]["color_load_op"] = "clear";
            std::string fragment_input;
            std::string fragment_value = "pelican_count_source() == 1u ? pelican_load_source(0) : -100.0";
            SECTION("fullscreen") {}
            SECTION("raster") {
                writer["op"] = "render.raster";
                writer["parameters"]["draw"] = {{"vertex_count", 3}};
                writer["parameters"]["draw"]["instance_source"] = "source";
                SECTION("direct") {}
                SECTION("indexed mesh") {
                    TestSkeletalFixture::writeGlb(root / "indices.glb");
                    writer["parameters"]["draw"].erase("vertex_count");
                    writer["parameters"]["draw"]["index_source"] = "indices";
                    writer["parameters"]["draw"]["first_instance"] = 1;
                    writer["inputs"]["source"] = {{"node", "prepare_instances"}, {"port", "destination"}};
                    writer["parameters"]["shader"]["vertex"] = "shaders/indexed_quad";
                    writer["parameters"]["ports"]["indices"] = {
                        {"element", "uint"}, {"domain", "index"}, {"access", "read"}};
                    writer["inputs"]["indices"] = {{"node", "mesh_indices"}, {"port", "resource"}};
                    writer["parameters"]["ports"]["positions"] = {
                        {"element", "vec3"}, {"domain", "vertex"}, {"access", "read"}};
                    writer["inputs"]["positions"] = {{"node", "mesh_positions"}, {"port", "resource"}};
                    fragment_input = "layout(location=0) flat in float instance_value;\n";
                    fragment_value = "instance_value";
                    document["nodes"].push_back({{"id", "instance_values"}, {"op", "render.attribute"},
                        {"parameters", {{"element", "float"}, {"domain", "instance"}, {"count", 2}}},
                        {"outputs", {"resource"}}});
                    document["nodes"].push_back({{"id", "prepare_instances"}, {"op", "render.compute"},
                        {"parameters", {{"shader", "shaders/prepare_instances"}, {"dispatch", {{"groups", {1,1,1}}}},
                            {"ports", {{"source", {{"element", "float"}, {"domain", "instance"}, {"access", "read"}}},
                                       {"destination", {{"element", "float"}, {"domain", "instance"}, {"access", "write"}}}}}}},
                        {"inputs", {{"source", {{"node", "render"}, {"port", "resource"}}},
                                    {"destination", {{"node", "instance_values"}, {"port", "resource"}}}}},
                        {"outputs", {"destination"}}});
                    writeText(root / "shaders/prepare_instances.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    pelican_store_destination(0, -100.0);
    pelican_store_destination(1, pelican_load_source(0));
})");
                    document["nodes"].push_back({{"id", "mesh_indices"}, {"op", "render.attribute"},
                        {"parameters", {{"element", "uint"}, {"domain", "index"},
                            {"source", {{"asset", "project://indices.glb"}, {"mesh", 0}, {"primitive", 0}}}}},
                        {"outputs", {"resource"}}});
                    document["nodes"].push_back({{"id", "mesh_positions"}, {"op", "render.attribute"},
                        {"parameters", {{"element", "vec3"}, {"domain", "vertex"},
                            {"source", {{"asset", "project://indices.glb"}, {"mesh", 0}, {"primitive", 0}, {"semantic", "POSITION"}}}}},
                        {"outputs", {"resource"}}});
                    writeText(root / "shaders/indexed_quad.vert", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location=0) flat out float instance_value;
void main() {
    vec3 position = pelican_load_positions();
    gl_Position = vec4(-position.x / 0.3, 1.0 - position.y, 0, 1);
    instance_value = pelican_load_source();
})");
                }
            }
            for (auto &node : document["nodes"])
                if (node["id"] == "intermediate") {
                    node["parameters"]["usage"].push_back("COLOR_ATTACHMENT");
                }
            writeText(root / "shaders/graph_image_write.frag", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location=0) out vec4 color;
)" + fragment_input + "void main() { color = vec4(" + fragment_value + "); }\n");
        }
        writeText(root / "shaders/graph_image_write.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { imageStore(pelican_resource_destination, ivec2(0), vec4(pelican_load_source(0))); }
)");
        writeText(root / "shaders/graph_image_read.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, )" + image_load + R"( * 3.0 + 1.0); }
)");
    }
    document["nodes"][0]["parameters"]["name"] = "scene_time";
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":1,"models":[]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
      {"name":"Left","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
      {"name":"Right","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}
    ]}}})");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &time = GET_MODULE(EngineTime);
    time.setTime(10);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(scene.sceneStartTime() == 10);
    const auto left = *scene.objectId("Left");
    const auto right = *scene.objectId("Right");
    for (const auto &binding : scene.runtimeObjectBindings()) {
        if (binding.object_id == left || binding.object_id == right)
            document["nodes"][binding.object_id == left ? 3 : 4]["owner"] = {
                {"scene", "default_scene"}, {"object", binding.authoring_object_id.value}};
    }
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    auto &renderer = GET_MODULE(Renderer);
    renderer.setExecutionTracingForTesting(true);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto read = [&](const char *buffer) {
        const auto bytes = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer(buffer), sizeof(float));
        float value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    };
    const auto render = [&](double now) {
        time.setTime(now);
        renderer.render();
        vkcore.waitIdle();
        return read("result");
    };
    REQUIRE(render(10.25) == 2.5f);
    REQUIRE(render(10.75) == 5.5f);
    GameContext context;
    REQUIRE_FALSE(context.setGraphParameter(right, "left/gain", 3));
    REQUIRE(context.setGraphParameter(left, "left/gain", 3));
    REQUIRE(render(10.25) == 3.25f);
    const auto other_graph = std::make_shared<const ValueGraph::Graph>(document.dump());
    REQUIRE(scene.setGraphParameter(other_graph, left, "left/gain", 9));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(scene.graphParameterValues(other_graph).at("left/gain") == 9);
    REQUIRE(context.setGraphParameter(right, "right/gain", 6));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(context.setGraphParameter(left, "left/gain", std::nullopt));
    REQUIRE(render(10.25) == 2.5f);
    REQUIRE(context.setGraphParameter(left, "left/gain", 4));
    GameObjects{}.remove(left);
    REQUIRE_FALSE(context.setGraphParameter(left, "left/gain", 5));
    const std::array<float, 1> sentinel{-17};
    const std::array<float, 1> input_sentinel{9};
    REQUIRE(context.setRenderValues("result", sentinel));
    REQUIRE(context.setRenderValues("render", input_sentinel));
    REQUIRE(render(10.25) == -17);
    REQUIRE(read("render") == 9);
    REQUIRE(read("right_result") == 5.5f);
    if (graphics_path) {
        const auto &trace = renderer.lastExecutionTraceForTesting();
        const auto &view = trace.contains("view_family") ? trace.at("view_family") : trace;
        const auto &nodes = view.at("nodes");
        const auto probe = std::find_if(nodes.begin(), nodes.end(), [](const auto &node) {
            return node.at("name") == "probe";
        });
        REQUIRE(probe != nodes.end());
        REQUIRE_FALSE(probe->at("recorded").template get<bool>());
        const auto &layouts = view.at("final_layouts");
        const auto intermediate = std::find_if(layouts.begin(), layouts.end(), [](const auto &entry) {
            return entry.at("resource") == "intermediate";
        });
        REQUIRE(intermediate != layouts.end());
        REQUIRE(intermediate->at("layout") == "undefined");
    }
    REQUIRE(scene.graphParameterValues(other_graph).empty());
    REQUIRE(context.setGraphParameter(right, "right/gain", 8));
    REQUIRE(render(10.5) == -17);
    REQUIRE(read("right_result") == 13);
    time.setTime(10.75);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(scene.sceneStartTime() == 10.75);
    REQUIRE(render(11) == 2.5f);
    const auto reloaded_left = *scene.objectId("Left");
    REQUIRE(reloaded_left != left);
    REQUIRE_FALSE(context.setGraphParameter(left, "left/gain", 3));
    REQUIRE(context.setGraphParameter(reloaded_left, "left/gain", 3));
    REQUIRE(render(11) == 3.25f);
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    auto invalid = document;
    invalid["nodes"][0]["parameters"]["name"] = "unbound_input";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    REQUIRE(render(11) == 3.25f);
    document["nodes"][3]["parameters"]["gain"] = 4;
    loadRenderGraphVariantsFromConfigData(document.dump());
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() > generation);
    REQUIRE(render(11) == 4.0f);
    auto event_document = document;
    addGraphEventClock(event_document);
    addGraphTriggerClock(document);
    loadRenderGraphVariantsFromConfigData(document.dump());
    REQUIRE(render(12) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(12) == 1);
    REQUIRE(render(12.25) == 4);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/paused", 1));
    REQUIRE(render(12.25) == 4);
    REQUIRE(render(12.75) == 4);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/paused", 0));
    REQUIRE(render(12.75) == 4);
    REQUIRE(render(13) == 7);
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(render(13) == 7);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/reset", 1));
    REQUIRE(render(13) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/reset", 0));
    REQUIRE(render(13.25) == 1); // A held trigger does not restart after reset.
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 0));
    REQUIRE(render(13.25) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(13.25) == 1);
    REQUIRE(render(13.5) == 4);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(render(14) == 1);
    loadRenderGraphVariantsFromConfigData(event_document.dump());
    const auto event_left = *scene.objectId("Left");
    const auto event_right = *scene.objectId("Right");
    using Command = Animation::CurvePlaybackCommand;
    REQUIRE_FALSE(context.emitGraphEvent(event_right, "left/commands", Command::start));
    REQUIRE_FALSE(context.emitGraphEvent(event_left, "missing", Command::start));
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    REQUIRE(render(14) == 1);
    time.setTime(14.125);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::pause));
    time.setTime(14.5);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::resume));
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(15) == 8.5);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::reset));
    REQUIRE(render(15.25) == 1);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    REQUIRE(render(17) == 1);
    REQUIRE(render(18.25) == 1); // A retired scene's queued start must not fire later.
#if PELICAN_WITH_AUDIO
    const unsigned char wav[]{'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,64,31,0,0,128,62,0,0,2,0,16,0,'d','a','t','a',8,0,0,0,0,0,0,0,0,0,0,0};
    writeText(root / "tone.wav", {reinterpret_cast<const char *>(wav), sizeof(wav)});
    event_document["definitions"]["scaled_curve"]["nodes"].push_back({
        {"id", "sound"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                    {"events", {{"node", "commands"}, {"port", "events"}}}}}, {"outputs", {"voice"}}});
    for (const auto *op : {"audio.volume", "audio.pitch"})
        event_document["definitions"]["scaled_curve"]["nodes"].push_back({
            {"id", op}, {"op", op}, {"parameters", nlohmann::json::object()},
            {"inputs", {{"value", {{"node", "gain"}, {"port", "value"}}},
                        {"voice", {{"node", "sound"}, {"port", "voice"}}}}},
            {"outputs", nlohmann::json::array()}});
    loadRenderGraphVariantsFromConfigData(event_document.dump());
    auto sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    const auto requireSound = [&](const auto &graph, const char *node) {
        const auto sound = scene.graphSound(graph, node);
        REQUIRE(sound);
        return *sound;
    };
    const auto sound_left = *scene.objectId("Left");
    const auto sound_right = *scene.objectId("Right");
    REQUIRE(context.setGraphParameter(sound_left, "left/gain", 3));
    (void)render(18.25);
    REQUIRE_FALSE(scene.graphSound(sound_graph, "left/sound"));
    auto wrong_voice = event_document;
    wrong_voice["definitions"]["scaled_curve"]["nodes"].back()["inputs"]["voice"] =
        {{"node", "gain"}, {"port", "value"}};
    REQUIRE_THROWS(ValueGraph::Graph{wrong_voice.dump()});
    auto double_target = event_document;
    double_target["definitions"]["scaled_curve"]["nodes"].back()["parameters"]["target"] = "external";
    REQUIRE_THROWS(ValueGraph::Graph{double_target.dump()});
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::start));
    REQUIRE(context.emitGraphEvent(sound_right, "right/commands", Command::start));
    (void)render(18.25);
    const auto left_voice = requireSound(sound_graph, "left/sound");
    const auto right_voice = requireSound(sound_graph, "right/sound");
    REQUIRE(left_voice != right_voice);
    REQUIRE(context.isPlaying(left_voice));
    REQUIRE(context.isPlaying(right_voice));
    REQUIRE(context.soundState(left_voice));
    REQUIRE(context.soundState(right_voice));
    REQUIRE(context.soundState(left_voice)->volume == 3.0f);
    REQUIRE(context.soundState(left_voice)->pitch == 3.0f);
    REQUIRE(context.soundState(right_voice)->volume == 4.0f);
    REQUIRE(context.soundState(right_voice)->pitch == 4.0f);
    auto attachment_document = event_document;
    auto extra_unit = attachment_document["definitions"]["scaled_curve"];
    extra_unit["nodes"].push_back({{"id", "gpu"}, {"op", "render.buffer"},
        {"parameters", {{"element", "float"}}},
        {"inputs", {{"value", {{"node", "gain"}, {"port", "value"}}}}}, {"outputs", {"resource"}}});
    attachment_document["definitions"]["extra_unit"] = extra_unit;
    auto extra = attachment_document["nodes"][4];
    extra["id"] = "extra";
    extra["definition"] = "extra_unit";
    attachment_document = editGraphInstances(attachment_document, nlohmann::json::array({extra}), nlohmann::json::array());
    const auto apply_attachment = [&](const nlohmann::json &candidate, bool fail_source) {
        const auto reference = GET_MODULE(ProjectBasicConfig).renderingConfigReference();
        const auto normalized = GET_MODULE(PathResolver).normalizedReference(reference);
        const auto path = root / "passes/main_rendering_config.json";
        const auto bytes = candidate.dump();
        return renderer.applyRenderPipelineAuthoringCandidate(RenderConfigCandidateDocumentSet{normalized,
            {{.reference = reference, .normalized_reference = normalized, .path = path,
              .operation = RenderConfigDocumentOperation::replace,
              .expected = {RenderConfigDocumentExistence::present,
                  renderConfigSourceDigest(GET_MODULE(ProjectBasicConfig).renderingConfigJson())}, .bytes = bytes}}},
            [&, bytes, path] {
                if (fail_source) throw std::runtime_error("injected graph source failure");
                writeText(path, bytes);
            });
    };
    const auto old_generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    const auto rejected_attachment = apply_attachment(attachment_document, true);
    REQUIRE_FALSE(rejected_attachment.committed);
    REQUIRE(rejected_attachment.error.find("injected graph source failure") != std::string::npos);
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == old_generation);
    REQUIRE(context.isPlaying(left_voice));
    REQUIRE(context.isPlaying(right_voice));
    const auto attached = apply_attachment(attachment_document, false);
    INFO(attached.error);
    REQUIRE(attached.committed);
    sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    REQUIRE(requireSound(sound_graph, "left/sound") == left_voice);
    REQUIRE(requireSound(sound_graph, "right/sound") == right_voice);
    REQUIRE(scene.graphParameterValues(sound_graph).at("left/gain") == 3);
    (void)render(18.25);
    REQUIRE(read("extra/gpu") == 4);
    REQUIRE(context.emitGraphEvent(sound_right, "extra/commands", Command::start));
    (void)render(18.25);
    const auto extra_voice = requireSound(sound_graph, "extra/sound");
    REQUIRE(context.isPlaying(extra_voice));
    attachment_document = editGraphInstances(attachment_document, nlohmann::json::array(), nlohmann::json::array({"extra"}));
    REQUIRE(apply_attachment(attachment_document, false).committed);
    sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    REQUIRE(requireSound(sound_graph, "left/sound") == left_voice);
    REQUIRE(requireSound(sound_graph, "right/sound") == right_voice);
    REQUIRE_THROWS(GET_MODULE(FrameGraphResourceContainer).buffer("extra/gpu"));
    REQUIRE_FALSE(context.soundState(extra_voice));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::pause));
    (void)render(18.5);
    REQUIRE(context.soundState(left_voice));
    REQUIRE(context.soundState(left_voice)->paused);
    REQUIRE(context.isPlaying(right_voice));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::resume));
    (void)render(18.75);
    REQUIRE(context.isPlaying(left_voice));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::reset));
    (void)render(19);
    REQUIRE_FALSE(context.soundState(left_voice));
    REQUIRE(context.setGraphParameter(sound_left, "left/gain", 6));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::start));
    (void)render(19.25);
    const auto restarted_voice = requireSound(sound_graph, "left/sound");
    REQUIRE(restarted_voice != left_voice);
    REQUIRE(context.soundState(restarted_voice));
    REQUIRE(context.soundState(restarted_voice)->volume == 6.0f);
    REQUIRE(context.soundState(restarted_voice)->pitch == 6.0f);
    auto bad_audio = event_document;
    for (auto &node : bad_audio["definitions"]["scaled_curve"]["nodes"])
        if (node["id"] == "sound") node["parameters"]["asset"] = "project://missing.wav";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(bad_audio.dump()));
    REQUIRE(context.isPlaying(restarted_voice));
    GameObjects{}.remove(sound_left);
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.soundState(restarted_voice));
    REQUIRE(context.isPlaying(right_voice));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.soundState(right_voice));
    auto temporary_graph = std::make_shared<const ValueGraph::Graph>(event_document.dump());
    REQUIRE(scene.applyGraphAudioEvent(temporary_graph, "right/sound", Command::start));
    const auto temporary_voice = requireSound(temporary_graph, "right/sound");
    temporary_graph.reset();
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.soundState(temporary_voice));
#endif
}

} // namespace Pelican
