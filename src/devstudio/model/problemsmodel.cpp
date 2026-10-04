#include "problemsmodel.hpp"

#include "frameplanmodel.hpp"
#include "inspectormodel.hpp"
#include "materialsmodel.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string stringOf(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::int64_t numberOf(const Json &object, const char *key) {
    if (!object.is_object()) return 0;
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    if (found->is_number_integer()) return found->get<std::int64_t>();
    if (found->is_number_unsigned()) return static_cast<std::int64_t>(found->get<std::uint64_t>());
    return 0;
}

std::optional<std::uint64_t> revisionOf(const Json &object) {
    if (!object.is_object()) return std::nullopt;
    const auto found = object.find("revision");
    if (found == object.end()) return std::nullopt;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer() && found->get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(found->get<std::int64_t>());
    return std::nullopt;
}

std::string fileNameOf(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string withoutFullStop(std::string sentence) {
    while (!sentence.empty() && (sentence.back() == '.' || sentence.back() == ' ' || sentence.back() == '\n'))
        sentence.pop_back();
    return sentence;
}

// What is wrong, in one sentence: the tables every panel reads refusals with
// (the document is the row's own column, so it is not named again), the
// engine's first line for a code Studio has no sentence for.
std::string problemSentence(const ProblemRow &row) {
    Json error = row.fields.is_object() ? row.fields : Json::object();
    error["code"] = row.code;
    error["message"] = row.message;
    if (row.code == RenderFeatureRequiresRestartCode) {
        const auto feature = renderRestartFeature(error);
        auto module = stringOf(row.fields, "module");
        return "The rendering setup turns on " + feature + ", whose runtime module " +
               (module.empty() ? std::string{"it needs"} : module) +
               " this engine was started without; restart the engine (" + row.code + ").";
    }
    if (row.code == "shader_reload_failed@1") {
        auto file = stringOf(row.fields, "path");
        if (file.empty()) file = row.document;
        file = fileNameOf(file);
        if (file.empty()) file = "The shader";
        auto first = row.message;
        if (const auto newline = first.find('\n'); newline != std::string::npos) first.erase(newline);
        return file + " does not compile" + (first.empty() ? std::string{} : ": " + first) + " (" + row.code + ").";
    }
    // FO-1s, decision 2 (engine FO-1): a model whose bindings name a material
    // of a document the engine did not load.
    if (row.code == "material.binding_unresolved@1") {
        std::string materials;
        if (const auto names = row.fields.find("materials"); names != row.fields.end() && names->is_array())
            for (const auto &name : *names)
                if (name.is_string()) materials += (materials.empty() ? "" : ", ") + name.get<std::string>();
        const auto model = stringOf(row.fields, "model");
        return (model.empty() ? std::string{"A model"} : modelShortName(model)) + " binds " +
               (materials.empty() ? std::string{"a material"} : materials) +
               ", which no loaded material document holds; it draws with the model's own material (" + row.code +
               ").";
    }
    if (row.code == "material_route_unserved@1") {
        if (auto notice = materialRouteUnservedNotice(error); !notice.empty())
            return withoutFullStop(std::move(notice)) + " (" + row.code + ").";
    }
    if (auto syntax = renderDocumentSyntaxDescription(error); !syntax.empty()) return syntax;
    if (auto graph = graphLoopRefusalDescription(error, false); !graph.empty()) return graph;
    if (auto known = knownRefusalDescription(row.code, row.message, false); !known.empty()) return known;
    if (auto material = materialRefusalDescription(error); !material.empty()) return material;
    auto sentence = row.message;
    if (const auto newline = sentence.find('\n'); newline != std::string::npos) sentence.erase(newline);
    if (sentence.empty()) return row.code;
    if (!row.code.empty() && sentence.find(row.code) == std::string::npos) sentence += " (" + row.code + ")";
    return sentence;
}

// Where it is fixed (decision 3): a node on the Graph tab, a material in the
// Materials dock, a line of the document's text, the shader's file; the
// rendering section's settings or the Features tab when nothing finer is
// named; a restart when only a restart turns a feature on.
void placeOf(ProblemRow &row) {
    using Place = ProblemRow::Place;
    const auto &kind = row.document_kind;
    Json error = row.fields.is_object() ? row.fields : Json::object();
    error["code"] = row.code;
    if (auto feature = renderRestartFeature(error); !feature.empty()) {
        row.place = Place::Restart;
        row.restart_feature = std::move(feature);
        row.place_text = "Restart the engine";
        return;
    }
    if (kind == "shader") {
        row.place = Place::ShaderFile;
        row.place_text = "Shader file";
        return;
    }
    if (kind == "material" || row.code.starts_with("material.")) {
        auto name = stringOf(row.fields, "name");
        if (name.empty()) name = stringOf(row.fields, "material");
        if (!name.empty() || !row.document.empty()) {
            row.place = Place::Material;
            row.material = name;
            // FO-1s, decision 2: a document the engine did not load is its
            // row in the Materials dock.
            row.place_text = !name.empty()           ? "Materials: " + name
                             : !row.document.empty() ? "Materials: " + fileNameOf(row.document)
                                                     : std::string{"Materials"};
            return;
        }
        // FO-1s, decision 2 (engine FO-1): a model whose bindings name a
        // material no loaded document holds is fixed on the model's row.
        if (auto model = stringOf(row.fields, "model"); !model.empty()) {
            row.place = Place::Material;
            row.place_text = "Materials: " + modelShortName(model);
            row.model = std::move(model);
            return;
        }
    }
    if (!row.node.empty() && (kind.empty() || kind == "root_graph" || kind == "definition")) {
        const auto node = row.definition.empty() ? row.node : row.definition + " / " + row.node;
        // The Graph holds the document as it is on disk: the one the engine
        // was given at start, and -- RR-1s, decision 3 (engine RR-1) -- the
        // one a reload refused, which an edit there is based on.
        row.place = Place::GraphNode;
        row.place_text = "Graph: " + node;
        return;
    }
    if (row.line > 0 && !row.document.empty()) {
        row.place = Place::DocumentText;
        row.place_text = "Text";
        return;
    }
    if (kind == "root_rendering") {
        row.place = Place::RenderingSettings;
        row.place_text = "Rendering settings";
        return;
    }
    if (kind == "feature") {
        row.place = Place::Features;
        row.place_text = "Features";
        return;
    }
    if (!row.document.empty() && !row.document.starts_with("engine://")) {
        row.place = Place::DocumentText;
        row.place_text = "Text";
        return;
    }
    row.place = Place::None;
}

} // namespace

std::string ProblemRow::lineText() const {
    if (line <= 0) return {};
    return column > 0 ? std::to_string(line) + ":" + std::to_string(column) : std::to_string(line);
}

std::optional<ProblemRow> readEngineProblem(const Json &problem) {
    if (!problem.is_object()) return std::nullopt;
    ProblemRow row;
    row.origin = ProblemRow::Origin::Engine;
    row.id = stringOf(problem, "id");
    row.severity = stringOf(problem, "severity") == "warning" ? ProblemRow::Severity::Warning
                                                              : ProblemRow::Severity::Error;
    row.source = stringOf(problem, "source");
    row.code = stringOf(problem, "code");
    row.message = stringOf(problem, "message");
    if (const auto fields = problem.find("fields"); fields != problem.end() && fields->is_object())
        row.fields = *fields;
    row.document = graphRefusalSource(problem, "document");
    row.document_kind = stringOf(problem, "document_kind");
    row.line = numberOf(problem, "line");
    row.column = numberOf(problem, "column");
    row.node = stringOf(problem, "node");
    row.port = stringOf(problem, "port");
    row.definition = stringOf(problem, "definition");
    row.scope = stringOf(problem, "scope");
    // An engine without ids still has rows Studio can tell apart.
    if (row.id.empty())
        row.id = row.code + "|" + row.document + "|" + row.node + "|" + row.port + "|" + row.lineText();
    row.sentence = problemSentence(row);
    placeOf(row);
    return row;
}

std::vector<ProblemRow> assetProblemRows(const std::vector<AssetStoreVerification> &stores) {
    std::vector<ProblemRow> rows;
    for (const auto &store : stores)
        for (const auto &issue : store.issues) {
            if (issue.severity != "warning" && issue.severity != "error") continue;
            ProblemRow row;
            row.origin = ProblemRow::Origin::Assets;
            row.source = "assets";
            row.severity =
                issue.severity == "error" ? ProblemRow::Severity::Error : ProblemRow::Severity::Warning;
            row.code = issue.kind;
            row.message = issue.message;
            row.document = issue.file;
            row.id = "assets|" + store.name + "|" + issue.kind + "|" + issue.file;
            row.sentence = assetIssueKindLabel(issue.kind);
            if (!row.sentence.empty())
                row.sentence[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(row.sentence[0])));
            row.sentence += " against " + (store.manifest.empty() ? store.name + "'s manifest"
                                                                  : store.manifest.filename().string());
            if (!issue.message.empty()) row.sentence += ": " + issue.message;
            row.asset_path = issue.physical;
            row.place = issue.physical.empty() ? ProblemRow::Place::None : ProblemRow::Place::AssetFile;
            row.place_text = "Assets: " + fileNameOf(issue.file);
            rows.push_back(std::move(row));
        }
    return rows;
}

std::string problemsCountLine(std::size_t errors, std::size_t warnings) {
    if (errors == 0 && warnings == 0) return "No problems";
    const auto counted = [](std::size_t count, const char *word) {
        return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
    };
    if (warnings == 0) return counted(errors, "error");
    if (errors == 0) return counted(warnings, "warning");
    return counted(errors, "error") + ", " + counted(warnings, "warning");
}

ProblemsSession::ProblemsSession(Transport transport) : transport_{std::move(transport)} {}

void ProblemsSession::observeStatus(const Json &status) {
    if (!ready_ || !status.is_object()) return;
    const auto problems = status.find("problems");
    // An engine before PB-1 says nothing: there is nothing to read.
    if (problems == status.end() || !problems->is_object()) return;
    engine_lists_ = true;
    const auto revision = revisionOf(*problems);
    if (!revision) return;
    if (revision_ && *revision_ == *revision) return;
    readNow();
}

void ProblemsSession::readNow() {
    if (!ready_) return;
    if (request_ != 0) {
        again_ = true;
        return;
    }
    std::string error;
    request_ = transport_.request ? transport_.request("get_problems", Json::object(), &error) : 0;
    if (request_ == 0 && !error.empty()) notice_ = "Could not read the engine's problems: " + error;
}

void ProblemsSession::engineReady() {
    ready_ = true;
    engine_lists_ = false;
    request_ = 0;
    again_ = false;
    revision_.reset();
    notice_.clear();
}

void ProblemsSession::engineGone() {
    ready_ = false;
    engine_lists_ = false;
    request_ = 0;
    again_ = false;
    revision_.reset();
    notice_.clear();
    if (engine_rows_.empty()) return;
    engine_rows_.clear();
    compose();
}

bool ProblemsSession::setAssetIssues(const std::vector<AssetStoreVerification> &stores) {
    auto rows = assetProblemRows(stores);
    const auto same = std::ranges::equal(rows, asset_rows_, [](const ProblemRow &a, const ProblemRow &b) {
        return a.id == b.id && a.sentence == b.sentence && a.severity == b.severity;
    });
    if (same) return false;
    asset_rows_ = std::move(rows);
    compose();
    return true;
}

bool ProblemsSession::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    request_ = 0;
    notice_.clear();
    std::vector<ProblemRow> rows;
    if (result.is_object()) {
        if (const auto revision = revisionOf(result)) revision_ = *revision;
        if (const auto problems = result.find("problems"); problems != result.end() && problems->is_array())
            for (const auto &problem : *problems) {
                auto row = readEngineProblem(problem);
                if (!row) continue;
                // One row an id (the engine lists one already).
                if (std::ranges::any_of(rows, [&](const ProblemRow &held) { return held.id == row->id; }))
                    continue;
                rows.push_back(std::move(*row));
            }
        engine_lists_ = true;
    }
    const bool same = std::ranges::equal(rows, engine_rows_, [](const ProblemRow &a, const ProblemRow &b) {
        return a.id == b.id && a.sentence == b.sentence && a.source == b.source && a.severity == b.severity;
    });
    if (!same) {
        engine_rows_ = std::move(rows);
        compose();
    }
    if (std::exchange(again_, false)) readNow();
    return true;
}

bool ProblemsSession::receiveFailure(std::int64_t id, const std::string &message) {
    if (!owns(id)) return false;
    request_ = 0;
    notice_ = "Could not read the engine's problems: " + message;
    if (std::exchange(again_, false)) readNow();
    if (changed) changed();
    return true;
}

const ProblemRow *ProblemsSession::find(const std::string &id) const {
    const auto found = std::ranges::find(rows_, id, &ProblemRow::id);
    return found == rows_.end() ? nullptr : &*found;
}

std::size_t ProblemsSession::errors() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(rows_, [](const ProblemRow &row) { return row.error(); }));
}

std::size_t ProblemsSession::warnings() const noexcept {
    return rows_.size() - errors();
}

void ProblemsSession::compose() {
    rows_ = engine_rows_;
    rows_.insert(rows_.end(), asset_rows_.begin(), asset_rows_.end());
    if (changed) changed();
}

} // namespace PelicanStudio
