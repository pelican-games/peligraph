#pragma once

#include "projectdocumenttransaction.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// P-2a-4a: a `pelican.graph` root owns five keys of its own and keeps the
// whole legacy rendering config under `rendering`. One predicate decides
// where a key lives so no caller grows its own `if (graph)`.
bool isGraphRootKey(std::string_view key);

// A graph root without an object `rendering` member has nowhere to put an
// authored key. Authoring says so by name instead of writing a member the
// composer never reads.
class RenderConfigSectionUnlocated : public std::invalid_argument {
  public:
    RenderConfigSectionUnlocated();
};

// MG-2b: the authored rendering config root is a `pelican.graph` document and
// nothing else. A root in the older form (the rendering config's own keys at
// the root) is not read: every reader of an authored root stops on it by this
// name. It is rewritten by hand as
//   {"schema": "pelican.graph", "version": 1, "definitions": {},
//    "nodes": [], "outputs": {}, "rendering": <the old root>}
// which is the same config -- the section is read exactly as the old root was.
inline constexpr std::string_view renderConfigLegacyRootCode =
    "render_config.legacy_root@1";

class RenderConfigLegacyRoot : public std::invalid_argument {
  public:
    RenderConfigLegacyRoot();
};

// WP331's byte-lossless authoring contract is deliberately separate from the
// scene document's semantic encoder.  The root bytes remain authoritative;
// edits replace only the selected lexical value span or insert one root member.
class AuthoredRenderConfigDocument {
    struct FeatureToken {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool editable = false;
        std::string reference;
    };

    std::string bytes_;
    std::string digest_;
    bool section_located_ = false;
    bool has_features_array_ = false;
    std::size_t features_open_ = 0;
    std::size_t features_close_ = 0;
    std::vector<FeatureToken> feature_tokens_;
    std::vector<std::string> feature_references_;
    std::size_t uneditable_feature_entry_count_ = 0;

    explicit AuthoredRenderConfigDocument(std::string bytes,
                                          bool require_features);

  public:
    // All three refuse a root that is not `pelican.graph` with
    // RenderConfigLegacyRoot.
    static AuthoredRenderConfigDocument initialize(std::string bytes);
    // Read-only inspection accepts a graph root without features[] (or
    // without a rendering section). initialize() remains the explicit
    // transition to the editable form.
    static AuthoredRenderConfigDocument inspect(std::string bytes);
    static AuthoredRenderConfigDocument parse(std::string bytes);

    const std::string &bytes() const noexcept { return bytes_; }
    const std::string &sourceDigest() const noexcept { return digest_; }
    const std::vector<std::string> &featureReferences() const noexcept {
        return feature_references_;
    }
    bool hasFeaturesArray() const noexcept { return has_features_array_; }
    std::size_t uneditableFeatureEntryCount() const noexcept {
        return uneditable_feature_entry_count_;
    }
    std::size_t featuresArrayOpenOffset() const noexcept {
        return features_open_;
    }
    std::size_t featuresArrayCloseOffset() const noexcept {
        return features_close_;
    }

    // False when the graph root's `rendering` member is missing or not an
    // object.
    bool hasRenderingSection() const noexcept { return section_located_; }

    AuthoredRenderConfigDocument withRootValue(
        std::string_view key, std::string_view json_value) const;
    // FG-1: takes the root member `key` out with its comma, every other byte
    // as it was; an absent key changes nothing.
    AuthoredRenderConfigDocument withoutRootValue(std::string_view key) const;
    // Writes the key where the composer reads it: the root for a graph
    // root's own keys, `rendering.<key>` otherwise.
    AuthoredRenderConfigDocument withSectionValue(
        std::string_view key, std::string_view json_value) const;
    // EL-3: takes `rendering.<key>` out with its comma, every other byte as
    // it was; an absent key changes nothing. A graph root key is refused.
    AuthoredRenderConfigDocument withoutSectionValue(std::string_view key) const;

    bool containsFeature(std::string_view reference) const noexcept;
    AuthoredRenderConfigDocument withFeatureAdded(
        std::string reference) const;
    AuthoredRenderConfigDocument withFeatureRemoved(
        std::string_view reference) const;
};

std::string renderConfigSourceDigest(std::string_view bytes);

// A render-authoring candidate is a document set, not a replacement string
// for the root document. References use PathResolver's canonical key so an
// alias cannot bypass the request-local overlay. The members are the project
// document transaction's (MG-1), which the render config editor shares with
// `project migrate`.
using RenderConfigDocumentOperation = ProjectDocumentOperation;
using RenderConfigDocumentExistence = ProjectDocumentExistence;
using RenderConfigDocumentExpectedState = ProjectDocumentExpectedState;
using RenderConfigCandidateDocument = ProjectCandidateDocument;

class RenderConfigCandidateDocumentSet {
    std::string root_normalized_reference_;
    std::vector<RenderConfigCandidateDocument> documents_;

  public:
    RenderConfigCandidateDocumentSet(
        std::string root_normalized_reference,
        std::vector<RenderConfigCandidateDocument> documents);

    const std::string &rootNormalizedReference() const noexcept {
        return root_normalized_reference_;
    }
    const std::vector<RenderConfigCandidateDocument> &documents() const
        noexcept {
        return documents_;
    }
    const RenderConfigCandidateDocument &rootDocument() const;
    const RenderConfigCandidateDocument *find(
        std::string_view normalized_reference) const noexcept;
};

} // namespace Pelican
