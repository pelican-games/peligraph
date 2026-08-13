#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
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

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open fixture: " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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
        // CI runner の %TEMP% は 8.3 短縮形(RUNNER~1)のことがあり、
        // エンジン側は canonical 長形式で保持するため、比較の基準を
        // 先に長形式へそろえる。
        base = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
               ("pelican_shader_stem_" + suffix);
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
    REQUIRE(bundle.reflection.bindings.size() == 5);
    REQUIRE(bundle.reflection.bindings[0].set == PELICAN_SET_FRAME);
    REQUIRE(bundle.reflection.bindings[0].binding == PELICAN_FRAME_UBO_BINDING);
    REQUIRE(bundle.reflection.bindings[4].set == PELICAN_SET_FRAME);
    REQUIRE(bundle.reflection.bindings[4].binding ==
            PELICAN_FRAME_RESOLUTION_UBO_BINDING);
    REQUIRE(bundle.reflection.bindings[4].type ==
            vk::DescriptorType::eUniformBuffer);
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
                SUCCEED("mode owned by another fixture test");
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

TEST_CASE(
    "shader library preserves generated virtual includes across reload",
    "[shader][hot-reload][resource-port][wp207a]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto work_dir =
        sourceRoot() /
        "build/test_artifacts/"
        "pelican_shader_virtual_include_test";
    std::filesystem::remove_all(work_dir);
    REQUIRE(
        std::filesystem::create_directories(
            work_dir));
    const auto shader_path =
        work_dir / "generated.frag";
    writeText(
        shader_path,
        "#version 450\n"
        "#extension GL_GOOGLE_include_directive : enable\n"
        "#include \"generated_color.glsl\"\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() { outColor = generatedColor(); }\n");

    ShaderLibrary library{
        ShaderLibraryModuleMode::reflection_only};
    const auto id = library.loadFromFile(
        shader_path, {},
        {{"generated_color.glsl",
          "vec4 generatedColor() { return vec4(0.25); }\n"}});
    REQUIRE(
        library.get(id).virtual_includes.size() == 1);
    REQUIRE(library.reload(id));
    REQUIRE(library.get(id).version == 2);
    REQUIRE((
        library.get(id).virtual_includes ==
        std::vector<
            std::pair<std::string, std::string>>{
            {"generated_color.glsl",
             "vec4 generatedColor() { return vec4(0.25); }\n"}}));
#endif
}

TEST_CASE("shader library tracks watcher keys and reloads every include and surface variant atomically",
          "[shader][hot-reload][hr2-s]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    FastModuleContainer modules;
    ShaderSandbox sandbox;
    GET_MODULE(PathResolver).setup(sandbox.root, false);

    const auto include_path = sandbox.root / "shaders" / "include" / "reload_common.glsl";
    const auto shader_path = sandbox.root / "shaders" / "reload.vert";
    writeText(include_path, "vec4 reload_offset() { return vec4(0.0); }\n");
    writeText(shader_path,
              "#version 450\n"
              "#extension GL_GOOGLE_include_directive : enable\n"
              "#include \"include/reload_common.glsl\"\n"
              "layout(location = 0) in vec3 inPosition;\n"
              "void main() { gl_Position = vec4(inPosition, 1.0) + reload_offset(); }\n");

    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto variant_a = library.loadFromFile(shader_path, {"RELOAD_VARIANT_A"});
    const auto variant_b = library.loadFromFile(shader_path, {"RELOAD_VARIANT_B"});
    const auto root_key = watch::makeAssetKey("shaders/reload.vert");
    const auto include_key = watch::makeAssetKey("shaders/include/reload_common.glsl");
    REQUIRE(library.handlesReload(root_key));
    REQUIRE(library.handlesReload(include_key));
    REQUIRE(library.reloadTrackingStatus().units == 2);

    writeText(include_path, "vec4 reload_offset() { return vec4(0.25, 0.0, 0.0, 0.0); }\n");
    const std::array include_change{include_key};
    const auto first_prepare = library.prepareReload(include_change);
    REQUIRE(first_prepare.candidates.size() == 2);
    REQUIRE(first_prepare.cache_misses == 2);
    REQUIRE(library.get(variant_a).version == 1);
    REQUIRE(library.get(variant_b).version == 1);
    const auto cached_prepare = library.prepareReload(include_change);
    REQUIRE(cached_prepare.cache_hits == 2);

    REQUIRE(library.reload(variant_a));
    REQUIRE(library.get(variant_a).version == 2);
    REQUIRE(library.get(variant_b).version == 2);
    REQUIRE(library.takeDirtyBundles().size() == 2);
    writeText(include_path, "this is not valid GLSL\n");
    REQUIRE_FALSE(library.reload(variant_a));
    REQUIRE(library.get(variant_a).version == 2);
    REQUIRE(library.get(variant_b).version == 2);
    REQUIRE(library.takeDirtyBundles().empty());

    const auto surface_path = sandbox.root / "shaders" / "reload.surface";
    const std::string initial_surface =
        "//! pelican.surface v1\n"
        "//! language: glsl\n"
        "//! params:\n"
        "//!   - { name: scalar, type: float, default: 1.0 }\n\n"
        "void pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) {}\n";
    writeText(surface_path, initial_surface);
    const auto surface = parseSurfaceFormat(readText(surface_path),
                                            "project://shaders/reload.surface");
    const auto surface_a = library.loadFromSurface(
        surface, "project://shaders/reload.surface", SurfacePass::main,
        {"RELOAD_SURFACE_A"});
    const auto surface_b = library.loadFromSurface(
        surface, "project://shaders/reload.surface", SurfacePass::main,
        {"RELOAD_SURFACE_B"});
    const auto surface_key = watch::makeAssetKey("shaders/reload.surface");
    REQUIRE(library.handlesReload(surface_key));
    REQUIRE(library.get(surface_a.vertex).source_path == surface_path);
    REQUIRE(library.get(surface_a.fragment).source_path == surface_path);

    writeText(surface_path, initial_surface + "\n// surface generation two\n");
    REQUIRE(library.reload(surface_a.vertex));
    for (const auto id : {surface_a.vertex, surface_a.fragment,
                          surface_b.vertex, surface_b.fragment}) {
        REQUIRE(library.get(id).version == 2);
    }
    REQUIRE(library.takeDirtyBundles().size() == 4);

    writeText(surface_path,
              "//! pelican.surface v1\n//! language: glsl\n\n"
              "void pelican_surface_v1(in PelicanSurfaceInputV1 i, inout PelicanSurfaceV1 s) { broken }\n");
    REQUIRE_FALSE(library.reload(surface_a.fragment));
    for (const auto id : {surface_a.vertex, surface_a.fragment,
                          surface_b.vertex, surface_b.fragment}) {
        REQUIRE(library.get(id).version == 2);
    }
#endif
}

TEST_CASE(
    "WP218 surface hot reload retains the selected material output ABI",
    "[shader][hot-reload][material-output][wp218]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    FastModuleContainer modules;
    ShaderSandbox sandbox;
    GET_MODULE(PathResolver).setup(
        sandbox.root, false);
    const auto path =
        sandbox.root / "shaders" /
        "extended_gbuffer.surface";
    const auto source = [](std::string_view object_id) {
        return std::string{
                   "//! pelican.surface v1\n"
                   "//! language: glsl\n\n"
                   "void pelican_surface_v1("
                   "in PelicanSurfaceInputV1 i, "
                   "inout PelicanSurfaceV1 s) { "
                   "s.roughness = 0.4; }\n"
                   "void pelican_material_outputs_v1("
                   "in PelicanSurfaceInputV1 i, "
                   "in PelicanSurfaceV1 s, "
                   "inout PelicanMaterialOutputsV1 o) { "
                   "o.object_id = "} +
               std::string{object_id} + "; }\n";
    };
    writeText(path, source("7u"));
    const auto document = parseSurfaceFormat(
        readText(path),
        "project://shaders/extended_gbuffer.surface");
    const MaterialOutputSchema schema{
        .name = "project.reloadable_gbuffer",
        .outputs = {
            {"base_color", MaterialOutputType::vec4,
             MaterialOutputSource::surface_base_color},
            {"object_id",
             MaterialOutputType::unsigned_integer,
             MaterialOutputSource::custom},
        },
    };
    ShaderLibrary library{
        ShaderLibraryModuleMode::reflection_only};
    const auto ids = library.loadFromSurface(
        document,
        "project://shaders/extended_gbuffer.surface",
        SurfacePass::deferred_geometry, {}, schema);
    REQUIRE(
        library.get(ids.fragment)
            .material_output_schema == schema);
    REQUIRE(
        library.get(ids.fragment)
            .reflection.fragment_outputs.size() == 2);

    writeText(path, source("11u"));
    REQUIRE(library.reload(ids.fragment));
    REQUIRE(library.get(ids.fragment).version == 2);
    REQUIRE(
        library.get(ids.fragment)
            .material_output_schema == schema);
    REQUIRE(
        library.get(ids.fragment)
            .reflection.fragment_outputs[1]
            .format == vk::Format::eR32Uint);

    writeText(path, source("vec4(1.0)"));
    REQUIRE_FALSE(library.reload(ids.fragment));
    REQUIRE(library.get(ids.fragment).version == 2);
    REQUIRE(
        library.get(ids.fragment)
            .material_output_schema == schema);
#endif
}

} // namespace Pelican
