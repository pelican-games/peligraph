#include <catch2/catch_test_macros.hpp>

#include "../src/core/config.hpp"
#include "../src/core/log.hpp"
#include "../src/core/shader/shadercompiler.hpp"

#include <quill/Frontend.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace Pelican {

namespace {

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pelican_shader_cache_wp254_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

class CurrentDirectoryGuard {
  public:
    explicit CurrentDirectoryGuard(const std::filesystem::path &path)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentDirectoryGuard() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
    }

    CurrentDirectoryGuard(const CurrentDirectoryGuard &) = delete;
    CurrentDirectoryGuard &operator=(const CurrentDirectoryGuard &) = delete;

  private:
    std::filesystem::path previous_;
};

std::filesystem::path paddedPath(std::filesystem::path path, std::size_t minimum_length) {
    while (path.native().size() < minimum_length) {
        const auto missing = minimum_length - path.native().size();
        const auto component_length = missing > 1 ? std::min<std::size_t>(missing - 1, 32) : 1;
        path /= std::string(component_length, 'x');
    }
    return path;
}

std::size_t occurrenceCount(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (auto position = text.find(needle); position != std::string_view::npos;
         position = text.find(needle, position + needle.size())) {
        ++count;
    }
    return count;
}

constexpr std::string_view vertexSource = R"glsl(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)glsl";

constexpr std::string_view secondVertexSource = R"glsl(#version 450
void main() { gl_Position = vec4(1.0, 0.0, 0.0, 1.0); }
)glsl";

} // namespace

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("shader cache handles long temporary paths and reports fallback once",
          "[shader][startup][cache]") {
    TempDirectory temp;
    CurrentDirectoryGuard current_directory{temp.path};

    // A SHA-256 cache entry under a 180-character directory stays below
    // MAX_PATH, while the old path + ".tmp-" + nonce form exceeds it even
    // with the shortest possible nonce.
    const auto long_cache = paddedPath(temp.path / "long-cache", 180);
    ShaderCompiler writer;
    writer.setCacheDirectory(long_cache);
    const auto written = writer.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                               "long-path.vert");
    const auto entry = long_cache / (written.cache_key + ".spv-cache");

    ShaderCompiler reader;
    reader.setCacheDirectory(long_cache);
    const auto cached = reader.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                             "long-path.vert");

    const auto unavailable_cache = temp.path / "not-a-directory";
    {
        std::ofstream file{unavailable_cache};
        file << "occupied";
    }

    setupLogger(true);
    ShaderCompiler first_fallback_compiler;
    first_fallback_compiler.setCacheDirectory(unavailable_cache);
    const auto first_fallback = first_fallback_compiler.compileSource(
        vertexSource, vk::ShaderStageFlagBits::eVertex, "fallback-a.vert");

    ShaderCompiler second_fallback_compiler;
    second_fallback_compiler.setCacheDirectory(unavailable_cache);
    const auto second_fallback = second_fallback_compiler.compileSource(
        secondVertexSource, vk::ShaderStageFlagBits::eVertex, "fallback-b.vert");

    logger->flush_log();
    quill::Frontend::remove_logger_blocking(logger);
    logger = nullptr;

    std::ifstream log_file{temp.path / logFileName};
    const std::string log{std::istreambuf_iterator<char>{log_file},
                          std::istreambuf_iterator<char>{}};

    REQUIRE(written.ok);
    REQUIRE_FALSE(written.cache_hit);
    REQUIRE(entry.native().size() < 260);
    REQUIRE(entry.native().size() + std::string_view{".tmp-0-0"}.size() > 260);
    REQUIRE(std::filesystem::is_regular_file(entry));
    REQUIRE(cached.ok);
    REQUIRE(cached.cache_hit);
    REQUIRE(cached.log == "shader disk cache hit");
    REQUIRE(cached.spirv == written.spirv);

    REQUIRE(first_fallback.ok);
    REQUIRE_FALSE(first_fallback.cache_hit);
    REQUIRE_FALSE(first_fallback.spirv.empty());
    REQUIRE(second_fallback.ok);
    REQUIRE_FALSE(second_fallback.cache_hit);
    REQUIRE_FALSE(second_fallback.spirv.empty());
    REQUIRE(occurrenceCount(log, "shader disk cache is unavailable") == 1);
    REQUIRE(log.find("directory cannot be created") != std::string::npos);
}
#endif

} // namespace Pelican
