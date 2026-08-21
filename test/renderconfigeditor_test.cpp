#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/communication/renderconfigtransaction.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/renderconfigcandidate.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
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
        auto graphs = parseFrameGraphDefinitionsFromConfigJson(
            composition.config);
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
    std::function<RenderConfigEditorGateObservation()> gate = {}) {
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
                    const RenderConfigSourceCommit &commit) {
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
                           const RenderConfigSourceCommit &commit) {
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
                    auto composition = composeRenderFeatureConfig(
                        Json::parse(documents.rootDocument().bytes),
                        RenderFeatureComposeDependencies{
                            .load_feature_json = loader,
                            .runtime_shader_compiler_enabled =
                                runtime.runtime_shader_compiler_enabled,
                            .load_pipeline_json = loader,
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
                            const RenderConfigSourceCommit &commit) {
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
                    const RenderConfigSourceCommit &commit) {
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
