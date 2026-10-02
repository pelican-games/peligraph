#include <catch2/catch_test_macros.hpp>

#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/project/sha256stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace Pelican {

namespace {
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pelican_shader_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

constexpr std::string_view vertexSource = R"glsl(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)glsl";

// Sets (or, with an empty value, removes) PELICAN_SHADER_CACHE_DIR for one
// test and restores what the process had.
class ScopedSharedCacheDirectory {
    std::optional<std::string> previous;

    static void set(const std::string &value) {
#ifdef _WIN32
        (void)_putenv_s(shaderCacheDirectoryEnvironment, value.c_str());
#else
        if (value.empty()) unsetenv(shaderCacheDirectoryEnvironment);
        else setenv(shaderCacheDirectoryEnvironment, value.c_str(), 1);
#endif
    }

  public:
    explicit ScopedSharedCacheDirectory(const std::string &value) {
        if (const char *current = std::getenv(shaderCacheDirectoryEnvironment)) previous = current;
        set(value);
    }
    ~ScopedSharedCacheDirectory() { set(previous.value_or("")); }
};

void writeFile(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

// A project with a shader that includes a file next to it.
void writeIncludingProject(const std::filesystem::path &root, std::string_view constant) {
    writeFile(root / "shaders" / "common.glsl",
              std::string{"const float kTint = "} + std::string{constant} + ";\n");
    writeFile(root / "shaders" / "tint.vert", R"glsl(#version 450
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
void main() { gl_Position = vec4(kTint, 0.0, 0.0, 1.0); }
)glsl");
}

// Compiles a project's file as an engine does: the project is the
// PathResolver root, the compiler is built after it.
ShaderCompileResult compileInProject(const std::filesystem::path &project,
                                     const std::filesystem::path &shader,
                                     std::optional<std::filesystem::path> *cache_directory = nullptr) {
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(project, false);
    ShaderCompiler compiler;
    if (cache_directory != nullptr) *cache_directory = compiler.cacheDirectory();
    return compiler.compileFile(shader);
}
} // namespace

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("shader disk cache is byte-identical and recovers from corruption", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto cache = temp.path / "cache";
    ShaderCompiler compiler;
    compiler.setCacheDirectory(cache);

    const auto first = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex, "same.vert");
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.cache_hit);
    REQUIRE_FALSE(first.cache_key.empty());

    const auto second = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex, "same.vert");
    REQUIRE(second.ok);
    REQUIRE(second.cache_hit);
    REQUIRE(second.cache_key == first.cache_key);
    REQUIRE(second.spirv == first.spirv);

    const auto entry = cache / (first.cache_key + ".spv-cache");
    {
        std::ofstream corrupt{entry, std::ios::binary | std::ios::trunc};
        corrupt << "corrupt";
    }
    ShaderCompiler recovery_compiler;
    recovery_compiler.setCacheDirectory(cache);
    const auto recovered = recovery_compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                                           "same.vert");
    REQUIRE(recovered.ok);
    REQUIRE_FALSE(recovered.cache_hit);
    REQUIRE(recovered.spirv == first.spirv);
    REQUIRE(std::filesystem::file_size(entry) > 7);
}

TEST_CASE("shader cache key covers defines and transitive source bytes", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto include = temp.path / "common.glsl";
    {
        std::ofstream file{include};
        file << "const float kValue = 0.0;\n";
    }
    const std::string source = R"glsl(#version 450
#include "common.glsl"
void main() { gl_Position = vec4(kValue, 0.0, 0.0, 1.0); }
)glsl";

    ShaderCompiler compiler;
    compiler.addIncludeDir(temp.path);
    compiler.setCacheDirectory(temp.path / "cache");
    const auto base = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "include.vert");
    REQUIRE(base.ok);

    ShaderCompileOptions defined_options;
    defined_options.defines = {"PELICAN_FEATURE_TAA_ALPHA=0.100000001"};
    const auto defined = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex,
                                                "include.vert", defined_options);
    REQUIRE(defined.ok);
    REQUIRE(defined.cache_key != base.cache_key);

    ShaderCompileOptions changed_value_options;
    changed_value_options.defines = {"PELICAN_FEATURE_TAA_ALPHA=0.200000003"};
    const auto changed_value = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex,
                                                      "include.vert", changed_value_options);
    REQUIRE(changed_value.ok);
    REQUIRE(changed_value.cache_key != defined.cache_key);

    {
        std::ofstream file{include, std::ios::trunc};
        file << "const float kValue = 1.0;\n";
    }
    const auto changed = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "include.vert");
    REQUIRE(changed.ok);
    REQUIRE_FALSE(changed.cache_hit);
    REQUIRE(changed.cache_key != base.cache_key);

    const auto absolute_source = std::string{"#version 450\n#include \""} +
        include.generic_string() +
        "\"\nvoid main() { gl_Position = vec4(kValue, 0.0, 0.0, 1.0); }\n";
    ShaderCompiler absolute_compiler;
    absolute_compiler.setCacheDirectory(temp.path / "absolute-cache");
    const auto absolute_first = absolute_compiler.compileSource(
        absolute_source, vk::ShaderStageFlagBits::eVertex, "absolute.vert");
    REQUIRE(absolute_first.ok);
    {
        std::ofstream file{include, std::ios::trunc};
        file << "const float kValue = 2.0;\n";
    }
    const auto absolute_changed = absolute_compiler.compileSource(
        absolute_source, vk::ShaderStageFlagBits::eVertex, "absolute.vert");
    REQUIRE(absolute_changed.ok);
    REQUIRE_FALSE(absolute_changed.cache_hit);
    REQUIRE(absolute_changed.cache_key != absolute_first.cache_key);
}

TEST_CASE("unwritable shader cache never prevents compilation", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto not_a_directory = temp.path / "regular-file";
    {
        std::ofstream file{not_a_directory};
        file << "occupied";
    }
    ShaderCompiler compiler;
    compiler.setCacheDirectory(not_a_directory);
    const auto result = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                               "fallback.vert");
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.cache_hit);
    REQUIRE_FALSE(result.spirv.empty());
}

TEST_CASE("TS-1 a shared shader cache directory serves an engine shader to a second project",
          "[shader][startup][cache][ts1]") {
    TempDirectory temp;
    const auto shared = temp.path / "shared";
    const ScopedSharedCacheDirectory environment{shared.string()};
    const auto source = engineResourceOrThrow("fullscreen.vert");

    const auto compile = [&](const std::filesystem::path &project) {
        std::filesystem::create_directories(project);
        FastModuleContainer modules;
        GET_MODULE(PathResolver).setup(project, false);
        ShaderCompiler compiler;
        REQUIRE(compiler.cacheDirectory() == std::filesystem::weakly_canonical(shared));
        return compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "engine://fullscreen.vert");
    };
    const auto first = compile(temp.path / "first_project");
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.cache_hit);
    const auto second = compile(temp.path / "second_project");
    REQUIRE(second.ok);
    CHECK(second.cache_hit);
    CHECK(second.log == "shader disk cache hit");
    CHECK(second.cache_key == first.cache_key);
    CHECK(second.spirv == first.spirv);
    CHECK(std::filesystem::exists(shared / (first.cache_key + ".spv-cache")));
    CHECK_FALSE(std::filesystem::exists(temp.path / "first_project" / ".pelican"));
    CHECK_FALSE(std::filesystem::exists(temp.path / "second_project" / ".pelican"));

    // The variable moves a project's cache; it gives none to a compiler that
    // has no project.
    FastModuleContainer modules;
    CHECK_FALSE(ShaderCompiler{}.cacheDirectory().has_value());
}

TEST_CASE("TS-1 without the shared cache variable a project keeps its own shader cache",
          "[shader][startup][cache][ts1]") {
    TempDirectory temp;
    const ScopedSharedCacheDirectory environment{""};
    const auto project = temp.path / "project";
    std::filesystem::create_directories(project);
    FastModuleContainer modules;
    auto &resolver = GET_MODULE(PathResolver);
    resolver.setup(project, false);
    ShaderCompiler compiler;
    const auto expected = resolver.projectRoot() / ".pelican" / "shader_cache";
    REQUIRE(compiler.cacheDirectory() == expected);
    const auto result = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex, "project.vert");
    REQUIRE(result.ok);
    CHECK(std::filesystem::exists(expected / (result.cache_key + ".spv-cache")));
}

TEST_CASE("TS-1 a project shader shares its key across project locations and misses on an include byte",
          "[shader][startup][cache][ts1]") {
    TempDirectory temp;
    const auto shared = temp.path / "shared";
    const ScopedSharedCacheDirectory environment{shared.string()};
    const auto first_project = temp.path / "a" / "first";
    const auto second_project = temp.path / "b" / "second_project";
    writeIncludingProject(first_project, "0.25");
    writeIncludingProject(second_project, "0.25");

    const auto first = compileInProject(first_project, first_project / "shaders" / "tint.vert");
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.cache_hit);
    const auto second = compileInProject(second_project, second_project / "shaders" / "tint.vert");
    REQUIRE(second.ok);
    CHECK(second.cache_hit);
    CHECK(second.cache_key == first.cache_key);
    // The cached SPIR-V is what the second project compiles to by itself.
    auto uncached = ShaderCompiler::detached();
    const auto own = uncached.compileFile(second_project / "shaders" / "tint.vert");
    REQUIRE(own.ok);
    CHECK(own.spirv == second.spirv);
    // Hot reload still learns the second project's own files.
    CHECK(std::ranges::find(second.dependencies,
                            std::filesystem::weakly_canonical(second_project / "shaders" / "common.glsl")) !=
          second.dependencies.end());

    // One byte of the include changes the key.
    writeFile(second_project / "shaders" / "common.glsl", "const float kTint = 0.75;\n");
    const auto changed = compileInProject(second_project, second_project / "shaders" / "tint.vert");
    REQUIRE(changed.ok);
    CHECK_FALSE(changed.cache_hit);
    CHECK(changed.cache_key != first.cache_key);

    // The same bytes at another place below the root change the key too.
    const auto moved_project = temp.path / "c" / "moved";
    writeIncludingProject(moved_project, "0.25");
    std::filesystem::rename(moved_project / "shaders" / "common.glsl",
                            moved_project / "shaders" / "common_moved.glsl");
    writeFile(moved_project / "shaders" / "common.glsl", "const float kTint = 0.25;\n");
    const auto moved = compileInProject(moved_project, moved_project / "shaders" / "tint.vert");
    REQUIRE(moved.ok);
    CHECK_FALSE(moved.cache_hit);
    CHECK(moved.cache_key != first.cache_key);
}

TEST_CASE("TS-1 a cache key reads an unchanged include file once per compiler",
          "[shader][startup][cache][ts1]") {
    TempDirectory temp;
    const ScopedSharedCacheDirectory environment{""};
    const auto project = temp.path / "project";
    writeIncludingProject(project, "0.25");
    const auto include = project / "shaders" / "common.glsl";
    const auto shader = project / "shaders" / "tint.vert";
    // Written long enough ago that a same-size rewrite would move the time.
    const auto settled = std::filesystem::file_time_type::clock::now() - std::chrono::minutes{1};
    std::filesystem::last_write_time(include, settled);
    std::filesystem::last_write_time(shader, settled);

    ShaderCompiler compiler;
    compiler.setCacheDirectory(temp.path / "cache");
    const auto first = compiler.compileFile(shader);
    REQUIRE(first.ok);
    const auto reads_after_first = compiler.keyFileReadCount();
    REQUIRE(reads_after_first > 0);
    const auto second = compiler.compileFile(shader);
    REQUIRE(second.ok);
    CHECK(second.cache_key == first.cache_key);
    CHECK(compiler.keyFileReadCount() == reads_after_first);

    // Same size, other bytes, a later write time: read again, new key.
    writeFile(include, "const float kTint = 0.75;\n");
    std::filesystem::last_write_time(include, settled + std::chrono::seconds{30});
    const auto changed = compiler.compileFile(shader);
    REQUIRE(changed.ok);
    CHECK(compiler.keyFileReadCount() == reads_after_first + 1);
    CHECK(changed.cache_key != first.cache_key);
    CHECK_FALSE(changed.cache_hit);
    const auto again = compiler.compileFile(shader);
    CHECK(again.cache_key == changed.cache_key);
    CHECK(compiler.keyFileReadCount() == reads_after_first + 1);

    // A file written just now is read on every key: a second same-size write
    // inside one write-time tick must not look unchanged.
    writeFile(include, "const float kTint = 0.50;\n");
    const auto fresh = compiler.compileFile(shader);
    REQUIRE(fresh.ok);
    writeFile(include, "const float kTint = 0.12;\n");
    const auto rewritten = compiler.compileFile(shader);
    REQUIRE(rewritten.ok);
    CHECK(rewritten.cache_key != fresh.cache_key);
    CHECK_FALSE(rewritten.cache_hit);
}

namespace {
// The key's own encoding of one part (shadercompiler.cpp appendKeyPart).
void appendExpectedKeyPart(std::string &key, std::string_view label, std::string_view value) {
    key += std::string{label} + "=" + std::to_string(value.size()) + ":" + std::string{value} + ";";
}
} // namespace

// TS-2 changed how the key is computed (the hash, the include scan, the root
// listing), not what it is: every project's and every machine's existing
// cache entries stay valid. The expected key is written out here from the
// key's layout, from the fixed files below and from what the build supplies
// (shaderCacheKeyBuildInputs), so the test holds across engine shader edits.
TEST_CASE("TS-2 a project shader's cache key is the documented digest of its sources",
          "[shader][startup][cache][ts2]") {
    TempDirectory temp;
    const ScopedSharedCacheDirectory environment{""};
    const auto project = temp.path / "project";
    const std::string root_bytes = "#version 450\n"
                                   "#extension GL_GOOGLE_include_directive : enable\n"
                                   "// a line that mentions #include \"absent.glsl\" is not one\n"
                                   "  #  include \"common.glsl\"\n"
                                   "void main() { gl_Position = vec4(kValue + kDeep, 0.0, 0.0, 1.0); }\n";
    const std::string common_bytes = "#include <deep.glsl>\nconst float kValue = 0.25;\n";
    const std::string deep_bytes = "const float kDeep = 0.5;\n";
    const std::string notes_bytes = "not a shader\n";
    writeFile(project / "fixed.vert", root_bytes);
    writeFile(project / "common.glsl", common_bytes);
    writeFile(project / "shaders" / "include" / "deep.glsl", deep_bytes);
    writeFile(project / "notes" / "readme.txt", notes_bytes);

    ShaderCompiler compiler;
    compiler.setCacheDirectory(temp.path / "cache");
    const auto result = compiler.compileFile(project / "fixed.vert");
    REQUIRE(result.ok);

    // The embedded engine sources, as the key lists them.
    const auto inputs = shaderCacheKeyBuildInputs();
    std::string universe;
    for (const auto id : registeredEngineResourceIds()) {
        const auto extension = std::filesystem::path{id}.extension().string();
        if (extension != ".glsl" && extension != ".vert" && extension != ".frag" && extension != ".comp" &&
            extension != ".rgen" && extension != ".rmiss" && extension != ".rchit" && extension != ".rahit") {
            continue;
        }
        if (const auto resource = engineResource(id)) {
            appendExpectedKeyPart(universe, "potential-engine-name", id);
            appendExpectedKeyPart(universe, "potential-engine-source", *resource);
        }
    }
    CHECK(sha256Hex(universe) == inputs.engine_universe_sha256);

    // compileFile searches the shader's folder (root 0), then its
    // shaders/include (root 1). Files are named by root index and the path
    // below the first root that holds them.
    std::string graph;
    appendExpectedKeyPart(graph, "source-name", "0/fixed.vert");
    appendExpectedKeyPart(graph, "source-bytes", root_bytes);
    appendExpectedKeyPart(graph, "source-name", "0/common.glsl");
    appendExpectedKeyPart(graph, "source-bytes", common_bytes);
    appendExpectedKeyPart(graph, "source-name", "0/shaders/include/deep.glsl");
    appendExpectedKeyPart(graph, "source-bytes", deep_bytes);
    appendExpectedKeyPart(graph, "include-root-count", "2");
    for (const auto &[name, bytes] : std::vector<std::pair<std::string, std::string>>{
             {"0/common.glsl", common_bytes},
             {"0/fixed.vert", root_bytes},
             {"0/notes/readme.txt", notes_bytes},
             {"0/shaders/include/deep.glsl", deep_bytes},
             {"1/deep.glsl", deep_bytes}}) {
        appendExpectedKeyPart(graph, "potential-file-name", name);
        appendExpectedKeyPart(graph, "potential-file-source", bytes);
    }
    appendExpectedKeyPart(graph, "potential-engine-universe-sha256", inputs.engine_universe_sha256);

    std::string raw;
    appendExpectedKeyPart(raw, "source-sha256", sha256Hex(graph));
    appendExpectedKeyPart(raw, "shaderc-version", inputs.shaderc_version);
    appendExpectedKeyPart(raw, "shaderc-spirv-version", std::to_string(inputs.spirv_version));
    appendExpectedKeyPart(raw, "shaderc-spirv-revision", std::to_string(inputs.spirv_revision));
    appendExpectedKeyPart(raw, "target-env", inputs.target_environment);
    appendExpectedKeyPart(raw, "compile-options", inputs.compile_options);
    appendExpectedKeyPart(raw, "stage", std::to_string(static_cast<std::uint32_t>(vk::ShaderStageFlagBits::eVertex)));
    appendExpectedKeyPart(raw, "entry-point", "main");
    appendExpectedKeyPart(raw, "contract-salt", inputs.contract_salt);
    CHECK(result.cache_key == sha256Hex(raw));

    // Hot reload finds a file by these exact strings: each one is the
    // file's own weakly_canonical path, whatever the key scan does.
    std::vector<std::string> expected_dependencies;
    for (const auto &file : {project / "common.glsl", project / "fixed.vert", project / "notes" / "readme.txt",
                             project / "shaders" / "include" / "deep.glsl"}) {
        expected_dependencies.push_back(std::filesystem::weakly_canonical(file).string());
    }
    std::sort(expected_dependencies.begin(), expected_dependencies.end());
    std::vector<std::string> dependencies;
    for (const auto &dependency : result.dependencies) dependencies.push_back(dependency.string());
    std::sort(dependencies.begin(), dependencies.end());
    CHECK(dependencies == expected_dependencies);
}
#endif

} // namespace Pelican
