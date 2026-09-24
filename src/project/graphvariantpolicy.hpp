#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
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
    // G-6: the feature is fine by itself but uses a pass or a declared
    // resource of a feature this variant excluded.
    dependency,
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
    // Reason `dependency` only: the excluded feature whose pass or resource
    // this one anchors on, orders against, overrides, reads or writes.
    std::string depends_on;

    bool operator==(const GraphVariantFeatureDecision &) const = default;
};

// Pure classification of one already-validated feature envelope, by what the
// feature itself declares. It knows nothing of the other features; the
// selection below adds what they take with them.
GraphVariantFeatureDecision decideGraphVariantFeature(
    const CompiledGraphVariantPolicy &policy,
    std::string_view feature_name,
    const nlohmann::json &feature);

// G-6: one variant's decisions over a whole features[] list, asked in that
// list's order (the order composition adds them). A feature the variant
// excludes takes its passes, compute tasks, render targets and buffers with
// it. A later feature that is fine by itself is excluded too, with reason
// `dependency` and `depends_on`, when it
//  (a) inserts a pass before/after one of those passes,
//  (b) names one in a pass's explicit `after` / `before` edge, or overrides
//      one, or
//  (c) reads or writes one of those resources through a pass's `input` /
//      `output`, or overrides one.
// What it excludes this way is carried forward the same way, so the closure
// is taken in one pass and is deterministic. The flat variant excludes
// nothing, so it never has anything to carry.
class GraphVariantFeatureSelection {
  public:
    explicit GraphVariantFeatureSelection(
        CompiledGraphVariantPolicy policy);

    GraphVariantFeatureDecision decide(
        std::string_view feature_name,
        const nlohmann::json &feature);

  private:
    void carryExcluded(const std::string &feature_name,
                       const nlohmann::json &feature);
    [[nodiscard]] const std::string *usedExcludedFeature(
        const nlohmann::json &feature) const;

    CompiledGraphVariantPolicy policy_;
    // Pass / compute task name -> the excluded feature that declared it.
    std::map<std::string, std::string, std::less<>> excluded_passes_;
    // Render target / buffer name -> the excluded feature that declared it.
    std::map<std::string, std::string, std::less<>> excluded_resources_;
};

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

// G-6: a candidate that one graph variant could not compose, raised by an
// edit preflight that composes every variant the engine registered. The
// message is the composer's own; the variant says which program refused it.
class GraphVariantCompositionError : public std::runtime_error {
  public:
    GraphVariantCompositionError(RenderPipelineGraphVariant variant,
                                 const std::string &message)
        : std::runtime_error{message}, variant_{variant} {}

    [[nodiscard]] RenderPipelineGraphVariant variant() const noexcept {
        return variant_;
    }

  private:
    RenderPipelineGraphVariant variant_;
};

} // namespace Pelican
