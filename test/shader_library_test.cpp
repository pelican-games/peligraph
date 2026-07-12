#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

namespace {

std::filesystem::path sourceRoot() { return std::filesystem::path{PELICAN_TEST_SOURCE_DIR}; }
std::filesystem::path fixtureRoot() {
    return sourceRoot() / "test" / "fixtures" / "project_format";
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

ShaderStage parseStage(const std::string &stage) {
    if (stage == "vertex") {
        return ShaderStage::vertex;
    }
    if (stage == "fragment") {
        return ShaderStage::fragment;
    }
    throw std::runtime_error("unknown fixture shader stage: " + stage);
}

struct ShaderSandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    ShaderSandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() / ("pelican_shader_stem_" + suffix);
        root = base / "project";
        std::filesystem::create_directories(root / "shaders");
        std::filesystem::copy_file(sourceRoot() / "src/core/resources/fullscreen.vert.spv",
                                   root / "shaders" / "fullscreen.vert.spv");
        writeText(root / "shaders" / "note.txt", "not a shader");
    }

    ~ShaderSandbox() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
};

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
    REQUIRE(bundle.reflection.bindings.size() == 4);
    REQUIRE(bundle.reflection.bindings[0].set == PELICAN_SET_FRAME);
    REQUIRE(bundle.reflection.bindings[0].binding == PELICAN_FRAME_UBO_BINDING);
    REQUIRE(library.takeDirtyBundles().empty());

    REQUIRE(library.reload(id));
    REQUIRE(library.get(id).version == 2);
    const auto dirty = library.takeDirtyBundles();
    REQUIRE(dirty.size() == 1);
    REQUIRE(dirty[0] == id);
    REQUIRE(library.takeDirtyBundles().empty());
}

TEST_CASE("shader references reject explicit extensions and name the stem form", "[shader]") {
    struct ExplicitReference {
        const char *ref;
        ShaderStage stage;
    };
    const ExplicitReference references[] = {
        {"shaders/fullscreen.spv", ShaderStage::vertex},
        {"shaders/fullscreen.vert", ShaderStage::vertex},
        {"shaders/fullscreen.frag", ShaderStage::fragment},
        {"shaders/fullscreen.comp", ShaderStage::compute},
        {"shaders/fullscreen.wgsl", ShaderStage::fragment},
    };

    for (const auto &reference : references) {
        DYNAMIC_SECTION(reference.ref) {
            std::string message;
            try {
                (void)makeShaderReference(reference.ref, reference.stage);
            } catch (const std::exception &ex) {
                message = ex.what();
            }
            REQUIRE(contains(message, reference.ref));
            REQUIRE(contains(message, "extensionless"));
            REQUIRE(contains(message, shaderStageName(reference.stage)));
        }
    }
}

TEST_CASE("shader library resolves shader stem project format fixtures", "[shader]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");
    ShaderSandbox sandbox;

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto scenario = readJson(fixtureRoot() / file);
            if (scenario.value("mode", std::string{}) != "shader_stem") {
                continue;
            }

            PathResolver resolver;
            resolver.setup(sandbox.root, false);
            ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
            const auto reference = makeShaderReference(scenario.at("ref").get<std::string>(),
                                                       parseStage(scenario.at("stage").get<std::string>()));
            REQUIRE(reference.kind == ShaderReferenceKind::stem);

            const auto expected = entry.at("expect").get<std::string>();
            if (expected == "ok") {
                const auto id = library.loadFromReference(reference, resolver, true);
                REQUIRE(library.get(id).version == 1);
            } else {
                std::string message;
                try {
                    (void)library.loadFromReference(reference, resolver, true);
                } catch (const std::exception &ex) {
                    message = ex.what();
                }

                REQUIRE_FALSE(message.empty());
                REQUIRE(entry.at("error_kind").get<std::string>() == "shader_stem_missing");
                REQUIRE(contains(message, "Shader stem could not be resolved"));
#if PELICAN_RUNTIME_SHADER_COMPILER
                REQUIRE(contains(message, "shaders/missing.vert"));
#endif
                REQUIRE(contains(message, "shaders/missing.vert.spv"));
#if PELICAN_RUNTIME_SHADER_COMPILER
                REQUIRE(message.find("shaders/missing.vert") < message.find("shaders/missing.vert.spv"));
#endif
            }
        }
    }
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

TEST_CASE("shader library polls modified source files on a fixed interval", "[shader]") {
    const auto work_dir = sourceRoot() / "build/test_artifacts/pelican_shader_library_poll_test";
    std::filesystem::remove_all(work_dir);
    REQUIRE(std::filesystem::create_directories(work_dir));
    const auto shader_path = work_dir / "poll.vert.spv";
    std::filesystem::copy_file(sourceRoot() / "src/core/resources/fullscreen.vert.spv", shader_path);

    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto id = library.loadFromFile(shader_path);
    const auto start = std::chrono::steady_clock::now();

    REQUIRE(library.reloadModifiedSources(start) == 0);
    REQUIRE(library.get(id).version == 1);

    const auto previous_write_time = std::filesystem::last_write_time(shader_path);
    std::filesystem::last_write_time(shader_path, previous_write_time + std::chrono::seconds{2});

    REQUIRE(library.reloadModifiedSources(start + std::chrono::milliseconds{500}) == 0);
    REQUIRE(library.get(id).version == 1);

    REQUIRE(library.reloadModifiedSources(start + std::chrono::seconds{2}) == 1);
    REQUIRE(library.get(id).version == 2);
    const auto dirty = library.takeDirtyBundles();
    REQUIRE(dirty.size() == 1);
    REQUIRE(dirty[0] == id);
}

} // namespace Pelican
