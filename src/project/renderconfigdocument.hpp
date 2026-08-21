#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// WP331's byte-lossless authoring contract is deliberately separate from the
// scene document's semantic encoder.  The root bytes remain authoritative;
// only the lexical span of the top-level features array may be changed after
// a byte-lossless root initialization has established that span.
class AuthoredRenderConfigDocument {
    struct FeatureToken {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool editable = false;
        std::string reference;
    };

    std::string bytes_;
    std::string digest_;
    bool has_features_array_ = false;
    std::size_t features_open_ = 0;
    std::size_t features_close_ = 0;
    std::vector<FeatureToken> feature_tokens_;
    std::vector<std::string> feature_references_;
    std::size_t uneditable_feature_entry_count_ = 0;

    explicit AuthoredRenderConfigDocument(std::string bytes,
                                          bool require_features);

  public:
    static AuthoredRenderConfigDocument initialize(std::string bytes);
    // Read-only inspection accepts a valid legacy root without features[].
    // initialize() remains the explicit transition to the editable form.
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

    bool containsFeature(std::string_view reference) const noexcept;
    AuthoredRenderConfigDocument withFeatureAdded(
        std::string reference) const;
    AuthoredRenderConfigDocument withFeatureRemoved(
        std::string_view reference) const;
};

std::string renderConfigSourceDigest(std::string_view bytes);

// A render-authoring candidate is a document set, not a replacement string
// for the root document. References use PathResolver's canonical key so an
// alias cannot bypass the request-local overlay.
enum class RenderConfigDocumentOperation {
    create,
    replace,
    erase,
};

enum class RenderConfigDocumentExistence {
    missing,
    present,
};

struct RenderConfigDocumentExpectedState {
    RenderConfigDocumentExistence existence =
        RenderConfigDocumentExistence::missing;
    std::string digest;

    bool operator==(const RenderConfigDocumentExpectedState &) const = default;
};

struct RenderConfigCandidateDocument {
    std::string reference;
    std::string normalized_reference;
    std::filesystem::path path;
    RenderConfigDocumentOperation operation =
        RenderConfigDocumentOperation::replace;
    RenderConfigDocumentExpectedState expected;
    // Empty for erase; otherwise the exact candidate bytes.
    std::string bytes;

    bool operator==(const RenderConfigCandidateDocument &) const = default;
};

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
