#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// WP331's byte-lossless authoring contract is deliberately separate from the
// scene document's semantic encoder.  The root bytes remain authoritative;
// only the lexical span of the top-level features array may be changed.
class AuthoredRenderConfigDocument {
    struct FeatureToken {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::string reference;
    };

    std::string bytes_;
    std::string digest_;
    std::size_t features_open_ = 0;
    std::size_t features_close_ = 0;
    std::vector<FeatureToken> feature_tokens_;
    std::vector<std::string> feature_references_;

    explicit AuthoredRenderConfigDocument(std::string bytes);

  public:
    static AuthoredRenderConfigDocument parse(std::string bytes);

    const std::string &bytes() const noexcept { return bytes_; }
    const std::string &sourceDigest() const noexcept { return digest_; }
    const std::vector<std::string> &featureReferences() const noexcept {
        return feature_references_;
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

} // namespace Pelican
