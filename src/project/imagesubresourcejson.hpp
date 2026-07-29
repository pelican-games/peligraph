#pragma once

#include "imagesubresource.hpp"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

inline std::uint32_t parseImageSubresourceUint(
    const nlohmann::json &object,
    std::string_view field,
    std::uint32_t fallback,
    std::string_view context) {
    if (!object.contains(field)) {
        return fallback;
    }
    const auto &encoded = object.at(field);
    if ((!encoded.is_number_unsigned() &&
         !encoded.is_number_integer()) ||
        (!encoded.is_number_unsigned() &&
         encoded.get<std::int64_t>() < 0)) {
        throw std::runtime_error(
            std::string{context} + "." +
            std::string{field} +
            " must be a non-negative integer");
    }
    const auto value = encoded.get<std::uint64_t>();
    if (value >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} + "." +
            std::string{field} +
            " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

// Parses the shared image-view vocabulary from an object containing an
// optional "subresource" field. Shader ports and raster attachments use the
// same spelling so a view can move between sampled/storage and attachment
// roles without translating its range.
inline std::optional<ImageSubresourceRange>
parseOptionalImageSubresource(
    const nlohmann::json &entry,
    std::string_view context) {
    if (!entry.contains("subresource")) {
        return std::nullopt;
    }
    const auto &encoded = entry.at("subresource");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource must be an object");
    }
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (field.key() != "mip" &&
            field.key() != "mip_count" &&
            field.key() != "layer" &&
            field.key() != "layer_count") {
            throw std::runtime_error(
                std::string{context} +
                ".subresource has unknown field '" +
                field.key() + "'");
        }
    }

    auto mip_count_mode =
        ImageSubresourceMipCountMode::fixed;
    std::uint32_t mip_count = 1;
    if (encoded.contains("mip_count") &&
        encoded.at("mip_count").is_string()) {
        const auto mode =
            encoded.at("mip_count").get<std::string>();
        if (mode != "remaining") {
            throw std::runtime_error(
                std::string{context} +
                ".subresource.mip_count string must be "
                "'remaining'");
        }
        mip_count_mode =
            ImageSubresourceMipCountMode::remaining;
    } else {
        mip_count = parseImageSubresourceUint(
            encoded, "mip_count", 1,
            std::string{context} + ".subresource");
    }

    ImageSubresourceRange result{
        .base_mip_level =
            parseImageSubresourceUint(
                encoded, "mip", 0,
                std::string{context} +
                    ".subresource"),
        .level_count = mip_count,
        .base_array_layer =
            parseImageSubresourceUint(
                encoded, "layer", 0,
                std::string{context} +
                    ".subresource"),
        .layer_count =
            parseImageSubresourceUint(
                encoded, "layer_count", 1,
                std::string{context} +
                    ".subresource"),
        .mip_count_mode = mip_count_mode,
    };
    if ((result.mip_count_mode ==
             ImageSubresourceMipCountMode::fixed &&
         result.level_count == 0) ||
        result.layer_count == 0) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource counts must be positive");
    }
    const auto mip_end =
        result.mip_count_mode ==
                ImageSubresourceMipCountMode::remaining
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(
                  result.base_mip_level) +
                  result.level_count;
    const auto layer_end =
        static_cast<std::uint64_t>(
            result.base_array_layer) +
        result.layer_count;
    if ((result.mip_count_mode ==
             ImageSubresourceMipCountMode::fixed &&
         mip_end >
             std::uint64_t{
                 std::numeric_limits<
                     std::uint32_t>::max()} +
                 1u) ||
        layer_end >
            std::uint64_t{
                std::numeric_limits<
                    std::uint32_t>::max()} +
                1u) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource range overflows uint32");
    }
    return result;
}

inline nlohmann::ordered_json imageSubresourceToJson(
    const ImageSubresourceRange &range) {
    nlohmann::ordered_json result{
        {"mip", range.base_mip_level},
        {"layer", range.base_array_layer},
        {"layer_count", range.layer_count},
    };
    if (range.mip_count_mode ==
        ImageSubresourceMipCountMode::remaining) {
        result["mip_count"] = "remaining";
    } else {
        result["mip_count"] = range.level_count;
    }
    return result;
}

} // namespace Pelican
