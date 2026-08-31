#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/shader/raytracingoracle.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/project/featurecompose.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spirv_reflect.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace Pelican {
namespace {

#ifndef PELICAN_TEST_GIT_EXECUTABLE
#define PELICAN_TEST_GIT_EXECUTABLE ""
#endif

std::filesystem::path sourceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(
            "failed to open WP364 fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

std::vector<unsigned char> readBytes(
    const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(
            "failed to open WP364 byte fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
}

std::string sha256(std::span<const unsigned char> bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::string sha256(std::string_view bytes) {
    const auto *begin = reinterpret_cast<const unsigned char *>(
        bytes.data());
    return picosha2::hash256_hex_string(begin, begin + bytes.size());
}

struct ParentManifestMismatch {
    std::string path;
    std::string field;
    std::string detail;
};

struct GitCommandResult {
    int exit_code = -1;
    std::vector<unsigned char> output;
};

std::string shellQuote(std::string_view value) {
#ifdef _WIN32
    std::string quoted{"\""};
    for (const auto character : value) {
        if (character == '"') quoted += "\\\"";
        else quoted += character;
    }
    quoted += '"';
    return quoted;
#else
    std::string quoted{"'"};
    for (const auto character : value) {
        if (character == '\'') quoted += "'\"'\"'";
        else quoted += character;
    }
    quoted += '\'';
    return quoted;
#endif
}

GitCommandResult runGit(
    const std::filesystem::path &root,
    std::initializer_list<std::string> arguments) {
    if (std::string_view{PELICAN_TEST_GIT_EXECUTABLE}.empty()) {
        return {};
    }
    auto command = shellQuote(PELICAN_TEST_GIT_EXECUTABLE) +
                   " -C " + shellQuote(root.string()) +
                   " --no-pager";
    for (const auto &argument : arguments) {
        command += " " + shellQuote(argument);
    }
#ifdef _WIN32
    command += " 2>NUL";
    // _popen dispatches through cmd.exe. A command whose executable path is
    // quoted needs one additional pair around the complete command line.
    command = "\"" + command + "\"";
    auto *pipe = _popen(command.c_str(), "r");
#else
    command += " 2>/dev/null";
    auto *pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) return {};
#ifdef _WIN32
    if (_setmode(_fileno(pipe), _O_BINARY) == -1) {
        _pclose(pipe);
        return {};
    }
#endif

    GitCommandResult result;
    std::array<unsigned char, 4096> buffer{};
    while (const auto count =
               std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        result.output.insert(result.output.end(), buffer.begin(),
                             buffer.begin() + count);
    }
#ifdef _WIN32
    result.exit_code = _pclose(pipe);
#else
    result.exit_code = pclose(pipe);
#endif
    return result;
}

struct ParentManifestGitAccess {
    std::function<GitCommandResult(std::string_view)> show_head_blob;
    std::function<GitCommandResult(std::string_view)> list_tracked_files;
};

ParentManifestGitAccess gitAccess(
    const std::filesystem::path &root) {
    return {
        .show_head_blob = [root](std::string_view path) {
            return runGit(
                root,
                {"show", "--no-ext-diff", "--no-textconv",
                 "HEAD:" + std::string{path}});
        },
        .list_tracked_files = [root](std::string_view project) {
            return runGit(
                root,
                {"ls-files", "-z", "--",
                 "projects/" + std::string{project}});
        },
    };
}

std::string gitSnapshotUnavailableReason(
    const std::filesystem::path &root) {
    if (std::string_view{PELICAN_TEST_GIT_EXECUTABLE}.empty()) {
        return "git executable was not found at configure time";
    }
    const auto version = runGit(root, {"--version"});
    if (version.exit_code != 0) {
        return "configured git executable cannot be run (exit=" +
               std::to_string(version.exit_code) + ")";
    }
    if (runGit(root, {"rev-parse", "--verify", "HEAD"})
            .exit_code != 0) {
        return "source tree has no readable HEAD commit";
    }
    return {};
}

std::set<std::string> parseNullTerminatedPaths(
    std::span<const unsigned char> bytes) {
    std::set<std::string> paths;
    std::size_t start = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] != 0) continue;
        if (index != start) {
            paths.emplace(
                reinterpret_cast<const char *>(bytes.data() + start),
                index - start);
        }
        start = index + 1;
    }
    if (start != bytes.size()) {
        paths.emplace(
            reinterpret_cast<const char *>(bytes.data() + start),
            bytes.size() - start);
    }
    return paths;
}

bool confinedManifestPath(std::string_view relative,
                          std::string_view project) {
    if (relative.empty() ||
        relative.find('\\') != std::string_view::npos ||
        relative.starts_with('/') || relative.starts_with('\\') ||
        (relative.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(relative[0])) &&
         relative[1] == ':')) {
        return false;
    }
    const std::filesystem::path path{relative};
    if (path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    std::vector<std::string> components;
    for (const auto &component : path) {
        const auto text = component.generic_string();
        if (text.empty() || text == "." || text == "..") {
            return false;
        }
        components.push_back(text);
    }
    return components.size() >= 3 && components[0] == "projects" &&
           components[1] == project &&
           path.generic_string() == relative;
}

std::vector<ParentManifestMismatch> checkParentManifest(
    const nlohmann::json &manifest,
    const ParentManifestGitAccess &git) {
    std::vector<ParentManifestMismatch> mismatches;
    for (const auto &[project_name, project] :
         manifest.at("projects").items()) {
        const auto project_path = "projects/" + project_name;
        if (project_name.empty() || project_name == "." ||
            project_name == ".." ||
            project_name.find('/') != std::string::npos ||
            project_name.find('\\') != std::string::npos) {
            mismatches.push_back({
                project_path, "path_confinement",
                "invalid project name",
            });
            continue;
        }

        std::set<std::string> manifest_paths;
        std::set<std::string> valid_manifest_paths;
        for (const auto &entry : project.at("files")) {
            const auto relative = entry.at("path").get<std::string>();
            if (!manifest_paths.insert(relative).second) {
                mismatches.push_back({
                    relative, "path_set",
                    "duplicate manifest path",
                });
            }
            if (!confinedManifestPath(relative, project_name)) {
                mismatches.push_back({
                    relative, "path_confinement",
                    "path must remain below " + project_path,
                });
                continue;
            }
            valid_manifest_paths.insert(relative);
        }

        const auto tracked_result =
            git.list_tracked_files(project_name);
        if (tracked_result.exit_code != 0) {
            mismatches.push_back({
                project_path, "git_ls_files",
                "git ls-files failed with exit code " +
                    std::to_string(tracked_result.exit_code),
            });
            continue;
        }
        const auto tracked_paths = parseNullTerminatedPaths(
            tracked_result.output);
        const auto expected_count =
            project.at("tracked_file_count").get<std::uint64_t>();
        if (expected_count != tracked_paths.size() ||
            expected_count != project.at("files").size()) {
            mismatches.push_back({
                project_path, "tracked_file_count",
                "expected " + std::to_string(expected_count) +
                    ", manifest has " +
                    std::to_string(project.at("files").size()) +
                    ", git tracks " +
                    std::to_string(tracked_paths.size()),
            });
        }
        for (const auto &relative : manifest_paths) {
            if (!tracked_paths.contains(relative)) {
                mismatches.push_back({
                    relative,
                    "path_set",
                    "manifest path is not tracked at HEAD",
                });
            }
        }
        for (const auto &relative : tracked_paths) {
            if (!manifest_paths.contains(relative)) {
                mismatches.push_back({
                    relative,
                    "path_set",
                    "tracked HEAD path is absent from the manifest",
                });
            }
        }

        for (const auto &entry : project.at("files")) {
            const auto relative = entry.at("path").get<std::string>();
            if (!valid_manifest_paths.contains(relative)) continue;
            const auto blob = git.show_head_blob(relative);
            if (blob.exit_code != 0) {
                mismatches.push_back({
                    relative, "git_show",
                    "git show HEAD:path failed with exit code " +
                        std::to_string(blob.exit_code),
                });
                continue;
            }
            const auto expected_size =
                entry.at("blob_bytes").get<std::uint64_t>();
            const auto actual_size =
                static_cast<std::uint64_t>(blob.output.size());
            if (actual_size != expected_size) {
                mismatches.push_back({
                    relative,
                    "blob_bytes",
                    "expected " + std::to_string(expected_size) +
                        ", got " + std::to_string(actual_size),
                });
            }

            const auto expected_digest =
                entry.at("blob_sha256").get<std::string>();
            const auto actual_digest = sha256(
                std::span<const unsigned char>{blob.output});
            if (actual_digest != expected_digest) {
                mismatches.push_back({
                    relative,
                    "blob_sha256",
                    "expected " + expected_digest + ", got " +
                        actual_digest,
                });
            }
        }
    }
    return mismatches;
}

std::string describeParentManifestMismatches(
    std::span<const ParentManifestMismatch> mismatches) {
    std::string description;
    for (const auto &mismatch : mismatches) {
        description += "\n" + mismatch.path + " [" +
                       mismatch.field + "]: " + mismatch.detail;
    }
    return description;
}

GitCommandResult successfulGitOutput(std::string_view output) {
    return {
        .exit_code = 0,
        .output = std::vector<unsigned char>{output.begin(), output.end()},
    };
}

ParentManifestGitAccess fakeGitAccess(
    std::string blob, std::vector<std::string> tracked_paths,
    std::function<void(std::string_view)> on_show = {}) {
    std::string listing;
    for (const auto &path : tracked_paths) {
        listing += path;
        listing.push_back('\0');
    }
    return {
        .show_head_blob =
            [blob = std::move(blob), on_show = std::move(on_show)](
                std::string_view path) {
                if (on_show) on_show(path);
                return successfulGitOutput(blob);
            },
        .list_tracked_files =
            [listing = std::move(listing)](std::string_view) {
                return successfulGitOutput(listing);
            },
    };
}

nlohmann::json oneFileParentManifest(
    std::string path, std::string_view blob,
    std::uint64_t tracked_file_count = 1) {
    return {
        {"schema", "pelican.wp364.parent_manifest"},
        {"version", 3},
        {"projects",
         {{"demo",
           {{"tracked_file_count", tracked_file_count},
            {"files",
             nlohmann::json::array({
                 {{"path", std::move(path)},
                  {"eol", "lf"},
                  {"blob_sha256", sha256(blob)},
                  {"blob_bytes", blob.size()}},
             })}}}}},
    };
}

nlohmann::json currentRtFramePlanProjection() {
    const auto load_engine = [](std::string_view ref) {
        constexpr std::string_view prefix = "engine://";
        if (!ref.starts_with(prefix)) {
            throw std::runtime_error(
                "WP364 frame-plan fixture expected engine reference");
        }
        return engineResourceOrThrow(ref.substr(prefix.size()));
    };
    const auto composition = composeRenderFeatureConfig(
        nlohmann::json{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features",
             nlohmann::json::array({
                 "engine://features/rt_shadow_mask.json",
                 "engine://features/rt_shadow_mask_pipeline.json"})},
        },
        RenderFeatureComposeDependencies{
            load_engine, false, {}, load_engine});
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(
        composition.config);
    if (graphs.size() != 1) {
        throw std::runtime_error(
            "WP364 frame-plan fixture did not resolve one graph");
    }
    const auto plan = framePlanToJson(planFrameGraph(graphs.front()));
    nlohmann::json projection{
        {"schema", plan.at("schema")},
        {"version", plan.at("version")},
        {"graph", plan.at("graph")},
        {"node_order", nlohmann::json::array()},
        {"rt_nodes", nlohmann::json::array()},
        {"rt_barriers", nlohmann::json::array()},
    };
    for (const auto &node : plan.at("nodes")) {
        const auto name = node.at("name").get<std::string>();
        projection["node_order"].push_back(name);
        if (name == "rt_shadow_mask" ||
            name == "rt_shadow_mask_pipeline") {
            projection["rt_nodes"].push_back(node);
        }
    }
    for (const auto &barrier : plan.at("barriers")) {
        const auto target = barrier.at("to").get<std::string>();
        if (target == "rt_shadow_mask" ||
            target == "rt_shadow_mask_pipeline") {
            projection["rt_barriers"].push_back(barrier);
        }
    }
    return projection;
}

std::vector<std::uint32_t> resourceSpirv(std::string_view name) {
    const auto bytes = engineResourceOrThrow(name);
    REQUIRE(bytes.size() % sizeof(std::uint32_t) == 0);
    std::vector<std::uint32_t> words(
        bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

void appendInstruction(std::vector<std::uint32_t> &words,
                       SpvOp opcode,
                       std::initializer_list<std::uint32_t> operands) {
    words.push_back(
        (static_cast<std::uint32_t>(operands.size() + 1) << 16u) |
        static_cast<std::uint32_t>(opcode));
    words.insert(words.end(), operands.begin(), operands.end());
}

void appendStringInstruction(std::vector<std::uint32_t> &words,
                             SpvOp opcode,
                             std::string_view value) {
    const auto string_words =
        (value.size() + 1 + sizeof(std::uint32_t) - 1) /
        sizeof(std::uint32_t);
    words.push_back(
        (static_cast<std::uint32_t>(1 + string_words) << 16u) |
        static_cast<std::uint32_t>(opcode));
    const auto first = words.size();
    words.resize(first + string_words, 0);
    std::memcpy(words.data() + first, value.data(), value.size());
}

void appendMainEntry(std::vector<std::uint32_t> &words,
                     SpvExecutionModel model, std::uint32_t function,
                     std::initializer_list<std::uint32_t> interfaces = {}) {
    const auto word_count = 5u +
                            static_cast<std::uint32_t>(interfaces.size());
    words.push_back((word_count << 16u) | SpvOpEntryPoint);
    words.push_back(model);
    words.push_back(function);
    words.push_back(0x6e69616du); // "main"
    words.push_back(0u);
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}

std::vector<std::uint32_t> moduleHeader() {
    return {0x07230203u, 0x00010500u, 0u, 128u, 0u};
}

std::vector<std::uint32_t> validDualMainModule() {
    auto words = moduleHeader();
    appendInstruction(words, SpvOpCapability,
                      {SpvCapabilityShader});
    appendInstruction(words, SpvOpCapability,
                      {SpvCapabilityRayTracingKHR});
    appendStringInstruction(words, SpvOpExtension,
                            "SPV_KHR_ray_tracing");
    appendInstruction(words, SpvOpMemoryModel,
                      {SpvAddressingModelLogical,
                       SpvMemoryModelGLSL450});
    appendMainEntry(words, SpvExecutionModelRayGenerationKHR, 10);
    appendMainEntry(words, SpvExecutionModelMissKHR, 11);
    appendInstruction(words, SpvOpTypeVoid, {1});
    appendInstruction(words, SpvOpTypeFunction, {2, 1});
    appendInstruction(words, SpvOpFunction,
                      {1, 10, SpvFunctionControlMaskNone, 2});
    appendInstruction(words, SpvOpLabel, {12});
    appendInstruction(words, SpvOpReturn, {});
    appendInstruction(words, SpvOpFunctionEnd, {});
    appendInstruction(words, SpvOpFunction,
                      {1, 11, SpvFunctionControlMaskNone, 2});
    appendInstruction(words, SpvOpLabel, {13});
    appendInstruction(words, SpvOpReturn, {});
    appendInstruction(words, SpvOpFunctionEnd, {});
    return words;
}

std::vector<std::uint32_t> traceModule(SpvOp producer_opcode) {
    auto words = moduleHeader();
    appendMainEntry(words, SpvExecutionModelRayGenerationKHR, 10, {4});
    appendInstruction(words, SpvOpDecorate,
                      {4, SpvDecorationLocation, 0});
    appendInstruction(words, SpvOpTypeFloat, {1, 32});
    appendInstruction(words, SpvOpTypePointer,
                      {2, SpvStorageClassRayPayloadKHR, 1});
    appendInstruction(words, SpvOpVariable,
                      {2, 4, SpvStorageClassRayPayloadKHR});
    appendInstruction(words, SpvOpTypeInt, {5, 32, 0});
    if (producer_opcode == SpvOpConstantNull) {
        appendInstruction(words, producer_opcode, {5, 6});
    } else {
        appendInstruction(words, producer_opcode, {5, 6, 0});
    }
    appendInstruction(words, SpvOpFunction, {0, 10, 0, 0});
    appendInstruction(
        words, SpvOpTraceRayKHR,
        {20, 21, 22, 6, 6, 6, 23, 24, 25, 26, 4});
    appendInstruction(words, SpvOpFunctionEnd, {});
    return words;
}

std::vector<std::uint32_t> unsupportedArrayModule(
    bool specialization_length) {
    auto words = moduleHeader();
    appendMainEntry(words, SpvExecutionModelMissKHR, 10, {4});
    appendInstruction(words, SpvOpDecorate,
                      {4, SpvDecorationLocation, 0});
    appendInstruction(words, SpvOpTypeFloat, {1, 32});
    if (specialization_length) {
        appendInstruction(words, SpvOpTypeInt, {5, 32, 0});
        appendInstruction(words, SpvOpSpecConstant, {5, 6, 2});
        appendInstruction(words, SpvOpTypeArray, {7, 1, 6});
    } else {
        appendInstruction(words, SpvOpTypeRuntimeArray, {7, 1});
    }
    appendInstruction(words, SpvOpTypePointer,
                      {2, SpvStorageClassIncomingRayPayloadKHR, 7});
    appendInstruction(words, SpvOpVariable,
                      {2, 4, SpvStorageClassIncomingRayPayloadKHR});
    return words;
}

RayTracingCanonicalType scalar(
    RayTracingCanonicalTypeKind kind, std::uint32_t width) {
    return {.kind = kind, .bit_width = width};
}

RayTracingCanonicalType vectorType(
    std::uint32_t count, RayTracingCanonicalType element) {
    return {
        .kind = RayTracingCanonicalTypeKind::vector,
        .element_count = count,
        .elements = {std::move(element)},
    };
}

RayTracingInterfaceVariable interfaceVariable(
    RayTracingInterfaceStorage storage, std::uint32_t location,
    RayTracingCanonicalType type) {
    const auto fingerprint = rayTracingCanonicalTypeFingerprint(type);
    const auto size = rayTracingCanonicalTypeByteSize(type);
    return {
        .storage = storage,
        .location = location,
        .type = std::move(type),
        .fingerprint = fingerprint,
        .byte_size = size,
    };
}

struct InterfaceFixture {
    RayTracingEntryPointReflection raygen;
    std::array<RayTracingEntryPointReflection, 2> misses;
    std::array<RayTracingEntryPointReflection, 2> hits;
    std::array<RayTracingShaderOccurrence, 2> miss_occurrences;
    std::array<RayTracingShaderOccurrence, 2> hit_occurrences;
    RayTracingShaderOccurrence raygen_occurrence;

    InterfaceFixture() {
        const auto f32 = scalar(
            RayTracingCanonicalTypeKind::floating, 32);
        const auto u32 = scalar(
            RayTracingCanonicalTypeKind::unsigned_integer, 32);
        raygen.key = {SpvExecutionModelRayGenerationKHR, "main"};
        raygen.interfaces = {
            interfaceVariable(RayTracingInterfaceStorage::ray_payload,
                              0, f32),
            interfaceVariable(RayTracingInterfaceStorage::ray_payload,
                              1, u32),
        };
        raygen.trace_calls = {
            RayTracingTraceCall{
                .caller_entry = raygen.key,
                .instruction_number = 100,
                .payload_type = f32,
                .miss_index = 0,
                .sbt_record_offset = 0,
                .sbt_record_stride = 0,
            },
            RayTracingTraceCall{
                .caller_entry = raygen.key,
                .instruction_number = 101,
                .payload_type = u32,
                .miss_index = 1,
                .sbt_record_offset = 1,
                .sbt_record_stride = 0,
            },
        };
        for (std::uint32_t index = 0; index < 2; ++index) {
            const auto type = index == 0 ? f32 : u32;
            misses[index].key = {SpvExecutionModelMissKHR, "main"};
            misses[index].interfaces = {interfaceVariable(
                RayTracingInterfaceStorage::incoming_ray_payload,
                index, type)};
            hits[index].key = {SpvExecutionModelClosestHitKHR, "main"};
            hits[index].interfaces = {
                interfaceVariable(
                    RayTracingInterfaceStorage::incoming_ray_payload,
                    index, type),
                interfaceVariable(
                    RayTracingInterfaceStorage::hit_attribute, 0,
                    vectorType(2, f32)),
            };
            miss_occurrences[index] = {
                .stage = ShaderStage::miss,
                .group_index = 1 + index,
                .diagnostic_uri = index == 0 ? "miss0" : "miss1",
                .entry = &misses[index],
            };
            hit_occurrences[index] = {
                .stage = ShaderStage::closesthit,
                .group_index = 3 + index,
                .diagnostic_uri = index == 0 ? "hit0" : "hit1",
                .entry = &hits[index],
            };
        }
        raygen_occurrence = {
            .stage = ShaderStage::raygen,
            .group_index = 0,
            .diagnostic_uri = "raygen",
            .entry = &raygen,
        };
    }

    RayTracingPipelineInterfaceRequest request() const {
        return {
            .raygen = raygen_occurrence,
            .misses = miss_occurrences,
            .hit_groups = hit_occurrences,
        };
    }
};

RayTracingRuntimeFacts generousFacts() {
    return {
        .max_ray_recursion_depth = 8,
        .max_ray_hit_attribute_size = 32,
        .max_ray_dispatch_invocation_count = 1ull << 30u,
        .max_dispatch_dimensions = {1ull << 20u, 1ull << 20u,
                                    1ull << 20u},
        .device_name = "RT1a synthetic validator",
    };
}

std::string rtShadowMaskValidatorDigest(
    const std::vector<std::uint32_t> &raygen_spirv,
    const std::vector<std::uint32_t> &miss_spirv,
    const std::vector<std::uint32_t> &hit_spirv,
    std::string_view raygen_uri, std::string_view miss_uri,
    std::string_view hit_uri) {
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto raygen_id = library.loadFromSpirv(
        raygen_spirv, ShaderStage::raygen, raygen_uri);
    const auto miss_id = library.loadFromSpirv(
        miss_spirv, ShaderStage::miss, miss_uri);
    const auto hit_id = library.loadFromSpirv(
        hit_spirv, ShaderStage::closesthit, hit_uri);
    const auto occurrence = [&](ShaderBundleId id, ShaderStage stage,
                                std::uint32_t group) {
        const auto &bundle = library.get(id);
        return RayTracingShaderOccurrence{
            .stage = stage,
            .group_index = group,
            .diagnostic_uri = bundle.diagnostic_uri,
            .entry = &selectRayTracingEntryPoint(
                bundle.reflection.ray_tracing_module, stage, "main",
                bundle.diagnostic_uri),
        };
    };
    const auto raygen = occurrence(
        raygen_id, ShaderStage::raygen, 0);
    const std::array misses{
        occurrence(miss_id, ShaderStage::miss, 1)};
    const std::array hits{
        occurrence(hit_id, ShaderStage::closesthit, 2)};
    return validateRayTracingPipelineInterfaces(
        {.raygen = raygen,
         .misses = misses,
         .hit_groups = hits},
        generousFacts());
}

std::string embeddedRtShadowMaskValidatorDigest(
    std::string_view raygen_uri, std::string_view miss_uri,
    std::string_view hit_uri) {
    return rtShadowMaskValidatorDigest(
        resourceSpirv("rt_shadow_mask_pipeline.rgen.spv"),
        resourceSpirv("rt_shadow_mask_pipeline.rmiss.spv"),
        resourceSpirv("rt_shadow_mask_pipeline.rchit.spv"),
        raygen_uri, miss_uri, hit_uri);
}

} // namespace

TEST_CASE(
    "WP364 immutable parent fixtures remain byte exact",
    "[ray-tracing][rt1a][parent-baseline]") {
    const auto fixture_directory =
        sourceRoot() / "test" / "fixtures" / "wp364";
    const auto fixture =
        readJson(fixture_directory / "parent_baseline.json");
    const auto parent_manifest =
        readJson(fixture_directory / "parent_manifest.json");
    CHECK(fixture.at("source_commit") ==
          "d5ea0c5bdca4d5ea2d2437f50e6c8cb80f1fcf72");
    REQUIRE(parent_manifest.at("schema") ==
            "pelican.wp364.parent_manifest");
    REQUIRE(parent_manifest.at("version") == 3);
    CHECK(parent_manifest.at("source_commit") ==
          fixture.at("source_commit"));

    SECTION("embedded SPIR-V hashes are the compiler-OFF oracle") {
        for (const auto &[name, expected] :
             fixture.at("embedded_spirv_sha256").items()) {
            const auto bytes = engineResourceOrThrow(name);
            CAPTURE(name);
            CHECK(sha256(bytes) == expected.get<std::string>());
        }
    }

    SECTION("compiler-ON cache keys are non-empty parent values") {
#if PELICAN_RUNTIME_SHADER_COMPILER
        struct Source {
            std::string_view name;
            vk::ShaderStageFlagBits stage;
        };
        const std::array sources{
            Source{"rt_shadow_mask.frag",
                   vk::ShaderStageFlagBits::eFragment},
            Source{"rt_shadow_mask_pipeline.rgen",
                   vk::ShaderStageFlagBits::eRaygenKHR},
            Source{"rt_shadow_mask_pipeline.rmiss",
                   vk::ShaderStageFlagBits::eMissKHR},
            Source{"rt_shadow_mask_pipeline.rchit",
                   vk::ShaderStageFlagBits::eClosestHitKHR},
        };
        const auto cache =
            std::filesystem::temp_directory_path() /
            ("pelican_wp364_parent_cache_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
        struct CacheCleanup {
            std::filesystem::path path;
            ~CacheCleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{cache};
        ShaderCompiler compiler;
        compiler.setCacheDirectory(cache);
        for (const auto &source : sources) {
            const std::string source_name{source.name};
            const auto compiled = compiler.compileSource(
                engineResourceOrThrow(source.name), source.stage,
                "engine://" + source_name);
            CAPTURE(source_name, compiled.log);
            REQUIRE(compiled.ok);
            REQUIRE_FALSE(compiled.cache_key.empty());
            CHECK(compiled.cache_key ==
                  fixture.at("compiler_on_cache_key")
                      .at(source.name)
                      .get<std::string>());
        }
#else
        for (const auto &[name, expected] :
             fixture.at("compiler_on_cache_key").items()) {
            CAPTURE(name);
            CHECK_FALSE(expected.get<std::string>().empty());
        }
#endif
    }

    SECTION("canonical frame plan projection is the detached-parent value") {
        const auto actual = currentRtFramePlanProjection();
        CHECK(actual == fixture.at("frame_plan_projection"));
        for (const auto &node : actual.at("rt_nodes")) {
            CHECK_FALSE(node.contains("max_recursion"));
        }
    }

    SECTION("manifest-listed shipping project bytes remain exact") {
        const auto unavailable =
            gitSnapshotUnavailableReason(sourceRoot());
        if (!unavailable.empty()) {
            SKIP("WP364a parent manifest Git-blob validation skipped: " +
                 unavailable);
        }
        const auto mismatches =
            checkParentManifest(parent_manifest, gitAccess(sourceRoot()));
        INFO(describeParentManifestMismatches(mismatches));
        CHECK(mismatches.empty());
    }

    SECTION("a tampered manifest digest is rejected") {
        const auto unavailable =
            gitSnapshotUnavailableReason(sourceRoot());
        if (!unavailable.empty()) {
            SKIP("WP364a parent manifest Git-blob validation skipped: " +
                 unavailable);
        }
        auto tampered_manifest = parent_manifest;
        auto &projects = tampered_manifest.at("projects");
        REQUIRE_FALSE(projects.empty());
        auto &files = projects.begin().value().at("files");
        REQUIRE_FALSE(files.empty());
        auto &entry = files.at(0);
        const auto relative = entry.at("path").get<std::string>();
        auto tampered_digest =
            entry.at("blob_sha256").get<std::string>();
        REQUIRE_FALSE(tampered_digest.empty());
        tampered_digest.front() =
            tampered_digest.front() == '0' ? '1' : '0';
        entry["blob_sha256"] = tampered_digest;

        const auto mismatches =
            checkParentManifest(
                tampered_manifest, gitAccess(sourceRoot()));
        CHECK(std::any_of(
            mismatches.begin(), mismatches.end(),
            [&](const auto &mismatch) {
                return mismatch.path == relative &&
                       mismatch.field == "blob_sha256";
            }));
    }

    SECTION("representative existing GPU goldens are immutable bytes") {
        for (const auto &[relative, expected] :
             fixture.at("representative_golden_sha256").items()) {
            const auto bytes = readBytes(
                sourceRoot() / "test" / "golden" / relative);
            CAPTURE(relative);
            CHECK(sha256(std::span<const unsigned char>{bytes}) ==
                  expected.get<std::string>());
        }
        const auto &empty =
            fixture.at("gpu_golden")
                .at("rt_shadow_mask_pipeline_empty_scene_r8");
        const auto extent = empty.at("extent");
        std::vector<unsigned char> pixels(
            extent.at(0).get<std::size_t>() *
                extent.at(1).get<std::size_t>(),
            empty.at("fill_byte").get<unsigned char>());
        CHECK(sha256(std::span<const unsigned char>{pixels}) ==
              empty.at("sha256").get<std::string>());
    }
}

TEST_CASE(
    "WP364a parent manifest judge uses raw HEAD blobs and a complete confined path set",
    "[ray-tracing][rt1a][parent-baseline][mutation]") {
    constexpr std::string_view path = "projects/demo/file.txt";
    constexpr std::string_view lf_blob = "line one\nline two\n";
    const auto has_mismatch = [](
                                  const auto &mismatches,
                                  std::string_view expected_path,
                                  std::string_view expected_field) {
        return std::any_of(
            mismatches.begin(), mismatches.end(),
            [&](const auto &mismatch) {
                return mismatch.path == expected_path &&
                       mismatch.field == expected_field;
            });
    };

    SECTION("CRLF in the HEAD blob is not normalized to manifest LF") {
        const auto manifest = oneFileParentManifest(
            std::string{path}, lf_blob);
        const auto mismatches = checkParentManifest(
            manifest,
            fakeGitAccess("line one\r\nline two\r\n",
                          {std::string{path}}));
        CHECK(has_mismatch(mismatches, path, "blob_bytes"));
        CHECK(has_mismatch(mismatches, path, "blob_sha256"));
    }

    SECTION("a tracked path missing from the manifest is rejected") {
        auto manifest = oneFileParentManifest(
            std::string{path}, lf_blob);
        manifest["projects"]["demo"]["files"] =
            nlohmann::json::array();
        const auto mismatches = checkParentManifest(
            manifest,
            fakeGitAccess(std::string{lf_blob},
                          {std::string{path}}));
        CHECK(has_mismatch(mismatches, path, "path_set"));
    }

    SECTION("a manifest path absent from tracked HEAD is rejected") {
        auto manifest = oneFileParentManifest(
            std::string{path}, lf_blob, 0);
        const auto mismatches = checkParentManifest(
            manifest, fakeGitAccess(std::string{lf_blob}, {}));
        CHECK(has_mismatch(mismatches, path, "path_set"));
    }

    SECTION("tracked_file_count is an enforced field") {
        const auto manifest = oneFileParentManifest(
            std::string{path}, lf_blob, 2);
        const auto mismatches = checkParentManifest(
            manifest,
            fakeGitAccess(std::string{lf_blob},
                          {std::string{path}}));
        CHECK(has_mismatch(
            mismatches, "projects/demo", "tracked_file_count"));
    }

    SECTION("parent traversal is rejected before git show") {
        constexpr std::string_view escaped =
            "projects/demo/../escape.txt";
        std::size_t show_calls = 0;
        const auto manifest = oneFileParentManifest(
            std::string{escaped}, lf_blob);
        const auto mismatches = checkParentManifest(
            manifest,
            fakeGitAccess(
                std::string{lf_blob}, {std::string{escaped}},
                [&](std::string_view) { ++show_calls; }));
        CHECK(has_mismatch(
            mismatches, escaped, "path_confinement"));
        CHECK(show_calls == 0);
    }

    SECTION("an absolute path is rejected before git show") {
        constexpr std::string_view absolute =
            "C:/outside/demo.txt";
        std::size_t show_calls = 0;
        const auto manifest = oneFileParentManifest(
            std::string{absolute}, lf_blob);
        const auto mismatches = checkParentManifest(
            manifest,
            fakeGitAccess(
                std::string{lf_blob}, {std::string{absolute}},
                [&](std::string_view) { ++show_calls; }));
        CHECK(has_mismatch(
            mismatches, absolute, "path_confinement"));
        CHECK(show_calls == 0);
    }
}

TEST_CASE(
    "RT reflection retains every execution-model and entry-name pair",
    "[ray-tracing][rt1a][reflection]") {
    auto words = moduleHeader();
    appendMainEntry(words, SpvExecutionModelRayGenerationKHR, 10);
    appendMainEntry(words, SpvExecutionModelMissKHR, 11);
    const auto module = inspectRayTracingSpirv(
        words, "memory://same-main-two-stages.spv");
    REQUIRE(module.entries.size() == 2);
    CHECK(selectRayTracingEntryPoint(
              module, ShaderStage::raygen, "main", "fixture")
              .key.execution_model ==
          SpvExecutionModelRayGenerationKHR);
    CHECK(selectRayTracingEntryPoint(
              module, ShaderStage::miss, "main", "fixture")
              .key.execution_model == SpvExecutionModelMissKHR);

    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto valid_dual = validDualMainModule();
    const auto dual_raygen = library.loadFromSpirv(
        valid_dual, ShaderStage::raygen,
        "same-main-two-stages.spv");
    const auto dual_miss = library.loadFromSpirv(
        valid_dual, ShaderStage::miss,
        "same-main-two-stages.spv");
    REQUIRE(library.get(dual_raygen)
                .reflection.selected_entry_point.has_value());
    REQUIRE(library.get(dual_miss)
                .reflection.selected_entry_point.has_value());
    CHECK(library.get(dual_raygen)
              .reflection.selected_entry_point->execution_model ==
          SpvExecutionModelRayGenerationKHR);
    CHECK(library.get(dual_miss)
              .reflection.selected_entry_point->execution_model ==
          SpvExecutionModelMissKHR);

    const auto rgen = resourceSpirv(
        "rt_shadow_mask_pipeline.rgen.spv");
    const auto id = library.loadFromSpirv(
        rgen, ShaderStage::raygen, "same-api.rgen.spv");
    const auto &bundle = library.get(id);
    REQUIRE(bundle.reflection.selected_entry_point.has_value());
    CHECK(bundle.reflection.selected_entry_point->execution_model ==
          SpvExecutionModelRayGenerationKHR);
    CHECK(bundle.diagnostic_uri == "memory://same-api.rgen.spv");
    REQUIRE_THROWS_WITH(
        library.loadFromSpirv(
            rgen, ShaderStage::miss, "wrong-expected-stage.spv"),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.interface_type_unsupported@1"));
}

TEST_CASE(
    "RT canonical trees ignore names and ids while scalar layout stays exact",
    "[ray-tracing][rt1a][reflection][canonical]") {
    const auto f32 = scalar(
        RayTracingCanonicalTypeKind::floating, 32);
    const auto f64 = scalar(
        RayTracingCanonicalTypeKind::floating, 64);
    const auto vec3 = vectorType(3, f32);
    const RayTracingCanonicalType structure{
        .kind = RayTracingCanonicalTypeKind::structure,
        .elements = {f32, vec3, f64},
    };
    const auto independently_built = structure;
    CHECK(structure == independently_built);
    CHECK(describeRayTracingCanonicalType(structure) ==
          "struct{f32,vec3<f32>,f64}");
    CHECK(rayTracingCanonicalTypeFingerprint(structure) ==
          rayTracingCanonicalTypeFingerprint(independently_built));
    CHECK(rayTracingCanonicalTypeByteSize(structure) == 24);

    const RayTracingCanonicalType array{
        .kind = RayTracingCanonicalTypeKind::fixed_array,
        .element_count = 2,
        .elements = {vec3},
    };
    CHECK(rayTracingCanonicalTypeByteSize(array) == 24);

    auto reordered = structure;
    std::swap(reordered.elements[0], reordered.elements[2]);
    CHECK_FALSE(structure == reordered);
}

TEST_CASE(
    "existing embedded RT pipeline passes actual SPIR-V loads and canonical validation",
    "[ray-tracing][rt1a][load][positive][regression]") {
    const auto embedded = embeddedRtShadowMaskValidatorDigest(
        "engine://rt_shadow_mask_pipeline.rgen.spv",
        "engine://rt_shadow_mask_pipeline.rmiss.spv",
        "engine://rt_shadow_mask_pipeline.rchit.spv");
    const auto source = embeddedRtShadowMaskValidatorDigest(
        "engine://rt_shadow_mask_pipeline.rgen",
        "engine://rt_shadow_mask_pipeline.rmiss",
        "engine://rt_shadow_mask_pipeline.rchit");
    const auto project = embeddedRtShadowMaskValidatorDigest(
        "project://shaders/wp364_reload.rgen.spv",
        "project://shaders/wp364_reload.rmiss.spv",
        "project://shaders/wp364_reload.rchit.spv");
    CHECK(embedded == "rt1a-fnv1a64:22899212d139d671");
    CHECK(source == "rt1a-fnv1a64:d18212b9cb401962");
    CHECK(project == "rt1a-fnv1a64:6cba33c01f3e8dae");
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto compiled_raygen = compiler.compileSource(
        engineResourceOrThrow("rt_shadow_mask_pipeline.rgen"),
        vk::ShaderStageFlagBits::eRaygenKHR,
        "engine://rt_shadow_mask_pipeline.rgen");
    const auto compiled_miss = compiler.compileSource(
        engineResourceOrThrow("rt_shadow_mask_pipeline.rmiss"),
        vk::ShaderStageFlagBits::eMissKHR,
        "engine://rt_shadow_mask_pipeline.rmiss");
    const auto compiled_hit = compiler.compileSource(
        engineResourceOrThrow("rt_shadow_mask_pipeline.rchit"),
        vk::ShaderStageFlagBits::eClosestHitKHR,
        "engine://rt_shadow_mask_pipeline.rchit");
    REQUIRE(compiled_raygen.ok);
    REQUIRE(compiled_miss.ok);
    REQUIRE(compiled_hit.ok);
    const auto compiled_source = rtShadowMaskValidatorDigest(
        compiled_raygen.spirv, compiled_miss.spirv,
        compiled_hit.spirv,
        "engine://rt_shadow_mask_pipeline.rgen",
        "engine://rt_shadow_mask_pipeline.rmiss",
        "engine://rt_shadow_mask_pipeline.rchit");
    CHECK(compiled_source == source);
#endif
}

TEST_CASE(
    "RT1a GLSL negatives traverse ShaderLibrary source loads",
    "[ray-tracing][rt1a][load][negative][source]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto fixture_root = sourceRoot() / "test" / "fixtures" /
                              "wp364";
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};

    SECTION("dynamic trace operand") {
        REQUIRE_THROWS_WITH(
            library.loadFromFile(
                fixture_root / "negative_dynamic.rgen"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.trace_operand_dynamic@1"));
    }

    SECTION("pipeline interface policies") {
        const auto raygen_id = library.loadFromFile(
            fixture_root / "negative_valid.rgen");
        const auto make_occurrence = [&](ShaderBundleId id,
                                         ShaderStage stage,
                                         std::uint32_t group) {
            const auto &bundle = library.get(id);
            return RayTracingShaderOccurrence{
                .stage = stage,
                .group_index = group,
                .diagnostic_uri = bundle.diagnostic_uri,
                .entry = &selectRayTracingEntryPoint(
                    bundle.reflection.ray_tracing_module, stage,
                    "main", bundle.diagnostic_uri),
            };
        };
        const auto raygen = make_occurrence(
            raygen_id, ShaderStage::raygen, 0);

        SECTION("payload mismatch") {
            const auto miss = make_occurrence(
                library.loadFromFile(
                    fixture_root /
                    "negative_payload_mismatch.rmiss"),
                ShaderStage::miss, 1);
            const auto hit = make_occurrence(
                library.loadFromFile(
                    fixture_root / "negative_valid.rchit"),
                ShaderStage::closesthit, 2);
            const std::array misses{miss};
            const std::array hits{hit};
            REQUIRE_THROWS_WITH(
                validateRayTracingPipelineInterfaces(
                    {.raygen = raygen,
                     .misses = misses,
                     .hit_groups = hits},
                    generousFacts()),
                Catch::Matchers::ContainsSubstring(
                    "pelican.ray_tracing.payload_mismatch@1"));
        }

        SECTION("forbidden triangle hit attribute") {
            const auto miss = make_occurrence(
                library.loadFromFile(
                    fixture_root / "negative_valid.rmiss"),
                ShaderStage::miss, 1);
            const auto hit = make_occurrence(
                library.loadFromFile(
                    fixture_root / "negative_vec3.rchit"),
                ShaderStage::closesthit, 2);
            const std::array misses{miss};
            const std::array hits{hit};
            REQUIRE_THROWS_WITH(
                validateRayTracingPipelineInterfaces(
                    {.raygen = raygen,
                     .misses = misses,
                     .hit_groups = hits},
                    generousFacts()),
                Catch::Matchers::ContainsSubstring(
                    "pelican.ray_tracing.hit_attribute_forbidden@1"));
        }
    }
#else
    SUCCEED(
        "source-load negatives require the runtime compiler; direct SPIR-V negatives remain active");
#endif
}

TEST_CASE(
    "RT1a GLSL trace operands select canonical payload trees",
    "[ray-tracing][rt1a][load][payload-operand]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto fixture_root = sourceRoot() / "test" / "fixtures" /
                              "wp364";
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto make_occurrence = [&](ShaderBundleId id,
                                     ShaderStage stage,
                                     std::uint32_t group) {
        const auto &bundle = library.get(id);
        return RayTracingShaderOccurrence{
            .stage = stage,
            .group_index = group,
            .diagnostic_uri = bundle.diagnostic_uri,
            .entry = &selectRayTracingEntryPoint(
                bundle.reflection.ray_tracing_module, stage, "main",
                bundle.diagnostic_uri),
        };
    };

    const std::array misses{
        make_occurrence(
            library.loadFromFile(
                fixture_root / "dual_payload_float.rmiss"),
            ShaderStage::miss, 1),
        make_occurrence(
            library.loadFromFile(
                fixture_root / "dual_payload_vec2.rmiss"),
            ShaderStage::miss, 2),
    };
    const std::array hits{
        make_occurrence(
            library.loadFromFile(
                fixture_root / "dual_payload_float.rchit"),
            ShaderStage::closesthit, 3),
        make_occurrence(
            library.loadFromFile(
                fixture_root / "dual_payload_vec2.rchit"),
            ShaderStage::closesthit, 4),
    };

    const auto valid_raygen = make_occurrence(
        library.loadFromFile(fixture_root / "dual_payload.rgen"),
        ShaderStage::raygen, 0);
    REQUIRE(valid_raygen.entry->trace_calls.size() == 2);
    CHECK(valid_raygen.entry->trace_calls[0].payload_type ==
          scalar(RayTracingCanonicalTypeKind::floating, 32));
    CHECK(valid_raygen.entry->trace_calls[1].payload_type ==
          vectorType(
              2, scalar(RayTracingCanonicalTypeKind::floating, 32)));
    std::size_t payload_interface_count = 0;
    for (const auto &interface : valid_raygen.entry->interfaces) {
        if (interface.storage ==
            RayTracingInterfaceStorage::ray_payload) {
            ++payload_interface_count;
            CHECK_FALSE(interface.location.has_value());
        }
    }
    CHECK(payload_interface_count == 2);
    CHECK(validateRayTracingPipelineInterfaces(
              {.raygen = valid_raygen,
               .misses = misses,
               .hit_groups = hits},
              generousFacts())
              .starts_with("rt1a-fnv1a64:"));

    const auto crosswired_raygen = make_occurrence(
        library.loadFromFile(
            fixture_root / "dual_payload_crosswire.rgen"),
        ShaderStage::raygen, 0);
    try {
        (void)validateRayTracingPipelineInterfaces(
            {.raygen = crosswired_raygen,
             .misses = misses,
             .hit_groups = hits},
            generousFacts());
        FAIL("cross-wired missIndex unexpectedly passed");
    } catch (const std::exception &error) {
        const std::string message = error.what();
        CHECK(message.find(
                  "pelican.ray_tracing.payload_mismatch@1") !=
              std::string::npos);
        CHECK(message.find("miss_group=1") != std::string::npos);
        CHECK(message.find("outgoing=f32") != std::string::npos);
        CHECK(message.find("incoming=vec2<f32>") !=
              std::string::npos);
    }
#else
    SUCCEED(
        "dual-payload GLSL acceptance requires the runtime compiler");
#endif
}

TEST_CASE(
    "RT1a two-hit-group oracle shaders compile and validate from source",
    "[ray-tracing][rt1a][load][positive][oracle]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto fixture_root = sourceRoot() / "test" / "fixtures" /
                              "wp364";
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto miss0_id = library.loadFromFile(
        fixture_root / "oracle.rmiss");
    const auto miss1_id = library.loadFromFile(
        fixture_root / "oracle_recursive.rmiss");
    const std::array recursive_hit_ids{
        library.loadFromFile(fixture_root / "oracle_hit0.rchit"),
        library.loadFromFile(fixture_root / "oracle_hit1.rchit")};
    const std::array shifted_hit_ids{
        library.loadFromFile(
            fixture_root / "oracle_shift_hit0.rchit"),
        library.loadFromFile(
            fixture_root / "oracle_shift_hit1.rchit")};
    const auto make_occurrence = [&](ShaderBundleId id,
                                     ShaderStage stage,
                                     std::uint32_t group) {
        const auto &bundle = library.get(id);
        return RayTracingShaderOccurrence{
            .stage = stage,
            .group_index = group,
            .diagnostic_uri = bundle.diagnostic_uri,
            .entry = &selectRayTracingEntryPoint(
                bundle.reflection.ray_tracing_module, stage, "main",
                bundle.diagnostic_uri),
        };
    };
    const std::array recursive_misses{
        make_occurrence(miss0_id, ShaderStage::miss, 1),
        make_occurrence(miss1_id, ShaderStage::miss, 2)};
    const std::array recursive_hits{
        make_occurrence(recursive_hit_ids[0],
                        ShaderStage::closesthit, 3),
        make_occurrence(recursive_hit_ids[1],
                        ShaderStage::closesthit, 4)};
    const auto recursive_raygen = make_occurrence(
        library.loadFromFile(
            fixture_root / "oracle_offset0.rgen"),
        ShaderStage::raygen, 0);
    CHECK(validateRayTracingPipelineInterfaces(
              {.raygen = recursive_raygen,
               .misses = recursive_misses,
               .hit_groups = recursive_hits},
              generousFacts())
              .starts_with("rt1a-fnv1a64:"));

    const std::array shifted_misses{
        make_occurrence(miss0_id, ShaderStage::miss, 1)};
    const std::array shifted_hits{
        make_occurrence(shifted_hit_ids[0],
                        ShaderStage::closesthit, 2),
        make_occurrence(shifted_hit_ids[1],
                        ShaderStage::closesthit, 3)};
    const auto shifted_raygen = make_occurrence(
        library.loadFromFile(
            fixture_root / "oracle_offset1.rgen"),
        ShaderStage::raygen, 0);
    CHECK(validateRayTracingPipelineInterfaces(
              {.raygen = shifted_raygen,
               .misses = shifted_misses,
               .hit_groups = shifted_hits},
              generousFacts())
              .starts_with("rt1a-fnv1a64:"));
#else
    SUCCEED(
        "oracle source compilation requires the runtime compiler; embedded-SPIR-V positive validation remains active");
#endif
}

TEST_CASE(
    "RT SPIR-V inspection records trace operands and rejects indirect constants",
    "[ray-tracing][rt1a][reflection][load]") {
    const auto words = resourceSpirv(
        "rt_shadow_mask_pipeline.rgen.spv");
    const auto module = inspectRayTracingSpirv(
        words, "engine://rt_shadow_mask_pipeline.rgen.spv");
    const auto &entry = selectRayTracingEntryPoint(
        module, ShaderStage::raygen, "main", "fixture");
    REQUIRE(entry.trace_calls.size() == 1);
    const auto &trace = entry.trace_calls.front();
    CHECK(trace.caller_entry == entry.key);
    CHECK(trace.payload_type ==
          scalar(RayTracingCanonicalTypeKind::floating, 32));
    CHECK(trace.miss_index == 0);
    CHECK(trace.sbt_record_offset == 0);
    CHECK(trace.sbt_record_stride == 0);
    REQUIRE(trace.ray_flags);
    CHECK(*trace.ray_flags == 5u);

    const auto dynamic_flags = inspectRayTracingSpirv(
        traceModule(SpvOpConstant), "memory://constant.spv");
    const auto &dynamic_entry = selectRayTracingEntryPoint(
        dynamic_flags, ShaderStage::raygen, "main",
        "memory://constant.spv");
    REQUIRE(dynamic_entry.trace_calls.size() == 1);
    CHECK_FALSE(dynamic_entry.trace_calls.front().ray_flags);
    CHECK_NOTHROW(inspectRayTracingSpirv(
        traceModule(SpvOpConstantNull), "memory://constant-null.spv"));
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    for (const auto [opcode, producer] :
         {std::pair{SpvOpSpecConstant, "OpSpecConstant*"},
          std::pair{SpvOpCopyObject, "OpCopyObject"}}) {
        REQUIRE_THROWS_WITH(
            inspectRayTracingSpirv(
                traceModule(opcode), "memory://dynamic-trace.spv"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.trace_operand_dynamic@1"));
        try {
            (void)inspectRayTracingSpirv(
                traceModule(opcode), "memory://dynamic-trace.spv");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            CHECK(message.find("memory://dynamic-trace.spv") !=
                  std::string::npos);
            CHECK(message.find("sbtRecordOffset") != std::string::npos);
            CHECK(message.find(producer) != std::string::npos);
            CHECK(message.find("instruction") != std::string::npos);
        }
        REQUIRE_THROWS_WITH(
            library.loadFromSpirv(
                traceModule(opcode), ShaderStage::raygen,
                "actual-load-dynamic.rgen.spv"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.trace_operand_dynamic@1"));
    }
}

TEST_CASE(
    "RT interface arrays and incoming counts fail with fixed named codes",
    "[ray-tracing][rt1a][reflection][negative]") {
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    for (const auto specialization : {false, true}) {
        const auto words = unsupportedArrayModule(specialization);
        REQUIRE_THROWS_WITH(
            inspectRayTracingSpirv(
                words,
                specialization ? "memory://spec-array.spv"
                               : "memory://runtime-array.spv"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.interface_type_unsupported@1"));
        REQUIRE_THROWS_WITH(
            library.loadFromSpirv(
                words, ShaderStage::miss,
                specialization ? "actual-load-spec-array.rmiss.spv"
                               : "actual-load-runtime-array.rmiss.spv"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.interface_type_unsupported@1"));
    }

    auto duplicate = moduleHeader();
    appendMainEntry(duplicate, SpvExecutionModelMissKHR, 10, {4, 8});
    appendInstruction(duplicate, SpvOpDecorate,
                      {4, SpvDecorationLocation, 0});
    appendInstruction(duplicate, SpvOpDecorate,
                      {8, SpvDecorationLocation, 1});
    appendInstruction(duplicate, SpvOpTypeFloat, {1, 32});
    appendInstruction(duplicate, SpvOpTypePointer,
                      {2, SpvStorageClassIncomingRayPayloadKHR, 1});
    appendInstruction(duplicate, SpvOpVariable,
                      {2, 4, SpvStorageClassIncomingRayPayloadKHR});
    appendInstruction(duplicate, SpvOpVariable,
                      {2, 8, SpvStorageClassIncomingRayPayloadKHR});
    REQUIRE_THROWS_WITH(
        inspectRayTracingSpirv(duplicate, "memory://two-incoming.spv"),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.interface_count@1"));
    REQUIRE_THROWS_WITH(
        library.loadFromSpirv(
            duplicate, ShaderStage::miss,
            "actual-load-two-incoming.rmiss.spv"),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.interface_count@1"));

    auto duplicate_attribute = moduleHeader();
    appendMainEntry(duplicate_attribute,
                    SpvExecutionModelClosestHitKHR, 10, {4, 8});
    appendInstruction(duplicate_attribute, SpvOpDecorate,
                      {4, SpvDecorationLocation, 0});
    appendInstruction(duplicate_attribute, SpvOpDecorate,
                      {8, SpvDecorationLocation, 1});
    appendInstruction(duplicate_attribute, SpvOpTypeFloat, {1, 32});
    appendInstruction(duplicate_attribute, SpvOpTypeVector, {2, 1, 2});
    appendInstruction(duplicate_attribute, SpvOpTypePointer,
                      {3, SpvStorageClassHitAttributeKHR, 2});
    appendInstruction(duplicate_attribute, SpvOpVariable,
                      {3, 4, SpvStorageClassHitAttributeKHR});
    appendInstruction(duplicate_attribute, SpvOpVariable,
                      {3, 8, SpvStorageClassHitAttributeKHR});
    REQUIRE_THROWS_WITH(
        library.loadFromSpirv(
            duplicate_attribute, ShaderStage::closesthit,
            "actual-load-two-hit-attributes.rchit.spv"),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.interface_count@1"));
}

TEST_CASE(
    "RT1a reachability compares operand-selected canonical trees",
    "[ray-tracing][rt1a][interface][reachability]") {
    auto fixture = InterfaceFixture{};
    const auto facts = generousFacts();
    const auto digest = validateRayTracingPipelineInterfaces(
        fixture.request(), facts);
    CHECK(digest.starts_with("rt1a-fnv1a64:"));

    SECTION("digest binds resolved trace routing targets and payload trees") {
        std::swap(fixture.raygen.trace_calls[0],
                  fixture.raygen.trace_calls[1]);
        const auto rerouted_digest =
            validateRayTracingPipelineInterfaces(
                fixture.request(), facts);
        CHECK(rerouted_digest != digest);
    }

    SECTION("missIndex chooses the mismatching miss group") {
        fixture.misses[1].interfaces.front() = interfaceVariable(
            RayTracingInterfaceStorage::incoming_ray_payload, 1,
            scalar(RayTracingCanonicalTypeKind::floating, 32));
        REQUIRE_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                fixture.request(), facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.payload_mismatch@1"));
        try {
            (void)validateRayTracingPipelineInterfaces(
                fixture.request(), facts);
        } catch (const std::exception &error) {
            const std::string message = error.what();
            CHECK(message.find("instruction=101") != std::string::npos);
            CHECK(message.find("miss_group=1") != std::string::npos);
            CHECK(message.find("u32") != std::string::npos);
            CHECK(message.find("f32") != std::string::npos);
        }
    }

    SECTION("sbtRecordOffset chooses the mismatching hit group") {
        fixture.hits[1].interfaces.front() = interfaceVariable(
            RayTracingInterfaceStorage::incoming_ray_payload, 1,
            scalar(RayTracingCanonicalTypeKind::floating, 32));
        try {
            (void)validateRayTracingPipelineInterfaces(
                fixture.request(), facts);
            FAIL("mismatching hit group unexpectedly passed");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            CHECK(message.find(
                      "pelican.ray_tracing.payload_mismatch@1") !=
                  std::string::npos);
            CHECK(message.find("hit_group=1") != std::string::npos);
        }
    }

    SECTION("canonical equality does not use fingerprints") {
        fixture.misses[0].interfaces.front().fingerprint =
            "deliberately-unrelated-index";
        CHECK_NOTHROW(validateRayTracingPipelineInterfaces(
            fixture.request(), facts));
    }
}

TEST_CASE(
    "triangle hit attributes and incoming counts enforce absolute rules",
    "[ray-tracing][rt1a][interface][negative]") {
    const auto facts = generousFacts();

    SECTION("vec3 is forbidden even when every stage agrees") {
        auto fixture = InterfaceFixture{};
        fixture.hits[0].interfaces.back() = interfaceVariable(
            RayTracingInterfaceStorage::hit_attribute, 0,
            vectorType(
                3, scalar(RayTracingCanonicalTypeKind::floating, 32)));
        REQUIRE_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                fixture.request(), facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.hit_attribute_forbidden@1"));
    }

    SECTION("synthetic size overrun remains independently testable") {
        auto fixture = InterfaceFixture{};
        fixture.hits[0].interfaces.back().byte_size = 64;
        REQUIRE_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                fixture.request(), facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.hit_attribute_size@1"));
    }

    SECTION("one incoming variable per kind") {
        auto fixture = InterfaceFixture{};
        fixture.misses[0].interfaces.push_back(interfaceVariable(
            RayTracingInterfaceStorage::incoming_ray_payload, 7,
            scalar(RayTracingCanonicalTypeKind::floating, 32)));
        REQUIRE_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                fixture.request(), facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.interface_count@1"));
    }
}

TEST_CASE(
    "RT runtime validators cover recursion dimensions invocations and overflow",
    "[ray-tracing][rt1a][facts]") {
    auto facts = generousFacts();
    facts.max_ray_recursion_depth = 1;
    REQUIRE_THROWS_WITH(
        validateRayTracingRequest(0, facts),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.invalid_recursion@1"));
    REQUIRE_THROWS_WITH(
        validateRayTracingRequest(2, facts),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.recursion_limit@1"));
    CHECK_NOTHROW(validateRayTracingRequest(1, facts));

    facts.max_dispatch_dimensions = {64, 64, 1};
    facts.max_ray_dispatch_invocation_count = 1024;
    REQUIRE_THROWS_WITH(
        validateTraceExtent({65, 1, 1}, facts),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.dispatch_dimension_limit@1"));
    REQUIRE_THROWS_WITH(
        validateTraceExtent({0, 1, 1}, facts),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.dispatch_dimension_limit@1"));
    REQUIRE_THROWS_WITH(
        validateTraceExtent({64, 64, 1}, facts),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.dispatch_invocation_limit@1"));

    facts.max_dispatch_dimensions = {
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
    };
    facts.max_ray_dispatch_invocation_count =
        std::numeric_limits<std::uint64_t>::max();
    REQUIRE_THROWS_WITH(
        validateTraceExtent(
            {std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max(),
             std::numeric_limits<std::uint32_t>::max()},
            facts),
        Catch::Matchers::ContainsSubstring("product overflow"));
}

TEST_CASE(
    "RT1a production delegates reject and observe exact values only while enabled",
    "[ray-tracing][rt1a][wiring][mutation]") {
    auto facts = generousFacts();
    facts.max_ray_recursion_depth = 1;
    facts.max_dispatch_dimensions = {1, 1, 1};
    facts.max_ray_dispatch_invocation_count = 1;

    const auto invoke = [&](RayTracingValidationSite site) {
        switch (site) {
        case RayTracingValidationSite::pipeline_build:
            validateRayTracingPipelineBuild(2, facts);
            return;
        case RayTracingValidationSite::initial_extent:
            validateRayTracingInitialExtent({2, 1, 1}, facts);
            return;
        case RayTracingValidationSite::rebind:
            validateRayTracingRebindExtent({2, 1, 1}, facts);
            return;
        case RayTracingValidationSite::final_command:
            validateRayTracingFinalCommandExtent({2, 1, 1}, facts);
            return;
        }
        FAIL("unknown RT1a validation site");
    };

    for (const auto site : {
             RayTracingValidationSite::pipeline_build,
             RayTracingValidationSite::initial_extent,
             RayTracingValidationSite::rebind,
             RayTracingValidationSite::final_command}) {
        CAPTURE(static_cast<unsigned>(site));
        std::vector<RayTracingValidationObservation> observations;
        const ScopedRayTracingValidationObserver observer{
            [&](const auto &observation) {
                observations.push_back(observation);
            }};

        REQUIRE_THROWS(invoke(site));
        REQUIRE(observations.size() == 1);
        CHECK(observations.front().site == site);
        CHECK(observations.front().facts == facts);
        if (site == RayTracingValidationSite::pipeline_build) {
            CHECK(observations.front().requested_depth == 2);
            CHECK_FALSE(observations.front().extent.has_value());
        } else {
            CHECK(observations.front().extent ==
                  std::array<std::uint32_t, 3>{2, 1, 1});
            CHECK_FALSE(
                observations.front().requested_depth.has_value());
        }

        {
            const ScopedRayTracingValidationDelegateOverrideForTesting
                disabled{site, false};
            CHECK_NOTHROW(invoke(site));
        }
        CHECK(observations.size() == 1);

        REQUIRE_THROWS(invoke(site));
        CHECK(observations.size() == 2);
    }
}

TEST_CASE(
    "CPU RT oracle constructively covers hit miss and recursion sentinels",
    "[ray-tracing][rt1a][oracle][sentinel]") {
    const auto hit_fixture = makeConstructiveRayTracingOracleFixture(
        RayTracingSentinelKind::hit, 0x364u, 8.0f);
    const auto hit = traceRayTracingOracle(
        hit_fixture.ray, hit_fixture.triangles);
    REQUIRE(hit.hit.has_value());
    CHECK(hit.hit->record_id == 0x364u);
    CHECK(rayTracingOracleContainsGpuHit(
        *hit.hit, 8.0f, 0x364u));
    // Deliberate group-handle shift is detected by record identity.
    CHECK_FALSE(rayTracingOracleContainsGpuHit(
        *hit.hit, 8.0f, 0x365u));

    // A replaceable intersector runs a real inv-determinant sign bug. This is
    // deliberately not a post-hoc mutation of the normative result interval.
    std::size_t sign_bug_calls = 0;
    const RayTracingOracleIntersectorForTesting sign_bug_intersector =
        [&](std::uint32_t record_id,
            const RayTracingOracleRay &ray,
            const std::array<RayTracingOracleVec3, 3> &vertices) {
            ++sign_bug_calls;
            const auto subtract_vec = [](
                                          RayTracingOracleVec3 left,
                                          RayTracingOracleVec3 right) {
                return RayTracingOracleVec3{
                    left.x - right.x, left.y - right.y,
                    left.z - right.z};
            };
            const auto cross_vec = [](
                                       RayTracingOracleVec3 left,
                                       RayTracingOracleVec3 right) {
                return RayTracingOracleVec3{
                    left.y * right.z - left.z * right.y,
                    left.z * right.x - left.x * right.z,
                    left.x * right.y - left.y * right.x};
            };
            const auto dot_vec = [](
                                     RayTracingOracleVec3 left,
                                     RayTracingOracleVec3 right) {
                return (left.x * right.x + left.y * right.y) +
                       left.z * right.z;
            };

            const auto e1 = subtract_vec(vertices[1], vertices[0]);
            const auto e2 = subtract_vec(vertices[2], vertices[0]);
            const auto p = cross_vec(ray.direction, e2);
            const auto det = dot_vec(e1, p);
            const auto inv = -1.0 / det; // the injected sign defect
            const auto tvec = subtract_vec(ray.origin, vertices[0]);
            const auto u = dot_vec(tvec, p) * inv;
            const auto q = cross_vec(tvec, e1);
            const auto v = dot_vec(ray.direction, q) * inv;
            const auto t = dot_vec(e2, q) * inv;
            return RayTracingOracleIntersection{
                .record_id = record_id,
                .u = u,
                .v = v,
                .t = t,
                .t_interval = {t, t},
                .inside = u >= 0.0 && v >= 0.0 &&
                          u + v <= 1.0 && t >= 0.0,
            };
        };
    const auto sign_bug =
        traceRayTracingOracleWithIntersectorForTesting(
            hit_fixture.ray, hit_fixture.triangles,
            sign_bug_intersector);
    CHECK_FALSE(sign_bug.hit.has_value());
    CHECK(sign_bug_calls >= 2);

    const auto miss_fixture = makeConstructiveRayTracingOracleFixture(
        RayTracingSentinelKind::miss, 0x777u, 8.0f);
    CHECK_FALSE(traceRayTracingOracle(
                    miss_fixture.ray, miss_fixture.triangles)
                    .hit.has_value());

    CHECK((rayTracingRecursionSentinel(1, 4, 10) ==
           RayTracingRecursionSentinel{1, 11}));
    CHECK((rayTracingRecursionSentinel(2, 4, 10) ==
           RayTracingRecursionSentinel{2, 12}));
}

TEST_CASE(
    "CPU RT oracle outward f32 endpoint rounding accepts the WP364a review hit",
    "[ray-tracing][rt1a][oracle][rounding][regression]") {
    constexpr std::uint32_t record_id = 0x364au;
    const RayTracingOracleRay ray{
        .origin = {0.0, 0.0, 0.0},
        .direction = {0.6000000238418579, 0.0,
                      0.800000011920929},
    };
    const std::array triangles{
        RayTracingOracleTriangle{
            .vertices = {
                RayTracingOracleVec3{5.0, -1.0, 8.0},
                RayTracingOracleVec3{7.0, -1.0, 8.0},
                RayTracingOracleVec3{6.0, 1.0, 8.0},
            },
            .record_id = record_id,
        },
    };
    const auto result = traceRayTracingOracle(ray, triangles);
    REQUIRE(result.hit.has_value());
    CHECK(rayTracingOracleContainsGpuHit(
        *result.hit, 10.0f, record_id));

    // This is the removed implementation: add four f32 ULPs in double
    // without first rounding the double endpoint outward to f32. It rejects
    // the valid 10.0f GPU result from the review counterexample.
    constexpr double review_t = 9.99999985098839;
    const auto endpoint = std::abs(static_cast<float>(review_t));
    const auto endpoint_ulp = static_cast<double>(
        std::nextafter(
            endpoint, std::numeric_limits<float>::infinity()) -
        endpoint);
    auto legacy_hit = *result.hit;
    legacy_hit.t_interval = {
        std::nextafter(
            review_t - RayTracingOracleContract::endpoint_f32_ulp_padding *
                           endpoint_ulp,
            -std::numeric_limits<double>::infinity()),
        std::nextafter(
            review_t + RayTracingOracleContract::endpoint_f32_ulp_padding *
                           endpoint_ulp,
            std::numeric_limits<double>::infinity()),
    };
    CHECK_FALSE(rayTracingOracleContainsGpuHit(
        legacy_hit, 10.0f, record_id));

    CHECK_FALSE(rayTracingOracleContainsGpuHit(
        *result.hit,
        std::nextafter(
            10.0f, std::numeric_limits<float>::infinity()),
        record_id));
}

TEST_CASE(
    "CPU RT oracle exercises both sides of every numeric-contract boundary",
    "[ray-tracing][rt1a][oracle][contract][boundary]") {
    const auto baseline = makeConstructiveRayTracingOracleFixture(
        RayTracingSentinelKind::hit, 0x364u, 8.0f);
    const auto rejects = [&](const RayTracingOracleFixture &fixture,
                             std::string_view expected) {
        const std::string captured_expected{expected};
        CAPTURE(captured_expected);
        REQUIRE_THROWS_WITH(
            validateRayTracingOracleFixture(
                fixture.ray, fixture.triangles),
            Catch::Matchers::ContainsSubstring(
                captured_expected));
    };
    CHECK_NOTHROW(validateRayTracingOracleFixture(
        baseline.ray, baseline.triangles));

    SECTION("finite ray is accepted and non-finite ray is rejected") {
        auto infinite_origin = baseline;
        infinite_origin.ray.origin.x =
            std::numeric_limits<double>::infinity();
        rejects(infinite_origin, "ray contains NaN or infinity");

        auto nan_direction = baseline;
        nan_direction.ray.direction.y =
            std::numeric_limits<double>::quiet_NaN();
        rejects(nan_direction, "ray contains NaN or infinity");
    }

    SECTION("finite transform is accepted and non-finite transform is rejected") {
        auto fixture = baseline;
        fixture.triangles[0].transform.object_to_world[3] =
            std::numeric_limits<double>::infinity();
        rejects(fixture, "object-to-world transform is not finite");
    }

    SECTION("f32-quantized ray direction must remain normalized") {
        auto fixture = baseline;
        fixture.ray.direction.z = 0.5;
        rejects(fixture, "ray direction is not normalized");
    }
    SECTION("world-space coordinate domain") {
        auto fixture = baseline;
        fixture.triangles[0].vertices[0].x = 2048.0;
        rejects(fixture, "world-space coordinate exceeds +/-2^10");
    }
    SECTION("object-to-world determinant") {
        auto fixture = baseline;
        fixture.triangles[0].transform.object_to_world[10] = 5.0e-4;
        rejects(fixture, "object-to-world |det| is below 1e-3");
    }
    SECTION("object-to-world infinity-norm condition") {
        auto fixture = baseline;
        fixture.triangles[0].transform.object_to_world[10] = 5.0e-3;
        rejects(fixture,
                "object-to-world infinity-norm condition exceeds 100");
    }
    SECTION("ray-plane incidence") {
        auto fixture = baseline;
        fixture.ray.direction = {1.0, 0.0, 0.0};
        rejects(fixture,
                "ray-plane incidence is below the 5 degree bound");
    }
    SECTION("edge and vertex barycentric margin") {
        auto fixture = baseline;
        fixture.ray.origin = {0.0, 1.0, 0.0};
        rejects(fixture,
                "intersection is within epsilon_b of an edge or vertex");
    }

    SECTION("minimum triangle area has accepted and rejected sides") {
        const auto area_fixture = [&](double scale) {
            auto fixture = baseline;
            const auto half_extent = static_cast<double>(
                static_cast<float>(
                    std::sqrt(
                        RayTracingOracleContract::minimum_triangle_area /
                        2.0) *
                    scale));
            fixture.triangles[0].vertices = {
                RayTracingOracleVec3{-half_extent, -half_extent, 8.0},
                RayTracingOracleVec3{half_extent, -half_extent, 8.0},
                RayTracingOracleVec3{0.0, half_extent, 8.0},
            };
            return fixture;
        };
        const auto accepted = area_fixture(1.001);
        CHECK_NOTHROW(validateRayTracingOracleFixture(
            accepted.ray, accepted.triangles));
        rejects(area_fixture(0.999),
                "world-space triangle area is below 1e-4");
    }

    SECTION("minimum triangle angle has accepted and rejected sides") {
        const auto angle_fixture = [&](double apex_degrees) {
            auto fixture = baseline;
            const auto radians = apex_degrees *
                                 (std::acos(-1.0) / 180.0);
            const auto height = static_cast<double>(
                static_cast<float>(1.0 / std::tan(radians * 0.5)));
            fixture.triangles[0].vertices = {
                RayTracingOracleVec3{-1.0, -height / 3.0, 8.0},
                RayTracingOracleVec3{1.0, -height / 3.0, 8.0},
                RayTracingOracleVec3{0.0, 2.0 * height / 3.0, 8.0},
            };
            return fixture;
        };
        const auto accepted = angle_fixture(5.001);
        CHECK_NOTHROW(validateRayTracingOracleFixture(
            accepted.ray, accepted.triangles));
        rejects(angle_fixture(4.999),
                "world-space triangle angle is below 5 degrees");
    }

    SECTION("t_min rejects below the lower endpoint") {
        REQUIRE_THROWS_WITH(
            makeConstructiveRayTracingOracleFixture(
                RayTracingSentinelKind::hit, 0x364u, 5.0e-4f),
            Catch::Matchers::ContainsSubstring(
                "hit t is outside [1e-3, 2^12]"));
    }

    SECTION("t_max accepts its endpoint and rejects the next f32 value") {
        const auto t_fixture = [&](float t) {
            auto fixture = baseline;
            fixture.ray.origin = {0.0, 0.0,
                                  -static_cast<double>(t)};
            for (auto &vertex : fixture.triangles[0].vertices) {
                vertex.z = 0.0;
            }
            return fixture;
        };
        const auto accepted = t_fixture(
            static_cast<float>(RayTracingOracleContract::t_max));
        CHECK_NOTHROW(validateRayTracingOracleFixture(
            accepted.ray, accepted.triangles));
        rejects(
            t_fixture(std::nextafter(
                static_cast<float>(RayTracingOracleContract::t_max),
                std::numeric_limits<float>::infinity())),
            "hit t is outside [1e-3, 2^12]");
    }

    SECTION("relative interval width has accepted and rejected sides") {
        const auto interval_intersector = [](
                                               double relative_width) {
            return RayTracingOracleIntersectorForTesting{
                [relative_width](
                    std::uint32_t record_id,
                    const RayTracingOracleRay &,
                    const std::array<RayTracingOracleVec3, 3> &) {
                    constexpr double t = 8.0;
                    const auto half_width =
                        relative_width * t * 0.5;
                    return RayTracingOracleIntersection{
                        .record_id = record_id,
                        .u = 0.25,
                        .v = 0.5,
                        .t = t,
                        .t_interval = {
                            t - half_width, t + half_width},
                        .inside = true,
                    };
                }};
        };
        CHECK_NOTHROW(
            validateRayTracingOracleFixtureWithIntersectorForTesting(
                baseline.ray, baseline.triangles,
                interval_intersector(0.00999)));
        REQUIRE_THROWS_WITH(
            validateRayTracingOracleFixtureWithIntersectorForTesting(
                baseline.ray, baseline.triangles,
                interval_intersector(0.01001)),
            Catch::Matchers::ContainsSubstring(
                "relative t interval width exceeds 1e-2"));
    }

    SECTION("nearest-hit uniqueness uses the CPU interval midpoint") {
        auto fixture = baseline;
        auto neighbor = fixture.triangles.front();
        for (auto &vertex : neighbor.vertices) vertex.z += 0.001;
        neighbor.record_id = 0x365u;
        fixture.triangles.push_back(neighbor);
        rejects(fixture,
                "nearest-hit uniqueness margin is not constructive");
    }
}

} // namespace Pelican
