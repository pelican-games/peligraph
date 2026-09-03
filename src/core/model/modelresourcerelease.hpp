#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace Pelican {

enum class ReleaseStage : std::uint8_t {
    vertbuf_index,
    vertbuf_static_vertex,
    vertbuf_skinned_vertex,
    vertbuf_morph_delta,
    material,
    texture,
};

enum class ReleaseErrorCode : std::uint8_t {
    operation_failed,
    resource_still_referenced,
    owner_unavailable,
};

struct ReleaseError {
    ReleaseStage stage = ReleaseStage::vertbuf_index;
    ReleaseErrorCode code = ReleaseErrorCode::operation_failed;
    std::uint32_t index = 0;

    bool operator==(const ReleaseError &) const = default;
};

struct ReleaseReport {
    static constexpr std::size_t capacity = 8;

    std::array<ReleaseError, capacity> errors{};
    std::uint8_t error_count = 0;
    std::uint32_t overflow_count = 0;

    void add(ReleaseError error) noexcept {
        if (error_count < errors.size()) {
            errors[error_count++] = error;
        } else {
            ++overflow_count;
        }
    }

    void append(const ReleaseReport &other) noexcept {
        for (const auto &error : other.view()) add(error);
        overflow_count += other.overflow_count;
    }

    bool empty() const noexcept {
        return error_count == 0 && overflow_count == 0;
    }

    std::span<const ReleaseError> view() const noexcept {
        return {errors.data(), error_count};
    }
};

static_assert(noexcept(std::declval<ReleaseReport &>().add(
    std::declval<ReleaseError>())));
static_assert(noexcept(std::declval<ReleaseReport &>().append(
    std::declval<const ReleaseReport &>())));

constexpr std::string_view releaseStageName(ReleaseStage stage) noexcept {
    switch (stage) {
    case ReleaseStage::vertbuf_index: return "vertbuf.index";
    case ReleaseStage::vertbuf_static_vertex:
        return "vertbuf.static_vertex";
    case ReleaseStage::vertbuf_skinned_vertex:
        return "vertbuf.skinned_vertex";
    case ReleaseStage::vertbuf_morph_delta:
        return "vertbuf.morph_delta";
    case ReleaseStage::material: return "material";
    case ReleaseStage::texture: return "texture";
    }
    return "unknown";
}

constexpr std::string_view
releaseErrorCodeName(ReleaseErrorCode code) noexcept {
    switch (code) {
    case ReleaseErrorCode::operation_failed: return "operation_failed";
    case ReleaseErrorCode::resource_still_referenced:
        return "resource_still_referenced";
    case ReleaseErrorCode::owner_unavailable: return "owner_unavailable";
    }
    return "unknown";
}

} // namespace Pelican
