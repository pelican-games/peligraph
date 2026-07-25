#include "../src/project/graphvariantpolicy.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace Pelican;
using Json = nlohmann::json;

Json feature(std::string name) {
    return {
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", std::move(name)},
    };
}

Json baseConfig(std::vector<std::string> features = {}) {
    return {
        {"features", std::move(features)},
        {"render_targets", Json::array()},
        {"rendering_passes",
         Json::array({
             {
                 {"name", "main"},
                 {"passes",
                  Json::array({
                      {
                          {"name", "present"},
                          {"type", "fullscreen"},
                          {"output",
                           {
                               {"color", "swapchain"},
                               {"depth", nullptr},
                           }},
                      },
                  })},
             },
         })},
    };
}

} // namespace

TEST_CASE("WP192 builtin graph variants compile to explicit execution contracts",
          "[wp192][graph-variant][policy]") {
    const auto flat = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::flat});
    REQUIRE(flat.history == GraphVariantHistoryPolicy::preserve);
    REQUIRE(flat.projection_jitter ==
            GraphVariantProjectionJitterPolicy::preserve);
    REQUIRE(flat.view_family ==
            GraphVariantViewFamily::caller_defined);
    REQUIRE(flat.view_execution ==
            GraphVariantViewExecution::caller_defined);
    REQUIRE(flat.resource_layout ==
            GraphVariantResourceLayout::shared_2d);
    REQUIRE(flat.terminal == GraphVariantTerminal::presentation);
    REQUIRE(flat.mirror_output ==
            GraphVariantMirrorOutput::none);
    REQUIRE(flat.view_count == 0);
    REQUIRE(flat.rendering_pass_name_suffix.empty());

    const auto preview = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::preview});
    REQUIRE(preview.history == GraphVariantHistoryPolicy::forbid);
    REQUIRE(preview.projection_jitter ==
            GraphVariantProjectionJitterPolicy::forbid);
    REQUIRE(preview.view_execution ==
            GraphVariantViewExecution::single_view);
    REQUIRE(preview.terminal ==
            GraphVariantTerminal::request_local_capture);
    REQUIRE(preview.view_count == 1);

#if PELICAN_WITH_OPENXR
    const auto xr = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::xr});
    REQUIRE(xr.history == GraphVariantHistoryPolicy::forbid);
    REQUIRE(xr.projection_jitter ==
            GraphVariantProjectionJitterPolicy::forbid);
    REQUIRE(xr.view_family == GraphVariantViewFamily::stereo);
    REQUIRE(xr.view_execution ==
            GraphVariantViewExecution::sequential);
    REQUIRE(xr.resource_layout ==
            GraphVariantResourceLayout::sequential_2d);
    REQUIRE(xr.terminal == GraphVariantTerminal::external_view);
    REQUIRE(xr.mirror_output ==
            GraphVariantMirrorOutput::left_eye);
    REQUIRE(xr.view_count == 2);
    REQUIRE(xr.rendering_pass_name_suffix == "#xr");
#else
    REQUIRE_THROWS_WITH(
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::xr}),
        "XR graph variant is unavailable in this build");
#endif
}

TEST_CASE("WP192 policy compilation rejects unavailable mechanisms without backend lifecycle",
          "[wp192][graph-variant][policy][cpu-only]") {
    GraphVariantPolicyCapabilities capabilities;

    capabilities.request_local_capture = false;
    REQUIRE_THROWS_WITH(
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::preview},
            capabilities),
        "preview graph variant requires request-local capture support");

#if PELICAN_WITH_OPENXR
    capabilities = {};
    capabilities.sequential_stereo = false;
    REQUIRE_THROWS_WITH(
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::xr},
            capabilities),
        "XR graph variant requires sequential stereo support");

    capabilities = {};
    capabilities.left_eye_mirror = false;
    REQUIRE_THROWS_WITH(
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::xr},
            capabilities),
        "XR graph variant requires left-eye mirror support");
#endif
}

TEST_CASE("WP192 graph feature decisions carry typed exclusion and rejection reasons",
          "[wp192][graph-variant][feature-decision]") {
    const auto flat = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::flat});
    const auto preview = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::preview});
#if PELICAN_WITH_OPENXR
    const auto xr = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::xr});
#endif

    auto history = feature("custom_trails");
    history["render_targets"] = Json::array({
        {{"name", "trail_history"}, {"history", true}},
    });
    auto motion = feature("custom_motion");
    motion["render_targets"] = Json::array({
        {{"name", "motion_vectors"}},
    });

    REQUIRE(decideGraphVariantFeature(flat, "custom_trails", history)
                .disposition ==
            GraphVariantFeatureDisposition::include);

    const auto preview_history =
        decideGraphVariantFeature(
            preview, "custom_trails", history);
    REQUIRE(preview_history.disposition ==
            GraphVariantFeatureDisposition::exclude);
    REQUIRE(preview_history.reason ==
            GraphVariantFeatureReason::history);

#if PELICAN_WITH_OPENXR
    const auto xr_taa =
        decideGraphVariantFeature(xr, "taa", history);
    REQUIRE(xr_taa.disposition ==
            GraphVariantFeatureDisposition::exclude);
    REQUIRE(xr_taa.reason ==
            GraphVariantFeatureReason::known_incompatible);

    const auto xr_history =
        decideGraphVariantFeature(
            xr, "custom_trails", history);
    REQUIRE(xr_history.disposition ==
            GraphVariantFeatureDisposition::reject);
    REQUIRE(xr_history.reason ==
            GraphVariantFeatureReason::history);
    REQUIRE(graphVariantFeatureRejectionMessage(
                xr, xr_history) ==
            "OpenXR activation rejected history feature 'custom_trails'");

    const auto xr_motion =
        decideGraphVariantFeature(
            xr, "custom_motion", motion);
    REQUIRE(xr_motion.disposition ==
            GraphVariantFeatureDisposition::exclude);
    REQUIRE(xr_motion.reason ==
            GraphVariantFeatureReason::velocity);
#endif
}

TEST_CASE("WP192 preview transform and validation are selected by typed policy",
          "[wp192][graph-variant][preview]") {
    const auto policy = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{
            RenderPipelineGraphVariant::preview});
    auto config = baseConfig();
    transformGraphVariantConfig(policy, config);
    REQUIRE(config.dump().find("swapchain") == std::string::npos);
    REQUIRE(config.dump().find("preview_capture") !=
            std::string::npos);
    REQUIRE_NOTHROW(validateGraphVariantConfig(policy, config));

    config.at("rendering_passes")
        .at(0)
        .at("passes")
        .push_back({
            {"name", "authored_velocity"},
            {"type", "velocity"},
        });
    REQUIRE_THROWS_WITH(
        validateGraphVariantConfig(policy, config),
        "preview graph 'main' rejects authored pass 'authored_velocity': unsafe pass type/name velocity");
}

TEST_CASE("WP192 resolver owns XR and preview policy without injected callbacks",
          "[wp192][graph-variant][resolve][cpu-only]") {
    const std::unordered_map<std::string, Json> features{
        {"fixture://safe", feature("safe")},
        {"fixture://ui", feature("ui")},
    };
    const auto load = [&features](std::string_view ref) {
        return features.at(std::string{ref}).dump();
    };
    const auto authored =
        baseConfig({"fixture://safe", "fixture://ui"});

#if PELICAN_WITH_OPENXR
    const auto xr = resolveRenderPipeline(
        RenderPipelineRequest{authored, "CPU-only XR fixture"},
        RenderEnvironmentCapabilities{
            true, RenderPipelineGraphVariant::xr},
        RenderPipelineResolveDependencies{
            .load_feature_json = load,
        });
    REQUIRE(xr.graph_variant_policy.view_execution ==
            GraphVariantViewExecution::sequential);
    REQUIRE(xr.normalized_config.at("rendering_passes")
                .at(0)
                .at("name") == "main#xr");
    REQUIRE(xr.feature_names == std::vector<std::string>{"safe"});
    REQUIRE(xr.excluded_feature_names ==
            std::vector<std::string>{"ui"});
    REQUIRE(xr.graph_variant_feature_decisions ==
            std::vector<GraphVariantFeatureDecision>{
                {
                    "ui",
                    GraphVariantFeatureDisposition::exclude,
                    GraphVariantFeatureReason::known_incompatible,
                },
            });
#endif

    const auto preview = resolveRenderPipeline(
        RenderPipelineRequest{authored, "CPU-only preview fixture"},
        RenderEnvironmentCapabilities{
            true, RenderPipelineGraphVariant::preview},
        RenderPipelineResolveDependencies{
            .load_feature_json = load,
        });
    REQUIRE(preview.graph_variant_policy.terminal ==
            GraphVariantTerminal::request_local_capture);
    REQUIRE(preview.normalized_config.dump().find("swapchain") ==
            std::string::npos);
    REQUIRE(preview.normalized_config.dump().find(
                "preview_capture") != std::string::npos);
}

TEST_CASE("WP203 XR target policy separates authoring preference from the logical graph variant",
          "[wp203][xr][target-policy]") {
    const auto defaults = compileXrTargetPolicy(
        Json::object(), RenderPipelineGraphVariant::xr);
    REQUIRE(defaults.active);
    REQUIRE_FALSE(defaults.authored);
    REQUIRE(defaults.view_execution ==
            XrViewExecutionPreference::automatic);

    const auto declaration = Json{
        {"xr", {{"view_execution", "multiview"}}},
    };
    const auto xr = compileXrTargetPolicy(
        declaration, RenderPipelineGraphVariant::xr);
    REQUIRE(xr.active);
    REQUIRE(xr.authored);
    REQUIRE(xr.view_execution ==
            XrViewExecutionPreference::require_multiview);
    REQUIRE(xrTargetPolicyToJson(xr).at("view_execution") ==
            "multiview");

    const auto flat = compileXrTargetPolicy(
        declaration, RenderPipelineGraphVariant::flat);
    REQUIRE_FALSE(flat.active);
    REQUIRE(flat.authored);
    REQUIRE(flat.view_execution ==
            XrViewExecutionPreference::require_multiview);

    REQUIRE_THROWS_WITH(
        compileXrTargetPolicy(
            Json{{"xr", {{"view_execution", "both"}}}},
            RenderPipelineGraphVariant::xr),
        "rendering config xr has unknown view_execution: both");
    REQUIRE_THROWS_WITH(
        compileXrTargetPolicy(
            Json{{"xr", {{"surprise", true}}}},
            RenderPipelineGraphVariant::flat),
        "rendering config xr has unknown key 'surprise'");
}

TEST_CASE("WP203c XR multiview auto profiles resolve measured device evidence",
          "[wp203c][xr][multiview][device-profile]") {
    const auto declaration = Json{
        {"xr",
         {{"view_execution", "auto"},
          {"multiview_auto",
           {{"minimum_gain_percent", 2.0},
            {"profiles",
             Json::array({
                 {
                     {"id", "vendor_fallback"},
                     {"vendor_id", 4318},
                     {"measurement",
                      {
                          {"sequential_gpu_ms", 8.0},
                          {"multiview_gpu_ms", 8.4},
                          {"sample_count", 240},
                          {"source", "gpu_timestamp/vendor"},
                      }},
                 },
                 {
                     {"id", "exact_graph_win"},
                     {"vendor_id", 4318},
                     {"device_id", 9860},
                     {"device_name_contains", "mock gpu"},
                     {"graph", "main#xr"},
                     {"measurement",
                      {
                          {"sequential_gpu_ms", 8.0},
                          {"multiview_gpu_ms", 6.0},
                          {"sample_count", 300},
                          {"source", "gpu_timestamp/exact"},
                      }},
                 },
             })}}}}},
    };
    const auto policy = compileXrTargetPolicy(
        declaration,
        RenderPipelineGraphVariant::xr);
    REQUIRE(policy.multiview_auto_authored);
    REQUIRE(policy.multiview_auto
                .minimum_gain_percent == 2.0);
    REQUIRE(policy.multiview_auto.profiles.size() == 2);

    const XrMultiviewDeviceIdentity exact{
        .vendor_id = 4318,
        .device_id = 9860,
        .driver_version = 7,
        .device_name = "Mock GPU Ultra",
    };
    const auto beneficial =
        resolveXrMultiviewAutoPolicy(
            policy.multiview_auto, exact,
            "main#xr");
    REQUIRE(beneficial.matched_profile);
    REQUIRE(beneficial.profile_id ==
            "exact_graph_win");
    REQUIRE(beneficial.selection ==
            XrMultiviewAutoSelection::multiview);
    REQUIRE(beneficial.measured_gain_percent ==
            25.0);

    auto other = exact;
    other.device_id = 1;
    const auto regression =
        resolveXrMultiviewAutoPolicy(
            policy.multiview_auto, other,
            "main#xr");
    REQUIRE(regression.profile_id ==
            "vendor_fallback");
    REQUIRE(regression.selection ==
            XrMultiviewAutoSelection::sequential);
    REQUIRE(regression.measurement
                ->sample_count == 240);

    other.vendor_id = 1234;
    const auto defaulted =
        resolveXrMultiviewAutoPolicy(
            policy.multiview_auto, other,
            "main#xr");
    REQUIRE_FALSE(defaulted.matched_profile);
    REQUIRE(defaulted.profile_id ==
            "optimize_by_default");
    REQUIRE(defaulted.selection ==
            XrMultiviewAutoSelection::multiview);

    const auto encoded =
        xrTargetPolicyToJson(policy);
    REQUIRE(encoded.at("multiview_auto")
                .at("authored") == true);
    REQUIRE(encoded.at("multiview_auto")
                .at("policy")
                .at("profiles")
                .size() == 2);

    REQUIRE_THROWS_WITH(
        compileXrTargetPolicy(
            Json{
                {"xr",
                 {{"multiview_auto",
                   {{"profiles",
                     Json::array({
                         {
                             {"id", "invalid"},
                             {"vendor_id", 4318},
                             {"measurement",
                              {
                                  {"sequential_gpu_ms", 8.0},
                                  {"multiview_gpu_ms", 7.0},
                                  {"sample_count", 0},
                                  {"source", "gpu_timestamp"},
                              }},
                         },
                     })}}}}},
            },
            RenderPipelineGraphVariant::xr),
        "rendering config xr multiview_auto profile "
        "'invalid' measurement sample_count must be "
        "greater than zero");
}

#if PELICAN_WITH_OPENXR
TEST_CASE("WP203 pipeline preset settings carry an authored XR target preference",
          "[wp203][xr][target-policy][preset]") {
    const auto preset = Json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "xr_policy_fixture"},
        {"config", baseConfig()},
    };
    const auto authored = Json{
        {"pipeline",
         {{"preset", "fixture://xr_policy"},
          {"settings",
           {{"xr", {{"view_execution", "sequential"}}}}}}},
    };
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "XR policy preset"},
        RenderEnvironmentCapabilities{
            true, RenderPipelineGraphVariant::xr},
        RenderPipelineResolveDependencies{
            .load_pipeline_json =
                [&preset](std::string_view reference) {
                    if (reference != "fixture://xr_policy") {
                        throw std::runtime_error(
                            "unexpected pipeline reference");
                    }
                    return preset.dump();
                },
        });
    REQUIRE(resolved.xr_target_policy.active);
    REQUIRE(resolved.xr_target_policy.authored);
    REQUIRE(resolved.xr_target_policy.view_execution ==
            XrViewExecutionPreference::sequential);

    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(serializeCompiledRenderPipelineMetadata(compiled)
                .at("xr_target_policy")
                .at("view_execution") == "sequential");
}
#endif
