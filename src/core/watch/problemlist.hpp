#pragma once

#include "../container.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// PB-1: the engine's list of the problems that are true now -- what Studio's
// Problems window shows. A refusal the engine met on its own (at start, on a
// hot reload, on reload_render_documents) stays here until the documents it
// was met in are taken in again and committed; get_status.problems is its
// summary and get_problems the list.
//
// One problem:
//   {id, severity: "error" | "warning", source, code, message,
//    fields: {...the refusal's own fields},
//    document?, line?, column?, node?, port?, definition?, scope?,
//    document_kind?, at_frame?}
// `id` is a hash of what the problem is (code and where), not of when or how
// it was met: the same problem met again, from another source, has the same
// id. `document` is spelled project://<path> (CT-2). The members after it are
// copied out of `fields` when the refusal has them; nothing is guessed (a
// document that is not JSON has no node).
//
// An edit RPC's preflight refusal stays in its answer. A failure while the
// renderer applies a candidate is listed until a generation commits. EB-6: an
// edit RPC that wrote its documents but whose engine state cannot draw them
// yet (a material its pass does not declare what for, answered applied
// "unusable") is listed, source edit_rpc, until the engine takes it in.

// Where a problem was met.
inline constexpr std::string_view problemSourceStartup = "startup";
inline constexpr std::string_view problemSourceHotReload = "hot_reload";
inline constexpr std::string_view problemSourceReloadRpc = "reload_rpc";
inline constexpr std::string_view problemSourceEditRpc = "edit_rpc";

inline constexpr std::string_view problemSeverityError = "error";
inline constexpr std::string_view problemSeverityWarning = "warning";

// Which part of the project a problem's document is (Studio decides from it
// where the problem is fixed):
//   root            the rendering root, as text (it is not JSON)
//   root_graph      a node of the rendering root's graph
//   definition      a node inside a graph definition of the root (with
//                   `definition`, and `scope` when the refusal has one)
//   root_rendering  the root's rendering section
//   preset          the pipeline preset the root selects
//   feature         a render feature document the root (or its preset) enables
//   shader          a shader source file
//   material        a material document asset_data lists (FO-1), or a
//                   model's material bindings that name a material no
//                   registered document holds (fields.model)
//   scene           the scene document (EB-6: a scene object's own material
//                   no registered document holds, fields.object)
inline constexpr std::string_view problemKindRoot = "root";
inline constexpr std::string_view problemKindRootGraph = "root_graph";
inline constexpr std::string_view problemKindDefinition = "definition";
inline constexpr std::string_view problemKindRootRendering = "root_rendering";
inline constexpr std::string_view problemKindPreset = "preset";
inline constexpr std::string_view problemKindFeature = "feature";
inline constexpr std::string_view problemKindShader = "shader";
inline constexpr std::string_view problemKindMaterial = "material";
inline constexpr std::string_view problemKindScene = "scene";

// The owner of a set of problems: a new attempt of the same owner replaces
// its problems (a failure) or clears them (a commit).
inline constexpr std::string_view renderPipelineProblemDomain =
    "render_pipeline";
inline constexpr std::string_view shaderProblemDomain = "shaders";
// FO-1: the material documents refused at start and the bindings that kept
// their model's material for want of them (ProjectMaterialAssetContainer).
inline constexpr std::string_view materialProblemDomain = "materials";

// One "Shader compile failed: <path>\n<log>" the shader library wrote
// (shaderlibrary.cpp); a reload's error joins several with "; ".
struct ShaderCompileFailureText {
    std::string path;
    std::string log;
};
std::vector<ShaderCompileFailureText> shaderCompileFailures(
    std::string_view text);
// The first error of a compile log -- "<file>:<line>: error: <reason>" as
// glslang writes it, or a line that starts "ERROR: " -- with its file, line
// and the whole line; nullopt when the log has none.
struct ShaderLogError {
    std::string file;
    std::uint64_t line = 0;
    std::string text;
};
std::optional<ShaderLogError> firstShaderLogError(std::string_view log);

// The render documents of one rendering root, each spelled project://<path>
// (graphDocumentReference): what a refusal's document is matched against.
struct RenderDocumentRoles {
    std::string root;
    std::optional<std::string> preset;
    std::vector<std::string> features;
    // A shader file a compile failure names, as a document (project://...);
    // null or nullopt: not one of the project's.
    std::function<std::optional<std::string>(const std::string &)>
        shader_document;
};
// `root_bytes` may not be JSON (then only `root` is known). `load_document`
// reads the preset's bytes to find the features it enables; null or nullopt:
// none.
RenderDocumentRoles renderDocumentRoles(
    std::string_view root_bytes, std::string_view root_reference,
    const std::function<std::optional<std::string>(const std::string &)>
        &load_document);

// One problem from a rendering refusal {code, message, ...fields} -- the
// shape get_status.render_pipeline.refusal and an edit ticket's `error` have.
// Bookkeeping members of a ticket's error (participant, method, ticket) are
// not fields of the problem.
nlohmann::json renderPipelineProblem(
    std::string_view source, const nlohmann::json &refusal,
    const RenderDocumentRoles &roles,
    std::optional<std::uint64_t> at_frame = std::nullopt);

// A problem's id: 16 hex digits of the SHA-256 of its code and where it is
// (document, definition, scope, node, port, field, line, column, feature,
// module); the message too when none of those is known.
std::string problemId(const nlohmann::json &problem);

DECLARE_MODULE(ProblemList) {
  public:
    // Replaces every problem of `domain` with `problems` (each a whole
    // problem; an entry without `id` gets one). The revision moves only when
    // the list changes.
    void replace(std::string_view domain,
                 std::vector<nlohmann::json> problems);
    std::vector<nlohmann::json> problemsOf(std::string_view domain) const;
    void clear(std::string_view domain) { replace(domain, {}); }
    std::uint64_t revision() const;
    // get_status.problems: {revision, count, errors, warnings}.
    nlohmann::json statusJson() const;
    // get_problems: {revision, problems: [...]}, one entry per id.
    nlohmann::json problemsJson() const;

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<nlohmann::json>, std::less<>> domains_;
    std::uint64_t revision_ = 0;
};

// get_status.problems / get_problems of a host without the module (or before
// it exists): the empty list at revision 0.
nlohmann::json problemListStatusJson(const ProblemList *problems);
nlohmann::json problemListJson(const ProblemList *problems);

// PB-1: records a rendering refusal as the render pipeline's one problem
// (replacing what it had), when the ProblemList module exists. The root's
// bytes and the preset it selects are read from disk through PathResolver to
// tell the refusal's document_kind; `at_frame` is EngineTime's evaluation
// index when there is one.
void recordRenderPipelineProblem(std::string_view source,
                                 const nlohmann::json &refusal,
                                 std::string_view root_reference);
// The render pipeline's problems cleared (a committed reload or edit).
void clearRenderPipelineProblems();

// PB-1 stage 4: after a shader reload (the file watch's, source hot_reload,
// or reload_shaders', source reload_rpc), the shader problems are the shader
// files the library still holds as failed (`failed_documents`, each
// project://...), one problem each:
//   {code: "shader_reload_failed@1", document, document_kind: "shader",
//    line?, message: the log's first error line,
//    fields: {path: the file as the compiler named it, log}}
// A file this reload's `error_text` names gets a new problem; one it does not
// name keeps the problem it had. A file no longer failed is dropped.
void syncShaderProblems(std::string_view source,
                        const std::vector<std::string> &failed_documents,
                        std::string_view error_text);

} // namespace Pelican
