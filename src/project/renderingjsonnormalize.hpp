#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// RC-3b: the one normal form of rendering JSON (design_render_graph_ir.md
// §2.1). Every boundary JSON enters the rendering pipeline through -- the
// root document, a preset, a feature, a provider's response -- passes it
// through normalizeRenderingJson; the generators inside (graph lowering,
// canonical passes, snapshots, ImGui, templates) write it directly. Readers
// and rewriters after that take only the normal form (the RenderJson helpers
// below) and refuse anything else by name.
//
// The normal form, wherever a pass, a compute task or a buffer list stands
// (a config's rendering_passes[].passes[] and their family-local compute_tasks
// / buffers, compute_tasks[], buffers[]; a feature's passes[].pass,
// compute_tasks[], buffers[], pass_overrides.*.input; a preset's config):
//   - input / reads / writes / after / before: an array of strings
//   - output.color: an array of attachments {target, subresource?, load_op?,
//     store_op?}
//   - output.depth: null or one attachment
//   - buffers[]: objects ({"name": ...} for the short string form)
//   - load_op / store_op (attachment) and color_load_op / color_store_op /
//     depth_load_op / depth_store_op (pass): one spelling each, "clear" /
//     "load" / "dont_care" and "store" / "dont_care"
// Nothing else is changed: no key is added, a key the core parser rejects
// keeps its value for that parser to refuse.

// render.json_shape_invalid@1 {path, expected}: a value the normal form cannot
// be made from (output.color ["rt", 7], after ["producer", 7]). `path` is the
// value's JSON pointer in the document normalized.
inline constexpr std::string_view renderJsonShapeInvalidCode = "render.json_shape_invalid@1";
// render.json_not_normalized@1 {path}: a reader (or the check below) met a
// value in a form other than the normal one. An internal invariant: what
// reaches it skipped normalizeRenderingJson.
inline constexpr std::string_view renderJsonNotNormalizedCode = "render.json_not_normalized@1";

// The schema decides where passes stand: pelican.graph (its `rendering`
// section), pelican.render_feature, pelican.render_pipeline (its `config`),
// anything else a rendering config. Idempotent.
nlohmann::json normalizeRenderingJson(nlohmann::json document);
// The same walk over a feature document whatever its schema says (the
// public feature APIs take documents a caller read itself).
nlohmann::json normalizeRenderFeatureJson(nlohmann::json feature);
// Refuses (render.json_not_normalized@1, the first value's pointer) a
// document normalizeRenderingJson would change.
void requireNormalizedRenderingJson(const nlohmann::json &document);
// A provider's response (graph transform, subgraph replacement, render
// strategy) on its way to the core: normalized, then checked.
nlohmann::json normalizeProviderRenderingJson(nlohmann::json response);

// Tests only: while one is alive, normalizeRenderingJson returns its input
// unchanged (the checks stay), so a test can show what the readers do with
// JSON that skipped it.
class ScopedRenderingJsonNormalizationBypass {
  public:
    ScopedRenderingJsonNormalizationBypass() noexcept;
    ~ScopedRenderingJsonNormalizationBypass();
    ScopedRenderingJsonNormalizationBypass(const ScopedRenderingJsonNormalizationBypass &) = delete;
    ScopedRenderingJsonNormalizationBypass &operator=(const ScopedRenderingJsonNormalizationBypass &) = delete;
};

// The reading and rewriting of target references, on the normal form. A
// declaration is a pass or a compute task; what is not in the normal form is
// refused (render.json_not_normalized@1, path "<name>/<key>...").
namespace RenderJson {

// `key` of a declaration (input, reads, writes, after, before): absent is
// none.
std::vector<std::string> names(const nlohmann::json &declaration, std::string_view key);
bool namesContain(const nlohmann::json &declaration, std::string_view key, std::string_view name);
// Adds `name` to the list (created when absent) unless it is there.
void appendName(nlohmann::json &declaration, std::string_view key, const std::string &name);

// The targets of output.color, in order; of output.depth. No output, none.
std::vector<std::string> colorTargets(const nlohmann::json &pass);
std::optional<std::string> depthTarget(const nlohmann::json &pass);
// Color, then depth.
std::vector<std::string> outputTargets(const nlohmann::json &pass);
bool writesOutputTarget(const nlohmann::json &pass, std::string_view target);

// The names of a buffers[] list (absent is none).
std::vector<std::string> bufferNames(const nlohmann::json &holder);

// What `rename` returns for an attachment's target replaces it; its
// subresource and load / store stay. std::nullopt keeps the target.
using TargetRename = std::function<std::optional<std::string>(const std::string &)>;
void retargetColorOutputs(nlohmann::json &pass, const TargetRename &rename);
void retargetOutputs(nlohmann::json &pass, const TargetRename &rename);
// The same over a name list (input, reads, writes, after, before).
void retargetNames(nlohmann::json &declaration, std::string_view key, const TargetRename &rename);

// {"target": target}.
nlohmann::json attachment(std::string target);

} // namespace RenderJson

} // namespace Pelican
