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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        // The five, and UX-6a's optional shadow mask after them.
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 6);
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
        // UX-6a: the mask's binding, for the node to offer as a socket.
        CHECK(findBinding(reflection, 1, 5) != nullptr);
        CHECK(findBinding(reflection, 1, 6) == nullptr);
    }

    SECTION("six inputs, with the engine's other feature switches on") {
        const auto inputs = gbufferInputs(true);
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(occurrences(text, "\nPELICAN_DECLARE_INPUT_") == 7);
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

    SECTION("UX-5b (RS-10): shadow_map after the G-buffer is the directional "
            "shadow, sampled when the feature defines PELICAN_FEATURE_SHADOW") {
        auto inputs = gbufferInputs(false);
        inputs.push_back({.resource = "shadow_map"});
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(text.find("#define PELICAN_TEMPLATE_SHADOW_MAP in_shadow_map") !=
              std::string::npos);
        // No role reads it: ambient occlusion keeps its constant.
        CHECK(text.find("float ambient_occlusion = 1.0;  // input 5 (ambient "
                        "occlusion): the shadow map is there") != std::string::npos);
        const auto plain = compileFragment(text, "ux5b_lighting_shadow_off.frag");
        INFO(plain.log);
        REQUIRE(plain.ok);
        // Bound whatever the feature says: the pass lists it.
        CHECK(findBinding(Pelican::reflect(plain.spirv), 1, 5) != nullptr);
        const auto shadowed = compileFragment(
            text, "ux5b_lighting_shadow_on.frag",
            {"PELICAN_FEATURE_SHADOW", "PELICAN_INPUT_5_LAYERED=1",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_PCF_RADIUS=1",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_BIAS=0.002",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_STRENGTH=1.0"});
        INFO(shadowed.log);
        REQUIRE(shadowed.ok);
        // The feature's shadow data block is read.
        CHECK(findBinding(Pelican::reflect(shadowed.spirv), 0, 5) != nullptr);
        // And the fill puts it after a whole G-buffer only.
        CHECK(fullscreenTemplateInputFill(Kind::deferred_lighting,
                                          {"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                                           "gbuffer_worldpos", "gbuffer_emissive", "shadow_map"})
                  .back() == "shadow_map");
        CHECK(fullscreenTemplateInputFill(Kind::deferred_lighting,
                                          {"gbuffer_albedo", "shadow_map"}) ==
              std::vector<std::string>{"gbuffer_albedo"});
    }

    SECTION("UX-6a decision 6: the optional shadow mask is the binding after the "
            "inputs, and the key light takes the darker of it and the shadow map") {
        auto inputs = gbufferInputs(false);
        inputs.push_back({.resource = "shadow_map"});
        const auto text = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(text.find("PELICAN_DECLARE_INPUT_6(in_shadow_mask);") != std::string::npos);
        CHECK(text.find("#define PELICAN_TEMPLATE_SHADOW_MASK_READ(uv) "
                        "PELICAN_TEXTURE_2D_6(in_shadow_mask, uv)") != std::string::npos);
        CHECK(text.find("shadow = min(shadow, shadow_mask(inUV));") != std::string::npos);
        const auto shadowed = compileFragment(
            text, "ux6a_lighting_mask.frag",
            {"PELICAN_FEATURE_SHADOW", "PELICAN_INPUT_5_LAYERED=1",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_PCF_RADIUS=1",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_BIAS=0.002",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_STRENGTH=1.0"});
        INFO(shadowed.log);
        REQUIRE(shadowed.ok);
        const auto reflection = Pelican::reflect(shadowed.spirv);
        const auto *mask = findBinding(reflection, 1, 6);
        REQUIRE(mask != nullptr);
        CHECK(mask->type == vk::DescriptorType::eCombinedImageSampler);

        // A pass that already reads a mask (the RT task's, by name): that
        // input is the mask, no role reads it, and nothing more is declared.
        inputs.push_back({.resource = "render_compute__shadow_mask"});
        const auto connected = fullscreenFragmentShaderTemplate(
            "lit", inputs, Kind::deferred_lighting);
        CHECK(occurrences(connected, "\nPELICAN_DECLARE_INPUT_") == 7);
        CHECK(connected.find("#define PELICAN_TEMPLATE_SHADOW_MASK_READ(uv) "
                             "PELICAN_TEXTURE_2D_6(in_render_compute_shadow_mask, uv)") !=
              std::string::npos);
        const auto plain = compileFragment(connected, "ux6a_lighting_mask_connected.frag");
        INFO(plain.log);
        REQUIRE(plain.ok);
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


// --- VI-3b: Fix sockets rewrites a variadic group into raw layout lines, and
// the result still compiles with the engine's includes -- past the eight
// inputs the PELICAN_DECLARE_INPUT_<i> macros stop at. Making the sockets
// variadic again gives the source back byte for byte.

namespace {

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

std::vector<std::string> numberedInputs(std::size_t count) {
    std::vector<std::string> inputs;
    for (std::size_t index = 0; index < count; ++index)
        inputs.push_back("t" + std::to_string(index));
    return inputs;
}

std::string passthroughSource() {
    const auto source = engineFragmentSource(EnginePassthroughFragment);
    REQUIRE(source);
    return std::string{*source};
}

} // namespace

TEST_CASE("VI-3b Studio carries engine://passthrough as the engine's own file",
          "[devstudio][vi3b][shader-template][passthrough-copy]") {
    // The embedded copy is the file the engine compiles, byte for byte.
    const auto engine_file = readFile(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                                      "src" / "core" / "resources" /
                                      "passthrough.frag");
    CHECK(passthroughSource() == engine_file);
    CHECK(passthroughSource().find("PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);") !=
          std::string::npos);
    // Only engine://passthrough is carried.
    CHECK_FALSE(engineFragmentSource("engine://fullscreen"));
    CHECK_FALSE(engineFragmentSource("project://shaders/passthrough"));

    // The copy's name avoids every file already there.
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_vi3b_copy_" + std::to_string(std::rand()));
    std::filesystem::remove_all(root);
    CHECK(engineShaderCopyName(root, "present") == "present");
    const auto first = writeShaderTemplate(root, "present", passthroughSource());
    REQUIRE(first.created);
    CHECK(readFile(first.path) == engine_file);
    CHECK(engineShaderCopyName(root, "present") == "present_2");
    std::ofstream{root / "shaders" / "present_2.frag"} << "// mine\n";
    CHECK(engineShaderCopyName(root, "present") == "present_3");
    std::filesystem::remove_all(root);
}

TEST_CASE("VI-3b fixed sockets are raw layout lines plus the variadic "
          "function and count, and they compile",
          "[devstudio][vi3b][shader-template][fix-sockets]") {
    const auto source = passthroughSource();

    SECTION("three inputs") {
        const auto fixed = fixShaderSockets(
            source, 0, {"gbuffer_albedo", "gbuffer_normal", "lit@history"});
        REQUIRE(fixed.ok());
        const auto &text = fixed.source;
        const std::string eol = source.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        CHECK(text.find("// pelican: fixed sockets, was "
                        "PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);" + eol) !=
              std::string::npos);
        CHECK(text.find("// pelican: end fixed sockets" + eol) != std::string::npos);
        CHECK(occurrences(text, eol + "PELICAN_DECLARE_VARIADIC_INPUTS") == 0);
        CHECK(text.find("layout(set = PELICAN_SET_PASS_INPUT, binding = 0) "
                        "uniform sampler2D in_gbuffer_albedo;") != std::string::npos);
        CHECK(text.find("layout(set = PELICAN_SET_PASS_INPUT, binding = 1) "
                        "uniform sampler2D in_gbuffer_normal;") != std::string::npos);
        CHECK(text.find("layout(set = PELICAN_SET_PASS_INPUT, binding = 2) "
                        "uniform sampler2D in_lit_history;") != std::string::npos);
        CHECK(occurrences(text, "layout(set = PELICAN_SET_PASS_INPUT") == 3);
        CHECK(text.find("vec4 pelican_variadic_texture(int pelican_variadic_index, "
                        "vec2 pelican_variadic_uv) {") != std::string::npos);
        CHECK(text.find("if (pelican_variadic_index == 1) return "
                        "texture(in_gbuffer_normal, pelican_variadic_uv);") !=
              std::string::npos);
        CHECK(text.find("const int pelican_variadic_input_count = 3;") !=
              std::string::npos);
        // The file keeps one kind of line end.
        if (eol == "\r\n")
            CHECK(occurrences(text, "\n") == occurrences(text, "\r\n"));

        const auto compiled = compileFragment(
            text, "vi3b_fixed_3.frag",
            {"PELICAN_PASS_TEXTURE_INPUT_COUNT=3", "PELICAN_PASSTHROUGH_INPUT=1"});
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        const auto reflection = Pelican::reflect(compiled.spirv);
        for (std::uint32_t binding = 0; binding < 3; ++binding) {
            INFO("binding " << binding);
            const auto *input = findBinding(reflection, 1, binding);
            REQUIRE(input != nullptr);
            CHECK(input->type == vk::DescriptorType::eCombinedImageSampler);
            // No variadic member is left for the engine to recognise.
            CHECK(input->name.find("pelican_variadic_input_") == std::string::npos);
        }
        CHECK(findBinding(reflection, 1, 3) == nullptr);

        // Back to variadic: the engine's file again, byte for byte.
        const auto back = variadicShaderSockets(text);
        REQUIRE(back.ok());
        CHECK(back.source == source);
        CHECK(shaderHasFixedSockets(text));
        CHECK_FALSE(shaderHasFixedSockets(source));
    }

    SECTION("ten inputs: past what the fixed input macros reach") {
        const auto fixed = fixShaderSockets(source, 0, numberedInputs(10));
        REQUIRE(fixed.ok());
        CHECK(occurrences(fixed.source, "layout(set = PELICAN_SET_PASS_INPUT") == 10);
        CHECK(fixed.source.find("const int pelican_variadic_input_count = 10;") !=
              std::string::npos);
        const auto compiled = compileFragment(
            fixed.source, "vi3b_fixed_10.frag",
            {"PELICAN_PASS_TEXTURE_INPUT_COUNT=10", "PELICAN_PASSTHROUGH_INPUT=9"});
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        const auto reflection = Pelican::reflect(compiled.spirv);
        REQUIRE(findBinding(reflection, 1, 9) != nullptr);
        CHECK(findBinding(reflection, 1, 9)->name == "in_t9");
        CHECK(variadicShaderSockets(fixed.source).source == source);
    }

    SECTION("a group after fixed inputs, indented, on a file with LF ends and "
            "no final newline") {
        const std::string shader =
            "#version 460\n"
            "#extension GL_GOOGLE_include_directive : require\n"
            "#include \"pelican_sets.glsl\"\n"
            "#include \"pelican_view.glsl\"\n"
            "layout(location = 0) in vec2 inUV;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "PELICAN_DECLARE_INPUT_0(in_base);\n"
            "  PELICAN_DECLARE_VARIADIC_INPUTS(1, 4);  // the layers\n"
            "void main() {\n"
            "    vec4 sum = PELICAN_TEXTURE_2D_0(in_base, inUV);\n"
            "    for (int k = 0; k < PELICAN_VARIADIC_INPUT_COUNT; ++k)\n"
            "        sum += PELICAN_VARIADIC_TEXTURE(k, inUV);\n"
            "    outColor = sum;\n"
            "}";
        // A name the shader already uses is not taken twice.
        const auto fixed = fixShaderSockets(shader, 1, {"base", "layer"}, {"in_base"});
        REQUIRE(fixed.ok());
        CHECK(fixed.source.find('\r') == std::string::npos);
        CHECK(fixed.source.find("  layout(set = PELICAN_SET_PASS_INPUT, binding = 1) "
                                "uniform sampler2D in_base_2;") != std::string::npos);
        CHECK(fixed.source.find("  layout(set = PELICAN_SET_PASS_INPUT, binding = 2) "
                                "uniform sampler2D in_layer;") != std::string::npos);
        CHECK(fixed.source.find("  // pelican: fixed sockets, was "
                                "PELICAN_DECLARE_VARIADIC_INPUTS(1, 4);  // the layers\n") !=
              std::string::npos);
        const auto compiled = compileFragment(fixed.source, "vi3b_fixed_after.frag",
                                              {"PELICAN_PASS_TEXTURE_INPUT_COUNT=3"});
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        CHECK(variadicShaderSockets(fixed.source).source == shader);
        // The control: the variadic original compiles as well.
        CHECK(compileFragment(shader, "vi3b_variadic_after.frag",
                              {"PELICAN_PASS_TEXTURE_INPUT_COUNT=3"})
                  .ok);
    }

    SECTION("refusals: nothing connected, no group, two groups, no block") {
        CHECK(fixShaderSockets(source, 0, {}).refusal ==
              "Connect at least one input before fixing the sockets.");
        const std::string plain =
            "PELICAN_DECLARE_INPUT_0(in_a);\n"
            "// PELICAN_DECLARE_VARIADIC_INPUTS(1, 4);\n";
        CHECK(fixShaderSockets(plain, 0, {"a"}).refusal ==
              "The shader has no PELICAN_DECLARE_VARIADIC_INPUTS line to fix.");
        CHECK(fixShaderSockets("PELICAN_DECLARE_VARIADIC_INPUTS(0, 2);\n"
                               "PELICAN_DECLARE_VARIADIC_INPUTS(2, 2);\n",
                               0, {"a"})
                  .refusal == "The shader has more than one "
                              "PELICAN_DECLARE_VARIADIC_INPUTS line.");
        // A declaration written by hand is never made variadic.
        CHECK_FALSE(shaderHasFixedSockets(plain));
        CHECK(variadicShaderSockets(plain).refusal ==
              "The shader has no sockets Studio fixed.");
    }
}
} // namespace PelicanStudio
