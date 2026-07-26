#pragma once

#include "stablefingerprint.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::size_t maximumMaterialDrawTagBytes = 255;
inline constexpr std::string_view materialDrawTagFilterProvenance =
    "pelican.draw_queue_builder.material_tag_filter@1";

struct MaterialDrawTagFilterId {
    std::uint64_t value = 0;

    bool operator==(const MaterialDrawTagFilterId &) const = default;

    struct Hash {
        std::size_t operator()(MaterialDrawTagFilterId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value);
        }
    };
};

struct MaterialDrawTagFilter {
    // Both lists are canonical (sorted and unique). Include uses any-match
    // semantics; an empty include list accepts every item. Any exclude match
    // rejects the item and therefore takes precedence.
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    MaterialDrawTagFilterId id{};

    bool operator==(const MaterialDrawTagFilter &) const = default;
};

inline void validateMaterialDrawTag(std::string_view tag,
                                    std::string_view context) {
    if (tag.empty() || tag.size() > maximumMaterialDrawTagBytes) {
        throw std::runtime_error(std::string{context} +
                                 " tag is empty or too long");
    }
}

inline std::vector<std::string>
canonicalizeMaterialDrawTags(std::vector<std::string> tags,
                             std::string_view context) {
    for (const auto &tag : tags) {
        validateMaterialDrawTag(tag, context);
    }
    std::sort(tags.begin(), tags.end());
    const auto duplicate =
        std::adjacent_find(tags.begin(), tags.end());
    if (duplicate != tags.end()) {
        throw std::runtime_error(std::string{context} +
                                 " has duplicate tag: " + *duplicate);
    }
    return tags;
}

inline MaterialDrawTagFilterId materialDrawTagFilterId(
    std::span<const std::string> include,
    std::span<const std::string> exclude) noexcept {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(materialDrawTagFilterProvenance);
    fingerprint.appendUnsigned(include.size());
    for (const auto &tag : include) fingerprint.appendString(tag);
    fingerprint.appendUnsigned(exclude.size());
    for (const auto &tag : exclude) fingerprint.appendString(tag);
    return MaterialDrawTagFilterId{fingerprint.value()};
}

inline MaterialDrawTagFilter makeMaterialDrawTagFilter(
    std::vector<std::string> include,
    std::vector<std::string> exclude,
    std::string_view context) {
    MaterialDrawTagFilter result;
    result.include = canonicalizeMaterialDrawTags(
        std::move(include), std::string{context} + " include");
    result.exclude = canonicalizeMaterialDrawTags(
        std::move(exclude), std::string{context} + " exclude");

    std::vector<std::string> overlap;
    std::set_intersection(
        result.include.begin(), result.include.end(),
        result.exclude.begin(), result.exclude.end(),
        std::back_inserter(overlap));
    if (!overlap.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " includes and excludes the same tag: " + overlap.front());
    }
    result.id =
        materialDrawTagFilterId(result.include, result.exclude);
    return result;
}

inline void validateMaterialDrawTagFilter(
    const MaterialDrawTagFilter &filter,
    std::string_view context) {
    const auto canonical = makeMaterialDrawTagFilter(
        filter.include, filter.exclude, context);
    if (canonical != filter) {
        throw std::invalid_argument(
            std::string{context} +
            " must be canonical and carry its matching stable id");
    }
}

inline bool sortedTagsIntersect(
    std::span<const std::string> left,
    std::span<const std::string> right) noexcept {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() &&
           right_index < right.size()) {
        if (left[left_index] < right[right_index]) {
            ++left_index;
        } else if (right[right_index] < left[left_index]) {
            ++right_index;
        } else {
            return true;
        }
    }
    return false;
}

inline bool materialDrawTagFilterMatches(
    std::span<const std::string> material_tags,
    const MaterialDrawTagFilter &filter) noexcept {
    const bool included =
        filter.include.empty() ||
        sortedTagsIntersect(material_tags, filter.include);
    return included &&
           !sortedTagsIntersect(material_tags, filter.exclude);
}

} // namespace Pelican
