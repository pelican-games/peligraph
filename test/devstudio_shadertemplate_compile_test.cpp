// F-3. The fragment shader the Pass Editor's "New..." writes, for 0, 1 and 3
// inputs, and G-3's deferred lighting and tone map starting points: each
// names its inputs with the engine's own macros, and the engine's own
// compiler accepts it with the engine's own includes. Registered only in
// a build that has the runtime compiler.
#include "fullscreenshadertemplate.hpp"

#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderreflection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

std::size_t occurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

Pelican::ShaderCompileResult compileFragment(
    const std::string &source, const std::string &name,
    std::vector<std::string> defines = {}) {
    Pelican::ShaderCompiler compiler;
    Pelican::ShaderCompileOptions options;
    options.defines = std::move(defines);
    return compiler.compileSource(source, vk::ShaderStageFlagBits::eFragment,
                                  name, options);
}

const Pelican::ReflectedBinding *findBinding(
    const Pelican::ShaderReflection &reflection, std::uint32_t set,
    std::uint32_t binding) {
    const auto found = std::ranges::find_if(
        reflection.bindings, [&](const Pelican::ReflectedBinding &entry) {
            return entry.set == set && entry.binding == binding;
        });
    return found == reflection.bindings.end() ? nullptr : &*found;
}

// A G-buffer as the engine's own lighting pass reads it.
std::vector<FullscreenShaderTemplateInput> gbufferInputs(bool with_ssao) {
    std::vector<FullscreenShaderTemplateInput> inputs{
        {.resource = "gbuffer_albedo"},   {.resource = "gbuffer_normal"},
        {.resource = "gbuffer_material"}, {.resource = "gbuffer_worldpos"},
        {.resource = "g_emissive"}};
    if (with_ssao) inputs.push_back({.resource = "ssao_blur"});
    return inputs;
}

} // namespace

TEST_CASE("F-3 the shader template declares each input with the engine's "
          "macros and compiles",
          "[devstudio][fullscreen-pass][f3][shader-template]") {
    SECTION("no input draws the UV gradient") {
        const auto text = fullscreenFragmentShaderTemplate("tint", {});
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 0);
        CHECK(text.find("outColor = vec4(inUV, 0.0, 1.0);") !=
              std::string::npos);
        CHECK(text.find("#include \"pelican_view.glsl\"") !=
              std::string::npos);
        CHECK(text.find("\"tint\"") != std::string::npos);
        CHECK(text.find('\r') == std::string::npos);
        const auto compiled = compileFragment(text, "f3_template_0.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        CHECK_FALSE(compiled.spirv.empty());
    }

    SECTION("one input is declared at binding 0 and written through") {
        const std::vector<FullscreenShaderTemplateInput> inputs{
            {.resource = "lit_color"}};
        const auto text = fullscreenFragmentShaderTemplate("tint", inputs);
        CHECK(text.find("PELICAN_DECLARE_INPUT_0(in_lit_color);") !=
              std::string::npos);
        CHECK(text.find("vec4 value0 = PELICAN_TEXTURE_2D_0(in_lit_color, "
                        "inUV);") != std::string::npos);
        CHECK(text.find("outColor = value0;") != std::string::npos);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 1);
        const auto compiled = compileFragment(text, "f3_template_1.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
    }

    SECTION("three inputs are declared in list order, a history read "
            "included") {
        const std::vector<FullscreenShaderTemplateInput> inputs{
            {.resource = "lit_color"},
            {.resource = "ssao-blur"},
            {.resource = "lit_color@history"}};
        const auto text = fullscreenFragmentShaderTemplate("mix", inputs);
        CHECK(text.find("PELICAN_DECLARE_INPUT_0(in_lit_color);") !=
              std::string::npos);
        CHECK(text.find("PELICAN_DECLARE_INPUT_1(in_ssao_blur);") !=
              std::string::npos);
        CHECK(text.find("PELICAN_DECLARE_INPUT_2(in_lit_color_history);") !=
              std::string::npos);
        CHECK(text.find("PELICAN_TEXTURE_2D_2(in_lit_color_history, inUV)") !=
              std::string::npos);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 3);
        CHECK(text.find("outColor = value0;") != std::string::npos);
        const auto compiled = compileFragment(text, "f3_template_3.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
    }

    SECTION("a buffer input is named for the author, not declared") {
        const std::vector<FullscreenShaderTemplateInput> inputs{
            {.resource = "light_list", .image = false},
            {.resource = "lit_color"}};
        const auto text = fullscreenFragmentShaderTemplate("mix", inputs);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 1);
        CHECK(text.find("PELICAN_DECLARE_INPUT_1(in_lit_color);") !=
              std::string::npos);
        CHECK(text.find("light_list) is a buffer") != std::string::npos);
        CHECK(text.find("outColor = value1;") != std::string::npos);
        const auto compiled = compileFragment(text, "f3_template_buffer.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
    }
}

TEST_CASE("G-3 the deferred lighting template reads a G-buffer and the "
          "scene lights, and compiles",
          "[devstudio][fullscreen-pass][g3][shader-template]") {
    using Kind = FullscreenShaderTemplateKind;
    const auto settings = fullscreenShaderTemplatePassSettings(
        Kind::deferred_lighting);
    CHECK(settings.uses_light_data);
    CHECK(std::string{settings.push_constants} == "camera_position");
    const auto roles = fullscreenShaderTemplateInputRoles(Kind::deferred_lighting);
    REQUIRE(roles.size() == 6);
    CHECK(std::string{roles[0].name} == "albedo");
    CHECK(std::string{roles[3].name} == "world position");
    CHECK(roles[5].optional);

    SECTION("five inputs: the G-buffer, without ambient occlusion") {
        const auto inputs = gbufferInputs(false);
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(text.find("#include \"pelican_lighting_v1.glsl\"") !=
              std::string::npos);
        CHECK(text.find("pelican_light(index, world_position)") !=
              std::string::npos);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 5);
        CHECK(text.find("PELICAN_DECLARE_INPUT_3(in_gbuffer_worldpos);  // "
                        "gbuffer_worldpos: world position") !=
              std::string::npos);
        CHECK(text.find("vec3 world_position = PELICAN_TEXTURE_2D_3("
                        "in_gbuffer_worldpos, inUV).rgb;") != std::string::npos);
        CHECK(text.find("float ambient_occlusion = 1.0;") != std::string::npos);
        CHECK(text.find('\r') == std::string::npos);
        const auto compiled = compileFragment(text, "g3_lighting_5.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        // What the engine binds for it: the frame's light block and five
        // G-buffer images.
        const auto reflection = Pelican::reflect(compiled.spirv);
        const auto *lights = findBinding(reflection, 0, 2);
        REQUIRE(lights != nullptr);
        CHECK(lights->type == vk::DescriptorType::eUniformBuffer);
        for (std::uint32_t binding = 0; binding < 5; ++binding) {
            INFO("input " << binding);
            const auto *input = findBinding(reflection, 1, binding);
            REQUIRE(input != nullptr);
            CHECK(input->type == vk::DescriptorType::eCombinedImageSampler);
        }
        CHECK(findBinding(reflection, 1, 5) == nullptr);
    }

    SECTION("six inputs, with the engine's other feature switches on") {
        const auto inputs = gbufferInputs(true);
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 6);
        CHECK(text.find("float ambient_occlusion = PELICAN_TEXTURE_2D_5("
                        "in_ssao_blur, inUV).r;") != std::string::npos);
        const auto plain = compileFragment(text, "g3_lighting_6.frag");
        INFO(plain.log);
        REQUIRE(plain.ok);
        // A project with clustered lighting, sky ambient or HDR compiles the
        // same file with those defines; the template does not break there.
        const auto featured = compileFragment(
            text, "g3_lighting_6_features.frag",
            {"PELICAN_FEATURE_CLUSTERED_LIGHTING", "PELICAN_FEATURE_SKY_AMBIENT",
             "PELICAN_FEATURE_HDR"});
        INFO(featured.log);
        REQUIRE(featured.ok);
    }

    SECTION("a missing role is a named constant, so it still compiles") {
        const std::vector<FullscreenShaderTemplateInput> inputs{
            {.resource = "gbuffer_albedo"},
            {.resource = "light_list", .image = false}};
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(text.find("vec3 normal_encoded = vec3(0.5, 0.5, 1.0);  // input 1 "
                        "(normal): not an image") != std::string::npos);
        CHECK(text.find("vec3 world_position = vec3(0.0);  // input 3 (world "
                        "position): not in the input list") != std::string::npos);
        const auto compiled = compileFragment(text, "g3_lighting_missing.frag");
        INFO(compiled.log);
        REQUIRE(compiled.ok);
    }
}

TEST_CASE("G-3 the tone map template reads the scene colour and compiles, "
          "with and without the hdr feature",
          "[devstudio][fullscreen-pass][g3][shader-template]") {
    using Kind = FullscreenShaderTemplateKind;
    CHECK_FALSE(
        fullscreenShaderTemplatePassSettings(Kind::tone_map).uses_light_data);
    const std::vector<FullscreenShaderTemplateInput> inputs{
        {.resource = "lit_color"}};
    const auto text =
        fullscreenFragmentShaderTemplate("present", inputs, Kind::tone_map);
    CHECK(text.find("vec3 scene_color = PELICAN_TEXTURE_2D_0(in_lit_color, "
                    "inUV).rgb;") != std::string::npos);
    CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 1);
    CHECK(text.find("#if defined(PELICAN_FEATURE_HDR)") != std::string::npos);
    for (const bool hdr : {false, true}) {
        INFO("hdr " << hdr);
        const auto compiled = compileFragment(
            text, "g3_tonemap.frag",
            hdr ? std::vector<std::string>{"PELICAN_FEATURE_HDR"}
                : std::vector<std::string>{});
        INFO(compiled.log);
        REQUIRE(compiled.ok);
    }
    // No input yet: a gradient stands in for the scene colour.
    const auto empty =
        fullscreenFragmentShaderTemplate("present", {}, Kind::tone_map);
    CHECK(empty.find("vec3 scene_color = vec3(inUV, 0.5);") !=
          std::string::npos);
    const auto compiled = compileFragment(empty, "g3_tonemap_empty.frag");
    INFO(compiled.log);
    REQUIRE(compiled.ok);
}

TEST_CASE("G-3 the kinds have stable names and the pass-through is F-3's",
          "[devstudio][fullscreen-pass][g3][shader-template]") {
    for (const auto kind : FullscreenShaderTemplateKinds) {
        CHECK(fullscreenShaderTemplateKindFromName(
                  fullscreenShaderTemplateKindName(kind)) == kind);
    }
    CHECK_FALSE(fullscreenShaderTemplateKindFromName("lighting").has_value());
    const std::vector<FullscreenShaderTemplateInput> inputs{
        {.resource = "lit_color"}};
    CHECK(fullscreenFragmentShaderTemplate("tint", inputs) ==
          fullscreenFragmentShaderTemplate(
              "tint", inputs, FullscreenShaderTemplateKind::pass_through));
    CHECK(fullscreenShaderTemplateInputRoles(
              FullscreenShaderTemplateKind::pass_through)
              .empty());
}

} // namespace PelicanStudio
