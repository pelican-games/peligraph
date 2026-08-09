#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/spvlink.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint32_t> readSpirv(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    const auto bytes = static_cast<std::size_t>(file.tellg());
    if (bytes % sizeof(std::uint32_t) != 0) throw std::runtime_error("unaligned SPIR-V fixture");
    std::vector<std::uint32_t> words(bytes / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(bytes));
    return words;
}

ShaderCompileResult compileFragment(ShaderCompiler &compiler, std::string_view source,
                                    std::string_view name,
                                    std::vector<std::string> defines = {}) {
    ShaderCompileOptions options;
    options.defines = std::move(defines);
    auto result = compiler.compileSource(source, vk::ShaderStageFlagBits::eFragment, name, options);
    INFO(name << ": " << result.log);
    REQUIRE(result.ok);
    return result;
}

std::string spikeTemplateSource() {
    return readText(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "experiments" / "spvlink" /
                    "shaders" / "template.frag.glsl");
}

bool hasBinding(const SpvLinkResult &result, std::uint32_t binding, std::string_view type) {
    return std::any_of(result.bindings.begin(), result.bindings.end(), [&](const auto &item) {
        return item.set == 2 && item.binding == binding && item.descriptor_type == type;
    });
}

} // namespace

TEST_CASE("SPIRV-Tools API linker remaps GLSL descriptors and validates warm/cool variants",
          "[spv-link][corpus][glsl]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto template_module = compileFragment(compiler, spikeTemplateSource(), "template.frag");
    const auto user_source = readText(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                                      "experiments" / "spvlink" / "shaders" / "surface.glsl");
    std::string previous_key;
    for (const auto &defines : {std::vector<std::string>{},
                                std::vector<std::string>{"PELICAN_VARIANT_WARM"}}) {
        const auto user = compileFragment(compiler, user_source, "surface.glsl", defines);
        SpvLinkRequest request;
        request.template_module = template_module.spirv;
        request.user_module = user.spirv;
        request.user_exports = {"pelican_surface"};
        auto linked = linkSpirvModules(request);
        REQUIRE_FALSE(linked.spirv.empty());
        REQUIRE(hasBinding(linked, 8, "combined_image_sampler"));
        REQUIRE(linked.cache_key.find("key-sha256=") != std::string::npos);
        if (!previous_key.empty()) REQUIRE(linked.cache_key != previous_key);
        previous_key = linked.cache_key;
    }
#endif
}

TEST_CASE("SPV link corpus covers struct control flow derivative discard and multiple hooks",
          "[spv-link][corpus][glsl]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr std::string_view template_source = R"glsl(#version 460
struct Surface { vec4 color; vec2 uv; };
layout(location=0) out vec4 outColor;
void pelican_surface(inout Surface s) {}
vec3 pelican_brdf(Surface s, vec3 radiance) { return vec3(0.0); }
void main() { Surface s; s.color=vec4(1.0); s.uv=vec2(0.0); pelican_surface(s);
              outColor=vec4(pelican_brdf(s, vec3(1.0)), 1.0); }
)glsl";
    constexpr std::string_view user_source = R"glsl(#version 460
struct Surface { vec4 color; vec2 uv; };
layout(location=0) out vec4 dummyOut;
void pelican_surface(inout Surface s) {
  for (int i=0; i<3; ++i) { if (i == 2) s.color.rgb += vec3(dFdx(s.uv.x)); }
  if (s.color.a < 0.0) discard;
}
vec3 pelican_brdf(Surface s, vec3 radiance) {
  return s.color.rgb * max(radiance, vec3(0.0));
}
void main() { Surface s; s.color=vec4(1.0); s.uv=vec2(0.0); pelican_surface(s);
              dummyOut=vec4(pelican_brdf(s, vec3(1.0)), 1.0); }
)glsl";
    ShaderCompiler compiler;
    const auto templ = compileFragment(compiler, template_source, "corpus_template.frag");
    const auto user = compileFragment(compiler, user_source, "corpus_user.frag");
    SpvLinkRequest request;
    request.template_module = templ.spirv;
    request.user_module = user.spirv;
    request.user_exports = {"pelican_surface", "pelican_brdf"};
    REQUIRE_NOTHROW(linkSpirvModules(request));
#endif
}

TEST_CASE("SPV link ABI rejects arrays matrices and resource handles by symbol name",
          "[spv-link][abi]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    struct Case { const char *name; const char *field; };
    for (const Case item : {Case{"matrix", "mat4 forbidden;"},
                            Case{"array", "vec4 forbidden[2];"}}) {
        DYNAMIC_SECTION(item.name) {
            const auto source = std::string{"#version 460\nstruct Bad { "} + item.field +
                " }; layout(location=0) out vec4 color; "
                "void pelican_hook(inout Bad value) {} "
                "void main(){ Bad value; pelican_hook(value); color=vec4(0.0); }";
            ShaderCompiler compiler;
            const auto templ = compileFragment(compiler, source, std::string{"bad_template_"} + item.name);
            const auto user = compileFragment(compiler, source, std::string{"bad_user_"} + item.name);
            SpvLinkRequest request;
            request.template_module = templ.spirv;
            request.user_module = user.spirv;
            request.user_exports = {"pelican_hook"};
            REQUIRE_THROWS_WITH(linkSpirvModules(request),
                                Catch::Matchers::ContainsSubstring("pelican_hook") &&
                                Catch::Matchers::ContainsSubstring("forbidden type"));
        }
    }
    SECTION("resource handle") {
        constexpr std::string_view source = R"glsl(#version 460
layout(set=2,binding=0) uniform sampler2D forbidden;
layout(location=0) out vec4 color;
void pelican_hook(sampler2D value) {}
void main(){ pelican_hook(forbidden); color=vec4(0.0); }
)glsl";
        ShaderCompiler compiler;
        const auto templ = compileFragment(compiler, source, "bad_template_resource");
        const auto user = compileFragment(compiler, source, "bad_user_resource");
        SpvLinkRequest request;
        request.template_module = templ.spirv;
        request.user_module = user.spirv;
        request.user_exports = {"pelican_hook"};
        REQUIRE_THROWS_WITH(linkSpirvModules(request),
                            Catch::Matchers::ContainsSubstring("pelican_hook") &&
                            Catch::Matchers::ContainsSubstring("forbidden type"));
    }
#endif
}

#if PELICAN_RUNTIME_SHADER_COMPILER && defined(PELICAN_TEST_SLANG_COOL_FIXTURE) && \
    defined(PELICAN_TEST_SLANG_WARM_FIXTURE)
TEST_CASE("SPV link accepts noinline Slang split texture sampler variant corpus",
          "[spv-link][corpus][slang]") {
    ShaderCompiler compiler;
    const auto template_module = compileFragment(compiler, spikeTemplateSource(), "template_slang.frag");
    std::string previous_key;
    for (const auto path : {PELICAN_TEST_SLANG_COOL_FIXTURE, PELICAN_TEST_SLANG_WARM_FIXTURE}) {
        const auto user = readSpirv(path);
        SpvLinkRequest request;
        request.template_module = template_module.spirv;
        request.user_module = user;
        request.user_exports = {"pelican_surface"};
        const auto linked = linkSpirvModules(request);
        REQUIRE_FALSE(linked.spirv.empty());
        REQUIRE(hasBinding(
            linked,
            PELICAN_MATERIAL_CUSTOM_TEXTURE_FIRST_BINDING,
            "sampled_image"));
        REQUIRE(hasBinding(
            linked,
            PELICAN_MATERIAL_CUSTOM_TEXTURE_FIRST_BINDING + 1,
            "sampler"));
        if (!previous_key.empty()) REQUIRE(linked.cache_key != previous_key);
        previous_key = linked.cache_key;
    }
}
#endif

} // namespace Pelican
