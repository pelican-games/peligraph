#include "renderconfigdocument.hpp"

#include <algorithm>
#include <optional>
#include <cctype>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

struct StringToken {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string value;
};

struct FeatureElementToken {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool editable = false;
    std::string reference;
};

constexpr std::string_view renderingSectionKey = "rendering";

class JsonCursor {
    std::string_view bytes_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(std::string_view message) const {
        throw std::invalid_argument(
            "render config lexical parse failed at byte " +
            std::to_string(position_) + ": " + std::string{message});
    }

    void skipLiteral() {
        const auto begin = position_;
        while (position_ < bytes_.size()) {
            const unsigned char ch =
                static_cast<unsigned char>(bytes_[position_]);
            if (std::isspace(ch) || ch == ',' || ch == ']' || ch == '}') {
                break;
            }
            ++position_;
        }
        if (position_ == begin) fail("expected JSON value");
    }

  public:
    explicit JsonCursor(std::string_view bytes) : bytes_{bytes} {
        if (bytes_.size() >= 3 &&
            static_cast<unsigned char>(bytes_[0]) == 0xef &&
            static_cast<unsigned char>(bytes_[1]) == 0xbb &&
            static_cast<unsigned char>(bytes_[2]) == 0xbf) {
            position_ = 3;
        }
    }

    // Re-enters the same bytes at a nested value: the section scan reuses the
    // root scan instead of becoming a second lexer.
    JsonCursor(std::string_view bytes, std::size_t position)
        : bytes_{bytes}, position_{position} {}

    std::size_t position() const noexcept { return position_; }
    bool atEnd() const noexcept { return position_ == bytes_.size(); }

    void skipWhitespace() noexcept {
        while (position_ < bytes_.size() &&
               std::isspace(static_cast<unsigned char>(bytes_[position_]))) {
            ++position_;
        }
    }

    char peek() const {
        if (position_ >= bytes_.size()) fail("unexpected end of input");
        return bytes_[position_];
    }

    void require(char expected) {
        if (peek() != expected) {
            fail(std::string{"expected '"} + expected + "'");
        }
        ++position_;
    }

    StringToken parseString() {
        const auto begin = position_;
        require('"');
        bool escaped = false;
        while (position_ < bytes_.size()) {
            const char ch = bytes_[position_++];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                const auto end = position_;
                try {
                    return StringToken{
                        .begin = begin,
                        .end = end,
                        .value = nlohmann::json::parse(
                                     bytes_.substr(begin, end - begin))
                                     .get<std::string>(),
                    };
                } catch (const std::exception &error) {
                    fail(std::string{"invalid JSON string: "} + error.what());
                }
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                fail("unescaped control byte in JSON string");
            }
        }
        fail("unterminated JSON string");
    }

    void skipValue() {
        skipWhitespace();
        switch (peek()) {
        case '"':
            (void)parseString();
            return;
        case '{': {
            require('{');
            skipWhitespace();
            if (peek() == '}') {
                require('}');
                return;
            }
            for (;;) {
                (void)parseString();
                skipWhitespace();
                require(':');
                skipValue();
                skipWhitespace();
                if (peek() == '}') {
                    require('}');
                    return;
                }
                require(',');
                skipWhitespace();
            }
        }
        case '[': {
            require('[');
            skipWhitespace();
            if (peek() == ']') {
                require(']');
                return;
            }
            for (;;) {
                skipValue();
                skipWhitespace();
                if (peek() == ']') {
                    require(']');
                    return;
                }
                require(',');
                skipWhitespace();
            }
        }
        default:
            skipLiteral();
            return;
        }
    }
};

struct RootLocation {
    bool found = false;
    std::size_t open = 0;
    std::size_t close = 0;
    std::vector<FeatureElementToken> elements;
    std::size_t root_open = 0;
    std::size_t root_close = 0;
    std::size_t last_member_key = 0;
    std::size_t last_member_value_end = 0;
    bool has_members = false;
    bool selected_found = false;
    std::size_t selected_begin = 0;
    std::size_t selected_end = 0;
};

// Scans one JSON object's members lexically. `cursor` is positioned at the
// object's opening brace; `require_end` is true only for the document root,
// where trailing bytes are an error.
RootLocation locateObjectMembers(JsonCursor cursor, bool require_end,
    std::optional<std::string_view> selected_key) {
    cursor.skipWhitespace();
    RootLocation result;
    result.root_open = cursor.position();
    cursor.require('{');
    cursor.skipWhitespace();

    if (cursor.peek() == '}') {
        result.root_close = cursor.position();
        cursor.require('}');
    } else {
        for (;;) {
            const auto key = cursor.parseString();
            result.has_members = true;
            result.last_member_key = key.begin;
            cursor.skipWhitespace();
            cursor.require(':');
            cursor.skipWhitespace();
            const auto value_begin = cursor.position();
            if (key.value == "features") {
                if (result.found) {
                    throw std::invalid_argument(
                        "render config contains duplicate top-level features keys");
                }
                result.found = true;
                result.open = cursor.position();
                cursor.require('[');
                cursor.skipWhitespace();
                if (cursor.peek() != ']') {
                    for (;;) {
                        const auto element_begin = cursor.position();
                        if (cursor.peek() == '"') {
                            const auto element = cursor.parseString();
                            result.elements.push_back(FeatureElementToken{
                                .begin = element.begin,
                                .end = element.end,
                                .editable = true,
                                .reference = std::move(element.value),
                            });
                        } else if (cursor.peek() == '{') {
                            cursor.skipValue();
                            result.elements.push_back(FeatureElementToken{
                                .begin = element_begin,
                                .end = cursor.position(),
                            });
                        } else {
                            throw std::invalid_argument(
                                "render config top-level features entries must be strings or objects");
                        }
                        cursor.skipWhitespace();
                        if (cursor.peek() == ']') break;
                        cursor.require(',');
                        cursor.skipWhitespace();
                    }
                }
                result.close = cursor.position();
                cursor.require(']');
            } else {
                cursor.skipValue();
            }
            result.last_member_value_end = cursor.position();
            if (selected_key && key.value == *selected_key) {
                // Match the semantic parser's last-wins behavior for non-feature keys.
                result.selected_found = true;
                result.selected_begin = value_begin;
                result.selected_end = cursor.position();
            }

            cursor.skipWhitespace();
            if (cursor.peek() == '}') {
                result.root_close = cursor.position();
                cursor.require('}');
                break;
            }
            cursor.require(',');
            cursor.skipWhitespace();
        }
    }
    cursor.skipWhitespace();
    if (require_end && !cursor.atEnd()) {
        throw std::invalid_argument(
            "render config has trailing bytes after its root object");
    }
    return result;
}

RootLocation locateRootMembers(std::string_view bytes,
    std::optional<std::string_view> selected_key = std::nullopt) {
    return locateObjectMembers(JsonCursor{bytes}, true, selected_key);
}

// The `rendering` section of a graph root, located in the same bytes so every
// offset stays absolute and the document digest keeps covering the whole file.
RootLocation locateSectionMembers(std::string_view bytes,
    std::optional<std::string_view> selected_key = std::nullopt) {
    const auto root = locateRootMembers(bytes, renderingSectionKey);
    if (!root.selected_found) throw RenderConfigSectionUnlocated{};
    JsonCursor cursor{bytes, root.selected_begin};
    cursor.skipWhitespace();
    if (cursor.peek() != '{') throw RenderConfigSectionUnlocated{};
    return locateObjectMembers(cursor, false, selected_key);
}

std::string encodedJsonString(std::string_view value) {
    return nlohmann::json(std::string{value}).dump();
}

bool containsNewline(std::string_view value) {
    return value.find('\n') != std::string_view::npos ||
           value.find('\r') != std::string_view::npos;
}

std::pair<std::size_t, std::string_view> lastLineBreak(
    std::string_view value) {
    const auto lf = value.find_last_of('\n');
    if (lf != std::string_view::npos) {
        if (lf > 0 && value[lf - 1] == '\r') {
            return {lf - 1, value.substr(lf - 1, 2)};
        }
        return {lf, value.substr(lf, 1)};
    }
    const auto cr = value.find_last_of('\r');
    if (cr != std::string_view::npos) {
        return {cr, value.substr(cr, 1)};
    }
    return {std::string_view::npos, {}};
}

std::string_view horizontalWhitespaceBefore(
    std::string_view bytes, std::size_t position) {
    auto begin = position;
    while (begin > 0 &&
           (bytes[begin - 1] == ' ' || bytes[begin - 1] == '\t')) {
        --begin;
    }
    return bytes.substr(begin, position - begin);
}

struct RootInsertion {
    std::size_t offset = 0;
    std::string bytes;
};

RootInsertion missingRootMemberInsertion(
    std::string_view bytes, const RootLocation &location, std::string_view member) {
    if (!location.has_members) {
        const auto interior = bytes.substr(
            location.root_open + 1,
            location.root_close - location.root_open - 1);
        if (!containsNewline(interior)) {
            return {.offset = location.root_close,
                    .bytes = std::string{member}};
        }

        const auto [newline, line_break] = lastLineBreak(interior);
        const auto closing_indent = interior.substr(
            newline + line_break.size());
        std::string insertion{closing_indent};
        insertion += "  ";
        insertion += member;
        insertion += line_break;
        return {
            .offset = location.root_open + 1 + newline +
                      line_break.size(),
            .bytes = std::move(insertion),
        };
    }

    const auto closing_whitespace = bytes.substr(
        location.last_member_value_end,
        location.root_close - location.last_member_value_end);
    std::string insertion{","};
    if (containsNewline(closing_whitespace)) {
        const auto [newline, line_break] =
            lastLineBreak(closing_whitespace);
        (void)newline;
        insertion += line_break;
        insertion += horizontalWhitespaceBefore(
            bytes, location.last_member_key);
    } else {
        insertion += " ";
    }
    insertion += member;
    return {
        .offset = location.last_member_value_end,
        .bytes = std::move(insertion),
    };
}

template <class FeatureTokens>
std::string separatorForAppend(
    std::string_view bytes, std::size_t open, std::size_t close,
    const FeatureTokens &tokens) {
    if (tokens.size() >= 2) {
        return std::string{bytes.substr(
            tokens[tokens.size() - 2].end,
            tokens.back().begin - tokens[tokens.size() - 2].end)};
    }

    const auto prefix = bytes.substr(open + 1,
                                     tokens.front().begin - open - 1);
    if (containsNewline(prefix)) {
        const auto [newline, line_break] = lastLineBreak(prefix);
        return "," + std::string{line_break} +
               std::string{prefix.substr(
                   newline + line_break.size())};
    }
    const auto suffix = bytes.substr(tokens.front().end,
                                     close - tokens.front().end);
    if (!prefix.empty() || !suffix.empty()) return ", ";
    return ", ";
}

} // namespace

std::string renderConfigSourceDigest(std::string_view bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

RenderConfigSectionUnlocated::RenderConfigSectionUnlocated()
    : std::invalid_argument(
          "graph root render config has no rendering section object") {}

RenderConfigLegacyRoot::RenderConfigLegacyRoot()
    : std::invalid_argument(
          std::string{renderConfigLegacyRootCode} +
          ": the rendering config root is not a pelican.graph document; this "
          "engine reads only the graph root. Move the old root under "
          "\"rendering\" of {\"schema\": \"pelican.graph\", \"version\": 1, "
          "\"definitions\": {}, \"nodes\": [], \"outputs\": {}}") {}

bool isGraphRootKey(std::string_view key) {
    // `layout` is the editor's own placement (N-6). It is a graph root member
    // like the four that carry the graph itself, so it must not be written into
    // the `rendering` section by a caller that reaches for withSectionValue.
    return key == "schema" || key == "version" || key == "definitions" ||
           key == "nodes" || key == "outputs" || key == "layout";
}

RenderConfigCandidateDocumentSet::RenderConfigCandidateDocumentSet(
    std::string root_normalized_reference,
    std::vector<RenderConfigCandidateDocument> documents)
    : root_normalized_reference_{std::move(root_normalized_reference)},
      documents_{std::move(documents)} {
    if (root_normalized_reference_.empty()) {
        throw std::invalid_argument(
            "render config candidate set requires a normalized root reference");
    }
    if (documents_.empty()) {
        throw std::invalid_argument(
            "render config candidate set must not be empty");
    }
    std::vector<std::string_view> keys;
    keys.reserve(documents_.size());
    bool found_root = false;
    for (const auto &document : documents_) {
        if (document.reference.empty() ||
            document.normalized_reference.empty() || document.path.empty()) {
            throw std::invalid_argument(
                "render config candidate document requires reference, normalized reference and path");
        }
        if (std::find(keys.begin(), keys.end(),
                      document.normalized_reference) != keys.end()) {
            throw std::invalid_argument(
                "render config candidate document reference is duplicated: " +
                document.reference);
        }
        keys.push_back(document.normalized_reference);
        if (document.expected.existence ==
            RenderConfigDocumentExistence::present) {
            if (document.expected.digest.size() != 64) {
                throw std::invalid_argument(
                    "present render config candidate document requires a digest: " +
                    document.reference);
            }
        } else if (!document.expected.digest.empty()) {
            throw std::invalid_argument(
                "missing render config candidate document must not carry a digest: " +
                document.reference);
        }
        if (document.operation == RenderConfigDocumentOperation::create &&
            document.expected.existence !=
                RenderConfigDocumentExistence::missing) {
            throw std::invalid_argument(
                "render config create candidate must expect a missing document: " +
                document.reference);
        }
        if ((document.operation == RenderConfigDocumentOperation::replace ||
             document.operation == RenderConfigDocumentOperation::erase) &&
            document.expected.existence !=
                RenderConfigDocumentExistence::present) {
            throw std::invalid_argument(
                "render config replace/delete candidate must expect a present document: " +
                document.reference);
        }
        if (document.operation == RenderConfigDocumentOperation::erase &&
            !document.bytes.empty()) {
            throw std::invalid_argument(
                "render config delete candidate must not carry bytes: " +
                document.reference);
        }
        if (document.normalized_reference == root_normalized_reference_) {
            if (document.operation == RenderConfigDocumentOperation::erase) {
                throw std::invalid_argument(
                    "render config candidate root cannot be deleted");
            }
            found_root = true;
        }
    }
    if (!found_root) {
        throw std::invalid_argument(
            "render config candidate set does not contain its root document");
    }
}

const RenderConfigCandidateDocument &
RenderConfigCandidateDocumentSet::rootDocument() const {
    const auto *root = find(root_normalized_reference_);
    if (root == nullptr) {
        throw std::logic_error(
            "render config candidate set lost its root document");
    }
    return *root;
}

const RenderConfigCandidateDocument *RenderConfigCandidateDocumentSet::find(
    std::string_view normalized_reference) const noexcept {
    const auto found = std::find_if(
        documents_.begin(), documents_.end(), [&](const auto &document) {
            return document.normalized_reference == normalized_reference;
        });
    return found == documents_.end() ? nullptr : &*found;
}

AuthoredRenderConfigDocument::AuthoredRenderConfigDocument(
    std::string bytes, bool require_features)
    : bytes_{std::move(bytes)}, digest_{renderConfigSourceDigest(bytes_)} {
    try {
        const auto semantic = nlohmann::json::parse(bytes_);
        if (!semantic.is_object()) {
            throw std::invalid_argument("render config root must be an object");
        }
        if (semantic.value("schema", std::string{}) != "pelican.graph")
            throw RenderConfigLegacyRoot{};
        // The graph root keeps the authored rendering config one level down.
        // Everything below reads and writes that object, so `features` at the
        // root of a graph document is not this document's features array.
        const auto found = semantic.find(std::string{renderingSectionKey});
        if (found == semantic.end() || !found->is_object()) {
            if (require_features) throw RenderConfigSectionUnlocated{};
            return;
        }
        const auto *section = &*found;
        section_located_ = true;
        if (section->contains("features") &&
            !section->at("features").is_array()) {
            throw std::invalid_argument(
                "render config requires a top-level features array");
        }
        if (require_features && !section->contains("features")) {
            throw std::invalid_argument(
                "render config requires a top-level features array");
        }
    } catch (const nlohmann::json::exception &error) {
        throw std::invalid_argument(
            std::string{"render config is not valid JSON: "} + error.what());
    }
    if (!section_located_) return;

    const auto location = locateSectionMembers(bytes_);
    if (!location.found) {
        if (require_features) {
            throw std::invalid_argument(
                "render config requires a top-level features array");
        }
        return;
    }
    has_features_array_ = true;
    features_open_ = location.open;
    features_close_ = location.close;
    feature_tokens_.reserve(location.elements.size());
    feature_references_.reserve(location.elements.size());
    for (const auto &element : location.elements) {
        feature_tokens_.push_back(FeatureToken{
            .begin = element.begin,
            .end = element.end,
            .editable = element.editable,
            .reference = element.reference,
        });
        if (element.editable) {
            feature_references_.push_back(element.reference);
        } else {
            ++uneditable_feature_entry_count_;
        }
    }
}

AuthoredRenderConfigDocument
AuthoredRenderConfigDocument::initialize(std::string bytes) {
    try {
        const auto semantic = nlohmann::json::parse(bytes);
        if (!semantic.is_object()) {
            throw std::invalid_argument(
                "render config root must be an object");
        }
        if (semantic.value("schema", std::string{}) != "pelican.graph")
            throw RenderConfigLegacyRoot{};
        const auto found = semantic.find(std::string{renderingSectionKey});
        // The features array belongs in the section the composer reads.
        // Without one there is no silent root member to fall back on.
        if (found == semantic.end() || !found->is_object()) {
            throw RenderConfigSectionUnlocated{};
        }
        if (found->contains("features")) {
            return parse(std::move(bytes));
        }
    } catch (const nlohmann::json::exception &error) {
        throw std::invalid_argument(
            std::string{"render config is not valid JSON: "} +
            error.what());
    }

    const auto location = locateSectionMembers(bytes);
    const auto insertion = missingRootMemberInsertion(bytes, location, "\"features\": []");
    bytes.insert(insertion.offset, insertion.bytes);
    return parse(std::move(bytes));
}

AuthoredRenderConfigDocument
AuthoredRenderConfigDocument::inspect(std::string bytes) {
    return AuthoredRenderConfigDocument{std::move(bytes), false};
}

AuthoredRenderConfigDocument
AuthoredRenderConfigDocument::parse(std::string bytes) {
    return AuthoredRenderConfigDocument{std::move(bytes), true};
}

AuthoredRenderConfigDocument AuthoredRenderConfigDocument::withRootValue(
    std::string_view key, std::string_view json_value) const {
    if (!nlohmann::json::accept(json_value))
        throw std::invalid_argument("render config root replacement must be one valid JSON value");
    JsonCursor value_cursor{json_value};
    value_cursor.skipWhitespace();
    const auto value_begin = value_cursor.position();
    value_cursor.skipValue();
    const auto value = json_value.substr(value_begin, value_cursor.position() - value_begin);
    const auto location = locateRootMembers(bytes_, key);
    auto edited = bytes_;
    if (location.selected_found) {
        edited.replace(location.selected_begin, location.selected_end - location.selected_begin, value);
    } else {
        const auto member = encodedJsonString(key) + ": " + std::string{value};
        const auto insertion = missingRootMemberInsertion(bytes_, location, member);
        edited.insert(insertion.offset, insertion.bytes);
    }
    return inspect(std::move(edited));
}

AuthoredRenderConfigDocument AuthoredRenderConfigDocument::withSectionValue(
    std::string_view key, std::string_view json_value) const {
    if (isGraphRootKey(key)) return withRootValue(key, json_value);
    if (!nlohmann::json::accept(json_value))
        throw std::invalid_argument("render config section replacement must be one valid JSON value");
    JsonCursor value_cursor{json_value};
    value_cursor.skipWhitespace();
    const auto value_begin = value_cursor.position();
    value_cursor.skipValue();
    const auto value = json_value.substr(value_begin, value_cursor.position() - value_begin);
    const auto location = locateSectionMembers(bytes_, key);
    auto edited = bytes_;
    if (location.selected_found) {
        edited.replace(location.selected_begin, location.selected_end - location.selected_begin, value);
    } else {
        const auto member = encodedJsonString(key) + ": " + std::string{value};
        const auto insertion = missingRootMemberInsertion(bytes_, location, member);
        edited.insert(insertion.offset, insertion.bytes);
    }
    return inspect(std::move(edited));
}

bool AuthoredRenderConfigDocument::containsFeature(
    std::string_view reference) const noexcept {
    return std::find(feature_references_.begin(), feature_references_.end(),
                     reference) != feature_references_.end();
}

AuthoredRenderConfigDocument
AuthoredRenderConfigDocument::withFeatureAdded(std::string reference) const {
    if (reference.empty()) {
        throw std::invalid_argument("render feature reference must not be empty");
    }
    if (containsFeature(reference)) return *this;

    auto next = bytes_;
    const auto encoded = encodedJsonString(reference);
    if (feature_tokens_.empty()) {
        const auto interior = std::string_view{bytes_}.substr(
            features_open_ + 1,
            features_close_ - features_open_ - 1);
        if (containsNewline(interior)) {
            const auto [newline, line_break] =
                lastLineBreak(interior);
            const auto closing_indent = interior.substr(
                newline + line_break.size());
            std::string insertion{closing_indent};
            insertion += "  ";
            insertion += encoded;
            insertion += line_break;
            next.insert(features_open_ + 1 +
                            newline + line_break.size(),
                        insertion);
        } else {
            // Preserve every existing whitespace byte and insert only the
            // new JSON string immediately before the closing bracket.
            next.insert(features_close_, encoded);
        }
    } else {
        const auto separator = separatorForAppend(
            bytes_, features_open_, features_close_, feature_tokens_);
        next.insert(feature_tokens_.back().end, separator + encoded);
    }
    return parse(std::move(next));
}

AuthoredRenderConfigDocument
AuthoredRenderConfigDocument::withFeatureRemoved(
    std::string_view reference) const {
    const auto found = std::find_if(
        feature_tokens_.begin(), feature_tokens_.end(),
        [&](const auto &token) {
            return token.editable && token.reference == reference;
        });
    if (found == feature_tokens_.end()) {
        throw std::invalid_argument(
            "render feature is not present in the authored root: " +
            std::string{reference});
    }

    const auto index = static_cast<std::size_t>(
        std::distance(feature_tokens_.begin(), found));
    auto next = bytes_;
    if (feature_tokens_.size() == 1) {
        const auto prefix = std::string_view{bytes_}.substr(
            features_open_ + 1, found->begin - features_open_ - 1);
        const auto suffix = std::string_view{bytes_}.substr(
            found->end, features_close_ - found->end);
        const auto replacement =
            containsNewline(prefix) && containsNewline(suffix)
                ? std::string{suffix}
                : std::string{prefix} + std::string{suffix};
        next.replace(features_open_ + 1,
                     features_close_ - features_open_ - 1,
                     replacement);
    } else if (index + 1 < feature_tokens_.size()) {
        next.erase(found->begin,
                   feature_tokens_[index + 1].begin - found->begin);
    } else {
        next.erase(feature_tokens_[index - 1].end,
                   found->end - feature_tokens_[index - 1].end);
    }
    return parse(std::move(next));
}

} // namespace Pelican
