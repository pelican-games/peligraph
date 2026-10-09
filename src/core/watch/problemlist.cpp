#include "problemlist.hpp"

#include "assetkey.hpp"
#include "../appflow/enginetime.hpp"
#include "../loader/pathresolver.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/renderconfigdocument.hpp"
#include "../../project/renderconfigsection.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace Pelican {

namespace {

using Json = nlohmann::json;

std::optional<std::string> spelled(const Json &value) {
    if (!value.is_string()) return std::nullopt;
    return graphDocumentReference(value.get<std::string>());
}

void appendFeatures(const Json &section, std::vector<std::string> &out) {
    const auto features = section.find("features");
    if (features == section.end() || !features->is_array()) return;
    for (const auto &entry : *features) {
        std::optional<std::string> reference;
        if (entry.is_string()) {
            reference = spelled(entry);
        } else if (entry.is_object()) {
            if (const auto ref = entry.find("ref"); ref != entry.end())
                reference = spelled(*ref);
        }
        if (reference) out.push_back(std::move(*reference));
    }
}

// The members of a ticket's error that say how the refusal travelled, not
// what it is.
bool bookkeeping(std::string_view key) {
    return key == "code" || key == "message" || key == "participant" ||
           key == "method" || key == "ticket";
}

// Where a problem is, in the order the id reads them.
constexpr std::string_view location_keys[] = {
    "document", "definition", "scope", "node", "port",
    "field",    "line",       "column", "feature", "module",
};

constexpr std::string_view shader_failure_marker = "Shader compile failed: ";
constexpr std::string_view shader_reload_failed_code = "shader_reload_failed@1";

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

// The project document a file names, through the watch's identity of it.
std::optional<std::string> projectDocumentOfFile(const std::string &file) {
    const auto *paths = FastModuleContainer::tryGet<PathResolver>();
    if (paths == nullptr || file.empty()) return std::nullopt;
    try {
        if (auto key = watch::assetKeyForFile(*paths, std::filesystem::path{file}))
            return watch::assetKeyString(*key);
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

// One shader problem: the file, the log's first error line as the message,
// and the line when that error is in the file itself.
Json shaderProblem(std::string_view source, const std::string &document,
                   const ShaderCompileFailureText &failure,
                   const std::function<std::optional<std::string>(const std::string &)>
                       &document_of_file,
                   std::optional<std::uint64_t> at_frame) {
    Json problem{
        {"severity", std::string{problemSeverityError}},
        {"source", std::string{source}},
        {"code", std::string{shader_reload_failed_code}},
        {"message", std::string{shader_failure_marker} + failure.path},
        {"document", document},
        {"document_kind", std::string{problemKindShader}},
    };
    if (const auto error = firstShaderLogError(failure.log)) {
        problem["message"] = error->text;
        if (document_of_file && document_of_file(error->file) == document)
            problem["line"] = error->line;
    }
    if (at_frame) problem["at_frame"] = *at_frame;
    problem["fields"] = {{"path", failure.path}, {"log", failure.log}};
    problem["id"] = problemId(problem);
    return problem;
}

std::optional<std::uint64_t> currentFrame() {
    if (const auto *time = FastModuleContainer::tryGet<EngineTime>())
        return time->evaluationIndex();
    return std::nullopt;
}

} // namespace

std::vector<ShaderCompileFailureText> shaderCompileFailures(std::string_view text) {
    std::vector<ShaderCompileFailureText> failures;
    auto at = text.find(shader_failure_marker);
    while (at != std::string_view::npos) {
        const auto begin = at + shader_failure_marker.size();
        const auto next = text.find(shader_failure_marker, begin);
        auto chunk = text.substr(begin, next == std::string_view::npos ? std::string_view::npos : next - begin);
        // The "; " a reload joined the next failure with.
        if (next != std::string_view::npos && chunk.ends_with("; ")) chunk.remove_suffix(2);
        ShaderCompileFailureText failure;
        const auto newline = chunk.find('\n');
        failure.path = trimmed(chunk.substr(0, newline));
        if (newline != std::string_view::npos) failure.log = trimmed(chunk.substr(newline + 1));
        failures.push_back(std::move(failure));
        at = next;
    }
    return failures;
}

std::optional<ShaderLogError> firstShaderLogError(std::string_view log) {
    constexpr std::string_view prefix = "ERROR: ";
    std::size_t begin = 0;
    while (begin < log.size()) {
        auto end = log.find('\n', begin);
        if (end == std::string_view::npos) end = log.size();
        auto line = log.substr(begin, end - begin);
        if (line.ends_with('\r')) line.remove_suffix(1);
        begin = end + 1;
        // glslang writes "<file>:<line>: error: <reason>"; some logs put
        // "ERROR: " in front instead.
        const bool marked = line.starts_with(prefix);
        const auto rest = marked ? line.substr(prefix.size()) : line;
        // "<file>:<line>:" -- the file may hold a drive's colon.
        for (auto colon = rest.find(':'); colon != std::string_view::npos;
             colon = rest.find(':', colon + 1)) {
            auto digits_end = colon + 1;
            while (digits_end < rest.size() && rest[digits_end] >= '0' && rest[digits_end] <= '9') ++digits_end;
            if (colon == 0 || digits_end == colon + 1 || digits_end >= rest.size() || rest[digits_end] != ':')
                continue;
            const auto after = trimmed(rest.substr(digits_end + 1));
            if (!marked && !after.starts_with("error")) break;
            return ShaderLogError{
                .file = trimmed(rest.substr(0, colon)),
                .line = std::stoull(std::string{rest.substr(colon + 1, digits_end - colon - 1)}),
                .text = std::string{line},
            };
        }
        if (marked) return ShaderLogError{.text = std::string{line}};
    }
    return std::nullopt;
}

RenderDocumentRoles renderDocumentRoles(
    std::string_view root_bytes, std::string_view root_reference,
    const std::function<std::optional<std::string>(const std::string &)>
        &load_document) {
    RenderDocumentRoles roles;
    roles.root = graphDocumentReference(root_reference)
                     .value_or(std::string{root_reference});
    const auto root = Json::parse(root_bytes, nullptr, false);
    if (!isRenderGraphRoot(root)) return roles;
    const auto &section = renderingSectionOrEmpty(root);
    appendFeatures(section, roles.features);
    const auto pipeline = section.find("pipeline");
    if (pipeline == section.end() || !pipeline->is_object()) return roles;
    const auto preset = pipeline->find("preset");
    if (preset == pipeline->end() || !preset->is_string()) return roles;
    roles.preset = spelled(*preset);
    if (!load_document) return roles;
    try {
        if (const auto bytes = load_document(preset->get<std::string>())) {
            const auto document = Json::parse(*bytes, nullptr, false);
            if (document.is_object() && document.contains("config") &&
                document.at("config").is_object())
                appendFeatures(document.at("config"), roles.features);
        }
    } catch (const std::exception &) {
        // A preset that does not read enables no feature we can name.
    }
    return roles;
}

Json renderPipelineProblem(std::string_view source, const Json &refusal,
                           const RenderDocumentRoles &roles,
                           std::optional<std::uint64_t> at_frame) {
    Json fields = Json::object();
    if (refusal.is_object())
        for (const auto &[key, value] : refusal.items())
            if (!bookkeeping(key)) fields[key] = value;
    const auto code = refusal.is_object()
                          ? refusal.value("code", std::string{})
                          : std::string{};

    Json problem{
        {"severity", std::string{problemSeverityError}},
        {"source", std::string{source}},
        {"code", code},
        {"message", refusal.is_object()
                        ? refusal.value("message", std::string{})
                        : std::string{}},
    };

    // The document: the refusal's own, or the reference a preflight
    // refusal names, or the rendering config a route refusal names.
    std::optional<std::string> document;
    for (const auto *key : {"document", "reference", "rendering_config"}) {
        if (const auto found = fields.find(key); found != fields.end()) {
            document = spelled(*found);
            if (document) break;
        }
    }
    // A refusal that is a shader that did not compile is the shader's.
    bool shader = false;
    if (!document && roles.shader_document) {
        const auto failures = shaderCompileFailures(problem.value("message", std::string{}));
        if (!failures.empty())
            if (auto file = roles.shader_document(failures.front().path)) {
                document = std::move(file);
                shader = true;
                if (const auto error = firstShaderLogError(failures.front().log);
                    error && roles.shader_document(error->file) == document)
                    problem["line"] = error->line;
            }
    }
    if (document) problem["document"] = *document;
    for (const auto *key :
         {"line", "column", "node", "port", "definition", "scope"}) {
        if (const auto found = fields.find(key);
            found != fields.end() && !found->is_null())
            problem[key] = *found;
    }

    if (shader) {
        problem["document_kind"] = std::string{problemKindShader};
    } else if (document) {
        std::optional<std::string_view> kind;
        if (*document == roles.root) {
            if (code == renderDocumentSyntaxCode)
                kind = problemKindRoot;
            else if (fields.contains("definition"))
                kind = problemKindDefinition;
            // EB-7: or any refusal that names the root graph's node (a ray
            // tracing task a render.compute node made).
            else if (code.starts_with("graph.") ||
                     (fields.contains("node") && !fields.at("node").is_null()))
                kind = problemKindRootGraph;
            else
                kind = problemKindRootRendering;
        } else if (roles.preset && *document == *roles.preset) {
            kind = problemKindPreset;
        } else if (std::find(roles.features.begin(), roles.features.end(),
                             *document) != roles.features.end()) {
            kind = problemKindFeature;
        }
        if (kind) problem["document_kind"] = std::string{*kind};
    }
    if (at_frame) problem["at_frame"] = *at_frame;
    problem["fields"] = std::move(fields);
    problem["id"] = problemId(problem);
    return problem;
}

std::string problemId(const Json &problem) {
    Json identity{{"code", problem.value("code", std::string{})}};
    const auto fields = problem.find("fields");
    bool located = false;
    for (const auto key : location_keys) {
        const std::string name{key};
        const Json *value = nullptr;
        if (const auto top = problem.find(name); top != problem.end())
            value = &*top;
        else if (fields != problem.end() && fields->is_object())
            if (const auto inner = fields->find(name); inner != fields->end())
                value = &*inner;
        if (value == nullptr || value->is_null()) continue;
        identity[name] = *value;
        located = true;
    }
    if (!located) identity["message"] = problem.value("message", std::string{});
    return renderConfigSourceDigest(identity.dump()).substr(0, 16);
}

void ProblemList::replace(std::string_view domain,
                          std::vector<Json> problems) {
    for (auto &problem : problems)
        if (!problem.contains("id")) problem["id"] = problemId(problem);
    std::scoped_lock lock{mutex_};
    const auto found = domains_.find(domain);
    if (problems.empty()) {
        if (found == domains_.end()) return;
        domains_.erase(found);
        ++revision_;
        return;
    }
    if (found != domains_.end()) {
        if (found->second == problems) return;
        found->second = std::move(problems);
    } else {
        domains_.emplace(std::string{domain}, std::move(problems));
    }
    ++revision_;
}

std::vector<Json> ProblemList::problemsOf(std::string_view domain) const {
    std::scoped_lock lock{mutex_};
    const auto found = domains_.find(domain);
    return found == domains_.end() ? std::vector<Json>{} : found->second;
}

std::uint64_t ProblemList::revision() const {
    std::scoped_lock lock{mutex_};
    return revision_;
}

Json ProblemList::problemsJson() const {
    std::scoped_lock lock{mutex_};
    auto list = Json::array();
    std::set<std::string> ids;
    for (const auto &[domain, problems] : domains_) {
        (void)domain;
        for (const auto &problem : problems)
            if (ids.insert(problem.value("id", std::string{})).second)
                list.push_back(problem);
    }
    return {{"revision", revision_}, {"problems", std::move(list)}};
}

Json ProblemList::statusJson() const {
    const auto all = problemsJson();
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const auto &problem : all.at("problems")) {
        const auto severity = problem.value("severity", std::string{});
        if (severity == problemSeverityError) ++errors;
        else if (severity == problemSeverityWarning) ++warnings;
    }
    return {{"revision", all.at("revision")},
            {"count", all.at("problems").size()},
            {"errors", errors},
            {"warnings", warnings}};
}

Json problemListStatusJson(const ProblemList *problems) {
    if (problems != nullptr) return problems->statusJson();
    return {{"revision", 0}, {"count", 0}, {"errors", 0}, {"warnings", 0}};
}

Json problemListJson(const ProblemList *problems) {
    if (problems != nullptr) return problems->problemsJson();
    return {{"revision", 0}, {"problems", Json::array()}};
}

void recordRenderPipelineProblem(std::string_view source,
                                 const Json &refusal,
                                 std::string_view root_reference) {
    auto *problems = FastModuleContainer::tryGet<ProblemList>();
    if (problems == nullptr) return;
    const auto *paths = FastModuleContainer::tryGet<PathResolver>();
    const auto load =
        [paths](const std::string &reference) -> std::optional<std::string> {
        if (paths == nullptr) return std::nullopt;
        try {
            return paths->loadText(reference);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    };
    const auto root_bytes = load(std::string{root_reference});
    auto roles = renderDocumentRoles(root_bytes.value_or(std::string{}),
                                     root_reference, load);
    roles.shader_document = projectDocumentOfFile;
    auto effective = refusal;
    const auto reason_code = refusal.value("code", std::string{});
    if (reason_code.starts_with("compute.descriptor_")) {
        effective["code"] = "render.config_apply_failed@1";
        effective["reason_code"] = reason_code;
        effective["reason"] = refusal;
        effective["document"] = graphDocumentReference(root_reference).value_or(std::string{root_reference});
    }
    problems->replace(renderPipelineProblemDomain,
                      {renderPipelineProblem(source, effective, roles, currentFrame())});
}

void clearRenderPipelineProblems() {
    if (auto *problems = FastModuleContainer::tryGet<ProblemList>())
        problems->clear(renderPipelineProblemDomain);
}

void syncShaderProblems(std::string_view source,
                        const std::vector<std::string> &failed_documents,
                        std::string_view error_text) {
    auto *problems = FastModuleContainer::tryGet<ProblemList>();
    if (problems == nullptr) return;
    const auto previous = problems->problemsOf(shaderProblemDomain);
    const auto failures = shaderCompileFailures(error_text);
    const auto at_frame = currentFrame();
    std::vector<Json> next;
    for (const auto &document : failed_documents) {
        const ShaderCompileFailureText *named = nullptr;
        for (const auto &failure : failures)
            if (projectDocumentOfFile(failure.path) == document) {
                named = &failure;
                break;
            }
        if (named != nullptr) {
            next.push_back(shaderProblem(source, document, *named,
                                         projectDocumentOfFile, at_frame));
            continue;
        }
        const auto kept = std::find_if(previous.begin(), previous.end(), [&](const Json &problem) {
            return problem.value("document", std::string{}) == document;
        });
        if (kept != previous.end()) {
            next.push_back(*kept);
            continue;
        }
        // Failed, but this reload's words name no file of it (an include
        // that broke every unit reading it is named by those units).
        next.push_back(shaderProblem(
            source, document,
            ShaderCompileFailureText{.path = document, .log = trimmed(error_text)},
            projectDocumentOfFile, at_frame));
    }
    problems->replace(shaderProblemDomain, std::move(next));
}

} // namespace Pelican
