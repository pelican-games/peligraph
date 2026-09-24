#include "../src/project/graphvariantpolicy.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

TEST_CASE("WP206b material variant selection survives every graph variant rewrite",
          "[wp206b][material-variant][graph-variant]") {
    const auto source = [] {
        auto config = baseConfig();
        auto &passes =
            config.at("rendering_passes")
                .at(0)
                .at("passes");
        passes.insert(
            passes.begin(),
            Json{
                {"name", "silhouette_overlay"},
                {"type", "material"},
                {"material_contract", "forward_opaque_v1"},
                {"material_filter",
                 {{"include", {"outlined"}}}},
                {"material_variant", "silhouette"},
            });
        return config;
    }();

    auto variants = std::vector{
        RenderPipelineGraphVariant::flat,
        RenderPipelineGraphVariant::preview,
    };
#if PELICAN_WITH_OPENXR
    variants.push_back(RenderPipelineGraphVariant::xr);
#endif
    for (const auto variant : variants) {
        const auto variant_name =
            std::string{renderPipelineGraphVariantName(variant)};
        CAPTURE(variant_name);
        const auto policy = compileGraphVariantPolicy(
            GraphVariantPolicyRequest{variant});
        auto transformed = source;
        transformGraphVariantConfig(policy, transformed);
        const auto &passes =
            transformed.at("rendering_passes")
                .at(0)
                .at("passes");
        const auto selected = std::find_if(
            passes.begin(), passes.end(),
            [](const auto &pass) {
                return pass.value("name", "") ==
                       "silhouette_overlay";
            });
        REQUIRE(selected != passes.end());
        REQUIRE(selected->at("material_variant") ==
                "silhouette");
        REQUIRE(selected->at("material_filter")
                    .at("include") ==
                Json::array({"outlined"}));
        REQUIRE(selected->at("material_contract") ==
                "forward_opaque_v1");
        REQUIRE_NOTHROW(
            validateGraphVariantConfig(
                policy, transformed));
    }
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
        RenderPipelineRequest{Pelican::TestSupport::renderGraphRoot(authored), "CPU-only XR fixture"},
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
        RenderPipelineRequest{Pelican::TestSupport::renderGraphRoot(authored), "CPU-only preview fixture"},
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
        RenderPipelineRequest{Pelican::TestSupport::renderGraphRoot(authored), "XR policy preset"},
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

// --- G-6: preview judges a feature by its passes, and an exclusion is carried
// to the features that use what it took away.

namespace {

Json g6Target(std::string name, bool history = false) {
    Json target{
        {"name", std::move(name)},
        {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"},
        {"format_class", "scene"},
        {"role", "color"},
        {"sample_count", 1},
        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
    };
    if (history) target["history"] = true;
    return target;
}

Json g6Pass(std::string name, Json input, Json color) {
    return {
        {"name", name},
        {"type", "fullscreen"},
        {"input", std::move(input)},
        {"output", {{"color", std::move(color)}, {"depth", nullptr}}},
        {"shader",
         {{"vertex", "shaders/fullscreen"},
          {"fragment", "shaders/" + name}}},
    };
}

Json g6Feature(std::string name, std::string insert, Json pass,
               Json targets = Json::array()) {
    auto result = feature(std::move(name));
    result["passes"] = Json::array({
        {{"insert", std::move(insert)}, {"pass", std::move(pass)}},
    });
    if (!targets.empty()) result["render_targets"] = std::move(targets);
    return result;
}

GraphVariantFeatureDecision g6Decision(
    const CompiledGraphVariantPolicy &policy, const Json &feature_json) {
    return decideGraphVariantFeature(
        policy, feature_json.at("name").get<std::string>(), feature_json);
}

// Resolves `features` (in that order) for one variant; the refs are the
// feature names.
ResolvedRenderPipeline g6Resolve(RenderPipelineGraphVariant variant,
                                 const std::vector<Json> &features) {
    std::unordered_map<std::string, Json> documents;
    std::vector<std::string> refs;
    for (const auto &entry : features) {
        const auto name = entry.at("name").get<std::string>();
        documents.emplace(name, entry);
        refs.push_back(name);
    }
    return resolveRenderPipeline(
        RenderPipelineRequest{Pelican::TestSupport::renderGraphRoot(baseConfig(refs)),
                              "G-6 fixture"},
        RenderEnvironmentCapabilities{true, variant},
        RenderPipelineResolveDependencies{
            .load_feature_json =
                [documents](std::string_view reference) {
                    return documents.at(std::string{reference}).dump();
                },
        });
}

const GraphVariantFeatureDecision *g6Excluded(
    const ResolvedRenderPipeline &resolved, std::string_view name) {
    const auto found = std::find_if(
        resolved.graph_variant_feature_decisions.begin(),
        resolved.graph_variant_feature_decisions.end(),
        [&](const auto &decision) {
            return decision.feature_name == name;
        });
    return found == resolved.graph_variant_feature_decisions.end()
               ? nullptr
               : &*found;
}

bool g6Included(const ResolvedRenderPipeline &resolved,
                std::string_view name) {
    return std::find(resolved.feature_names.begin(),
                     resolved.feature_names.end(),
                     name) != resolved.feature_names.end();
}

// The G-5 story B in miniature: a G-buffer pass placed before present, and a
// lighting pass placed after it that reads its targets.
std::vector<Json> g6DeferredLoop() {
    return {
        g6Feature("g6_geometry", "before:present",
                  g6Pass("deferred_geometry", Json::array(),
                         Json::array({"gbuffer_albedo"})),
                  Json::array({g6Target("gbuffer_albedo"),
                               g6Target("lit_color")})),
        g6Feature("g6_lighting", "after:deferred_geometry",
                  g6Pass("deferred_lighting",
                         Json::array({"gbuffer_albedo"}),
                         Json::array({"lit_color"}))),
    };
}

} // namespace

TEST_CASE("G-6 preview judges a feature by its passes, not by where it is inserted",
          "[g6][graph-variant][preview][feature-decision]") {
    const auto preview = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{RenderPipelineGraphVariant::preview});
    const auto include = [&](const Json &feature_json) {
        const auto decision = g6Decision(preview, feature_json);
        CAPTURE(feature_json.dump());
        REQUIRE(decision.disposition ==
                GraphVariantFeatureDisposition::include);
    };
    const auto exclude = [&](const Json &feature_json,
                             GraphVariantFeatureReason reason) {
        const auto decision = g6Decision(preview, feature_json);
        CAPTURE(feature_json.dump());
        REQUIRE(decision.disposition ==
                GraphVariantFeatureDisposition::exclude);
        REQUIRE(decision.reason == reason);
    };

    // A place next to present is only a place.
    include(g6Feature("g6_geometry", "before:present",
                      g6Pass("deferred_geometry", Json::array(),
                             Json::array({"gbuffer_albedo"}))));
    include(g6Feature("g6_late_lighting", "after:present",
                      g6Pass("deferred_lighting",
                             Json::array({"gbuffer_albedo"}),
                             Json::array({"lit_color"}))));

    // A pass that presents, or draws the UI, straight to the screen is out.
    exclude(g6Feature("g6_overlay", "after:present",
                      g6Pass("final_present", Json::array(), "swapchain")),
            GraphVariantFeatureReason::unsafe_surface);
    auto hud = g6Pass("hud", Json::array(), "display");
    hud["type"] = "ui";
    exclude(g6Feature("g6_hud", "end", hud),
            GraphVariantFeatureReason::user_interface);
    exclude(g6Feature("g6_on_top", "after:pelican_ui",
                      g6Pass("on_top", Json::array(), "swapchain")),
            GraphVariantFeatureReason::user_interface);
    exclude(g6Feature("g6_beneath", "before:pelican_ui",
                      g6Pass("beneath", Json::array(), "swapchain")),
            GraphVariantFeatureReason::user_interface);

    // Words, not pieces of words: "ui" is not in fluid or build, and
    // "present" is not in presentation.
    include(g6Feature("fluid_sim", "before:present",
                      g6Pass("build_grid", Json::array(),
                             Json::array({"fluid_density"})),
                      Json::array({g6Target("fluid_density")})));
    include(g6Feature("g6_presentation", "before:present",
                      g6Pass("presentation_blur", Json::array(),
                             Json::array({"blurred"}))));
    exclude(g6Feature("debug_ui", "before:present",
                      g6Pass("debug_panel", Json::array(),
                             Json::array({"panel"}))),
            GraphVariantFeatureReason::unsafe_name);

    // Composed together, the lighting pass finds its anchor in preview as it
    // does in flat.
    const auto flat = g6Resolve(RenderPipelineGraphVariant::flat,
                                g6DeferredLoop());
    REQUIRE(g6Included(flat, "g6_lighting"));
    const auto resolved = g6Resolve(RenderPipelineGraphVariant::preview,
                                    g6DeferredLoop());
    REQUIRE(g6Included(resolved, "g6_geometry"));
    REQUIRE(g6Included(resolved, "g6_lighting"));
    REQUIRE(resolved.excluded_feature_names.empty());
}

TEST_CASE("G-6 a feature that uses what an excluded feature declared is excluded with it",
          "[g6][graph-variant][preview][xr][feature-decision][dependency]") {
    // "taa" is excluded by preview (its name) and by XR (known). Each later
    // feature is fine by itself and uses taa's pass or target one way.
    const auto taa = g6Feature(
        "taa", "before:present",
        g6Pass("taa_resolve", Json::array({"taa_history@history"}),
               Json::array({"taa_history"})),
        Json::array({g6Target("taa_history", true)}));
    const auto anchored = g6Feature(
        "g6_anchored", "after:taa_resolve",
        g6Pass("sharpen", Json::array({"taa_history"}),
               Json::array({"sharpened"})),
        Json::array({g6Target("sharpened")}));
    auto edged_pass = g6Pass("edged_pass", Json::array(),
                             Json::array({"edged"}));
    edged_pass["after"] = Json::array({"taa_resolve"});
    const auto edged =
        g6Feature("g6_edged", "before:present", edged_pass,
                  Json::array({g6Target("edged")}));
    const auto reader = g6Feature(
        "g6_reader", "before:present",
        g6Pass("reader_pass", Json::array({"taa_history"}),
               Json::array({"read_out"})),
        Json::array({g6Target("read_out")}));
    // Second order: it depends on g6_anchored, which only depended on taa.
    const auto second = g6Feature(
        "g6_second", "after:sharpen",
        g6Pass("second_pass", Json::array({"sharpened"}),
               Json::array({"second_out"})),
        Json::array({g6Target("second_out")}));
    const auto independent = g6Feature(
        "g6_independent", "before:present",
        g6Pass("independent_pass", Json::array(),
               Json::array({"independent_out"})),
        Json::array({g6Target("independent_out")}));
    const std::vector<Json> features{taa,    anchored, edged,
                                     reader, second,   independent};

    std::vector<RenderPipelineGraphVariant> variants{
        RenderPipelineGraphVariant::preview};
#if PELICAN_WITH_OPENXR
    variants.push_back(RenderPipelineGraphVariant::xr);
#endif
    for (const auto variant : variants) {
        const auto variant_name =
            std::string{renderPipelineGraphVariantName(variant)};
        CAPTURE(variant_name);

        // One by one, each is fine...
        const auto policy =
            compileGraphVariantPolicy(GraphVariantPolicyRequest{variant});
        for (const auto &entry : {anchored, edged, reader, independent}) {
            REQUIRE(g6Decision(policy, entry).disposition ==
                    GraphVariantFeatureDisposition::include);
        }

        // ...and in the list, the ones that use taa go with it.
        const auto resolved = g6Resolve(variant, features);
        REQUIRE(g6Included(resolved, "g6_independent"));
        for (const auto &[name, depends_on] :
             std::vector<std::pair<std::string, std::string>>{
                 {"g6_anchored", "taa"},
                 {"g6_edged", "taa"},
                 {"g6_reader", "taa"},
                 {"g6_second", "g6_anchored"},
             }) {
            CAPTURE(name);
            REQUIRE_FALSE(g6Included(resolved, name));
            const auto *decision = g6Excluded(resolved, name);
            REQUIRE(decision != nullptr);
            REQUIRE(*decision ==
                    GraphVariantFeatureDecision{
                        name,
                        GraphVariantFeatureDisposition::exclude,
                        GraphVariantFeatureReason::dependency,
                        depends_on,
                    });
            REQUIRE(std::any_of(
                resolved.diagnostics.begin(), resolved.diagnostics.end(),
                [&](const auto &diagnostic) {
                    return diagnostic.kind ==
                               RenderPipelineDiagnosticKind::
                                   feature_excluded &&
                           diagnostic.subject == name &&
                           diagnostic.detail ==
                               variant_name + ":dependency:" + depends_on;
                }));
        }
    }

    // Flat excludes nothing, so nothing is carried.
    const auto flat =
        g6Resolve(RenderPipelineGraphVariant::flat, features);
    REQUIRE(flat.excluded_feature_names.empty());
    REQUIRE(std::string{graphVariantFeatureReasonName(
                GraphVariantFeatureReason::dependency)} == "dependency");
}
