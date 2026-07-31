#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/watch/assetkey.hpp"
#include "../src/core/watch/filewatcher.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "ktx2_test_writer.hpp"
#include "vulkan_test_support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>

namespace Pelican {
namespace {

using namespace std::chrono_literals;

constexpr std::array<std::uint8_t, 70> redPng{
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0xF8,0xCF,0xC0,0xF0,
    0x1F,0x00,0x05,0x00,0x01,0xFF,0x56,0xC7,0x2F,0x0D,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82};
constexpr std::array<std::uint8_t, 70> bluePng{
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0x60,0x60,0xF8,0xFF,
    0x1F,0x00,0x03,0x02,0x01,0xFF,0x39,0x29,0x19,0xBE,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4E,0x44,0xAE,0x42,0x60,0x82};
constexpr std::array<std::uint8_t, 69> green2Png{
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0xF4,0x22,0x7F,
    0x8A,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0x60,0xF8,0x0F,0x81,
    0x00,0x0F,0xF9,0x03,0xFD,0xE6,0xC4,0xA2,0xED,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
    0x44,0xAE,0x42,0x60,0x82};

struct Sandbox {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pelican_wp100_" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() { std::filesystem::create_directories(root); }
    ~Sandbox() { std::error_code error; std::filesystem::remove_all(root, error); }
};

template <class T, std::size_t Extent>
void writeBytes(const std::filesystem::path &path, std::span<const T, Extent> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size_bytes()));
    output.close();
    REQUIRE(output.good());
}

void configureGpu(const Sandbox &box) {
    const auto scene = box.root / "scene.json";
    const auto assets = box.root / "assets.json";
    std::ofstream{scene} << "{}";
    std::ofstream{assets}
        << R"json({"schema":"pelican.asset_data","version":1,"models":[]})json";
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{
        {"basic_config", {{"window_size", {{"width", 16}, {"height", 16}}},
                          {"scene_data_json", scene.generic_string()},
                          {"asset_data_json", assets.generic_string()}}}}
        .dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
}

GlobalMaterialId registerReferencingMaterial(MaterialContainer &materials,
                                             GlobalTextureId texture) {
    const auto &standard = GET_MODULE(StandardMaterialResource);
    return materials.registerMaterial(MaterialInfo{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = texture,
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    });
}

} // namespace

TEST_CASE("HR1-T reload keeps identity, rebinds shape changes, and rolls back failures",
          "[wp100][texture-reload][gpu]") {
    setupLogger();
    Sandbox box;
    FastModuleContainer modules;
    configureGpu(box);
    TestSupport::requireVulkanDevice(
        "Vulkan texture reload unavailable");
        const auto path = box.root / "color.png";
        writeBytes(path, std::span{redPng});
        const auto key = watch::makeAssetKey("color.png");
        auto &materials = GET_MODULE(MaterialContainer);
        const auto texture = materials.registerReloadableTextureFile(key, path);
        const auto material = registerReferencingMaterial(materials, texture);
        auto &reload = GET_MODULE(watch::ReloadService);

        const auto initial_views = materials.textureViewsForTesting(texture);
        REQUIRE(materials.texturePixelsForTesting(texture) ==
                std::vector<std::uint8_t>{255, 0, 0, 255});
        REQUIRE(materials.referencingMaterialCountForTesting(texture) == 1);
        REQUIRE(materials.materialDescriptorRevisionForTesting(material) == 0);

        writeBytes(path, std::span{bluePng});
        REQUIRE(reload.applyRequestForTesting({key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(materials.textureViewsForTesting(texture) == initial_views);
        REQUIRE(materials.texturePixelsForTesting(texture) ==
                std::vector<std::uint8_t>{0, 0, 255, 255});
        REQUIRE(materials.materialDescriptorRevisionForTesting(material) == 0);

        writeBytes(path, std::span{green2Png});
        REQUIRE(reload.applyRequestForTesting({key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(materials.textureViewsForTesting(texture) != initial_views);
        REQUIRE(materials.texturePixelsForTesting(texture) ==
                std::vector<std::uint8_t>{0, 255, 0, 255, 0, 255, 0, 255});
        REQUIRE(materials.materialDescriptorRevisionForTesting(material) == 1);
        REQUIRE(materials.textureCountForTesting() >= 1);
        REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 2);

        const auto before_failure = materials.textureViewsForTesting(texture);
        const auto before_pixels = materials.texturePixelsForTesting(texture);
        const std::array<std::uint8_t, 4> broken{0, 1, 2, 3};
        writeBytes(path, std::span{broken});
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(materials.textureViewsForTesting(texture) == before_failure);
        REQUIRE(materials.texturePixelsForTesting(texture) == before_pixels);
        const auto status = reload.transactions().status();
        REQUIRE(status.failed == 1);
        REQUIRE(status.last_reload_error);

        GET_MODULE(VulkanManageCore).waitIdle();
        GET_MODULE(DeletionQueue).flushAll();
        REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 0);
}

TEST_CASE("HR1-T uses watcher gate and survives 1000 same-shape reloads plus KTX2 drift",
          "[wp100][texture-reload][gpu][stress]") {
    setupLogger();
    Sandbox box;
    FastModuleContainer modules;
    configureGpu(box);
    TestSupport::requireVulkanDevice(
        "Vulkan texture reload stress unavailable");
        const auto path = box.root / "stress.png";
        writeBytes(path, std::span{redPng});
        const auto key = watch::makeAssetKey("stress.png");
        auto &materials = GET_MODULE(MaterialContainer);
        const auto texture = materials.registerReloadableTextureFile(key, path);
        const auto material = registerReferencingMaterial(materials, texture);
        auto &reload = GET_MODULE(watch::ReloadService);
        const auto views = materials.textureViewsForTesting(texture);
        const auto texture_count = materials.textureCountForTesting();
        const auto resources = reload.transactions().registry().snapshot().resourceCount();

        for (int i = 0; i < 1000; ++i) {
            writeBytes(path, i % 2 == 0 ? std::span{bluePng} : std::span{redPng});
            REQUIRE(reload.applyRequestForTesting(
                {key, watch::ReloadKind::modified, {}, 1}));
        }
        REQUIRE(materials.textureViewsForTesting(texture) == views);
        REQUIRE(materials.textureCountForTesting() == texture_count);
        REQUIRE(reload.transactions().registry().snapshot().resourceCount() == resources);
        REQUIRE(reload.transactions().registry().snapshot().liveBytes() == 4);
        REQUIRE(materials.materialDescriptorRevisionForTesting(material) == 0);
        REQUIRE(materials.referencingMaterialCountForTesting(texture) == 1);
        REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 0);

        bool gate_enabled = false;
        watch::FileWatcherOptions options;
        options.manual_clock = true;
        options.poll_interval = 10ms;
        options.watch_arm_override = [](const watch::WatchStore &, unsigned) { return false; };
        watch::FileWatcher watcher({{"project", box.root, {}}},
            [&] { return watch::ReloadGateSnapshot{gate_enabled, 7, "fixture"}; }, options);
        const auto t0 = watch::FileWatcher::Clock::now();
        watcher.runControlCycleForTesting(t0);
        writeBytes(path, std::span{bluePng});
        watcher.runControlCycleForTesting(t0 + 20ms);
        REQUIRE(watcher.applyFrame([&](const watch::ReloadRequest &request) {
            return reload.applyRequestForTesting(request);
        }) == 0);
        REQUIRE(materials.textureViewsForTesting(texture) == views);

        // Real-filesystem end-to-end: reconcile the disabled edit as the new
        // baseline, then overwrite the PNG and let FileWatcher drive HR1-T.
        gate_enabled = true;
        watcher.runControlCycleForTesting(t0 + 40ms);
        writeBytes(path, std::span{redPng});
        watcher.runControlCycleForTesting(t0 + 60ms);
        REQUIRE(watcher.applyFrame([&](const watch::ReloadRequest &request) {
            return reload.applyRequestForTesting(request);
        }) >= 1);
        REQUIRE(materials.texturePixelsForTesting(texture) ==
                std::vector<std::uint8_t>{255, 0, 0, 255});
        watcher.stop();

        const auto ktx_path = box.root / "mipped.ktx2";
        const auto ktx_key = watch::makeAssetKey("mipped.ktx2");
        const auto srgb_mips = TestKtx2::makeRgba8Srgb188();
        writeBytes(ktx_path, std::span{srgb_mips});
        const auto ktx_texture = materials.registerReloadableTextureFile(ktx_key, ktx_path);
        REQUIRE(materials.textureMipLevelsForTesting(ktx_texture) == 2);
        const auto old_ktx_views = materials.textureViewsForTesting(ktx_texture);
        const auto unorm_single = TestKtx2::makeRgba8UnormSingleMip();
        writeBytes(ktx_path, std::span{unorm_single});
        REQUIRE(reload.applyRequestForTesting(
            {ktx_key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(materials.textureMipLevelsForTesting(ktx_texture) == 1);
        REQUIRE(materials.textureViewsForTesting(ktx_texture) != old_ktx_views);
        GET_MODULE(VulkanManageCore).waitIdle();
        GET_MODULE(DeletionQueue).flushAll();
        REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 0);
}

} // namespace Pelican
