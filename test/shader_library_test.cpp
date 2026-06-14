#include "../src/core/shader/shaderlibrary.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Pelican {

namespace {

std::filesystem::path sourceRoot() { return std::filesystem::path{PELICAN_TEST_SOURCE_DIR}; }

std::vector<uint32_t> readSpirvWords(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary | std::ios_base::ate};
    REQUIRE(file.is_open());
    const auto size = static_cast<size_t>(file.tellg());
    REQUIRE(size % sizeof(uint32_t) == 0);
    std::vector<uint32_t> words(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(size));
    REQUIRE(file.good());
    return words;
}

} // namespace

TEST_CASE("shader library loads SPIR-V files and reports dirty reloads", "[shader]") {
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto shader_path = sourceRoot() / "src/core/resources/fullscreen.vert.spv";
    const auto id = library.loadFromFile(shader_path);

    const auto &bundle = library.get(id);
    REQUIRE(bundle.version == 1);
    REQUIRE(bundle.source_path == shader_path);
    REQUIRE(bundle.reflection.bindings.empty());
    REQUIRE(library.takeDirtyBundles().empty());

    REQUIRE(library.reload(id));
    REQUIRE(library.get(id).version == 2);
    const auto dirty = library.takeDirtyBundles();
    REQUIRE(dirty.size() == 1);
    REQUIRE(dirty[0] == id);
    REQUIRE(library.takeDirtyBundles().empty());
}

TEST_CASE("shader library keeps embedded SPIR-V bundles out of reload tracking", "[shader]") {
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto spirv = readSpirvWords(sourceRoot() / "src/core/resources/fullscreen.vert.spv");
    const auto id = library.loadFromSpirv(spirv, "embedded_fullscreen.vert");

    REQUIRE(library.get(id).version == 1);
    REQUIRE(library.get(id).source_path.empty());
    REQUIRE_FALSE(library.reload(id));
    REQUIRE(library.takeDirtyBundles().empty());
}

TEST_CASE("shader library keeps old bundle when reload fails", "[shader]") {
    const auto work_dir = sourceRoot() / "build/test_artifacts/pelican_shader_library_test";
    std::filesystem::remove_all(work_dir);
    REQUIRE(std::filesystem::create_directories(work_dir));
    const auto shader_path = work_dir / "reload.vert.spv";
    std::filesystem::copy_file(sourceRoot() / "src/core/resources/fullscreen.vert.spv", shader_path);

    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto id = library.loadFromFile(shader_path);
    REQUIRE(library.get(id).version == 1);
    REQUIRE(library.get(id).source_path == shader_path);

    REQUIRE(library.reload(id));
    REQUIRE(library.get(id).version == 2);
    REQUIRE(library.takeDirtyBundles().size() == 1);

    const auto previous_version = library.get(id).version;
    std::filesystem::remove(shader_path);
    REQUIRE_FALSE(library.reload(id));
    REQUIRE(library.get(id).version == previous_version);
    REQUIRE_FALSE(library.get(id).log.empty());
    REQUIRE(library.takeDirtyBundles().empty());
}

TEST_CASE("shader library accepts GLSL source files when runtime compiler is enabled", "[shader]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto shader_path = sourceRoot() / "src/core/resources/default.frag";
    const auto id = library.loadFromFile(shader_path);

    REQUIRE(library.get(id).version == 1);
    REQUIRE(library.get(id).source_path == shader_path);
    REQUIRE_FALSE(library.get(id).reflection.bindings.empty());
#else
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    REQUIRE_THROWS_AS(library.loadFromFile(sourceRoot() / "src/core/resources/default.frag"), std::runtime_error);
#endif
}

} // namespace Pelican
