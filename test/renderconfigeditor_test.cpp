#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/renderconfigeditor.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
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
    bool module_creation_frozen = false;
    std::set<std::string, std::less<>> initialized_modules;

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
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
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

    Prepared prepare(std::string_view bytes) const {
        auto composition = composeRenderFeatureConfig(
            Json::parse(bytes),
            RenderFeatureComposeDependencies{
                .load_feature_json = loadEngineDocument,
                .runtime_shader_compiler_enabled =
                    PELICAN_RUNTIME_SHADER_COMPILER != 0,
                .load_pipeline_json = loadEngineDocument,
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
        auto prepared = prepare(bytes);
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
        std::string bytes,
        const RenderConfigSourceCommit &source_commit) {
        ++apply_calls;
        Prepared prepared;
        try {
            prepared = prepare(bytes);
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
                    std::string candidate,
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

} // namespace Pelican
