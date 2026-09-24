// F-3. The fragment shader the Pass Editor's "New..." writes, for 0, 1 and 3
// inputs: it names each input with the engine's own macros, and the engine's
// own compiler accepts it with the engine's own includes. Registered only in
// a build that has the runtime compiler.
#include "fullscreenshadertemplate.hpp"

#include "../src/core/shader/shadercompiler.hpp"

#include <catch2/catch_test_macros.hpp>

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

Pelican::ShaderCompileResult compileFragment(const std::string &source,
                                             const std::string &name) {
    Pelican::ShaderCompiler compiler;
    return compiler.compileSource(source, vk::ShaderStageFlagBits::eFragment,
                                  name);
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

} // namespace PelicanStudio
