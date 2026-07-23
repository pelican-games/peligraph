#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace Pelican {

// Identifies an immutable graph program. Backend lifecycle (for example an
// OpenXR session or a preview request) remains outside the project compiler.
enum class RenderPipelineGraphVariant {
    flat,
    preview,
    xr,
};

std::string_view renderPipelineGraphVariantName(
    RenderPipelineGraphVariant variant);

enum class GraphVariantHistoryPolicy {
    preserve,
    forbid,
};

enum class GraphVariantProjectionJitterPolicy {
    preserve,
    forbid,
};

enum class GraphVariantViewFamily {
    caller_defined,
    mono,
    stereo,
};

// RPE9 deliberately models only execution modes that the runtime can execute.
// Multiview will be added as a new capability and lowering result rather than
// being advertised as an alias for today's sequential stereo path.
enum class GraphVariantViewExecution {
    caller_defined,
    single_view,
    sequential,
};

enum class GraphVariantResourceLayout {
    shared_2d,
    sequential_2d,
};

enum class GraphVariantTerminal {
    presentation,
    request_local_capture,
    external_view,
};

enum class GraphVariantMirrorOutput {
    none,
    left_eye,
};

std::string_view graphVariantHistoryPolicyName(
    GraphVariantHistoryPolicy policy);
std::string_view graphVariantProjectionJitterPolicyName(
    GraphVariantProjectionJitterPolicy policy);
std::string_view graphVariantViewFamilyName(GraphVariantViewFamily family);
std::string_view graphVariantViewExecutionName(
    GraphVariantViewExecution execution);
std::string_view graphVariantResourceLayoutName(
    GraphVariantResourceLayout layout);
std::string_view graphVariantTerminalName(GraphVariantTerminal terminal);
std::string_view graphVariantMirrorOutputName(
    GraphVariantMirrorOutput output);

struct GraphVariantPolicyRequest {
    RenderPipelineGraphVariant variant =
        RenderPipelineGraphVariant::flat;
};

// These are compiler-mechanism capabilities, not live backend/session state.
// Their defaults describe mechanisms implemented by the current engine and
// make policy compilation usable in CPU-only tests.
struct GraphVariantPolicyCapabilities {
    bool request_local_capture = true;
    bool sequential_stereo = true;
    bool left_eye_mirror = true;
};

struct CompiledGraphVariantPolicy {
    RenderPipelineGraphVariant variant =
        RenderPipelineGraphVariant::flat;
    GraphVariantHistoryPolicy history =
        GraphVariantHistoryPolicy::preserve;
    GraphVariantProjectionJitterPolicy projection_jitter =
        GraphVariantProjectionJitterPolicy::preserve;
    GraphVariantViewFamily view_family =
        GraphVariantViewFamily::caller_defined;
    GraphVariantViewExecution view_execution =
        GraphVariantViewExecution::caller_defined;
    GraphVariantResourceLayout resource_layout =
        GraphVariantResourceLayout::shared_2d;
    GraphVariantTerminal terminal =
        GraphVariantTerminal::presentation;
    GraphVariantMirrorOutput mirror_output =
        GraphVariantMirrorOutput::none;
    // Zero means the caller supplies the view cardinality. Preview and XR
    // compile to exact non-zero cardinalities.
    std::uint32_t view_count = 0;
    std::string rendering_pass_name_suffix;

    bool operator==(const CompiledGraphVariantPolicy &) const = default;
};

CompiledGraphVariantPolicy compileGraphVariantPolicy(
    const GraphVariantPolicyRequest &request,
    const GraphVariantPolicyCapabilities &capabilities = {});

enum class GraphVariantFeatureDisposition {
    include,
    exclude,
    reject,
};

enum class GraphVariantFeatureReason {
    compatible,
    known_incompatible,
    unsafe_name,
    history,
    projection_jitter,
    velocity,
    user_interface,
    unsafe_surface,
};

std::string_view graphVariantFeatureDispositionName(
    GraphVariantFeatureDisposition disposition);
std::string_view graphVariantFeatureReasonName(
    GraphVariantFeatureReason reason);

struct GraphVariantFeatureDecision {
    std::string feature_name;
    GraphVariantFeatureDisposition disposition =
        GraphVariantFeatureDisposition::include;
    GraphVariantFeatureReason reason =
        GraphVariantFeatureReason::compatible;

    bool operator==(const GraphVariantFeatureDecision &) const = default;
};

// Pure classification of one already-validated feature envelope.
GraphVariantFeatureDecision decideGraphVariantFeature(
    const CompiledGraphVariantPolicy &policy,
    std::string_view feature_name,
    const nlohmann::json &feature);

// Converts a typed reject decision to the stable author-facing error used by
// the pipeline resolver. Calling this for include/exclude is an error.
std::string graphVariantFeatureRejectionMessage(
    const CompiledGraphVariantPolicy &policy,
    const GraphVariantFeatureDecision &decision);

// Applies only the structural rewrite selected by the compiled policy. The
// caller controls its position relative to generic normalization transforms.
void transformGraphVariantConfig(
    const CompiledGraphVariantPolicy &policy,
    nlohmann::json &config);

// Validates direct authored surfaces left after whole-feature decisions and
// structural transformation. It has no backend or module dependencies.
void validateGraphVariantConfig(
    const CompiledGraphVariantPolicy &policy,
    const nlohmann::json &config);

} // namespace Pelican
