#include "../src/project/materiallowering.hpp"
#include "../src/project/gltfmateriallowering.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

std::string readFixture() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                      "surface_format" / "valid" / "wp76.surface";
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

template <typename T>
T readAt(const std::vector<std::byte> &bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

TEST_CASE("surface values use declaration-order std140 offsets", "[material-lowering]") {
    const auto source = readFixture();
    const auto surface = parseSurfaceFormat(source, "wp76.surface");
    REQUIRE(source.substr(surface.code_offset) == surface.code);

    const auto layout = makeSurfaceStd140Layout(surface);
    REQUIRE(layout.size == 64);
    REQUIRE(layout.members.size() == 6);
    REQUIRE(layout.members[0].offset == 0);
    REQUIRE(layout.members[1].offset == 8);
    REQUIRE(layout.members[2].offset == 16);
    REQUIRE(layout.members[3].offset == 28);
    REQUIRE(layout.members[4].offset == 32);
    REQUIRE(layout.members[5].offset == 48);

    const auto bytes = bindSurfaceValues(surface);
    REQUIRE(readAt<float>(bytes, 0) == Catch::Approx(2.0f));
    REQUIRE(readAt<float>(bytes, 28) == Catch::Approx(8.0f));
    REQUIRE(readAt<std::int32_t>(bytes, 32) == 9);
    REQUIRE(readAt<float>(bytes, 48) == Catch::Approx(0.21404114f));
    REQUIRE(readAt<float>(bytes, 52) == Catch::Approx(0.05087609f));
    REQUIRE(readAt<float>(bytes, 56) == Catch::Approx(1.0f));
    REQUIRE(readAt<float>(bytes, 60) == Catch::Approx(0.75f));
}

TEST_CASE("material overrides bind by name without changing declaration layout",
          "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    MaterialDefinition material;
    material.name = "named_override";
    material.tags = {"character", "outline"};
    material.surface = "project://wp76.surface";
    SurfaceParamValue scalar;
    scalar.type = SurfaceParamType::floating;
    scalar.values[0] = 42.0;
    material.values.push_back(MaterialValue{"packed_after_vec3", scalar});
    material.texture_overrides.push_back(
        MaterialTextureOverride{"normal_detail", "project://textures/custom_normal.png"});

    const auto lowered = lowerMaterial(material, surface);
    REQUIRE(lowered.tags ==
            std::vector<std::string>{
                "character", "outline"});
    REQUIRE(readAt<float>(lowered.values, 28) == Catch::Approx(42.0f));
    REQUIRE(readAt<float>(lowered.values, 0) == Catch::Approx(2.0f));
    REQUIRE(lowered.textures.size() == 3);
    REQUIRE(lowered.textures[0].binding == 8);
    REQUIRE(lowered.textures[0].view == LoweredTextureView::srgb);
    REQUIRE(lowered.textures[1].binding == 9);
    REQUIRE(lowered.textures[1].view == LoweredTextureView::unorm);
    REQUIRE(lowered.textures[1].reference ==
            "project://textures/custom_normal.png");
    REQUIRE(lowered.textures[1].missing_default == MaterialDummyTexture::flat_normal);
    REQUIRE(lowered.textures[2].missing_default == MaterialDummyTexture::black);
    REQUIRE(lowered.render_state.blend == SurfaceBlendMode::additive);
    REQUIRE(lowered.render_state.cull == SurfaceCullMode::none);
    REQUIRE(lowered.render_state.depth_test);
    REQUIRE_FALSE(lowered.render_state.depth_write);
    REQUIRE(lowered.render_state.depth_compare == SurfaceDepthCompare::greater_equal);
}

TEST_CASE("material lowering carries texture dimension and sampler contract",
          "[material-lowering][texture][wp209a]") {
    const auto surface = parseSurfaceFormat(
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! textures:\n"
        "//!   - { name: environment, default: "
        "\"project://environment.ktx2\", color_space: linear, "
        "dimension: cube, sampler: { filter: nearest, "
        "mip_filter: nearest, address: clamp_to_edge, "
        "anisotropy: 4 } }\n\n"
        "void pelican_surface_v1("
        "in PelicanSurfaceInputV1 input_data, "
        "inout PelicanSurfaceV1 surface) {}\n",
        "environment.surface");
    MaterialDefinition material;
    material.name = "sky";
    material.surface =
        "project://environment.surface";

    const auto lowered =
        lowerMaterial(material, surface);
    REQUIRE(lowered.textures.size() == 1);
    const auto &texture = lowered.textures.front();
    REQUIRE(texture.dimension ==
            SurfaceTextureDimension::cube);
    REQUIRE(texture.sampler.filter ==
            SurfaceTextureFilter::nearest);
    REQUIRE(texture.sampler.mip_filter ==
            SurfaceTextureFilter::nearest);
    REQUIRE(texture.sampler.address ==
            SurfaceTextureAddressMode::clamp_to_edge);
    REQUIRE(texture.sampler.anisotropy ==
            Catch::Approx(4.0f));
}

TEST_CASE("material sampler resolution is deterministic and names fallbacks",
          "[material-lowering][sampler][wp209a]") {
    SurfaceTextureSampler request;
    request.filter = SurfaceTextureFilter::nearest;
    request.mip_filter =
        SurfaceTextureFilter::nearest;
    request.address =
        SurfaceTextureAddressMode::clamp_to_edge;
    request.anisotropy = 8.0f;

    const auto clamped = resolveMaterialSampler(
        request,
        MaterialSamplerCapabilities{
            .comparison_sampling = true,
            .sampler_anisotropy = true,
            .max_sampler_anisotropy = 4.0f,
        },
        "material texture 'environment'");
    REQUIRE(clamped.filter ==
            SurfaceTextureFilter::nearest);
    REQUIRE(clamped.mip_filter ==
            SurfaceTextureFilter::nearest);
    REQUIRE(clamped.address ==
            SurfaceTextureAddressMode::clamp_to_edge);
    REQUIRE(clamped.anisotropy_enabled);
    REQUIRE(clamped.max_anisotropy ==
            Catch::Approx(4.0f));
    REQUIRE(clamped.resolution ==
            "anisotropy_clamped_to_device_limit");

    const auto disabled = resolveMaterialSampler(
        request,
        MaterialSamplerCapabilities{
            .comparison_sampling = true,
            .sampler_anisotropy = false,
            .max_sampler_anisotropy = 1.0f,
        },
        "material texture 'environment'");
    REQUIRE_FALSE(disabled.anisotropy_enabled);
    REQUIRE(disabled.max_anisotropy ==
            Catch::Approx(1.0f));
    REQUIRE(disabled.resolution ==
            "anisotropy_disabled_feature_unavailable");

    request.anisotropy_fallback =
        SurfaceTextureAnisotropyFallback::reject;
    REQUIRE_THROWS_WITH(
        resolveMaterialSampler(
            request,
            MaterialSamplerCapabilities{
                .comparison_sampling = true,
                .sampler_anisotropy = false,
                .max_sampler_anisotropy = 1.0f,
            },
            "material texture 'environment'"),
        Catch::Matchers::ContainsSubstring("environment") &&
            Catch::Matchers::ContainsSubstring(
                "samplerAnisotropy"));

    request.anisotropy = 1.0f;
    request.compare = SurfaceTextureCompare::less_equal;
    REQUIRE_THROWS_WITH(
        resolveMaterialSampler(
            request,
            MaterialSamplerCapabilities{
                .comparison_sampling = false,
                .sampler_anisotropy = true,
                .max_sampler_anisotropy = 16.0f,
            },
            "material texture 'shadow'"),
        Catch::Matchers::ContainsSubstring("shadow") &&
            Catch::Matchers::ContainsSubstring(
                "comparison sampling"));
}

TEST_CASE("named material variants lower through the ordinary surface and route logic",
          "[material-lowering][material-variant][wp206b]") {
    const auto base_surface = parseSurfaceFormat(
        "//! pelican.surface v1\n"
        "//! language: glsl\n\n"
        "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
        "inout PelicanSurfaceV1 surface) {}\n",
        "project://surfaces/base.surface");
    const auto outline_surface = parseSurfaceFormat(
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! params:\n"
        "//!   - { name: width, type: float, default: 0.01 }\n"
        "//! render_state: { blend: opaque, cull: front, depth: read_only, "
        "depth_compare: less_equal }\n\n"
        "void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) { "
        "vertex.position += vertex.normal * pelican_param_width(); }\n"
        "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
        "in PelicanSurfaceInputV1 input_data) { return vec3(0.02); }\n",
        "project://surfaces/silhouette.surface");

    MaterialDefinition base;
    base.name = "hero";
    base.tags = {"outlined"};
    base.surface = "project://surfaces/base.surface";
    MaterialNamedVariantDefinition variant;
    variant.name = "silhouette";
    variant.surface =
        "project://surfaces/silhouette.surface";
    SurfaceParamValue width;
    width.type = SurfaceParamType::floating;
    width.values[0] = 0.025;
    variant.values.push_back({"width", width});
    base.variants.push_back(variant);

    const MaterialSurfaceCatalog surfaces{
        {*base.surface, base_surface},
        {variant.surface, outline_surface},
    };
    const auto lowered_base =
        lowerMaterial(base, base_surface);
    const auto lowered_variants =
        lowerMaterialVariants(base, surfaces);

    REQUIRE(lowered_base.tags ==
            std::vector<std::string>{"outlined"});
    REQUIRE(lowered_variants.size() == 1);
    REQUIRE(lowered_variants.front().name ==
            "silhouette");
    const auto &lowered =
        lowered_variants.front().material;
    REQUIRE(lowered.name == "hero#silhouette");
    REQUIRE(lowered.tags.empty());
    REQUIRE(lowered.route ==
            MaterialRouteClass::forward_opaque);
    REQUIRE(lowered.render_state.cull ==
            SurfaceCullMode::front);
    REQUIRE_FALSE(lowered.render_state.depth_write);
    REQUIRE(readAt<float>(lowered.values, 0) ==
            Catch::Approx(0.025f));
}

TEST_CASE("type color supports explicit linear encoding without decoding alpha",
          "[material-lowering]") {
    const std::string source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! params:\n"
        "//!   - { name: tint, type: color, default: [0.5, 0.25, 1.0, 0.4], encoding: linear }\n"
        "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    const auto surface = parseSurfaceFormat(source, "linear_color.surface");
    REQUIRE(source.substr(surface.code_offset) == surface.code);
    const auto values = bindSurfaceValues(surface);
    REQUIRE(readAt<float>(values, 0) == Catch::Approx(0.5f));
    REQUIRE(readAt<float>(values, 4) == Catch::Approx(0.25f));
    REQUIRE(readAt<float>(values, 8) == Catch::Approx(1.0f));
    REQUIRE(readAt<float>(values, 12) == Catch::Approx(0.4f));
}

TEST_CASE("lowering capability errors identify the material and requested feature",
          "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    MaterialDefinition material;
    material.name = "too_many_slots";
    material.surface = "project://wp76.surface";
    MaterialLoweringCapabilities capabilities;
    capabilities.max_custom_textures = 2;

    REQUIRE_THROWS_WITH(lowerMaterial(material, surface, capabilities),
                        Catch::Matchers::ContainsSubstring("too_many_slots") &&
                            Catch::Matchers::ContainsSubstring("custom textures"));
}

TEST_CASE("dump includes defines bindings values and render state", "[material-lowering]") {
    const auto surface = parseSurfaceFormat(readFixture(), "wp76.surface");
    const auto text = dumpLoweredMaterial(lowerSurfaceDefaults(surface, "wp76.surface"));
    REQUIRE(text.find("defines: []") != std::string::npos);
    REQUIRE(text.find("binding=6 type=storage_buffer name=MaterialBuffer") != std::string::npos);
    REQUIRE(text.find("binding=8") != std::string::npos);
    REQUIRE(text.find("tint type=color offset=48") != std::string::npos);
    REQUIRE(text.find("blend=additive cull=none") != std::string::npos);
}

TEST_CASE("B surface lowering has a byte-stable public C description golden",
          "[material-lowering][golden]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto source = readText(root / "test" / "fixtures" / "surface_format" / "valid" /
                                 "wp78.surface");
    const auto surface = parseSurfaceFormat(source, "wp78.surface");
    const auto actual = dumpLoweredMaterial(lowerSurfaceDefaults(surface, "wp78.surface"));
    const auto expected = readText(root / "test" / "fixtures" / "material_lowering" /
                                   "wp78_dump.txt");
    REQUIRE(actual == expected);
}

TEST_CASE("screen input materials route forward and reject undefined snapshots by name",
          "[material-lowering][snapshot]") {
    const std::string source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! screen_inputs: [opaque_color]\n"
        "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    const auto surface = parseSurfaceFormat(source, "refract.surface");
    MaterialDefinition material;
    material.name = "glass";
    material.surface = "project://shaders/refract.surface";

    REQUIRE_THROWS_WITH(lowerMaterialWithSnapshots(material, surface, {}),
                        Catch::Matchers::ContainsSubstring("glass") &&
                            Catch::Matchers::ContainsSubstring("opaque_color") &&
                            Catch::Matchers::ContainsSubstring("undefined screen snapshot"));

    const std::array snapshots{std::string{"opaque_color"}};
    const auto lowered = lowerMaterialWithSnapshots(material, surface, snapshots);
    REQUIRE(lowered.screen_inputs == std::vector<std::string>{"opaque_color"});
    REQUIRE(lowered.target_pass == "forward_transparent");
    const auto dump = dumpLoweredMaterial(lowered);
    REQUIRE(dump.find("set=1 binding=0 type=combined_image_sampler name=opaque_color") !=
            std::string::npos);
    REQUIRE(dump.find("target_pass: forward_transparent") != std::string::npos);
}

TEST_CASE("material render path overrides are explicit and preserve exact pass intent",
          "[material-lowering][routing]") {
    const std::string opaque_source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    const auto opaque = parseSurfaceFormat(opaque_source, "opaque.surface");

    MaterialDefinition forward;
    forward.name = "forced_forward";
    forward.render_path = MaterialRenderPath::forward;
    forward.exact_pass = "hero_forward";
    const auto lowered = lowerMaterial(forward, opaque);
    REQUIRE(lowered.route == MaterialRouteClass::forward_opaque);
    REQUIRE(lowered.route_reason == MaterialRouteReason::explicit_forward);
    REQUIRE(lowered.exact_pass == "hero_forward");
    REQUIRE(dumpLoweredMaterial(lowered).find("pass: hero_forward") != std::string::npos);

    const std::string blended_source =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! render_state: { blend: blend, cull: back, depth: read_only }\n"
        "\nvoid pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    MaterialDefinition deferred;
    deferred.name = "invalid_deferred";
    deferred.render_path = MaterialRenderPath::deferred;
    REQUIRE_THROWS_WITH(
        lowerMaterial(deferred, parseSurfaceFormat(blended_source, "blend.surface")),
        Catch::Matchers::ContainsSubstring("invalid_deferred") &&
            Catch::Matchers::ContainsSubstring("incompatible with blending"));
}

TEST_CASE("custom ambient hooks stay on the forward route",
          "[material-lowering][routing]") {
    const auto surface = parseSurfaceFormat(
        "//! pelican.surface v1\n"
        "//! language: glsl\n\n"
        "vec3 pelican_ambient_v1(in PelicanSurfaceV1 surface, vec3 view_direction, "
        "vec3 environment) { return environment; }\n",
        "custom_ambient.surface");
    MaterialDefinition material;
    material.name = "custom_ambient";

    const auto lowered = lowerMaterial(material, surface);
    REQUIRE(lowered.route == MaterialRouteClass::forward_opaque);
    REQUIRE(lowered.route_reason ==
            MaterialRouteReason::automatic_custom_ambient);
    REQUIRE_FALSE(lowered.deferred_eligibility.compatible);
    REQUIRE(lowered.deferred_eligibility.reason == "custom_ambient");

    material.render_path = MaterialRenderPath::deferred;
    REQUIRE_THROWS_WITH(
        lowerMaterial(material, surface),
        Catch::Matchers::ContainsSubstring("custom_ambient") &&
            Catch::Matchers::ContainsSubstring("ambient"));
}

TEST_CASE("OpenPBR base subset routes deferred while extended lobes stay forward",
          "[material-lowering][routing][openpbr]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "src" / "core" / "resources" / "surfaces" / "openpbr";
    const auto opaque_reference =
        std::string{"engine://surfaces/openpbr/opaque_single.surface"};
    const auto opaque = parseSurfaceFormat(readText(root / "opaque_single.surface"),
                                           opaque_reference);

    MaterialDefinition base;
    base.name = "openpbr_base";
    base.surface = opaque_reference;
    base.routing = MaterialVariantRouting{MaterialAlphaMode::opaque, false};
    const auto lowered_base = lowerMaterial(base, opaque);
    REQUIRE(lowered_base.route == MaterialRouteClass::deferred_geometry);
    REQUIRE(lowered_base.route_reason == MaterialRouteReason::automatic_openpbr_base);
    REQUIRE(lowered_base.deferred_eligibility.compatible);
    REQUIRE(lowered_base.deferred_eligibility.model ==
            DeferredMaterialModel::openpbr_base_v1);
    REQUIRE(std::find(lowered_base.defines.begin(), lowered_base.defines.end(),
                      "PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1") !=
            lowered_base.defines.end());

    auto coated = base;
    coated.name = "openpbr_coated";
    SurfaceParamValue coat_weight;
    coat_weight.type = SurfaceParamType::floating;
    coat_weight.values[0] = 0.5;
    coated.values.push_back(MaterialValue{"coat_weight", coat_weight});
    const auto lowered_coated = lowerMaterial(coated, opaque);
    REQUIRE(lowered_coated.route == MaterialRouteClass::forward_opaque);
    REQUIRE(lowered_coated.route_reason ==
            MaterialRouteReason::automatic_custom_lighting);
    REQUIRE_FALSE(lowered_coated.deferred_eligibility.compatible);
    REQUIRE(lowered_coated.deferred_eligibility.reason == "coat_weight_nonzero");
    REQUIRE(std::find(lowered_coated.defines.begin(), lowered_coated.defines.end(),
                      "PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1") ==
            lowered_coated.defines.end());

    const auto blend_reference =
        std::string{"engine://surfaces/openpbr/blend_single.surface"};
    const auto blend = parseSurfaceFormat(readText(root / "blend_single.surface"),
                                          blend_reference);
    MaterialDefinition transparent;
    transparent.name = "openpbr_transparent";
    transparent.surface = blend_reference;
    transparent.routing = MaterialVariantRouting{MaterialAlphaMode::blend, false};
    const auto lowered_transparent = lowerMaterial(transparent, blend);
    REQUIRE(lowered_transparent.route == MaterialRouteClass::forward_transparent);
    REQUIRE(lowered_transparent.route_reason == MaterialRouteReason::automatic_blended);
    REQUIRE_FALSE(lowered_transparent.deferred_eligibility.compatible);
    REQUIRE(lowered_transparent.deferred_eligibility.reason ==
            "blend_requires_forward");
}

TEST_CASE("six routing variants lower only through lighting hook and fixed forward states",
          "[material-lowering][routing]") {
    const std::array alpha_modes{MaterialAlphaMode::opaque, MaterialAlphaMode::mask,
                                 MaterialAlphaMode::blend};
    for (const auto alpha_mode : alpha_modes) {
        for (const bool double_sided : {false, true}) {
            const MaterialVariantRouting routing{alpha_mode, double_sided};
            const auto state = materialVariantRenderState(routing);
            const auto source =
                std::string{"//! pelican.surface v1\n"
                            "//! language: glsl\n"} +
                (alpha_mode == MaterialAlphaMode::mask
                     ? "//! params:\n//!   - { name: alpha_cutoff, type: float, default: 0.5 }\n"
                     : "") +
                std::string{
                            "//! render_state: { blend: "} +
                (state.blend == SurfaceBlendMode::blend ? "blend" : "opaque") +
                ", cull: " + (state.cull == SurfaceCullMode::none ? "none" : "back") +
                ", depth: " + (state.depth_write ? "read_write" : "read_only") +
                " }\n\n"
                "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
                "in PelicanSurfaceInputV1 input_data) { return vec3(1.0); }\n";
            const auto surface = parseSurfaceFormat(source, materialVariantName(routing));
            MaterialDefinition material;
            material.name = std::string{materialVariantName(routing)};
            material.surface = "project://openpbr/" + material.name + ".surface";
            material.routing = routing;

            const auto lowered = lowerMaterial(material, surface);
            REQUIRE(lowered.routing == material.routing);
            REQUIRE(lowered.target_pass ==
                    (alpha_mode == MaterialAlphaMode::blend ? "forward_transparent"
                                                            : "forward_opaque"));
            const auto dump = dumpLoweredMaterial(lowered);
            REQUIRE(dump.find("variant=" + std::string{materialVariantName(routing)}) !=
                    std::string::npos);
            REQUIRE((dump.find("discard=alpha<alpha_cutoff") != std::string::npos) ==
                    (alpha_mode == MaterialAlphaMode::mask));
        }
    }

    const std::string mask_without_cutoff =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! render_state: { blend: opaque, cull: back, depth: read_write }\n\n"
        "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
        "in PelicanSurfaceInputV1 input_data) { return vec3(1.0); }\n";
    MaterialDefinition missing_cutoff;
    missing_cutoff.name = "mask_without_cutoff";
    missing_cutoff.routing = MaterialVariantRouting{MaterialAlphaMode::mask, false};
    REQUIRE_THROWS_WITH(
        lowerMaterial(missing_cutoff,
                      parseSurfaceFormat(mask_without_cutoff, "mask_without_cutoff.surface")),
        Catch::Matchers::ContainsSubstring("mask_without_cutoff") &&
            Catch::Matchers::ContainsSubstring("alpha_cutoff"));

    const std::string wrong_state =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! render_state: { blend: opaque, cull: back, depth: read_write }\n\n"
        "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
        "in PelicanSurfaceInputV1 input_data) { return vec3(1.0); }\n";
    MaterialDefinition bad;
    bad.name = "bad_blend_route";
    bad.routing = MaterialVariantRouting{MaterialAlphaMode::blend, false};
    REQUIRE_THROWS_WITH(lowerMaterial(bad, parseSurfaceFormat(wrong_state, "bad.surface")),
                        Catch::Matchers::ContainsSubstring("bad_blend_route") &&
                            Catch::Matchers::ContainsSubstring("blend_single_sided") &&
                            Catch::Matchers::ContainsSubstring("pipeline state"));
}

TEST_CASE("glTF core materials converge through the six OpenPBR routing wrappers",
          "[material-lowering][gltf][wp240c]") {
    struct Case {
        std::string_view alpha_mode;
        bool double_sided;
        std::string_view surface;
        MaterialRouteClass route;
    };
    constexpr std::array cases{
        Case{"OPAQUE", false, "opaque_single",
             MaterialRouteClass::deferred_geometry},
        Case{"OPAQUE", true, "opaque_double",
             MaterialRouteClass::deferred_geometry},
        Case{"MASK", false, "mask_single",
             MaterialRouteClass::deferred_geometry},
        Case{"MASK", true, "mask_double",
             MaterialRouteClass::deferred_geometry},
        Case{"BLEND", false, "blend_single",
             MaterialRouteClass::forward_transparent},
        Case{"BLEND", true, "blend_double",
             MaterialRouteClass::forward_transparent},
    };
    const auto root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "src" / "core" / "resources" / "surfaces" /
        "openpbr";

    for (const auto &test_case : cases) {
        CAPTURE(std::string{test_case.alpha_mode},
                test_case.double_sided);
        MaterialBase base;
        base.base_color_factor =
            {0.2, 0.4, 0.8, 0.35};
        auto definition =
            makeGltfCoreMaterialDefinition({
                .name = "gltf_source",
                .base = base,
                .alpha_mode =
                    std::string{test_case.alpha_mode},
                .alpha_cutoff = 0.625,
                .double_sided =
                    test_case.double_sided,
            });

        const auto expected_reference =
            "engine://surfaces/openpbr/" +
            std::string{test_case.surface} +
            ".surface";
        REQUIRE(definition.surface ==
                expected_reference);
        REQUIRE(definition.routing.has_value());
        REQUIRE(
            definition.routing->double_sided ==
            test_case.double_sided);
        REQUIRE(definition.base.base_color_factor ==
                base.base_color_factor);
        REQUIRE(definition.defines ==
                std::vector<std::string>{
                    std::string{
                        gltfCoreOpenPbrSurfaceDefine}});

        const auto surface = parseSurfaceFormat(
            readText(
                root /
                (std::string{test_case.surface} +
                 ".surface")),
            expected_reference);
        const auto lowered =
            lowerMaterial(definition, surface);
        REQUIRE(lowered.route ==
                test_case.route);
        REQUIRE(lowered.routing ==
                definition.routing);
        if (test_case.alpha_mode != "BLEND") {
            REQUIRE(
                lowered.deferred_eligibility.model ==
                DeferredMaterialModel::standard_pbr_v1);
            REQUIRE(
                std::find(
                    lowered.defines.begin(),
                    lowered.defines.end(),
                    "PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1") ==
                lowered.defines.end());
        }
        const auto expected_state =
            materialVariantRenderState(
                *definition.routing);
        REQUIRE(lowered.render_state.blend ==
                expected_state.blend);
        REQUIRE(lowered.render_state.cull ==
                expected_state.cull);
        REQUIRE(lowered.render_state.depth_test ==
                expected_state.depth_test);
        REQUIRE(lowered.render_state.depth_write ==
                expected_state.depth_write);
        REQUIRE(lowered.render_state.depth_compare ==
                expected_state.depth_compare);

        const auto cutoff =
            std::find_if(
                lowered.values_layout.members.begin(),
                lowered.values_layout.members.end(),
                [](const auto &member) {
                    return member.name ==
                           "alpha_cutoff";
                });
        REQUIRE(cutoff !=
                lowered.values_layout.members.end());
        REQUIRE(
            readAt<float>(
                lowered.values, cutoff->offset) ==
            Catch::Approx(0.625f));
    }

    REQUIRE_THROWS_WITH(
        makeGltfCoreMaterialDefinition({
            .name = "bad_alpha",
            .alpha_mode = "ADDITIVE",
        }),
        Catch::Matchers::ContainsSubstring(
            "bad_alpha") &&
            Catch::Matchers::ContainsSubstring(
                "ADDITIVE"));
}

} // namespace Pelican
