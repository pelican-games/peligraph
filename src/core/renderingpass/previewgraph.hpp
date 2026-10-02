#pragma once

#include "../../project/graphvariantpolicy.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct CompiledRenderPipeline;

// A third, startup-compiled graph program.  Unlike flat/xr RenderingPassId it
// is data-only: render_preview executes it against request-local resources and
// therefore never enters Renderer::renderLogicalFrame.
struct PreviewGraphProgram {
    std::string name = "preview";
    std::uint64_t generation = 0;
    std::vector<std::string> pass_names;
    std::vector<std::string> excluded_feature_names;
    // The same immutable compiler provenance and typed policy used by the
    // runtime variants. Legacy standalone precompile callers also populate
    // this, but only the coordinated family path carries program provenance.
    std::shared_ptr<const CompiledRenderPipeline>
        render_pipeline;
    CompiledGraphVariantPolicy graph_variant_policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::preview});
    nlohmann::json composed_config = nlohmann::json::object();
    // PV-1: false when the preview variant could not compose an authoring
    // the runtime variants published (an authored history target, say).
    // Normal play runs; render_preview refuses as preview.unavailable@1 with
    // `unavailable_reason`, previewUnsupportedAuthoringReason's object. The
    // program is otherwise empty (generation 0, no pipeline).
    bool available = true;
    nlohmann::json unavailable_reason = nullptr;
};

// PV-1: the reason a preview could not be composed, from the refusal:
//   {code: "preview.unsupported_authoring@1", feature, target?, graph?,
//    pass?, cause?, message}
// `feature` is PreviewUnsupportedAuthoringError's kind (history,
// projection_jitter, unsafe_target_name, unsafe_pass, history_read,
// history_write); "composition" for any other refusal, which a named one
// (a GraphRegistryError or a carried "<code>: <fields>") adds as
// cause: {code, ...its fields}. `message` is the refusal's own text.
nlohmann::json previewUnsupportedAuthoringReason(
    const std::exception_ptr &refusal);
// PV-1: a preview that is not available for `reason`.
PreviewGraphProgram unavailablePreviewGraphProgram(nlohmann::json reason);
// PV-1: {available: true} or {available: false, reason}, as get_status,
// get_frame_plan and a committed render edit report it.
nlohmann::json previewGraphStatusJson(const PreviewGraphProgram &program);

PreviewGraphProgram makePreviewGraphProgram(
    std::shared_ptr<const CompiledRenderPipeline>
        render_pipeline,
    nlohmann::json composed_config);

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled);

// G-6: an edit's candidate, composed by every runtime graph variant the
// engine registered when it started: flat always, xr only when this launch
// activated it (xr refuses history features outright, so composing it for an
// engine that never opened a session would refuse every edit of a TAA
// project). PV-1: preview is not composed here. A candidate only the
// one-shot preview cannot take is an edit like any other; the published
// generation then reports preview as unavailable.
//
// The flat composition comes first and is the baseline: a candidate flat
// cannot compose is left to the caller's own flat path, which reports it in
// its own words. Once flat composes, a variant that cannot is raised as
// GraphVariantCompositionError naming that variant.
void preflightRenderGraphVariants(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled, bool xr_active);

} // namespace Pelican
