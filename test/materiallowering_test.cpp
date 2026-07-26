#include "../src/project/materiallowering.hpp"

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
    REQUIRE(lowered.textures[0].binding == 7);
    REQUIRE(lowered.textures[0].view == LoweredTextureView::srgb);
    REQUIRE(lowered.textures[1].binding == 8);
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
    REQUIRE(text.find("binding=7") != std::string::npos);
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

} // namespace Pelican
