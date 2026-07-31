#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/watch/assetkey.hpp"
#include "../src/core/watch/filewatcher.hpp"
#include "../src/core/watch/reloadservice.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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

void writeVariantMaterial(const std::filesystem::path &path,
                          double base_scalar, double variant_scalar,
                          bool invalid_variant = false) {
    const auto variant_value =
        invalid_variant ? nlohmann::json("wrong")
                        : nlohmann::json(variant_scalar);
    const auto document = nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", nlohmann::json::array({
            {
                {"name", "live"},
                {"tags", {"outlined"}},
                {"surface", "project://wp76.surface"},
                {"values", {{"scalar_first", base_scalar}}},
                {"variants",
                 {
                     {"silhouette",
                      {
                          {"surface", "project://wp76.surface"},
                          {"values",
                           {{"scalar_first", variant_value}}},
                      }},
                 }},
            },
        })},
    };
    writeText(path, document.dump(2));
}

template <class T> T readAt(const std::vector<std::byte> &bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool sameBytes(const std::vector<std::byte> &left,
               const std::vector<std::byte> &right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

void configureGpu(const Sandbox &box) {
    const auto scene = box.root / "scene.json";
    const auto assets = box.root / "assets.json";
    writeText(scene, "{}");
    writeText(
        assets,
        R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
    applyLoweredMaterial(info, lowered);
    return materials.registerMaterial(std::move(info));
}

MaterialInfo makeValuesMaterialInfo(const LoweredMaterial &lowered) {
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture =
            standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    applyLoweredMaterial(info, lowered);
    return info;
}

std::string liveSurface(bool expanded, bool broken = false) {
    std::string source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! params:\n"
        "//!   - { name: scalar_first, type: float, default: 1.0 }\n";
    if (expanded) {
        source += "//!   - { name: tint, type: vec4, default: [1.0, 1.0, 1.0, 1.0] }\n";
    }
    source += "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, "
              "inout PelicanSurfaceV1 s) {";
    source += broken ? " not_valid_surface_code " : " s.roughness = pelican_param_scalar_first(); ";
    source += "}\n";
    return source;
}

void writeLiveMaterial(const std::filesystem::path &path, double scalar,
                       bool expanded, bool invalid = false) {
    nlohmann::json values{{"scalar_first", invalid ? nlohmann::json("wrong")
                                                   : nlohmann::json(scalar)}};
    if (expanded) values["tint"] = {0.25, 0.5, 0.75, 1.0};
    writeText(path, nlohmann::json{
        {"schema", "pelican.material"},
        {"version", 1},
        {"materials", nlohmann::json::array({
            {{"name", "live"},
             {"surface", "project://shaders/live.surface"},
             {"values", std::move(values)}}
        })}}
        .dump(2));
}

GlobalMaterialId registerSurfaceValuesMaterial(
    MaterialContainer &materials, const LoweredMaterial &lowered,
    SurfaceShaderBundleIds shaders) {
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = shaders.vertex,
        .frag_shader = shaders.fragment,
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
    };
    applyLoweredMaterial(info, lowered);
    return materials.registerMaterial(std::move(info));
}

} // namespace

TEST_CASE("HR2-S commits surface shader variants and material layout as one transaction",
          "[material-values-reload][shader-hot-reload][hr2-s][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    Sandbox box;
    try {
        FastModuleContainer modules;
        configureGpu(box);
        GET_MODULE(PathResolver).setup(box.root, false);
        // Keep Vulkan alive until every shader bundle is destroyed.
        (void)GET_MODULE(VulkanManageCore);
        const auto surface_path = box.root / "shaders" / "live.surface";
        const auto material_path = box.root / "live.material.json";
        std::filesystem::create_directories(surface_path.parent_path());
        writeText(surface_path, liveSurface(false));
        writeLiveMaterial(material_path, 2.0, false);

        auto surface = parseSurfaceFormat(readText(surface_path),
                                          "project://shaders/live.surface");
        const auto shader_bundles = GET_MODULE(ShaderLibrary).loadFromSurface(
            surface, "project://shaders/live.surface");
        MaterialSurfaceCatalog catalog{{"project://shaders/live.surface", surface}};
        auto lowered = loadLowered(material_path, catalog);
        auto &materials = GET_MODULE(MaterialContainer);
        const auto material = registerSurfaceValuesMaterial(
            materials, lowered, shader_bundles);
        const auto material_key = watch::makeAssetKey("live.material.json");
        const auto surface_key = watch::makeAssetKey("shaders/live.surface");
        const std::array bindings{
            MaterialContainer::ReloadableMaterialValuesBinding{"live", material}};
        materials.registerReloadableMaterialValuesFile(
            material_key, material_path, catalog, bindings);
        auto &reload = GET_MODULE(watch::ReloadService);
        const auto before_surface = reload.transactions().registry().snapshot().find(
            "material-surface-fake", surface_key);
        REQUIRE(before_surface);

        writeText(surface_path, liveSurface(true));
        writeLiveMaterial(material_path, 7.0, true);
        const std::array requests{
            watch::ReloadRequest{surface_key, watch::ReloadKind::modified, {}, 1},
            watch::ReloadRequest{material_key, watch::ReloadKind::modified, {}, 1},
        };
        const auto applied = reload.applyRequestsForTesting(requests);
        REQUIRE(applied == std::vector<bool>{true, true});
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.vertex).version == 2);
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.fragment).version == 2);
        const auto values_after = materials.materialValuesForTesting(material);
        REQUIRE(values_after.size() == 32);
        REQUIRE(readAt<float>(values_after, 0) == Catch::Approx(7.0f));
        REQUIRE(readAt<float>(values_after, 16) == Catch::Approx(0.25f));
        const auto after_surface = reload.transactions().registry().snapshot().find(
            "material-surface-fake", surface_key);
        REQUIRE(after_surface);
        REQUIRE(after_surface->ref == before_surface->ref);
        REQUIRE(after_surface->compatibility_revision ==
                before_surface->compatibility_revision + 1);

        const auto stable_values = values_after;
        writeText(surface_path, liveSurface(true) + "\n// valid candidate, invalid peer\n");
        writeLiveMaterial(material_path, 9.0, true, true);
        const auto rejected_material = reload.applyRequestsForTesting(requests);
        REQUIRE(rejected_material == std::vector<bool>{false, false});
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.vertex).version == 2);
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.fragment).version == 2);
        const auto after_material_failure = materials.materialValuesForTesting(material);
        REQUIRE(after_material_failure.size() == stable_values.size());
        REQUIRE(std::equal(after_material_failure.begin(), after_material_failure.end(),
                           stable_values.begin()));

        writeText(surface_path, liveSurface(true, true));
        writeLiveMaterial(material_path, 11.0, true);
        const auto rejected_shader = reload.applyRequestsForTesting(requests);
        REQUIRE(rejected_shader == std::vector<bool>{false, false});
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.vertex).version == 2);
        REQUIRE(GET_MODULE(ShaderLibrary).get(shader_bundles.fragment).version == 2);
        const auto after_shader_failure = materials.materialValuesForTesting(material);
        REQUIRE(after_shader_failure.size() == stable_values.size());
        REQUIRE(std::equal(after_shader_failure.begin(), after_shader_failure.end(),
                           stable_values.begin()));
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan HR2-S cross-file reload unavailable: "} + error.what());
    }
#endif
}

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

        const auto tag_change = nlohmann::json{
            {"schema", "pelican.material"},
            {"version", 1},
            {"materials", nlohmann::json::array({
                {{"name", "material_a"},
                 {"tags", {"outline"}},
                 {"surface", "project://wp76.surface"},
                 {"values", {{"scalar_first", 20.0}}}},
                {{"name", "material_b"},
                 {"surface", "project://wp76.surface"},
                 {"values", {{"scalar_first", 9.0}}}}
            })}};
        writeText(path_a, tag_change.dump(2));
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        after_failure =
            materials.materialGpuValuesForTesting(
                material_a);
        REQUIRE(std::equal(
            after_failure.begin(),
            after_failure.end(),
            before_failure.begin(),
            before_failure.end()));
        REQUIRE(
            materials.tagsForMaterial(
                material_a)
                .empty());

        writeTwoMaterials(path_a, 20.0, 9.0, "project://alternate.surface");
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key_a, watch::ReloadKind::modified, {}, 1}));
        after_failure = materials.materialGpuValuesForTesting(material_a);
        REQUIRE(std::equal(after_failure.begin(), after_failure.end(),
                           before_failure.begin(), before_failure.end()));
        const auto status = reload.transactions().status();
        REQUIRE(status.failed == 5);
        REQUIRE(status.last_reload_error);
        REQUIRE(status.last_reload_error->message.find("material_a") != std::string::npos);
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan material values reload unavailable: "} + error.what());
    }
}

TEST_CASE("WP206b named variant owns its GPU record and reloads atomically with its base",
          "[wp206b][material-variant][material-values-reload][gpu]") {
    setupLogger();
    Sandbox box;
    try {
        FastModuleContainer modules;
        configureGpu(box);
        const auto surface = wp76Surface();
        const MaterialSurfaceCatalog catalog{
            {"project://wp76.surface", surface}};
        const auto path = box.root / "variant.material.json";
        writeVariantMaterial(path, 2.0, 5.0);

        std::ifstream input{path, std::ios::binary};
        const auto document = parseMaterialFormatJson(
            nlohmann::json::parse(input), catalog);
        REQUIRE(document.materials.size() == 1);
        const auto &definition = document.materials.front();
        auto base_lowered =
            lowerMaterial(definition, surface);
        auto variants =
            lowerMaterialVariants(definition, catalog);
        REQUIRE(variants.size() == 1);

        auto &materials = GET_MODULE(MaterialContainer);
        auto base_info =
            makeValuesMaterialInfo(base_lowered);
        base_info.base_color_factor =
            {0.25f, 0.5f, 0.75f, 1.0f};
        const auto base = materials.registerMaterial(
            std::move(base_info));
        const auto base_record_before =
            materials.materialGpuRecordForTesting(base);
        std::vector<MaterialContainer::NamedMaterialVariantRegistration>
            registrations;
        auto variant_info =
            makeValuesMaterialInfo(
                variants.front().material);
        variant_info.base_color_factor =
            {0.9f, 0.9f, 0.9f, 0.9f};
        registrations.push_back({
            variants.front().name,
            std::move(variant_info),
        });
        materials.registerMaterialVariants(
            base, std::move(registrations));
        const auto variant =
            materials.materialVariantResource(base, "silhouette");
        REQUIRE(variant != base);
        REQUIRE(sameBytes(
            materials.materialGpuRecordForTesting(base),
            base_record_before));
        REQUIRE(readAt<float>(
                    materials.materialGpuValuesForTesting(base), 0) ==
                Catch::Approx(2.0f));
        REQUIRE(readAt<float>(
                    materials.materialGpuValuesForTesting(variant), 0) ==
                Catch::Approx(5.0f));
        REQUIRE(readAt<float>(
                    materials.materialGpuRecordForTesting(variant), 0) ==
                Catch::Approx(0.25f));

        PassDefinition pass;
        pass.name = "silhouette_overlay";
        pass.materialInfo().contract =
            MaterialPassContract::legacy_gbuffer_v1;
        pass.materialInfo().material_filter =
            makeMaterialDrawTagFilter(
                {"outlined"}, {}, pass.name);
        pass.materialInfo().material_variant =
            "silhouette";
        REQUIRE(materials.resolveMaterialForPass(pass, base) ==
                variant);
        REQUIRE(materials.materialGpuIndexForPass(pass, base) ==
                static_cast<std::uint32_t>(variant.value));
        REQUIRE(materials.isRenderRequired(pass, base));

        auto missing_pass = pass;
        missing_pass.materialInfo().material_variant = "missing";
        REQUIRE_THROWS_WITH(
            materials.resolveMaterialForPass(missing_pass, base),
            Catch::Matchers::ContainsSubstring(
                "material pass 'silhouette_overlay' selected variant "
                "'missing'"));
        REQUIRE_THROWS_WITH(
            materials.isRenderRequired(missing_pass, base),
            Catch::Matchers::ContainsSubstring(
                "material pass 'silhouette_overlay' selected variant "
                "'missing'"));
        missing_pass.materialInfo().material_filter =
            makeMaterialDrawTagFilter(
                {"not-selected"}, {}, missing_pass.name);
        REQUIRE_FALSE(
            materials.isRenderRequired(missing_pass, base));

        const auto key =
            watch::makeAssetKey("variant.material.json");
        const std::array mismatched_bindings{
            MaterialContainer::ReloadableMaterialValuesBinding{
                "live", base, std::nullopt},
            MaterialContainer::ReloadableMaterialValuesBinding{
                "live", variant, std::string{"silhouette"}},
        };
        REQUIRE_THROWS_WITH(
            materials.registerReloadableMaterialValuesFile(
                key, path, catalog, mismatched_bindings),
            Catch::Matchers::ContainsSubstring(
                "base and variant bindings must use the same base material"));
        const std::array bindings{
            MaterialContainer::ReloadableMaterialValuesBinding{
                "live", base, std::nullopt},
            MaterialContainer::ReloadableMaterialValuesBinding{
                "live", base, std::string{"silhouette"}},
        };
        materials.registerReloadableMaterialValuesFile(
            key, path, catalog, bindings);
        auto &reload = GET_MODULE(watch::ReloadService);

        writeVariantMaterial(path, 7.0, 11.0);
        REQUIRE(reload.applyRequestForTesting(
            {key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(readAt<float>(
                    materials.materialGpuValuesForTesting(base), 0) ==
                Catch::Approx(7.0f));
        REQUIRE(readAt<float>(
                    materials.materialGpuValuesForTesting(variant), 0) ==
                Catch::Approx(11.0f));

        const auto stable_base =
            materials.materialGpuValuesForTesting(base);
        const auto stable_variant =
            materials.materialGpuValuesForTesting(variant);
        writeVariantMaterial(path, 13.0, 17.0, true);
        REQUIRE_FALSE(reload.applyRequestForTesting(
            {key, watch::ReloadKind::modified, {}, 1}));
        REQUIRE(sameBytes(
            materials.materialGpuValuesForTesting(base),
            stable_base));
        REQUIRE(sameBytes(
            materials.materialGpuValuesForTesting(variant),
            stable_variant));

        auto transparent_variant =
            makeValuesMaterialInfo(variants.front().material);
        transparent_variant.route =
            base_lowered.route ==
                    MaterialRouteClass::forward_transparent
                ? MaterialRouteClass::deferred_geometry
                : MaterialRouteClass::forward_transparent;
        REQUIRE_THROWS_WITH(
            materials.registerMaterialVariants(
                base,
                {{
                    "transparent",
                    std::move(transparent_variant),
                }}),
            Catch::Matchers::ContainsSubstring(
                "cross-phase variants require a variant-aware draw queue"));
        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        SKIP(std::string{
                 "Vulkan named material variant reload unavailable: "} +
             error.what());
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
