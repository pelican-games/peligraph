#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/communication/proceduralasseteditor.hpp"
#include "../src/core/communication/renderconfigtransaction.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/renderconfigcandidate.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/project/vulkanviewplanning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
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
        const auto root =
            std::filesystem::temp_directory_path();
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
            directory_ = std::filesystem::temp_directory_path() /
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
    ActualCpuRenderRuntime &runtime) {
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
                    };
                },
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

std::string emptyHybridConfig() {
    return R"json({
  "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
  "features": []
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
    config["compute_tasks"] = Json::array({task});
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
        {"nodes", Json::array()}, {"outputs", Json::object()}, {"rendering", Json::parse(emptyHybridConfig())}};
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

    SECTION("the operation catalog reaches the editor over the same service") {
        const auto catalog = service->getGraphCatalog(Json::object());
        REQUIRE(catalog.at("schema") == "pelican.graph_catalog");
        std::vector<std::string> namespaces;
        for (const auto &module : catalog.at("modules"))
            namespaces.push_back(module.at("namespace").get<std::string>());
        CHECK(containsName(namespaces, "pelican.value"));
        CHECK(containsName(namespaces, "pelican.audio"));
        CHECK(containsName(namespaces, "pelican.render"));
#if PELICAN_WITH_ANIMATION
        CHECK(containsName(namespaces, "pelican.pose"));
        CHECK(namespaces.size() == 4);
#else
        CHECK(namespaces.size() == 3);
#endif
    }
}

TEST_CASE("scene AS edits use render publication and source CAS", "[render-config-editor][rt7][scene-as]") {
    TemporaryConfig source{emptyHybridConfig()};
    const auto original = readBytes(source.path);
    ActualCpuRenderRuntime runtime;
    runtime.initialize(original);
    bool gate_open = true;
    auto service = makeService(source, runtime, [&] {
        return RenderConfigEditorGateObservation{.can_edit = gate_open};
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
        CHECK(Json::parse(readBytes(source.path)).at("ray_task_callable_records") == Json::object());
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
        CHECK(Json::parse(readBytes(source.path)).at("ray_task_acceleration_structures") == Json::object());
    }
    SECTION("add modify remove and reopen") {
        auto accepted = submit(definitions);
        REQUIRE(accepted.at("status") == "accepted");
        CHECK(readBytes(source.path) == original);
        service->commitPending();
        REQUIRE(service->getResult({{"ticket", accepted.at("ticket")}}).at("committed") == true);
        CHECK(service->getSceneAccelerationStructures(Json::object()).at("definitions") == definitions);
        CHECK(Json::parse(readBytes(source.path)).at("scene_acceleration_structures") == definitions);
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
            CHECK(Json::parse(readBytes(source.path)).at("scene_acceleration_structures") == value);
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
    root["ray_group_layout"] = "ray_layout.json";
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
            root["scene_acceleration_structures"] = definitions;
            root["ray_task_acceleration_structures"] = Json::object();
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
            root["scene_acceleration_structures"] = definitions;
            root["ray_task_acceleration_structures"] = Json::object();
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
    SECTION("version two without AS settings gains only default AS settings and the chosen space") {
        layout["version"] = 2;
        layout.erase("acceleration_structure");
        layout["groups"][0].erase("type");
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
    const std::string original =
        "{\r\n  \"features\" : [ \"keep\" ],\r\n"
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
        const auto source = AuthoredRenderConfigDocument::inspect("{\r\n\t\"keep\" : 1\r\n}\r\n");
        const auto edited = source.withRootValue("scene_acceleration_structures", " [] ");
        CHECK(edited.bytes() == "{\r\n\t\"keep\" : 1,\r\n\t\"scene_acceleration_structures\": []\r\n}\r\n");
        CHECK_FALSE(edited.hasFeaturesArray());
    }
    SECTION("empty object and escaped key") {
        const std::string key = "quoted\"\\\nkey";
        const auto edited = AuthoredRenderConfigDocument::inspect("{ }").withRootValue(key, "42");
        CHECK(Json::parse(edited.bytes()).at(key) == 42);
        CHECK(edited.bytes() == "{ " + Json(key).dump() + ": 42}");
    }
    SECTION("duplicate non-feature keys keep existing last-wins contract") {
        const auto source = AuthoredRenderConfigDocument::inspect("{\"x\":1, \"x\":2}");
        CHECK(source.withRootValue("x", "3").bytes() == "{\"x\":1, \"x\":3}");
        CHECK_THROWS_AS(AuthoredRenderConfigDocument::inspect("{\"features\":[],\"features\":[]}"), std::invalid_argument);
    }
    SECTION("invalid or multiple JSON values reject without changing source") {
        for (const auto value : {"", "[", "false true", "{\"x\":}", "NaN"}) {
            CAPTURE(value);
            CHECK_THROWS_AS(document.withRootValue("scene_acceleration_structures", value), std::invalid_argument);
        }
        CHECK_THROWS_AS(document.withRootValue("features", "null"), std::invalid_argument);
        CHECK(document.bytes() == original);
    }
}

TEST_CASE(
    "WP335 RPC startup is read-only and the first requested edit initializes features",
    "[render-config-editor][wp335][rpc][byte-lossless][initialization]") {
    const std::string original =
        "{\r\n"
        "  \"pipeline\" : { \"preset\" : \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "  \"shader_defines\" : [ \"WP334A_KEPT\" ]\r\n"
        "}\r\n";
    const std::string initialized =
        "{\r\n"
        "  \"pipeline\" : { \"preset\" : \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "  \"shader_defines\" : [ \"WP334A_KEPT\" ],\r\n"
        "  \"features\": []\r\n"
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
        "  \"features\" : [\r\n"
        "    " + object_entry + ",\r\n"
        "    \"" + std::string{editable_old} + "\"\r\n"
        "  ],\r\n"
        "  \"sentinel\" : \"WP335_KEEP\"\r\n"
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
    const auto semantic_after_add = Json::parse(bytes_after_add);
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
    const auto semantic_after_remove = Json::parse(bytes_after_remove);
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
        "{\n  \"features\" : [ \"" + std::string{editable_old} +
        "\" ],\n  \"sentinel\" : \"WP335_STRINGS\"\n}\n";
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
    const auto strings_semantic = Json::parse(readBytes(strings_source.path));
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
        "  \"pipeline\": { \"preset\": \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "  \"shader_defines\": [ \"WP331_KEPT\" ],\r\n"
        "  \"features\": []\r\n"
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
        "  \"pipeline\": { \"preset\": \"engine://render_pipelines/hybrid_v1.json\" },\r\n"
        "  \"shader_defines\": [ \"WP331_KEPT\" ],\r\n"
        "  \"features\": [\"" +
        std::string{buildAvailableReference} +
        "\"]\r\n"
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
            root["features"][0] = {{"ref", "project://passes/shadow-source.json"}, {"parameters", {{"color_r", 0.7}}}};
            root["features"].erase(1); // The selected local sky replaces the engine sky instance.
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
                preset["config"]["features"] = root.at("features");
                root["features"] = Json::array();
            }
            writeBytes(preset_path, preset.dump(2));
            const auto preset_before = readBytes(preset_path);
            root["pipeline"]["preset"] = "project://passes/editable.json";
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
                CHECK_FALSE(saved.contains("parameters"));
                CHECK(saved.at("bound_parameters").at("color_r") == 0.7);
                const auto owner = Json::parse(readBytes(owner_path));
                CHECK((scope == "root" ? owner : owner.at("config")).at("features").at(0) == "project://passes/shadow-local.json");
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
    CHECK(Json::parse(readBytes(project.render_config))
              .at("features")
              .at(index) == "project://passes/bloom-local.json");
    CHECK(readBytes(engine_source) == engine_source_before);
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
        expected_root["pipeline"]["preset"] = request.at("destination_reference");
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
        local_root["pipeline"]["preset"] = "project://passes/source-preset.json";
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
    root["features"].push_back("project://passes/order_tasks.json");
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

TEST_CASE("Compute task editing resolves direct roots and project presets",
          "[render-config-editor][compute-task-edit]") {
    for (const bool use_preset : {false, true}) {
        CAPTURE(use_preset);
        TemporaryAnimgraphProject project;
        PathResolver resolver;
        resolver.setup(project.root, false);
        auto preset = Json::parse(resolver.loadText("engine://render_pipelines/hybrid_v1.json"));
        const Json task{{"name", "owned_compute"}, {"shader", "project://shaders/probe"},
                        {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}};
        preset["config"]["compute_tasks"] = Json::array({task});
        const auto preset_path = project.root / "passes" / "owned.json";
        writeBytes(preset_path, preset.dump(2));
        writeBytes(project.render_config, use_preset
            ? Json{{"pipeline", {{"preset", "project://passes/owned.json"}}}}.dump(2)
            : preset.at("config").dump(2));
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
        if (use_preset) REQUIRE(readBytes(project.render_config) == root_before);
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
        if (use_preset) REQUIRE(readBytes(project.render_config) == root_before);
    }
}

TEST_CASE("Procedural asset creation upgrades an empty asset document atomically",
          "[render-config-editor][procedural-asset-create]") {
    TemporaryAnimgraphProject project;
    PathResolver resolver;
    resolver.setup(project.root, false);
    const auto path = project.root / "created_assets.json";
    const Json original{{"schema", "pelican.asset_data"}, {"version", 1}, {"models", Json::array()}};
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
    root["features"].push_back("project://passes/compute.json");
    writeBytes(project.render_config, root.dump(2));
    const auto root_before = readBytes(project.render_config);
    const auto source_before = readBytes(source_path);
    ActualCpuRenderRuntime runtime;
    auto service = makeProjectService(project, resolver, runtime);
    const auto catalog = service->getComputeTaskAuthoring(Json::object());
    REQUIRE(catalog.at("tasks").size() == 1);
    const auto &selected = catalog.at("tasks").at(0);
    REQUIRE(selected.at("editable") == true);
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
    root["pipeline"]["preset"] = "project://passes/editable.json";
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
    root["pipeline"]["preset"] = "project://passes/editable.json";
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
    const auto index = root.at("features").size();
    const std::string reference = "project://passes/parameters.json";
    root["features"].push_back(reference);
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
            root["features"][index] = {{"ref", reference}, {"parameters", parameters}};
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
        root["features"].erase(index);
        root["pipeline"]["preset"] = "project://passes/parameter_preset.json";
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
        root["features"].push_back("project://passes/editable_feature.json");
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
        root["pipeline"]["preset"] = "project://passes/auxiliary.json";
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
    root["pipeline"]["preset"] = "project://passes/raster.json";
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
        for (const auto &entry : root.at("features")) {
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
        root.at("features"), [&](const auto &feature) {
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
        root.at("features"), [&](const auto &feature) {
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
    writeBytes(transaction / "manifest.json",
               manifest("committed").dump(2) + "\n");
    recoverRenderConfigDocumentTransaction(project.root);
    REQUIRE(readBytes(project.render_config) == candidate_root);
    REQUIRE(readBytes(fragment_path) == fragment);
    REQUIRE_FALSE(std::filesystem::exists(transaction));
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

} // namespace Pelican
