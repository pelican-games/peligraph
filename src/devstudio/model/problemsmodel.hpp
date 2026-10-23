#pragma once

#include "assetsdockmodel.hpp"
#include "materialsmodel.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// PB-1s. The Problems window: what is wrong with the project now, one row a
// problem -- which document, which line, what is wrong and where Studio fixes
// it -- gathered from two places:
//
//   - the engine's list (engine PB-1): get_problems {revision, problems}, read
//     again only when get_status.problems.revision moves. A refusal the
//     engine met on its own -- at start, on a reload of a file saved outside
//     Studio, a shader that does not compile -- stays there until the
//     documents are taken in again. An edit's refusal is not in it: the panel
//     that sent the edit says it where it was made.
//   - the contents check of the Assets dock (AV-1s): its warnings and errors.
//
// One problem of the engine:
//   {id, severity, source, code, message, fields, document?, line?, column?,
//    node?, port?, definition?, scope?, document_kind?, at_frame?}
// document_kind says which part of the project the document is (root,
// root_graph, definition, root_rendering, preset, feature, shader; material
// and scene once the engine lists them), and with node / line where it is
// fixed (ProblemRow::Place).
struct ProblemRow {
    enum class Severity { Error, Warning };
    enum class Origin { Engine, Assets };
    // Where a click on the row goes (decision 3).
    enum class Place {
        None,
        // The Graph tab, the node selected (inside its definition): the Graph
        // holds the document on disk, at start and after a refused reload
        // (RR-1s, decision 3).
        GraphNode,
        // The Materials dock, the material selected.
        Material,
        // The document's text (BS-1s's Show the document), at the line (a
        // document that is not JSON, or a problem without a node).
        DocumentText,
        // The shader file, in the desktop's editor.
        ShaderFile,
        // The Assets dock, the file's row.
        AssetFile,
        // The Rendering settings tab (the root's rendering section).
        RenderingSettings,
        // The Features tab.
        Features,
        // Only a restart of the engine fixes it.
        Restart,
        // RTL-3s, decision 8 (engine FO-2): a pass of the frame plan on the
        // Graph tab -- a preset's or a feature's folded pass, or the node of
        // the root's graph that made it -- picked, its fields open. `pass`.
        GraphPass,
        // EB-6s, decision 5 (engine EB-6): a scene object whose own material
        // no registered material holds -- the object picked in the Outliner,
        // its Material row in the Inspector. `object`, `object_name`.
        SceneObject,
    };

    std::string id;
    Severity severity = Severity::Error;
    Origin origin = Origin::Engine;
    // startup, hot_reload, reload_rpc, edit_rpc (the engine's: EB-6, a
    // material written that its pass cannot draw yet); "assets" for the check.
    std::string source;
    std::string code;
    std::string message;
    nlohmann::json fields = nlohmann::json::object();
    // As the project names it ("passes/main.json"); an engine document keeps
    // its engine://.
    std::string document;
    std::string document_kind;
    std::int64_t line = 0;
    std::int64_t column = 0;
    std::string node;
    std::string port;
    std::string definition;
    std::string scope;

    // What is wrong: the code's sentence from the tables every panel reads
    // refusals with, else the engine's first line.
    std::string sentence;
    Place place = Place::None;
    // Where it is fixed, as the row says it: "Graph: sim", "Materials:
    // brick", "Text", "Rendering settings", "Restart the engine".
    std::string place_text;
    // Place::Restart: the feature only a restart turns on (decision 6).
    std::string restart_feature;
    // Place::Material: the material's name.
    std::string material;
    // FO-1s, decision 2: Place::Material for a model whose bindings name a
    // material no loaded document holds (material.binding_unresolved@1).
    std::string model;
    // Place::AssetFile: the file on disk.
    std::filesystem::path asset_path;
    // Place::GraphPass: the pass's authored name (fields.pass).
    std::string pass;
    // Place::SceneObject: the authoring object id (fields.object), its name
    // when it has one, and its scene.
    std::uint64_t object = 0;
    std::string object_name;
    std::string scene_id;

    [[nodiscard]] bool error() const noexcept { return severity == Severity::Error; }
    // "12:5", "12", or empty without a line.
    [[nodiscard]] std::string lineText() const;
};

// One problem of get_problems as a row; nullopt when it is not an object.
[[nodiscard]] std::optional<ProblemRow> readEngineProblem(const nlohmann::json &problem);
// The rows of the Assets dock's contents check (warnings and errors only).
[[nodiscard]] std::vector<ProblemRow> assetProblemRows(const std::vector<AssetStoreVerification> &stores);
// "2 errors, 1 warning", "1 error", "No problems".
[[nodiscard]] std::string problemsCountLine(std::size_t errors, std::size_t warnings);
// MR-1s, decisions 2 and 3: what the rows say of materials, as the Materials
// dock reads it -- every material row's id and sentence (a change means the
// engine took a document in or refused one), and the registered documents the
// engine's watch refused after start (source hot_reload).
[[nodiscard]] MaterialProblems materialProblemsOf(const std::vector<ProblemRow> &rows);

// The list the Problems window shows, and the reads it takes. Qt-free: the
// owner hands it every get_status reply, the replies of what it sent, and the
// Assets dock's check; it says `changed` when the rows did.
class ProblemsSession {
  public:
    using Json = nlohmann::json;

    struct Transport {
        // Sends one request; the id its reply comes back under, 0 when it
        // could not be sent.
        std::function<std::int64_t(const std::string &method, const Json &params, std::string *error)> request;
    };

    explicit ProblemsSession(Transport transport);

    // Every get_status reply, whoever asked. get_problems is read when
    // problems.revision is not the one last read (decision 2).
    void observeStatus(const Json &status);
    // Something may have changed the list (a reload answered, a document
    // restored): get_problems now. One read is in flight at a time; one asked
    // for meanwhile follows it.
    void readNow();
    void engineReady();
    void engineGone();
    // The Assets dock's check (AV-1s). True when the rows changed.
    bool setAssetIssues(const std::vector<AssetStoreVerification> &stores);

    [[nodiscard]] bool owns(std::int64_t id) const noexcept { return id != 0 && id == request_; }
    // The replies to get_problems. False for an id that is not its own.
    bool receiveResult(std::int64_t id, const Json &result);
    bool receiveFailure(std::int64_t id, const std::string &message);

    // Engine rows first (in the engine's order), then the assets' rows.
    [[nodiscard]] const std::vector<ProblemRow> &rows() const noexcept { return rows_; }
    [[nodiscard]] const ProblemRow *find(const std::string &id) const;
    [[nodiscard]] std::size_t errors() const noexcept;
    [[nodiscard]] std::size_t warnings() const noexcept;
    // Whether the engine answers get_problems (an engine before PB-1 does
    // not: its get_status has no problems).
    [[nodiscard]] bool engineLists() const noexcept { return engine_lists_; }
    // The revision of the list last read; nullopt before the first.
    [[nodiscard]] const std::optional<std::uint64_t> &revision() const noexcept { return revision_; }
    // Why the list could not be read, empty while it could.
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }

    // The rows changed.
    std::function<void()> changed;

  private:
    Transport transport_;
    bool ready_ = false;
    bool engine_lists_ = false;
    std::int64_t request_ = 0;
    bool again_ = false;
    std::optional<std::uint64_t> revision_;
    std::vector<ProblemRow> engine_rows_;
    std::vector<ProblemRow> asset_rows_;
    std::vector<ProblemRow> rows_;
    std::string notice_;

    void compose();
};

} // namespace PelicanStudio
