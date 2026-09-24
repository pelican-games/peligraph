#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"
#include "../src/core/animation/posegraphruntime.hpp"
#include "../src/core/communication/animationstatus.hpp"
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
#include "polygoninstancecontainer_testaccess.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "renderer_testaccess.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/graphcomposition.hpp"
#include "skeletal_fixture.hpp"
#include "graph_composition_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {
void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary}; file << value;
}
void writeRenderingConfig(const std::filesystem::path &root) {
    std::filesystem::create_directories(root / "passes");
    writeText(root / "passes/main_rendering_config.json",
              R"({"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":{"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"}}})");
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
    SECTION("graph source only") {
        REQUIRE_FALSE(shared_model);
        REQUIRE_FALSE(external_buffer);
    }
    SECTION("shared model and graph source") {
        shared_model = true;
        REQUIRE_FALSE(external_buffer);
    }
    SECTION("shared model and external buffer") {
        shared_model = true;
        external_buffer = true;
        REQUIRE(shared_model);
    }
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
    writeText(root / "assets.json", nlohmann::json{{"schema", "pelican.asset_data"}, {"version", 2},
        {"models", shared_model ? nlohmann::json::array({{{"name", "character"}, {"path", source_file}}})
                                : nlohmann::json::array()}}.dump());
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
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
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.glb"}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
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
            .layout_generation = PolygonInstanceTestAccess::morphLayoutGeneration(instances, id),
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

TEST_CASE("owned graph attributes read current GPU deformation and survive owner replacement", "[skeletal][graph-deformed-attribute]") {
    setupLogger();
    bool ray_query = false;
    SECTION("ordinary graph consumers") { REQUIRE_FALSE(ray_query); }
    SECTION("graph consumers share deformation with scene AS") {
        ray_query = true;
        REQUIRE(ray_query);
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_deformed_attribute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin", {.morph_target = true});
    auto document = parameterGraphCompositionFixture();
    document["definitions"]["geometry"] = nlohmann::json::parse(R"({
      "nodes":[
        {"id":"position","op":"render.attribute","parameters":{"element":"vec3","domain":"vertex",
          "source":{"asset":"project://character.gltf","mesh":0,"primitive":0,"semantic":"POSITION","deformed_node":0}},"outputs":["resource"]},
        {"id":"normal","op":"render.attribute","parameters":{"element":"vec3","domain":"vertex",
          "source":{"asset":"project://character.gltf","mesh":0,"primitive":0,"semantic":"NORMAL","deformed_node":0}},"outputs":["resource"]}],
      "outputs":{"position":{"node":"position","port":"resource"},"normal":{"node":"normal","port":"resource"}}})");
    document["nodes"][5] = {{"id", "render"}, {"op", "graph.instance"}, {"definition", "geometry"},
        {"owner", {{"scene", "default_scene"}, {"object", 1}}}};
    document["nodes"][6]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][7];
    probe["parameters"]["ports"]["source"] = {{"element", "vec3"}, {"domain", "vertex"}, {"access", "read"}};
    probe["parameters"]["ports"]["normal"] = probe["parameters"]["ports"]["source"];
    probe["inputs"]["source"] = {{"node", "render"}, {"port", "position"}};
    probe["inputs"]["normal"] = {{"node", "render"}, {"port", "normal"}};
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    if (ray_query)
        document["rendering"]["target_planning"]["graphs"]["main_render"]["required_capabilities"] =
            nlohmann::json::array({"pelican.vulkan.ray_query@1"});
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    pelican_store_destination(0, pelican_load_source(0).x);
    pelican_store_destination(1, pelican_load_source(1).x);
    pelican_store_destination(2, pelican_load_normal(0).y);
    pelican_store_destination(3, pelican_load_normal(0).z);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.gltf"}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
      {"name":"Character","components":[{"name":"transform","pos":[10,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
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
        if (binding.object_id == *object) document["nodes"][5]["owner"]["object"] = binding.authoring_object_id.value;
    writeText(root / "passes/main_rendering_config.json", document.dump());
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
            .layout_generation = PolygonInstanceTestAccess::morphLayoutGeneration(instances, id),
            .frame_revision = revision, .weights = &value, .weight_count = 1}) == Animation::Status::ok);
    };
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };
    const auto expect = [&](std::array<float, 4> expected) {
        const auto actual = render();
        for (std::size_t i = 0; i < actual.size(); ++i) {
            INFO("GPU deformation result " << i);
            REQUIRE(std::abs(actual[i] - expected[i]) < 0.00001f);
        }
    };
    weight(initial, 0.5f, 1);
    expect({0.7f, 1.3f, 0.0f, 1.0f});
    if (ray_query) {
        const auto diagnostics = RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        REQUIRE(diagnostics.requested);
        REQUIRE(diagnostics.active_blas_count > 0);
    }
    weight(initial, 0.75f, 2);
    expect({1.2f, 1.8f, 0.0f, 1.0f});
    const auto skin = glm::translate(glm::mat4{1}, glm::vec3{1, 0, 0}) *
        glm::rotate(glm::mat4{1}, glm::radians(90.0f), glm::vec3{1, 0, 0});
    const std::array palette{skin, skin};
    instances.setSkinningPalette(initial, palette);
    expect({2.2f, 2.8f, -1.0f, 0.0f});
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    const auto active_programs = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(active_programs);
    REQUIRE_FALSE(active_programs->programs.empty());
    auto invalid_count = active_programs->programs.begin()->second.frame_graph.deformed_attributes;
    REQUIRE_FALSE(invalid_count.empty());
    REQUIRE(invalid_count.front().expected_count > 0);
    --invalid_count.front().expected_count;
    REQUIRE_THROWS_WITH(validateGraphDeformedAttributes(invalid_count),
        Catch::Matchers::ContainsSubstring("count"));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    auto invalid = document;
    invalid["definitions"]["geometry"]["nodes"][0]["parameters"]["source"]["deformed_node"] = 99;
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    expect({2.2f, 2.8f, -1.0f, 0.0f});
    GameObjects{}.remove(*object);
    expect({2.2f, 2.8f, -1.0f, 0.0f});
    test_support::loadScene(scene, "default_scene");
    GET_MODULE(ECSCore).update();
    const auto reloaded = scene.objectId("Character");
    REQUIRE(reloaded);
    const auto replacement = instance_for(*reloaded);
    REQUIRE(replacement != initial);
    expect({-0.3f, 0.3f, 0.0f, 1.0f});
    weight(replacement, 2.0f, 1);
    expect({3.7f, 4.3f, 0.0f, 1.0f});
    auto &reload = GET_MODULE(watch::ReloadService);
    auto &models = GET_MODULE(ModelAssetContainer);
    const auto revision = models.contentRevisionForTesting("character");
    const watch::ReloadRequest changed{.key = watch::makeAssetKey("character.gltf")};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin", {.position_scale = 2, .morph_target = true});
    REQUIRE(reload.applyRequestForTesting(changed));
    REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    const auto current_instance = instance_for(*reloaded);
    const std::array identity_palette{glm::mat4{1}, glm::mat4{1}};
    instances.setSkinningPalette(current_instance, identity_palette);
    weight(current_instance, 0.5f, 2);
    expect({0.4f, 1.6f, 0.0f, 1.0f});
    const auto before_failed_reload = GET_MODULE(RenderingPassContainer).snapshot();
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin", {.position_scale = 3, .morph_target = true});
    {
        watch::ScopedRenderPipelineCompanionReloadObserver fail_after_model_prepare{
            [](auto) { throw std::runtime_error("reject deformed attribute companion"); }};
        REQUIRE_FALSE(reload.applyRequestForTesting(changed));
    }
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == before_failed_reload);
    REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    expect({0.4f, 1.6f, 0.0f, 1.0f});
    // A valid source node can disappear from the model's selected scene.
    // Reject the model candidate before publishing broken graph references.
    auto detached_mesh = nlohmann::json::parse(std::ifstream{root / "character.gltf"});
    detached_mesh["scenes"][0]["nodes"] = nlohmann::json::array({1});
    writeText(root / "character.gltf", detached_mesh.dump());
    REQUIRE_FALSE(reload.applyRequestForTesting(changed));
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == before_failed_reload);
    REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    expect({0.4f, 1.6f, 0.0f, 1.0f});
    REQUIRE(nlohmann::json::parse(std::ifstream{root / "passes/main_rendering_config.json"}) == document);
    const auto final_generation = GET_MODULE(RenderingPassContainer).snapshot();
    REQUIRE(final_generation);
    REQUIRE_FALSE(final_generation->programs.empty());
    REQUIRE(final_generation->programs.begin()->second.frame_graph.value_graph);
    REQUIRE(nlohmann::json::parse(final_generation->programs.begin()->second.frame_graph.value_graph->serialize()) == document);
}

TEST_CASE("owned graph pose attributes expose evaluated model-space nodes", "[skeletal][graph-pose-attribute]") {
    setupLogger();
    bool animation_graph = false;
    SECTION("animation component clip") { REQUIRE_FALSE(animation_graph); }
    SECTION("animation graph phase publication") {
        animation_graph = true;
        REQUIRE(animation_graph);
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_pose_attribute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin");
    auto document = parameterGraphCompositionFixture();
    document["definitions"]["pose"] = nlohmann::json::parse(R"({
      "nodes":[
        {"id":"tip","op":"render.attribute","parameters":{"element":"mat4","domain":"instance",
          "source":{"asset":"project://character.gltf","pose_node":2}},"outputs":["resource"]},
        {"id":"model","op":"render.attribute","parameters":{"element":"mat4","domain":"instance",
          "source":{"asset":"project://character.gltf","pose_node":0}},"outputs":["resource"]}],
      "outputs":{"tip":{"node":"tip","port":"resource"},"model":{"node":"model","port":"resource"}}})");
    document["nodes"][5] = {{"id", "render"}, {"op", "graph.instance"}, {"definition", "pose"},
        {"owner", {{"scene", "default_scene"}, {"object", 1}}}};
    document["nodes"][6]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][7];
    probe["parameters"]["ports"].erase("source");
    probe["inputs"].erase("source");
    for (const auto *port : {"tip", "model"}) {
        probe["parameters"]["ports"][port] = {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}};
        probe["inputs"][port] = {{"node", "render"}, {"port", port}};
    }
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 tip = pelican_load_tip(0);
    pelican_store_destination(0, tip[3].x);
    pelican_store_destination(1, tip[3].y);
    pelican_store_destination(2, tip[0].y);
    pelican_store_destination(3, pelican_load_model(0)[0].x);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.gltf"}]})");
    writeText(root / "turn.json", R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{},"initial_state":"turn","states":[{"name":"turn","type":"clip","clip":"Turn","loop":false}],"transitions":[]})");
    auto scene_document = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
      {"name":"Character","components":[{"name":"transform","pos":[10,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]}
    ]}}})");
    scene_document["scenes"]["default_scene"]["objects"][0]["components"].push_back(animation_graph
        ? nlohmann::json{{"name", "animation"}, {"graph", "turn.json"}}
        : nlohmann::json{{"name", "animation"}, {"clip", "character.gltf#animation/Turn"}, {"loop", false}});
    writeText(root / "scene.json", scene_document.dump());
    auto &service = Animation::animationServiceRuntime();
    service.reset();
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
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance_for = [&](GameObjectId id) {
        const auto *model = ecs.tryComponent<SimpleModelViewComponent>(id);
        REQUIRE(model);
        REQUIRE(model->model_instance_id);
        return *model->model_instance_id;
    };
    const auto object = scene.objectId("Character");
    REQUIRE(object);
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object) document["nodes"][5]["owner"]["object"] = binding.authoring_object_id.value;
    writeText(root / "passes/main_rendering_config.json", document.dump());
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    const auto animate = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    animate();
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };
    const auto verify_pose = [&](GameObjectId owner) {
        const auto id = instance_for(owner);
        const auto &palette = PolygonInstanceTestAccess::currentSkinPalette(instances, id);
        REQUIRE(palette.size() == 2);
        const auto tip = instances.animationNodeMatrix(id, 2);
        REQUIRE(tip);
        // TipJoint's inverse bind removes its y=1 rest translation from the
        // palette. The graph must expose that hierarchy translation instead.
        const auto expected = palette[0] * glm::vec4{0, 1, 0, 1};
        REQUIRE(std::abs((*tip)[3].x - expected.x) < 0.00001f);
        REQUIRE(std::abs((*tip)[3].y - expected.y) < 0.00001f);
        REQUIRE_FALSE(instances.animationNodeMatrix(id, 99));
        const auto values = render();
        const std::array expected_values{expected.x, expected.y, palette[0][0].y, 1.0f};
        for (std::size_t i = 0; i < values.size(); ++i)
            REQUIRE(std::abs(values[i] - expected_values[i]) < 0.00001f);
        return values;
    };
    animate();
    const auto initial = instance_for(*object);
    const auto first = verify_pose(*object);
    animate();
    const auto second = verify_pose(*object);
    REQUIRE(first != second);
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    auto invalid = document;
    invalid["definitions"]["pose"]["nodes"][0]["parameters"]["source"]["pose_node"] = 99;
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    REQUIRE(render() == second);
    GameObjects{}.remove(*object);
    REQUIRE_FALSE(instances.animationNodeMatrix(initial, 2));
    REQUIRE(render() == second);
    test_support::loadScene(scene, "default_scene");
    animate();
    const auto replacement = scene.objectId("Character");
    REQUIRE(replacement);
    REQUIRE(instance_for(*replacement) != initial);
    verify_pose(*replacement);
    auto &reload = GET_MODULE(watch::ReloadService);
    auto &models = GET_MODULE(ModelAssetContainer);
    const auto revision = models.contentRevisionForTesting("character");
    const watch::ReloadRequest changed{.key = watch::makeAssetKey("character.gltf")};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin", {.position_scale = 2});
    REQUIRE(reload.applyRequestForTesting(changed));
    REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    animate();
    const auto after_reload = verify_pose(*replacement);
    const auto before_failure = GET_MODULE(RenderingPassContainer).snapshot();
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin", {.position_scale = 3});
    {
        watch::ScopedRenderPipelineCompanionReloadObserver fail_after_model_prepare{
            [](auto) { throw std::runtime_error("reject pose attribute companion"); }};
        REQUIRE_FALSE(reload.applyRequestForTesting(changed));
    }
    REQUIRE(models.contentRevisionForTesting("character") == revision + 1);
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == before_failure);
    REQUIRE(render() == after_reload);
    // Palette-only publications cannot describe arbitrary hierarchy nodes.
    // Reject attachment before publishing a graph with unavailable pose data.
    const auto current = instance_for(*replacement);
    const auto palette_only = PolygonInstanceTestAccess::currentSkinPalette(instances, current);
    instances.setSkinningPalette(current, palette_only);
    REQUIRE_FALSE(instances.animationNodeMatrix(current, 2));
    REQUIRE_THROWS_WITH(loadRenderGraphVariantsFromConfigData(document.dump()),
        Catch::Matchers::ContainsSubstring("no published node matrix"));
    REQUIRE(GET_MODULE(RenderingPassContainer).snapshot() == before_failure);
    animate();
    verify_pose(*replacement);
    REQUIRE(nlohmann::json::parse(std::ifstream{root / "passes/main_rendering_config.json"}) == document);
    service.reset();
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
              R"json({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.glb"}]})json");
    writeText(root / "scene.json", R"json({
      "schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[{
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
    const auto instance = PolygonInstanceTestAccess::modelInstanceId(instances, 0);
    const auto first_revision = PolygonInstanceTestAccess::currentAnimationRevision(instances, instance);
    REQUIRE(first_revision != 0);
    REQUIRE(PolygonInstanceTestAccess::previousAnimationRevision(instances, instance) == first_revision);
    instances.advanceTemporalHistoryAfterRender();
    time.setTime(0.75);
    time.advance();
    GET_MODULE(ECSCore).update();
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, instance) > first_revision);
    REQUIRE(PolygonInstanceTestAccess::previousAnimationRevision(instances, instance) == first_revision);
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
              R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writeText(root / "scene.json", R"json({"schema":"pelican.scene","version":2,
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
              R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.glb"}]})");
    writeRenderingConfig(root);
    writeText(root / "turn.json", R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0},"initial_state":"turn",
      "states":[{"name":"turn","type":"clip","clip":"Turn","loop":false},
                {"name":"step","type":"clip","clip":"Step","loop":false}],
      "transitions":[{"from":"turn","to":"step","duration":0,
                      "conditions":[{"parameter":"go","op":">","value":0}]}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,
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
    const auto first_palette = PolygonInstanceTestAccess::currentSkinPalette(instances, instance);
    REQUIRE(first_palette.size() == 2);
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, instance) == time.frameIndex());
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentSkinPalette(instances, instance) != first_palette);
    auto unnamed = invalidEntityId;
    for (const auto entity : ecs.liveEntities()) {
        if (entity != *named && ecs.tryComponent<AnimationComponent>(entity)) unnamed = entity;
    }
    REQUIRE(unnamed != invalidEntityId);
    REQUIRE_FALSE(scene.objectName(unnamed));
    const auto unnamed_instance = *ecs.tryComponent<SimpleModelViewComponent>(unnamed)->model_instance_id;
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, unnamed_instance) == time.frameIndex());
    // Shared graph, different authored initial values: only the named actor turns.
    REQUIRE(std::abs(PolygonInstanceTestAccess::currentSkinPalette(instances, unnamed_instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(PolygonInstanceTestAccess::currentSkinPalette(instances, instance)[0][0][1]) > 0.1f);

    GameContext context;
    const auto turn_palette = PolygonInstanceTestAccess::currentSkinPalette(instances, instance);
    REQUIRE(std::abs(turn_palette[0][0][1]) > 0.1f);
    REQUIRE(context.animation().setParameter(*named, "go", 1) == Animation::Status::ok);
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentSkinPalette(instances, instance) != turn_palette);
    // Step translates the tip only; a continuing Turn would still rotate the root.
    REQUIRE(std::abs(PolygonInstanceTestAccess::currentSkinPalette(instances, instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(PolygonInstanceTestAccess::currentSkinPalette(instances, instance)[0][0][0] - 1.0f) < 0.0001f);
    REQUIRE(context.animation().setParameter(*named, "go", 0) == Animation::Status::ok);
    const auto step_palette = PolygonInstanceTestAccess::currentSkinPalette(instances, instance);
    REQUIRE(context.animation().forceState(*named, "turn") == Animation::Status::ok);
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentSkinPalette(instances, instance) != step_palette);
    REQUIRE(std::abs(PolygonInstanceTestAccess::currentSkinPalette(instances, instance)[0][0][1]) > 0.1f);

    // Switching to a clip removes the graph writer before phase evaluation.
    auto *animation = ecs.tryComponent<AnimationComponent>(*named);
    animation->graph.clear();
    animation->clip = "character.glb#animation/Turn";
    REQUIRE(context.animation().setParameter(*named, "go", 1) == Animation::Status::not_found);
    REQUIRE(context.animation().forceState(*named, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, instance) == time.frameIndex());
    REQUIRE(context.animation().forceState(*named, "turn") == Animation::Status::not_found);
    animation->clip.clear();
    animation->graph = "turn.json";
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, instance) == time.frameIndex());
    // Removing the unnamed actor retires both its renderer slot and writer.
    REQUIRE(ecs.remove(unnamed));
    REQUIRE(context.animation().forceState(unnamed, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE_THROWS(PolygonInstanceTestAccess::currentAnimationRevision(instances, unnamed_instance));
    REQUIRE(context.animation().forceState(unnamed, "turn") == Animation::Status::not_found);

    test_support::loadScene(scene, "default_scene");
    frame();
    const auto reloaded = scene.objectId("Character");
    REQUIRE(reloaded);
    REQUIRE(*reloaded != *named);
    const auto new_instance = *ecs.tryComponent<SimpleModelViewComponent>(*reloaded)->model_instance_id;
    REQUIRE(new_instance != instance);
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, new_instance) == time.frameIndex());
    const auto reloaded_palette = PolygonInstanceTestAccess::currentSkinPalette(instances, new_instance);
    frame();
    REQUIRE(PolygonInstanceTestAccess::currentSkinPalette(instances, new_instance) != reloaded_palette);
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
    REQUIRE_FALSE(context.render().setRenderValues("missing", values));
    REQUIRE_THROWS(context.render().setRenderValues("curve_values", std::span{values}.first(1)));
    REQUIRE(context.render().setRenderValues("curve_values", values));
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
        GET_MODULE(ShaderLibrary), GET_MODULE(PathResolver)});
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
    SECTION("buffer compute path") {
        REQUIRE(document["outputs"]["gpu"]["node"].get<std::string>() == "probe");
        REQUIRE_FALSE(graphics_path);
    }
    SECTION("image compute path") {
        addGraphImageStage(document);
        std::string image_load = "imageLoad(pelican_resource_source, ivec2(0)).x";
        SECTION("storage image read") {
            const auto &source = document["nodes"].back()["parameters"]["ports"]["source"];
            REQUIRE(source["kind"].get<std::string>() == "image");
            REQUIRE_FALSE(source.contains("binding"));
        }
        SECTION("sampled image read") {
            document["nodes"].back()["parameters"]["ports"]["source"]["binding"] = "sampled";
            image_load = "texelFetch(pelican_resource_source, ivec2(0), 0).x";
            REQUIRE(document["nodes"].back()["parameters"]["ports"]["source"]["binding"]
                        .get<std::string>() == "sampled");
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
            SECTION("fullscreen") { REQUIRE(writer["op"].get<std::string>() == "render.fullscreen"); }
            SECTION("raster") {
                writer["op"] = "render.raster";
                writer["parameters"]["draw"] = {{"vertex_count", 3}};
                writer["parameters"]["draw"]["instance_source"] = "source";
                SECTION("direct") {
                    REQUIRE(writer["op"].get<std::string>() == "render.raster");
                    REQUIRE(writer["parameters"]["draw"]["vertex_count"].get<int>() == 3);
                }
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
                    // Assert before the pushes below, which can reseat the `writer` reference.
                    REQUIRE_FALSE(writer["parameters"]["draw"].contains("vertex_count"));
                    REQUIRE(writer["parameters"]["draw"]["index_source"].get<std::string>() == "indices");
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
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
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
    const auto install_graph = [&](const nlohmann::json &candidate) {
        loadRenderGraphVariantsFromConfigData(candidate.dump());
        GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(candidate.dump());
    };
    RendererTestAccess::setExecutionTracing(renderer, true);
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
    REQUIRE_FALSE(context.render().setValueGraphParameter(right, "left/gain", 3));
    REQUIRE(context.render().setValueGraphParameter(left, "left/gain", 3));
    // An edge supplies left/time, so the expansion left no parameter for an
    // override to land on. That is said by name rather than through the same
    // false an unknown parameter returns -- the override does not go quiet.
    REQUIRE_THROWS_AS(context.render().setValueGraphParameter(left, "left/time", 1), Pelican::GraphRegistryError);
    // Ownership still decides first: another object gets the plain refusal.
    REQUIRE_FALSE(context.render().setValueGraphParameter(right, "left/time", 1));
    REQUIRE(render(10.25) == 3.25f);
    const auto other_graph = std::make_shared<const ValueGraph::Graph>(document.dump());
    REQUIRE(scene.setGraphParameter(other_graph, left, "left/gain", 9));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(scene.graphParameterValues(other_graph).at("left/gain") == 9);
    REQUIRE(context.render().setValueGraphParameter(right, "right/gain", 6));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(context.render().setValueGraphParameter(left, "left/gain", std::nullopt));
    REQUIRE(render(10.25) == 2.5f);
    REQUIRE(context.render().setValueGraphParameter(left, "left/gain", 4));
    auto held_generation = GET_MODULE(RenderingPassContainer).snapshot();
    auto &graph_buffers = GET_MODULE(FrameGraphResourceContainer);
    const auto retired_input = graph_buffers.getBufferIdByName("render");
    const auto source_before_removal = GET_MODULE(ProjectBasicConfig).renderingConfigJson();
    GameObjects{}.remove(left);
    REQUIRE_FALSE(context.render().setValueGraphParameter(left, "left/gain", 5));
    const std::array<float, 1> sentinel{-17};
    const std::array<float, 1> input_sentinel{9};
    REQUIRE(context.render().setRenderValues("result", sentinel));
    REQUIRE(context.render().setRenderValues("render", input_sentinel));
    REQUIRE(render(10.25) == -17);
    REQUIRE_FALSE(graph_buffers.hasBuffer("render"));
    REQUIRE(graph_buffers.hasBuffer(retired_input)); // The old frame still owns its exact GPU record.
    REQUIRE(GET_MODULE(ProjectBasicConfig).renderingConfigJson() == source_before_removal);
    REQUIRE(read("right_result") == 5.5f);
    held_generation.reset();
    // Cycle the in-flight slots so their old generation leases retire too.
    for (int frame = 0; frame < 3; ++frame) (void)render(10.25);
    GET_MODULE(DeletionQueue).flushAll();
    REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 0);
    REQUIRE_FALSE(graph_buffers.hasBuffer(retired_input));
    const auto reclaimed_generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    if (graphics_path) {
        const auto &trace = RendererTestAccess::lastExecutionTrace(renderer);
        const auto &view = trace.contains("view_family") ? trace.at("view_family") : trace;
        const auto &nodes = view.at("nodes");
        const auto probe = std::find_if(nodes.begin(), nodes.end(), [](const auto &node) {
            return node.at("name") == "probe";
        });
        REQUIRE(probe == nodes.end());
        const auto &layouts = view.at("final_layouts");
        const auto intermediate = std::find_if(layouts.begin(), layouts.end(), [](const auto &entry) {
            return entry.at("resource") == "intermediate";
        });
        REQUIRE(intermediate == layouts.end());
    }
    REQUIRE(scene.graphParameterValues(other_graph).empty());
    REQUIRE(context.render().setValueGraphParameter(right, "right/gain", 8));
    REQUIRE(render(10.5) == -17);
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == reclaimed_generation);
    REQUIRE(read("right_result") == 13);
    time.setTime(10.75);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(scene.sceneStartTime() == 10.75);
    REQUIRE(render(11) == 2.5f);
    REQUIRE(graph_buffers.hasBuffer("render"));
    REQUIRE(graph_buffers.getBufferIdByName("render") != retired_input);
    REQUIRE(GET_MODULE(ProjectBasicConfig).renderingConfigJson() == source_before_removal);
    const auto reloaded_left = *scene.objectId("Left");
    REQUIRE(reloaded_left != left);
    REQUIRE_FALSE(context.render().setValueGraphParameter(left, "left/gain", 3));
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/gain", 3));
    REQUIRE(render(11) == 3.25f);
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    auto invalid = document;
    invalid["nodes"][0]["parameters"]["name"] = "unbound_input";
    REQUIRE_THROWS(install_graph(invalid));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    REQUIRE(render(11) == 3.25f);
    document["nodes"][3]["parameters"]["gain"] = 4;
    install_graph(document);
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() > generation);
    REQUIRE(render(11) == 4.0f);
    auto event_document = document;
    addGraphEventClock(event_document);
    addGraphTriggerClock(document);
    install_graph(document);
    REQUIRE(render(12) == 1);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(12) == 1);
    REQUIRE(render(12.25) == 4);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/paused", 1));
    REQUIRE(render(12.25) == 4);
    REQUIRE(render(12.75) == 4);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/paused", 0));
    REQUIRE(render(12.75) == 4);
    REQUIRE(render(13) == 7);
    REQUIRE_THROWS(install_graph(invalid));
    REQUIRE(render(13) == 7);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/reset", 1));
    REQUIRE(render(13) == 1);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/reset", 0));
    REQUIRE(render(13.25) == 1); // A held trigger does not restart after reset.
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/fire", 0));
    REQUIRE(render(13.25) == 1);
    REQUIRE(context.render().setValueGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(13.25) == 1);
    REQUIRE(render(13.5) == 4);
    const auto before_replacement = graph_buffers.getBufferIdByName("render");
    test_support::loadScene(scene, "default_scene");
    REQUIRE(render(14) == 1);
    REQUIRE(graph_buffers.getBufferIdByName("render") != before_replacement);
    install_graph(event_document);
    const auto event_left = *scene.objectId("Left");
    const auto event_right = *scene.objectId("Right");
    using Command = Animation::CurvePlaybackCommand;
    REQUIRE_FALSE(context.render().emitValueGraphEvent(event_right, "left/commands", Command::start));
    REQUIRE_FALSE(context.render().emitValueGraphEvent(event_left, "missing", Command::start));
    REQUIRE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::start));
    REQUIRE(render(14) == 1);
    time.setTime(14.125);
    REQUIRE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::pause));
    time.setTime(14.5);
    REQUIRE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::resume));
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(15) == 8.5);
    REQUIRE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::reset));
    REQUIRE(render(15.25) == 1);
    REQUIRE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::start));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.render().emitValueGraphEvent(event_left, "left/commands", Command::start));
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
    install_graph(event_document);
    auto sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    const auto requireSound = [&](const auto &graph, const char *node) {
        const auto sound = scene.graphSound(graph, node);
        REQUIRE(sound);
        return *sound;
    };
    const auto sound_left = *scene.objectId("Left");
    const auto sound_right = *scene.objectId("Right");
    REQUIRE(context.render().setValueGraphParameter(sound_left, "left/gain", 3));
    (void)render(18.25);
    REQUIRE_FALSE(scene.graphSound(sound_graph, "left/sound"));
    auto wrong_voice = event_document;
    wrong_voice["definitions"]["scaled_curve"]["nodes"].back()["inputs"]["voice"] =
        {{"node", "gain"}, {"port", "value"}};
    REQUIRE_THROWS(ValueGraph::Graph{wrong_voice.dump()});
    auto double_target = event_document;
    double_target["definitions"]["scaled_curve"]["nodes"].back()["parameters"]["target"] = "external";
    REQUIRE_THROWS(ValueGraph::Graph{double_target.dump()});
    REQUIRE(context.render().emitValueGraphEvent(sound_left, "left/commands", Command::start));
    REQUIRE(context.render().emitValueGraphEvent(sound_right, "right/commands", Command::start));
    (void)render(18.25);
    const auto left_voice = requireSound(sound_graph, "left/sound");
    const auto right_voice = requireSound(sound_graph, "right/sound");
    REQUIRE(left_voice != right_voice);
    REQUIRE(context.audio().isPlaying(left_voice));
    REQUIRE(context.audio().isPlaying(right_voice));
    REQUIRE(context.audio().soundState(left_voice));
    REQUIRE(context.audio().soundState(right_voice));
    REQUIRE(context.audio().soundState(left_voice)->volume == 3.0f);
    REQUIRE(context.audio().soundState(left_voice)->pitch == 3.0f);
    REQUIRE(context.audio().soundState(right_voice)->volume == 4.0f);
    REQUIRE(context.audio().soundState(right_voice)->pitch == 4.0f);
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
    REQUIRE(context.audio().isPlaying(left_voice));
    REQUIRE(context.audio().isPlaying(right_voice));
    const auto attached = apply_attachment(attachment_document, false);
    INFO(attached.error);
    REQUIRE(attached.committed);
    sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    REQUIRE(requireSound(sound_graph, "left/sound") == left_voice);
    REQUIRE(requireSound(sound_graph, "right/sound") == right_voice);
    REQUIRE(scene.graphParameterValues(sound_graph).at("left/gain") == 3);
    (void)render(18.25);
    REQUIRE(read("extra/gpu") == 4);
    REQUIRE(context.render().emitValueGraphEvent(sound_right, "extra/commands", Command::start));
    (void)render(18.25);
    const auto extra_voice = requireSound(sound_graph, "extra/sound");
    REQUIRE(context.audio().isPlaying(extra_voice));
    attachment_document = editGraphInstances(attachment_document, nlohmann::json::array(), nlohmann::json::array({"extra"}));
    REQUIRE(apply_attachment(attachment_document, false).committed);
    sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    REQUIRE(requireSound(sound_graph, "left/sound") == left_voice);
    REQUIRE(requireSound(sound_graph, "right/sound") == right_voice);
    REQUIRE_THROWS(GET_MODULE(FrameGraphResourceContainer).buffer("extra/gpu"));
    REQUIRE_FALSE(context.audio().soundState(extra_voice));
    REQUIRE(context.render().emitValueGraphEvent(sound_left, "left/commands", Command::pause));
    (void)render(18.5);
    REQUIRE(context.audio().soundState(left_voice));
    REQUIRE(context.audio().soundState(left_voice)->paused);
    REQUIRE(context.audio().isPlaying(right_voice));
    REQUIRE(context.render().emitValueGraphEvent(sound_left, "left/commands", Command::resume));
    (void)render(18.75);
    REQUIRE(context.audio().isPlaying(left_voice));
    REQUIRE(context.render().emitValueGraphEvent(sound_left, "left/commands", Command::reset));
    (void)render(19);
    REQUIRE_FALSE(context.audio().soundState(left_voice));
    REQUIRE(context.render().setValueGraphParameter(sound_left, "left/gain", 6));
    REQUIRE(context.render().emitValueGraphEvent(sound_left, "left/commands", Command::start));
    (void)render(19.25);
    const auto restarted_voice = requireSound(sound_graph, "left/sound");
    REQUIRE(restarted_voice != left_voice);
    REQUIRE(context.audio().soundState(restarted_voice));
    REQUIRE(context.audio().soundState(restarted_voice)->volume == 6.0f);
    REQUIRE(context.audio().soundState(restarted_voice)->pitch == 6.0f);
    auto bad_audio = event_document;
    for (auto &node : bad_audio["definitions"]["scaled_curve"]["nodes"])
        if (node["id"] == "sound") node["parameters"]["asset"] = "project://missing.wav";
    REQUIRE_THROWS(install_graph(bad_audio));
    REQUIRE(context.audio().isPlaying(restarted_voice));
    GameObjects{}.remove(sound_left);
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.audio().soundState(restarted_voice));
    REQUIRE(context.audio().isPlaying(right_voice));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.audio().soundState(right_voice));
    auto temporary_graph = std::make_shared<const ValueGraph::Graph>(event_document.dump());
    ValueGraph::AudioVoiceDirective started;
    started.phase = ValueGraph::AudioVoiceDirective::Phase::playing;
    started.epoch = 1;
    REQUIRE(scene.applyGraphVoice(temporary_graph, "right/sound", started));
    const auto temporary_voice = requireSound(temporary_graph, "right/sound");
    temporary_graph.reset();
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.audio().soundState(temporary_voice));
#endif
}

// AG-1a: the pose itself travels on a common-graph port. The object carries no
// animation component here; a pose.clip_sample or pose.state_machine node
// inside an owned graph instance is the only writer of its skeletal sink, and
// the evaluated pose is read back through the GPU pose attribute.
TEST_CASE("a pose graph drives an object's skeletal sink", "[skeletal][graph-pose-sink]") {
    setupLogger();
    // Generated rather than sectioned: every case below runs the same body, so
    // no case can end up as a section that asserts nothing.
    const auto mode = GENERATE(as<std::string>{}, "clip sample", "state machine",
                               "state machine without the parameter edge",
                               "animation component already writes the object");
    CAPTURE(mode);
    const bool state_machine = mode.starts_with("state machine");
    const bool connect_parameter = mode != "state machine without the parameter edge";
    const bool component_writer = mode == "animation component already writes the object";
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_pose_sink_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin");
    auto document = parameterGraphCompositionFixture();
    auto nodes = nlohmann::json::parse(R"([
      {"id":"time","op":"graph.input"},
      {"id":"tip","op":"render.attribute","parameters":{"element":"mat4","domain":"instance",
        "source":{"asset":"project://character.gltf","pose_node":2}},"outputs":["resource"]},
      {"id":"model","op":"render.attribute","parameters":{"element":"mat4","domain":"instance",
        "source":{"asset":"project://character.gltf","pose_node":0}},"outputs":["resource"]},
      {"id":"out","op":"pose.skeletal","inputs":{"pose":{"node":"source","port":"pose"}},
       "outputs":[]}])");
    nlohmann::json instance_inputs{{"time", {{"node", "clock"}, {"port", "value"}}}};
    if (state_machine) {
        nodes.push_back(nlohmann::json::parse(R"({"id":"speed","op":"graph.input","default":0})"));
        nodes.push_back(nlohmann::json{{"id", "source"}, {"op", "pose.state_machine"},
            {"parameters", {{"rig", "project://character.gltf"},
                {"graph", nlohmann::json::parse(R"({"schema":"pelican.anim_graph","version":1,
                  "parameters":{"speed":0},
                  "states":[{"name":"idle","type":"clip","clip":"Turn","loop":true},
                            {"name":"walk","type":"clip","clip":"Step","loop":true}],
                  "transitions":[{"from":"idle","to":"walk","duration":0.3,
                    "conditions":[{"parameter":"speed","op":">","value":0.5}]}],
                  "initial_state":"idle"})")}}},
            {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                        {"speed", {{"node", "speed"}, {"port", "value"}}}}},
            {"outputs", {"pose"}}});
        // The value side of the same document decides when the state machine
        // leaves `idle`; today only GameContext could do that.
        document["nodes"].insert(document["nodes"].begin() + 3,
            nlohmann::json::parse(R"({"id":"gate","op":"value.curve",
              "inputs":{"time":{"node":"clock","port":"value"}},
              "parameters":{"keys":[{"time":0,"value":0},{"time":0.4,"value":0},
                                    {"time":0.6,"value":1},{"time":40,"value":1}]},
              "outputs":["value"]})"));
        if (connect_parameter)
            instance_inputs["speed"] = {{"node", "gate"}, {"port", "value"}};
        else
            nodes.back()["inputs"].erase("speed");
    } else {
        nodes.push_back(nlohmann::json::parse(R"({"id":"source","op":"pose.clip_sample",
          "parameters":{"rig":"project://character.gltf","clip":"Turn"},
          "inputs":{"time":{"node":"time","port":"value"}},"outputs":["pose"]})"));
    }
    document["definitions"]["pose"] = {{"nodes", std::move(nodes)},
        {"outputs", {{"tip", {{"node", "tip"}, {"port", "resource"}}},
                     {"model", {{"node", "model"}, {"port", "resource"}}}}}};
    const auto instance_index = state_machine ? 6 : 5;
    document["nodes"][instance_index] = {{"id", "render"}, {"op", "graph.instance"},
        {"definition", "pose"}, {"owner", {{"scene", "default_scene"}, {"object", 1}}},
        {"inputs", instance_inputs}};
    document["nodes"][instance_index + 1]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][instance_index + 2];
    probe["parameters"]["ports"].erase("source");
    probe["inputs"].erase("source");
    for (const auto *port : {"tip", "model"}) {
        probe["parameters"]["ports"][port] = {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}};
        probe["inputs"][port] = {{"node", "render"}, {"port", port}};
    }
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 tip = pelican_load_tip(0);
    pelican_store_destination(0, tip[3].x);
    pelican_store_destination(1, tip[3].y);
    pelican_store_destination(2, tip[0].y);
    pelican_store_destination(3, pelican_load_model(0)[0].x);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.gltf"}]})");
    // No animation component: the graph is the only thing that can pose this
    // object, so anything the attribute shows came through the pose port.
    auto scene_document = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
      {"name":"Character","components":[{"name":"transform","pos":[10,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]}]}}})");
    if (component_writer)
        scene_document["scenes"]["default_scene"]["objects"][0]["components"].push_back(
            nlohmann::json{{"name", "animation"},
                           {"clip", "character.gltf#animation/Turn"}, {"loop", false}});
    writeText(root / "scene.json", scene_document.dump());
    auto &service = Animation::animationServiceRuntime();
    service.reset();
    Animation::poseGraphRuntime().clear();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto object = scene.objectId("Character");
    REQUIRE(object);
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object)
            document["nodes"][instance_index]["owner"]["object"] = binding.authoring_object_id.value;
    writeText(root / "passes/main_rendering_config.json", document.dump());
    const auto animate = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto *view = ecs.tryComponent<SimpleModelViewComponent>(*object);
    REQUIRE(view);
    REQUIRE(view->model_instance_id);
    const auto instance = *view->model_instance_id;
    // Before the graph is published nothing writes the sink: the attribute
    // still reports the rest pose, which is what the acceptance control
    // reproduces by removing the sink.
    animate();
    const auto rest = instances.animationNodeMatrix(instance, 2);
    REQUIRE(rest);
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    animate();
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };
    if (component_writer) {
        // The component already writes this object's pose. The graph's sink on
        // the same object is named, not silently a second writer. Attachment
        // happens once the published program reaches the animation tick, so the
        // rejection is expected within the first few frames.
        std::string code;
        std::string second;
        for (int frame = 0; frame < 6 && code.empty(); ++frame) {
            try {
                animate();
                renderer.render();
                core.waitIdle();
            } catch (const GraphRegistryError &error) {
                code = error.code();
                second = error.detail().at("second").get<std::string>();
                REQUIRE(error.detail().at("first") == "animation component");
            }
        }
        REQUIRE(code == "pose.sink_conflict@1");
        REQUIRE(second == "render/out");
        service.reset();
        Animation::poseGraphRuntime().clear();
        return;
    }
    // Every observation below is the GPU-side attribute, filled from the pose
    // the graph published this frame.
    std::vector<std::array<float, 4>> observed;
    bool left_rest = false;
    // AG-3a acceptance 1: the phase the frame drew, frame by frame, and -- for
    // a state machine -- whether a transition was ever caught in the middle
    // rather than only at its two ends.
    std::vector<double> phases;
    bool saw_transition = false;
    bool state_named_every_frame = true;
    for (int frame = 0; frame < 6; ++frame) {
        animate();
        const auto matrix = instances.animationNodeMatrix(instance, 2);
        REQUIRE(matrix);
        left_rest = left_rest || *matrix != *rest;
        const auto values = render();
        REQUIRE(std::abs(values[0] - (*matrix)[3].x) < 0.00001f);
        REQUIRE(std::abs(values[1] - (*matrix)[3].y) < 0.00001f);
        observed.push_back(values);
        const auto observed_trace = Animation::poseGraphRuntime().trace();
        if (observed_trace.empty()) continue;
        const auto &sink = observed_trace.front();
        if (!sink.cursors.empty()) phases.push_back(sink.cursors.front().normalized_phase);
        if (state_machine) {
            state_named_every_frame = state_named_every_frame && !sink.state.empty();
            saw_transition = saw_transition ||
                (sink.transition_active && sink.transition_progress >= 0.0 &&
                 sink.transition_progress < 1.0);
        }
    }
    REQUIRE(left_rest);
    // Determinism: one evaluation per frame revision, and the trace names the
    // node that produced the pose together with the layout generation it was
    // produced for.
    const auto trace = Animation::poseGraphRuntime().trace();
    REQUIRE(trace.size() == 1);
    REQUIRE(trace.front().sink == "render/out");
    REQUIRE(trace.front().node == "render/source");
    REQUIRE(trace.front().frame_revision == time.frameIndex());
    REQUIRE(trace.front().layout_identity != 0);
    REQUIRE(trace.front().layout_generation != 0);
    // AG-3a acceptance 1. The sink says which individual it writes and what
    // kind of chain produced the pose, and it stands on a named clip at a
    // named place inside it. None of this existed before: the clip path keeps
    // no cursor of its own, so a frame that does not record what it sampled
    // leaves nothing to observe.
    REQUIRE(trace.front().owner.scene == "default_scene");
    REQUIRE(trace.front().owner.object != 0);
    REQUIRE_FALSE(trace.front().cursors.empty());
    REQUIRE(phases.size() >= 2);
    // Time moves, so the phase moves: a build that recorded the phase once at
    // attach would show the same number on every frame.
    REQUIRE(phases.front() != phases.back());
    if (!state_machine) {
        REQUIRE(std::string{Animation::PoseGraphRuntime::sinkSourceName(trace.front().source)} ==
                "pose_graph_clip");
        REQUIRE(trace.front().cursors.size() == 1);
        REQUIRE(trace.front().cursors.front().clip == "Turn");
        REQUIRE(trace.front().state.empty());
        REQUIRE_FALSE(trace.front().root_motion);
        // The measured number rather than the same formula spelled twice: put
        // the clock on half a second and step once. `Turn` spans [0, 2] s, so
        // half a second into it is a quarter of the way through.
        time.setTime(0.5 - 0.25);
        animate();
        (void)render();
        const auto at_half = Animation::poseGraphRuntime().trace();
        REQUIRE(at_half.size() == 1);
        REQUIRE(std::abs(at_half.front().cursors.front().time_seconds - 0.5) < 1e-3);
        REQUIRE(std::abs(at_half.front().cursors.front().normalized_phase - 0.25) < 1e-3);
    } else {
        REQUIRE(std::string{Animation::PoseGraphRuntime::sinkSourceName(trace.front().source)} ==
                "pose_graph_state_machine");
        REQUIRE(state_named_every_frame);
        REQUIRE(trace.front().state == (connect_parameter ? "walk" : "idle"));
        // A machine that never leaves its initial state never transitions;
        // the one whose parameter edge is connected is caught mid-transition,
        // which is the half of the observation a start/end-only record loses.
        REQUIRE(saw_transition == connect_parameter);
    }
    // The same frame through the section get_status carries, so the shape a
    // tool reads is the shape a live pose graph actually produces -- not a
    // hand-written example of it.
    const auto status = internal::animationStatusJson();
    REQUIRE(status.at("available") == true);
    REQUIRE(status.at("sinks").size() == 1);
    const auto &reported = status.at("sinks").at(0);
    REQUIRE(reported.at("owner").at("scene") == "default_scene");
    REQUIRE(reported.at("owner").at("object").get<std::uint64_t>() != 0);
    REQUIRE(reported.at("node") == "render/source");
    REQUIRE(reported.at("source") ==
            (state_machine ? "pose_graph_state_machine" : "pose_graph_clip"));
    REQUIRE_FALSE(reported.at("cursors").empty());
    REQUIRE(reported.at("cursors").at(0).contains("normalized_phase"));
    // Absent, not empty: this chain extracts no root motion, and a sink with
    // no state machine has no transition to be between.
    REQUIRE_FALSE(reported.contains("root_delta"));
    REQUIRE(reported.contains("transition") == state_machine);
    REQUIRE(reported.contains("state") == state_machine);
    // The individual follows the clip: the pose is not frozen at the rest pose
    // the sink-less document leaves behind.
    REQUIRE(observed.front() != observed.back());

    if (state_machine) {
        // `Step` translates TipJoint away from the rest x=0; `Turn` only rotates
        // RootJoint, so how far x travels says which state is running.
        const bool moved = std::abs(observed.back()[0]) > 0.9f;
        REQUIRE(moved == connect_parameter);
    }
    service.reset();
    Animation::poseGraphRuntime().clear();
}

// AG-1b acceptance 1. Two individuals in one scene carry the same clip with the
// same playback parameters: one through the animation component, one through a
// pose graph in the pass document. Both poses reach the GPU as instance
// attributes and are read back from the same compute dispatch, so the frames
// compared are the frames that were drawn.
TEST_CASE("one clip poses the same whichever way it is attached", "[skeletal][graph-pose-parity]") {
    setupLogger();
    const auto mode = GENERATE(as<std::string>{}, "looping", "clamped", "shifted playback",
                               "clip that does not begin at zero",
                               "clamped clip that does not begin at zero");
    CAPTURE(mode);
    const bool loop = mode != "clamped" && mode != "clamped clip that does not begin at zero";
    const double speed = mode == "shifted playback" ? 1.5 : 1.0;
    const double start_time = mode == "shifted playback" ? 0.75 : 0.0;
    const float clip_offset = mode.ends_with("does not begin at zero") ? 3.0f : 0.0f;
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_pose_parity_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin",
                                   {.animation_time_offset = clip_offset});

    const nlohmann::json attribute{{"id", "tip"}, {"op", "render.attribute"},
        {"parameters", {{"element", "mat4"}, {"domain", "instance"},
            {"source", {{"asset", "project://character.gltf"}, {"pose_node", 2}}}}},
        {"outputs", {"resource"}}};
    auto document = parameterGraphCompositionFixture();
    // The component-driven individual only reads its pose out; nothing in this
    // definition writes a sink, so the component stays its single writer.
    document["definitions"]["view"] = {{"nodes", nlohmann::json::array({attribute})},
        {"outputs", {{"tip", {{"node", "tip"}, {"port", "resource"}}}}}};
    nlohmann::json posed_nodes = nlohmann::json::array();
    posed_nodes.push_back(nlohmann::json::parse(R"({"id":"time","op":"graph.input"})"));
    posed_nodes.push_back(attribute);
    posed_nodes.push_back(nlohmann::json{{"id", "source"}, {"op", "pose.clip_sample"},
        {"parameters", {{"rig", "project://character.gltf"}, {"clip", "Turn"},
                        {"speed", speed}, {"loop", loop}, {"start_time", start_time}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}}}}, {"outputs", {"pose"}}});
    posed_nodes.push_back(nlohmann::json::parse(R"({"id":"out","op":"pose.skeletal",
      "inputs":{"pose":{"node":"source","port":"pose"}},"outputs":[]})"));
    document["definitions"]["posed"] = {{"nodes", std::move(posed_nodes)},
        {"outputs", {{"tip", {{"node", "tip"}, {"port", "resource"}}}}}};

    document["nodes"][5] = {{"id", "component_view"}, {"op", "graph.instance"},
        {"definition", "view"}, {"owner", {{"scene", "default_scene"}, {"object", 1}}},
        {"inputs", nlohmann::json::object()}};
    document["nodes"][6]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][7];
    probe["parameters"]["ports"].erase("source");
    probe["inputs"].erase("source");
    for (const auto *port : {"component", "posed"}) {
        probe["parameters"]["ports"][port] = {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}};
    }
    probe["inputs"]["component"] = {{"node", "component_view"}, {"port", "tip"}};
    probe["inputs"]["posed"] = {{"node", "pose_view"}, {"port", "tip"}};
    document["nodes"].push_back({{"id", "pose_view"}, {"op", "graph.instance"},
        {"definition", "posed"}, {"owner", {{"scene", "default_scene"}, {"object", 2}}},
        {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}}});
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 component = pelican_load_component(0);
    mat4 posed = pelican_load_posed(0);
    pelican_store_destination(0, component[3].x);
    pelican_store_destination(1, component[3].y);
    pelican_store_destination(2, posed[3].x);
    pelican_store_destination(3, posed[3].y);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.gltf"}]})");
    nlohmann::json animation_component{{"name", "animation"},
        {"clip", "character.gltf#animation/Turn"}, {"loop", loop},
        {"speed", speed}, {"start_time", start_time}};
    auto scene_document = nlohmann::json::parse(R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
      {"name":"Component","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]},
      {"name":"Posed","components":[{"name":"transform","pos":[4,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]}]}}})");
    scene_document["scenes"]["default_scene"]["objects"][0]["components"].push_back(animation_component);
    writeText(root / "scene.json", scene_document.dump());

    auto &service = Animation::animationServiceRuntime();
    service.reset();
    Animation::poseGraphRuntime().clear();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &time = GET_MODULE(EngineTime);
    // The observed window straddles the clip's end: it opens inside the clip
    // and closes well past it, so a clamp that is not applied shows up as a
    // wrap back to the first keyframe.
    time.setup(EngineTime::Mode::fixed_step, 0.5);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto component_object = scene.objectId("Component");
    const auto posed_object = scene.objectId("Posed");
    REQUIRE(component_object);
    REQUIRE(posed_object);
    for (const auto &binding : scene.runtimeObjectBindings()) {
        if (binding.object_id == *component_object)
            document["nodes"][5]["owner"]["object"] = binding.authoring_object_id.value;
        if (binding.object_id == *posed_object)
            document["nodes"].back()["owner"]["object"] = binding.authoring_object_id.value;
    }
    writeText(root / "passes/main_rendering_config.json", document.dump());
    const auto animate = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto *component_view = ecs.tryComponent<SimpleModelViewComponent>(*component_object);
    const auto *posed_view = ecs.tryComponent<SimpleModelViewComponent>(*posed_object);
    REQUIRE(component_view);
    REQUIRE(posed_view);
    REQUIRE(component_view->model_instance_id);
    REQUIRE(posed_view->model_instance_id);
    animate();
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    animate();
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };
    std::vector<std::array<float, 4>> observed;
    for (int frame = 0; frame < 8; ++frame) {
        animate();
        const auto component_matrix = instances.animationNodeMatrix(*component_view->model_instance_id, 2);
        const auto posed_matrix = instances.animationNodeMatrix(*posed_view->model_instance_id, 2);
        REQUIRE(component_matrix);
        REQUIRE(posed_matrix);
        const auto values = render();
        // What the GPU read is what each path published this frame.
        REQUIRE(std::abs(values[0] - (*component_matrix)[3].x) < 0.00001f);
        REQUIRE(std::abs(values[1] - (*component_matrix)[3].y) < 0.00001f);
        REQUIRE(std::abs(values[2] - (*posed_matrix)[3].x) < 0.00001f);
        REQUIRE(std::abs(values[3] - (*posed_matrix)[3].y) < 0.00001f);
        // ... and the two attachments agree, frame for frame.
        INFO("frame " << frame << " time " << time.now());
        REQUIRE(std::abs(values[0] - values[2]) < 0.00001f);
        REQUIRE(std::abs(values[1] - values[3]) < 0.00001f);
        observed.push_back(values);
    }
    // AG-1b acceptance 4, the publish side. Both attachments left a writer
    // token on the instance they wrote, naming which of the two kinds of writer
    // it was -- the component's direct publish, or a claimed animation source.
    const auto component_writer = instances.animationFrameWriter(*component_view->model_instance_id);
    const auto posed_writer = instances.animationFrameWriter(*posed_view->model_instance_id);
    REQUIRE(component_writer.kind == AnimationFrameWriterKind::animation_component_clip);
    REQUIRE(posed_writer.kind == AnimationFrameWriterKind::animation_source);
    REQUIRE(component_writer.frame_revision == time.frameIndex());
    REQUIRE(posed_writer.frame_revision == time.frameIndex());
    // A different writer reaching the same instance in the same revision is
    // refused before anything about the frame it brought is even read, so it
    // cannot land on top of the pose that is already there.
    Animation::PublishAnimationFrameDescV1 intruder{};
    intruder.struct_size = sizeof(intruder);
    intruder.version = Animation::descriptorVersionV1;
    intruder.instance = instances.animationInstance(*component_view->model_instance_id);
    intruder.frame_revision = time.frameIndex();
    intruder.root_delta.rotation.w = 1.0f;
    REQUIRE(instances.publishAnimationFrame(*component_view->model_instance_id,
                {AnimationFrameWriterKind::animation_source, 7}, intruder) ==
            Animation::Status::authority_conflict);
    // The writer that owns the revision is not refused by the same rule: it
    // gets as far as its own frame being validated.
    REQUIRE(instances.publishAnimationFrame(*component_view->model_instance_id,
                {AnimationFrameWriterKind::animation_component_clip, animationComponentClipWriter}, intruder) ==
            Animation::Status::invalid_argument);
    // Nor is a different writer in the next revision: a handover between frames
    // is legitimate, only sharing one frame is not.
    intruder.frame_revision = time.frameIndex() + 1;
    REQUIRE(instances.publishAnimationFrame(*component_view->model_instance_id,
                {AnimationFrameWriterKind::animation_source, 7}, intruder) ==
            Animation::Status::invalid_argument);

    // The window really covers the interesting part of the clip: the pose moves,
    // and a clamped clip ends up standing still on its final frame while a
    // looping one comes back round.
    REQUIRE(observed.front() != observed.back());
    if (!loop) {
        REQUIRE(observed[observed.size() - 2] == observed.back());
        // 180 degrees about z at the clip's last keyframe: x mirrors, and the
        // wrap this test would see instead returns to the first keyframe.
        REQUIRE(std::abs(observed.back()[2] - observed.front()[2]) > 0.5f);
    }
    service.reset();
    Animation::poseGraphRuntime().clear();
}

// AG-1b acceptance 2. A parameter edit is not a reason to forget where a state
// machine is. The instance parameter travels the runtime override path, so the
// program is not rebuilt at all; when the document itself is republished the
// new generation adopts the evaluator instead of constructing a fresh one.
TEST_CASE("editing a parameter does not send a state machine back to its initial state",
          "[skeletal][graph-pose-state-transfer]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_pose_state_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestSkeletalFixture::writeGltf(root / "character.gltf", "character.bin");
    const auto state_machine_document = nlohmann::json::parse(R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{"jump":0,"tune":0},
      "states":[{"name":"move","type":"clip","clip":"Turn","loop":true},
                {"name":"jump","type":"clip","clip":"Step","loop":true}],
      "transitions":[{"from":"move","to":"jump",
        "conditions":[{"parameter":"jump","op":">","value":0.5}]}],
      "initial_state":"move"})");
    auto document = parameterGraphCompositionFixture();
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(nlohmann::json::parse(R"({"id":"time","op":"graph.input"})"));
    // Declared with a default and left unconnected by the instance, so the
    // composition lowers it to a value.parameter the scene can override at
    // runtime -- the path a parameter edit actually takes.
    nodes.push_back(nlohmann::json::parse(R"({"id":"jump","op":"graph.input","default":0})"));
    nodes.push_back(nlohmann::json::parse(R"({"id":"tune","op":"graph.input","default":0})"));
    nodes.push_back(nlohmann::json{{"id", "tip"}, {"op", "render.attribute"},
        {"parameters", {{"element", "mat4"}, {"domain", "instance"},
            {"source", {{"asset", "project://character.gltf"}, {"pose_node", 2}}}}},
        {"outputs", {"resource"}}});
    nodes.push_back(nlohmann::json{{"id", "brain"}, {"op", "pose.state_machine"},
        {"parameters", {{"rig", "project://character.gltf"}, {"graph", state_machine_document}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                    {"jump", {{"node", "jump"}, {"port", "value"}}},
                    {"tune", {{"node", "tune"}, {"port", "value"}}}}},
        {"outputs", {"pose"}}});
    nodes.push_back(nlohmann::json::parse(R"({"id":"out","op":"pose.skeletal",
      "inputs":{"pose":{"node":"brain","port":"pose"}},"outputs":[]})"));
    document["definitions"]["posed"] = {{"nodes", std::move(nodes)},
        {"outputs", {{"tip", {{"node", "tip"}, {"port", "resource"}}}}}};
    document["nodes"][5] = {{"id", "hero"}, {"op", "graph.instance"},
        {"definition", "posed"}, {"owner", {{"scene", "default_scene"}, {"object", 1}}},
        {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}}};
    document["nodes"][6]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][7];
    probe["parameters"]["ports"].erase("source");
    probe["inputs"].erase("source");
    probe["parameters"]["ports"]["tip"] = {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}};
    probe["inputs"]["tip"] = {{"node", "hero"}, {"port", "tip"}};
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 tip = pelican_load_tip(0);
    pelican_store_destination(0, tip[3].x);
    pelican_store_destination(1, tip[3].y);
    pelican_store_destination(2, tip[3].z);
    pelican_store_destination(3, tip[0].x);
}
)");
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"character.gltf"}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
      {"name":"Hero","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"character"}]}]}}})");

    auto &service = Animation::animationServiceRuntime();
    service.reset();
    Animation::poseGraphRuntime().clear();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    const auto object = scene.objectId("Hero");
    REQUIRE(object);
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object)
            document["nodes"][5]["owner"]["object"] = binding.authoring_object_id.value;
    writeText(root / "passes/main_rendering_config.json", document.dump());
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto tick = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto animate = [&] {
        tick();
        renderer.render();
        core.waitIdle();
    };
    const auto traced = [&] {
        const auto trace = Animation::poseGraphRuntime().trace();
        REQUIRE(trace.size() == 1);
        return trace.front();
    };
    const auto valueGraph = [&]() -> std::shared_ptr<const ValueGraph::Graph> {
        const auto passes = GET_MODULE(RenderingPassContainer).snapshot();
        REQUIRE(passes);
        for (const auto &[id, program] : passes->programs) {
            (void)id;
            if (program.frame_graph.value_graph) return program.frame_graph.value_graph;
        }
        FAIL("the published program has no value graph");
        return {};
    };
    tick();
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    tick();
    for (int frame = 0; frame < 4; ++frame) animate();
    REQUIRE(traced().state == "move");

    // Leaving the initial state is what makes the rest of this test mean
    // something: the parameter edit below must not undo it.
    REQUIRE(scene.setGraphParameter(valueGraph(), *object, "hero/jump", 1.0));
    for (int frame = 0; frame < 4; ++frame) animate();
    REQUIRE(traced().state == "jump");

    // A second parameter edit. The override path does not rebuild the program,
    // so the attachment -- and the evaluator inside it -- is the same object.
    REQUIRE(scene.setGraphParameter(valueGraph(), *object, "hero/tune", 0.5));
    animate();
    REQUIRE(traced().state == "jump");

    // Now the document itself changes. A new program generation compiles a new
    // pose graph, and the state machine is handed over rather than rebuilt.
    const auto previous_graph = valueGraph();
    document["nodes"][2]["parameters"]["value"] = 5;
    writeText(root / "passes/main_rendering_config.json", document.dump());
    loadRenderGraphVariantsFromConfigData(document.dump());
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    for (int frame = 0; frame < 8 && valueGraph() == previous_graph; ++frame) animate();
    // The generation really did change: this is a different compiled document,
    // not the same one still running.
    REQUIRE(valueGraph() != previous_graph);
    REQUIRE(traced().state == "jump");

    // Control: the handover is by identity, not a blanket "keep whatever was
    // running". Republish with a different state machine document and the node
    // is a different machine, so it starts where its own document says.
    const auto carried_graph = valueGraph();
    // Drop the override first: the machine has no way back to `move`, so what
    // the control sees can only come from a machine that was started afresh.
    REQUIRE(scene.setGraphParameter(carried_graph, *object, "hero/jump", std::nullopt));
    animate();
    REQUIRE(traced().state == "jump");
    auto edited = state_machine_document;
    edited["states"].push_back(nlohmann::json::parse(
        R"({"name":"land","type":"clip","clip":"Turn","loop":false})"));
    document["definitions"]["posed"]["nodes"][4]["parameters"]["graph"] = edited;
    writeText(root / "passes/main_rendering_config.json", document.dump());
    loadRenderGraphVariantsFromConfigData(document.dump());
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    for (int frame = 0; frame < 10; ++frame) animate();
    REQUIRE(valueGraph() != carried_graph);
    REQUIRE(traced().state == "move");
    service.reset();
    Animation::poseGraphRuntime().clear();
}


// P-2b acceptance 2. The shipped learning project drives one object's pose
// through `pose.blend`, and the weight of that blend comes from a curve on the
// values side of the very same document. The document and the scene are the
// ones in `projects/posegraph_demo`: the test copies the project, adds a probe
// that reads the posed joint back off the GPU, and changes nothing else.
// `t = 0.25 s` is the measurement point because both clips are at identity on
// the 1 s loop boundary, where no weight can be told from another.
TEST_CASE("the posegraph_demo curve drives the blend that poses the hero",
          "[skeletal][graph-pose-sink][posegraph-demo]") {
    setupLogger();
    const auto mode = GENERATE(as<std::string>{}, "the shipped curve",
                               "weight edge disconnected", "weight parameter set to one",
                               "owner that matches no object", "owner that names the camera");
    CAPTURE(mode);
    const auto shipped_root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                              "projects" / "posegraph_demo";
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_posegraph_demo_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    std::filesystem::copy(shipped_root, root,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(root / "shaders");

    // The shipped bytes, parsed and not rewritten: every edit below is one the
    // README tells a reader to make, or the probe the acceptance needs.
    std::ifstream shipped_stream{shipped_root / "passes" / "main.json", std::ios::binary};
    REQUIRE(shipped_stream.good());
    auto document = nlohmann::json::parse(std::string{
        std::istreambuf_iterator<char>{shipped_stream}, std::istreambuf_iterator<char>{}});
    REQUIRE(document.at("schema") == "pelican.graph");
    // By id, never by position: a reordered document would otherwise be edited
    // somewhere else and still pass.
    const auto node_by_id = [](nlohmann::json &nodes, const std::string &id) -> nlohmann::json & {
        for (auto &node : nodes)
            if (node.at("id").get<std::string>() == id) return node;
        FAIL("the shipped document has no node " << id);
        return nodes.at(0);
    };
    const auto rig = node_by_id(document["definitions"]["walk_run"]["nodes"], "walk")
                         .at("parameters").at("rig").get<std::string>();
    if (mode == "weight edge disconnected" || mode == "weight parameter set to one") {
        // `set_input` with a null source is what the Inspector's "disconnect"
        // does; the port then falls back to the instance's own `parameters`.
        document = editGraphDocument(document, nlohmann::json::array({
            {{"op", "set_input"}, {"node", "hero_walk_run"}, {"port", "weight"}, {"source", nullptr}}}));
        if (mode == "weight parameter set to one")
            document = editGraphDocument(document, nlohmann::json::array({
                {{"op", "set_node_parameters"}, {"node", "hero_walk_run"},
                 {"parameters", {{"weight", 1.0}}}}}));
    }
    // Two ways to get the positional owner id wrong. `99` names nothing; `2`
    // is the Camera, the off-by-one an author makes by putting the camera
    // first in scene.json. Neither is rejected when the document is authored.
    if (mode.starts_with("owner"))
        node_by_id(document["nodes"], "hero_walk_run")["owner"]["object"] =
            mode == "owner that matches no object" ? 99 : 2;

    // The probe: one attribute inside the definition, and the compute that
    // copies it into a host-readable buffer. The demo ships neither.
    const bool probe = !mode.starts_with("owner");
    if (probe) {
        document["definitions"]["walk_run"]["nodes"].push_back(nlohmann::json{
            {"id", "tip"}, {"op", "render.attribute"},
            {"parameters", {{"element", "mat4"}, {"domain", "instance"},
                            {"source", {{"asset", rig}, {"pose_node", 2}}}}},
            {"outputs", {"resource"}}});
        document["definitions"]["walk_run"]["outputs"]["tip"] = {{"node", "tip"}, {"port", "resource"}};
        document["nodes"].push_back(nlohmann::json{{"id", "result"}, {"op", "render.buffer"},
            {"parameters", {{"size", 16}, {"element", "float"}, {"host_source", "runtime_f32"}}},
            {"outputs", {"resource"}}});
        document["nodes"].push_back(nlohmann::json{{"id", "probe"}, {"op", "render.compute"},
            {"parameters", {{"shader", "shaders/graph_probe"}, {"dispatch", {{"groups", {1, 1, 1}}}},
                {"ports", {{"tip", {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}}},
                           {"destination", {{"element", "float"}, {"access", "write"}}}}}}},
            {"inputs", {{"tip", {{"node", "hero_walk_run"}, {"port", "tip"}}},
                        {"destination", {{"node", "result"}, {"port", "resource"}}}}},
            {"outputs", {"destination"}}});
        document["outputs"]["gpu"] = {{"node", "probe"}, {"port", "destination"}};
        writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 tip = pelican_load_tip(0);
    pelican_store_destination(0, tip[0].y);
    pelican_store_destination(1, tip[0].x);
    pelican_store_destination(2, tip[3].y);
    pelican_store_destination(3, tip[3].z);
}
)");
    }
    writeText(root / "passes/main_rendering_config.json", document.dump());

    auto &service = Animation::animationServiceRuntime();
    service.reset();
    Animation::poseGraphRuntime().clear();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    // The shipped basic_config, minus the rendering config reference: the
    // document under test is published below, and the UI overlay the demo's
    // `ui` feature needs is the one the project names.
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets/asset_data.json"},
        {"ui_config_json", "ui/status_overlay.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto object = scene.objectId("Hero");
    REQUIRE(object);
    // The authoring id the shipped document names is positional. Naming it here
    // is the only thing that keeps `owner.object: 1` true when the scene's
    // object list is reordered.
    std::optional<std::uint32_t> hero_authoring_id;
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object) hero_authoring_id = binding.authoring_object_id.value;
    REQUIRE(hero_authoring_id);
    REQUIRE(*hero_authoring_id == 1);

    const auto animate = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto *view = ecs.tryComponent<SimpleModelViewComponent>(*object);
    REQUIRE(view);
    REQUIRE(view->model_instance_id);
    const auto instance = *view->model_instance_id;
    animate();
    const auto rest = instances.animationNodeMatrix(instance, 2);
    REQUIRE(rest);
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    animate();
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };

    if (mode.starts_with("owner")) {
        // Whatever the engine does with a misdirected owner, the one thing the
        // reader sees is that the hero is not posed. Whether that arrives as a
        // named failure or as silence is recorded, not assumed.
        std::string named_failure;
        try {
            for (int frame = 0; frame < 4; ++frame) {
                animate();
                renderer.render();
                core.waitIdle();
            }
        } catch (const std::exception &error) {
            named_failure = error.what();
        }
        WARN("misdirected owner (" << mode << ") reported: "
             << (named_failure.empty() ? std::string{"nothing at all"} : named_failure));
        const auto matrix = instances.animationNodeMatrix(instance, 2);
        REQUIRE(matrix);
        REQUIRE(*matrix == *rest);
        service.reset();
        Animation::poseGraphRuntime().clear();
        return;
    }

    // One frame is one fixed 0.25 s step, so the clip's own quarter-second
    // key is a frame and not an interpolation of the test's making. Two more
    // frames so the published program has reached the animation
    // tick and the graph is attached; the values they carry are the curve at
    // other points, which is what makes the measured frame a measurement and
    // not a constant.
    animate();
    const auto warm = render();
    animate();
    (void)render();
    // Then the clock goes back, so the frame that is measured is the quarter
    // second after the scene start -- the point the acceptance names, and the
    // one place in the first second where both clips are far from the loop
    // boundary they share.
    time.setTime(0.0);
    animate();
    const auto scene_time = time.now() - scene.sceneStartTime();
    CAPTURE(scene_time);
    REQUIRE(std::abs(scene_time - 0.25) < 1e-9);
    const auto matrix = instances.animationNodeMatrix(instance, 2);
    REQUIRE(matrix);
    const auto values = render();
    // The GPU read-back is the same number the published pose carries, so the
    // assertion below is about the frame that was drawn.
    REQUIRE(std::abs(values[0] - (*matrix)[0].y) < 0.00001f);
    const auto measured = values[0];
    CAPTURE(measured);
    // The pose is not frozen: a frame at another point of the curve reads
    // differently through the same probe.
    REQUIRE(measured != warm[0]);
    if (mode == "the shipped curve") {
        // The curve is at 0.5 a quarter second in: Walk's +20 degrees and
        // Run's +50 degrees blend to 35 degrees, so sin 35 is what the joint
        // carries. sin 20 = 0.342 and sin 50 = 0.766 are the two controls.
        REQUIRE(std::abs(measured - 0.5735765f) < 1e-3f);
    } else if (mode == "weight edge disconnected") {
        REQUIRE(std::abs(measured - 0.3420201f) < 1e-3f);
    } else {
        REQUIRE(std::abs(measured - 0.7660444f) < 1e-3f);
    }
    service.reset();
    Animation::poseGraphRuntime().clear();
}

} // namespace Pelican
