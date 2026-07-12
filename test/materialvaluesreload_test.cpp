#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/assetkey.hpp"
#include "../src/core/watch/filewatcher.hpp"
#include "../src/core/watch/reloadservice.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using namespace std::chrono_literals;

struct Sandbox {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pelican_wp105_" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() { std::filesystem::create_directories(root); }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.is_open());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    REQUIRE(output.good());
}

void writeMaterial(const std::filesystem::path &path, std::string_view name,
                   double scalar, std::string_view surface = "project://wp76.surface") {
    const auto document = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", nlohmann::json::array({
            {{"name", name}, {"surface", surface},
             {"values", {{"scalar_first", scalar}}}}
        })},
    };
    writeText(path, document.dump(2));
}

void writeTwoMaterials(const std::filesystem::path &path, double scalar_a,
                       double scalar_b,
                       std::string_view surface_a = "project://wp76.surface") {
    const auto document = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", nlohmann::json::array({
            {{"name", "material_a"}, {"surface", surface_a},
             {"values", {{"scalar_first", scalar_a}}}},
            {{"name", "material_b"}, {"surface", "project://wp76.surface"},
             {"values", {{"scalar_first", scalar_b}}}}
        })},
    };
    writeText(path, document.dump(2));
}

template <class T> T readAt(const std::vector<std::byte> &bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void configureGpu(const Sandbox &box) {
    const auto scene = box.root / "scene.json";
    const auto assets = box.root / "assets.json";
    writeText(scene, "{}");
    writeText(assets, "{}");
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{
        {"basic_config", {{"window_size", {{"width", 16}, {"height", 16}}},
                          {"scene_data_json", scene.generic_string()},
                          {"asset_data_json", assets.generic_string()}}}}
        .dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
}

SurfaceFormatDocument wp76Surface() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                      "surface_format" / "valid" / "wp76.surface";
    auto surface = parseSurfaceFormat(readText(path), "project://wp76.surface");
    surface.params.front().min = 0.0;
    surface.params.front().max = 100.0;
    return surface;
}

LoweredMaterial loadLowered(const std::filesystem::path &path,
                            const MaterialSurfaceCatalog &catalog,
                            std::string_view name = {}) {
    std::ifstream input{path, std::ios::binary};
    const auto document = parseMaterialFormatJson(nlohmann::json::parse(input), catalog);
    const auto found = name.empty()
        ? document.materials.begin()
        : std::find_if(document.materials.begin(), document.materials.end(),
                       [name](const auto &material) { return material.name == name; });
    REQUIRE(found != document.materials.end());
    const auto &material = *found;
    return lowerMaterial(material, catalog.at(*material.surface));
}

GlobalMaterialId registerValuesMaterial(MaterialContainer &materials,
                                        const LoweredMaterial &lowered) {
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    info.custom_values_layout = lowered.values_layout;
    info.custom_values = lowered.values;
    return materials.registerMaterial(std::move(info));
}

} // namespace

TEST_CASE("HR1-M updates one same-layout material and rolls back invalid candidates",
          "[wp105][material-values-reload][gpu]") {
    setupLogger();
    Sandbox box;
    try {
        FastModuleContainer modules;
        configureGpu(box);
        const auto surface = wp76Surface();
        MaterialSurfaceCatalog catalog{{"project://wp76.surface", surface}};
        auto alternate = surface;
        alternate.params.push_back(alternate.params.front());
        alternate.params.back().name = "layout_drift";
        catalog.emplace("project://alternate.surface", std::move(alternate));

        const auto path_a = box.root / "pair.material.json";
        writeTwoMaterials(path_a, 4.0, 9.0);

        auto &materials = GET_MODULE(MaterialContainer);
        const auto material_a = registerValuesMaterial(
            materials, loadLowered(path_a, catalog, "material_a"));
        const auto material_b = registerValuesMaterial(
            materials, loadLowered(path_a, catalog, "material_b"));
        const auto pipeline_a = materials.pipelineLayout(material_a);
        const auto descriptor_a = materials.materialDescriptorRevisionForTesting(material_a);
        const auto key_a = watch::makeAssetKey("pair.material.json");
        const std::array bindings{
            MaterialContainer::ReloadableMaterialValuesBinding{"material_a", material_a},
            MaterialContainer::ReloadableMaterialValuesBinding{"material_b", material_b}};
        materials.registerReloadableMaterialValuesFile(key_a, path_a, catalog, bindings);
        auto &reload = GET_MODULE(watch::ReloadService);

        const auto resources = reload.transactions().registry().snapshot().resourceCount();
        writeTwoMaterials(path_a, 12.0, 9.0);
        REQUIRE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(readAt<float>(materials.materialValuesForTesting(material_a), 0) ==
                Catch::Approx(12.0f));
        REQUIRE(readAt<float>(materials.materialGpuValuesForTesting(material_a), 0) ==
                Catch::Approx(12.0f));
        REQUIRE(readAt<float>(materials.materialGpuValuesForTesting(material_b), 0) ==
                Catch::Approx(9.0f));
        REQUIRE(materials.pipelineLayout(material_a) == pipeline_a);
        REQUIRE(materials.materialDescriptorRevisionForTesting(material_a) == descriptor_a);
        REQUIRE(reload.transactions().registry().snapshot().resourceCount() == resources);

        const auto before_failure = materials.materialGpuValuesForTesting(material_a);
        writeText(path_a, "{ broken json");
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        auto after_failure = materials.materialGpuValuesForTesting(material_a);
        REQUIRE(std::equal(after_failure.begin(), after_failure.end(),
                           before_failure.begin(), before_failure.end()));

        auto type_mismatch = nlohmann::json{
            {"schema", "pelican.material"}, {"version", 1},
            {"materials", nlohmann::json::array({
                {{"name", "material_a"}, {"surface", "project://wp76.surface"},
                 {"values", {{"scalar_first", "wrong"}}}},
                {{"name", "material_b"}, {"surface", "project://wp76.surface"},
                 {"values", {{"scalar_first", 9.0}}}}
            })}};
        writeText(path_a, type_mismatch.dump(2));
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        after_failure = materials.materialGpuValuesForTesting(material_a);
        REQUIRE(std::equal(after_failure.begin(), after_failure.end(),
                           before_failure.begin(), before_failure.end()));

        writeTwoMaterials(path_a, 101.0, 9.0);
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        after_failure = materials.materialGpuValuesForTesting(material_a);
        REQUIRE(std::equal(after_failure.begin(), after_failure.end(),
                           before_failure.begin(), before_failure.end()));

        writeTwoMaterials(path_a, 20.0, 9.0, "project://alternate.surface");
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        after_failure = materials.materialGpuValuesForTesting(material_a);
        REQUIRE(std::equal(after_failure.begin(), after_failure.end(),
                           before_failure.begin(), before_failure.end()));
        const auto status = reload.transactions().status();
        REQUIRE(status.failed == 4);
        REQUIRE(status.last_reload_error);
        REQUIRE(status.last_reload_error->message.find("material_a") != std::string::npos);
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan material values reload unavailable: "} + error.what());
    }
}

TEST_CASE("HR1-M watcher gate and 1000 reloads keep resources bounded",
          "[wp105][material-values-reload][gpu][stress]") {
    setupLogger();
    Sandbox box;
    try {
        FastModuleContainer modules;
        configureGpu(box);
        const auto surface = wp76Surface();
        MaterialSurfaceCatalog catalog{{"project://wp76.surface", surface}};
        const auto path = box.root / "stress.material.json";
        writeMaterial(path, "stress", 1.0);
        const auto key = watch::makeAssetKey("stress.material.json");
        auto &materials = GET_MODULE(MaterialContainer);
        const auto material = registerValuesMaterial(materials, loadLowered(path, catalog));
        const std::array bindings{
            MaterialContainer::ReloadableMaterialValuesBinding{"stress", material}};
        materials.registerReloadableMaterialValuesFile(key, path, catalog, bindings);
        auto &reload = GET_MODULE(watch::ReloadService);
        const auto before = reload.transactions().registry().snapshot();
        const auto resource = before.find("material-values", watch::makeAssetKey(
            "stress.material.json#stress"));
        REQUIRE(resource);

        for (int i = 0; i < 1000; ++i) {
            writeMaterial(path, "stress", i % 2 == 0 ? 2.0 : 3.0);
            REQUIRE(reload.applyRequestForTesting(
                {key, watch::ReloadKind::modified, {}, 1}));
        }
        const auto after = reload.transactions().registry().snapshot();
        const auto reloaded = after.find("material-values", watch::makeAssetKey(
            "stress.material.json#stress"));
        REQUIRE(reloaded);
        REQUIRE(reloaded->ref == resource->ref);
        REQUIRE(after.resourceCount() == before.resourceCount());
        REQUIRE(after.liveBytes() == before.liveBytes());

        bool gate_enabled = false;
        watch::FileWatcherOptions options;
        options.manual_clock = true;
        options.poll_interval = 10ms;
        options.watch_arm_override = [](const watch::WatchStore &, unsigned) { return false; };
        watch::FileWatcher watcher({{"project", box.root, {}}},
            [&] { return watch::ReloadGateSnapshot{gate_enabled, 7, "replay"}; }, options);
        const auto t0 = watch::FileWatcher::Clock::now();
        watcher.runControlCycleForTesting(t0);
        const auto disabled_value = materials.materialGpuValuesForTesting(material);
        writeMaterial(path, "stress", 7.0);
        watcher.runControlCycleForTesting(t0 + 20ms);
        REQUIRE(watcher.applyFrame([&](const watch::ReloadRequest &request) {
            return reload.applyRequestForTesting(request);
        }) == 0);
        const auto still_disabled = materials.materialGpuValuesForTesting(material);
        REQUIRE(std::equal(still_disabled.begin(), still_disabled.end(),
                           disabled_value.begin(), disabled_value.end()));

        gate_enabled = true;
        watcher.runControlCycleForTesting(t0 + 40ms);
        writeMaterial(path, "stress", 8.0);
        watcher.runControlCycleForTesting(t0 + 60ms);
        REQUIRE(watcher.applyFrame([&](const watch::ReloadRequest &request) {
            return reload.applyRequestForTesting(request);
        }) >= 1);
        REQUIRE(readAt<float>(materials.materialGpuValuesForTesting(material), 0) ==
                Catch::Approx(8.0f));
        watcher.stop();
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan material values reload stress unavailable: "} + error.what());
    }
}

} // namespace Pelican
