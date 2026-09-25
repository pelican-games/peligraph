#include "../src/core/communication/editorcommandservice.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/communication/proceduralasseteditor.hpp"
#include "../src/core/communication/renderconfigtransaction.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/renderconfigcandidate.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/previewgraph.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/rendercomponent.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/project/vulkanviewplanning.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <functional>
#include <catch2/generators/catch_generators.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

#ifndef PELICAN_RUNTIME_SHADER_COMPILER
#define PELICAN_RUNTIME_SHADER_COMPILER 0
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

// The component staging directory is named after the exporting process, so the
// sweep test has to say which pid is "ours" the same way the writer does.
std::string currentProcessIdText() {
#ifdef _WIN32
    return std::to_string(static_cast<long long>(_getpid()));
#else
    return std::to_string(static_cast<long long>(getpid()));
#endif
}

// Every fixture below used to create its directory straight in the system temp
// directory -- the one parent every concurrently running test process shares,
// and the one that accumulates thousands of entries between cleanups.  One root
// per process keeps the create/remove traffic of a `-j16` run inside a
// directory nobody else is touching, and leaves a single entry behind if a
// process dies before its destructors run.
class TemporaryRoot {
  public:
    std::filesystem::path path;

    TemporaryRoot() {
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("pelican-renderconfigeditor-" + currentProcessIdText() +
                    "-" + std::to_string(seed) + "-" +
                    std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
            path.clear();
        }
        throw std::runtime_error(
            "failed to allocate the renderconfigeditor temporary root");
    }

    ~TemporaryRoot() {
        if (path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TemporaryRoot(const TemporaryRoot &) = delete;
    TemporaryRoot &operator=(const TemporaryRoot &) = delete;
};

const std::filesystem::path &temporaryRoot() {
    static const TemporaryRoot root;
    return root.path;
}

constexpr std::string_view skyReference =
    "engine://features/sky_ambient.json";
constexpr std::string_view rtShadowReference =
    "engine://features/rt_shadow_mask.json";
constexpr std::string_view debugDrawReference =
    "engine://features/debug_draw.json";

#if PELICAN_RUNTIME_SHADER_COMPILER
constexpr std::string_view buildAvailableReference = skyReference;
constexpr std::string_view buildAvailableFeature = "sky_ambient";
constexpr std::string_view rejectedReference = debugDrawReference;
constexpr std::string_view rejectedFeature = "debug_draw";
#else
constexpr std::string_view buildAvailableReference = rtShadowReference;
constexpr std::string_view buildAvailableFeature = "rt_shadow_mask";
constexpr std::string_view rejectedReference = skyReference;
constexpr std::string_view rejectedFeature = "sky_ambient";
#endif

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error(
            "failed to open test file: " + path.string());
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path,
                std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error(
            "failed to write test file: " + path.string());
    }
    output.write(bytes.data(),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error(
            "failed to finish test file: " + path.string());
    }
}

class TemporaryConfig {
    std::filesystem::path directory_;

  public:
    std::filesystem::path path;

    explicit TemporaryConfig(std::string_view bytes) {
        static std::atomic<std::uint64_t> next{1};
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        const auto &root = temporaryRoot();
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            directory_ = root /
                         ("pelican-wp331-" +
                          std::to_string(seed) + "-" +
                          std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                break;
            }
            directory_.clear();
        }
        if (directory_.empty()) {
            throw std::runtime_error(
                "failed to allocate WP331 temporary directory");
        }
        path = directory_ / "main.json";
        writeBytes(path, bytes);
    }

    ~TemporaryConfig() {
        if (directory_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TemporaryConfig(const TemporaryConfig &) = delete;
    TemporaryConfig &operator=(const TemporaryConfig &) = delete;
};

std::string loadEngineDocument(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error(
            "WP331 test expected an engine document reference");
    }
    return engineResourceOrThrow(reference.substr(prefix.size()));
}

CompiledRenderPipeline compileComposition(
    const RenderFeatureComposeResult &composition) {
    ResolvedRenderPipeline resolved;
    resolved.normalized_config = composition.config;
    resolved.shader_defines = composition.shader_defines;
    resolved.feature_names = composition.feature_names;
    resolved.excluded_feature_names =
        composition.excluded_feature_names;
    resolved.projection_jitter = composition.projection_jitter;
    resolved.feature_instances = composition.feature_instances;
    resolved.surface_resource_contracts =
        composition.surface_resource_contracts;
    resolved.material_routing = composition.material_routing;
    resolved.draw_sort = composition.draw_sort;
    resolved.sample_count_policy =
        compileSampleCountPolicy(composition.config);
    resolved.pipeline_preset = composition.pipeline_preset;
    resolved.pass_provenance = composition.pass_provenance;
    resolved.resource_provenance =
        composition.resource_provenance;
    resolved.used_features = composition.used_features;
    return compileRenderPipeline(resolved);
}

struct ActualCpuRenderRuntime {
    struct Prepared {
        std::vector<std::string> enabled_feature_names;
        Json frame_plan;
    };

    RenderConfigRuntimeSnapshot snapshot;
    Json frame_plan;
    bool fail_before_source_commit = false;
    bool fail_after_source_commit = false;
    // G-4: stands in for the renderer's live-material check of the candidate
    // generation; what it throws comes back as the apply error.
    std::function<void(const RenderConfigCandidateDocumentSet &)>
        validate_candidate;
    std::string post_commit_error;
    std::size_t apply_calls = 0;
    bool ray_query_available = true;
    bool runtime_shader_compiler_enabled =
        PELICAN_RUNTIME_SHADER_COMPILER != 0;
    bool module_creation_frozen = false;
    std::set<std::string, std::less<>> initialized_modules;
    PathResolver *path_resolver = nullptr;

    RenderFeatureRuntimeAvailabilityEnvironment
    availabilityEnvironment() const {
        TargetEndpoint endpoint{
            .id = "device:0",
            .kind = TargetEndpointKind::vulkan_device,
            .capabilities = {
                "pelican.vulkan.graphics@1",
                "pelican.vulkan.sampled_image@1",
                "pelican.vulkan.storage_buffer@1",
                "pelican.vulkan.transfer_copy@1",
            },
        };
        if (ray_query_available) {
            endpoint.capabilities.push_back(
                std::string{vulkanRayQueryCapability});
        }
        return {
            .runtime_shader_compiler_enabled =
                runtime_shader_compiler_enabled,
            .target_endpoint = std::move(endpoint),
            .runtime_module_creation_frozen =
                module_creation_frozen,
            .runtime_module_initialized =
                [this](std::string_view module) {
                    return initialized_modules.contains(module);
                },
        };
    }

    RenderFeatureRuntimeAvailability availability(
        std::string_view name,
        const Json &document) const {
        return evaluateRenderFeatureRuntimeAvailability(
            name, document, availabilityEnvironment());
    }

    Prepared prepare(
        std::string_view bytes,
        std::function<std::string(std::string_view)> load_document =
            loadEngineDocument) const {
        auto composition = composeRenderFeatureConfig(
            Json::parse(bytes),
            RenderFeatureComposeDependencies{
                .load_feature_json = load_document,
                .runtime_shader_compiler_enabled =
                    runtime_shader_compiler_enabled,
                .load_pipeline_json = load_document,
                .validate_feature =
                    [this](std::string_view name,
                           const Json &document) {
                        requireRenderFeatureRuntimeAvailability(
                            name, document,
                            availabilityEnvironment());
                    },
            });
        // Production lowers output-relative targets against the active
        // window extent before frame planning.  Supply the same concrete
        // display extent in this CPU-only runtime so snapshot byte sizes are
        // real rather than bypassing the snapshot validation.
        for (auto &target :
             composition.config.at("render_targets")) {
            if (target.value("format_class", std::string{}) ==
                "display") {
                target["width"] = 1280;
                target["height"] = 720;
            }
        }
        SubgraphReplacementRegistry replacements;
        composition.config = resolveTaggedSubgraphReplacements(
            composition.config, replacements.snapshot()).config;
        auto graphs = parseFrameGraphDefinitionsFromConfigJson(
            composition.config);
        if (const auto order = composition.config.find("frame_node_order"); order != composition.config.end())
            detail::applyFrameNodeOrder(graphs, *order);
        const auto graph = std::find_if(
            graphs.begin(), graphs.end(), [](const auto &candidate) {
                return candidate.name == "main_render";
            });
        if (graph == graphs.end()) {
            throw std::runtime_error(
                "hybrid_v1 did not produce main_render");
        }
        const auto compiled = compileComposition(composition);
        return Prepared{
            .enabled_feature_names = composition.feature_names,
            .frame_plan = framePlanToJson(
                planFrameGraph(*graph), &compiled),
        };
    }

    void initialize(std::string_view bytes) {
        module_creation_frozen = false;
        auto prepared = prepare(
            bytes, path_resolver
                       ? std::function<std::string(std::string_view)>{
                             [this](std::string_view reference) {
                                 return path_resolver->loadText(reference);
                             }}
                       : std::function<std::string(std::string_view)>{
                             loadEngineDocument});
        initialized_modules.clear();
        for (const auto &requirement :
             renderFeatureRuntimeModuleRequirements()) {
            if (std::find(
                    prepared.enabled_feature_names.begin(),
                    prepared.enabled_feature_names.end(),
                    requirement.feature) ==
                prepared.enabled_feature_names.end()) {
                continue;
            }
            for (const auto &module : requirement.modules) {
                initialized_modules.emplace(module.module);
            }
        }
        module_creation_frozen = true;
        snapshot = {
            .published_generation = 37,
            .enabled_feature_names =
                std::move(prepared.enabled_feature_names),
        };
        frame_plan = std::move(prepared.frame_plan);
    }

    RenderConfigRuntimeApplyResult apply(
        RenderConfigCandidateDocumentSet documents,
        const RenderConfigSourceCommit &source_commit) {
        ++apply_calls;
        Prepared prepared;
        try {
            const auto load_document =
                path_resolver
                    ? std::function<std::string(std::string_view)>{
                          [this, &documents](std::string_view reference) {
                              return loadRenderConfigCandidateText(
                                  documents, *path_resolver, reference);
                          }}
                    : std::function<std::string(std::string_view)>{
                          loadEngineDocument};
            prepared = prepare(documents.rootDocument().bytes,
                               load_document);
        } catch (const std::exception &error) {
            return {.error = error.what()};
        }
        if (fail_before_source_commit) {
            return {.error = "injected preflight failure"};
        }
        if (validate_candidate) {
            try {
                validate_candidate(documents);
            } catch (const std::exception &error) {
                // As the renderer does: the text, and what it was read from.
                return {.error = error.what(),
                        .exception = std::current_exception()};
            }
        }
        source_commit();
        if (fail_after_source_commit) {
            return {.error =
                        "injected failure after source replacement"};
        }
        ++snapshot.published_generation;
        snapshot.enabled_feature_names =
            std::move(prepared.enabled_feature_names);
        frame_plan = std::move(prepared.frame_plan);
        return {
            .committed = true,
            .published_generation =
                snapshot.published_generation,
            .post_commit_error = post_commit_error,
        };
    }

    RenderConfigRuntimeApplyResult apply(
        std::string bytes,
        const RenderConfigSourceCommit &source_commit) {
        const auto digest = renderConfigSourceDigest(bytes);
        return apply(
            RenderConfigCandidateDocumentSet{
                "test://root",
                {{.reference = "test://root",
                  .normalized_reference = "test://root",
                  .path = "main.json",
                  .operation = RenderConfigDocumentOperation::replace,
                  .expected =
                      {.existence =
                           RenderConfigDocumentExistence::present,
                       .digest = digest},
                  .bytes = std::move(bytes)}}},
            source_commit);
    }
};

class TemporaryAnimgraphProject {
    std::filesystem::path directory_;

  public:
    std::filesystem::path root;
    std::filesystem::path render_config;

    TemporaryAnimgraphProject() {
        static std::atomic<std::uint64_t> next{1};
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            directory_ = temporaryRoot() /
                         ("pelican-wp334b-animgraph-" +
                          std::to_string(seed) + "-" +
                          std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                break;
            }
            directory_.clear();
        }
        if (directory_.empty()) {
            throw std::runtime_error(
                "failed to allocate WP334b temporary project");
        }
        root = directory_;
        std::filesystem::create_directories(root / "passes");
        const auto shipped =
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
            "animgraph_demo";
        std::filesystem::copy_file(
            shipped / "project.json", root / "project.json",
            std::filesystem::copy_options::none);
        render_config = root / "passes" / "main.json";
        std::filesystem::copy_file(
            shipped / "passes" / "main.json", render_config,
            std::filesystem::copy_options::none);
    }

    ~TemporaryAnimgraphProject() {
        if (directory_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TemporaryAnimgraphProject(const TemporaryAnimgraphProject &) = delete;
    TemporaryAnimgraphProject &operator=(
        const TemporaryAnimgraphProject &) = delete;
};

class TemporaryDirectory {
  public:
    std::filesystem::path path;

    explicit TemporaryDirectory(std::string_view label) {
        static std::atomic<std::uint64_t> next{1};
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path = temporaryRoot() /
                   ("pelican-" + std::string{label} + "-" +
                    std::to_string(seed) + "-" +
                    std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
            path.clear();
        }
        throw std::runtime_error("failed to allocate a temporary directory");
    }

    ~TemporaryDirectory() {
        if (path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
};

std::unique_ptr<RenderConfigEditorService> makeService(
    TemporaryConfig &source, ActualCpuRenderRuntime &runtime,
    std::function<RenderConfigEditorGateObservation()> gate = {},
    std::function<ValueGraph::PublishedInstanceInputs()> graph_values = {}) {
    if (!gate) {
        gate = [] {
            return RenderConfigEditorGateObservation{};
        };
    }
    return std::make_unique<RenderConfigEditorService>(
        RenderConfigEditorDependencies{
            .source_reference = "passes/main.json",
            .source_path = source.path,
            .source_bytes = readBytes(source.path),
            .gate = std::move(gate),
            .runtime_snapshot = [&runtime] {
                return runtime.snapshot;
            },
            .apply_candidate =
                [&runtime](
                    RenderConfigCandidateDocumentSet candidate,
                    RenderConfigSourceCommit commit,
                    RenderConfigRuntimeApplyCompletion) {
                    return runtime.apply(
                        std::move(candidate), commit);
                },
            .feature_catalog = [&runtime] {
                return enumerateEngineRenderFeatureDocuments(
                    [&runtime](std::string_view name,
                               const Json &document) {
                        return runtime.availability(
                            name, document);
                    });
            },
            .graph_values = std::move(graph_values),
        });
}

std::unique_ptr<RenderConfigEditorService> makeProjectService(
    TemporaryAnimgraphProject &project, PathResolver &resolver,
    ActualCpuRenderRuntime &runtime,
    std::function<void(const RenderConfigCandidateDocumentSet &)>
        preflight_graph_variants = {}) {
    runtime.path_resolver = &resolver;
    // This producer models an already-running animgraph_demo instance. Shader
    // availability has its own ON/OFF gate tests; keep this lifecycle fixture
    // focused on the document-set preflight and publish path in both builds.
    runtime.runtime_shader_compiler_enabled = true;
    runtime.initialize(readBytes(project.render_config));
    return std::make_unique<RenderConfigEditorService>(
        RenderConfigEditorDependencies{
            .source_reference = "passes/main.json",
            .source_path = project.render_config,
            .source_bytes = readBytes(project.render_config),
            .project_root = project.root,
            .normalize_reference = [&resolver](std::string_view reference) {
                return resolver.normalizedReference(reference);
            },
            .resolve_document_path =
                [&resolver](std::string_view reference) {
                    const auto resolved = resolver.resolveProjectRef(reference);
                    const auto *path =
                        std::get_if<std::filesystem::path>(&resolved);
                    if (path == nullptr) {
                        throw std::runtime_error(
                            "WP334b authored document did not resolve to a project file");
                    }
                    return *path;
                },
            .gate = [] { return RenderConfigEditorGateObservation{}; },
            .runtime_snapshot = [&runtime] { return runtime.snapshot; },
            .apply_candidate =
                [&runtime](RenderConfigCandidateDocumentSet documents,
                           RenderConfigSourceCommit commit,
                           RenderConfigRuntimeApplyCompletion) {
                    return runtime.apply(std::move(documents), commit);
                },
            .resolve_authoring_context =
                [&resolver, &runtime](
                    const RenderConfigCandidateDocumentSet &documents) {
                    const auto loader =
                        [&resolver, &documents](std::string_view reference) {
                            return loadRenderConfigCandidateText(
                                documents, resolver, reference);
                        };
                    std::optional<std::string> preset_bytes;
                    auto composition = composeRenderFeatureConfig(
                        Json::parse(documents.rootDocument().bytes),
                        RenderFeatureComposeDependencies{
                            .load_feature_json = loader,
                            .runtime_shader_compiler_enabled =
                                runtime.runtime_shader_compiler_enabled,
                            .load_pipeline_json = [&](std::string_view reference) {
                                preset_bytes = loader(reference);
                                return *preset_bytes;
                            },
                            .validate_feature =
                                [&runtime](std::string_view name,
                                           const Json &feature) {
                                    requireRenderFeatureRuntimeAvailability(
                                        name, feature,
                                        runtime.availabilityEnvironment());
                                },
                        });
                    return ResolvedRenderConfigAuthoringContext{
                        .config = std::move(composition.config),
                        .pipeline_preset =
                            std::move(composition.pipeline_preset),
                        .pass_provenance =
                            std::move(composition.pass_provenance),
                        .pipeline_preset_bytes = std::move(preset_bytes),
                        .resource_provenance =
                            std::move(composition.resource_provenance),
                    };
                },
            .preflight_graph_variants = std::move(preflight_graph_variants),
            .feature_catalog = [&runtime] {
                return enumerateEngineRenderFeatureDocuments(
                    [&runtime](std::string_view name,
                               const Json &document) {
                        return runtime.availability(name, document);
                    });
            },
        });
}

Json submitAndCommit(RenderConfigEditorService &service,
                     std::string_view digest,
                     Json operations) {
    const auto accepted = service.editRenderFeatures(
        {{"base_source_digest", digest},
         {"operations", std::move(operations)}});
    REQUIRE(accepted.at("status") == "accepted");
    const auto ticket =
        accepted.at("ticket").get<std::string>();
    service.commitPending();
    return service.getResult({{"ticket", ticket}});
}

Json operation(std::string_view op,
               std::string_view reference) {
    return Json::array({
        {{"op", op}, {"feature", reference}},
    });
}

bool hasProvider(const Json &frame_plan,
                 std::string_view feature_name) {
    return std::ranges::any_of(
        frame_plan.at("nodes"), [&](const auto &node) {
            return node.value("provider_feature", std::string{}) ==
                   feature_name;
        });
}

bool containsName(const std::vector<std::string> &values,
                  std::string_view name) {
    return std::find(values.begin(), values.end(), name) !=
           values.end();
}

std::string graphRootFixture(std::string_view stem) {
    return readBytes(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                     "fixtures" / "graph_root_example" / "passes" /
                     (std::string{stem} + ".json"));
}

std::vector<std::string> rootKeys(const Json &document) {
    std::vector<std::string> keys;
    for (auto entry = document.begin(); entry != document.end(); ++entry)
        keys.push_back(entry.key());
    return keys;
}

// P-2a-4a acceptance 2: the edit landed in `rendering.<key>`, no root member
// was added, and every other byte of the document is where it was.  The byte
// half matters: a semantic-only check passes for a document that was
// reformatted, and a root-fixed locator would be caught only by the key set.
void requireOnlySectionKeyChanged(const std::string &before,
                                  const std::string &after,
                                  const char *key) {
    const auto before_document = Json::parse(before);
    const auto after_document = Json::parse(after);
    INFO("after: " << after);
    REQUIRE(rootKeys(after_document) == rootKeys(before_document));
    REQUIRE(after_document.contains("rendering"));
    auto before_section = before_document.at("rendering");
    auto after_section = after_document.at("rendering");
    REQUIRE(after_section.at(key) != before_section.value(key, Json()));
    before_section.erase(key);
    after_section.erase(key);
    REQUIRE(after_section == before_section);
    for (const auto *root_key : {"schema", "version", "definitions", "nodes",
                                 "outputs"}) {
        REQUIRE(after_document.at(root_key) == before_document.at(root_key));
    }
    const auto shared = static_cast<std::size_t>(
        std::mismatch(before.begin(), before.end(), after.begin(), after.end())
            .first -
        before.begin());
    // Everything up to the section member -- the graph's own keys included --
    // is byte-identical, so the change cannot have been made at the root.
    const auto section_member = before.find("\"rendering\"");
    REQUIRE(section_member != std::string::npos);
    REQUIRE(shared > section_member);
}

std::string emptyHybridConfig() {
    return R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
    "features": []
  }
}
)json";
}

} // namespace

#if PELICAN_WITH_RPC
TEST_CASE("Callable editor normalizes one shader and rejects invalid record values",
          "[render-config-editor][rt7][callable-records]") {
    auto config = Json::parse(emptyHybridConfig());
    Json task{{"name", "callable_task"}};
    task["ray_tracing"] = {
        {"callable", "shaders/gain.rcall"},
        {"callable_record", Json::array({{{"name", "count"}, {"type", "u32"}}})},
        {"callable_records", Json::array({{{"count", 1}}})}};
    // MG-2b: the root is a graph root. This service has no authoring-context
    // resolver, so it reads tasks from the section it was handed; the document
    // is never composed here (a composed graph root takes its tasks from a
    // feature -- the lowering refuses `rendering.compute_tasks`).
    config.at("rendering")["compute_tasks"] = Json::array({task});
    TemporaryConfig source{config.dump(2)};
    ActualCpuRenderRuntime runtime;
    auto service = makeService(source, runtime);
    const auto context = service->getRayCallableRecords(Json::object());
    REQUIRE(context.at("tasks").size() == 1);
    CHECK(context.at("tasks")[0].at("callables") == Json::array({"shaders/gain.rcall"}));
    const auto rejected = service->editRayCallableRecords({
        {"base_source_digest", context.at("source_digest").at("hex")},
        {"task_records", {{"callable_task", Json::array({{{"count", -1}}})}}}});
    CHECK(rejected.at("status") == "rejected");
    CHECK(rejected.at("error").at("code") == "invalid_callable_records");
    CHECK(Json::parse(readBytes(source.path)) == config);
}

TEST_CASE("owned graph instances attach and detach through source publication", "[render-config-editor][graph-attachment]") {
    const Json unit{{"nodes", Json::array({
        {{"id", "gain"}, {"op", "graph.input"}, {"default", 2}},
        {{"id", "buffer"}, {"op", "render.buffer"},
         {"inputs", {{"value", {{"node", "gain"}, {"port", "value"}}}}},
         {"parameters", {{"element", "float"}}}, {"outputs", {"resource"}}}})},
        {"outputs", {{"value", {{"node", "gain"}, {"port", "value"}}}}}};
    const Json original{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", {{"unit", unit}}},
        {"nodes", Json::array()}, {"outputs", Json::object()}, {"rendering", Json::parse(emptyHybridConfig()).at("rendering")}};
    TemporaryConfig source{original.dump(2)};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);
    const auto instance = [](const char *id, int object, double gain) {
        return Json{{"id", id}, {"op", "graph.instance"}, {"definition", "unit"},
            {"owner", {{"scene", "main"}, {"object", object}}}, {"parameters", {{"gain", gain}}}};
    };
    const auto edit = [&](Json attach, Json detach = Json::array()) {
        return service->editGraphInstances({{"base_source_digest", service->getGraphInstances(Json::object()).at("source_digest").at("hex")},
            {"attach", std::move(attach)}, {"detach", std::move(detach)}});
    };
    const auto commit = [&](const nlohmann::ordered_json &submission) {
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        return service->getResult({{"ticket", submission.at("ticket")}});
    };
    const auto left = instance("left", 1, 3);
    const auto right = instance("right", 2, 7);
    REQUIRE(commit(edit(Json::array({left, right}))).at("committed") == true);
    const auto attached = Json::parse(readBytes(source.path));
    REQUIRE(attached.at("definitions") == original.at("definitions"));
    REQUIRE(attached.at("rendering") == original.at("rendering"));
    const ValueGraph::Graph values{attached.dump()};
    REQUIRE(values.parameterDefaults().at("left/gain") == 3);
    REQUIRE(values.parameterDefaults().at("right/gain") == 7);
    REQUIRE(values.renderBufferBindings().size() == 2);
    const auto generation = runtime.snapshot.published_generation;
    const auto saved = readBytes(source.path);
    SECTION("detach saves and reopens with the sibling intact") {
        service = makeService(source, runtime);
        REQUIRE(service->getGraphInstances(Json::object()).at("instances") == Json::array({left, right}));
        REQUIRE(commit(edit(Json::array(), Json::array({"left"}))).at("committed") == true);
        const auto remaining = Json::parse(readBytes(source.path));
        REQUIRE(remaining.at("nodes") == Json::array({right}));
        const ValueGraph::Graph next{remaining.dump()};
        REQUIRE(next.renderBufferBindings().size() == 1);
        REQUIRE(next.renderBufferBindings()[0].target == "right/buffer");
    }
    SECTION("invalid definitions owners duplicates and dangling references preserve the source") {
        auto invalid = instance("other", 3, 4);
        invalid["definition"] = "missing";
        REQUIRE(edit(Json::array({invalid})).at("status") == "rejected");
        invalid = instance("other", 0, 4);
        REQUIRE(edit(Json::array({invalid})).at("status") == "rejected");
        REQUIRE(edit(Json::array({left})).at("status") == "rejected");
        REQUIRE(edit(Json::array(), Json::array({"missing"})).at("status") == "rejected");
        auto dependent = instance("dependent", 3, 4);
        dependent.erase("parameters");
        dependent["inputs"] = {{"gain", {{"node", "left"}, {"port", "value"}}}};
        REQUIRE(commit(edit(Json::array({dependent}))).at("committed") == true);
        REQUIRE(edit(Json::array(), Json::array({"left"})).at("status") == "rejected");
        REQUIRE(Json::parse(readBytes(source.path)).at("nodes").size() == 3);
    }
    SECTION("runtime failure rolls back a detached source") {
        runtime.fail_after_source_commit = true;
        REQUIRE(commit(edit(Json::array(), Json::array({"left"}))).at("committed") == false);
        REQUIRE(readBytes(source.path) == saved);
        REQUIRE(runtime.snapshot.published_generation == generation);
    }
}

TEST_CASE("instance inputs change supply atomically and report what was drawn",
          "[render-config-editor][graph-instance-inputs]") {
    // No shipped project has a pelican.graph document, so the acceptance is
    // taken on a fixture: a definition with one defaulted and one undefaulted
    // input, and one owned attachment that stores both.
    const auto fixture_path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
        "fixtures" / "graph_instance_inputs" / "passes" / "main.json";
    TemporaryConfig source{readBytes(fixture_path)};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    // The snapshot the draw path leaves behind, produced by really publishing
    // the document under test. The service never evaluates a graph itself.
    ValueGraph::PublishedInstanceInputs snapshot;
    ValueGraph::ParameterValues overrides;
    std::size_t evaluations = 0;
    const auto draw = [&] {
        const ValueGraph::Graph graph{readBytes(source.path)};
        auto state = graph.createState();
        auto published = graph.publish(GameContext{},
            {{"time", 0.5}, {"delta_time", 0.5}, {"scene_time", 0.5}}, {},
            [](std::string_view, std::span<const float>) { return true; },
            overrides, {}, &state);
        ++evaluations;
        snapshot = {.published = true, .generation = 7, .frame = 11,
                    .inputs = std::move(published.instance_inputs)};
    };
    draw();
    auto service = makeService(source, runtime, {}, [&] { return snapshot; });
    const auto edit = [&](Json ops) {
        return service->editGraphDocument(
            {{"base_source_digest", service->getGraphInstances(Json::object()).at("source_digest").at("hex")},
             {"ops", std::move(ops)}});
    };
    const auto commit = [&](const nlohmann::ordered_json &submission) {
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        return service->getResult({{"ticket", submission.at("ticket")}});
    };
    const auto instance = [&] {
        const auto instances = service->getGraphInstances(Json::object()).at("instances");
        REQUIRE(instances.size() == 1);
        return instances.at(0);
    };
    const auto portValue = [&](const char *port) {
        const auto values = service->getGraphValues({{"instance", "left"}});
        for (const auto &entry : values.at("inputs"))
            if (entry.at("port") == port) return entry;
        FAIL("no reported value for " + std::string{port});
        return nlohmann::ordered_json{};
    };

    SECTION("a stored value is reported with its origin and does not move when read") {
        const auto values = service->getGraphValues({{"instance", "left"}});
        REQUIRE(values.at("published") == true);
        REQUIRE(values.at("generation") == 7);
        REQUIRE(values.at("frame") == 11);
        REQUIRE(values.at("inputs").size() == 2);
        REQUIRE(portValue("gain").at("value") == 2.5);
        REQUIRE(portValue("gain").at("source") == "parameter");
        REQUIRE(portValue("gain").at("parameter") == "left/gain");
        REQUIRE(portValue("bias").at("value") == 5);
        // Reading is a map lookup. Only the draw path evaluates, so no number
        // of reads can advance a clock or consume an event.
        const auto drawn = evaluations;
        for (int i = 0; i < 5; ++i) (void)service->getGraphValues({{"instance", "left"}});
        REQUIRE(evaluations == drawn);
        // A run-time override is a fourth origin, not a fourth kind of value.
        overrides = {{"left/gain", 9}};
        draw();
        REQUIRE(portValue("gain").at("value") == 9);
        REQUIRE(portValue("gain").at("source") == "override");
    }

    SECTION("an authored node label survives the loader and a commit") {
        // Studio's node canvas heads a node with its `label` (N-1b). The
        // engine has to carry the key without knowing it: this whole case
        // already loads, composes and publishes a document whose definition
        // node holds one, and an edit must not drop it on the way back out.
        const auto labelOf = [&](const char *id) {
            const auto document = Json::parse(readBytes(source.path));
            for (const auto &node : document.at("definitions").at("gain_curve").at("nodes"))
                if (node.at("id") == id) return node.value("label", std::string{});
            FAIL("no such definition node: " + std::string{id});
            return std::string{};
        };
        REQUIRE(labelOf("buffer") == "pulse buffer");
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_input"}, {"node", "left"},
            {"port", "bias"}, {"source", {{"node", "clock"}, {"port", "value"}}}}})))
                .at("committed") == true);
        REQUIRE(labelOf("buffer") == "pulse buffer");
        // And the document that came back still draws.
        draw();
        REQUIRE(portValue("bias").at("source") == "connected");
    }

    SECTION("control with a curve lands as one op list and comes back off it") {
        // The four ops the Inspector's "control with a curve" sends: reuse the
        // document's scene clock, a playback, a constant two-key curve, and the
        // edge itself. One call, so one ticket and one CAS.
        Json ops = Json::array();
        Json playback{{"id", "left__gain_playback"}, {"op", "value.playback"},
            {"inputs", {{"elapsed", {{"node", "clock"}, {"port", "value"}}}}},
            {"outputs", Json::array({"value"})}};
        Json curve{{"id", "left__gain_curve"}, {"op", "value.curve"},
            {"inputs", {{"time", {{"node", "left__gain_playback"}, {"port", "value"}}}}},
            {"parameters", {{"keys", Json::array({Json{{"time", 0}, {"value", 2.5}},
                                                  Json{{"time", 1}, {"value", 2.5}}})}}},
            {"outputs", Json::array({"value"})}};
        ops.push_back({{"op", "add_node"}, {"node", playback}});
        ops.push_back({{"op", "add_node"}, {"node", curve}});
        ops.push_back({{"op", "set_input"}, {"node", "left"}, {"port", "gain"},
                       {"source", {{"node", "left__gain_curve"}, {"port", "value"}}}});
        REQUIRE(commit(edit(ops)).at("committed") == true);
        const auto connected = instance();
        REQUIRE(connected.at("inputs").at("gain") ==
                Json{{"node", "left__gain_curve"}, {"port", "value"}});
        // The stored value was not removed, so there is a way back.
        REQUIRE(connected.at("parameters").at("gain") == 2.5);
        draw();
        REQUIRE(portValue("gain").at("source") == "connected");
        REQUIRE(portValue("gain").at("connected").at("node") == "left__gain_curve");
        REQUIRE(portValue("gain").at("value") == 2.5);

        // Removing the edge puts the port back on its stored value.
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_input"}, {"node", "left"},
            {"port", "gain"}, {"source", nullptr}}}))).at("committed") == true);
        // The last edge left with the object that held it.
        REQUIRE_FALSE(instance().value("inputs", Json::object()).contains("gain"));
        draw();
        REQUIRE(portValue("gain").at("source") == "parameter");
        REQUIRE(portValue("gain").at("value") == 2.5);

        // The same for the input the definition gives no default: the stored
        // value is the only way back, and it is still there.
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_input"}, {"node", "left"},
            {"port", "bias"}, {"source", {{"node", "left__gain_curve"}, {"port", "value"}}}}})))
                .at("committed") == true);
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_input"}, {"node", "left"},
            {"port", "bias"}, {"source", nullptr}}}))).at("committed") == true);
        REQUIRE(instance().at("parameters").at("bias") == 5);
    }

    SECTION("a rejected op writes nothing and is named") {
        const auto saved = readBytes(source.path);
        const auto reject = [&](Json ops, const char *code) {
            const auto result = edit(std::move(ops));
            REQUIRE(result.at("status") == "rejected");
            CHECK(result.at("error").at("code") == code);
            CHECK(readBytes(source.path) == saved);
        };
        reject(Json::array({Json{{"op", "add_node"}, {"node", {{"id", "clock"}, {"op", "value.constant"},
            {"parameters", {{"value", 1}}}, {"outputs", Json::array({"value"})}}}}}),
            "graph.node_duplicate@1");
        reject(Json::array({Json{{"op", "remove_node"}, {"node", "absent"}}}), "graph.node_missing@1");
        reject(Json::array({Json{{"op", "set_input"}, {"node", "left"}, {"port", "missing"},
            {"source", {{"node", "clock"}, {"port", "value"}}}}}), "graph.input_unknown@1");
        // A node another node still reads cannot be removed silently, even
        // when the reader was added by an earlier op in the same list.
        reject(Json::array({Json{{"op", "add_node"}, {"node", {{"id", "later"}, {"op", "value.playback"},
                {"inputs", {{"elapsed", {{"node", "clock"}, {"port", "value"}}}}},
                {"outputs", Json::array({"value"})}}}},
            Json{{"op", "remove_node"}, {"node", "clock"}}}),
            "graph.output_referenced@1");
        // Inside a definition, and across an instance path, is out of range.
        reject(Json::array({Json{{"op", "set_node_parameters"}, {"node", "sum"},
            {"parameters", Json::object()}}}), "graph.definition_immutable@1");
        reject(Json::array({Json{{"op", "set_input"}, {"node", "left/sum"}, {"port", "a"},
            {"source", {{"node", "clock"}, {"port", "value"}}}}}), "graph.definition_immutable@1");
        // The whole list is one transaction: the first op is valid on its own
        // and must still leave nothing behind.
        reject(Json::array({Json{{"op", "add_node"}, {"node", {{"id", "spare"}, {"op", "value.constant"},
                {"parameters", {{"value", 1}}}, {"outputs", Json::array({"value"})}}}},
            Json{{"op", "remove_node"}, {"node", "absent"}}}), "graph.node_missing@1");
        REQUIRE(readBytes(source.path) == saved);
    }

    SECTION("a definition body is edited in its own scope") {
        // N-2: every edit op may name a scope. Without one it still means the
        // root document, so the four ops that existed before keep their reach,
        // and `definitions` now travels back out with the edit.
        // The reply is a value: a range-for over `reply.at(...)` would iterate
        // a destroyed temporary, so the whole of it is bound first.
        const auto definitionBody = [&] {
            return Json(service->getGraphInstances(Json::object())
                            .at("definitions")
                            .at("gain_curve"));
        };
        const auto definitionNode = [&](const char *id) {
            const auto body = definitionBody();
            for (const auto &node : body.at("nodes"))
                if (node.at("id") == id) return Json(node);
            FAIL("no such definition node: " + std::string{id});
            return Json{};
        };
        const auto scoped = [](Json op) {
            op["scope"] = "gain_curve";
            return Json::array({std::move(op)});
        };

        // add_node lands inside the definition, not beside it in the root.
        REQUIRE(commit(edit(scoped({{"op", "add_node"},
            {"node", {{"id", "knob"}, {"op", "value.constant"},
                      {"parameters", {{"value", 1}}},
                      {"outputs", Json::array({"value"})}}}})))
                .at("committed") == true);
        REQUIRE(commit(edit(scoped({{"op", "add_node"},
            {"node", {{"id", "scaled"}, {"op", "value.multiply"},
                      {"inputs", {{"a", {{"node", "sum"}, {"port", "value"}}},
                                  {"b", {{"node", "knob"}, {"port", "value"}}}}},
                      {"outputs", Json::array({"value"})}}}})))
                .at("committed") == true);
        REQUIRE(definitionNode("scaled").at("op") == "value.multiply");
        // The root document did not grow a node of its own.
        REQUIRE(service->getGraphInstances(Json::object()).at("nodes").size() == 2);

        // set_input rewires one edge inside the body.
        REQUIRE(commit(edit(scoped({{"op", "set_input"}, {"node", "buffer"},
            {"port", "value"}, {"source", {{"node", "scaled"}, {"port", "value"}}}})))
                .at("committed") == true);
        REQUIRE(definitionNode("buffer").at("inputs").at("value").at("node") == "scaled");

        // set_node_parameters replaces that node's own settings -- it is not a
        // merge, so the whole object goes out and comes back.
        REQUIRE(definitionNode("knob").at("parameters").at("value") == 1);
        REQUIRE(commit(edit(scoped({{"op", "set_node_parameters"}, {"node", "knob"},
            {"parameters", {{"value", 3}}}}))).at("committed") == true);
        REQUIRE(definitionNode("knob").at("parameters").at("value") == 3);

        // set_node_label writes the authored name, and the empty string clears
        // it -- there is no second op for "unname this".
        REQUIRE(commit(edit(scoped({{"op", "set_node_label"}, {"node", "sum"},
            {"label", "gain + bias"}}))).at("committed") == true);
        REQUIRE(definitionNode("sum").at("label") == "gain + bias");
        REQUIRE(commit(edit(scoped({{"op", "set_node_label"}, {"node", "buffer"},
            {"label", ""}}))).at("committed") == true);
        REQUIRE_FALSE(definitionNode("buffer").contains("label"));
        // The same op reaches a root node with no scope at all.
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_node_label"},
            {"node", "clock"}, {"label", "scene clock"}}}))).at("committed") == true);
        const auto root_nodes = Json(service->getGraphInstances(Json::object()).at("nodes"));
        for (const auto &node : root_nodes)
            if (node.at("id") == "clock") REQUIRE(node.at("label") == "scene clock");

        // remove_node takes the node back out of the body it was added to.
        REQUIRE(commit(edit(scoped({{"op", "set_input"}, {"node", "buffer"},
            {"port", "value"}, {"source", {{"node", "sum"}, {"port", "value"}}}})))
                .at("committed") == true);
        REQUIRE(commit(edit(scoped({{"op", "remove_node"}, {"node", "scaled"}})))
                .at("committed") == true);
        REQUIRE(commit(edit(scoped({{"op", "remove_node"}, {"node", "knob"}})))
                .at("committed") == true);
        const auto remaining = definitionBody();
        for (const auto &node : remaining.at("nodes")) {
            CHECK(node.at("id") != "scaled");
            CHECK(node.at("id") != "knob");
        }

        // And what came back is still a document that loads and draws.
        draw();
        REQUIRE(portValue("gain").at("value") == 2.5);
    }

    SECTION("the scope boundary is refused by name in both directions") {
        const auto saved = readBytes(source.path);
        const auto reject = [&](Json ops, const char *code) {
            const auto result = edit(std::move(ops));
            REQUIRE(result.at("status") == "rejected");
            CHECK(result.at("error").at("code") == code);
            CHECK(readBytes(source.path) == saved);
        };
        // The definition's outputs map still names `sum`.
        reject(Json::array({Json{{"op", "remove_node"}, {"scope", "gain_curve"},
            {"node", "sum"}}}), "graph.output_referenced@1");
        // `left` stores a value for both boundary ports, so neither can go.
        reject(Json::array({Json{{"op", "remove_node"}, {"scope", "gain_curve"},
            {"node", "gain"}}}), "graph.input_referenced@1");
        reject(Json::array({Json{{"op", "remove_node"}, {"scope", "gain_curve"},
            {"node", "bias"}}}), "graph.input_referenced@1");
        // An instance belongs to the root document; a boundary port does not.
        reject(Json::array({Json{{"op", "add_node"}, {"scope", "gain_curve"},
            {"node", {{"id", "inner"}, {"op", "graph.instance"},
                      {"definition", "gain_curve"}}}}}), "graph.nested_instance@1");
        reject(Json::array({Json{{"op", "add_node"},
            {"node", {{"id", "knob"}, {"op", "graph.input"}, {"default", 1}}}}}),
            "graph.input_scope@1");
        // The control for the whole feature: without a scope, a definition node
        // is exactly as unreachable as it was before N-2.
        reject(Json::array({Json{{"op", "set_node_label"}, {"node", "sum"},
            {"label", "renamed"}}}), "graph.definition_immutable@1");
        reject(Json::array({Json{{"op", "set_input"}, {"node", "buffer"},
            {"port", "value"}, {"source", {{"node", "gain"}, {"port", "value"}}}}}),
            "graph.definition_immutable@1");
        // A scope nobody declared is not a silent no-op either.
        const auto unknown = edit(Json::array({Json{{"op", "set_node_label"},
            {"scope", "nowhere"}, {"node", "sum"}, {"label", "x"}}}));
        REQUIRE(unknown.at("status") == "rejected");
        CHECK(unknown.at("error").at("code") == "invalid_graph_document");
        REQUIRE(readBytes(source.path) == saved);
    }

    SECTION("the operation catalog reaches the editor over the same service") {
        const auto catalog = service->getGraphCatalog(Json::object());
        REQUIRE(catalog.at("schema") == "pelican.graph_catalog");
        std::vector<std::string> namespaces;
        for (const auto &module : catalog.at("modules"))
            namespaces.push_back(module.at("namespace").get<std::string>());
        CHECK(containsName(namespaces, "pelican.value"));
        CHECK(containsName(namespaces, "pelican.render"));
        // pelican.ui is always registered: the UI has no build without it (GP-3).
        CHECK(containsName(namespaces, "pelican.ui"));
        std::size_t expected_modules = 3;
        // pelican.audio and pelican.pose are registered only by the builds
        // that carry their module.
        CHECK(containsName(namespaces, "pelican.audio") == static_cast<bool>(PELICAN_WITH_AUDIO));
        expected_modules += PELICAN_WITH_AUDIO ? 1 : 0;
        CHECK(containsName(namespaces, "pelican.pose") == static_cast<bool>(PELICAN_WITH_ANIMATION));
        expected_modules += PELICAN_WITH_ANIMATION ? 1 : 0;
        CHECK(namespaces.size() == expected_modules);
    }
}

// N-6 acceptance 1. The canvas's own placement -- where the nodes were put, the
// control points its edges are drawn through and the bundles that carry several
// side by side -- is authored state the engine carries and reads nothing of. It
// goes in through one additive op, comes back on the read that was already
// there, and costs no recompose: moving a node must not rebuild the frame.
TEST_CASE("the graph document carries the editor's placement without recomposing",
          "[render-config-editor][graph-layout]") {
    const auto fixture_path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
        "fixtures" / "graph_instance_inputs" / "passes" / "main.json";
    TemporaryConfig source{readBytes(fixture_path)};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);
    const auto edit = [&](Json ops) {
        return service->editGraphDocument(
            {{"base_source_digest", service->getGraphInstances(Json::object())
                                        .at("source_digest").at("hex")},
             {"ops", std::move(ops)}});
    };
    const auto commit = [&](const nlohmann::ordered_json &submission) {
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        return service->getResult({{"ticket", submission.at("ticket")}});
    };
    const auto reportedLayout = [&] {
        return Json(service->getGraphInstances(Json::object()).at("layout"));
    };
    const auto definitionLayout = [&] {
        return Json(service->getGraphInstances(Json::object())
                        .at("definitions").at("gain_curve")
                        .value("layout", Json::object()));
    };
    // Two edges into the one instance, so there is something for a bundle to
    // carry. An edge is named by the input end it lands on.
    REQUIRE(commit(edit(Json::array({
                Json{{"op", "set_input"}, {"node", "left"}, {"port", "gain"},
                     {"source", {{"node", "clock"}, {"port", "value"}}}},
                Json{{"op", "set_input"}, {"node", "left"}, {"port", "bias"},
                     {"source", {{"node", "clock"}, {"port", "value"}}}}})))
                .at("committed") == true);

    const Json placement{
        {"nodes", {{"clock", {{"x", 10.0}, {"y", 20.0}}},
                   {"left", {{"x", 340.5}, {"y", -12.25}}}}},
        {"reroutes", Json::array({Json{
             {"edge", {{"node", "left"}, {"port", "gain"}}},
             {"points", Json::array({Json::array({180.0, 44.0}),
                                     Json::array({220.0, 44.0})})}}})},
        {"ties", Json::array({Json{
             {"x", 260.0}, {"y", 70.0},
             {"edges", Json::array({Json{{"node", "left"}, {"port", "gain"}},
                                    Json{{"node", "left"}, {"port", "bias"}}})}}})}};

    SECTION("a placement lands, comes back, and no candidate is compiled for it") {
        REQUIRE(reportedLayout() == Json::object());
        const auto applies = runtime.apply_calls;
        const auto generation = runtime.snapshot.published_generation;
        const auto plan = runtime.frame_plan;
        const auto catalog = Json(service->getGraphCatalog(Json::object()));

        const auto result = commit(edit(Json::array({Json{
            {"op", "set_layout"}, {"scope", "root"}, {"layout", placement}}})));
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        // The claim, said by the reply as well as by the numbers.
        CHECK(result.at("recomposed") == false);
        CHECK(runtime.apply_calls == applies);
        CHECK(runtime.snapshot.published_generation == generation);
        CHECK(result.at("published_generation") == generation);
        CHECK(runtime.frame_plan == plan);
        CHECK(Json(service->getGraphCatalog(Json::object())) == catalog);

        // And it is really in the document, in the shape it was written.
        CHECK(reportedLayout() == placement);
        CHECK(Json::parse(readBytes(source.path)).at("layout") == placement);

        // The document that came back still expands and still draws.
        const auto saved = Json::parse(readBytes(source.path));
        REQUIRE_NOTHROW(expandGraphDocument(saved));
        const ValueGraph::Graph graph{readBytes(source.path)};
        auto state = graph.createState();
        REQUIRE_NOTHROW(graph.publish(GameContext{},
            {{"time", 0.5}, {"delta_time", 0.5}, {"scene_time", 0.5}}, {},
            [](std::string_view, std::span<const float>) { return true; },
            {}, {}, &state));

        // An edit that is about the graph does recompose -- which is the
        // control for the three numbers above -- and carries the placement
        // through untouched.
        const auto layout_bytes = readBytes(source.path);
        const auto member = layout_bytes.substr(layout_bytes.find("\"layout\""));
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_node_label"},
            {"node", "clock"}, {"label", "scene clock"}}})))
                .at("committed") == true);
        CHECK(runtime.apply_calls == applies + 1);
        CHECK(runtime.snapshot.published_generation == generation + 1);
        CHECK(reportedLayout() == placement);
        CHECK(readBytes(source.path).find(member) != std::string::npos);

        // Clearing it is spelled with null. The member is left standing as an
        // empty object rather than deleted: the root write-back replaces one
        // member's bytes and has no way to take a member out, and "no
        // placement" and "a placement that places nothing" are the same claim.
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_layout"},
            {"layout", nullptr}}}))).at("committed") == true);
        CHECK(Json::parse(readBytes(source.path)).at("layout") == Json::object());
        CHECK(reportedLayout() == Json::object());
    }

    // N-8 acceptance 4. The Frame Plan canvas draws passes and resources, and
    // neither is a node of this document: the member is checked for shape and
    // its names are deliberately not looked up.
    SECTION("the frame plan's placement rides in the same layout member") {
        const Json frame_plan{
            {"main_render",
             {{"nodes", {{"ssao_pass", {{"x", 40.0}, {"y", -8.5}}},
                         {"deferred_lighting", {{"x", 300.0}, {"y", 12.0}}}}},
              {"reroutes", Json::array({Json{
                   {"edge", {{"node", "deferred_lighting"},
                             {"resource", "ssao_blur"}}},
                   {"points", Json::array({Json::array({180.0, 4.0})})}}})},
              {"ties", Json::array({Json{
                   {"x", 220.0}, {"y", 30.0},
                   {"edges", Json::array({
                        Json{{"node", "deferred_lighting"},
                             {"resource", "ssao_blur"}},
                        Json{{"node", "deferred_lighting"},
                             {"resource", "gbuffer_albedo"}}})}}})},
              {"groups", Json::array({Json{{"id", "session-1"},
                                           {"members", Json::array({"ssao_pass"})}}})}}}};
        Json both = placement;
        both["frame_plan"] = frame_plan;
        const auto applies = runtime.apply_calls;
        const auto generation = runtime.snapshot.published_generation;
        const auto plan = runtime.frame_plan;
        const auto catalog = Json(service->getGraphCatalog(Json::object()));

        const auto result = commit(edit(Json::array({Json{
            {"op", "set_layout"}, {"scope", "root"}, {"layout", both}}})));
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        // The same claim `nodes`/`reroutes`/`ties` make: nothing is recomposed.
        CHECK(result.at("recomposed") == false);
        CHECK(runtime.apply_calls == applies);
        CHECK(runtime.snapshot.published_generation == generation);
        CHECK(runtime.frame_plan == plan);
        CHECK(Json(service->getGraphCatalog(Json::object())) == catalog);
        CHECK(reportedLayout() == both);
        CHECK(Json::parse(readBytes(source.path)).at("layout") == both);
        REQUIRE_NOTHROW(expandGraphDocument(Json::parse(readBytes(source.path))));

        const auto saved = readBytes(source.path);
        const auto reject = [&](Json member, const char *code) {
            Json layout = placement;
            layout["frame_plan"] = std::move(member);
            const auto rejected = edit(Json::array({Json{
                {"op", "set_layout"}, {"layout", std::move(layout)}}}));
            INFO(rejected.dump());
            REQUIRE(rejected.at("status") == "rejected");
            CHECK(rejected.at("error").at("code") == code);
            CHECK(readBytes(source.path) == saved);
        };
        // Shape, and only shape.
        reject(Json::array(), "invalid_graph_document");
        reject(Json{{"main_render", {{"columns", Json::array()}}}},
               "graph.layout_invalid@1");
        reject(Json{{"main_render",
                     {{"nodes", {{"ssao_pass", {{"x", "left"}, {"y", 0}}}}}}}},
               "graph.layout_invalid@1");
        reject(Json{{"main_render",
                     {{"reroutes", Json::array({Json{
                          {"edge", {{"node", "ssao_pass"}, {"port", "input"}}},
                          {"points", Json::array()}}})}}}},
               "graph.layout_invalid@1");
        reject(Json{{"main_render",
                     {{"ties", Json::array({Json{{"x", 0}, {"y", 0}}})}}}},
               "graph.layout_invalid@1");
        reject(Json{{"main_render",
                     {{"groups", Json::array({Json{{"id", "g"}}})}}}},
               "graph.layout_invalid@1");

        // And the names really are not looked up: a pass this document has
        // never heard of is a perfectly good placement, because pass names are
        // decided in the preset and the features, not here. `nodes` above it
        // still refuses the same name, which is the control for that claim.
        Json unmatched = placement;
        unmatched["frame_plan"] = Json{
            {"a_graph_nobody_declared",
             {{"nodes", {{"a_pass_nobody_declared", {{"x", 1.0}, {"y", 2.0}}}}}}}};
        REQUIRE(commit(edit(Json::array({Json{
                    {"op", "set_layout"}, {"layout", unmatched}}})))
                    .at("committed") == true);
        CHECK(reportedLayout() == unmatched);
        const auto refused = edit(Json::array({Json{
            {"op", "set_layout"},
            {"layout", Json{{"nodes", {{"a_pass_nobody_declared",
                                        {{"x", 1.0}, {"y", 2.0}}}}}}}}}));
        REQUIRE(refused.at("status") == "rejected");
        CHECK(refused.at("error").at("code") == "graph.layout_invalid@1");
    }

    SECTION("a definition's placement travels inside its body") {
        const Json inner{
            {"nodes", {{"sum", {{"x", 4.0}, {"y", 8.0}}}}},
            {"reroutes", Json::array({Json{
                 {"edge", {{"node", "sum"}, {"port", "a"}}},
                 {"points", Json::array({Json::array({1.0, 2.0})})}}})},
            {"ties", Json::array()}};
        const auto applies = runtime.apply_calls;
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_layout"},
            {"scope", "gain_curve"}, {"layout", inner}}})))
                .at("committed") == true);
        CHECK(runtime.apply_calls == applies);
        CHECK(definitionLayout() == inner);
        // The root's own is untouched: the two are separate members.
        CHECK(reportedLayout() == Json::object());
        CHECK(Json::parse(readBytes(source.path))
                  .at("definitions").at("gain_curve").at("layout") == inner);
        REQUIRE_NOTHROW(expandGraphDocument(Json::parse(readBytes(source.path))));
    }

    SECTION("a placement that names nothing is refused by name") {
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_layout"},
            {"layout", placement}}}))).at("committed") == true);
        const auto saved = readBytes(source.path);
        const auto reject = [&](Json layout, const char *code,
                                const char *scope = nullptr) {
            Json operation{{"op", "set_layout"}, {"layout", std::move(layout)}};
            if (scope != nullptr) operation["scope"] = scope;
            const auto result = edit(Json::array({std::move(operation)}));
            INFO(result.dump());
            REQUIRE(result.at("status") == "rejected");
            CHECK(result.at("error").at("code") == code);
            CHECK(readBytes(source.path) == saved);
        };
        // A node this scope does not hold.
        reject(Json{{"nodes", {{"absent", {{"x", 0}, {"y", 0}}}}}},
               "graph.layout_invalid@1");
        // A port with no edge into it is not an edge.
        reject(Json{{"reroutes", Json::array({Json{
                   {"edge", {{"node", "clock"}, {"port", "value"}}},
                   {"points", Json::array({Json::array({0.0, 0.0})})}}})}},
               "graph.layout_invalid@1");
        reject(Json{{"ties", Json::array({Json{
                   {"x", 0}, {"y", 0},
                   {"edges", Json::array({
                        Json{{"node", "left"}, {"port", "gain"}},
                        Json{{"node", "left"}, {"port", "missing"}}})}}})}},
               "graph.layout_invalid@1");
        // A coordinate that is not a finite number.
        reject(Json{{"nodes", {{"clock", {{"x", "left"}, {"y", 0}}}}}},
               "graph.layout_invalid@1");
        // A member nobody declared: a placement the editor cannot read back is
        // not a placement it should have been allowed to write.
        reject(Json{{"columns", Json::array()}}, "graph.layout_invalid@1");
        // The scope rules are the ones every other op is bound by. An instance
        // path belongs to no single body...
        reject(Json{{"nodes", {{"left/sum", {{"x", 0}, {"y", 0}}}}}},
               "graph.definition_immutable@1");
        // ...and a node that lives in a definition is not reachable from the
        // root scope, which is a different fix from "there is no such node".
        reject(Json{{"nodes", {{"sum", {{"x", 0}, {"y", 0}}}}}},
               "graph.definition_immutable@1");
        // A scope nobody declared is not a silent no-op either.
        reject(Json{{"nodes", Json::object()}}, "invalid_graph_document",
               "nowhere");
        // The one that did land is still the one the document holds.
        CHECK(reportedLayout() == placement);
    }
}

// AG-3a acceptance 2 and 3. get_graph_values answers for one instance's input
// ports; the nodes inside the definition it instantiates have no answer there
// at all. This is that answer, taken from the same publish snapshot so reading
// it cannot advance a clock or consume an event.
TEST_CASE("node values report the definition scope the drawn frame evaluated",
          "[render-config-editor][graph-node-values]") {
    const auto fixture_path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
        "fixtures" / "graph_instance_inputs" / "passes" / "main.json";
    TemporaryConfig source{readBytes(fixture_path)};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    ValueGraph::PublishedInstanceInputs snapshot;
    std::size_t evaluations = 0;
    double scene_time = 0.5;
    // The snapshot the draw path leaves behind, produced by really publishing
    // the document under test.
    const auto draw = [&] {
        auto graph = std::make_shared<const ValueGraph::Graph>(readBytes(source.path));
        auto state = graph->createState();
        auto published = graph->publish(GameContext{},
            {{"time", scene_time}, {"delta_time", 0.5}, {"scene_time", scene_time}}, {},
            [](std::string_view, std::span<const float>) { return true; }, {}, {}, &state);
        ++evaluations;
        snapshot = {.published = true, .generation = 7, .frame = 11,
                    .inputs = std::move(published.instance_inputs),
                    .graph = std::move(graph),
                    .node_values = std::move(published.node_values)};
    };
    auto service = makeService(source, runtime, {}, [&] { return snapshot; });
    const auto edit = [&](Json ops) {
        return service->editGraphDocument(
            {{"base_source_digest", service->getGraphInstances(Json::object()).at("source_digest").at("hex")},
             {"ops", std::move(ops)}});
    };
    const auto commit = [&](const nlohmann::ordered_json &submission) {
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        return service->getResult({{"ticket", submission.at("ticket")}});
    };
    const auto nodeOf = [&](const nlohmann::ordered_json &reply, const char *id) {
        for (const auto &node : reply.at("nodes"))
            if (node.at("node") == id) return node;
        FAIL("no reported node " + std::string{id});
        return nlohmann::ordered_json{};
    };

    SECTION("nothing drawn yet is reported as nothing, not as zeroes") {
        const auto reply = service->getGraphNodeValues({{"instance", "left"}});
        REQUIRE(reply.at("published") == false);
        REQUIRE(reply.at("nodes").empty());
        REQUIRE(reply.at("source") == "rendering");
    }

    SECTION("a definition's nodes carry what the drawn frame gave them") {
        draw();
        const auto reply = service->getGraphNodeValues({{"instance", "left"}});
        REQUIRE(reply.at("published") == true);
        REQUIRE(reply.at("instance") == "left");
        REQUIRE(reply.at("generation") == 7);
        REQUIRE(reply.at("frame") == 11);
        REQUIRE(reply.at("source") == "rendering");
        // `sum` is value.add over the two instance parameters, and `clock` is
        // connected to nothing in this document, so 2.5 + 5 is a constant. It
        // is the constant the acceptance names, not an accident of the frame.
        const auto sum = nodeOf(reply, "sum");
        REQUIRE(sum.at("expanded") == "left/sum");
        REQUIRE(sum.at("outputs").at("value") == 7.5);
        REQUIRE(nodeOf(reply, "gain").at("outputs").at("value") == 2.5);
        REQUIRE(nodeOf(reply, "bias").at("outputs").at("value") == 5);
        // The rendering-domain node of the same definition is listed with no
        // outputs at all: the values partition has no value for it, and a
        // canvas that draws four nodes must not be told there are three.
        const auto buffer = nodeOf(reply, "buffer");
        REQUIRE(buffer.at("expanded") == "left/buffer");
        REQUIRE(buffer.at("outputs").is_object());
        REQUIRE(buffer.at("outputs").empty());
        // Reading is a lookup in the snapshot. No number of reads evaluates
        // anything, so two reads of an unstepped frame are the same bytes.
        const auto drawn = evaluations;
        REQUIRE(service->getGraphNodeValues({{"instance", "left"}}) == reply);
        REQUIRE(evaluations == drawn);
    }

    SECTION("an omitted instance is the root scope, where the two ids agree") {
        draw();
        const auto omitted = service->getGraphNodeValues(Json::object());
        REQUIRE(omitted.at("instance") == "");
        REQUIRE(nodeOf(omitted, "clock").at("outputs").at("value") == 0.5);
        for (const auto &node : omitted.at("nodes")) REQUIRE(node.at("node") == node.at("expanded"));
        // An explicit empty string asks the same question.
        REQUIRE(service->getGraphNodeValues({{"instance", ""}}) == omitted);
    }

    SECTION("connecting the clock makes the definition's sum move with the frame") {
        // The control for the constant above: with an edge from the document's
        // clock, `left/sum` is the frame's own number and two frames disagree.
        REQUIRE(commit(edit(Json::array({Json{{"op", "set_input"}, {"node", "left"},
            {"port", "gain"}, {"source", {{"node", "clock"}, {"port", "value"}}}}})))
                .at("committed") == true);
        draw();
        const auto first = service->getGraphNodeValues({{"instance", "left"}});
        REQUIRE(nodeOf(first, "sum").at("outputs").at("value") == 5.5);
        scene_time = 2.25;
        draw();
        const auto second = service->getGraphNodeValues({{"instance", "left"}});
        REQUIRE(nodeOf(second, "sum").at("outputs").at("value") == 7.25);
        // A connected port leaves no node of its own behind in the expansion,
        // so the value.parameter that carried it is gone rather than stale.
        for (const auto &node : second.at("nodes")) REQUIRE(node.at("node") != "gain");
    }

    SECTION("an animation node reports the scalar the frame fed it") {
        // Acceptance 3. A pose operation produces no value of its own, but the
        // values partition publishes every scalar it consumes as
        // `$pose/<node>/<port>` -- which is the only place the number an
        // animation node ran on is observable at all.
        const auto add = edit(Json::array({Json{{"op", "add_node"}, {"node",
            {{"id", "pose"}, {"op", "pose.clip_sample"},
             {"parameters", {{"rig", "project://character.gltf"}, {"clip", "Turn"}}},
             {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}},
             {"outputs", Json::array({"pose"})}}}}}));
#if PELICAN_WITH_ANIMATION
        REQUIRE(commit(add).at("committed") == true);
        draw();
        const auto reply = service->getGraphNodeValues(Json::object());
        const auto pose = nodeOf(reply, "pose");
        REQUIRE(pose.at("outputs").at("time") == 0.5);
        scene_time = 1.5;
        draw();
        REQUIRE(nodeOf(service->getGraphNodeValues(Json::object()), "pose")
                    .at("outputs").at("time") == 1.5);
#else
        // The control: without the animation module the operation is not
        // registered, so the document carrying it never lands and there is no
        // node to report a value for.
        REQUIRE(add.at("status") == "rejected");
        CHECK(add.at("error").at("code") == "graph.operation_unregistered@1");
#endif
    }
}

TEST_CASE("scene AS edits use render publication and source CAS", "[render-config-editor][rt7][scene-as]") {
    TemporaryConfig source{emptyHybridConfig()};
    const auto original = readBytes(source.path);
    ActualCpuRenderRuntime runtime;
    runtime.initialize(original);
    bool gate_open = true;
    std::uint64_t transition = 0;
    auto service = makeService(source, runtime, [&] {
        return RenderConfigEditorGateObservation{
            .can_edit = gate_open, .transition_epoch = transition};
    });
    const auto before = service->getSceneAccelerationStructures(Json::object());
    const auto digest = before.at("source_digest").at("hex").get<std::string>();
    REQUIRE(before.at("definitions").empty());
    const Json definitions = Json::array({{{"name", "actors"}, {"instance_mask", 1}}});
    const auto submit = [&](const Json &value) {
        return service->editSceneAccelerationStructures({{"base_source_digest", digest}, {"definitions", value}});
    };
    SECTION("callable record editing rejects unknown tasks and persists override reset") {
        const auto callable = service->getRayCallableRecords(Json::object());
        CHECK(callable.at("tasks").empty());
        const auto rejected = service->editRayCallableRecords({
            {"base_source_digest", digest}, {"task_records", {{"missing", Json::array()}}}});
        CHECK(rejected.at("status") == "rejected");
        CHECK(readBytes(source.path) == original);
        const auto accepted = service->editRayCallableRecords({
            {"base_source_digest", digest}, {"task_records", Json::object()}});
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == true);
        CHECK(Json::parse(readBytes(source.path)).at("rendering").at("ray_task_callable_records") == Json::object());
    }
    SECTION("task connection mapping rejects invalid values and accepts clearing overrides") {
        CHECK(before.at("task_connections").empty());
        for (const auto &mapping : Json::array({Json::array(), Json{{"unknown", "scene"}}, Json{{"unknown", ""}}})) {
            const auto rejected = service->editSceneAccelerationStructures({
                {"base_source_digest", digest}, {"definitions", definitions}, {"task_connections", mapping}});
            CHECK(rejected.at("status") == "rejected");
            CHECK(readBytes(source.path) == original);
        }
        const auto accepted = service->editSceneAccelerationStructures({
            {"base_source_digest", digest}, {"definitions", definitions}, {"task_connections", Json::object()}});
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == true);
        CHECK(Json::parse(readBytes(source.path)).at("rendering").at("ray_task_acceleration_structures") == Json::object());
    }
    SECTION("add modify remove and reopen") {
        auto accepted = submit(definitions);
        REQUIRE(accepted.at("status") == "accepted");
        CHECK(readBytes(source.path) == original);
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == true);
        CHECK(service->getSceneAccelerationStructures(Json::object()).at("definitions") == definitions);
        CHECK(Json::parse(readBytes(source.path)).at("rendering").at("scene_acceleration_structures") == definitions);
        const auto generation = runtime.snapshot.published_generation;
        CHECK(submit(definitions).at("error").at("code") == "external_modification");
        auto updated = definitions;
        updated[0]["instance_mask"] = 2;
        for (const auto &value : {updated, Json::array()}) {
            const auto current = service->getSceneAccelerationStructures(Json::object());
            accepted = service->editSceneAccelerationStructures({
                {"base_source_digest", current.at("source_digest").at("hex")}, {"definitions", value}});
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == true);
            CHECK(Json::parse(readBytes(source.path)).at("rendering").at("scene_acceleration_structures") == value);
        }
        CHECK(runtime.snapshot.published_generation == generation + 2);
        auto reopened = makeService(source, runtime);
        CHECK(reopened->getSceneAccelerationStructures(Json::object()).at("definitions").empty());
    }
    SECTION("failed publication restores the exact source") {
        runtime.fail_after_source_commit = true;
        const auto generation = runtime.snapshot.published_generation;
        const auto accepted = submit(definitions);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(source.path) == original);
        CHECK(service->getSceneAccelerationStructures(Json::object()) == before);
        CHECK(runtime.snapshot.published_generation == generation);
    }
    SECTION("a transition or a closed-and-reopened gate between admission and commit rejects") {
        for (const bool reopen : {false, true}) {
            const auto accepted = submit(definitions);
            REQUIRE(accepted.at("status") == "accepted");
            if (reopen) {
                gate_open = false;
                CHECK(submit(definitions).at("error").at("code") == "gate_closed");
                gate_open = true;
            } else {
                ++transition;
            }
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            CHECK(result.at("committed") == false);
            CHECK(result.at("error").at("code") == "gate_closed");
            CHECK(readBytes(source.path) == original);
            CHECK(runtime.apply_calls == 0);
        }
    }
    SECTION("invalid definitions and closed gate do not write") {
        for (const auto &invalid : Json::array({
                Json::array({{{"name", "scene"}}}), Json::array({{{"name", "x"}, {"instance_mask", 256}}}),
                Json::array({{{"name", "x"}}, {{"name", "x"}}})}))
            CHECK(submit(invalid).at("error").at("code") == "invalid_scene_acceleration_structures");
        gate_open = false;
        CHECK(submit(definitions).at("error").at("code") == "gate_closed");
        CHECK(runtime.apply_calls == 0);
        CHECK(readBytes(source.path) == original);
    }
    SECTION("external file edit after admission is preserved") {
        const auto accepted = submit(definitions);
        REQUIRE(accepted.at("status") == "accepted");
        const auto external = original + "\n";
        writeBytes(source.path, external);
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(source.path) == external);
    }
}

TEST_CASE("scene AS triangle space edits publish the referenced layout atomically",
          "[render-config-editor][scene-as][triangle-space]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto layout_path = project.root / "ray_layout.json";
    Json layout{{"schema", "pelican.ray_group_layout"}, {"version", 3},
                {"ray_types", Json::array({"primary"})}, {"default_group", "surface"},
                {"groups", Json::array({{{"name", "surface"}, {"type", "triangles"}}})},
                {"misses", Json::array({{{"name", "sky"}}})},
                {"record", Json::array({{{"name", "tint"}, {"type", "vec4"}}})},
                {"acceleration_structure", {{"defaults", {{"mask", 7}, {"geometry_flags", Json::array({"opaque"})}}},
                                             {"procedural_defaults", {{"geometry_flags", Json::array({"opaque"})}}},
                                             {"rules", Json::array()}}}};
    writeBytes(layout_path, layout.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["ray_group_layout"] = "ray_layout.json";
    writeBytes(project.render_config, root.dump(2));
    const auto source_before = readBytes(project.render_config);
    const auto layout_before = readBytes(layout_path);
    const auto abi_before = parseRayGroupLayout(layout_before).digest_sha256;
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto before = service->getSceneAccelerationStructures(Json::object());
    REQUIRE(before.at("triangle_geometry").at("editable") == true);
    REQUIRE(before.at("triangle_geometry").at("space") == "geometry");
    REQUIRE(before.at("triangle_geometry").at("source_reference") == "ray_layout.json");
    const auto plan_before = runtime.frame_plan;
    const Json definitions = Json::array({{{"name", "actors"}, {"instance_mask", 1}}});
    Json request{{"base_source_digest", before.at("source_digest").at("hex")},
                 {"definitions", definitions}, {"task_connections", Json::object()},
                 {"triangle_space", "world"},
                 {"base_layout_digest", before.at("triangle_geometry").at("source_digest").at("hex")}};
    SECTION("world and geometry edits retain layout ABI and reopen") {
        for (const auto *space : {"world", "geometry"}) {
            const auto current = service->getSceneAccelerationStructures(Json::object());
            request["base_source_digest"] = current.at("source_digest").at("hex");
            request["base_layout_digest"] = current.at("triangle_geometry").at("source_digest").at("hex");
            request["triangle_space"] = space;
            const auto accepted = service->editSceneAccelerationStructures(request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            CHECK(Json::parse(readBytes(layout_path)) == layout);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            layout["acceleration_structure"]["triangle_space"] = space;
            renderingSectionOf(root)["scene_acceleration_structures"] = definitions;
            renderingSectionOf(root)["ray_task_acceleration_structures"] = Json::object();
            CHECK(Json::parse(readBytes(layout_path)) == layout);
            CHECK(Json::parse(readBytes(project.render_config)) == root);
            CHECK(parseRayGroupLayout(readBytes(layout_path)).digest_sha256 == abi_before);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto reopened = service->getSceneAccelerationStructures(Json::object());
            CHECK(reopened.at("definitions") == definitions);
            CHECK(reopened.at("triangle_geometry").at("space") == space);
            CHECK(service->editSceneAccelerationStructures(request).at("status") == "rejected");
        }
    }
    SECTION("procedural and triangle spaces share one layout edit without changing the shader ABI") {
        layout["engine_record"] = Json::array({"procedural_transform"});
        writeBytes(layout_path, layout.dump(2));
        const auto procedural_abi = parseRayGroupLayout(readBytes(layout_path)).digest_sha256;
        for (const auto *space : {"world", "geometry"}) {
            const auto current = service->getSceneAccelerationStructures(Json::object());
            REQUIRE(current.at("procedural_geometry").at("editable") == true);
            CHECK(current.at("procedural_geometry").at("source_reference") == "ray_layout.json");
            CHECK(current.at("procedural_geometry").at("source_digest") == current.at("triangle_geometry").at("source_digest"));
            request["base_source_digest"] = current.at("source_digest").at("hex");
            request["base_layout_digest"] = current.at("procedural_geometry").at("source_digest").at("hex");
            request["procedural_space"] = space;
            // The first edit changes both spaces in one candidate. The second
            // changes procedural space alone, retaining the triangle choice.
            if (std::string_view{space} == "geometry") request.erase("triangle_space");
            const auto accepted = service->editSceneAccelerationStructures(request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            CHECK(Json::parse(readBytes(layout_path)) == layout);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            layout["acceleration_structure"]["triangle_space"] = "world";
            layout["acceleration_structure"]["procedural_space"] = space;
            renderingSectionOf(root)["scene_acceleration_structures"] = definitions;
            renderingSectionOf(root)["ray_task_acceleration_structures"] = Json::object();
            CHECK(Json::parse(readBytes(layout_path)) == layout);
            CHECK(Json::parse(readBytes(project.render_config)) == root);
            CHECK(parseRayGroupLayout(readBytes(layout_path)).digest_sha256 == procedural_abi);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto reopened = service->getSceneAccelerationStructures(Json::object());
            CHECK(reopened.at("procedural_geometry").at("space") == space);
            CHECK(reopened.at("triangle_geometry").at("space") == "world");
        }
    }
    SECTION("procedural space editing refuses an absent transform contract") {
        CHECK(before.at("procedural_geometry").at("editable") == false);
        CHECK_FALSE(before.at("procedural_geometry").at("reason").get<std::string>().empty());
        request.erase("triangle_space");
        request["procedural_space"] = "world";
        const auto rejected = service->editSceneAccelerationStructures(request);
        CHECK(rejected.at("error").at("code") == "procedural_space_not_editable");
        CHECK(readBytes(layout_path) == layout_before);
        CHECK(readBytes(project.render_config) == source_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("MG-2a an older layout version is reported by name and is not editable") {
        for (const int version : {1, 2}) {
            INFO("layout version " << version);
            auto old = layout;
            old["version"] = version;
            old["groups"] = Json::array({{{"name", "surface"}}});
            writeBytes(layout_path, old.dump(2));
            const auto current = service->getSceneAccelerationStructures(Json::object());
            for (const auto *space : {"triangle_geometry", "procedural_geometry"}) {
                INFO(space);
                CHECK(current.at(space).at("editable") == false);
                CHECK_THAT(current.at(space).at("reason").get<std::string>(),
                           Catch::Matchers::ContainsSubstring(
                               "pelican.ray_group_layout.version_unsupported@1"));
            }
        }
    }
    SECTION("MG-2a a transaction directory the previous format wrote stops the commit by name") {
        const auto accepted = service->editSceneAccelerationStructures(request);
        REQUIRE(accepted.at("status") == "accepted");
        const auto directory = renderConfigTransactionDirectory(project.root);
        std::filesystem::create_directories(directory);
        const auto manifest =
            Json{{"schema", "pelican.render_authoring_transaction"},
                 {"version", 1},
                 {"status", "committed"},
                 {"documents", Json::array()}}
                .dump(2);
        writeBytes(directory / "manifest.json", manifest);
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        CHECK(result.at("committed") == false);
        CHECK(result.at("error").at("code") == "render_config.legacy_transaction_dir@1");
        REQUIRE(result.at("error").contains("path"));
        CHECK(std::filesystem::equivalent(
            std::filesystem::path{result.at("error").at("path").get<std::string>()},
            directory));
        CHECK_THAT(result.at("error").at("message").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("delete this directory"));
        // Nothing was published, and the directory is left for the developer.
        CHECK(readBytes(directory / "manifest.json") == manifest);
        CHECK(readBytes(project.render_config) == source_before);
        CHECK(readBytes(layout_path) == layout_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("stale layout digest and invalid requests cannot publish") {
        auto stale = request;
        stale["base_layout_digest"] = std::string(64, '0');
        CHECK(service->editSceneAccelerationStructures(stale).at("error").at("code") == "external_modification");
        auto incomplete = request;
        incomplete.erase("base_layout_digest");
        CHECK(service->editSceneAccelerationStructures(incomplete).at("status") == "rejected");
        request["triangle_space"] = "object";
        CHECK(service->editSceneAccelerationStructures(request).at("status") == "rejected");
        CHECK(readBytes(project.render_config) == source_before);
        CHECK(readBytes(layout_path) == layout_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("external layout changes after admission are preserved") {
        const auto accepted = service->editSceneAccelerationStructures(request);
        REQUIRE(accepted.at("status") == "accepted");
        writeBytes(layout_path, layout_before + "\n");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(project.render_config) == source_before);
        CHECK(readBytes(layout_path) == layout_before + "\n");
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("publication failure restores root and layout") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->editSceneAccelerationStructures(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        CHECK(result.at("error").at("message") == "injected failure after source replacement");
        CHECK(result.at("committed") == false);
        CHECK(readBytes(project.render_config) == source_before);
        CHECK(readBytes(layout_path) == layout_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("version one and unreadable layouts remain read only") {
        layout["version"] = 1;
        layout.erase("acceleration_structure");
        layout["groups"][0].erase("type");
        for (const auto &bytes : {layout.dump(2), std::string{"invalid JSON"}}) {
            writeBytes(layout_path, bytes);
            const auto current = service->getSceneAccelerationStructures(Json::object());
            CHECK(current.at("triangle_geometry").at("editable") == false);
            CHECK_FALSE(current.at("triangle_geometry").at("reason").get<std::string>().empty());
            CHECK(service->editSceneAccelerationStructures(request).at("error").at("code") == "triangle_space_not_editable");
            CHECK(readBytes(layout_path) == bytes);
            CHECK(readBytes(project.render_config) == source_before);
        }
    }
    SECTION("a layout without AS settings gains only default AS settings and the chosen space") {
        layout.erase("acceleration_structure");
        writeBytes(layout_path, layout.dump(2));
        const auto current = service->getSceneAccelerationStructures(Json::object());
        REQUIRE(current.at("triangle_geometry").at("editable") == true);
        CHECK(current.at("procedural_geometry").at("editable") == false);
        request["base_layout_digest"] = current.at("triangle_geometry").at("source_digest").at("hex");
        auto procedural_request = request;
        procedural_request.erase("triangle_space");
        procedural_request["procedural_space"] = "world";
        CHECK(service->editSceneAccelerationStructures(procedural_request).at("error").at("code") == "procedural_space_not_editable");
        const auto accepted = service->editSceneAccelerationStructures(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        layout["acceleration_structure"] = {{"defaults", Json::object()}, {"triangle_space", "world"}};
        CHECK(Json::parse(readBytes(layout_path)) == layout);
    }
}

TEST_CASE("authored render root value edits preserve unrelated lexical bytes",
          "[renderconfig][document][root-value]") {
    // MG-2b: the document is a graph root (the only root read); its features
    // live in the section, and the root members below stay root members.
    const std::string original =
        "{\r\n  \"schema\" : \"pelican.graph\",\r\n"
        "  \"rendering\" : { \"features\" : [ \"keep\" ] },\r\n"
        "  \"scene_acceleration_structures\" : [ {\"name\":\"old\"} ],\r\n"
        "  \"nested\": {\"scene_acceleration_structures\": [1,2], \"text\": \"} ]\"}\r\n}\r\n";
    const auto document = AuthoredRenderConfigDocument::parse(original);
    SECTION("replace only the existing value including nested JSON") {
        const std::string value = R"json([{"name":"actors","instance_mask":1,"nested":{"list":[null,true,"}"]}}])json";
        const auto edited = document.withRootValue("scene_acceleration_structures", value);
        auto expected = original;
        const std::string old_value = "[ {\"name\":\"old\"} ]";
        expected.replace(expected.find(old_value), old_value.size(), value);
        CHECK(edited.bytes() == expected);
        CHECK(edited.featureReferences() == document.featureReferences());
        CHECK(edited.sourceDigest() != document.sourceDigest());
        CHECK(document.bytes() == original);
    }
    SECTION("insert absent member without adding features or changing existing whitespace") {
        const auto source = AuthoredRenderConfigDocument::inspect(
            "{\r\n\t\"schema\" : \"pelican.graph\",\r\n\t\"keep\" : 1\r\n}\r\n");
        const auto edited = source.withRootValue("scene_acceleration_structures", " [] ");
        CHECK(edited.bytes() ==
              "{\r\n\t\"schema\" : \"pelican.graph\",\r\n\t\"keep\" : 1,\r\n"
              "\t\"scene_acceleration_structures\": []\r\n}\r\n");
        CHECK_FALSE(edited.hasFeaturesArray());
    }
    SECTION("empty object and escaped key") {
        const std::string key = "quoted\"\\\nkey";
        const auto edited =
            AuthoredRenderConfigDocument::inspect("{\"schema\":\"pelican.graph\" }")
                .withRootValue(key, "42");
        CHECK(Json::parse(edited.bytes()).at(key) == 42);
        CHECK(edited.bytes() ==
              "{\"schema\":\"pelican.graph\", " + Json(key).dump() + ": 42 }");
    }
    SECTION("duplicate non-feature keys keep existing last-wins contract") {
        const auto source = AuthoredRenderConfigDocument::inspect(
            "{\"schema\":\"pelican.graph\",\"x\":1, \"x\":2}");
        CHECK(source.withRootValue("x", "3").bytes() ==
              "{\"schema\":\"pelican.graph\",\"x\":1, \"x\":3}");
        CHECK_THROWS_AS(
            AuthoredRenderConfigDocument::inspect(
                "{\"schema\":\"pelican.graph\",\"rendering\":{\"features\":[],\"features\":[]}}"),
            std::invalid_argument);
    }
    SECTION("invalid or multiple JSON values reject without changing source") {
        for (const auto value : {"", "[", "false true", "{\"x\":}", "NaN"}) {
            CAPTURE(value);
            CHECK_THROWS_AS(document.withRootValue("scene_acceleration_structures", value), std::invalid_argument);
        }
        CHECK_THROWS_AS(document.withSectionValue("features", "null"), std::invalid_argument);
        CHECK(document.bytes() == original);
    }
}

TEST_CASE("P-2a-4a section value edits address the graph root's rendering member",
          "[renderconfig][document][graph-root][p2a4a]") {
    const std::string original =
        "{\r\n  \"schema\": \"pelican.graph\",\r\n  \"version\": 1,\r\n"
        "  \"nodes\": [],\r\n"
        "  \"rendering\" : {\r\n    \"features\" : [ \"engine://features/ui.json\" ],\r\n"
        "    \"frame_node_order\" : { \"main_render\": [\"a\"] }\r\n  }\r\n}\r\n";
    const auto document = AuthoredRenderConfigDocument::parse(original);
    REQUIRE(document.hasRenderingSection());
    REQUIRE(document.hasFeaturesArray());
    REQUIRE(document.featureReferences() ==
            std::vector<std::string>{"engine://features/ui.json"});

    SECTION("an existing section member is replaced in place") {
        const auto edited =
            document.withSectionValue("frame_node_order", "{\"main_render\":[\"b\"]}");
        auto expected = original;
        const std::string old_value = "{ \"main_render\": [\"a\"] }";
        expected.replace(expected.find(old_value), old_value.size(),
                         "{\"main_render\":[\"b\"]}");
        CHECK(edited.bytes() == expected);
        CHECK(Json::parse(edited.bytes()).contains("rendering"));
        CHECK_FALSE(Json::parse(edited.bytes()).contains("frame_node_order"));
    }
    SECTION("an absent section member is inserted with the section's indent") {
        const auto edited = document.withSectionValue("pipeline", "{}");
        CHECK(edited.bytes() ==
              "{\r\n  \"schema\": \"pelican.graph\",\r\n  \"version\": 1,\r\n"
              "  \"nodes\": [],\r\n"
              "  \"rendering\" : {\r\n    \"features\" : [ \"engine://features/ui.json\" ],\r\n"
              "    \"frame_node_order\" : { \"main_render\": [\"a\"] },\r\n"
              "    \"pipeline\": {}\r\n  }\r\n}\r\n");
    }
    SECTION("the graph's own keys stay at the root") {
        const auto edited = document.withSectionValue("nodes", "[1]");
        CHECK(Json::parse(edited.bytes()).at("nodes") == Json::array({1}));
        CHECK(Json::parse(edited.bytes()).at("rendering").size() == 2);
        CHECK(isGraphRootKey("definitions"));
        CHECK(isGraphRootKey("outputs"));
        // N-6: the editor's own placement is a root member too, so a caller
        // that reaches for withSectionValue cannot bury it in `rendering`.
        CHECK(isGraphRootKey("layout"));
        CHECK(Json::parse(document.withSectionValue("layout", "{\"nodes\":{}}")
                              .bytes())
                  .at("layout")
                  .at("nodes") == Json::object());
        CHECK_FALSE(isGraphRootKey("features"));
        CHECK_FALSE(isGraphRootKey("rendering_passes"));
    }
    SECTION("features are added and removed inside the section") {
        const auto added = document.withFeatureAdded("project://features/x.json");
        CHECK(added.bytes().find("\"features\" : [ \"engine://features/ui.json\", \"project://features/x.json\" ]") !=
              std::string::npos);
        CHECK_FALSE(Json::parse(added.bytes()).contains("features"));
        const auto removed = added.withFeatureRemoved("project://features/x.json");
        CHECK(removed.bytes() == original);
    }
    SECTION("a root that is not a graph root is refused by name (MG-2b)") {
        // The older root kept the config's own keys at the root. None of the
        // three entry points reads it; each stops with the same name.
        const std::string legacy =
            "{\"features\":[],\"rendering\":{\"features\":[]}}";
        for (const auto &entry :
             std::vector<std::function<void()>>{
                 [&] { (void)AuthoredRenderConfigDocument::parse(legacy); },
                 [&] { (void)AuthoredRenderConfigDocument::inspect(legacy); },
                 [&] { (void)AuthoredRenderConfigDocument::initialize(legacy); },
             }) {
            CHECK_THROWS_MATCHES(
                entry(), RenderConfigLegacyRoot,
                Catch::Matchers::MessageMatches(
                    Catch::Matchers::StartsWith("render_config.legacy_root@1")));
        }
    }
    SECTION("initialize creates the features array in the section") {
        const auto initialized = AuthoredRenderConfigDocument::initialize(
            "{\"schema\": \"pelican.graph\", \"rendering\": {\"shader_defines\": []}}");
        CHECK(initialized.bytes() ==
              "{\"schema\": \"pelican.graph\", \"rendering\": {\"shader_defines\": [], \"features\": []}}");
        CHECK(initialized.hasFeaturesArray());
        CHECK_FALSE(Json::parse(initialized.bytes()).contains("features"));
    }
    SECTION("a graph root without a section refuses every write") {
        const std::string sectionless =
            "{\"schema\": \"pelican.graph\", \"nodes\": []}";
        const auto inspected =
            AuthoredRenderConfigDocument::inspect(sectionless);
        CHECK_FALSE(inspected.hasRenderingSection());
        CHECK_FALSE(inspected.hasFeaturesArray());
        CHECK(inspected.featureReferences().empty());
        CHECK_THROWS_AS(inspected.withSectionValue("features", "[]"),
                        RenderConfigSectionUnlocated);
        CHECK_THROWS_AS(AuthoredRenderConfigDocument::initialize(sectionless),
                        RenderConfigSectionUnlocated);
        CHECK_THROWS_AS(AuthoredRenderConfigDocument::parse(sectionless),
                        RenderConfigSectionUnlocated);
        CHECK_THROWS_AS(
            AuthoredRenderConfigDocument::inspect(
                "{\"schema\": \"pelican.graph\", \"rendering\": 7}")
                .withSectionValue("features", "[]"),
            RenderConfigSectionUnlocated);
        // A graph root's own keys are still addressable without a section:
        // `edit_graph_document` keeps working on a document that has no
        // rendering config yet.
        CHECK(inspected.withRootValue("nodes", "[1]").bytes() ==
              "{\"schema\": \"pelican.graph\", \"nodes\": [1]}");
    }
}

TEST_CASE(
    "WP335 RPC startup is read-only and the first requested edit initializes features",
    "[render-config-editor][wp335][rpc][byte-lossless][initialization]") {
    const std::string original =
        "{\r\n"
        "  \"schema\": \"pelican.graph\",\r\n"
        "  \"version\": 1,\r\n"
        "  \"definitions\": {},\r\n"
        "  \"nodes\": [],\r\n"
        "  \"outputs\": {},\r\n"
        "  \"rendering\" : {\r\n"
        "    \"pipeline\" : { \"preset\" : \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "    \"shader_defines\" : [ \"WP334A_KEPT\" ]\r\n"
        "  }\r\n"
        "}\r\n";
    const std::string initialized =
        "{\r\n"
        "  \"schema\": \"pelican.graph\",\r\n"
        "  \"version\": 1,\r\n"
        "  \"definitions\": {},\r\n"
        "  \"nodes\": [],\r\n"
        "  \"outputs\": {},\r\n"
        "  \"rendering\" : {\r\n"
        "    \"pipeline\" : { \"preset\" : \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "    \"shader_defines\" : [ \"WP334A_KEPT\" ],\r\n"
        "    \"features\": []\r\n"
        "  }\r\n"
        "}\r\n";

    REQUIRE_FALSE(Json::parse(original).contains("features"));
    REQUIRE_THROWS_AS(
        AuthoredRenderConfigDocument::parse(original),
        std::invalid_argument);

    const auto first =
        AuthoredRenderConfigDocument::initialize(original);
    const auto deterministic =
        AuthoredRenderConfigDocument::initialize(original);
    REQUIRE(first.bytes() == initialized);
    REQUIRE(deterministic.bytes() == first.bytes());
    REQUIRE(AuthoredRenderConfigDocument::initialize(first.bytes()).bytes() ==
            first.bytes());
    const auto expected_edited =
        first.withFeatureAdded(std::string{buildAvailableReference}).bytes();

    TemporaryConfig source{original};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    const auto baseline_generation =
        runtime.snapshot.published_generation;
    REQUIRE_FALSE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));

    // This is the editor service and RPC adapter that the --rpc composition
    // constructs unconditionally. Merely constructing and querying them must
    // leave the authored source untouched.
    EngineLaunchConfig launch;
    launch.rpc = true;
    REQUIRE(launch.rpc);
    auto render_service =
        std::shared_ptr<RenderConfigEditorService>{
            makeService(source, runtime).release()};
    const auto scene_bytes = readBytes(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test" / "fixtures" / "authoring_scene" /
        "multi_scene_roundtrip.json");
    const auto scene_document =
        AuthoringSceneDocument::load(scene_bytes, SceneRevision{1});
    EditorCommandService commands{
        EditorCommandServiceDependencies{
            .document = [&scene_document]()
                -> const AuthoringSceneDocument & {
                return scene_document;
            },
            .current_scene_id = [] { return std::string{"main"}; },
            .render_config_editor = render_service,
        }};
    EditorCommandRpcAdapter adapter{commands};
    std::istringstream input;
    std::ostringstream output;
    RpcServer rpc{input, output};
    configureEditorRpcHandlers(
        rpc, adapter,
        EditorRpcHandlerHooks{
            .snapshot_imported = [] {},
            .save_busy = [] { return false; },
        });
    const auto call = [&](int id, std::string_view method,
                          Json params) {
        const auto response = Json::parse(rpc.processLine(
            Json{{"jsonrpc", "2.0"},
                 {"id", id},
                 {"method", method},
                 {"params", std::move(params)}}
                .dump()));
        REQUIRE(response.contains("result"));
        return response.at("result");
    };

    REQUIRE(readBytes(source.path) == original);
    REQUIRE(render_service->documentForTesting().bytes() == original);
    const auto before = call(3350, "get_render_features", Json::object());
    REQUIRE(before.at("features").empty());
    REQUIRE(before.at("has_uneditable_feature_entries") == false);
    REQUIRE(before.at("uneditable_feature_entry_count") == 0);
    REQUIRE(before.at("source_digest").at("hex") ==
            renderConfigSourceDigest(original));
    REQUIRE(readBytes(source.path) == original);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation);
    REQUIRE(runtime.apply_calls == 0);

    const auto accepted = call(
        3351, "edit_render_features",
        {{"base_source_digest", before.at("source_digest").at("hex")},
         {"operations", operation("add", buildAvailableReference)}});
    REQUIRE(accepted.at("status") == "accepted");
    REQUIRE(readBytes(source.path) == original);
    const auto ticket = accepted.at("ticket").get<std::string>();
    commands.commitPendingEdits();
    const auto result = call(
        3352, "get_edit_result", {{"ticket", ticket}});
    REQUIRE(result.at("committed") == true);
    REQUIRE(readBytes(source.path) == expected_edited);
    REQUIRE(readBytes(source.path) != original);
    REQUIRE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
    REQUIRE(containsName(runtime.snapshot.enabled_feature_names,
                         buildAvailableFeature));

    const auto edited_bytes = readBytes(source.path);
    const auto after = call(3353, "get_render_features", Json::object());
    const auto no_op = call(
        3354, "edit_render_features",
        {{"base_source_digest", after.at("source_digest").at("hex")},
         {"operations", operation("add", buildAvailableReference)}});
    const auto no_op_ticket = no_op.at("ticket").get<std::string>();
    commands.commitPendingEdits();
    const auto no_op_result = call(
        3355, "get_edit_result", {{"ticket", no_op_ticket}});
    REQUIRE(no_op_result.at("committed") == true);
    REQUIRE(no_op_result.at("no_change") == true);
    REQUIRE(readBytes(source.path) == edited_bytes);
    REQUIRE(runtime.apply_calls == 1);
}
#endif

TEST_CASE(
    "WP331 adding a build-available feature publishes its real frame-plan node without restarting",
    "[render-config-editor][wp331][central-control]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    runtime.post_commit_error =
        "injected post-commit cleanup diagnostic";
    auto service = makeService(source, runtime);

    const auto *same_runtime = &runtime;
    const auto before_generation =
        runtime.snapshot.published_generation;
    REQUIRE_FALSE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
    REQUIRE_FALSE(containsName(
        runtime.snapshot.enabled_feature_names,
        buildAvailableFeature));

    const auto before = service->getRenderFeatures(Json::object());
    const auto result = submitAndCommit(
        *service,
        before.at("source_digest").at("hex").get<std::string>(),
        operation("add", buildAvailableReference));

    REQUIRE(&runtime == same_runtime);
    REQUIRE(result.at("status") == "committed");
    REQUIRE(result.at("committed") == true);
    REQUIRE(result.at("published_generation") ==
            before_generation + 1);
    REQUIRE(result.at("source_digest").at("hex") ==
            renderConfigSourceDigest(readBytes(source.path)));
    REQUIRE(result.at("post_commit_error") ==
            runtime.post_commit_error);
    REQUIRE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
    REQUIRE(containsName(runtime.snapshot.enabled_feature_names,
                         buildAvailableFeature));
}

TEST_CASE(
    "WP335 parameterized feature entries stay byte-exact while string entries remain editable",
    "[render-config-editor][wp335][byte-lossless][parameterized-feature]") {
    constexpr std::string_view parameterized_reference =
        "project://features/parameterized.json";
    constexpr std::string_view editable_old =
        "project://features/editable-old.json";
    constexpr std::string_view editable_new =
        "project://features/editable-new.json";
    const std::string object_entry =
        "{  \"ref\" : \"" + std::string{parameterized_reference} +
        "\", \"parameters\" : { \"alpha\" : 0.125 } }";
    const std::string object_config =
        "{\r\n"
        "  \"schema\" : \"pelican.graph\",\r\n"
        "  \"rendering\" : {\r\n"
        "    \"features\" : [\r\n"
        "      " + object_entry + ",\r\n"
        "      \"" + std::string{editable_old} + "\"\r\n"
        "    ],\r\n"
        "    \"sentinel\" : \"WP335_KEEP\"\r\n"
        "  }\r\n"
        "}\r\n";

    struct PassthroughRuntime {
        std::uint64_t generation = 7;
        std::size_t apply_calls = 0;
    };
    const auto make_passthrough_service =
        [&](TemporaryConfig &source,
            PassthroughRuntime &runtime) {
            return std::make_unique<RenderConfigEditorService>(
                RenderConfigEditorDependencies{
                    .source_reference = "passes/main.json",
                    .source_path = source.path,
                    .source_bytes = readBytes(source.path),
                    .gate = [] {
                        return RenderConfigEditorGateObservation{};
                    },
                    .runtime_snapshot = [&runtime] {
                        return RenderConfigRuntimeSnapshot{
                            .published_generation = runtime.generation,
                        };
                    },
                    .apply_candidate =
                        [&runtime](
                            RenderConfigCandidateDocumentSet,
                            RenderConfigSourceCommit commit,
                            RenderConfigRuntimeApplyCompletion) {
                            ++runtime.apply_calls;
                            commit();
                            ++runtime.generation;
                            return RenderConfigRuntimeApplyResult{
                                .committed = true,
                                .published_generation = runtime.generation,
                            };
                        },
                    .feature_catalog = [=] {
                        return std::vector<RenderFeatureCatalogEntry>{
                            {.name = "editable_old",
                             .reference = std::string{editable_old},
                             .available = true},
                            {.name = "editable_new",
                             .reference = std::string{editable_new},
                             .available = true},
                        };
                    },
                });
        };

    TemporaryConfig object_source{object_config};
    PassthroughRuntime object_runtime;
    auto object_service =
        make_passthrough_service(object_source, object_runtime);
    REQUIRE(readBytes(object_source.path) == object_config);
    const auto object_view =
        object_service->getRenderFeatures(Json::object());
    REQUIRE(object_view.at("features") ==
            Json::array({editable_old}));
    REQUIRE(object_view.at("has_uneditable_feature_entries") == true);
    REQUIRE(object_view.at("uneditable_feature_entry_count") == 1);

    const auto object_no_op = submitAndCommit(
        *object_service,
        object_view.at("source_digest").at("hex").get<std::string>(),
        Json::array());
    REQUIRE(object_no_op.at("no_change") == true);
    REQUIRE(readBytes(object_source.path) == object_config);
    REQUIRE(object_runtime.apply_calls == 0);

    const auto object_add = submitAndCommit(
        *object_service,
        object_service->documentForTesting().sourceDigest(),
        operation("add", editable_new));
    REQUIRE(object_add.at("committed") == true);
    const auto bytes_after_add = readBytes(object_source.path);
    REQUIRE(bytes_after_add.find(object_entry) != std::string::npos);
    const auto semantic_after_add =
        Json::parse(bytes_after_add).at("rendering");
    REQUIRE(semantic_after_add.at("features").at(0) ==
            Json::parse(object_entry));
    REQUIRE(semantic_after_add.at("features").at(1) ==
            std::string{editable_old});
    REQUIRE(semantic_after_add.at("features").at(2) ==
            std::string{editable_new});

    const auto object_remove = submitAndCommit(
        *object_service,
        object_service->documentForTesting().sourceDigest(),
        operation("remove", editable_old));
    REQUIRE(object_remove.at("committed") == true);
    const auto bytes_after_remove = readBytes(object_source.path);
    REQUIRE(bytes_after_remove.find(object_entry) != std::string::npos);
    const auto semantic_after_remove =
        Json::parse(bytes_after_remove).at("rendering");
    REQUIRE(semantic_after_remove.at("features").size() == 2);
    REQUIRE(semantic_after_remove.at("features").at(0) ==
            Json::parse(object_entry));
    REQUIRE(semantic_after_remove.at("features").at(1) ==
            std::string{editable_new});
    const auto object_view_after =
        object_service->getRenderFeatures(Json::object());
    REQUIRE(object_view_after.at("features") ==
            Json::array({editable_new}));
    REQUIRE(object_view_after.at("uneditable_feature_entry_count") == 1);

    const std::string strings_only_config =
        "{\n  \"schema\" : \"pelican.graph\",\n  \"rendering\" : {\n"
        "    \"features\" : [ \"" + std::string{editable_old} +
        "\" ],\n    \"sentinel\" : \"WP335_STRINGS\"\n  }\n}\n";
    TemporaryConfig strings_source{strings_only_config};
    PassthroughRuntime strings_runtime;
    auto strings_service =
        make_passthrough_service(strings_source, strings_runtime);
    const auto strings_view =
        strings_service->getRenderFeatures(Json::object());
    REQUIRE(strings_view.at("features") ==
            Json::array({editable_old}));
    REQUIRE(strings_view.at("has_uneditable_feature_entries") == false);
    REQUIRE(strings_view.at("uneditable_feature_entry_count") == 0);
    REQUIRE(readBytes(strings_source.path) == strings_only_config);

    const auto strings_no_op = submitAndCommit(
        *strings_service,
        strings_view.at("source_digest").at("hex").get<std::string>(),
        operation("add", editable_old));
    REQUIRE(strings_no_op.at("no_change") == true);
    REQUIRE(readBytes(strings_source.path) == strings_only_config);

    REQUIRE(submitAndCommit(
                *strings_service,
                strings_service->documentForTesting().sourceDigest(),
                operation("add", editable_new))
                .at("committed") == true);
    REQUIRE(submitAndCommit(
                *strings_service,
                strings_service->documentForTesting().sourceDigest(),
                operation("remove", editable_old))
                .at("committed") == true);
    const auto strings_semantic =
        Json::parse(readBytes(strings_source.path)).at("rendering");
    REQUIRE(strings_semantic.at("features") ==
            Json::array({editable_new}));
    REQUIRE(strings_semantic.at("sentinel") == "WP335_STRINGS");
}

TEST_CASE(
    "WP331 deletion removes an existing build-available feature node",
    "[render-config-editor][wp331][delete-control]") {
    const auto shipped =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "projects" / "animgraph_demo" / "passes" /
        "main.json";
    std::string initial_bytes;
    std::string_view feature_reference;
    std::string_view feature_name;
#if PELICAN_RUNTIME_SHADER_COMPILER
    initial_bytes = readBytes(shipped);
    feature_reference = skyReference;
    feature_name = "sky_ambient";
#else
    initial_bytes = AuthoredRenderConfigDocument::parse(
                        emptyHybridConfig())
                        .withFeatureAdded(
                            std::string{rtShadowReference})
                        .bytes();
    feature_reference = rtShadowReference;
    feature_name = "rt_shadow_mask";
#endif
    TemporaryConfig source{initial_bytes};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);

    REQUIRE(hasProvider(runtime.frame_plan, feature_name));
    REQUIRE(containsName(runtime.snapshot.enabled_feature_names,
                         feature_name));
    const auto before = service->getRenderFeatures(Json::object());
    const auto result = submitAndCommit(
        *service,
        before.at("source_digest").at("hex").get<std::string>(),
        operation("remove", feature_reference));

    REQUIRE(result.at("committed") == true);
    REQUIRE_FALSE(hasProvider(runtime.frame_plan, feature_name));
    REQUIRE_FALSE(containsName(
        runtime.snapshot.enabled_feature_names, feature_name));
    REQUIRE(readBytes(shipped).find(std::string{skyReference}) !=
            std::string::npos);
}

TEST_CASE(
    "WP331 module features remain visible and are rejected by name",
    "[render-config-editor][wp331][restart-required]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);
    const auto baseline_bytes = readBytes(source.path);
    const auto baseline_runtime = runtime.snapshot;

    std::set<std::string> expected_module_features;
    std::size_t multi_module_features = 0;
    for (const auto &requirement :
         renderFeatureRuntimeModuleRequirements()) {
        REQUIRE_FALSE(requirement.feature.empty());
        REQUIRE_FALSE(requirement.modules.empty());
        REQUIRE(expected_module_features.emplace(
                    requirement.feature).second);
        if (requirement.modules.size() > 1) {
            ++multi_module_features;
        }
    }
    REQUIRE(multi_module_features == 2);
    const auto implementation_names =
        renderFeaturesRequiringRuntimeModules();
    REQUIRE(std::set<std::string>{implementation_names.begin(),
                                  implementation_names.end()} ==
            expected_module_features);

    const auto catalog =
        service->listRenderFeatures(Json::object());
    for (const auto &name : expected_module_features) {
        const auto found = std::ranges::find_if(
            catalog.at("features"), [&](const auto &entry) {
                return entry.at("name")
                           .template get<std::string>() ==
                       name;
            });
        REQUIRE(found != catalog.at("features").end());
        REQUIRE(found->at("requires_runtime_module") == true);
        REQUIRE(found->at("hot_add_supported") == false);
        REQUIRE(found->at("available") == false);
        REQUIRE_FALSE(found->at("unavailable_reason")
                          .get<std::string>().empty());

        const auto response = service->editRenderFeatures(
            {{"base_source_digest",
              service->documentForTesting().sourceDigest()},
             {"operations",
              operation("add",
                        found->at("reference")
                            .get<std::string>())}});
        REQUIRE(response.at("status") == "rejected");
        REQUIRE(response.at("error").at("code") ==
                "restart_required_feature");
        REQUIRE(response.at("error").at("message")
                    .get<std::string>()
                    .find(std::string{name}) != std::string::npos);
    }
    REQUIRE(readBytes(source.path) == baseline_bytes);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_runtime.published_generation);
    REQUIRE(runtime.apply_calls == 0);
}

TEST_CASE(
    "WP332 no-op preserves bytes generation and apply count while a real edit advances them",
    "[render-config-editor][wp331][wp332][byte-lossless][no-op]") {
    const std::string original =
        "{\r\n"
        "  \"schema\": \"pelican.graph\",\r\n"
        "  \"version\": 1,\r\n"
        "  \"definitions\": {},\r\n"
        "  \"nodes\": [],\r\n"
        "  \"outputs\": {},\r\n"
        "  \"rendering\": {\r\n"
        "    \"pipeline\": { \"preset\": \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "    \"shader_defines\": [ \"WP331_KEPT\" ],\r\n"
        "    \"features\": []\r\n"
        "  }\r\n"
        "}\r\n";
    TemporaryConfig source{original};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);

    const auto baseline_generation =
        runtime.snapshot.published_generation;
    const auto baseline_apply_calls = runtime.apply_calls;

    auto result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        Json::array());
    REQUIRE(result.at("committed") == true);
    REQUIRE(result.at("no_change") == true);
    REQUIRE(result.at("published_generation") ==
            baseline_generation);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation);
    REQUIRE(runtime.apply_calls == baseline_apply_calls);
    REQUIRE(readBytes(source.path) == original);

    result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(result.at("committed") == true);
    REQUIRE_FALSE(result.contains("no_change"));
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation + 1);
    REQUIRE(runtime.apply_calls == baseline_apply_calls + 1);
    const std::string expected =
        "{\r\n"
        "  \"schema\": \"pelican.graph\",\r\n"
        "  \"version\": 1,\r\n"
        "  \"definitions\": {},\r\n"
        "  \"nodes\": [],\r\n"
        "  \"outputs\": {},\r\n"
        "  \"rendering\": {\r\n"
        "    \"pipeline\": { \"preset\": \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "    \"shader_defines\": [ \"WP331_KEPT\" ],\r\n"
        "    \"features\": [\"" +
        std::string{buildAvailableReference} +
        "\"]\r\n"
        "  }\r\n"
        "}\r\n";
    REQUIRE(readBytes(source.path) == expected);

    const auto changed_generation =
        runtime.snapshot.published_generation;
    const auto changed_apply_calls = runtime.apply_calls;
    result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(result.at("committed") == true);
    REQUIRE(result.at("no_change") == true);
    REQUIRE(result.at("published_generation") ==
            changed_generation);
    REQUIRE(runtime.snapshot.published_generation ==
            changed_generation);
    REQUIRE(runtime.apply_calls == changed_apply_calls);
    REQUIRE(readBytes(source.path) == expected);
}

TEST_CASE(
    "WP332 catalog and apply share the engine availability decision",
    "[render-config-editor][wp332][catalog][availability]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);

    const auto catalog =
        service->listRenderFeatures(Json::object());
    const auto find_entry = [&](std::string_view reference)
        -> const nlohmann::ordered_json & {
        const auto found = std::ranges::find_if(
            catalog.at("features"), [&](const auto &entry) {
                return entry.at("reference")
                           .template get<std::string>() ==
                       reference;
            });
        REQUIRE(found != catalog.at("features").end());
        return *found;
    };

    const auto &unavailable = find_entry(rejectedReference);
    REQUIRE(unavailable.at("name").get<std::string>() ==
            std::string{rejectedFeature});
    REQUIRE(unavailable.at("available") == false);
    REQUIRE(unavailable.at("hot_add_supported") == false);
    const auto unavailable_reason =
        unavailable.at("unavailable_reason")
            .get<std::string>();
    REQUIRE_FALSE(unavailable_reason.empty());
#if !PELICAN_RUNTIME_SHADER_COMPILER
    REQUIRE(unavailable_reason ==
            std::string{renderFeatureRuntimeCompilerRequiredMessage});
#endif

    const auto baseline_generation =
        runtime.snapshot.published_generation;
    bool rejected_source_commit_called = false;
    const auto directly_rejected = runtime.apply(
        AuthoredRenderConfigDocument::parse(emptyHybridConfig())
            .withFeatureAdded(std::string{rejectedReference})
            .bytes(),
        [&] { rejected_source_commit_called = true; });
    REQUIRE_FALSE(directly_rejected.committed);
    REQUIRE(directly_rejected.error == unavailable_reason);
    REQUIRE_FALSE(rejected_source_commit_called);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation);
    const auto baseline_apply_calls = runtime.apply_calls;
    const auto rejected = service->editRenderFeatures(
        {{"base_source_digest",
          service->documentForTesting().sourceDigest()},
         {"operations",
          operation("add", rejectedReference)}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("message") ==
            unavailable_reason);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation);
    REQUIRE(runtime.apply_calls == baseline_apply_calls);

    const auto &available =
        find_entry(buildAvailableReference);
    REQUIRE(available.at("name").get<std::string>() ==
            std::string{buildAvailableFeature});
    REQUIRE(available.at("available") == true);
    REQUIRE(available.at("hot_add_supported") == true);
    REQUIRE_FALSE(available.contains("unavailable_reason"));

    const auto applied = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(applied.at("committed") == true);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_generation + 1);
    REQUIRE(runtime.apply_calls == baseline_apply_calls + 1);
}

TEST_CASE(
    "WP332 device capability changes both catalog availability and apply",
    "[render-config-editor][wp332][catalog][capability]") {
    TemporaryConfig unavailable_source{emptyHybridConfig()};
    ActualCpuRenderRuntime unavailable_runtime;
    unavailable_runtime.ray_query_available = false;
    unavailable_runtime.initialize(
        readBytes(unavailable_source.path));
    auto unavailable_service = makeService(
        unavailable_source, unavailable_runtime);
    const auto unavailable_catalog =
        unavailable_service->listRenderFeatures(Json::object());
    const auto unavailable = std::ranges::find_if(
        unavailable_catalog.at("features"),
        [](const auto &entry) {
            return entry.at("reference")
                       .template get<std::string>() ==
                   rtShadowReference;
        });
    REQUIRE(unavailable !=
            unavailable_catalog.at("features").end());
    REQUIRE(unavailable->at("available") == false);
    const auto reason = unavailable->at("unavailable_reason")
                            .get<std::string>();
    REQUIRE(reason.find(
                std::string{vulkanRayQueryCapability}) !=
            std::string::npos);
    bool rejected_source_commit_called = false;
    const auto directly_rejected = unavailable_runtime.apply(
        AuthoredRenderConfigDocument::parse(emptyHybridConfig())
            .withFeatureAdded(std::string{rtShadowReference})
            .bytes(),
        [&] { rejected_source_commit_called = true; });
    REQUIRE_FALSE(directly_rejected.committed);
    REQUIRE(directly_rejected.error == reason);
    REQUIRE_FALSE(rejected_source_commit_called);
    const auto rejected_apply_calls =
        unavailable_runtime.apply_calls;
    const auto rejected =
        unavailable_service->editRenderFeatures(
            {{"base_source_digest",
              unavailable_service->documentForTesting()
                  .sourceDigest()},
             {"operations",
              operation("add", rtShadowReference)}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("message") == reason);
    REQUIRE(unavailable_runtime.apply_calls ==
            rejected_apply_calls);

    TemporaryConfig available_source{emptyHybridConfig()};
    ActualCpuRenderRuntime available_runtime;
    available_runtime.ray_query_available = true;
    available_runtime.initialize(readBytes(available_source.path));
    auto available_service = makeService(
        available_source, available_runtime);
    const auto available_catalog =
        available_service->listRenderFeatures(Json::object());
    const auto available = std::ranges::find_if(
        available_catalog.at("features"),
        [](const auto &entry) {
            return entry.at("reference")
                       .template get<std::string>() ==
                   rtShadowReference;
        });
    REQUIRE(available !=
            available_catalog.at("features").end());
    REQUIRE(available->at("available") == true);
    const auto baseline_generation =
        available_runtime.snapshot.published_generation;
    const auto applied = submitAndCommit(
        *available_service,
        available_service->documentForTesting().sourceDigest(),
        operation("add", rtShadowReference));
    REQUIRE(applied.at("committed") == true);
    REQUIRE(available_runtime.snapshot.published_generation ==
            baseline_generation + 1);
    REQUIRE(available_runtime.apply_calls == 1);
}

TEST_CASE(
    "WP331 digest CAS accepts unchanged source then preserves an external edit",
    "[render-config-editor][wp331][external-modification]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime);

    auto result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(result.at("committed") == true);
    const auto generation_after_success =
        runtime.snapshot.published_generation;

    auto external_bytes = readBytes(source.path);
    external_bytes += " \r\n";
    writeBytes(source.path, external_bytes);
    const auto rejected = service->editRenderFeatures(
        {{"base_source_digest",
          service->documentForTesting().sourceDigest()},
         {"operations",
          operation("remove", buildAvailableReference)}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("code") ==
            "external_modification");
    REQUIRE(readBytes(source.path) == external_bytes);
    REQUIRE(runtime.snapshot.published_generation ==
            generation_after_success);
}

TEST_CASE(
    "WP331 failed apply restores disk and leaves runtime unchanged",
    "[render-config-editor][wp331][failure-atomic]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    runtime.fail_after_source_commit = true;
    auto service = makeService(source, runtime);
    const auto baseline_bytes = readBytes(source.path);
    const auto baseline_snapshot = runtime.snapshot;
    const auto baseline_plan = runtime.frame_plan;

    const auto result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(result.at("status") == "rejected");
    REQUIRE(result.at("committed") == false);
    REQUIRE(result.at("error").at("code") ==
            "render_pipeline_preflight_failed");
    REQUIRE(result.at("error").at("participant") ==
            "pelican.render_pipeline");
    REQUIRE(readBytes(source.path) == baseline_bytes);
    REQUIRE(runtime.snapshot.published_generation ==
            baseline_snapshot.published_generation);
    REQUIRE(runtime.snapshot.enabled_feature_names ==
            baseline_snapshot.enabled_feature_names);
    REQUIRE(runtime.frame_plan == baseline_plan);
}

TEST_CASE(
    "WP331 editor can edit under rpc while ReloadGate is disabled",
    "[render-config-editor][wp331][can-edit]") {
    EngineLaunchConfig launch;
    launch.rpc = true;
    launch.shader_hot_reload = true;
    watch::ReloadGate reload_gate;
    reload_gate.configureFromLaunch(launch);
    REQUIRE_FALSE(reload_gate.enabled());
    REQUIRE(reload_gate.snapshot().reason == "rpc driver");

    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto service = makeService(source, runtime, [] {
        return RenderConfigEditorGateObservation{
            .can_edit = true,
        };
    });
    const auto result = submitAndCommit(
        *service, service->documentForTesting().sourceDigest(),
        operation("add", buildAvailableReference));
    REQUIRE(result.at("committed") == true);
    REQUIRE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
}

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP331 list_render_features RPC exactly reflects registered engine documents",
    "[render-config-editor][wp331][catalog][rpc]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto render_service =
        std::shared_ptr<RenderConfigEditorService>{
            makeService(source, runtime).release()};

    const auto scene_bytes = readBytes(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test" / "fixtures" / "authoring_scene" /
        "multi_scene_roundtrip.json");
    const auto scene_document =
        AuthoringSceneDocument::load(
            scene_bytes, SceneRevision{1});
    EditorCommandService commands{
        EditorCommandServiceDependencies{
            .document = [&scene_document]()
                -> const AuthoringSceneDocument & {
                return scene_document;
            },
            .current_scene_id = [] {
                return std::string{"main"};
            },
            .render_config_editor = render_service,
        }};
    EditorCommandRpcAdapter adapter{commands};
    std::istringstream input;
    std::ostringstream output;
    RpcServer rpc{input, output};
    configureEditorRpcHandlers(
        rpc, adapter,
        EditorRpcHandlerHooks{
            .snapshot_imported = [] {},
            .save_busy = [] { return false; },
        });

    const auto response = Json::parse(rpc.processLine(
        R"json({"jsonrpc":"2.0","id":331,"method":"list_render_features","params":{}})json"));
    REQUIRE(response.at("id") == 331);
    const auto &actual =
        response.at("result").at("features");

    std::set<std::pair<std::string, bool>> expected;
    for (const auto id : registeredEngineResourceIds()) {
        if (!id.starts_with("features/") ||
            !id.ends_with(".json")) {
            continue;
        }
        const auto bytes = engineResource(id);
        REQUIRE(bytes.has_value());
        const auto document = Json::parse(*bytes);
        if (document.value("schema", std::string{}) !=
            "pelican.render_feature") {
            continue;
        }
        const auto name =
            document.at("name").get<std::string>();
        expected.emplace(
            "engine://" + std::string{id},
            renderFeatureRequiresRuntimeModule(name));
    }
    std::set<std::pair<std::string, bool>> observed;
    for (const auto &entry : actual) {
        observed.emplace(
            entry.at("reference").get<std::string>(),
            entry.at("requires_runtime_module")
                .get<bool>());
        const auto available = entry.at("available").get<bool>();
        REQUIRE(entry.at("hot_add_supported").get<bool>() ==
                available);
        REQUIRE(available !=
                entry.contains("unavailable_reason"));
    }
    REQUIRE(observed == expected);
}
#endif

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP331 render feature RPC accepts a ticket and exposes the committed result",
    "[render-config-editor][wp331][rpc][ticket]") {
    TemporaryConfig source{emptyHybridConfig()};
    ActualCpuRenderRuntime runtime;
    runtime.initialize(readBytes(source.path));
    auto render_service =
        std::shared_ptr<RenderConfigEditorService>{
            makeService(source, runtime).release()};

    const auto scene_bytes = readBytes(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test" / "fixtures" / "authoring_scene" /
        "multi_scene_roundtrip.json");
    const auto scene_document =
        AuthoringSceneDocument::load(
            scene_bytes, SceneRevision{1});
    EditorCommandService commands{
        EditorCommandServiceDependencies{
            .document = [&scene_document]()
                -> const AuthoringSceneDocument & {
                return scene_document;
            },
            .current_scene_id = [] {
                return std::string{"main"};
            },
            .render_config_editor = render_service,
        }};
    EditorCommandRpcAdapter adapter{commands};
    std::istringstream input;
    std::ostringstream output;
    RpcServer rpc{input, output};
    configureEditorRpcHandlers(
        rpc, adapter,
        EditorRpcHandlerHooks{
            .snapshot_imported = [] {},
            .save_busy = [] { return false; },
        });
    const auto call = [&](int id, std::string_view method,
                          Json params) {
        return Json::parse(rpc.processLine(
            Json{{"jsonrpc", "2.0"},
                 {"id", id},
                 {"method", method},
                 {"params", std::move(params)}}
                .dump()));
    };

    REQUIRE_FALSE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
    const auto current =
        call(1, "get_render_features", Json::object());
    const auto digest = current.at("result")
                            .at("source_digest")
                            .at("hex")
                            .get<std::string>();
    const auto accepted = call(
        2, "edit_render_features",
        {{"base_source_digest", digest},
         {"operations",
          operation("add", buildAvailableReference)}});
    REQUIRE(accepted.at("result").at("status") ==
            "accepted");
    const auto ticket = accepted.at("result")
                            .at("ticket")
                            .get<std::string>();

    commands.commitPendingEdits();
    const auto completed = call(
        3, "get_edit_result", {{"ticket", ticket}});
    REQUIRE(completed.at("result").at("committed") == true);
    REQUIRE(hasProvider(
        runtime.frame_plan, buildAvailableFeature));
    const auto frame_results = commands.takeCompletedEditResults();
    REQUIRE(frame_results.size() == 1);
    REQUIRE(frame_results.front().at("ticket") == ticket);
}
#endif

TEST_CASE("Feature materialization saves the concrete feature and switches its owner atomically",
          "[render-config-editor][feature-materialize-edit]") {
    for (const auto scope : {std::string{"root"}, std::string{"preset"}}) {
        DYNAMIC_SECTION(scope) {
            TemporaryAnimgraphProject project;
            PathResolver resolver;
            resolver.setup(project.root, false);
            auto root = Json::parse(readBytes(project.render_config));
            auto shadow = Json::parse(resolver.loadText("engine://features/sky_ambient.json"));
            shadow["passes"][0]["pass"]["regions"] = Json::array({"region.shadow"});
            shadow["integrations"] = Json::array({
                {{"name", "active_connection"}, {"requires", Json::array({"ui"})},
                 {"fragment", {{"pass_overrides", {{"scene_present", {{"input", Json::array({"scene_depth"})}}}}}}}},
                {{"name", "inactive_connection"}, {"requires", Json::array({"not_enabled"})},
                 {"fragment", {{"pass_overrides", {{"deferred_lighting", {{"input", Json::array({"scene_depth"})}}}}}}}}});
            auto integrated_pass = shadow.at("passes").at(0);
            integrated_pass["insert"] = "after:sky_background";
            integrated_pass["pass"]["name"] = "integrated_sky";
            integrated_pass["pass"].erase("regions");
            shadow["integrations"][0]["fragment"]["passes"] = Json::array({integrated_pass});
            shadow["integrations"][0]["fragment"]["compute_tasks"] = Json::array({{
                {"name", "integrated_compute"}, {"shader", "project://shaders/probe"},
                {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}}});
            writeBytes(project.root / "passes" / "shadow-source.json", shadow.dump(2));
            renderingSectionOf(root)["features"][0] = {{"ref", "project://passes/shadow-source.json"}, {"parameters", {{"color_r", 0.7}}}};
            renderingSectionOf(root)["features"].erase(1); // The selected local sky replaces the engine sky instance.
            const auto preset_path = project.root / "passes" / "editable.json";
            auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
            auto &graph = preset["config"]["rendering_passes"][0];
            graph["region_replacements"] = Json::array({
                {{"region", "region.shadow"}, {"provider", "builtin.identity_v1"}},
                {{"region", "region.present"}, {"provider", "builtin.identity_v1"}},
                {{"region", "region.lighting"}, {"provider", "builtin.identity_v1"}}});
            for (auto &pass : graph["passes"]) {
                if (pass.at("name") == "scene_present") pass["regions"] = Json::array({"region.present"});
                if (pass.at("name") == "deferred_lighting") pass["regions"] = Json::array({"region.lighting"});
            }
            if (scope == "preset") {
                preset["config"]["features"] = renderingSectionOf(root).at("features");
                renderingSectionOf(root)["features"] = Json::array();
            }
            writeBytes(preset_path, preset.dump(2));
            const auto preset_before = readBytes(preset_path);
            renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/editable.json";
            writeBytes(project.render_config, root.dump(2));
            const auto owner_path = scope == "root" ? project.render_config : preset_path;
            const auto owner_before = readBytes(owner_path);
            const auto destination = project.root / "passes" / "shadow-local.json";
            ActualCpuRenderRuntime runtime;
            auto service = makeProjectService(project, resolver, runtime);
            const auto before = service->getRenderFeatures(Json::object());
            REQUIRE_FALSE(before.at("feature_instances").empty());
            const auto &instance = before.at("feature_instances").at(0);
            Json operation{{"op", "materialize"}, {"scope", scope}, {"index", 0},
                           {"destination_reference", "project://passes/shadow-local.json"},
                           {"base_feature_digest", instance.at("feature_document_digest").at("hex")}};
            if (scope == "preset") operation["base_preset_digest"] = instance.at("source_digest").at("hex");
            Json request{{"base_source_digest", before.at("source_digest").at("hex")}, {"operations", Json::array({operation})}};
            SECTION("save and reopen preserve bindings and enable pass editing") {
                const auto accepted = service->editRenderFeatures(request);
                INFO(accepted.dump());
                REQUIRE(accepted.at("status") == "accepted");
                CHECK_FALSE(std::filesystem::exists(destination));
                CHECK(readBytes(owner_path) == owner_before);
                service->commitPending();
                const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
                INFO(result.dump());
                REQUIRE(result.at("committed") == true);
                auto saved = Json::parse(readBytes(destination));
                // P-2a-1c: the derivative keeps the scalar declarations so its
                // parameters stay editable after materialization.
                CHECK(saved.at("parameters").at("scalars") == shadow.at("parameters").at("scalars"));
                CHECK(saved.at("bound_parameters").at("color_r") == 0.7);
                const auto owner = Json::parse(readBytes(owner_path));
                // MG-2b: the root owner is a graph root; its features are in
                // the section.
                CHECK((scope == "root" ? renderingSectionOf(owner) : owner.at("config")).at("features").at(0) == "project://passes/shadow-local.json");
                const auto saved_preset = Json::parse(readBytes(preset_path));
                CHECK(saved_preset.at("config").at("rendering_passes").at(0).at("region_replacements") ==
                    Json::array({{{"region", "region.lighting"}, {"provider", "builtin.identity_v1"}}}));
                service.reset();
                service = makeProjectService(project, resolver, runtime);
                const auto context = service->getRenderAuthoringContext(Json::object());
                const auto edit = service->editPresetPass({
                    {"base_source_digest", context.at("source_digest").at("hex")},
                    {"base_feature_digest", renderConfigSourceDigest(readBytes(destination))},
                    {"graph", "main_render"}, {"pass_name", "sky_background"}, {"ray_query", false},
                    {"input", saved.at("passes").at(0).at("pass").at("input")},
                    {"output", saved.at("passes").at(0).at("pass").at("output")},
                    {"shader", saved.at("passes").at(0).at("pass").at("shader")}});
                INFO(edit.dump());
                REQUIRE(edit.at("status") == "accepted");
                service->commitPending();
                REQUIRE(service->getResult({{"ticket", edit.at("ticket")}}).at("committed") == true);
                saved = Json::parse(readBytes(destination));
                CHECK(saved.at("passes").at(0).at("pass").at("ray_query") == false);
                CHECK(saved.at("bound_parameters").at("color_r") == 0.7);
                const auto root_digest = service->getRenderFeatures(Json::object()).at("source_digest").at("hex");
                auto declaration = integrated_pass.at("pass");
                const auto integration_edit = service->editPresetPass({
                    {"base_source_digest", root_digest}, {"base_feature_digest", renderConfigSourceDigest(readBytes(destination))},
                    {"graph", "main_render"}, {"pass_name", "integrated_sky"}, {"ray_query", false},
                    {"input", declaration.at("input")}, {"output", declaration.at("output")}, {"shader", declaration.at("shader")}});
                INFO(integration_edit.dump());
                REQUIRE(integration_edit.at("status") == "accepted");
                service->commitPending();
                REQUIRE(service->getResult({{"ticket", integration_edit.at("ticket")}}).at("committed") == true);
                const auto task_context = service->getComputeTaskAuthoring(Json::object());
                REQUIRE(task_context.at("tasks").size() == 1);
                const auto &task = task_context.at("tasks").at(0);
                REQUIRE(task.at("editable") == true);
                auto changed_task = task.at("declaration");
                changed_task["dispatch"]["groups"] = {2, 1, 1};
                const auto task_edit = service->editComputeTask({
                    {"base_source_digest", root_digest}, {"base_task_source_digest", task.at("source_digest").at("hex")},
                    {"task_name", "integrated_compute"}, {"task", changed_task}});
                INFO(task_edit.dump());
                REQUIRE(task_edit.at("status") == "accepted");
                service->commitPending();
                REQUIRE(service->getResult({{"ticket", task_edit.at("ticket")}}).at("committed") == true);
                service.reset();
                service = makeProjectService(project, resolver, runtime);
                saved = Json::parse(readBytes(destination));
                const auto &fragment = saved.at("integrations").at(0).at("fragment");
                CHECK(fragment.at("passes").at(0).at("pass").at("ray_query") == false);
                CHECK(fragment.at("compute_tasks").at(0) == changed_task);
                CHECK(saved.at("passes").at(0).at("pass").at("name") == "sky_background");
            }
            SECTION("failed candidate leaves the owner and destination untouched") {
                runtime.fail_before_source_commit = true;
                const auto accepted = service->editRenderFeatures(request);
                REQUIRE(accepted.at("status") == "accepted");
                service->commitPending();
                CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
                CHECK_FALSE(std::filesystem::exists(destination));
                CHECK(readBytes(owner_path) == owner_before);
                CHECK(readBytes(preset_path) == preset_before);
            }
        }
    }
}

TEST_CASE("P-2a-4a authoring reads and writes the rendering section of a graph root",
          "[render-config-editor][graph-root][p2a4a]") {
    TemporaryAnimgraphProject project;
    writeBytes(project.render_config, graphRootFixture("main"));
    PathResolver resolver;
    resolver.setup(project.root, false);
    // Two compute tasks from a project feature: a graph root may not declare
    // compute_tasks in its section (the lowering owns them), so the frame node
    // order test needs nodes that come from a feature.
    Json tasks = Json::array();
    for (const auto *name : {"order_first", "order_second"})
        tasks.push_back({{"name", name}, {"shader", "project://shaders/probe"},
                         {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}});
    writeBytes(project.root / "passes" / "order_tasks.json",
               Json{{"schema", "pelican.render_feature"}, {"version", 1},
                    {"name", "order_tasks"}, {"compute_tasks", tasks}}.dump(2));
    {
        auto document = Json::parse(readBytes(project.render_config));
        document.at("rendering").at("features").push_back(
            "project://passes/order_tasks.json");
        writeBytes(project.render_config, document.dump(2) + "\n");
    }
    const auto before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    // Acceptance 1: the dock sees what the composer sees.
    const auto features = service->getRenderFeatures(Json::object());
    INFO(features.dump());
    REQUIRE(features.at("features") ==
            Json::array({"engine://features/bloom.json",
                         "engine://features/ui.json",
                         "project://passes/order_tasks.json"}));
    REQUIRE(features.at("feature_instances").size() == 3);
    CHECK(features.at("feature_instances").at(0).at("reference") ==
          "engine://features/bloom.json");
    CHECK(features.at("feature_instances").at(0).at("scope") == "root");
    CHECK(features.at("feature_instances").at(0).at("materializable") == true);
    CHECK(features.at("has_uneditable_feature_entries") == false);

    const auto commit = [&](const nlohmann::ordered_json &submission) {
        INFO(submission.dump());
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        const auto result =
            service->getResult({{"ticket", submission.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        return readBytes(project.render_config);
    };
    const auto sourceDigest = [&] {
        return service->getRenderFeatures(Json::object())
            .at("source_digest")
            .at("hex")
            .get<std::string>();
    };

    SECTION("feature instance parameters are written into the section") {
        const auto after = commit(service->editRenderFeatures(
            {{"base_source_digest", sourceDigest()},
             {"operations",
              Json::array({Json{{"op", "set_parameters"},
                                {"scope", "root"},
                                {"index", 0},
                                {"parameters", {{"intensity", 3.5}}}}})}}));
        requireOnlySectionKeyChanged(before, after, "features");
        CHECK(Json::parse(after).at("rendering").at("features").at(0) ==
              Json{{"ref", "engine://features/bloom.json"},
                   {"parameters", {{"intensity", 3.5}}}});
        // The CAS is unchanged by the move: the digest still covers the whole
        // document, so a stale base is still refused.
        const auto stale = service->editRenderFeatures(
            {{"base_source_digest", renderConfigSourceDigest(before)},
             {"operations", Json::array()}});
        CHECK(stale.at("status") == "rejected");
        CHECK(stale.at("error").at("code") == "external_modification");
    }

    SECTION("an authored pass adds its managed fragment to the section") {
        const Json pass{
            {"name", "p2a4a_authored_probe"},
            {"type", "fullscreen"},
            {"input", Json::array({"gbuffer_normal"})},
            {"output", {{"color", Json::array({"ssao_output"})}, {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}}}};
        const auto accepted = service->addAuthoredPass(
            {{"base_source_digest", sourceDigest()},
             {"graph", "main_render"},
             {"insert", "after:ssao_pass"},
             {"pass", pass}});
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        const auto reference =
            accepted.at("fragment_reference").get<std::string>();
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}})
                    .at("committed") == true);
        const auto after = readBytes(project.render_config);
        requireOnlySectionKeyChanged(before, after, "features");
        const auto section_features =
            Json::parse(after).at("rendering").at("features");
        CHECK(section_features.back() == reference);
        CHECK(std::ranges::any_of(
            runtime.frame_plan.at("nodes"), [](const auto &node) {
                return node.value("name", std::string{}) ==
                       "p2a4a_authored_probe";
            }));
        // The managed fragment is reachable from the section, so its baseline
        // is loaded and an external edit is still detected.
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        const auto context =
            service->getRenderAuthoringContext(Json::object());
        const auto fragments = context.at("managed_fragments");
        REQUIRE(fragments.size() == 1);
        CHECK(fragments.at(0).at("reference") == reference);
    }

    SECTION("a new compute task feature is referenced from the section") {
        const Json task{{"name", "p2a4a_task"},
                        {"shader", "project://shaders/probe"},
                        {"dispatch", {{"groups", {1, 1, 1}}}},
                        {"schedule", "per_frame"}};
        const auto after = commit(service->addComputeTask(
            {{"base_source_digest", sourceDigest()},
             {"destination_reference", "project://passes/p2a4a_task.json"},
             {"task", task}}));
        requireOnlySectionKeyChanged(before, after, "features");
        CHECK(Json::parse(after).at("rendering").at("features").back() ==
              "project://passes/p2a4a_task.json");
    }

    SECTION("frame node order is written into the section and reorders the plan") {
        const auto position = [&](std::string_view name) {
            const auto &nodes = runtime.frame_plan.at("nodes");
            const auto found = std::find_if(
                nodes.begin(), nodes.end(), [&](const auto &node) {
                    return node.at("name").template get_ref<const std::string &>() == name;
                });
            REQUIRE(found != nodes.end());
            return found - nodes.begin();
        };
        REQUIRE(position("order_first") < position("order_second"));
        REQUIRE(service->getFrameNodeOrder(Json::object()).at("orders").empty());
        const Json orders{{"main_render", {"order_second", "order_first"}}};
        const auto after = commit(service->editFrameNodeOrder(
            {{"base_source_digest",
              service->getFrameNodeOrder(Json::object()).at("source_digest").at("hex")},
             {"orders", orders}}));
        requireOnlySectionKeyChanged(before, after, "frame_node_order");
        CHECK(Json::parse(after).at("rendering").at("frame_node_order") == orders);
        CHECK(position("order_second") < position("order_first"));
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        CHECK(service->getFrameNodeOrder(Json::object()).at("orders") == orders);
    }

    SECTION("scene acceleration structure definitions are written into the section") {
        const auto context =
            service->getSceneAccelerationStructures(Json::object());
        REQUIRE(context.at("definitions") == Json::array());
        const Json definitions =
            Json::array({{{"name", "p2a4a_probe_as"}, {"instance_mask", 1}}});
        const auto after = commit(service->editSceneAccelerationStructures(
            {{"base_source_digest", context.at("source_digest").at("hex")},
             {"definitions", definitions}}));
        requireOnlySectionKeyChanged(before, after,
                                     "scene_acceleration_structures");
        CHECK(Json::parse(after).at("rendering").at("scene_acceleration_structures") ==
              definitions);
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        CHECK(service->getSceneAccelerationStructures(Json::object())
                  .at("definitions") == definitions);
    }
}

TEST_CASE("P-2a-4a a graph root without a rendering section is refused by name",
          "[render-config-editor][graph-root][p2a4a]") {
    auto document = Json::parse(graphRootFixture("main"));
    document.erase("rendering");
    TemporaryConfig source{document.dump(2)};
    ActualCpuRenderRuntime runtime;
    auto service = makeService(source, runtime);
    const auto before = readBytes(source.path);

    // Reading answers "nothing authored here" instead of inventing a root.
    const auto features = service->getRenderFeatures(Json::object());
    CHECK(features.at("features") == Json::array());
    CHECK(features.at("feature_instances").empty());

    const auto digest = features.at("source_digest").at("hex").get<std::string>();
    const auto rejected = service->editRenderFeatures(
        {{"base_source_digest", digest},
         {"operations", operation("add", buildAvailableReference)}});
    INFO(rejected.dump());
    CHECK(rejected.at("status") == "rejected");
    CHECK(rejected.at("error").at("code") == "render_config_section_unlocated");
    const auto order_rejected = service->editFrameNodeOrder(
        {{"base_source_digest", digest}, {"orders", Json::object()}});
    CHECK(order_rejected.at("status") == "rejected");
    CHECK(order_rejected.at("error").at("code") ==
          "render_config_section_unlocated");
    CHECK(readBytes(source.path) == before);
}

TEST_CASE("P-2a-4a a preset under a graph root forks and takes features in its section",
          "[render-config-editor][graph-root][p2a4a][preset]") {
    TemporaryAnimgraphProject project;
    writeBytes(project.render_config, graphRootFixture("preset_main"));
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto commit = [&](const nlohmann::ordered_json &submission) {
        INFO(submission.dump());
        REQUIRE(submission.at("status") == "accepted");
        service->commitPending();
        const auto result =
            service->getResult({{"ticket", submission.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        return readBytes(project.render_config);
    };

    const auto context = service->getRenderAuthoringContext(Json::object());
    REQUIRE(context.at("config_kind") == "preset");
    REQUIRE(context.at("pipeline_preset").at("name") == "hybrid_v1");

    SECTION("the forked preset reference replaces the section's pipeline") {
        const auto after = commit(service->forkRenderPipeline(
            {{"base_source_digest", context.at("source_digest").at("hex")},
             {"base_preset_digest",
              context.at("pipeline_preset").at("source_digest").at("hex")},
             {"destination_reference", "project://passes/local-pipeline.json"}}));
        requireOnlySectionKeyChanged(before, after, "pipeline");
        CHECK(Json::parse(after).at("rendering").at("pipeline").at("preset") ==
              "project://passes/local-pipeline.json");
        CHECK(std::filesystem::exists(project.root / "passes" /
                                      "local-pipeline.json"));
    }

    SECTION("features are removed from and added back to the section") {
        const auto removed = commit(service->editRenderFeatures(
            {{"base_source_digest",
              service->getRenderFeatures(Json::object()).at("source_digest").at("hex")},
             {"operations", operation("remove", buildAvailableReference)}}));
        requireOnlySectionKeyChanged(before, removed, "features");
        CHECK(Json::parse(removed).at("rendering").at("features").size() == 2);
        const auto added = commit(service->editRenderFeatures(
            {{"base_source_digest",
              service->getRenderFeatures(Json::object()).at("source_digest").at("hex")},
             {"operations", operation("add", buildAvailableReference)}}));
        requireOnlySectionKeyChanged(removed, added, "features");
        CHECK(Json::parse(added).at("rendering").at("features").size() == 3);
        CHECK(Json::parse(added).at("rendering").at("features").back() ==
              std::string{buildAvailableReference});
    }
}

TEST_CASE("P-2a-4a root scope materialization rewrites the section and releases its regions",
          "[render-config-editor][graph-root][p2a4a][feature-materialize-edit]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    // A project copy of the shipping bloom component whose composite pass
    // carries a region tag, so materializing it must release the request the
    // section holds -- the graph-root twin of the preset release path.
    auto bloom = Json::parse(resolver.loadText("engine://features/bloom.json"));
    auto &bloom_passes = bloom.at("passes");
    bloom_passes.back().at("pass")["regions"] = Json::array({"region.bloom"});
    writeBytes(project.root / "passes" / "bloom-source.json", bloom.dump(2));

    auto document = Json::parse(graphRootFixture("main"));
    // N-6: the editor's own placement is a root member nothing composes. A
    // materialization rewrites the whole `rendering` section, and the
    // placement has to come back out of it exactly as it went in.
    // N-8 adds `frame_plan` to the same member -- the Frame Plan canvas's own
    // placement -- so it is carried through by the same claim.
    document["layout"] = Json{
        {"nodes", {{"clock", {{"x", 12.5}, {"y", -4.0}}}}},
        {"reroutes", Json::array()},
        {"ties", Json::array()},
        {"frame_plan",
         {{"main_render",
           {{"nodes", {{"ssao_pass", {{"x", 64.0}, {"y", 18.0}}}}},
            {"reroutes", Json::array()},
            {"ties", Json::array()}}}}}};
    auto &section = document.at("rendering");
    section.at("features").at(0) = "project://passes/bloom-source.json";
    for (auto &pass : section.at("rendering_passes").at(0).at("passes"))
        if (pass.at("name") == "ssao_pass")
            pass["regions"] = Json::array({"region.ssao"});
    section.at("rendering_passes").at(0)["region_replacements"] = Json::array({
        {{"region", "region.bloom"}, {"provider", "builtin.identity_v1"}},
        {{"region", "region.ssao"}, {"provider", "builtin.identity_v1"}}});
    writeBytes(project.render_config, document.dump(2) + "\n");
    const auto before = readBytes(project.render_config);

    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto features = service->getRenderFeatures(Json::object());
    const auto &instance = features.at("feature_instances").at(0);
    REQUIRE(instance.at("reference") == "project://passes/bloom-source.json");
    REQUIRE(instance.at("materializable") == true);
    const auto accepted = service->editRenderFeatures(
        {{"base_source_digest", features.at("source_digest").at("hex")},
         {"operations",
          Json::array({Json{{"op", "materialize"},
                            {"scope", "root"},
                            {"index", 0},
                            {"destination_reference",
                             "project://passes/bloom-local.json"},
                            {"base_feature_digest",
                             instance.at("feature_document_digest").at("hex")}}})}});
    INFO(accepted.dump());
    REQUIRE(accepted.at("status") == "accepted");
    service->commitPending();
    const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);

    const auto after = readBytes(project.render_config);
    const auto after_document = Json::parse(after);
    REQUIRE(rootKeys(after_document) == rootKeys(Json::parse(before)));
    // N-6 acceptance 1, the carrying half: the placement is the same object and
    // the same bytes, which a reformatting write-back would not leave it.
    CHECK(after_document.at("layout") == document.at("layout"));
    {
        const auto member = std::string{"\"layout\": "} +
                            document.at("layout").dump(2);
        // `before` is `document.dump(2)`, so the member is spelled the same way
        // in both; finding it in `after` is the byte-for-byte claim.
        const auto placed = before.find("\"layout\"");
        REQUIRE(placed != std::string::npos);
        const auto end = before.find("\n  \"rendering\"", placed);
        REQUIRE(end != std::string::npos);
        CHECK(after.find(before.substr(placed, end - placed)) !=
              std::string::npos);
        (void)member;
    }
    const auto &after_section = after_document.at("rendering");
    // The owner reference moved in the section, not at the root.
    CHECK(after_section.at("features").at(0) ==
          "project://passes/bloom-local.json");
    // The released region is gone from the section's own graph; the unrelated
    // request is still there.  Without the section locator this whole edit is
    // a no-op (the root has no rendering_passes to look at).
    CHECK(after_section.at("rendering_passes").at(0).at("region_replacements") ==
          Json::array({{{"region", "region.ssao"},
                        {"provider", "builtin.identity_v1"}}}));
    const auto derived = Json::parse(
        readBytes(project.root / "passes" / "bloom-local.json"));
    CHECK(derived.at("name") == "bloom");
    CHECK(derived.at("passes").size() == bloom.at("passes").size());
    CHECK(derived.contains("bound_parameters"));
}

TEST_CASE("The shipping bloom component is readable from the project and derives without touching the engine document",
          "[render-config-editor][feature-materialize-edit][bloom][p2a1b]") {
    TemporaryAnimgraphProject project;
    // The shipping project is the subject: P-2a-1b made its bloom an engine
    // component instead of 13 authored passes.
    writeBytes(project.render_config,
               readBytes(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                         "projects" / "example" / "passes" /
                         "main_rendering_config.json"));
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    // Operation 1, "look inside": the project names the component by its
    // engine reference, and that reference resolves to the 13 bloom passes.
    const auto features = service->getRenderFeatures(Json::object());
    const auto &instances = features.at("feature_instances");
    const auto bloom = std::find_if(
        instances.begin(), instances.end(), [](const auto &instance) {
            return instance.at("reference") ==
                   "engine://features/bloom.json";
        });
    REQUIRE(bloom != instances.end());
    CHECK(bloom->at("scope") == "root");
    CHECK(bloom->at("materializable") == true);
    const auto engine_bytes =
        engineResourceOrThrow("features/bloom.json");
    CHECK(bloom->at("feature_document_digest").at("hex") ==
          renderConfigSourceDigest(engine_bytes));
    const auto engine_document = Json::parse(engine_bytes);
    std::vector<std::string> pass_names;
    for (const auto &entry : engine_document.at("passes")) {
        pass_names.push_back(
            entry.at("pass").at("name").get<std::string>());
    }
    CHECK(pass_names ==
          std::vector<std::string>{
              "HighLuminanceExtraction", "HorizontalBlur_0",
              "VerticalBlur_0", "HorizontalBlur_1", "VerticalBlur_1",
              "HorizontalBlur_2", "VerticalBlur_2", "HorizontalBlur_3",
              "VerticalBlur_3", "UpsampleBlend_3", "UpsampleBlend_2",
              "UpsampleBlend_1", "FinalBloomComposite"});
    CHECK(engine_document.at("render_targets").size() == 9);

    // Operation 2, "derive for the project": root-scope materialization writes
    // the concrete document into the project and repoints the owner. The
    // engine copy in the repository is not a write target.
    const auto engine_source =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" / "core" /
        "resources" / "features" / "bloom.json";
    const auto engine_source_before = readBytes(engine_source);
    const auto index = bloom->at("index").get<std::size_t>();
    const auto accepted = service->editRenderFeatures(
        {{"base_source_digest",
          features.at("source_digest").at("hex")},
         {"operations",
          Json::array({Json{
              {"op", "materialize"},
              {"scope", "root"},
              {"index", index},
              {"destination_reference",
               "project://passes/bloom-local.json"},
              {"base_feature_digest",
               bloom->at("feature_document_digest").at("hex")}}})}});
    INFO(accepted.dump());
    REQUIRE(accepted.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}})
                .at("committed") == true);

    const auto derived = Json::parse(
        readBytes(project.root / "passes" / "bloom-local.json"));
    CHECK(derived.at("name") == "bloom");
    CHECK(derived.at("passes") == engine_document.at("passes"));
    CHECK(derived.at("render_targets") ==
          engine_document.at("render_targets"));
    // P-2a-4b: the subject is the shipping `pelican.graph` root, so the
    // editor wrote the reference into its `rendering` section. Reading the
    // document root here would be reading where the composer never looks.
    const auto derived_root = Json::parse(readBytes(project.render_config));
    CHECK(renderingSectionOf(derived_root).at("features").at(index) ==
          "project://passes/bloom-local.json");
    CHECK(readBytes(engine_source) == engine_source_before);

    // P-2a-1c, operation 3 "keep tuning the derivative": the derived document
    // keeps the scalar declarations and records the values it was cut with, so
    // the feature list still offers a schema for it and set_parameters is
    // accepted.  Before 1c parameter_schema was null here, the instance was
    // not editable, and composing an override threw.
    CHECK(derived.at("parameters").at("scalars") ==
          engine_document.at("parameters").at("scalars"));
    CHECK(derived.at("bound_parameters").at("intensity") == 2.0);
    // The scalar defines are regenerated at compose time from the declarations
    // instead of being frozen into the document.
    CHECK_FALSE(derived.contains("shader_defines"));

    const auto after = service->getRenderFeatures(Json::object());
    const auto &derived_instance = after.at("feature_instances").at(index);
    CHECK(derived_instance.at("reference") == "project://passes/bloom-local.json");
    CHECK(derived_instance.at("materializable") == false);
    CHECK(derived_instance.at("editable") == true);
    CHECK(derived_instance.at("bound_parameters").at("intensity") == 2.0);
    const auto &derived_scalars =
        derived_instance.at("parameter_schema").at("scalars");
    const auto intensity = std::find_if(
        derived_scalars.begin(), derived_scalars.end(),
        [](const auto &declaration) { return declaration.at("name") == "intensity"; });
    REQUIRE(intensity != derived_scalars.end());
    CHECK(intensity->at("default") == 2.0);

    const auto edited = service->editRenderFeatures(
        {{"base_source_digest", after.at("source_digest").at("hex")},
         {"operations",
          Json::array({Json{{"op", "set_parameters"},
                            {"scope", "root"},
                            {"index", index},
                            {"parameters", {{"intensity", 5.0}}}}})}});
    INFO(edited.dump());
    REQUIRE(edited.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", edited.at("ticket")}})
                .at("committed") == true);
    const auto tuned_root = Json::parse(readBytes(project.render_config));
    CHECK(renderingSectionOf(tuned_root).at("features").at(index) ==
          Json{{"ref", "project://passes/bloom-local.json"},
               {"parameters", {{"intensity", 5.0}}}});
}

TEST_CASE("P-2a-3 a derived feature is published as a component and copied into another project",
          "[render-config-editor][render-component][p2a3]") {
    TemporaryDirectory user_root{"p2a3-user"};
    TemporaryAnimgraphProject source_project;
    writeBytes(source_project.render_config, graphRootFixture("main"));
    PathResolver source_resolver;
    source_resolver.setup(source_project.root, false,
                          readBytes(source_project.root / "project.json"),
                          user_root.path);
    ActualCpuRenderRuntime source_runtime;
    auto source_service =
        makeProjectService(source_project, source_resolver, source_runtime);

    // Only a derivative is shareable, so the export starts where P-2a-1b ends.
    const auto before = source_service->getRenderFeatures(Json::object());
    const auto &engine_bloom = before.at("feature_instances").at(0);
    REQUIRE(engine_bloom.at("reference") == "engine://features/bloom.json");
    const auto materialized = source_service->editRenderFeatures(
        {{"base_source_digest", before.at("source_digest").at("hex")},
         {"operations",
          Json::array({Json{{"op", "materialize"},
                            {"scope", "root"},
                            {"index", 0},
                            {"destination_reference",
                             "project://passes/bloom-local.json"},
                            {"base_feature_digest",
                             engine_bloom.at("feature_document_digest").at("hex")}}})}});
    INFO(materialized.dump());
    REQUIRE(materialized.at("status") == "accepted");
    source_service->commitPending();
    REQUIRE(source_service->getResult({{"ticket", materialized.at("ticket")}})
                .at("committed") == true);
    source_service.reset();
    source_service =
        makeProjectService(source_project, source_resolver, source_runtime);
    const auto derived_bytes =
        readBytes(source_project.root / "passes" / "bloom-local.json");
    const auto project_before = readBytes(source_project.render_config);

    // An engine feature is already shared; the component is the derivative.
    const auto engine_export =
        source_service->exportRenderFeature({{"scope", "root"}, {"index", 1}});
    CHECK(engine_export.at("status") == "rejected");
    CHECK(engine_export.at("error").at("code") ==
          "render_component_engine_reference");

    // An export that dies between staging and rename leaves `<name>.tmp-<pid>`
    // behind.  The next export sweeps the ones another process left; a staging
    // directory this process owns may belong to an export still in flight, and
    // a component whose own name merely contains ".tmp-" is not a staging
    // directory at all.
    const auto components_dir = user_root.path / "components";
    std::filesystem::create_directories(components_dir);
    const auto own_staging =
        components_dir / ("in_flight.tmp-" + currentProcessIdText());
    const std::filesystem::path planted[] = {
        components_dir / "bloom.tmp-999999999",
        components_dir / "abandoned.tmp-1", own_staging,
        components_dir / "not-a-staging.tmp-x"};
    for (const auto &directory : planted) {
        std::filesystem::create_directories(directory);
        writeBytes(directory / "feature.json", "{}");
    }

    const auto exported =
        source_service->exportRenderFeature({{"scope", "root"}, {"index", 0}});
    INFO(exported.dump());
    REQUIRE(exported.at("status") == "exported");
    CHECK(exported.at("component_reference") == "user://components/bloom");
    const auto component_dir = user_root.path / "components" / "bloom";
    REQUIRE(std::filesystem::is_regular_file(component_dir / "feature.json"));
    REQUIRE(std::filesystem::is_regular_file(component_dir / "component.json"));
    CHECK(readBytes(component_dir / "feature.json") == derived_bytes);
    const auto manifest = Json::parse(readBytes(component_dir / "component.json"));
    CHECK(manifest.at("schema") == "pelican.render_component");
    CHECK(manifest.at("version") == 1);
    CHECK(manifest.at("name") == "bloom");
    CHECK(manifest.at("sha256") == renderConfigSourceDigest(derived_bytes));
    CHECK(manifest.at("source").at("reference") ==
          "project://passes/bloom-local.json");
    // The host contract: bloom reads the project's lit_color and writes the
    // engine's swapchain. Neither is in the feature's own render_targets.
    CHECK(manifest.at("requires").at("render_targets") ==
          Json::array({"lit_color", "swapchain"}));
    // Publishing is a read of the project.
    CHECK(readBytes(source_project.render_config) == project_before);
    // No staging directory of this export survives it, and the two that other
    // processes abandoned are gone with it.
    std::set<std::string> remaining;
    for (const auto &entry :
         std::filesystem::directory_iterator{components_dir}) {
        remaining.insert(entry.path().filename().string());
    }
    CHECK(remaining ==
          std::set<std::string>{"bloom", own_staging.filename().string(),
                                "not-a-staging.tmp-x"});
    std::filesystem::remove_all(own_staging);
    std::filesystem::remove_all(components_dir / "not-a-staging.tmp-x");
    const auto repeated =
        source_service->exportRenderFeature({{"scope", "root"}, {"index", 0}});
    CHECK(repeated.at("status") == "rejected");
    CHECK(repeated.at("error").at("code") ==
          "render_component_destination_exists");

    TemporaryDirectory destination_user_root{"p2a3-user2"};
    TemporaryAnimgraphProject destination_project;
    auto destination_document = Json::parse(graphRootFixture("main"));
    destination_document.at("rendering").at("features") =
        Json::array({"engine://features/ui.json"});
    writeBytes(destination_project.render_config,
               destination_document.dump(2) + "\n");
    PathResolver destination_resolver;
    destination_resolver.setup(
        destination_project.root, false,
        readBytes(destination_project.root / "project.json"),
        destination_user_root.path);
    ActualCpuRenderRuntime destination_runtime;
    auto destination_service = makeProjectService(
        destination_project, destination_resolver, destination_runtime);
    const auto destination_digest = [&] {
        return destination_service->getRenderFeatures(Json::object())
            .at("source_digest")
            .at("hex")
            .get<std::string>();
    };
    const auto hasBloomPasses = [](const Json &plan) {
        return std::ranges::any_of(plan.at("nodes"), [](const auto &node) {
            return node.value("name", std::string{}) == "FinalBloomComposite";
        });
    };
    REQUIRE_FALSE(hasBloomPasses(destination_runtime.frame_plan));

    SECTION("the imported component is a copy the source no longer owns") {
        const auto accepted = destination_service->importRenderFeature(
            {{"base_source_digest", destination_digest()},
             {"source_dir", component_dir.string()},
             {"destination_reference", "project://passes/bloom-imported.json"}});
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        destination_service->commitPending();
        REQUIRE(destination_service
                    ->getResult({{"ticket", accepted.at("ticket")}})
                    .at("committed") == true);
        const auto imported_path =
            destination_project.root / "passes" / "bloom-imported.json";
        CHECK(readBytes(imported_path) == derived_bytes);
        const auto saved = Json::parse(readBytes(destination_project.render_config));
        CHECK(saved.at("rendering").at("features") ==
              Json::array({"engine://features/ui.json",
                           "project://passes/bloom-imported.json"}));
        CHECK_FALSE(saved.contains("features"));
        CHECK(hasBloomPasses(destination_runtime.frame_plan));

        // The control for "copy": deleting the published component leaves the
        // importing project drawing the same frame.
        const auto plan_before = destination_runtime.frame_plan;
        std::filesystem::remove_all(component_dir);
        destination_service.reset();
        destination_service = makeProjectService(
            destination_project, destination_resolver, destination_runtime);
        CHECK(destination_runtime.frame_plan == plan_before);
        CHECK(hasBloomPasses(destination_runtime.frame_plan));

        // A second import of the same destination is refused by name.
        std::filesystem::create_directories(component_dir);
        writeBytes(component_dir / "feature.json", derived_bytes);
        writeBytes(component_dir / "component.json",
                   Json{{"schema", "pelican.render_component"},
                        {"version", 1},
                        {"name", "bloom"},
                        {"feature", "feature.json"},
                        {"sha256", renderConfigSourceDigest(derived_bytes)},
                        {"requires", {{"render_targets", Json::array({"lit_color", "swapchain"})}}}}
                       .dump(2));
        const auto again = destination_service->importRenderFeature(
            {{"base_source_digest", destination_digest()},
             {"source_dir", component_dir.string()},
             {"destination_reference", "project://passes/bloom-imported.json"}});
        CHECK(again.at("status") == "rejected");
        CHECK(again.at("error").at("code") == "feature_destination_exists");
    }

    SECTION("a component that does not survive the trip is refused by name") {
        const auto project_bytes = readBytes(destination_project.render_config);
        const auto refuse = [&](const std::filesystem::path &source) {
            return destination_service->importRenderFeature(
                {{"base_source_digest", destination_digest()},
                 {"source_dir", source.string()},
                 {"destination_reference",
                  "project://passes/bloom-imported.json"}});
        };

        TemporaryDirectory tampered{"p2a3-tampered"};
        const auto tampered_component = tampered.path / "bloom";
        std::filesystem::create_directories(tampered_component);
        writeBytes(tampered_component / "feature.json", derived_bytes + " ");
        writeBytes(tampered_component / "component.json",
                   readBytes(component_dir / "component.json"));
        const auto mismatched = refuse(tampered_component);
        CHECK(mismatched.at("status") == "rejected");
        CHECK(mismatched.at("error").at("code") ==
              "render_component_digest_mismatch");

        std::filesystem::remove(tampered_component / "component.json");
        const auto invalid = refuse(tampered_component);
        CHECK(invalid.at("status") == "rejected");
        CHECK(invalid.at("error").at("code") == "render_component_invalid");

        const auto inside = refuse(destination_project.root / "passes");
        CHECK(inside.at("status") == "rejected");
        CHECK(inside.at("error").at("code") ==
              "render_component_source_inside_project");

        // Nothing above reached a transaction.
        CHECK(readBytes(destination_project.render_config) == project_bytes);
        CHECK_FALSE(std::filesystem::exists(destination_project.root / "passes" /
                                            "bloom-imported.json"));
    }
}

TEST_CASE("P-2a-3 a component states the host contract it cannot carry",
          "[render-config-editor][render-component][p2a3]") {
    // The shipping project:// feature: four project shader references, so the
    // one candidate the shipped projects offer is refused by name instead of
    // producing a component that cannot be built anywhere else.
    const auto spheres = readBytes(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                                   "projects" / "raytracing_demo" / "features" /
                                   "spheres.json");
    try {
        (void)buildRenderComponent(spheres, "project://features/spheres.json",
                                   "raytracing_demo");
        FAIL("spheres.json carries project:// shaders and is not shareable");
    } catch (const RenderComponentError &error) {
        CHECK(error.code() == "render_component_project_reference");
        CHECK(std::string{error.what()}.find("project://shaders/") !=
              std::string::npos);
    }

    // A project that declares none of the render targets the component reads
    // is told which names are missing rather than composing into a failure.
    TemporaryDirectory user_root{"p2a3-minimal-user"};
    TemporaryAnimgraphProject project;
    const auto minimal = Json::parse(R"json({
      "schema": "pelican.graph", "version": 1,
      "definitions": {}, "nodes": [], "outputs": {},
      "rendering": {
        "features": [],
        "render_targets": [],
        "rendering_passes": [{"name": "main_render", "passes": [{
          "name": "present", "type": "fullscreen", "input": [],
          "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"},
          "output": {"color": "swapchain", "depth": null}}]}]
      }})json");
    writeBytes(project.render_config, minimal.dump(2) + "\n");
    PathResolver resolver;
    resolver.setup(project.root, false,
                   readBytes(project.root / "project.json"), user_root.path);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    TemporaryDirectory library{"p2a3-library"};
    const auto component = library.path / "bloom";
    const auto feature = engineResourceOrThrow("features/bloom.json");
    std::filesystem::create_directories(component);
    writeBytes(component / "feature.json", feature);
    writeBytes(component / "component.json",
               Json{{"schema", "pelican.render_component"},
                    {"version", 1},
                    {"name", "bloom"},
                    {"feature", "feature.json"},
                    {"sha256", renderConfigSourceDigest(feature)},
                    {"requires",
                     {{"render_targets", Json::array({"lit_color", "swapchain"})}}}}
                   .dump(2));
    const auto rejected = service->importRenderFeature(
        {{"base_source_digest",
          service->getRenderFeatures(Json::object()).at("source_digest").at("hex")},
         {"source_dir", component.string()},
         {"destination_reference", "project://passes/bloom-imported.json"}});
    INFO(rejected.dump());
    CHECK(rejected.at("status") == "rejected");
    CHECK(rejected.at("error").at("code") == "render_component_requires_missing");
    // swapchain is the engine's own output and is never missing.
    CHECK(rejected.at("error").at("render_targets") == Json::array({"lit_color"}));
    CHECK_FALSE(std::filesystem::exists(project.root / "passes" /
                                        "bloom-imported.json"));
}

TEST_CASE("Pipeline fork saves the original preset and switches the root atomically",
          "[render-config-editor][preset-fork]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto before = service->getRenderAuthoringContext(Json::object());
    const auto root_before = readBytes(project.render_config);
    const auto plan_before = runtime.frame_plan;
    const auto check_structure = [&] {
        REQUIRE(runtime.frame_plan.at("nodes").size() == plan_before.at("nodes").size());
        for (std::size_t i = 0; i < plan_before.at("nodes").size(); ++i) {
            for (const auto *key : {"name", "kind", "reads", "writes"})
                REQUIRE(runtime.frame_plan.at("nodes").at(i).at(key) == plan_before.at("nodes").at(i).at(key));
        }
        REQUIRE(runtime.frame_plan.at("resources").size() == plan_before.at("resources").size());
        for (std::size_t i = 0; i < plan_before.at("resources").size(); ++i)
            REQUIRE(runtime.frame_plan.at("resources").at(i).at("name") == plan_before.at("resources").at(i).at("name"));
    };
    const auto preset_bytes = resolver.loadText(
        before.at("pipeline_preset").at("reference").get<std::string>());
    const auto destination = project.root / "passes" / "local-pipeline.json";
    const Json request{
        {"base_source_digest", before.at("source_digest").at("hex")},
        {"base_preset_digest", before.at("pipeline_preset").at("source_digest").at("hex")},
        {"destination_reference", "project://passes/local-pipeline.json"}};

    SECTION("publish and reopen preserve features without composing them twice") {
        const auto accepted = service->forkRenderPipeline(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE(readBytes(project.render_config) == root_before);
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        REQUIRE(readBytes(destination) == preset_bytes);
        auto expected_root = Json::parse(root_before);
        renderingSectionOf(expected_root)["pipeline"]["preset"] = request.at("destination_reference");
        REQUIRE(Json::parse(readBytes(project.render_config)) == expected_root);
        check_structure();
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        REQUIRE(service->getRenderAuthoringContext(Json::object())
                    .at("pipeline_preset").at("reference") == request.at("destination_reference"));
        check_structure();
    }
    SECTION("failed publication rolls back both files") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->forkRenderPipeline(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE(readBytes(project.render_config) == root_before);
        REQUIRE(runtime.frame_plan == plan_before);
    }
    SECTION("existing destination is never overwritten") {
        writeBytes(destination, "user file");
        REQUIRE(service->forkRenderPipeline(request).at("status") == "rejected");
        REQUIRE(readBytes(destination) == "user file");
        REQUIRE(readBytes(project.render_config) == root_before);
    }
    SECTION("source preset changes while queued reject the fork") {
        const auto source = project.root / "passes" / "source-preset.json";
        writeBytes(source, preset_bytes);
        auto local_root = Json::parse(root_before);
        renderingSectionOf(local_root)["pipeline"]["preset"] = "project://passes/source-preset.json";
        writeBytes(project.render_config, local_root.dump());
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        auto local_request = request;
        local_request["base_source_digest"] = service->getRenderAuthoringContext(Json::object()).at("source_digest").at("hex");
        const auto accepted = service->forkRenderPipeline(local_request);
        REQUIRE(accepted.at("status") == "accepted");
        writeBytes(source, preset_bytes + "\n");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == false);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE(Json::parse(readBytes(project.render_config)) == local_root);
        REQUIRE(readBytes(source) == preset_bytes + "\n");
    }
}

TEST_CASE("Frame node order edits persist and preserve the old state on invalid order",
          "[render-config-editor][frame-node-order]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto path = project.root / "passes" / "order_tasks.json";
    Json tasks = Json::array();
    for (const auto *name : {"order_first", "order_second"})
        tasks.push_back({{"name", name}, {"shader", "project://shaders/probe"},
                         {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}});
    writeBytes(path, Json{{"schema", "pelican.render_feature"}, {"version", 1},
                         {"name", "order_tasks"}, {"compute_tasks", tasks}}.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["features"].push_back("project://passes/order_tasks.json");
    writeBytes(project.render_config, root.dump(2));
    const auto before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto position = [&](std::string_view name) {
        const auto &nodes = runtime.frame_plan.at("nodes");
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
            return node.at("name").template get_ref<const std::string &>() == name;
        });
        REQUIRE(found != nodes.end());
        return found - nodes.begin();
    };
    REQUIRE(position("order_first") < position("order_second"));
    const auto context = service->getFrameNodeOrder(Json::object());
    REQUIRE(context.at("orders").empty());
    Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                 {"orders", {{"main_render", {"order_second", "order_first"}}}}};
    SECTION("save reopen and clear") {
        const auto accepted = service->editFrameNodeOrder(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        CHECK(position("order_second") < position("order_first"));
        CHECK(service->editFrameNodeOrder(request).at("status") == "rejected");
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        CHECK(service->getFrameNodeOrder(Json::object()).at("orders") == request.at("orders"));
        CHECK(position("order_second") < position("order_first"));
        request["base_source_digest"] = service->getFrameNodeOrder(Json::object()).at("source_digest").at("hex");
        request["orders"] = Json::object();
        const auto cleared = service->editFrameNodeOrder(request);
        REQUIRE(cleared.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", cleared.at("ticket")}}).at("committed") == true);
        CHECK(position("order_first") < position("order_second"));
    }
    SECTION("unknown node cannot replace the source") {
        request["orders"]["main_render"] = {"missing"};
        const auto accepted = service->editFrameNodeOrder(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(project.render_config) == before);
        CHECK(position("order_first") < position("order_second"));
    }
    SECTION("publication failure restores the source") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->editFrameNodeOrder(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(project.render_config) == before);
    }
}

TEST_CASE("Compute task creation and removal publish project features transactionally",
          "[render-config-editor][compute-task-create]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto before = readBytes(project.render_config);
    const auto path = project.root / "passes" / "created.json";
    const Json task{{"name", "created_compute"}, {"shader", "project://shaders/probe"},
                    {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}};
    const Json request{{"base_source_digest", renderConfigSourceDigest(before)},
                       {"destination_reference", "project://passes/created.json"}, {"task", task}};
    SECTION("create remove and reopen") {
        REQUIRE(service->getComputeTaskAuthoring(Json::object()).at("tasks").empty());
        const auto accepted = service->addComputeTask(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        REQUIRE_FALSE(std::filesystem::exists(path));
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        REQUIRE(hasProvider(runtime.frame_plan, "created_compute"));
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        const auto catalog = service->getComputeTaskAuthoring(Json::object());
        REQUIRE(catalog.at("tasks").size() == 1);
        REQUIRE(catalog.at("tasks")[0].at("declaration") == task);
        auto duplicate = request;
        duplicate["base_source_digest"] = catalog.at("source_digest").at("hex");
        duplicate["destination_reference"] = "project://passes/duplicate.json";
        REQUIRE(service->addComputeTask(duplicate).at("status") == "rejected");
        const auto root_after_add = readBytes(project.render_config);
        const Json removal{{"base_source_digest", catalog.at("source_digest").at("hex")},
                           {"base_task_source_digest", catalog.at("tasks")[0].at("source_digest").at("hex")},
                           {"task_name", "created_compute"}};
        const auto removed = service->removeComputeTask(removal);
        REQUIRE(removed.at("status") == "accepted");
        service->commitPending();
        const auto removal_result = service->getResult({{"ticket", removed.at("ticket")}});
        INFO(removal_result.dump());
        REQUIRE(removal_result.at("committed") == true);
        REQUIRE_FALSE(hasProvider(runtime.frame_plan, "created_compute"));
        REQUIRE(readBytes(project.render_config) == root_after_add);
        REQUIRE(Json::parse(readBytes(path)).at("compute_tasks").empty());
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        REQUIRE(service->getComputeTaskAuthoring(Json::object()).at("tasks").empty());
        REQUIRE(service->removeComputeTask(removal).at("status") == "rejected");
    }
    SECTION("failed publication restores root and removes created file") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->addComputeTask(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(project.render_config) == before);
        REQUIRE_FALSE(std::filesystem::exists(path));
    }
    SECTION("a file created after acceptance is not overwritten") {
        const auto accepted = service->addComputeTask(request);
        REQUIRE(accepted.at("status") == "accepted");
        writeBytes(path, "external file");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(project.render_config) == before);
        REQUIRE(readBytes(path) == "external file");
    }
}

TEST_CASE("Compute task editing resolves project presets",
          "[render-config-editor][compute-task-edit]") {
    // MG-2b: the root is a `pelican.graph` root, whose `rendering` section
    // cannot declare compute tasks (the graph owns them; the lowering refuses
    // the key by name). A task declared in a project preset is edited in the
    // preset; the root's tasks are refused (MG-3, the test below).
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
    const Json task{{"name", "owned_compute"}, {"shader", "project://shaders/probe"},
                    {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}};
    preset["config"]["compute_tasks"] = Json::array({task});
    const auto preset_path = project.root / "passes" / "owned.json";
    writeBytes(preset_path, preset.dump(2));
    writeBytes(project.render_config,
        Pelican::TestSupport::renderGraphRoot(Json{{"pipeline", {{"preset", "project://passes/owned.json"}}}}).dump(2));
    const auto root_before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto catalog = service->getComputeTaskAuthoring(Json::object());
    REQUIRE(catalog.at("tasks").size() == 1);
    const auto &selected = catalog.at("tasks")[0];
    REQUIRE(selected.at("editable") == true);
    auto changed = task;
    changed["dispatch"]["groups"] = {2, 1, 1};
    const auto accepted = service->editComputeTask({
        {"base_source_digest", catalog.at("source_digest").at("hex")},
        {"base_task_source_digest", selected.at("source_digest").at("hex")},
        {"task_name", "owned_compute"}, {"task", changed}});
    INFO(accepted.dump());
    REQUIRE(accepted.at("status") == "accepted");
    service->commitPending();
    const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);
    REQUIRE(readBytes(project.render_config) == root_before);
    service.reset();
    service = makeProjectService(project, resolver, runtime);
    REQUIRE(service->getComputeTaskAuthoring(Json::object()).at("tasks")[0].at("declaration") == changed);
    const auto updated = service->getComputeTaskAuthoring(Json::object());
    const auto removed = service->removeComputeTask({
        {"base_source_digest", updated.at("source_digest").at("hex")},
        {"base_task_source_digest", updated.at("tasks")[0].at("source_digest").at("hex")},
        {"task_name", "owned_compute"}});
    REQUIRE(removed.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", removed.at("ticket")}}).at("committed") == true);
    REQUIRE(service->getComputeTaskAuthoring(Json::object()).at("tasks").empty());
    REQUIRE(readBytes(project.render_config) == root_before);
}

// MG-3: the rendering root holds no tasks. A task the composed config credits
// to the root -- here one a render.compute node in the root graph declares --
// is listed with the rest, read-only and with its reason, and an edit or a
// removal of it is refused by name before anything is written.
TEST_CASE("MG-3 a task credited to the rendering root is listed read-only and refused by name",
          "[render-config-editor][compute-task-edit][mg3]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    // The blank template's shape (no `pipeline`, so nothing is a preset's),
    // with one compute node in the root graph writing one buffer.
    auto root = Pelican::TestSupport::renderGraphRoot(Json{
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes", Json::array({{{"name", "present"},
                                                {"type", "fullscreen"},
                                                {"input", Json::array()},
                                                {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
                                                {"shader", {{"vertex", "engine://fullscreen"},
                                                            {"fragment", "engine://fullscreen"}}}}})}}})}});
    root["nodes"] = Json::array({
        {{"id", "probe_data"}, {"op", "render.buffer"},
         {"parameters", {{"size", 16}, {"element", "float"}}}, {"outputs", {"resource"}}},
        {{"id", "probe"}, {"op", "render.compute"},
         {"parameters", {{"shader", "project://shaders/probe"}, {"dispatch", {{"groups", {1, 1, 1}}}},
                         {"ports", {{"values", {{"access", "write"}, {"element", "float"}}}}}}},
         {"inputs", {{"values", {{"node", "probe_data"}, {"port", "resource"}}}}},
         {"outputs", {"values"}}}});
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    const auto catalog = service->getComputeTaskAuthoring(Json::object());
    INFO(catalog.dump());
    const auto &tasks = catalog.at("tasks");
    const auto probe = std::find_if(tasks.begin(), tasks.end(), [](const auto &entry) {
        return entry.at("name") == "probe";
    });
    // Listed, not left out: the canvas and the Task Editor show it.
    REQUIRE(probe != tasks.end());
    CHECK(probe->at("source_reference") == "passes/main.json");
    CHECK(probe->at("editable") == false);
    // MG-3b: the task the root is credited with is the render.compute node's,
    // and says so -- where the node is, and where to edit it.
    CHECK(probe->at("origin") == "graph_node");
    CHECK(Json::parse(probe->at("graph_node").dump()) == Json{{"scope", "root"}, {"node", "probe"}, {"instance", ""}});
    CHECK(probe->at("read_only_reason") ==
          "This task is made by a render.compute node in the graph. Edit the node in the Graph tab.");
    CHECK_FALSE(probe->contains("provider_feature"));

    Json task = probe->at("declaration");
    task["dispatch"] = {{"groups", {2, 1, 1}}};
    const Json edit{{"base_source_digest", catalog.at("source_digest").at("hex")},
                    {"base_task_source_digest", std::string(64, '0')},
                    {"task_name", "probe"},
                    {"task", task}};
    const auto edited = service->editComputeTask(edit);
    INFO(edited.dump());
    REQUIRE(edited.at("status") == "rejected");
    CHECK(edited.at("error").at("code") == "compute_task_not_editable");

    auto removal = edit;
    removal.erase("task");
    const auto removed = service->removeComputeTask(removal);
    INFO(removed.dump());
    REQUIRE(removed.at("status") == "rejected");
    CHECK(removed.at("error").at("code") == "compute_task_not_editable");

    service->commitPending();
    CHECK(readBytes(project.render_config) == root_before);
}

// MG-3b: a task a render.compute node makes names that node. The lowering
// names each task after the node's expanded id, so a node inside a definition
// placed by a graph.instance makes `<instance>/<node>`; the catalog says which
// scope the node is authored in (the scope token the graph edit ops take),
// the id it has there and the instance that placed it, so the Graph tab can
// go to it. The task stays read-only and is refused by name.
TEST_CASE("MG-3b a render.compute node's task names its node, in the root and in a definition",
          "[render-config-editor][compute-task-edit][mg3b]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto compute = [](const char *id, const char *buffer) {
        return Json{{"id", id}, {"op", "render.compute"},
                    {"parameters", {{"shader", "project://shaders/probe"}, {"dispatch", {{"groups", {1, 1, 1}}}},
                                    {"ports", {{"values", {{"access", "write"}, {"element", "float"}}}}}}},
                    {"inputs", {{"values", {{"node", buffer}, {"port", "resource"}}}}},
                    {"outputs", {"values"}}};
    };
    const auto buffer = [](const char *id) {
        return Json{{"id", id}, {"op", "render.buffer"},
                    {"parameters", {{"size", 16}, {"element", "float"}}}, {"outputs", {"resource"}}};
    };
    // The blank template's shape, as in the MG-3 test above.
    auto root = Pelican::TestSupport::renderGraphRoot(Json{
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes", Json::array({{{"name", "present"},
                                                {"type", "fullscreen"},
                                                {"input", Json::array()},
                                                {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
                                                {"shader", {{"vertex", "engine://fullscreen"},
                                                            {"fragment", "engine://fullscreen"}}}}})}}})}});
    root["definitions"] = {{"smooth", {{"nodes", Json::array({buffer("data"), compute("step", "data")})},
                                       {"outputs", Json::object()}}}};
    root["nodes"] = Json::array({buffer("probe_data"), compute("probe", "probe_data"),
                                 {{"id", "left"}, {"op", "graph.instance"}, {"definition", "smooth"}}});
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    const auto catalog = service->getComputeTaskAuthoring(Json::object());
    INFO(catalog.dump());
    const auto &tasks = catalog.at("tasks");
    const auto find = [&](const char *name) {
        const auto found = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) {
            return entry.at("name") == name;
        });
        REQUIRE(found != tasks.end());
        return *found;
    };
    const std::string reason =
        "This task is made by a render.compute node in the graph. Edit the node in the Graph tab.";
    const auto probe = find("probe");
    CHECK(probe.at("origin") == "graph_node");
    CHECK(Json::parse(probe.at("graph_node").dump()) == Json{{"scope", "root"}, {"node", "probe"}, {"instance", ""}});
    CHECK(probe.at("source_reference") == "passes/main.json");
    CHECK(probe.at("editable") == false);
    CHECK(probe.at("read_only_reason") == reason);
    const auto step = find("left/step");
    CHECK(step.at("origin") == "graph_node");
    CHECK(Json::parse(step.at("graph_node").dump()) == Json{{"scope", "smooth"}, {"node", "step"}, {"instance", "left"}});
    CHECK(step.at("source_reference") == "passes/main.json");
    CHECK(step.at("editable") == false);
    CHECK(step.at("read_only_reason") == reason);
    CHECK_FALSE(step.contains("provider_feature"));

    Json task = step.at("declaration");
    task["dispatch"] = {{"groups", {2, 1, 1}}};
    const auto edited = service->editComputeTask({{"base_source_digest", catalog.at("source_digest").at("hex")},
                                                  {"base_task_source_digest", std::string(64, '0')},
                                                  {"task_name", "left/step"},
                                                  {"task", task}});
    INFO(edited.dump());
    REQUIRE(edited.at("status") == "rejected");
    CHECK(edited.at("error").at("code") == "compute_task_not_editable");
    service->commitPending();
    CHECK(readBytes(project.render_config) == root_before);
}

// MG-3b: a root that names a preset still authors its render.compute nodes
// itself. Their tasks are the graph's -- not the preset's, whether the preset
// is the project's or the engine's -- while a task the preset declares stays
// the preset's and editable.
TEST_CASE("MG-3b in a preset root a render.compute node's task is the graph's, the preset's task the preset's",
          "[render-config-editor][compute-task-edit][mg3b]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
    const Json owned{{"name", "owned_compute"}, {"shader", "project://shaders/probe"},
                     {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}};
    preset["config"]["compute_tasks"] = Json::array({owned});
    writeBytes(project.root / "passes" / "owned.json", preset.dump(2));
    const auto nodes = Json::array({
        {{"id", "probe_data"}, {"op", "render.buffer"},
         {"parameters", {{"size", 16}, {"element", "float"}}}, {"outputs", {"resource"}}},
        {{"id", "probe"}, {"op", "render.compute"},
         {"parameters", {{"shader", "project://shaders/probe"}, {"dispatch", {{"groups", {1, 1, 1}}}},
                         {"ports", {{"values", {{"access", "write"}, {"element", "float"}}}}}}},
         {"inputs", {{"values", {{"node", "probe_data"}, {"port", "resource"}}}}},
         {"outputs", {"values"}}}});
    const std::string reason =
        "This task is made by a render.compute node in the graph. Edit the node in the Graph tab.";
    for (const std::string reference : {"project://passes/owned.json", "engine://render_pipelines/hybrid_v1.json"}) {
        INFO(reference);
        auto root = Pelican::TestSupport::renderGraphRoot(Json{{"pipeline", {{"preset", reference}}}});
        root["nodes"] = nodes;
        writeBytes(project.render_config, root.dump(2));
        ActualCpuRenderRuntime runtime;
        auto service = makeProjectService(project, resolver, runtime);
        const auto catalog = service->getComputeTaskAuthoring(Json::object());
        INFO(catalog.dump());
        const auto &tasks = catalog.at("tasks");
        const auto probe = std::find_if(tasks.begin(), tasks.end(), [](const auto &entry) {
            return entry.at("name") == "probe";
        });
        REQUIRE(probe != tasks.end());
        CHECK(probe->at("origin") == "graph_node");
        CHECK(Json::parse(probe->at("graph_node").dump()) == Json{{"scope", "root"}, {"node", "probe"}, {"instance", ""}});
        CHECK(probe->at("source_reference") == "passes/main.json");
        CHECK(probe->at("editable") == false);
        CHECK(probe->at("read_only_reason") == reason);
        if (reference.starts_with("project://")) {
            const auto preset_task = std::find_if(tasks.begin(), tasks.end(), [](const auto &entry) {
                return entry.at("name") == "owned_compute";
            });
            REQUIRE(preset_task != tasks.end());
            CHECK(preset_task->at("origin") == "preset");
            CHECK(preset_task->at("source_reference") == reference);
            CHECK(preset_task->at("editable") == true);
            CHECK_FALSE(preset_task->contains("graph_node"));
            CHECK_FALSE(preset_task->contains("read_only_reason"));
        }
    }
}

TEST_CASE("Procedural asset edits reject a gate that closed and reopened before publication",
          "[render-config-editor][procedural-asset-create][gate]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto path = project.root / "gated_assets.json";
    const Json original{{"schema", "pelican.asset_data"}, {"version", 2}, {"models", Json::array()}};
    writeBytes(path, original.dump());
    bool open = true;
    int applied = 0;
    ProceduralAssetEditorService service({
        .reference = "project://gated_assets.json",
        .normalized_reference = resolver.normalizedReference("project://gated_assets.json"),
        .path = path, .project_root = project.root, .source_bytes = original.dump(),
        .gate = [&] { return RenderConfigEditorGateObservation{.can_edit = open}; },
        .apply = [&](std::string_view, const std::function<void()> &commit) {
            ++applied;
            commit();
        }});
    const auto digest = service.get(Json::object()).at("source_digest").at("hex");
    const Json request{{"operation", "create"}, {"base_source_digest", digest}, {"asset_name", "gated"},
        {"aabb", {{"min", {-1, -1, -1}}, {"max", {1, 1, 1}}}}, {"record", {{"radius", 1}}}};
    const auto accepted = service.edit(request);
    REQUIRE(accepted.at("status") == "accepted");
    open = false;
    CHECK(service.edit(request).at("error").at("code") == "gate_closed");
    open = true;
    service.commitPending();
    CHECK(service.result({{"ticket", accepted.at("ticket")}}).at("committed") == false);
    CHECK(applied == 0);
    CHECK(readBytes(path) == original.dump());
}

TEST_CASE("Procedural asset creation upgrades an empty asset document atomically",
          "[render-config-editor][procedural-asset-create]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto path = project.root / "created_assets.json";
    const Json original{{"schema", "pelican.asset_data"}, {"version", 2}, {"models", Json::array()}};
    writeBytes(path, original.dump());
    std::string published;
    ProceduralAssetEditorService service({
        .reference = "project://created_assets.json",
        .normalized_reference = resolver.normalizedReference("project://created_assets.json"),
        .path = path, .project_root = project.root, .source_bytes = original.dump(),
        .gate = [] { return RenderConfigEditorGateObservation{}; },
        .apply = [&](std::string_view bytes, const std::function<void()> &commit) {
            auto next = std::string(bytes);
            commit();
            published.swap(next);
        }});
    const auto digest = service.get(Json::object()).at("source_digest").at("hex");
    Json request{{"operation", "create"}, {"base_source_digest", digest}, {"asset_name", "new_sphere"},
        {"aabb", {{"min", {-1, -1, -1}}, {"max", {1, 1, 1}}}}, {"record", {{"radius", 1}}}};
    SECTION("first asset is saved and exposed for later editing") {
        const auto accepted = service.edit(request);
        REQUIRE(accepted.at("status") == "accepted");
        CHECK(readBytes(path) == original.dump());
        service.commitPending();
        REQUIRE(service.result({{"ticket", accepted.at("ticket")}}).at("committed") == true);
        CHECK(readBytes(path) == published);
        const auto saved = Json::parse(published);
        CHECK(saved.at("version") == 2);
        CHECK(saved.at("models") == original.at("models"));
        CHECK(service.get(Json::object()).at("assets")[0].at("kind") == "aabb_v1");
        const Json remove{{"operation", "delete"}, {"asset_name", "new_sphere"},
            {"base_source_digest", service.get(Json::object()).at("source_digest").at("hex")}};
        auto invalid_remove = remove;
        invalid_remove["record"] = Json::object();
        CHECK_THROWS(service.edit(invalid_remove));
        CHECK(readBytes(path) == published);
        const auto deleted = service.edit(remove);
        REQUIRE(deleted.at("status") == "accepted");
        service.commitPending();
        REQUIRE(service.result({{"ticket", deleted.at("ticket")}}).at("committed") == true);
        CHECK(service.get(Json::object()).at("assets").empty());
        CHECK(Json::parse(readBytes(path)).at("models") == original.at("models"));
        auto missing = remove;
        missing["base_source_digest"] = service.get(Json::object()).at("source_digest").at("hex");
        CHECK(service.edit(missing).at("error").at("code") == "asset_not_found");
    }
    SECTION("invalid bounds do not write or publish") {
        request["aabb"]["max"] = {-2, -2, -2};
        CHECK_THROWS(service.edit(request));
        CHECK(readBytes(path) == original.dump());
        CHECK(published.empty());
    }
}

TEST_CASE("Compute task source edits preserve other declarations and reject conflicting saves",
          "[render-config-editor][compute-task-edit]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const Json task{{"name", "editable_compute"}, {"shader", "project://shaders/probe"},
                    {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}};
    const auto source_path = project.root / "passes" / "compute.json";
    const Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "compute_edit"},
                       {"buffers", Json::array({"project_scratch", {{"name", "feature_args"}, {"size", 16}}})},
                       {"compute_tasks", Json::array({task})}};
    writeBytes(source_path, feature.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["features"].push_back("project://passes/compute.json");
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    const auto source_before = readBytes(source_path);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto catalog = service->getComputeTaskAuthoring(Json::object());
    REQUIRE(catalog.at("tasks").size() == 1);
    const auto &selected = catalog.at("tasks").at(0);
    REQUIRE(selected.at("editable") == true);
    // MG-3b: a feature's task says it is the feature's, and names no node.
    CHECK(selected.at("origin") == "feature");
    CHECK_FALSE(selected.contains("graph_node"));
    REQUIRE(selected.at("declaration") == task);
    REQUIRE(Json::parse(selected.at("declaration_json").get<std::string>()) == task);
    SECTION("connection catalog contains composed images and both buffer declaration forms") {
        const auto &resources = catalog.at("resources");
        for (const auto *name : {"project_scratch", "feature_args"}) {
            const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto &resource) {
                return resource.at("name") == name;
            });
            REQUIRE(found != resources.end());
            CHECK(found->at("kind") == "buffer");
            const auto declaration = Json::parse(found->at("declaration_json").template get<std::string>());
            CHECK((declaration.is_string() ? declaration.get<std::string>() : declaration.at("name").get<std::string>()) == name);
        }
        CHECK(std::any_of(resources.begin(), resources.end(), [](const auto &resource) {
            return resource.at("kind") == "image";
        }));
    }
    Json changed = task;
    changed["dispatch"]["groups"] = {2, 3, 1};
    Json request{{"base_source_digest", catalog.at("source_digest").at("hex")},
                 {"base_task_source_digest", selected.at("source_digest").at("hex")},
                 {"task_name", "editable_compute"}, {"task", changed}};
    SECTION("authoring text preserves uint64 values across JSON transports") {
        auto external = feature;
        constexpr std::uint64_t offset = UINT64_MAX - 3;
        external["compute_tasks"][0]["dispatch"] = {{"indirect", {{"buffer", "args"}, {"offset", offset}}}};
        writeBytes(source_path, external.dump(2));
        const auto exact = service->getComputeTaskAuthoring(Json::object()).at("tasks")[0].at("declaration_json").get<std::string>();
        REQUIRE(Json::parse(exact).at("dispatch").at("indirect").at("offset").get<std::uint64_t>() == offset);
    }
    SECTION("save and reopen") {
        const auto accepted = service->editComputeTask(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        auto expected = feature;
        expected["compute_tasks"][0] = changed;
        REQUIRE(Json::parse(readBytes(source_path)) == expected);
        REQUIRE(readBytes(project.render_config) == root_before);
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        REQUIRE(service->getComputeTaskAuthoring(Json::object()).at("tasks")[0].at("declaration") == changed);
        REQUIRE(service->editComputeTask(request).at("status") == "rejected");
    }
    SECTION("external change after acceptance") {
        const auto accepted = service->editComputeTask(request);
        REQUIRE(accepted.at("status") == "accepted");
        writeBytes(source_path, source_before + "\n");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(source_path) == source_before + "\n");
    }
    SECTION("failed publication restores the source") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->editComputeTask(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(source_path) == source_before);
    }
    SECTION("renaming is rejected") {
        request["task"]["name"] = "different";
        REQUIRE(service->editComputeTask(request).at("status") == "rejected");
        REQUIRE(readBytes(source_path) == source_before);
    }
}

TEST_CASE("Region replacement selection preserves authored nodes and survives reopen",
          "[render-config-editor][region-replacement-edit]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
    auto &graph = preset["config"]["rendering_passes"][0];
    auto &passes = graph["passes"];
    const auto pass = std::find_if(passes.begin(), passes.end(), [](const auto &value) {
        return value.at("name") == "scene_present";
    });
    REQUIRE(pass != passes.end());
    (*pass)["regions"] = Json::array({"editable.present"});
    const auto preset_path = project.root / "passes" / "editable.json";
    writeBytes(preset_path, preset.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/editable.json";
    writeBytes(project.render_config, root.dump(2));
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto make_request = [&](Json provider) {
        const auto context = service->getRenderAuthoringContext(Json::object());
        return Json{{"base_source_digest", context.at("source_digest").at("hex")},
                    {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                    {"graph", "main_render"}, {"region", "editable.present"}, {"provider", provider}};
    };
    SECTION("selection and removal retain pass settings and connections") {
        for (const auto &provider : Json::array({"builtin.identity_v1", nullptr})) {
            const auto request = make_request(provider);
            const auto accepted = service->editRegionReplacement(request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            const auto saved = Json::parse(readBytes(preset_path));
            CHECK(saved.at("config").at("rendering_passes").at(0).at("passes") == passes);
            CHECK(Json::parse(readBytes(project.render_config)) == root);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto context = service->getRenderAuthoringContext(Json::object());
            const auto requests = context.at("graphs").at(0).at("region_replacements");
            if (provider.is_null()) CHECK(requests.empty());
            else CHECK(requests == Json::array({{{"region", "editable.present"}, {"provider", provider}}}));
            CHECK(service->editRegionReplacement(request).at("error").at("code") == "external_modification");
        }
    }
    SECTION("unavailable replacement cannot publish or overwrite the authored graph") {
        const auto before = readBytes(preset_path);
        const auto generation = runtime.snapshot.published_generation;
        const auto accepted = service->editRegionReplacement(make_request("missing.provider"));
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(preset_path) == before);
        CHECK(runtime.snapshot.published_generation == generation);
    }
    SECTION("unknown regions are rejected") {
        auto request = make_request(nullptr);
        request["region"] = "missing.region";
        CHECK(service->editRegionReplacement(request).at("error").at("code") == "region_not_found");
    }
}

TEST_CASE("Project preset pass edits publish without changing the root document",
          "[render-config-editor][preset-pass-edit]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
    auto &passes = preset["config"]["rendering_passes"][0]["passes"];
    auto original = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
        return pass.at("name") == "scene_present";
    });
    REQUIRE(original != passes.end());
    (*original)["clear_color"] = Json::array({0.2, 0.3, 0.4, 1.0});
    const auto preset_path = project.root / "passes" / "editable.json";
    writeBytes(preset_path, preset.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/editable.json";
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    const auto preset_before = readBytes(preset_path);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context = service->getRenderAuthoringContext(Json::object());
    const auto &context_passes = context.at("graphs").at(0).at("passes");
    const auto editable = std::find_if(context_passes.begin(), context_passes.end(), [](const auto &pass) {
        return pass.at("name") == "scene_present";
    });
    REQUIRE(editable != context_passes.end());
    REQUIRE(editable->at("editable_preset_pass") == true);
    REQUIRE(editable->at("preset_declaration") == *original);
    const auto plan_before = runtime.frame_plan;
    Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                 {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                 {"graph", "main_render"}, {"pass_name", "scene_present"},
                 {"input", Json::array({"gbuffer_albedo"})},
                 {"output", original->at("output")},
                 {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}}}};
    SECTION("valid edit changes the runtime connection and survives reopen") {
        const auto accepted = service->editPresetPass(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        REQUIRE(readBytes(preset_path) == preset_before);
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        REQUIRE(result.at("source_commit_called") == true);
        for (const auto *key : {"input", "output", "shader"}) (*original)[key] = request.at(key);
        REQUIRE(Json::parse(readBytes(preset_path)) == preset);
        REQUIRE(readBytes(project.render_config) == root_before);
        const auto nodes = runtime.frame_plan.at("nodes");
        const auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &value) {
            return value.at("name") == "scene_present";
        });
        REQUIRE(node != nodes.end());
        REQUIRE(node->at("reads") == Json::array({"gbuffer_albedo"}));
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        REQUIRE(runtime.frame_plan.at("nodes") == nodes);
        REQUIRE(service->editPresetPass(request).at("error").at("code") == "external_modification");
    }
    SECTION("scene query declaration can be enabled and disabled without replacing other pass settings") {
        for (const bool enabled : {true, false}) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            request["base_preset_digest"] = current.at("pipeline_preset").at("source_digest").at("hex");
            request["ray_query"] = enabled;
            const auto accepted = service->editPresetPass(request);
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            for (const auto *key : {"input", "output", "shader", "ray_query"}) (*original)[key] = request.at(key);
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
        }
        request["ray_query"] = "true";
        CHECK_THROWS(service->editPresetPass(request));
    }
    SECTION("G-3 scene lights, push constants and clear colours are set and "
            "removed without replacing other pass settings") {
        const Json clear_colors{
            {"swapchain", Json::array({0.1, 0.2, 0.3, 1.0})}};
        for (const bool enabled : {true, false}) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            request["base_preset_digest"] = current.at("pipeline_preset").at("source_digest").at("hex");
            request["uses_light_data"] = enabled ? Json(true) : Json(nullptr);
            request["push_constants"] = enabled ? Json("camera_position") : Json(nullptr);
            request["clear_colors"] = enabled ? clear_colors : Json(nullptr);
            const auto accepted = service->editPresetPass(request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            for (const auto *key : {"input", "output", "shader"}) (*original)[key] = request.at(key);
            for (const auto *key : {"uses_light_data", "push_constants", "clear_colors"}) {
                if (enabled) (*original)[key] = request.at(key);
                else original->erase(key);
            }
            // clear_color, which the request does not name, is still there.
            CHECK(original->at("clear_color") == Json::array({0.2, 0.3, 0.4, 1.0}));
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
        }
        // The engine's own reading refuses a value it cannot use, and the
        // preset is left as it was.
        const auto current = service->getRenderAuthoringContext(Json::object());
        request["base_preset_digest"] = current.at("pipeline_preset").at("source_digest").at("hex");
        request["uses_light_data"] = "yes";
        const auto refused = service->editPresetPass(request);
        REQUIRE(refused.at("status") == "accepted");
        service->commitPending();
        const auto refused_result = service->getResult({{"ticket", refused.at("ticket")}});
        INFO(refused_result.dump());
        CHECK(refused_result.at("committed") == false);
        CHECK(Json::parse(readBytes(preset_path)) == preset);
    }
    SECTION("generated and feature nodes cannot be edited as preset passes") {
        request["pass_name"] = "__anchor_tonemap";
        REQUIRE(service->editPresetPass(request).at("status") == "rejected");
        request["pass_name"] = "pelican_ui";
        REQUIRE(service->editPresetPass(request).at("status") == "rejected");
        REQUIRE(readBytes(preset_path) == preset_before);
    }
    SECTION("material contract and preset routing edits publish together and reopen") {
        auto material = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.at("name") == "forward_transparent";
        });
        REQUIRE(material != passes.end());
        const auto routing = preset.at("config").at("material_routing");
        CHECK(context.at("preset_material_routing") == routing);
        const auto patches = Json::array({
            Json{{"material_settings", {{"material_contract", "forward_opaque_v1"},
                                         {"material_filter", {{"include", Json::array({"outlined"})}}}}},
                 {"material_routing", nullptr}},
            Json{{"material_settings", {{"material_contract", "forward_transparent_v1"}, {"material_filter", nullptr}}},
                 {"material_routing", routing}}});
        for (const auto &patch : patches) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            Json material_request{
                {"base_source_digest", current.at("source_digest").at("hex")},
                {"base_preset_digest", current.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "forward_transparent"}};
            material_request.update(patch);
            const auto accepted = service->editPresetPass(material_request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            for (const auto &[key, value] : patch.at("material_settings").items()) {
                if (value.is_null()) material->erase(key);
                else (*material)[key] = value;
            }
            if (patch.at("material_routing").is_null()) preset["config"].erase("material_routing");
            else preset["config"]["material_routing"] = patch.at("material_routing");
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto reopened = service->getRenderAuthoringContext(Json::object());
            CHECK(reopened.at("preset_material_routing") == patch.at("material_routing"));
            const auto &reopened_passes = reopened.at("graphs").at(0).at("passes");
            const auto saved = std::find_if(reopened_passes.begin(), reopened_passes.end(), [](const auto &pass) {
                return pass.at("name") == "forward_transparent";
            });
            REQUIRE(saved != reopened_passes.end());
            CHECK(Json(saved->at("preset_declaration")) == *material);
        }
    }
    SECTION("invalid material contract and routing fail before preset replacement") {
        auto invalid_routing = preset.at("config").at("material_routing");
        invalid_routing["routes"]["forward_transparent"] = "missing_pass";
        for (const auto &patch : Json::array({
                 Json{{"material_settings", {{"material_contract", "forward_opaque_v1"}}}},
                 Json{{"material_routing", invalid_routing}}})) {
            Json material_request{{"base_source_digest", context.at("source_digest").at("hex")},
                {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "forward_transparent"}};
            material_request.update(patch);
            const auto accepted = service->editPresetPass(material_request);
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
            CHECK(readBytes(preset_path) == preset_before);
            CHECK(readBytes(project.render_config) == root_before);
            CHECK(runtime.frame_plan == plan_before);
        }
    }
    SECTION("material settings publication failure restores the preset") {
        runtime.fail_after_source_commit = true;
        const Json material_request{{"base_source_digest", context.at("source_digest").at("hex")},
            {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
            {"graph", "main_render"}, {"pass_name", "forward_transparent"},
            {"material_settings", {{"material_contract", "forward_opaque_v1"}}}, {"material_routing", nullptr}};
        const auto accepted = service->editPresetPass(material_request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        CHECK(result.at("error").at("message") == "injected failure after source replacement");
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("fullscreen passes cannot change material settings or routing") {
        for (const auto *key : {"material_settings", "material_routing"}) {
            auto invalid = request;
            invalid[key] = Json::object();
            CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        }
        CHECK(readBytes(preset_path) == preset_before);
    }
    SECTION("material query edits preserve the rendering contract and survive reopen") {
        auto material = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.at("name") == "deferred_geometry";
        });
        REQUIRE(material != passes.end());
        const auto editable_material = std::find_if(context_passes.begin(), context_passes.end(), [](const auto &pass) {
            return pass.at("name") == "deferred_geometry";
        });
        REQUIRE(editable_material != context_passes.end());
        REQUIRE(editable_material->at("editable_preset_pass") == true);
        REQUIRE(editable_material->at("preset_declaration") == *material);
        for (const auto &query : Json::array({Json(true), Json{{"acceleration_structure", "scene"}}, Json(false)})) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            const Json material_request{
                {"base_source_digest", current.at("source_digest").at("hex")},
                {"base_preset_digest", current.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "deferred_geometry"}, {"ray_query", query}};
            const auto accepted = service->editPresetPass(material_request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            const auto before_commit = readBytes(preset_path);
            CHECK(Json::parse(before_commit) == preset);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            (*material)["ray_query"] = query;
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto reopened = service->getRenderAuthoringContext(Json::object());
            const auto &reopened_passes = reopened.at("graphs").at(0).at("passes");
            const auto saved = std::find_if(reopened_passes.begin(), reopened_passes.end(), [](const auto &pass) {
                return pass.at("name") == "deferred_geometry";
            });
            REQUIRE(saved != reopened_passes.end());
            CHECK(Json(saved->at("preset_declaration")) == *material);
            CHECK(service->editPresetPass(material_request).at("error").at("code") == "external_modification");
        }
    }
    SECTION("material connection and attachment edits preserve omitted fields and survive reopen") {
        auto material = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.at("name") == "forward_transparent";
        });
        REQUIRE(material != passes.end());
        const auto patches = Json::array({
            Json{{"input", Json::array({"gbuffer_albedo"})}},
            Json{{"output", {{"color", Json::array({"gbuffer_normal"})}, {"depth", "scene_depth"}}}},
            Json{{"input", Json::array()}}});
        for (const auto &patch : patches) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            Json material_request{
                {"base_source_digest", current.at("source_digest").at("hex")},
                {"base_preset_digest", current.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "forward_transparent"}};
            material_request.update(patch);
            const auto accepted = service->editPresetPass(material_request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            REQUIRE(result.at("source_commit_called") == true);
            material->update(patch);
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
            const auto nodes = runtime.frame_plan.at("nodes");
            const auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &value) {
                return value.at("name") == "forward_transparent";
            });
            REQUIRE(node != nodes.end());
            const bool reads_albedo = std::ranges::find(node->at("reads"), Json("gbuffer_albedo"))
                                      != node->at("reads").end();
            CHECK(reads_albedo == !material->at("input").empty());
            for (const auto &target : material->at("output").at("color")) {
                CHECK(std::ranges::find(node->at("writes"), target) != node->at("writes").end());
            }
            CHECK(std::ranges::find(node->at("writes"), Json("scene_depth")) != node->at("writes").end());
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            CHECK(runtime.frame_plan.at("nodes") == nodes);
            const auto reopened = service->getRenderAuthoringContext(Json::object());
            const auto &reopened_passes = reopened.at("graphs").at(0).at("passes");
            const auto saved = std::find_if(reopened_passes.begin(), reopened_passes.end(), [](const auto &pass) {
                return pass.at("name") == "forward_transparent";
            });
            REQUIRE(saved != reopened_passes.end());
            CHECK(Json(saved->at("preset_declaration")) == *material);
        }
    }
    SECTION("material edits reject empty requests, invalid query and shader replacement") {
        Json material_request{{"base_source_digest", context.at("source_digest").at("hex")},
                              {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                              {"graph", "main_render"}, {"pass_name", "deferred_geometry"}};
        CHECK(service->editPresetPass(material_request).at("status") == "rejected");
        material_request["ray_query"] = "true";
        CHECK_THROWS(service->editPresetPass(material_request));
        material_request["ray_query"] = true;
        material_request["shader"] = request.at("shader");
        CHECK(service->editPresetPass(material_request).at("status") == "rejected");
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("invalid material input and duplicate attachments fail before preset publication") {
        const auto patches = Json::array({
            Json{{"input", Json::array({"missing_target"})}},
            // Frame graph lowering permits implicit output resources, but its
            // shared pass-shape policy forbids duplicate color attachments.
            Json{{"output", {{"color", Json::array({"lit_color", "lit_color"})}, {"depth", "scene_depth"}}}}});
        for (const auto &patch : patches) {
            Json material_request{
                {"base_source_digest", context.at("source_digest").at("hex")},
                {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "forward_transparent"}};
            material_request.update(patch);
            const auto accepted = service->editPresetPass(material_request);
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            CHECK(result.at("committed") == false);
            CHECK(result.value("source_commit_called", false) == false);
            CHECK(readBytes(preset_path) == preset_before);
            CHECK(readBytes(project.render_config) == root_before);
            CHECK(runtime.frame_plan == plan_before);
        }
    }
    SECTION("material attachment publication failure restores preset and runtime") {
        runtime.fail_after_source_commit = true;
        const Json material_request{
            {"base_source_digest", context.at("source_digest").at("hex")},
            {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
            {"graph", "main_render"}, {"pass_name", "forward_transparent"},
            {"output", {{"color", Json::array({"gbuffer_normal"})}, {"depth", "scene_depth"}}}};
        const auto accepted = service->editPresetPass(material_request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        // Rejections omit source_commit_called. This injected failure can
        // only be returned after the runtime's source_commit() completed.
        CHECK(result.at("error").at("message") == "injected failure after source replacement");
        CHECK(result.at("committed") == false);
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("an invalid connection fails before source replacement") {
        request["input"] = Json::array({"missing_target"});
        const auto accepted = service->editPresetPass(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        REQUIRE(result.at("committed") == false);
        REQUIRE(readBytes(preset_path) == preset_before);
        REQUIRE(runtime.frame_plan == plan_before);
    }
    SECTION("publication failure restores preset and runtime") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->editPresetPass(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(preset_path) == preset_before);
        REQUIRE(readBytes(project.render_config) == root_before);
        REQUIRE(runtime.frame_plan == plan_before);
    }
    SECTION("a queued edit cannot overwrite an external preset change") {
        const auto accepted = service->editPresetPass(request);
        REQUIRE(accepted.at("status") == "accepted");
        writeBytes(preset_path, preset_before + "\n");
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        REQUIRE(readBytes(preset_path) == preset_before + "\n");
        REQUIRE(runtime.frame_plan == plan_before);
    }
}

TEST_CASE("Feature parameter overrides publish through the shared binder and preserve feature sources",
          "[render-config-editor][feature-parameters]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto feature = Json::parse(R"json({
      "schema":"pelican.render_feature","version":1,"name":"editable_parameters",
      "runtime_shader_compiler":"optional",
      "parameters":{"schema":"pelican.render_feature_parameters","version":1,
                    "scalars":[{"name":"strength","type":"float","range":[0,1],"default":0.25}]},
      "passes":[{"insert":"before:scene_present","pass":{
        "name":"parameter_probe","type":"fullscreen","input":["gbuffer_normal"],
        "output":{"color":["lit_color"],"depth":null},"clear_color":["$strength",0,0,1],
        "shader":{"vertex":"engine://fullscreen","fragment":"engine://fullscreen"}}}]
    })json");
    const auto feature_path = project.root / "passes" / "parameters.json";
    const auto feature_before = feature.dump(2);
    writeBytes(feature_path, feature_before);
    auto root = Json::parse(readBytes(project.render_config));
    const auto index = renderingSectionOf(root).at("features").size();
    const std::string reference = "project://passes/parameters.json";
    renderingSectionOf(root)["features"].push_back(reference);
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context = service->getRenderFeatures(Json::object());
    const auto &instance = context.at("feature_instances").at(index);
    CHECK(instance.at("reference") == reference);
    CHECK(instance.at("parameters") == Json::object());
    CHECK(instance.at("parameter_schema") == feature.at("parameters"));
    REQUIRE(instance.at("editable") == true);
    CHECK(instance.at("removable") == true);
    Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                 {"operations", Json::array({{{"op", "set_parameters"}, {"index", index},
                                               {"parameters", {{"strength", 0.75}}}}})}};
    SECTION("overrides and reset use authored root entries and survive reopen") {
        for (const auto &parameters : Json::array({Json{{"strength", 0.75}}, Json::object()})) {
            const auto current = service->getRenderFeatures(Json::object());
            request["base_source_digest"] = current.at("source_digest").at("hex");
            request["operations"][0]["parameters"] = parameters;
            const auto accepted = service->editRenderFeatures(request);
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            renderingSectionOf(root)["features"][index] = {{"ref", reference}, {"parameters", parameters}};
            CHECK(Json::parse(readBytes(project.render_config)) == root);
            CHECK(readBytes(feature_path) == feature_before);
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            const auto reopened = service->getRenderFeatures(Json::object());
            CHECK(reopened.at("feature_instances").at(index).at("parameters") == parameters);
            CHECK(reopened.at("feature_instances").at(index).at("removable") == false);
            const auto authored = service->getRenderAuthoringContext(Json::object());
            const auto &passes = authored.at("graphs").at(0).at("passes");
            const auto pass = std::find_if(passes.begin(), passes.end(), [](const auto &value) { return value.at("name") == "parameter_probe"; });
            REQUIRE(pass != passes.end());
            CHECK(pass->at("declaration").at("clear_color")[0] == parameters.value("strength", 0.25));
            CHECK(service->editRenderFeatures(request).at("error").at("code") == "external_modification");
        }
    }
    SECTION("preset-owned parameter overrides and reset preserve the root") {
        auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
        preset["config"]["features"] = Json::array({reference});
        const auto preset_path = project.root / "passes" / "parameter_preset.json";
        writeBytes(preset_path, preset.dump(2));
        renderingSectionOf(root)["features"].erase(index);
        renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/parameter_preset.json";
        writeBytes(project.render_config, root.dump(2));
        const auto preset_root_before = readBytes(project.render_config);
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        for (const auto &parameters : Json::array({Json{{"strength", 0.75}}, Json::object()})) {
            const auto current = service->getRenderFeatures(Json::object());
            const auto &instances = current.at("feature_instances");
            const auto selected = std::find_if(instances.begin(), instances.end(), [&](const auto &value) {
                return value.at("scope") == "preset" && value.at("reference") == reference;
            });
            REQUIRE(selected != instances.end());
            REQUIRE(selected->at("editable") == true);
            CHECK(selected->at("removable") == false);
            request["base_source_digest"] = current.at("source_digest").at("hex");
            request["operations"][0] = {{"op", "set_parameters"}, {"scope", "preset"},
                {"index", selected->at("index")}, {"parameters", parameters},
                {"base_preset_digest", selected->at("source_digest").at("hex")}};
            const auto accepted = service->editRenderFeatures(request);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            preset["config"]["features"][0] = {{"ref", reference}, {"parameters", parameters}};
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == preset_root_before);
            CHECK(readBytes(feature_path) == feature_before);
            CHECK(service->editRenderFeatures(request).at("error").at("code") == "external_modification");
            service.reset();
            service = makeProjectService(project, resolver, runtime);
        }
    }
    SECTION("engine preset parameter mutation requires a project fork") {
        const auto authored = service->getRenderAuthoringContext(Json::object());
        request["operations"][0]["scope"] = "preset";
        request["operations"][0]["base_preset_digest"] = authored.at("pipeline_preset").at("source_digest").at("hex");
        CHECK(service->editRenderFeatures(request).at("error").at("code") == "project_preset_required");
        CHECK(readBytes(project.render_config) == root_before);
    }
    SECTION("invalid parameter values are rejected by the binder before source replacement") {
        request["operations"][0]["parameters"] = {{"strength", 2.0}};
        const auto accepted = service->editRenderFeatures(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(readBytes(feature_path) == feature_before);
    }
    SECTION("parameter publication failure restores the root") {
        runtime.fail_after_source_commit = true;
        const auto accepted = service->editRenderFeatures(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        CHECK(result.at("error").at("message") == "injected failure after source replacement");
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(readBytes(feature_path) == feature_before);
    }
}

TEST_CASE("Project feature pass edits publish their source and retain the rendering root",
          "[render-config-editor][feature-pass-edit]") {
    for (const auto *type : {"fullscreen", "raster", "material", "debug_draw", "debug_text", "gizmo"}) {
        CAPTURE(type);
        const bool auxiliary = std::string_view{type} == "debug_draw" ||
            std::string_view{type} == "debug_text" || std::string_view{type} == "gizmo";
        TemporaryAnimgraphProject project;
        PathResolver resolver;
        resolver.setup(project.root, false);
        Json pass{{"name", "feature_probe"}, {"type", type},
                  {"input", Json::array({"gbuffer_normal"})},
                  {"output", {{"color", Json::array({"lit_color"})}, {"depth", nullptr}}}};
        if (auxiliary) pass.erase("input");
        if (std::string_view{type} == "material") {
            pass["material_contract"] = "forward_opaque_v1";
            pass["output"]["depth"] = "scene_depth";
        } else {
            pass["shader"] = {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}};
            if (std::string_view{type} == "raster") pass["draw"] = {{"vertex_count", 3}};
        }
        Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "editable_probe"},
                     {"runtime_shader_compiler", "optional"},
                     {"passes", Json::array({{{"insert", "before:scene_present"}, {"pass", pass}}})}};
        const auto feature_path = project.root / "passes" / "editable_feature.json";
        writeBytes(feature_path, feature.dump(2));
        auto root = Json::parse(readBytes(project.render_config));
        renderingSectionOf(root)["features"].push_back("project://passes/editable_feature.json");
        writeBytes(project.render_config, root.dump(2));
        const auto root_before = readBytes(project.render_config);
        ActualCpuRenderRuntime runtime;
        auto service = makeProjectService(project, resolver, runtime);
        const auto context = service->getRenderAuthoringContext(Json::object());
        const auto &passes = context.at("graphs").at(0).at("passes");
        const auto editable = std::find_if(passes.begin(), passes.end(), [](const auto &value) { return value.at("name") == "feature_probe"; });
        REQUIRE(editable != passes.end());
        REQUIRE(editable->at("editable_feature_pass") == true);
        CHECK(editable->at("feature_declaration") == pass);
        CHECK(editable->at("feature_source_reference") == "project://passes/editable_feature.json");
        Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                     {"base_feature_digest", editable->at("feature_source_digest").at("hex")},
                     {"graph", "main_render"}, {"pass_name", "feature_probe"},
                     {"input", Json::array({"gbuffer_albedo"})}, {"output", pass.at("output")}};
        if (std::string_view{type} == "fullscreen") request["shader"] = pass.at("shader");
        if (auxiliary) {
            request.erase("input");
            request["ray_query"] = true;
        }
        auto invalid = request;
        invalid["material_routing"] = nullptr;
        CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        invalid = request;
        invalid["pass_name"] = "__anchor_tonemap";
        CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        const auto before = readBytes(feature_path);
        const auto accepted = service->editPresetPass(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        CHECK(readBytes(feature_path) == before);
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        if (auxiliary) feature["passes"][0]["pass"]["ray_query"] = true;
        else feature["passes"][0]["pass"]["input"] = request.at("input");
        CHECK(Json::parse(readBytes(feature_path)) == feature);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(service->editPresetPass(request).at("error").at("code") == "external_modification");
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        const auto reopened = service->getRenderAuthoringContext(Json::object());
        const auto &reopened_passes = reopened.at("graphs").at(0).at("passes");
        const auto saved = std::find_if(reopened_passes.begin(), reopened_passes.end(), [](const auto &value) { return value.at("name") == "feature_probe"; });
        REQUIRE(saved != reopened_passes.end());
    CHECK(nlohmann::json(saved->at("feature_declaration")) == nlohmann::json(feature.at("passes")[0].at("pass")));
        request["base_feature_digest"] = saved->at("feature_source_digest").at("hex");
        if (auxiliary) request["ray_query"] = false;
        else request["input"] = Json::array({"gbuffer_normal"});
        const auto queued = service->editPresetPass(request);
        REQUIRE(queued.at("status") == "accepted");
        const auto external = readBytes(feature_path) + "\n";
        writeBytes(feature_path, external);
        service->commitPending();
        CHECK(service->getResult({{"ticket", queued.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(feature_path) == external);
        CHECK(readBytes(project.render_config) == root_before);
    }
}

TEST_CASE("Project auxiliary preset edits preserve declarations and reopen",
          "[render-config-editor][preset-pass-edit][auxiliary-pass-edit]") {
    for (const auto *type : {"debug_draw", "debug_text", "gizmo", "shadow_depth", "velocity", "picking"}) {
        CAPTURE(type);
        TemporaryAnimgraphProject project;
        PathResolver resolver;
        resolver.setup(project.root, false);
        auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
        const bool shadow = std::string_view{type} == "shadow_depth";
        const bool geometry = shadow || std::string_view{type} == "velocity" || std::string_view{type} == "picking";
        preset["config"]["render_targets"].push_back({
            {"name", "aux_color"}, {"extent_scale", 1.0},
            {"format", std::string_view{type} == "picking" ? "R32_UINT" : "R16G16B16A16_SFLOAT"},
            {"format_class", "data"}, {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}});
        Json declaration{{"name", "aux_probe"}, {"type", type},
            {"output", {{"color", shadow ? Json(nullptr) : Json::array({"aux_color"})},
                        {"depth", geometry ? Json("scene_depth") : Json(nullptr)}}}};
        if (std::string_view{type} != "debug_draw") {
            declaration["shader"] = {{"vertex", "engine://" + std::string{type}}};
            if (!shadow) declaration["shader"]["fragment"] = "engine://" + std::string{type};
        }
        if (geometry && !shadow) declaration["shader"]["skinned_vertex"] = "engine://" + std::string{type} + "_skinned";
        preset["config"]["rendering_passes"][0]["passes"].push_back(declaration);
        const auto path = project.root / "passes" / "auxiliary.json";
        writeBytes(path, preset.dump(2));
        auto root = Json::parse(readBytes(project.render_config));
        renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/auxiliary.json";
        writeBytes(project.render_config, root.dump(2));
        const auto root_before = readBytes(project.render_config);
        ActualCpuRenderRuntime runtime;
        auto service = makeProjectService(project, resolver, runtime);
        auto request = [&] {
            const auto context = service->getRenderAuthoringContext(Json::object());
            return Json{{"base_source_digest", context.at("source_digest").at("hex")},
                {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                {"graph", "main_render"}, {"pass_name", "aux_probe"}};
        };
        auto patch = request();
        patch["output"] = declaration.at("output");
        patch["ray_query"] = true;
        if (declaration.contains("shader")) patch["shader"] = {{"vertex", declaration.at("shader").at("vertex")}};
        else patch["shader"] = {{"fragment", "engine://debug_draw"}};
        const auto accepted = service->editPresetPass(patch);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        declaration["ray_query"] = true;
        if (!declaration.contains("shader")) declaration["shader"] = {
            {"vertex", "engine://debug_draw"}, {"fragment", "engine://debug_draw"}};
        preset["config"]["rendering_passes"][0]["passes"].back() = declaration;
        CHECK(Json::parse(readBytes(path)) == preset);
        CHECK(readBytes(project.render_config) == root_before);
        service.reset();
        service = makeProjectService(project, resolver, runtime);
        const auto context = service->getRenderAuthoringContext(Json::object());
        const auto &passes = context.at("graphs")[0].at("passes");
        const auto saved = std::find_if(passes.begin(), passes.end(), [](const auto &pass) { return pass.at("name") == "aux_probe"; });
        REQUIRE(saved != passes.end());
        CHECK(saved->at("editable_preset_pass") == true);
    CHECK(nlohmann::json(saved->at("preset_declaration")) == nlohmann::json(declaration));
        const auto before = readBytes(path);
        auto invalid = request();
        invalid["input"] = Json::array({"gbuffer_normal"});
        CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        invalid = request();
        invalid["shader"] = {{shadow ? "fragment" : "unsupported", "engine://fullscreen"}};
        CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        CHECK(readBytes(path) == before);
        if (geometry && !shadow) {
            auto reset = request();
            reset["shader"] = {{"skinned_vertex", nullptr}};
            const auto ticket = service->editPresetPass(reset);
            REQUIRE(ticket.at("status") == "accepted");
            service->commitPending();
            REQUIRE(service->getResult({{"ticket", ticket.at("ticket")}}).at("committed") == true);
            declaration["shader"].erase("skinned_vertex");
            preset["config"]["rendering_passes"][0]["passes"].back() = declaration;
            CHECK(Json::parse(readBytes(path)) == preset);
        }
    }
}

TEST_CASE("Project raster preset edits preserve omitted fields and reopen",
          "[render-config-editor][preset-pass-edit][raster-connections]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
    for (const auto *name : {"raster_color", "raster_replacement"}) {
        preset["config"]["render_targets"].push_back({
            {"name", name}, {"extent_scale", 1.0}, {"format", "R16G16B16A16_SFLOAT"},
            {"format_class", "data"}, {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}});
    }
    auto &passes = preset["config"]["rendering_passes"][0]["passes"];
    passes.push_back({
        {"name", "raster_probe"}, {"type", "raster"},
        {"input", Json::array({"gbuffer_normal"})},
        {"output", {{"color", Json::array({"raster_color"})}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}}},
        {"draw", {{"operation", "direct"}, {"vertex_count", 3}, {"instance_count", 2}, {"first_instance", 4}}},
        {"raster_state", {{"cull", "none"}, {"depth_test", false}, {"depth_write", false}}},
        {"clear_color", Json::array({0.2, 0.3, 0.4, 1.0})}});
    auto &raster = passes.back();
    const auto preset_path = project.root / "passes" / "raster.json";
    writeBytes(preset_path, preset.dump(2));
    auto root = Json::parse(readBytes(project.render_config));
    renderingSectionOf(root)["pipeline"]["preset"] = "project://passes/raster.json";
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    const auto preset_before = readBytes(preset_path);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context = service->getRenderAuthoringContext(Json::object());
    const auto &context_passes = context.at("graphs").at(0).at("passes");
    const auto editable = std::find_if(context_passes.begin(), context_passes.end(), [](const auto &pass) {
        return pass.at("name") == "raster_probe";
    });
    REQUIRE(editable != context_passes.end());
    REQUIRE(editable->at("editable_preset_pass") == true);
    REQUIRE(editable->at("preset_declaration") == raster);
    const auto plan_before = runtime.frame_plan;
    Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                 {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
                 {"graph", "main_render"}, {"pass_name", "raster_probe"}};
    SECTION("partial raster edits preserve other fields and survive reopen") {
        const auto patches = Json::array({
            Json{{"input", Json::array({"gbuffer_albedo"})}},
            Json{{"output", {{"color", Json::array({"raster_replacement"})}, {"depth", nullptr}}}},
            Json{{"ray_query", true}},
            Json{{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://scene_present"}}}},
            Json{{"draw", {{"operation", "direct"}, {"vertex_count", 6}, {"instance_count", 3}, {"first_instance", 4}}}},
            Json{{"raster_state", {{"topology", "triangle_strip"}, {"cull", "front"},
                                   {"depth_test", false}, {"depth_write", false}}}},
            Json{{"raster_state", nullptr}}});
        for (const auto &patch : patches) {
            const auto current = service->getRenderAuthoringContext(Json::object());
            request["base_preset_digest"] = current.at("pipeline_preset").at("source_digest").at("hex");
            auto partial = request;
            partial.update(patch);
            const auto accepted = service->editPresetPass(partial);
            INFO(accepted.dump());
            REQUIRE(accepted.at("status") == "accepted");
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            service->commitPending();
            const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
            INFO(result.dump());
            REQUIRE(result.at("committed") == true);
            raster.update(patch);
            if (raster.contains("raster_state") && raster.at("raster_state").is_null())
                raster.erase("raster_state");
            CHECK(Json::parse(readBytes(preset_path)) == preset);
            CHECK(readBytes(project.render_config) == root_before);
            const auto nodes = runtime.frame_plan.at("nodes");
            const auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &value) {
                return value.at("name") == "raster_probe";
            });
            REQUIRE(node != nodes.end());
            CHECK(node->at("reads") == raster.at("input"));
            CHECK(node->at("writes") == raster.at("output").at("color"));
            service.reset();
            service = makeProjectService(project, resolver, runtime);
            CHECK(runtime.frame_plan.at("nodes") == nodes);
            const auto reopened = service->getRenderAuthoringContext(Json::object());
            const auto &reopened_passes = reopened.at("graphs").at(0).at("passes");
            const auto saved = std::find_if(reopened_passes.begin(), reopened_passes.end(), [](const auto &pass) {
                return pass.at("name") == "raster_probe";
            });
            REQUIRE(saved != reopened_passes.end());
            CHECK(Json(saved->at("preset_declaration")) == raster);
        }
    }
    SECTION("raster passes cannot change material settings or routing") {
        for (const auto *key : {"material_settings", "material_routing"}) {
            auto invalid = request;
            invalid["ray_query"] = false;
            invalid[key] = Json::object();
            CHECK(service->editPresetPass(invalid).at("status") == "rejected");
        }
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("invalid raster draw is rejected before publication") {
        request["draw"] = {{"operation", "direct"}, {"vertex_count", 0}};
        const auto result = service->editPresetPass(request);
        INFO(result.dump());
        CHECK(result.at("status") == "rejected");
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(runtime.frame_plan == plan_before);
    }
    SECTION("duplicate raster attachments cannot replace the source") {
        request["output"] = {{"color", Json::array({"raster_color", "raster_color"})}, {"depth", nullptr}};
        const auto accepted = service->editPresetPass(request);
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        CHECK(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == false);
        CHECK(readBytes(preset_path) == preset_before);
        CHECK(readBytes(project.render_config) == root_before);
        CHECK(runtime.frame_plan == plan_before);
    }
}

TEST_CASE(
    "WP334b animgraph preset authored pass appears only after save and purges without an orphan",
    "[render-config-editor][wp334b][integration][preset][overlay][purge]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    const std::string pass_name = "wp334b_authored_probe";
    const auto has_node = [&](std::string_view name) {
        return std::ranges::any_of(
            runtime.frame_plan.at("nodes"), [&](const auto &node) {
                return node.value("name", std::string{}) == name;
            });
    };
    const auto target_count = [&] {
        return runtime.frame_plan.at("resources").size();
    };
    const auto root_references = [&] {
        std::set<std::string, std::less<>> references;
        const auto root =
            Json::parse(readBytes(project.render_config));
        for (const auto &entry : renderingSectionOf(root).at("features")) {
            if (entry.is_string()) {
                references.insert(entry.get<std::string>());
            } else if (entry.is_object() && entry.contains("ref") &&
                       entry.at("ref").is_string()) {
                references.insert(entry.at("ref").get<std::string>());
            }
        }
        return references;
    };

    const auto before_context =
        service->getRenderAuthoringContext(Json::object());
    REQUIRE(before_context.at("config_kind") == "preset");
    REQUIRE(before_context.at("pipeline_preset").at("name") ==
            "hybrid_v1");
    REQUIRE_FALSE(has_node(pass_name));
    const auto targets_before = target_count();
    const auto root_before = readBytes(project.render_config);

    const Json pass{
        {"name", pass_name},
        {"type", "fullscreen"},
        {"input", Json::array({"gbuffer_normal"})},
        {"output",
         {{"color", Json::array({"ssao_output"})},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fullscreen"}}},
    };
    const auto accepted = service->addAuthoredPass(
        {{"base_source_digest",
          before_context.at("source_digest").at("hex")},
         {"graph", "main_render"},
         {"insert", "after:ssao_pass"},
         {"pass", pass}});
    REQUIRE(accepted.at("status") == "accepted");
    const auto ticket = accepted.at("ticket").get<std::string>();
    const auto fragment_reference =
        accepted.at("fragment_reference").get<std::string>();
    const auto resolved_fragment = resolver.resolveProjectRef(
        fragment_reference);
    const auto *fragment_path =
        std::get_if<std::filesystem::path>(&resolved_fragment);
    REQUIRE(fragment_path != nullptr);

    // The decisive request-local overlay contrast: the candidate compiled,
    // although the fallback resolver still cannot see its staged fragment.
    REQUIRE_THROWS(resolver.loadText(fragment_reference));
    REQUIRE_FALSE(std::filesystem::exists(*fragment_path));
    REQUIRE(readBytes(project.render_config) == root_before);
    REQUIRE_FALSE(root_references().contains(fragment_reference));
    REQUIRE_FALSE(has_node(pass_name));

    service->commitPending();
    const auto added = service->getResult({{"ticket", ticket}});
    REQUIRE(added.at("committed") == true);
    REQUIRE(std::filesystem::is_regular_file(*fragment_path));
    REQUIRE(root_references().contains(fragment_reference));
    REQUIRE(has_node(pass_name));
    REQUIRE(target_count() == targets_before);

    const auto after_context =
        service->getRenderAuthoringContext(Json::object());
    REQUIRE(after_context.at("config_kind") == "preset");
    const auto graph = std::ranges::find(
        after_context.at("graphs"), std::string{"main_render"},
        [](const auto &value) {
            return value.at("name").template get<std::string>();
        });
    REQUIRE(graph != after_context.at("graphs").end());
    const auto authored = std::ranges::find(
        graph->at("passes"), pass_name, [](const auto &value) {
            return value.at("name").template get<std::string>();
        });
    REQUIRE(authored != graph->at("passes").end());
    REQUIRE(authored->at("provenance").at("source") == "feature");
    REQUIRE(authored->at("provenance").at("provider_reference") ==
            fragment_reference);
    REQUIRE(authored->at("managed") == true);

    // An externally edited managed fragment is never overwritten or purged,
    // and the root remains byte-identical.
    const auto managed_before_external = readBytes(*fragment_path);
    const auto root_before_external = readBytes(project.render_config);
    const auto external_fragment = managed_before_external + " \n";
    writeBytes(*fragment_path, external_fragment);
    const auto rejected_remove = service->removeAuthoredPass(
        {{"base_source_digest",
          after_context.at("source_digest").at("hex")},
         {"fragment_reference", fragment_reference}});
    REQUIRE(rejected_remove.at("status") == "rejected");
    REQUIRE(rejected_remove.at("error").at("code") ==
            "external_modification");
    REQUIRE(readBytes(project.render_config) == root_before_external);
    REQUIRE(readBytes(*fragment_path) == external_fragment);
    writeBytes(*fragment_path, managed_before_external);

    const auto remove_accepted = service->removeAuthoredPass(
        {{"base_source_digest",
          after_context.at("source_digest").at("hex")},
         {"fragment_reference", fragment_reference}});
    REQUIRE(remove_accepted.at("status") == "accepted");
    const auto remove_ticket =
        remove_accepted.at("ticket").get<std::string>();
    service->commitPending();
    const auto removed =
        service->getResult({{"ticket", remove_ticket}});
    INFO(removed.dump());
    REQUIRE(removed.at("committed") == true);
    REQUIRE_FALSE(std::filesystem::exists(*fragment_path));
    REQUIRE_FALSE(root_references().contains(fragment_reference));
    REQUIRE_FALSE(has_node(pass_name));
    REQUIRE(target_count() == targets_before);

    // Semantic orphan scan: every managed marker below the owned namespace
    // must be reachable from a root features[] reference.
    const auto live_references = root_references();
    const auto managed_directory = project.root / "passes" / "authoring";
    if (std::filesystem::exists(managed_directory)) {
        for (const auto &entry :
             std::filesystem::directory_iterator(managed_directory)) {
            if (!entry.is_regular_file()) continue;
            const auto document = Json::parse(readBytes(entry.path()));
            const auto marker = document.find("pelican_editor_managed");
            if (marker == document.end()) continue;
            const auto relative =
                entry.path().lexically_relative(project.root).generic_string();
            REQUIRE(live_references.contains("project://" + relative));
        }
    }
}

TEST_CASE(
    "WP334b remove_authored_pass names both digests when the root moved on disk",
    "[render-config-editor][wp334b][authored-pass][external-modification]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    const Json pass{
        {"name", "wp334b_root_cas_probe"},
        {"type", "fullscreen"},
        {"input", Json::array({"gbuffer_normal"})},
        {"output",
         {{"color", Json::array({"ssao_output"})},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fullscreen"}}},
    };
    const auto accepted = service->addAuthoredPass(
        {{"base_source_digest",
          service->documentForTesting().sourceDigest()},
         {"graph", "main_render"},
         {"insert", "after:ssao_pass"},
         {"pass", pass}});
    REQUIRE(accepted.at("status") == "accepted");
    const auto fragment_reference =
        accepted.at("fragment_reference").get<std::string>();
    const auto add_ticket = accepted.at("ticket").get<std::string>();
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", add_ticket}}).at("committed") ==
            true);

    // The root moves under the editor -- not the managed fragment, which the
    // case above covers. The supplied base digest still matches the
    // engine-owned document, so the only check that can refuse is the
    // re-read-from-disk CAS, and it must name both digests: Studio shows the
    // drift from them, and the add and root-edit paths have always sent them.
    const auto engine_owned_digest =
        service->documentForTesting().sourceDigest();
    const auto root_before_external = readBytes(project.render_config);
    const auto external_root = root_before_external + " \r\n";
    writeBytes(project.render_config, external_root);

    const auto rejected = service->removeAuthoredPass(
        {{"base_source_digest", engine_owned_digest},
         {"fragment_reference", fragment_reference}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("committed") == false);
    REQUIRE_FALSE(rejected.contains("ticket"));
    const auto &error = rejected.at("error");
    REQUIRE(error.at("code") == "external_modification");
    REQUIRE(error.at("message") ==
            "render config source changed outside the editor");
    REQUIRE(error.contains("expected_source_digest"));
    REQUIRE(error.contains("actual_source_digest"));
    REQUIRE(error.at("expected_source_digest") == engine_owned_digest);
    REQUIRE(error.at("actual_source_digest") !=
            error.at("expected_source_digest"));
    // A refused precondition writes nothing: the external bytes stand.
    REQUIRE(readBytes(project.render_config) == external_root);
}

TEST_CASE(
    "WP334b multi-document transaction rolls back both failure directions and commits both directions",
    "[render-config-editor][wp334b][transaction][atomicity]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto root_reference = std::string{"passes/main.json"};
    const auto root_key = resolver.normalizedReference(root_reference);
    const auto fragment_reference =
        std::string{"project://passes/authoring/pass-atomic.json"};
    const auto fragment_key =
        resolver.normalizedReference(fragment_reference);
    const auto resolved = resolver.resolveProjectRef(fragment_reference);
    const auto fragment_path =
        std::get<std::filesystem::path>(resolved);
    const auto original_root = readBytes(project.render_config);
    const auto added_root =
        AuthoredRenderConfigDocument::parse(original_root)
            .withFeatureAdded(fragment_reference)
            .bytes();
    const std::string fragment_bytes =
        R"json({"schema":"pelican.render_feature","version":1,"name":"atomic","passes":[]})json";

    const auto addition = [&] {
        return RenderConfigCandidateDocumentSet{
            root_key,
            {{.reference = fragment_reference,
              .normalized_reference = fragment_key,
              .path = fragment_path,
              .operation = RenderConfigDocumentOperation::create,
              .expected =
                  {.existence = RenderConfigDocumentExistence::missing},
              .bytes = fragment_bytes},
             {.reference = root_reference,
              .normalized_reference = root_key,
              .path = project.render_config,
              .operation = RenderConfigDocumentOperation::replace,
              .expected =
                  {.existence = RenderConfigDocumentExistence::present,
                   .digest = renderConfigSourceDigest(original_root)},
              .bytes = added_root}}};
    };

    const auto external_root = original_root + " \n";
    REQUIRE_THROWS(commitRenderConfigCandidateDocuments(
        project.root, addition(),
        [&](std::size_t index,
            const RenderConfigCandidateDocument &) {
            if (index == 0) {
                writeBytes(project.render_config, external_root);
            }
        }));
    REQUIRE_FALSE(std::filesystem::exists(fragment_path));
    REQUIRE(readBytes(project.render_config) == external_root);
    writeBytes(project.render_config, original_root);
    recoverRenderConfigDocumentTransaction(project.root);
    REQUIRE_FALSE(std::filesystem::exists(
        renderConfigTransactionDirectory(project.root)));

    const auto add_receipt =
        commitRenderConfigCandidateDocuments(project.root, addition());
    REQUIRE(add_receipt.changed());
    REQUIRE(readBytes(project.render_config) == added_root);
    REQUIRE(readBytes(fragment_path) == fragment_bytes);

    const auto removed_root =
        AuthoredRenderConfigDocument::parse(added_root)
            .withFeatureRemoved(fragment_reference)
            .bytes();
    const auto removal = RenderConfigCandidateDocumentSet{
        root_key,
        {{.reference = root_reference,
          .normalized_reference = root_key,
          .path = project.render_config,
          .operation = RenderConfigDocumentOperation::replace,
          .expected =
              {.existence = RenderConfigDocumentExistence::present,
               .digest = renderConfigSourceDigest(added_root)},
          .bytes = removed_root},
         {.reference = fragment_reference,
          .normalized_reference = fragment_key,
          .path = fragment_path,
          .operation = RenderConfigDocumentOperation::erase,
          .expected =
              {.existence = RenderConfigDocumentExistence::present,
               .digest = renderConfigSourceDigest(fragment_bytes)}}}};
    REQUIRE_THROWS(commitRenderConfigCandidateDocuments(
        project.root, removal,
        [](std::size_t index,
           const RenderConfigCandidateDocument &) {
            if (index == 0) {
                throw std::runtime_error(
                    "injected fragment commit failure");
            }
        }));
    REQUIRE(readBytes(project.render_config) == added_root);
    REQUIRE(readBytes(fragment_path) == fragment_bytes);

    const auto remove_receipt =
        commitRenderConfigCandidateDocuments(project.root, removal);
    REQUIRE(remove_receipt.changed());
    REQUIRE(readBytes(project.render_config) == removed_root);
    REQUIRE_FALSE(std::filesystem::exists(fragment_path));
}

TEST_CASE(
    "WP334b engine authoring context exposes a direct root through the preset-capable contract",
    "[render-config-editor][wp334b][context][direct]") {
    TemporaryAnimgraphProject project;
    writeBytes(
        project.render_config,
        readBytes(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                  "projects" / "example" / "passes" /
                  "main_rendering_config.json"));
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);

    const auto context =
        service->getRenderAuthoringContext(Json::object());
    REQUIRE(context.at("config_kind") == "direct");
    REQUIRE_FALSE(context.contains("pipeline_preset"));
    REQUIRE(context.at("source_reference") == "passes/main.json");
    REQUIRE(context.at("published_generation") ==
            runtime.snapshot.published_generation);
    const auto graph = std::ranges::find(
        context.at("graphs"), std::string{"main_render"},
        [](const auto &value) {
            return value.at("name").template get<std::string>();
        });
    REQUIRE(graph != context.at("graphs").end());
    REQUIRE_FALSE(graph->at("passes").empty());
    REQUIRE_FALSE(graph->at("anchor_candidates").empty());
}

#if PELICAN_WITH_RPC
TEST_CASE(
    "WP334b engine RPC publishes context and both authored-pass operations",
    "[render-config-editor][wp334b][rpc][context][add][remove]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto unique_service = makeProjectService(project, resolver, runtime);
    auto render_service =
        std::shared_ptr<RenderConfigEditorService>{
            std::move(unique_service)};
    const auto scene_document = AuthoringSceneDocument::load(
        readBytes(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                  "test" / "fixtures" / "authoring_scene" /
                  "multi_scene_roundtrip.json"),
        SceneRevision{1});
    EditorCommandService commands{
        EditorCommandServiceDependencies{
            .document = [&scene_document]()
                -> const AuthoringSceneDocument & {
                return scene_document;
            },
            .current_scene_id = [] { return std::string{"main"}; },
            .render_config_editor = render_service,
        }};
    EditorCommandRpcAdapter adapter{commands};
    std::istringstream input;
    std::ostringstream output;
    RpcServer rpc{input, output};
    configureEditorRpcHandlers(
        rpc, adapter,
        EditorRpcHandlerHooks{
            .snapshot_imported = [] {},
            .save_busy = [] { return false; },
        });
    const auto call = [&](int id, std::string_view method, Json params) {
        return Json::parse(rpc.processLine(
            Json{{"jsonrpc", "2.0"},
                 {"id", id},
                 {"method", method},
                 {"params", std::move(params)}}
                .dump()));
    };

    const auto context =
        call(1, "get_render_authoring_context", Json::object())
            .at("result");
    REQUIRE(context.at("config_kind") == "preset");
    REQUIRE(context.at("graphs").at(0)
                .at("anchor_candidates")
                .is_array());
    const Json pass{
        {"name", "wp334b_rpc_probe"},
        {"type", "fullscreen"},
        {"input", Json::array({"gbuffer_normal"})},
        {"output",
         {{"color", Json::array({"ssao_output"})},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fullscreen"}}},
    };
    const auto add =
        call(2, "add_authored_pass",
             {{"base_source_digest",
               context.at("source_digest").at("hex")},
              {"graph", "main_render"},
              {"insert", "after:ssao_pass"},
              {"pass", pass}})
            .at("result");
    REQUIRE(add.at("status") == "accepted");
    commands.commitPendingEdits();
    const auto add_result =
        call(3, "get_edit_result", {{"ticket", add.at("ticket")}})
            .at("result");
    REQUIRE(add_result.at("committed") == true);

    const auto updated =
        call(4, "get_render_authoring_context", Json::object())
            .at("result");
    REQUIRE(updated.at("managed_fragments").size() == 1);
    const auto reference = updated.at("managed_fragments")
                               .at(0)
                               .at("reference");
    const auto remove =
        call(5, "remove_authored_pass",
             {{"base_source_digest",
               updated.at("source_digest").at("hex")},
              {"fragment_reference", reference}})
            .at("result");
    REQUIRE(remove.at("status") == "accepted");
    commands.commitPendingEdits();
    const auto remove_result =
        call(6, "get_edit_result",
             {{"ticket", remove.at("ticket")}})
            .at("result");
    REQUIRE(remove_result.at("committed") == true);
}
#endif

TEST_CASE(
    "WP334b removing a hand-written feature reference never deletes its file",
    "[render-config-editor][wp334b][manual-fragment][ownership]") {
    TemporaryAnimgraphProject project;
    const auto manual_path = project.root / "passes" / "manual.json";
    const std::string manual_reference =
        "project://passes/manual.json";
    const std::string manual_pass_name = "wp334b_manual_probe";
    const Json manual_feature{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "manual_probe"},
        {"passes",
         Json::array(
             {{{"insert", "after:ssao_pass"},
               {"pass",
                {{"name", manual_pass_name},
                 {"type", "fullscreen"},
                 {"input", Json::array({"gbuffer_normal"})},
                 {"output",
                  {{"color", Json::array({"ssao_output"})},
                   {"depth", nullptr}}},
                 {"shader",
                  {{"vertex", "engine://fullscreen"},
                   {"fragment", "engine://fullscreen"}}}}}}})},
    };
    writeBytes(manual_path, manual_feature.dump(2) + "\n");
    const auto root_with_manual =
        AuthoredRenderConfigDocument::parse(
            readBytes(project.render_config))
            .withFeatureAdded(manual_reference)
            .bytes();
    writeBytes(project.render_config, root_with_manual);

    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    REQUIRE(std::ranges::any_of(
        runtime.frame_plan.at("nodes"), [&](const auto &node) {
            return node.value("name", std::string{}) == manual_pass_name;
        }));
    const auto context =
        service->getRenderAuthoringContext(Json::object());
    const auto accepted = service->removeAuthoredPass(
        {{"base_source_digest",
          context.at("source_digest").at("hex")},
         {"fragment_reference", manual_reference}});
    REQUIRE(accepted.at("status") == "accepted");
    const auto ticket = accepted.at("ticket").get<std::string>();
    service->commitPending();
    const auto result = service->getResult({{"ticket", ticket}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);
    REQUIRE(std::filesystem::is_regular_file(manual_path));
    const auto root = Json::parse(readBytes(project.render_config));
    REQUIRE(std::ranges::none_of(
        renderingSectionOf(root).at("features"), [&](const auto &feature) {
            return feature.is_string() &&
                   feature.get<std::string>() == manual_reference;
        }));
    REQUIRE(std::ranges::none_of(
        runtime.frame_plan.at("nodes"), [&](const auto &node) {
            return node.value("name", std::string{}) == manual_pass_name;
        }));
}

TEST_CASE(
    "WP334b a marker-less file inside the managed directory is never deleted",
    "[render-config-editor][wp334b][manual-fragment][ownership]") {
    // The namespace gate alone does not protect this one: the file sits below
    // passes/authoring/, so only the managed marker keeps it out of the purge.
    TemporaryAnimgraphProject project;
    const auto intruder_path =
        project.root / "passes" / "authoring" / "handwritten.json";
    const std::string intruder_reference =
        "project://passes/authoring/handwritten.json";
    const std::string intruder_pass_name = "wp334b_intruder_probe";
    const Json intruder{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "handwritten"},
        {"passes",
         Json::array(
             {{{"insert", "after:ssao_pass"},
               {"pass",
                {{"name", intruder_pass_name},
                 {"type", "fullscreen"},
                 {"input", Json::array({"gbuffer_normal"})},
                 {"output",
                  {{"color", Json::array({"ssao_output"})},
                   {"depth", nullptr}}},
                 {"shader",
                  {{"vertex", "engine://fullscreen"},
                   {"fragment", "engine://fullscreen"}}}}}}})},
    };
    std::filesystem::create_directories(intruder_path.parent_path());
    writeBytes(intruder_path, intruder.dump(2) + "\n");
    const auto intruder_bytes = readBytes(intruder_path);
    writeBytes(project.render_config,
               AuthoredRenderConfigDocument::parse(
                   readBytes(project.render_config))
                   .withFeatureAdded(intruder_reference)
                   .bytes());

    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context =
        service->getRenderAuthoringContext(Json::object());
    const auto accepted = service->removeAuthoredPass(
        {{"base_source_digest",
          context.at("source_digest").at("hex")},
         {"fragment_reference", intruder_reference}});
    REQUIRE(accepted.at("status") == "accepted");
    service->commitPending();
    const auto result = service->getResult(
        {{"ticket", accepted.at("ticket").get<std::string>()}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);

    // The reference is gone from the root, but the file the editor did not
    // author stays exactly as it was.
    REQUIRE(std::filesystem::is_regular_file(intruder_path));
    REQUIRE(readBytes(intruder_path) == intruder_bytes);

    // Negative control in the same test: a fragment the editor did author,
    // in the same directory, is removed.
    const auto after_remove =
        service->getRenderAuthoringContext(Json::object());
    const auto added = service->addAuthoredPass(
        {{"base_source_digest",
          after_remove.at("source_digest").at("hex")},
         {"graph", "main_render"},
         {"insert", "after:ssao_pass"},
         {"pass",
          {{"name", "wp334b_marker_control"},
           {"type", "fullscreen"},
           {"input", Json::array({"gbuffer_normal"})},
           {"output",
            {{"color", Json::array({"ssao_output"})},
             {"depth", nullptr}}},
           {"shader",
            {{"vertex", "engine://fullscreen"},
             {"fragment", "engine://fullscreen"}}}}}});
    REQUIRE(added.at("status") == "accepted");
    service->commitPending();
    const auto added_result = service->getResult(
        {{"ticket", added.at("ticket").get<std::string>()}});
    INFO(added_result.dump());
    REQUIRE(added_result.at("committed") == true);
    const auto managed_reference =
        added_result.at("fragment_reference").get<std::string>();
    const auto managed_path = std::get<std::filesystem::path>(
        resolver.resolveProjectRef(managed_reference));
    REQUIRE(std::filesystem::is_regular_file(managed_path));

    const auto before_purge =
        service->getRenderAuthoringContext(Json::object());
    const auto purged = service->removeAuthoredPass(
        {{"base_source_digest",
          before_purge.at("source_digest").at("hex")},
         {"fragment_reference", managed_reference}});
    REQUIRE(purged.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service
                ->getResult({{"ticket",
                              purged.at("ticket").get<std::string>()}})
                .at("committed") == true);
    REQUIRE_FALSE(std::filesystem::exists(managed_path));
}

TEST_CASE(
    "WP334b a shared managed fragment is purged only after its normalized reference count reaches zero",
    "[render-config-editor][wp334b][managed-fragment][ownership][shared]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context =
        service->getRenderAuthoringContext(Json::object());
    const Json pass{
        {"name", "wp334b_shared_probe"},
        {"type", "fullscreen"},
        {"input", Json::array({"gbuffer_normal"})},
        {"output",
         {{"color", Json::array({"ssao_output"})},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fullscreen"}}},
    };
    const auto accepted = service->addAuthoredPass(
        {{"base_source_digest", context.at("source_digest").at("hex")},
         {"graph", "main_render"},
         {"insert", "after:ssao_pass"},
         {"pass", pass}});
    REQUIRE(accepted.at("status") == "accepted");
    const auto reference =
        accepted.at("fragment_reference").get<std::string>();
    service->commitPending();
    REQUIRE(service->getResult(
                {{"ticket", accepted.at("ticket")}})
                .at("committed") == true);
    const auto fragment_path = std::get<std::filesystem::path>(
        resolver.resolveProjectRef(reference));
    REQUIRE(std::filesystem::is_regular_file(fragment_path));

    const auto filename = fragment_path.filename().generic_string();
    const auto alias =
        "project://passes/authoring/./" + filename;
    REQUIRE(resolver.normalizedReference(alias) ==
            resolver.normalizedReference(reference));
    writeBytes(
        project.render_config,
        AuthoredRenderConfigDocument::parse(
            readBytes(project.render_config))
            .withFeatureAdded(alias)
            .bytes());

    std::uint64_t published_generation = runtime.snapshot.published_generation;
    service = std::make_unique<RenderConfigEditorService>(
        RenderConfigEditorDependencies{
            .source_reference = "passes/main.json",
            .source_path = project.render_config,
            .source_bytes = readBytes(project.render_config),
            .project_root = project.root,
            .normalize_reference = [&resolver](std::string_view value) {
                return resolver.normalizedReference(value);
            },
            .resolve_document_path =
                [&resolver](std::string_view value) {
                    const auto resolved = resolver.resolveProjectRef(value);
                    const auto *path =
                        std::get_if<std::filesystem::path>(&resolved);
                    if (path == nullptr) {
                        throw std::runtime_error(
                            "WP334b ownership fixture expected a project path");
                    }
                    return *path;
                },
            .gate = [] { return RenderConfigEditorGateObservation{}; },
            .runtime_snapshot = [&published_generation] {
                return RenderConfigRuntimeSnapshot{
                    .published_generation = published_generation};
            },
            .apply_candidate =
                [&published_generation](
                    RenderConfigCandidateDocumentSet,
                    RenderConfigSourceCommit commit,
                    RenderConfigRuntimeApplyCompletion) {
                    commit();
                    return RenderConfigRuntimeApplyResult{
                        .committed = true,
                        .published_generation = ++published_generation};
                },
            .resolve_authoring_context =
                [](const RenderConfigCandidateDocumentSet &) {
                    return ResolvedRenderConfigAuthoringContext{};
                },
            .feature_catalog = [] {
                return std::vector<RenderFeatureCatalogEntry>{};
            },
        });
    auto remove = service->removeAuthoredPass(
        {{"base_source_digest",
          service->documentForTesting().sourceDigest()},
         {"fragment_reference", reference}});
    REQUIRE(remove.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", remove.at("ticket")}})
                .at("committed") == true);
    REQUIRE(std::filesystem::is_regular_file(fragment_path));
    auto root = Json::parse(readBytes(project.render_config));
    REQUIRE(std::ranges::any_of(
        renderingSectionOf(root).at("features"), [&](const auto &feature) {
            return feature.is_string() &&
                   feature.get<std::string>() == alias;
        }));

    remove = service->removeAuthoredPass(
        {{"base_source_digest",
          service->documentForTesting().sourceDigest()},
         {"fragment_reference", alias}});
    REQUIRE(remove.at("status") == "accepted");
    service->commitPending();
    REQUIRE(service->getResult({{"ticket", remove.at("ticket")}})
                .at("committed") == true);
    REQUIRE_FALSE(std::filesystem::exists(fragment_path));
}

TEST_CASE(
    "WP334b startup recovery rolls back prepared state and retains commit-last state",
    "[render-config-editor][wp334b][transaction][recovery][startup]") {
    TemporaryAnimgraphProject project;
    const auto original_root = readBytes(project.render_config);
    const std::string reference =
        "project://passes/authoring/pass-crash.json";
    const auto candidate_root =
        AuthoredRenderConfigDocument::parse(original_root)
            .withFeatureAdded(reference)
            .bytes();
    const std::string fragment =
        R"json({"schema":"pelican.render_feature","version":1,"name":"crash","passes":[]})json";
    const auto fragment_path =
        project.root / "passes" / "authoring" / "pass-crash.json";
    const auto transaction =
        renderConfigTransactionDirectory(project.root);
    const auto manifest = [&](std::string status) {
        return Json{
            {"schema", "pelican.render_authoring_transaction"},
            {"version", 1},
            {"status", std::move(status)},
            {"documents",
             Json::array(
                 {{{"destination",
                    "passes/authoring/pass-crash.json"},
                   {"operation", "create"},
                   {"expected_digest", ""},
                   {"next_digest",
                    renderConfigSourceDigest(fragment)}},
                  {{"destination", "passes/main.json"},
                   {"operation", "replace"},
                   {"expected_digest",
                    renderConfigSourceDigest(original_root)},
                   {"next_digest",
                    renderConfigSourceDigest(candidate_root)},
                   {"backup", "1.before"}}})},
            {"created_directories",
             Json::array({"passes/authoring"})},
        };
    };

    std::filesystem::create_directories(fragment_path.parent_path());
    writeBytes(fragment_path, fragment);
    writeBytes(project.render_config, candidate_root);
    std::filesystem::create_directories(transaction);
    writeBytes(transaction / "1.before", original_root);
    writeBytes(transaction / "manifest.json",
               manifest("prepared").dump(2) + "\n");
    recoverRenderConfigDocumentTransaction(project.root);
    REQUIRE(readBytes(project.render_config) == original_root);
    REQUIRE_FALSE(std::filesystem::exists(fragment_path));
    REQUIRE_FALSE(std::filesystem::exists(transaction));

    std::filesystem::create_directories(fragment_path.parent_path());
    writeBytes(fragment_path, fragment);
    writeBytes(project.render_config, candidate_root);
    std::filesystem::create_directories(transaction);
    writeBytes(transaction / "1.before", original_root);
    // MG-2a: `status: "committed"` in manifest.json is the previous format.
    // Startup recovery stops on it by name and leaves every byte in place.
    const auto legacy_manifest = manifest("committed").dump(2) + "\n";
    writeBytes(transaction / "manifest.json", legacy_manifest);
    REQUIRE_THROWS_AS(recoverRenderConfigDocumentTransaction(project.root),
                      RenderConfigLegacyTransactionDirectory);
    REQUIRE(readBytes(project.render_config) == candidate_root);
    REQUIRE(readBytes(fragment_path) == fragment);
    REQUIRE(readBytes(transaction / "manifest.json") == legacy_manifest);
    REQUIRE(readBytes(transaction / "1.before") == original_root);
}

TEST_CASE(
    "WP334b one candidate-first loader resolves both staged preset and feature documents",
    "[render-config-editor][wp334b][overlay][feature][preset]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const std::string root_reference = "passes/main.json";
    const std::string preset_reference =
        "project://passes/staged-preset.json";
    const std::string feature_reference =
        "project://passes/staged-feature.json";
    const auto preset_path = project.root / "passes" /
                             "staged-preset.json";
    const auto feature_path = project.root / "passes" /
                              "staged-feature.json";
    const std::string root_bytes =
        Json{{"pipeline", {{"preset", preset_reference}}},
             {"features", Json::array({feature_reference})}}
            .dump();
    const std::string preset_bytes = engineResourceOrThrow(
        "render_pipelines/hybrid_v1.json");
    const std::string feature_bytes =
        Json{{"schema", "pelican.render_feature"},
             {"version", 1},
             {"name", "staged_feature"},
             {"runtime_shader_compiler", "optional"},
             {"passes", Json::array()}}
            .dump();
    const auto root_key = resolver.normalizedReference(root_reference);
    const auto documents = RenderConfigCandidateDocumentSet{
        root_key,
        {{.reference = root_reference,
          .normalized_reference = root_key,
          .path = project.render_config,
          .operation = RenderConfigDocumentOperation::replace,
          .expected =
              {.existence = RenderConfigDocumentExistence::present,
               .digest = renderConfigSourceDigest(
                   readBytes(project.render_config))},
          .bytes = root_bytes},
         {.reference = preset_reference,
          .normalized_reference =
              resolver.normalizedReference(preset_reference),
          .path = preset_path,
          .operation = RenderConfigDocumentOperation::create,
          .expected =
              {.existence = RenderConfigDocumentExistence::missing},
          .bytes = preset_bytes},
         {.reference = feature_reference,
          .normalized_reference =
              resolver.normalizedReference(feature_reference),
          .path = feature_path,
          .operation = RenderConfigDocumentOperation::create,
          .expected =
              {.existence = RenderConfigDocumentExistence::missing},
          .bytes = feature_bytes}}};
    REQUIRE_THROWS(resolver.loadText(preset_reference));
    REQUIRE_THROWS(resolver.loadText(feature_reference));

    std::vector<std::string> loaded;
    const auto loader = [&](std::string_view reference) {
        loaded.emplace_back(reference);
        return loadRenderConfigCandidateText(
            documents, resolver, reference);
    };
    const auto composition = composeRenderFeatureConfig(
        Json::parse(documents.rootDocument().bytes),
        RenderFeatureComposeDependencies{
            .load_feature_json = loader,
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .load_pipeline_json = loader,
        });
    REQUIRE(composition.pipeline_preset.has_value());
    REQUIRE(composition.pipeline_preset->reference == preset_reference);
    REQUIRE(containsName(composition.feature_names, "staged_feature"));
    REQUIRE(std::ranges::count(loaded, preset_reference) == 1);
    REQUIRE(std::ranges::count(loaded, feature_reference) == 1);
}

// --- F-1: render targets declared from the editor.

namespace {

const char *const renderTargetFragment =
    "project://passes/authoring/render-targets.json";

Json sceneTarget(std::string_view name, double extent_scale = 1.0) {
    return Json{{"name", name},
                {"extent_scale", extent_scale},
                {"format", "R16G16B16A16_SFLOAT"},
                {"format_class", "scene"},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
}

// One project, one engine-side service, and the calls a test makes against
// them. `direct` picks the graph root that declares its own render_targets;
// otherwise the root selects the engine preset hybrid_v1.
struct RenderTargetHarness {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    ActualCpuRenderRuntime runtime;
    std::unique_ptr<RenderConfigEditorService> service;

    explicit RenderTargetHarness(bool direct) {
        if (direct) writeBytes(project.render_config, graphRootFixture("main"));
        resolver.setup(project.root, false);
        service = makeProjectService(project, resolver, runtime);
    }

    nlohmann::ordered_json context() {
        return service->getRenderAuthoringContext(Json::object());
    }

    std::string digest() {
        return context().at("source_digest").at("hex").get<std::string>();
    }

    nlohmann::ordered_json edit(Json ops) {
        return service->editRenderTargets(
            {{"base_source_digest", digest()}, {"ops", std::move(ops)}});
    }

    nlohmann::ordered_json commit(Json ops) {
        const auto accepted = edit(std::move(ops));
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPending();
        const auto result =
            service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        return result;
    }

    std::optional<nlohmann::ordered_json> target(std::string_view name) {
        const auto current = context();
        for (const auto &row : current.at("render_targets"))
            if (row.at("name") == std::string{name}) return row;
        return std::nullopt;
    }

    std::filesystem::path fragmentPath() const {
        return project.root / "passes" / "authoring" / "render-targets.json";
    }

    Json rootFeatures() const {
        return renderingSectionOrEmpty(
                   Json::parse(readBytes(project.render_config)))
            .value("features", Json::array());
    }

    bool planHasResource(std::string_view name) const {
        return std::ranges::any_of(
            runtime.frame_plan.value("resources", Json::array()),
            [&](const Json &resource) {
                return resource.value("name", std::string{}) == name;
            });
    }
};

Json addOp(Json target) {
    return Json::array({{{"op", "add"}, {"target", std::move(target)}}});
}

} // namespace

TEST_CASE("F-1 edit_render_targets adds, replaces and removes a target in its managed fragment",
          "[render-config-editor][f1][render-targets]") {
    const bool direct = GENERATE(true, false);
    INFO((direct ? "direct graph root" : "hybrid_v1 preset root"));
    RenderTargetHarness harness{direct};
    REQUIRE(harness.context().at("config_kind") ==
            (direct ? "direct" : "preset"));
    REQUIRE_FALSE(std::filesystem::exists(harness.fragmentPath()));

    const auto added = harness.commit(addOp(sceneTarget("f1_probe")));
    CHECK(added.at("operation") == "edit_render_targets");
    CHECK(added.at("fragment_reference") == renderTargetFragment);

    // The fragment exists, is referenced from the root, and is managed.
    REQUIRE(std::filesystem::is_regular_file(harness.fragmentPath()));
    const auto fragment = Json::parse(readBytes(harness.fragmentPath()));
    CHECK(fragment.at("schema") == "pelican.render_feature");
    CHECK(fragment.at("pelican_editor_managed").at("id") == "render-targets");
    REQUIRE(fragment.at("render_targets").size() == 1);
    CHECK(fragment.at("render_targets").at(0) == sceneTarget("f1_probe"));
    CHECK(std::ranges::count(harness.rootFeatures(),
                             Json(renderTargetFragment)) == 1);

    // The context reports it first, editable, from the authoring fragment,
    // and does not offer the fragment as an authored pass.
    const auto context = harness.context();
    REQUIRE_FALSE(context.at("render_targets").empty());
    const auto &first = context.at("render_targets").at(0);
    CHECK(first.at("name") == "f1_probe");
    CHECK(first.at("editable") == true);
    CHECK(first.at("source") == "authoring");
    CHECK(Json(first.at("authored_declaration")) == sceneTarget("f1_probe"));
    CHECK(context.at("managed_fragments").empty());
    CHECK(context.at("render_target_fragment") == renderTargetFragment);
    const auto &vocabulary = context.at("render_target_vocabulary");
    CHECK(std::ranges::count(vocabulary.at("usage"), "SAMPLED") == 1);
    CHECK(std::ranges::count(vocabulary.at("formats"), "R16G16B16A16_SFLOAT") ==
          1);

    // The engine's own pipeline carries it.
    CHECK(harness.planHasResource("f1_probe"));

    // Everything else says where it came from.
    const auto display = harness.target("display");
    REQUIRE(display);
    CHECK(display->at("editable") == false);
    CHECK(display->at("source") == "engine");
    const auto albedo = harness.target("gbuffer_albedo");
    REQUIRE(albedo);
    CHECK(albedo->at("editable") == false);
    CHECK(albedo->at("source") == (direct ? "root" : "preset"));

    // Replace changes the declaration in place.
    harness.commit(Json::array({{{"op", "replace"},
                                 {"name", "f1_probe"},
                                 {"target", sceneTarget("f1_probe", 0.5)}}}));
    const auto replaced = harness.target("f1_probe");
    REQUIRE(replaced);
    CHECK(replaced->at("authored_declaration").at("extent_scale") == 0.5);
    CHECK(Json::parse(readBytes(harness.fragmentPath()))
              .at("render_targets")
              .at(0)
              .at("extent_scale") == 0.5);

    // A rename is a replace with a new name.
    harness.commit(Json::array({{{"op", "replace"},
                                 {"name", "f1_probe"},
                                 {"target", sceneTarget("f1_renamed", 0.5)}}}));
    CHECK_FALSE(harness.target("f1_probe"));
    CHECK(harness.target("f1_renamed"));
    CHECK(harness.planHasResource("f1_renamed"));

    // Remove leaves the fragment and its reference behind, empty.
    harness.commit(
        Json::array({{{"op", "remove"}, {"name", "f1_renamed"}}}));
    CHECK_FALSE(harness.target("f1_renamed"));
    CHECK_FALSE(harness.planHasResource("f1_renamed"));
    REQUIRE(std::filesystem::is_regular_file(harness.fragmentPath()));
    CHECK(Json::parse(readBytes(harness.fragmentPath()))
              .at("render_targets")
              .empty());
    CHECK(std::ranges::count(harness.rootFeatures(),
                             Json(renderTargetFragment)) == 1);
}

TEST_CASE("F-1 edit_render_targets refuses by name and leaves the documents alone",
          "[render-config-editor][f1][render-targets][rejection]") {
    RenderTargetHarness harness{true};
    harness.commit(addOp(sceneTarget("f1_owned")));
    const auto root_before = readBytes(harness.project.render_config);
    const auto fragment_before = readBytes(harness.fragmentPath());
    const auto unchanged = [&] {
        CHECK(readBytes(harness.project.render_config) == root_before);
        CHECK(readBytes(harness.fragmentPath()) == fragment_before);
    };
    const auto code = [](const nlohmann::ordered_json &response) {
        INFO(response.dump());
        REQUIRE(response.at("status") == "rejected");
        return response.at("error").at("code").get<std::string>();
    };

    SECTION("a target declared elsewhere is not editable") {
        const auto root = harness.edit(Json::array(
            {{{"op", "replace"},
              {"name", "gbuffer_albedo"},
              {"target", sceneTarget("gbuffer_albedo", 0.5)}}}));
        CHECK(code(root) == "render_target_not_editable");
        CHECK(root.at("error").at("name") == "gbuffer_albedo");
        CHECK(root.at("error").at("source") == "root");
        const auto feature = harness.edit(Json::array(
            {{{"op", "remove"}, {"name", "Bloom_Threshold_RT"}}}));
        CHECK(code(feature) == "render_target_not_editable");
        CHECK(feature.at("error").at("source") ==
              "feature:engine://features/bloom.json");
        unchanged();
    }

    SECTION("an added name that is declared anywhere already exists") {
        const auto root = harness.edit(addOp(sceneTarget("gbuffer_albedo")));
        CHECK(code(root) == "render_target_exists");
        CHECK(root.at("error").at("source") == "root");
        const auto own = harness.edit(addOp(sceneTarget("f1_owned")));
        CHECK(code(own) == "render_target_exists");
        CHECK(own.at("error").at("source") == "authoring");
        const auto renamed = harness.edit(Json::array(
            {{{"op", "replace"},
              {"name", "f1_owned"},
              {"target", sceneTarget("lit_color")}}}));
        CHECK(code(renamed) == "render_target_exists");
        unchanged();
    }

    SECTION("the engine's own names are reserved") {
        CHECK(code(harness.edit(addOp(sceneTarget("display")))) ==
              "render_target_reserved");
        CHECK(code(harness.edit(Json::array(
                  {{{"op", "remove"}, {"name", "swapchain"}}}))) ==
              "render_target_reserved");
        CHECK(code(harness.edit(Json::array(
                  {{{"op", "replace"},
                    {"name", "f1_owned"},
                    {"target", sceneTarget("display")}}}))) ==
              "render_target_reserved");
        unchanged();
    }

    SECTION("a target a pass still writes cannot be removed or renamed") {
        const auto pass = harness.service->addAuthoredPass(
            {{"base_source_digest", harness.digest()},
             {"graph", "main_render"},
             {"insert", "after:ssao_pass"},
             {"pass",
              {{"name", "f1_writer"},
               {"type", "fullscreen"},
               {"input", Json::array({"gbuffer_normal"})},
               {"output",
                {{"color", Json::array({"f1_owned"})}, {"depth", nullptr}}},
               {"shader",
                {{"vertex", "engine://fullscreen"},
                 {"fragment", "engine://fullscreen"}}}}}});
        REQUIRE(pass.at("status") == "accepted");
        harness.service->commitPending();
        REQUIRE(harness.service->getResult({{"ticket", pass.at("ticket")}})
                    .at("committed") == true);
        const auto root_with_pass = readBytes(harness.project.render_config);

        const auto removed = harness.edit(
            Json::array({{{"op", "remove"}, {"name", "f1_owned"}}}));
        CHECK(code(removed) == "render_target_referenced");
        CHECK(removed.at("error").at("name") == "f1_owned");
        CHECK(Json(removed.at("error").at("users")) == Json::array({"f1_writer"}));
        const auto renamed = harness.edit(Json::array(
            {{{"op", "replace"},
              {"name", "f1_owned"},
              {"target", sceneTarget("f1_elsewhere")}}}));
        CHECK(code(renamed) == "render_target_referenced");
        CHECK(readBytes(harness.project.render_config) == root_with_pass);
        CHECK(readBytes(harness.fragmentPath()) == fragment_before);
    }

    SECTION("a malformed declaration fails preflight in the parser's words") {
        const auto rejected =
            harness.edit(addOp(sceneTarget("f1_negative", -1.0)));
        CHECK(code(rejected) == "render_pipeline_preflight_failed");
        CHECK(rejected.at("error").at("message").get<std::string>().find(
                  "extent_scale must be positive") != std::string::npos);
        unchanged();
    }

    SECTION("a stale base digest is refused before anything is read") {
        const auto rejected = harness.service->editRenderTargets(
            {{"base_source_digest", std::string(64, '0')},
             {"ops", addOp(sceneTarget("f1_stale"))}});
        CHECK(code(rejected) == "external_modification");
        unchanged();
    }

    SECTION("the fragment cannot be removed as if it were an authored pass") {
        const auto rejected = harness.service->removeAuthoredPass(
            {{"base_source_digest", harness.digest()},
             {"fragment_reference", std::string{renderTargetFragment}}});
        CHECK(code(rejected) == "render_target_not_editable");
        unchanged();
    }
}

TEST_CASE("F-1 edit_render_targets on a missing fragment creates it only with the batch",
          "[render-config-editor][f1][render-targets]") {
    RenderTargetHarness harness{false};
    const auto root_before = readBytes(harness.project.render_config);
    // A remove with nothing authored names no target the editor owns.
    const auto rejected = harness.edit(
        Json::array({{{"op", "remove"}, {"name", "f1_nothing"}}}));
    INFO(rejected.dump());
    CHECK(rejected.at("error").at("code") == "render_target_not_found");
    CHECK_FALSE(std::filesystem::exists(harness.fragmentPath()));
    CHECK(readBytes(harness.project.render_config) == root_before);

    // Two ops in one request are applied in order and land together.
    harness.commit(Json::array({{{"op", "add"}, {"target", sceneTarget("f1_a")}},
                                {{"op", "add"}, {"target", sceneTarget("f1_b")}}}));
    const auto context = harness.context();
    CHECK(context.at("render_targets").at(0).at("name") == "f1_a");
    CHECK(context.at("render_targets").at(1).at("name") == "f1_b");

    // All or nothing: the second op fails, so the first does not land either.
    const auto fragment_before = readBytes(harness.fragmentPath());
    const auto partial = harness.edit(
        Json::array({{{"op", "remove"}, {"name", "f1_a"}},
                     {{"op", "add"}, {"target", sceneTarget("display")}}}));
    CHECK(partial.at("error").at("code") == "render_target_reserved");
    CHECK(readBytes(harness.fragmentPath()) == fragment_before);
}

// --- F-6: a direct root edits the passes it declares itself.

namespace {

Json f6DrawPattern() {
    return Json{{"name", "draw_pattern"},
                {"type", "fullscreen"},
                {"input", Json::array()},
                {"output", {{"color", Json::array({"scene_color"})}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"},
                            {"fragment", "engine://fullscreen"}}}};
}

// The blank template's `present`, with one member edit_preset_pass is never
// sent, so "the other fields are kept" has something to keep.
Json f6Present() {
    return Json{{"name", "present"},
                {"type", "fullscreen"},
                {"input", Json::array()},
                {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"},
                            {"fragment", "engine://fullscreen"}}},
                {"clear_color", Json::array({0.1, 0.2, 0.3, 1.0})}};
}

// A Blank pipeline project after its first two steps: a graph root with no
// `pipeline`, so config_kind is "direct", that declares scene_color and a pass
// writing it before the template's present.
Json f6DirectRoot() {
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", Json::object()},
                {"nodes", Json::array()},
                {"outputs", Json::object()},
                {"rendering",
                 {{"render_targets", Json::array({sceneTarget("scene_color")})},
                  {"rendering_passes",
                   Json::array({{{"name", "main_render"},
                                 {"passes", Json::array({f6DrawPattern(), f6Present()})}}})}}}};
}

const nlohmann::ordered_json *f6ContextPass(const nlohmann::ordered_json &context,
                                            std::string_view name) {
    for (const auto &graph : context.at("graphs"))
        for (const auto &pass : graph.at("passes"))
            if (pass.at("name") == std::string{name}) return &pass;
    return nullptr;
}

// Every file of the project but the root, so "the root and nothing else" is a
// comparison.
std::map<std::string, std::string> f6FilesBesideRoot(const TemporaryAnimgraphProject &project) {
    std::map<std::string, std::string> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{project.root}) {
        if (!entry.is_regular_file() ||
            std::filesystem::equivalent(entry.path(), project.render_config))
            continue;
        files.emplace(std::filesystem::relative(entry.path(), project.root).generic_string(),
                      readBytes(entry.path()));
    }
    return files;
}

} // namespace

TEST_CASE("F-6 edit_preset_pass writes a pass the direct root declares into the root",
          "[render-config-editor][f6][root-pass-edit]") {
    TemporaryAnimgraphProject project;
    writeBytes(project.render_config, f6DirectRoot().dump(2) + "\n");
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context = service->getRenderAuthoringContext(Json::object());
    REQUIRE(context.at("config_kind") == "direct");

    // Acceptance 2: the root's own passes are editable in the root, and carry
    // the declaration as the root holds it; what the engine adds is not.
    for (const auto *name : {"draw_pattern", "present"}) {
        CAPTURE(name);
        const auto *pass = f6ContextPass(context, name);
        REQUIRE(pass != nullptr);
        CHECK(pass->at("editable_root_pass") == true);
        CHECK(pass->at("editable_preset_pass") == false);
    }
    CHECK(Json(f6ContextPass(context, "present")->at("root_declaration")) == f6Present());
    const auto *engine_pass = f6ContextPass(context, "output_transform");
    REQUIRE(engine_pass != nullptr);
    CHECK(engine_pass->at("provenance").at("source") == "engine");
    CHECK(engine_pass->at("editable_root_pass") == false);
    CHECK_FALSE(engine_pass->contains("root_declaration"));

    const auto root_before = readBytes(project.render_config);
    const auto others_before = f6FilesBesideRoot(project);
    Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                 {"graph", "main_render"},
                 {"pass_name", "present"},
                 {"input", Json::array({"scene_color"})},
                 {"output", f6Present().at("output")},
                 {"shader", f6Present().at("shader")}};

    SECTION("present reads scene_color: the root changes, the other fields stay, nothing else is written") {
        const auto accepted = service->editPresetPass(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        CHECK(readBytes(project.render_config) == root_before);
        service->commitPending();
        const auto result = service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("committed") == true);
        const auto root_after = readBytes(project.render_config);
        auto expected = f6DirectRoot();
        expected["rendering"]["rendering_passes"][0]["passes"][1]["input"] =
            Json::array({"scene_color"});
        CHECK(Json::parse(root_after) == expected);
        requireOnlySectionKeyChanged(root_before, root_after, "rendering_passes");
        CHECK(f6FilesBesideRoot(project) == others_before);
        const auto &nodes = runtime.frame_plan.at("nodes");
        const auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &value) {
            return value.at("name") == "present";
        });
        REQUIRE(node != nodes.end());
        CHECK(node->at("reads") == Json::array({"scene_color"}));

        // The digest the edit was made against named the old root.
        const auto stale = service->editPresetPass(request);
        CHECK(stale.at("error").at("code") == "external_modification");

        service.reset();
        service = makeProjectService(project, resolver, runtime);
        const auto reopened = service->getRenderAuthoringContext(Json::object());
        const auto *saved = f6ContextPass(reopened, "present");
        REQUIRE(saved != nullptr);
        CHECK(saved->at("editable_root_pass") == true);
        CHECK(Json(saved->at("root_declaration")) ==
              expected["rendering"]["rendering_passes"][0]["passes"][1]);
    }
    SECTION("a pass the engine adds is not the root's") {
        request["pass_name"] = "output_transform";
        const auto rejected = service->editPresetPass(request);
        INFO(rejected.dump());
        REQUIRE(rejected.at("status") == "rejected");
        CHECK(rejected.at("error").at("code") == "root_pass_not_found");
        CHECK(rejected.at("error").at("graph") == "main_render");
        CHECK(rejected.at("error").at("pass_name") == "output_transform");
        CHECK(rejected.at("error").at("provenance") == "engine");
        request["pass_name"] = "nobody_declared_this";
        const auto absent = service->editPresetPass(request);
        CHECK(absent.at("error").at("code") == "root_pass_not_found");
        CHECK(absent.at("error").at("provenance").is_null());
        service->commitPending();
        CHECK(readBytes(project.render_config) == root_before);
    }
    SECTION("the root digest is compared before anything is written") {
        request["base_source_digest"] = std::string(64, '0');
        const auto rejected = service->editPresetPass(request);
        INFO(rejected.dump());
        CHECK(rejected.at("error").at("code") == "external_modification");
        service->commitPending();
        CHECK(readBytes(project.render_config) == root_before);
    }
    SECTION("material routing is a preset's, and is not written into a root") {
        request["material_routing"] = nullptr;
        CHECK(service->editPresetPass(request).at("error").at("code") == "root_routing_not_editable");
        CHECK(readBytes(project.render_config) == root_before);
    }
}

TEST_CASE("F-6 a preset root still requires a fork before its passes are edited",
          "[render-config-editor][f6][root-pass-edit]") {
    // animgraph_demo selects the engine preset hybrid_v1: a request without a
    // document digest is not a root edit there.
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto context = service->getRenderAuthoringContext(Json::object());
    REQUIRE(context.at("config_kind") == "preset");
    const auto *present = f6ContextPass(context, "scene_present");
    REQUIRE(present != nullptr);
    CHECK(present->at("editable_root_pass") == false);
    const auto root_before = readBytes(project.render_config);
    const Json request{{"base_source_digest", context.at("source_digest").at("hex")},
                       {"graph", "main_render"},
                       {"pass_name", "scene_present"},
                       {"input", Json::array({"gbuffer_albedo"})},
                       {"output", present->at("declaration").at("output")},
                       {"shader", {{"vertex", "engine://fullscreen"},
                                   {"fragment", "engine://fullscreen"}}}};
    const auto rejected = service->editPresetPass(request);
    INFO(rejected.dump());
    CHECK(rejected.at("error").at("code") == "project_preset_required");
    service->commitPending();
    CHECK(readBytes(project.render_config) == root_before);
}

// --- G-1: a material pass is authored the way a fullscreen pass is.

namespace {

Json g1ForwardColor(std::string_view name) {
    return Json{{"name", name},
                {"extent_scale", 1.0},
                {"format", "R16G16B16A16_SFLOAT"},
                {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
}

Json g1Depth(std::string_view name) {
    return Json{{"name", name},
                {"extent_scale", 1.0},
                {"format", "D32_SFLOAT"},
                {"format_class", "data"},
                {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}};
}

Json g1MaterialPass(std::string_view name, std::string_view contract,
                    Json colors, Json depth) {
    return Json{{"name", name},
                {"type", "material"},
                {"material_contract", contract},
                {"output", {{"color", std::move(colors)}, {"depth", std::move(depth)}}}};
}

// The first anchor the context offers for main_render: before its first pass,
// where a scene's geometry is drawn.
std::string g1FirstAnchor(const nlohmann::ordered_json &context) {
    for (const auto &graph : context.at("graphs"))
        if (graph.at("name") == "main_render")
            return graph.at("anchor_candidates").at(0).at("insert").get<std::string>();
    return {};
}

} // namespace

TEST_CASE("G-1 add_authored_pass adds a forward material pass to a direct and a preset root",
          "[render-config-editor][g1][material-pass]") {
    const bool direct = GENERATE(true, false);
    INFO((direct ? "direct graph root" : "hybrid_v1 preset root"));
    RenderTargetHarness harness{direct};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g1_color")}},
                                {{"op", "add"}, {"target", g1Depth("g1_depth")}}}));
    const auto context = harness.context();
    const auto accepted = harness.service->addAuthoredPass(
        {{"base_source_digest", context.at("source_digest").at("hex")},
         {"graph", "main_render"},
         {"insert", g1FirstAnchor(context)},
         {"pass", g1MaterialPass("g1_forward", "forward_opaque_v1",
                                 Json::array({"g1_color"}), "g1_depth")}});
    INFO(accepted.dump());
    REQUIRE(accepted.at("status") == "accepted");
    harness.service->commitPending();
    const auto result = harness.service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);

    // The frame plan the engine published draws it into the two targets.
    const auto &nodes = harness.runtime.frame_plan.at("nodes");
    const auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &value) {
        return value.at("name") == "g1_forward";
    });
    REQUIRE(node != nodes.end());
    CHECK(std::ranges::count(node->at("writes"), Json("g1_color")) == 1);

    // It is a managed feature pass, which the Pass Editor edits in its fragment.
    const auto after = harness.context();
    const auto *pass = f6ContextPass(after, "g1_forward");
    REQUIRE(pass != nullptr);
    CHECK(pass->at("managed") == true);
    CHECK(pass->at("provenance").at("source") == "feature");
    CHECK(pass->at("editable_feature_pass") == true);
    CHECK(pass->at("declaration").at("type") == "material");
    // The context says which features the running pipeline enabled.
    CHECK(after.at("enabled_feature_names").is_array());
}

TEST_CASE("G-1 a material pass of the wrong shape is refused in the parser's words",
          "[render-config-editor][g1][material-pass][rejection]") {
    RenderTargetHarness harness{true};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g1_color")}},
                                {{"op", "add"}, {"target", g1Depth("g1_depth")}}}));
    const auto context = harness.context();
    const auto root_before = readBytes(harness.project.render_config);
    const auto fragment_before = readBytes(harness.fragmentPath());
    const auto authoring_directory = harness.project.root / "passes" / "authoring";
    const auto files_before = [&] {
        std::set<std::string> files;
        for (const auto &entry : std::filesystem::directory_iterator{authoring_directory})
            files.insert(entry.path().filename().string());
        return files;
    };
    const auto listed_before = files_before();
    const auto refused = [&](Json pass, std::string_view words) {
        const auto rejected = harness.service->addAuthoredPass(
            {{"base_source_digest", context.at("source_digest").at("hex")},
             {"graph", "main_render"},
             {"insert", g1FirstAnchor(context)},
             {"pass", std::move(pass)}});
        INFO(rejected.dump());
        REQUIRE(rejected.at("status") == "rejected");
        CHECK(rejected.at("error").at("code") == "render_pipeline_preflight_failed");
        CHECK(rejected.at("error").at("message").get<std::string>().find(words) !=
              std::string::npos);
        harness.service->commitPending();
        CHECK(readBytes(harness.project.render_config) == root_before);
        CHECK(readBytes(harness.fragmentPath()) == fragment_before);
        CHECK(files_before() == listed_before);
    };

    SECTION("a G-buffer pass with three colors, not five") {
        refused(g1MaterialPass("g1_gbuffer", "deferred_geometry_v1",
                               Json::array({"gbuffer_albedo", "gbuffer_normal",
                                            "gbuffer_material"}),
                               "g1_depth"),
                "requires exactly 5 color outputs");
    }
    SECTION("a material pass drawing to the swapchain") {
        refused(g1MaterialPass("g1_swapchain", "forward_opaque_v1",
                               Json::array({"swapchain"}), "g1_depth"),
                "swapchain");
    }
    SECTION("a material pass with no depth") {
        refused(g1MaterialPass("g1_depthless", "forward_opaque_v1",
                               Json::array({"g1_color"}), nullptr),
                "requires depth output");
    }
    SECTION("a raster pass is still not authored here") {
        const auto rejected = harness.service->addAuthoredPass(
            {{"base_source_digest", context.at("source_digest").at("hex")},
             {"graph", "main_render"},
             {"insert", g1FirstAnchor(context)},
             {"pass",
              {{"name", "g1_raster"},
               {"type", "raster"},
               {"output", {{"color", Json::array({"g1_color"})}, {"depth", nullptr}}}}}});
        INFO(rejected.dump());
        CHECK(rejected.at("error").at("code") == "authored_pass_type_unavailable");
        CHECK(rejected.at("error").at("requested_type") == "raster");
        CHECK(readBytes(harness.project.render_config) == root_before);
    }
}

// --- G-4: material_route_default travels with the pass in one ticket.

namespace {

nlohmann::ordered_json g4AddForward(RenderTargetHarness &harness,
                                    std::optional<std::string> route_default) {
    const auto context = harness.context();
    Json params{{"base_source_digest", context.at("source_digest").at("hex")},
                {"graph", "main_render"},
                {"insert", g1FirstAnchor(context)},
                {"pass", g1MaterialPass("g4_forward", "forward_opaque_v1",
                                        Json::array({"g4_color"}), "g4_depth")}};
    if (route_default) params["material_route_default"] = *route_default;
    return harness.service->addAuthoredPass(params);
}

MaterialRouteDefault g4RootRouteDefault(const RenderTargetHarness &harness) {
    return readMaterialRouteDefault(
        renderingSectionOrEmpty(Json::parse(readBytes(harness.project.render_config))));
}

std::size_t g4AuthoringFileCount(const RenderTargetHarness &harness) {
    std::size_t count = 0;
    const auto directory = harness.project.root / "passes" / "authoring";
    if (!std::filesystem::exists(directory)) return 0;
    for (const auto &entry : std::filesystem::directory_iterator{directory}) {
        (void)entry;
        ++count;
    }
    return count;
}

} // namespace

#if PELICAN_RUNTIME_SHADER_COMPILER
// Registered only where the runtime shader compiler is on: without it the
// forward default is refused by name (the G-4 test at the end of this file).
TEST_CASE("G-4 add_authored_pass sets material_route_default in the same candidate set and asks for a restart",
          "[render-config-editor][g4][material-pass]") {
    const bool direct = GENERATE(true, false);
    INFO((direct ? "direct graph root" : "hybrid_v1 preset root"));
    RenderTargetHarness harness{direct};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g4_color")}},
                                {{"op", "add"}, {"target", g1Depth("g4_depth")}}}));
    REQUIRE(g4RootRouteDefault(harness) == MaterialRouteDefault::automatic);

    // The runtime judges one candidate set: the new fragment and the root
    // that names the forward default.
    std::optional<MaterialRouteDefault> judged_default;
    bool judged_fragment = false;
    harness.runtime.validate_candidate = [&](const RenderConfigCandidateDocumentSet &documents) {
        judged_default = readMaterialRouteDefault(
            renderingSectionOrEmpty(Json::parse(documents.rootDocument().bytes)));
        judged_fragment = std::ranges::any_of(documents.documents(), [](const auto &document) {
            return document.operation == RenderConfigDocumentOperation::create &&
                   document.bytes.find("g4_forward") != std::string::npos;
        });
    };
    const auto accepted = g4AddForward(harness, "forward");
    INFO(accepted.dump());
    REQUIRE(accepted.at("status") == "accepted");
    harness.service->commitPending();
    const auto result = harness.service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);
    CHECK(result.at("restart_required") == true);
    CHECK(judged_default == MaterialRouteDefault::forward);
    CHECK(judged_fragment);
    CHECK(g4RootRouteDefault(harness) == MaterialRouteDefault::forward);
}
#endif

TEST_CASE("G-4 an ordinary add says nothing about a restart, and a bad value is refused by name",
          "[render-config-editor][g4][material-pass]") {
    RenderTargetHarness harness{true};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g4_color")}},
                                {{"op", "add"}, {"target", g1Depth("g4_depth")}}}));
    REQUIRE_THROWS_WITH(g4AddForward(harness, "deferred"),
                        Catch::Matchers::ContainsSubstring("material_route_default") &&
                            Catch::Matchers::ContainsSubstring("\"forward\""));
    const auto accepted = g4AddForward(harness, std::nullopt);
    REQUIRE(accepted.at("status") == "accepted");
    harness.service->commitPending();
    const auto result = harness.service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("committed") == true);
    CHECK_FALSE(result.contains("restart_required"));
    CHECK(g4RootRouteDefault(harness) == MaterialRouteDefault::automatic);
}

TEST_CASE("G-4 the renderer's material_route_unserved@1 comes back as a named rejection with its fields",
          "[render-config-editor][g4][material-pass][rejection]") {
    RenderTargetHarness harness{true};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g4_color")}},
                                {{"op", "add"}, {"target", g1Depth("g4_depth")}}}));
    const auto root_before = readBytes(harness.project.render_config);
    const auto files_before = g4AuthoringFileCount(harness);
    harness.runtime.validate_candidate = [](const RenderConfigCandidateDocumentSet &) {
        throw std::runtime_error(materialRouteUnservedMessage(
            {UnservedMaterialRoute{
                MaterialRouteClass::deferred_geometry,
                MaterialShaderContract::gbuffer_v1,
                {describeUnservedMaterial("pelican.builtin.transparent", "", ""),
                 describeUnservedMaterial("box", "", "project://box.glb")}}},
            "project://passes/main.json"));
    };
    const auto accepted = g4AddForward(harness, std::nullopt);
    REQUIRE(accepted.at("status") == "accepted");
    harness.service->commitPending();
    const auto result = harness.service->getResult({{"ticket", accepted.at("ticket")}});
    INFO(result.dump());
    REQUIRE(result.at("status") == "rejected");
    const auto &error = result.at("error");
    CHECK(error.at("code") == "material_route_unserved@1");
    CHECK(error.at("participant") == "pelican.render_pipeline");
    CHECK(error.at("ticket") == accepted.at("ticket"));
    REQUIRE(error.at("unserved").size() == 1);
    CHECK(error.at("unserved").at(0).at("route") == "deferred_geometry");
    CHECK(error.at("unserved").at(0).at("materials").at(0).at("name") ==
          "pelican.builtin.transparent");
    CHECK(error.at("unserved").at(0).at("materials").at(0).at("origin") == "builtin");
    CHECK_THAT(error.at("hint").get<std::string>(),
               Catch::Matchers::ContainsSubstring("material_route_default"));
    CHECK_THAT(error.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("'pelican.builtin.transparent' (engine built-in)") &&
                   Catch::Matchers::ContainsSubstring("Hint: "));
    // Nothing was written.
    CHECK(readBytes(harness.project.render_config) == root_before);
    CHECK(g4AuthoringFileCount(harness) == files_before);
}

// Q-14e: the renderer's typed input/shader mismatch reaches the ticket's
// rejection as one structured field; the code and the message stay as they
// were. The running renderer's own case is in engine_primitives_test.
TEST_CASE("Q-14e a pass input the shader does not declare comes back as shader_input_mismatch",
          "[render-config-editor][q14e][rejection]") {
    RenderTargetHarness harness{true};
    const auto root_before = readBytes(harness.project.render_config);
    const auto refuse = [&](std::function<void()> thrower) {
        harness.runtime.validate_candidate =
            [thrower = std::move(thrower)](const RenderConfigCandidateDocumentSet &) { thrower(); };
        const auto accepted = harness.edit(addOp(sceneTarget("q14e_color")));
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        harness.service->commitPending();
        auto result = harness.service->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("status") == "rejected");
        CHECK(result.at("committed") == false);
        CHECK(result.at("error").at("code") == "render_pipeline_preflight_failed");
        CHECK(result.at("error").at("participant") == "pelican.render_pipeline");
        CHECK(readBytes(harness.project.render_config) == root_before);
        return result;
    };

    SECTION("a texture input at a binding the shader leaves empty") {
        const auto result = refuse([] {
            throw FullscreenInputBindingMismatch{FullscreenInputBindingMismatchInfo{
                .pass = "present",
                .fragment = "project://shaders/present",
                .kind = "texture",
                .binding = 0,
                .resource = "gbuffer_albedo",
                .expected = "sampled_image",
                .pass_texture_inputs = 1,
                .pass_buffer_inputs = 0,
            }};
        });
        const auto &error = result.at("error");
        CHECK(Json::parse(error.at("shader_input_mismatch").dump()) ==
              Json{{"pass", "present"},
                   {"fragment", "project://shaders/present"},
                   {"direction", "pass_input_undeclared"},
                   {"kind", "texture"},
                   {"binding", 0},
                   {"resource", "gbuffer_albedo"},
                   {"expected", "sampled_image"},
                   {"pass_texture_inputs", 1},
                   {"pass_buffer_inputs", 0},
                   {"shader_pass_inputs", Json::array()},
                   // VI-1: no variadic group.
                   {"variadic_range", nullptr}});
        CHECK(error.at("message") ==
              "Fullscreen pass input texture/local-read contract does not match shader "
              "reflection: pass 'present' input 0 ('gbuffer_albedo') expects a sampled image "
              "at set 1 binding 0, but fragment 'project://shaders/present' declares no pass "
              "input at that binding (shader declares 0 pass inputs)");
    }
    SECTION("a buffer input where the shader declares something else") {
        const auto result = refuse([] {
            throw FullscreenInputBindingMismatch{FullscreenInputBindingMismatchInfo{
                .kind = "buffer",
                .binding = 1,
                .expected = "storage_buffer",
                .pass_texture_inputs = 1,
                .pass_buffer_inputs = 1,
                .shader_pass_inputs = {{0, "sampled_image"}, {1, "uniform_buffer"}},
            }};
        });
        const auto &mismatch = result.at("error").at("shader_input_mismatch");
        CHECK(mismatch.at("direction") == "pass_input_undeclared");
        CHECK(mismatch.at("kind") == "buffer");
        CHECK(mismatch.at("binding") == 1);
        CHECK_FALSE(mismatch.contains("resource"));
        // VI-1: each declaration carries its name and whether it is variadic.
        CHECK(Json::parse(mismatch.at("shader_pass_inputs").dump()) ==
              Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", ""}, {"variadic", false}},
                           {{"binding", 1}, {"type", "uniform_buffer"}, {"name", ""}, {"variadic", false}}}));
        CHECK(result.at("error").at("message") ==
              "Fullscreen pass input buffer does not match shader reflection: input 1 "
              "expects a storage buffer at set 1 binding 1, but the shader declares a "
              "uniform buffer there (shader declares 2 pass inputs)");
    }
    SECTION("Q-14f: a declaration past the inputs the pass supplies") {
        const auto result = refuse([] {
            throw FullscreenInputBindingMismatch{FullscreenInputBindingMismatchInfo{
                .pass = "present",
                .fragment = "project://shaders/present",
                .direction = fullscreenInputShaderInputUnsupplied,
                .kind = "buffer",
                .binding = 1,
                .expected = "none",
                .pass_texture_inputs = 1,
                .pass_buffer_inputs = 0,
                .shader_pass_inputs = {{0, "sampled_image"}, {1, "storage_buffer"}},
            }};
        });
        const auto &error = result.at("error");
        CHECK(Json::parse(error.at("shader_input_mismatch").dump()) ==
              Json{{"pass", "present"},
                   {"fragment", "project://shaders/present"},
                   {"direction", "shader_input_unsupplied"},
                   {"kind", "buffer"},
                   {"binding", 1},
                   {"expected", "none"},
                   {"pass_texture_inputs", 1},
                   {"pass_buffer_inputs", 0},
                   {"shader_pass_inputs",
                    Json::array({{{"binding", 0}, {"type", "sampled_image"}, {"name", ""}, {"variadic", false}},
                                 {{"binding", 1}, {"type", "storage_buffer"}, {"name", ""}, {"variadic", false}}})},
                   {"variadic_range", nullptr}});
        CHECK(error.at("message") ==
              "Fullscreen shader declares a pass input the pass does not supply: pass "
              "'present' supplies 1 pass input, but fragment 'project://shaders/present' "
              "declares a storage buffer at set 1 binding 1 (shader declares 2 pass inputs)");
    }
    SECTION("Q-14f: with no pass named and no inputs supplied") {
        const auto result = refuse([] {
            throw FullscreenInputBindingMismatch{FullscreenInputBindingMismatchInfo{
                .direction = fullscreenInputShaderInputUnsupplied,
                .kind = "texture",
                .binding = 0,
                .expected = "none",
                .shader_pass_inputs = {{0, "input_attachment"}},
            }};
        });
        CHECK(result.at("error").at("shader_input_mismatch").at("direction") ==
              "shader_input_unsupplied");
        CHECK(result.at("error").at("message") ==
              "Fullscreen shader declares a pass input the pass does not supply: the pass "
              "supplies 0 pass inputs, but the shader declares an input attachment at set 1 "
              "binding 0 (shader declares 1 pass input)");
    }
    SECTION("any other preflight refusal carries no shader_input_mismatch") {
        const auto result = refuse([] {
            throw std::runtime_error(
                "Fullscreen pass input texture/local-read contract does not match shader reflection");
        });
        CHECK_FALSE(result.at("error").contains("shader_input_mismatch"));
    }
}

#if PELICAN_RUNTIME_SHADER_COMPILER
// Registered only where the runtime shader compiler is on: without it the
// forward default is refused by name (the G-4 test at the end of this file).
TEST_CASE("G-4 set_material_route_default rides the root-edit ticket and asks for a restart",
          "[render-config-editor][g4]") {
    const bool direct = GENERATE(true, false);
    INFO((direct ? "direct graph root" : "hybrid_v1 preset root"));
    RenderTargetHarness harness{direct};
    const auto set = [&](std::string value) {
        const auto accepted = harness.service->setMaterialRouteDefault(
            {{"base_source_digest", harness.digest()}, {"material_route_default", value}});
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        harness.service->commitPending();
        return harness.service->getResult({{"ticket", accepted.at("ticket")}});
    };
    const auto forward = set("forward");
    INFO(forward.dump());
    CHECK(forward.at("committed") == true);
    CHECK(forward.at("restart_required") == true);
    CHECK(g4RootRouteDefault(harness) == MaterialRouteDefault::forward);
    // The same value again changes no byte, so nothing needs a restart.
    const auto again = set("forward");
    INFO(again.dump());
    CHECK_FALSE(again.value("restart_required", false));
    const auto back = set("auto");
    INFO(back.dump());
    CHECK(back.at("restart_required") == true);
    CHECK(g4RootRouteDefault(harness) == MaterialRouteDefault::automatic);
    REQUIRE_THROWS(harness.service->setMaterialRouteDefault(
        {{"base_source_digest", harness.digest()}, {"material_route_default", 1}}));
}
#endif

#if !PELICAN_RUNTIME_SHADER_COMPILER
// Registered only where the runtime shader compiler is off: the engine could
// not build the forward version of its built-in materials, so the editor
// refuses the forward default by name before anything is written.
TEST_CASE("G-4 without the runtime shader compiler the forward default is refused by name",
          "[render-config-editor][g4][runtime-shader-compiler-off]") {
    RenderTargetHarness harness{true};
    harness.commit(Json::array({{{"op", "add"}, {"target", g1ForwardColor("g4_color")}},
                                {{"op", "add"}, {"target", g1Depth("g4_depth")}}}));
    const auto root_before = readBytes(harness.project.render_config);
    const auto added = g4AddForward(harness, "forward");
    INFO(added.dump());
    CHECK(added.at("status") == "rejected");
    CHECK(added.at("error").at("code") == "material_route_default_unavailable@1");
    CHECK(added.at("error").at("reason") == "runtime_shader_compiler_disabled");
    const auto set = harness.service->setMaterialRouteDefault(
        {{"base_source_digest", harness.digest()}, {"material_route_default", "forward"}});
    INFO(set.dump());
    CHECK(set.at("error").at("code") == "material_route_default_unavailable@1");
    harness.service->commitPending();
    CHECK(readBytes(harness.project.render_config) == root_before);
}
#endif

// --- G-6: an edit is composed by every graph variant the engine registered.

namespace {

// The Blank template's rendering document: present, reading nothing. A
// `pelican.graph` root, as the template writes it (MG-2b).
constexpr std::string_view g6BlankRoot = R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "rendering_passes": [
      {
        "name": "main_render",
        "passes": [
          {
            "name": "present",
            "type": "fullscreen",
            "input": [],
            "output": {"color": ["display"], "depth": null},
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "project://shaders/present"
            }
          }
        ]
      }
    ]
  }
}
)json";

// The engine's own preflight (the one editorruntimefactory installs), over
// the candidate document set.
std::function<void(const RenderConfigCandidateDocumentSet &)>
g6ProductionPreflight(PathResolver &resolver, bool xr_active) {
    return [&resolver, xr_active](
               const RenderConfigCandidateDocumentSet &documents) {
        preflightRenderGraphVariants(
            documents.rootDocument().bytes,
            [&resolver, &documents](std::string_view reference) {
                return loadRenderConfigCandidateText(documents, resolver,
                                                     reference);
            },
            true, xr_active);
    };
}

struct G6Harness {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    ActualCpuRenderRuntime runtime;
    std::unique_ptr<RenderConfigEditorService> service;

    explicit G6Harness(bool xr_active) {
        writeBytes(project.render_config, std::string{g6BlankRoot});
        resolver.setup(project.root, false);
        service = makeProjectService(
            project, resolver, runtime,
            g6ProductionPreflight(resolver, xr_active));
    }

    std::string digest() {
        return service->getRenderAuthoringContext(Json::object())
            .at("source_digest")
            .at("hex")
            .get<std::string>();
    }

    nlohmann::ordered_json targets(Json ops) {
        return service->editRenderTargets(
            {{"base_source_digest", digest()}, {"ops", std::move(ops)}});
    }

    nlohmann::ordered_json present(Json input) {
        return service->editPresetPass(
            {{"base_source_digest", digest()},
             {"graph", "main_render"},
             {"pass_name", "present"},
             {"input", std::move(input)},
             {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
             {"shader",
              {{"vertex", "engine://fullscreen"},
               {"fragment", "project://shaders/present"}}}});
    }

    nlohmann::ordered_json settle(const nlohmann::ordered_json &accepted) {
        if (accepted.value("status", std::string{}) != "accepted")
            return accepted;
        service->commitPending();
        return service->getResult({{"ticket", accepted.at("ticket")}});
    }
};

} // namespace

TEST_CASE("G-6 an edit preview cannot compose is refused before it takes a ticket, "
          "naming preview, and nothing is written",
          "[render-config-editor][g6][preflight][preview]") {
    auto history_target = sceneTarget("trail");
    history_target["history"] = true;

    // The same edits against an engine-side service without the variant
    // preflight: flat composes every one of them, the last included.
    {
        G6Harness flat_only{false};
        flat_only.service = makeProjectService(flat_only.project,
                                               flat_only.resolver,
                                               flat_only.runtime);
        REQUIRE(flat_only.settle(flat_only.targets(
                    addOp(sceneTarget("lit_color"))))
                    .at("committed") == true);
        REQUIRE(flat_only.settle(flat_only.targets(addOp(history_target)))
                    .at("committed") == true);
        const auto read = flat_only.settle(flat_only.present(
            Json::array({"lit_color", "trail@history"})));
        INFO(read.dump());
        REQUIRE(read.at("committed") == true);
    }

    G6Harness harness{false};
    REQUIRE(harness.settle(harness.targets(addOp(sceneTarget("lit_color"))))
                .at("committed") == true);
    REQUIRE(harness.settle(harness.present(Json::array({"lit_color"})))
                .at("committed") == true);
    // A history target is taken: preview leaves the fragment that declares
    // it out, and nothing preview keeps reads it.
    REQUIRE(harness.settle(harness.targets(addOp(history_target)))
                .at("committed") == true);

    // present reading last frame's trail is what preview cannot take.
    const auto root_before = readBytes(harness.project.render_config);
    const auto applies_before = harness.runtime.apply_calls;
    const auto refused =
        harness.present(Json::array({"lit_color", "trail@history"}));
    INFO(refused.dump());
    REQUIRE(refused.at("status") == "rejected");
    const auto &error = refused.at("error");
    CHECK(error.at("code") == "render_pipeline_preflight_failed");
    CHECK(error.at("variant") == "preview");
    CHECK(error.at("message").get<std::string>().find("history read") !=
          std::string::npos);
    CHECK_FALSE(refused.contains("ticket"));
    harness.service->commitPending();
    CHECK(harness.runtime.apply_calls == applies_before);
    CHECK(readBytes(harness.project.render_config) == root_before);
}

TEST_CASE("G-6 xr composes an edit only in an engine that started it",
          "[render-config-editor][g6][preflight][xr]") {
    auto history_target = sceneTarget("trail");
    history_target["history"] = true;

    // No XR session: a history target -- what a TAA-style project declares --
    // is an edit like any other.
    G6Harness flat{false};
    REQUIRE(flat.settle(flat.targets(addOp(history_target)))
                .at("committed") == true);

#if PELICAN_WITH_OPENXR
    // An engine that started XR composes xr too, and xr refuses history.
    G6Harness xr{true};
    REQUIRE(xr.settle(xr.targets(addOp(sceneTarget("lit_color"))))
                .at("committed") == true);
    const auto fragment_before = readBytes(
        xr.project.root / "passes" / "authoring" / "render-targets.json");
    const auto refused = xr.targets(addOp(history_target));
    INFO(refused.dump());
    REQUIRE(refused.at("status") == "rejected");
    CHECK(refused.at("error").at("code") ==
          "render_pipeline_preflight_failed");
    CHECK(refused.at("error").at("variant") == "xr");
    CHECK(refused.at("error").at("fragment_reference") ==
          renderTargetFragment);
    xr.service->commitPending();
    CHECK(readBytes(xr.project.root / "passes" / "authoring" /
                    "render-targets.json") == fragment_before);
#endif
}

} // namespace Pelican
