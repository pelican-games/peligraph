#include "projectmigration.hpp"

#include "projectformat.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Pelican {
namespace {

using OrderedJson = nlohmann::ordered_json;

std::string utf8(const std::filesystem::path &path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    const auto *begin = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path{std::u8string{begin, begin + value.size()}};
}

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("could not open " + utf8(path));
    }
    std::string bytes{std::istreambuf_iterator<char>{input},
                      std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error("could not read " + utf8(path));
    }
    return bytes;
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error("could not create " + utf8(path));
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("could not write " + utf8(path));
    }
}

std::filesystem::path canonicalRoot(const std::filesystem::path &project_root) {
    std::error_code error;
    auto root = std::filesystem::weakly_canonical(project_root, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        throw ProjectMigrationError(
            std::string{projectMigrationProjectMissingCode},
            {{"project_root", utf8(project_root)},
             {"message", "the project root is not a directory"}});
    }
    return root;
}

// The raw envelope read. A SAX pass over the file that keeps only the
// top-level `schema` string and `version` integer and stops as soon as it has
// both, so a large data file costs its first few tokens, and a file that is
// not an object stops at its first one. Nothing here knows any format.
class EnvelopeReader final : public nlohmann::json_sax<nlohmann::json> {
    int depth_ = 0;
    bool first_ = true;
    bool object_ = false;
    std::optional<std::string> key_;

    bool atTopLevelValue() {
        const bool top = depth_ == 1 && key_.has_value();
        return top;
    }
    bool finished() const { return schema.has_value() && version.has_value(); }
    bool scalar() {
        if (first_) return false; // a top-level scalar is not a document
        key_.reset();
        return true;
    }

  public:
    std::optional<std::string> schema;
    std::optional<std::int64_t> version;

    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t value) override {
        if (first_) return false;
        if (atTopLevelValue() && *key_ == "version") version = value;
        key_.reset();
        return !finished();
    }
    bool number_unsigned(number_unsigned_t value) override {
        if (first_) return false;
        if (atTopLevelValue() && *key_ == "version" &&
            value <= static_cast<number_unsigned_t>(INT32_MAX)) {
            version = static_cast<std::int64_t>(value);
        }
        key_.reset();
        return !finished();
    }
    bool number_float(number_float_t, const string_t &) override {
        return scalar();
    }
    bool string(string_t &value) override {
        if (first_) return false;
        if (atTopLevelValue() && *key_ == "schema") schema = value;
        key_.reset();
        return !finished();
    }
    bool binary(binary_t &) override { return scalar(); }
    bool start_object(std::size_t) override {
        if (first_) {
            first_ = false;
            object_ = true;
        }
        key_.reset();
        ++depth_;
        return true;
    }
    bool key(string_t &value) override {
        if (depth_ == 1) key_ = value;
        return true;
    }
    bool end_object() override {
        --depth_;
        key_.reset();
        return true;
    }
    bool start_array(std::size_t) override {
        if (first_) return false; // a top-level array is not a document
        key_.reset();
        ++depth_;
        return true;
    }
    bool end_array() override {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

    bool isDocument() const {
        return object_ && schema.has_value() && version.has_value();
    }
};

struct Envelope {
    std::string schema;
    int version = 0;
};

std::optional<Envelope> readEnvelope(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) return std::nullopt;
    EnvelopeReader reader;
    try {
        (void)nlohmann::json::sax_parse(input, &reader);
    } catch (const std::exception &) {
        // Whatever the reader already has is still the envelope; a file that
        // did not get that far is not a document.
    }
    if (!reader.isDocument() || *reader.version < INT32_MIN ||
        *reader.version > INT32_MAX) {
        return std::nullopt;
    }
    return Envelope{*reader.schema, static_cast<int>(*reader.version)};
}

bool hasJsonExtension(const std::filesystem::path &path) {
    auto extension = utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".json";
}

bool isHiddenName(const std::filesystem::path &path) {
    const auto name = utf8(path.filename());
    return !name.empty() && name.front() == '.';
}

struct FoundDocument {
    std::string path;
    Envelope envelope;
};

std::vector<FoundDocument> projectDocuments(const std::filesystem::path &root) {
    std::vector<FoundDocument> found;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it{
        root, std::filesystem::directory_options::skip_permission_denied,
        error};
    if (error) {
        throw std::runtime_error("could not list the project directory " +
                                 utf8(root) + ": " + error.message());
    }
    for (const std::filesystem::recursive_directory_iterator end; it != end;
         it.increment(error)) {
        if (error) {
            throw std::runtime_error("could not list the project directory " +
                                     utf8(root) + ": " + error.message());
        }
        const auto &entry = *it;
        std::error_code status_error;
        if (entry.is_directory(status_error)) {
            if (isHiddenName(entry.path()) ||
                entry.is_symlink(status_error)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(status_error) ||
            !hasJsonExtension(entry.path())) {
            continue;
        }
        if (auto envelope = readEnvelope(entry.path())) {
            found.push_back(FoundDocument{
                utf8(entry.path().lexically_relative(root)),
                std::move(*envelope)});
        }
    }
    std::sort(found.begin(), found.end(),
              [](const FoundDocument &lhs, const FoundDocument &rhs) {
                  return lhs.path < rhs.path;
              });
    return found;
}

int chainEnd(const DocumentMigrationRegistry &registry, std::string_view schema,
             int version) {
    // Registration only admits from + 1 == to, so the chain rises strictly
    // and ends; the bound is a second fence, not the termination argument.
    for (std::size_t guard = 0; guard <= registry.size(); ++guard) {
        const auto *step = registry.find(schema, version);
        if (step == nullptr) return version;
        version = step->to_version;
    }
    throw std::logic_error("document migration chain does not end");
}

OrderedJson manualItemsJson(const std::vector<MigrationManualItem> &items) {
    auto result = OrderedJson::array();
    for (const auto &item : items) {
        result.push_back(OrderedJson{{"code", item.code},
                                     {"pointer", item.pointer},
                                     {"message", item.message}});
    }
    return result;
}

std::string lineEnding(std::string_view bytes) {
    return bytes.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
}

std::string serializeDocument(const OrderedJson &document,
                              std::string_view original) {
    auto text = document.dump(2) + "\n";
    if (lineEnding(original) == "\n") return text;
    std::string converted;
    converted.reserve(text.size() + text.size() / 16);
    for (const char c : text) {
        if (c == '\n') converted += '\r';
        converted += c;
    }
    return converted;
}

// The top-level members of a JSON object text, as byte ranges, found by a
// scanner that only tracks strings and nesting. Enough to change one member
// of project.json without re-serializing the rest of it.
struct MemberSpan {
    std::string key;
    std::size_t key_begin = 0;
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
};

std::size_t skipString(std::string_view text, std::size_t at) {
    // `at` is the opening quote; returns one past the closing quote.
    for (std::size_t i = at + 1; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        } else if (text[i] == '"') {
            return i + 1;
        }
    }
    throw std::runtime_error("unterminated string");
}

std::size_t skipSpace(std::string_view text, std::size_t at) {
    while (at < text.size() &&
           (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' ||
            text[at] == '\n')) {
        ++at;
    }
    return at;
}

std::size_t skipValue(std::string_view text, std::size_t at) {
    if (at >= text.size()) throw std::runtime_error("missing value");
    if (text[at] == '"') return skipString(text, at);
    if (text[at] == '{' || text[at] == '[') {
        int depth = 0;
        for (std::size_t i = at; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '"') {
                i = skipString(text, i) - 1;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        throw std::runtime_error("unterminated value");
    }
    std::size_t i = at;
    while (i < text.size() && text[i] != ',' && text[i] != '}' &&
           text[i] != ']' && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\r' && text[i] != '\n') {
        ++i;
    }
    return i;
}

std::vector<MemberSpan> topLevelMembers(std::string_view text) {
    std::vector<MemberSpan> members;
    auto at = skipSpace(text, text.starts_with("\xEF\xBB\xBF") ? 3 : 0);
    if (at >= text.size() || text[at] != '{') {
        throw std::runtime_error("not an object");
    }
    at = skipSpace(text, at + 1);
    while (at < text.size() && text[at] != '}') {
        if (text[at] != '"') throw std::runtime_error("expected a key");
        MemberSpan member;
        member.key_begin = at;
        const auto key_end = skipString(text, at);
        member.key = nlohmann::json::parse(text.substr(at, key_end - at))
                         .get<std::string>();
        at = skipSpace(text, key_end);
        if (at >= text.size() || text[at] != ':') {
            throw std::runtime_error("expected ':'");
        }
        member.value_begin = skipSpace(text, at + 1);
        member.value_end = skipValue(text, member.value_begin);
        members.push_back(member);
        at = skipSpace(text, member.value_end);
        if (at < text.size() && text[at] == ',') at = skipSpace(text, at + 1);
    }
    return members;
}

// project.json with `saved_by_engine` set to `version`, every other byte as
// it was: the value is replaced where the key already is, or the member is
// inserted after `version`, indented like it. The result is checked against a
// re-parse; anything the scanner did not understand falls back to a
// re-serialization, which is still the same JSON.
std::string withSavedByEngine(const std::string &bytes, std::string_view version) {
    auto expected = nlohmann::json::parse(bytes);
    expected["saved_by_engine"] = std::string{version};
    const auto quoted = nlohmann::json(std::string{version}).dump();
    try {
        const auto members = topLevelMembers(bytes);
        std::string result;
        const auto existing = std::find_if(
            members.begin(), members.end(),
            [](const MemberSpan &m) { return m.key == "saved_by_engine"; });
        if (existing != members.end()) {
            result = bytes.substr(0, existing->value_begin) + quoted +
                     bytes.substr(existing->value_end);
        } else {
            const auto anchor = std::find_if(
                members.begin(), members.end(),
                [](const MemberSpan &m) { return m.key == "version"; });
            if (anchor == members.end()) throw std::runtime_error("no version");
            const auto line_start = bytes.rfind('\n', anchor->key_begin);
            std::string separator = " ";
            if (line_start != std::string::npos) {
                const auto indent =
                    bytes.substr(line_start + 1, anchor->key_begin - line_start - 1);
                if (indent.find_first_not_of(" \t") == std::string::npos) {
                    separator = lineEnding(bytes) + indent;
                }
            }
            result = bytes.substr(0, anchor->value_end) + "," + separator +
                     "\"saved_by_engine\": " + quoted +
                     bytes.substr(anchor->value_end);
        }
        if (nlohmann::json::parse(result) == expected) return result;
    } catch (const std::exception &) {
    }
    auto ordered = OrderedJson::parse(bytes);
    ordered["saved_by_engine"] = std::string{version};
    return serializeDocument(ordered, bytes);
}

std::string backupTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

// A failed apply leaves no trace of itself: the backup goes, and so does the
// migration directory when this backup was all it held.
void removeBackup(const std::filesystem::path &backup) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(backup, ignored);
    std::filesystem::remove(backup.parent_path(), ignored);
}

OrderedJson digestsJson(const std::map<std::string, std::string> &digests) {
    auto result = OrderedJson::object();
    for (const auto &[path, digest] : digests) result[path] = digest;
    return result;
}

} // namespace

ProjectMigrationError::ProjectMigrationError(std::string code,
                                             nlohmann::ordered_json fields)
    // Parentheses, not braces: a braced json initializer is an array.
    : std::runtime_error{code + ": " + fields.dump()}, code_{std::move(code)},
      fields_(std::move(fields)) {}

void DocumentMigrationRegistry::registerStep(DocumentMigrationStep step) {
    const auto describe = [&step] {
        return OrderedJson{{"id", step.id},
                           {"schema", step.schema},
                           {"from", step.from_version},
                           {"to", step.to_version}};
    };
    if (step.id.empty() || step.schema.empty() || step.from_version < 1 ||
        step.to_version < 1 || !step.plan) {
        auto fields = describe();
        fields["message"] =
            "a migration step needs an id, a schema, versions of at least 1 "
            "and a plan function";
        throw ProjectMigrationError(std::string{projectMigrationStepInvalidCode},
                                    std::move(fields));
    }
    for (const auto &[key, registered] : steps_) {
        (void)key;
        if (registered.id == step.id) {
            auto fields = describe();
            fields["registered"] = registered.id;
            throw ProjectMigrationError(
                std::string{projectMigrationStepDuplicateCode}, std::move(fields));
        }
    }
    if (const auto *registered = find(step.schema, step.from_version)) {
        auto fields = describe();
        fields["registered"] = registered->id;
        throw ProjectMigrationError(std::string{projectMigrationStepDuplicateCode},
                                    std::move(fields));
    }
    // Every admitted step raises the version by exactly one, so the graph of
    // steps cannot close on itself. A step that does not go up is the one
    // edge that could: it points back at a version the forward chain already
    // passes through (a self loop when to == from).
    if (step.to_version <= step.from_version) {
        throw ProjectMigrationError(std::string{projectMigrationStepCycleCode},
                                    describe());
    }
    if (step.to_version != step.from_version + 1) {
        throw ProjectMigrationError(std::string{projectMigrationStepGapCode},
                                    describe());
    }
    auto key = std::make_pair(step.schema, step.from_version);
    steps_.emplace(std::move(key), std::move(step));
}

const DocumentMigrationStep *
DocumentMigrationRegistry::find(std::string_view schema, int from_version) const {
    const auto found = steps_.find(std::make_pair(std::string{schema}, from_version));
    return found == steps_.end() ? nullptr : &found->second;
}

nlohmann::ordered_json DocumentMigrationRegistry::catalog() const {
    auto result = OrderedJson::array();
    for (const auto &[key, step] : steps_) {
        (void)key;
        result.push_back(OrderedJson{{"id", step.id},
                                     {"schema", step.schema},
                                     {"from", step.from_version},
                                     {"to", step.to_version},
                                     {"description", step.description}});
    }
    return result;
}

DocumentMigrationRegistry makeBuiltinDocumentMigrationRegistry() {
    // v0 registers nothing. A WP that raises a document's `version` registers
    // its step here, in the same change.
    return DocumentMigrationRegistry{};
}

const DocumentMigrationRegistry &builtinDocumentMigrationRegistry() {
    static const DocumentMigrationRegistry registry =
        makeBuiltinDocumentMigrationRegistry();
    return registry;
}

std::vector<ProjectMigrationDetection>
detectProjectMigration(const std::filesystem::path &project_root,
                       const DocumentMigrationRegistry &registry) {
    std::vector<ProjectMigrationDetection> detections;
    if (registry.empty()) return detections;
    const auto root = canonicalRoot(project_root);
    for (auto &document : projectDocuments(root)) {
        const auto &envelope = document.envelope;
        if (registry.find(envelope.schema, envelope.version) == nullptr) continue;
        detections.push_back(ProjectMigrationDetection{
            .path = std::move(document.path),
            .schema = envelope.schema,
            .from_version = envelope.version,
            .to_version = chainEnd(registry, envelope.schema, envelope.version),
        });
    }
    return detections;
}

void requireProjectMigrationNotRequired(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry) {
    recoverProjectDocumentTransaction(project_root);
    const auto detections = detectProjectMigration(project_root, registry);
    if (detections.empty()) return;
    auto documents = OrderedJson::array();
    for (const auto &detection : detections) {
        documents.push_back(OrderedJson{{"path", detection.path},
                                        {"schema", detection.schema},
                                        {"from", detection.from_version},
                                        {"to", detection.to_version}});
    }
    throw ProjectMigrationError(std::string{projectMigrationRequiredCode},
                                OrderedJson{{"documents", std::move(documents)}});
}

std::map<std::string, std::string> ProjectMigrationPlan::expectedDigests() const {
    std::map<std::string, std::string> digests;
    if (!required()) return digests;
    for (const auto &document : documents) {
        digests.emplace(document.path, document.digest);
    }
    digests.emplace("project.json", project_json_digest);
    return digests;
}

nlohmann::ordered_json ProjectMigrationPlan::toJson() const {
    auto documents_json = OrderedJson::array();
    for (const auto &document : documents) {
        documents_json.push_back(OrderedJson{
            {"path", document.path},
            {"schema", document.schema},
            {"from", document.from_version},
            {"to", document.to_version},
            {"steps", document.steps},
            {"digest", document.digest},
            {"manual_items", manualItemsJson(document.manual_items)},
        });
    }
    return OrderedJson{
        {"project_root", utf8(project_root)},
        {"required", required()},
        {"engine_version", engine_version},
        {"saved_by_engine", saved_by_engine ? OrderedJson(*saved_by_engine)
                                            : OrderedJson(nullptr)},
        {"documents", std::move(documents_json)},
        {"expected", digestsJson(expectedDigests())},
    };
}

ProjectMigrationPlan planProjectMigration(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry) {
    ProjectMigrationPlan plan;
    plan.project_root = canonicalRoot(project_root);
    plan.engine_version = std::string{pelicanEngineVersion};

    const auto project_file = plan.project_root / "project.json";
    std::string project_bytes;
    OrderedJson project;
    try {
        project_bytes = readBytes(project_file);
        project = OrderedJson::parse(project_bytes);
        if (!project.is_object()) throw std::runtime_error("not an object");
    } catch (const std::exception &error) {
        throw ProjectMigrationError(
            std::string{projectMigrationProjectMissingCode},
            {{"project_root", utf8(plan.project_root)},
             {"message", std::string{"project.json could not be read: "} +
                             error.what()}});
    }
    plan.project_json_digest = projectDocumentDigest(project_bytes);
    plan.project_json_before = project_bytes;
    if (const auto saved = project.find("saved_by_engine");
        saved != project.end() && saved->is_string()) {
        plan.saved_by_engine = saved->get<std::string>();
    }

    std::optional<std::string> migrated_project;
    for (const auto &detection : detectProjectMigration(plan.project_root, registry)) {
        ProjectMigrationDocumentPlan document{
            .path = detection.path,
            .schema = detection.schema,
            .from_version = detection.from_version,
            .to_version = detection.to_version,
        };
        const ProjectContext context{plan.project_root, detection.path};
        const auto fail = [&](const std::string &step, const std::string &message) {
            return ProjectMigrationError(
                std::string{projectMigrationStepFailedCode},
                {{"path", detection.path}, {"step", step}, {"message", message}});
        };
        OrderedJson current;
        try {
            document.before_bytes = readBytes(plan.project_root / pathFromUtf8(detection.path));
            current = OrderedJson::parse(document.before_bytes);
        } catch (const std::exception &error) {
            throw fail("", std::string{"the document could not be read: "} + error.what());
        }
        document.digest = projectDocumentDigest(document.before_bytes);
        int version = detection.from_version;
        while (const auto *step = registry.find(detection.schema, version)) {
            StepResult result;
            try {
                result = step->plan(current, context);
            } catch (const std::exception &error) {
                throw fail(step->id, error.what());
            }
            if (!result.document.is_object()) {
                throw fail(step->id, "the step did not return a JSON object");
            }
            const auto schema = result.document.find("schema");
            if (schema != result.document.end() &&
                (!schema->is_string() || schema->get<std::string>() != detection.schema)) {
                throw fail(step->id, "the step changed the document's schema");
            }
            result.document["schema"] = detection.schema;
            result.document["version"] = step->to_version;
            current = std::move(result.document);
            document.steps.push_back(step->id);
            for (auto &item : result.manual_items) {
                document.manual_items.push_back(std::move(item));
            }
            version = step->to_version;
        }
        document.after_bytes = serializeDocument(current, document.before_bytes);
        if (document.path == "project.json") migrated_project = document.after_bytes;
        plan.documents.push_back(std::move(document));
    }
    plan.project_json_after = withSavedByEngine(
        migrated_project ? *migrated_project : project_bytes, plan.engine_version);
    return plan;
}

nlohmann::ordered_json ProjectMigrationApplyResult::toJson() const {
    auto result = plan.toJson();
    result["applied"] = applied;
    result["backup"] = backup.empty() ? OrderedJson(nullptr) : OrderedJson(backup);
    result["saved_by_engine_after"] =
        applied ? OrderedJson(plan.engine_version) : result["saved_by_engine"];
    return result;
}

ProjectMigrationApplyResult applyProjectMigration(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry,
    const ProjectMigrationApplyOptions &options) {
    const auto root = canonicalRoot(project_root);
    // A transaction a crash left behind is resolved first, exactly as opening
    // the project would, so the plan reads the documents as they stand.
    recoverProjectDocumentTransaction(root);

    ProjectMigrationApplyResult result;
    result.plan = planProjectMigration(root, registry);
    const auto actual = result.plan.expectedDigests();
    if (options.expected_digests && *options.expected_digests != actual) {
        throw ProjectMigrationError(
            std::string{projectMigrationPlanStaleCode},
            {{"expected", digestsJson(*options.expected_digests)},
             {"actual", digestsJson(actual)}});
    }
    if (!result.plan.required()) return result;

    // (a) The documents apply writes, as they are now, next to a record of
    // the plan. A name that is already taken gets a suffix; nothing is
    // overwritten.
    const auto stamp = options.timestamp ? options.timestamp() : backupTimestamp();
    const auto migration_root = root / ".pelican" / "migration";
    auto backup = migration_root / pathFromUtf8(stamp);
    for (int suffix = 2; std::filesystem::exists(backup); ++suffix) {
        backup = migration_root / pathFromUtf8(stamp + "-" + std::to_string(suffix));
    }
    std::vector<ProjectCandidateDocument> candidates;
    bool project_json_migrated = false;
    try {
        std::filesystem::create_directories(backup);
        for (const auto &document : result.plan.documents) {
            writeBytes(backup / pathFromUtf8(document.path), document.before_bytes);
            const bool is_project = document.path == "project.json";
            project_json_migrated = project_json_migrated || is_project;
            candidates.push_back(ProjectCandidateDocument{
                .reference = document.path,
                .normalized_reference = document.path,
                .path = root / pathFromUtf8(document.path),
                .operation = ProjectDocumentOperation::replace,
                .expected = {ProjectDocumentExistence::present, document.digest},
                .bytes = is_project ? result.plan.project_json_after
                                    : document.after_bytes,
            });
        }
        if (!project_json_migrated) {
            writeBytes(backup / "project.json", result.plan.project_json_before);
            // (c) `saved_by_engine` is written in the same transaction as the
            // documents, so the record never claims a migration that was
            // rolled back.
            candidates.push_back(ProjectCandidateDocument{
                .reference = "project.json",
                .normalized_reference = "project.json",
                .path = root / "project.json",
                .operation = ProjectDocumentOperation::replace,
                .expected = {ProjectDocumentExistence::present,
                             result.plan.project_json_digest},
                .bytes = result.plan.project_json_after,
            });
        }
        auto record = result.plan.toJson();
        record.erase("project_root");
        record.erase("required");
        OrderedJson backup_record{{"schema", "pelican.project_migration_backup"},
                                  {"version", 1}};
        for (auto &[key, value] : record.items()) backup_record[key] = value;
        writeBytes(backup / "migration.json", backup_record.dump(2) + "\n");
    } catch (const std::exception &error) {
        removeBackup(backup);
        throw ProjectMigrationError(
            std::string{projectMigrationApplyFailedCode},
            {{"message", std::string{"could not write the backup: "} + error.what()}});
    }

    // (b) One transaction for every document: a failure before its commit
    // marker rolls each applied document back to the bytes it had.
    try {
        (void)commitProjectDocuments(root, candidates, options.fault);
    } catch (const std::exception &error) {
        removeBackup(backup);
        throw ProjectMigrationError(std::string{projectMigrationApplyFailedCode},
                                    {{"message", error.what()}});
    }
    result.applied = true;
    result.backup = utf8(backup.lexically_relative(root));
    return result;
}

} // namespace Pelican
