#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <glm/vec3.hpp>

namespace Pelican {

inline constexpr uint32_t maxVatVertexCount = 8192;

struct VatBufferViewInfo {
    int index = -1;
    int buffer = -1;
    size_t byte_offset = 0;
    size_t byte_length = 0;
};

struct VatBufferViewSpan {
    int index = -1;
    int buffer = -1;
    size_t byte_offset = 0;
    size_t byte_length = 0;
};

struct VatPrimitiveInfo {
    std::string generator;
    double fps = 0.0;
    uint32_t frame_count = 0;
    uint32_t vertex_count = 0;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    bool loop = false;
    VatBufferViewSpan position_view;
    std::optional<VatBufferViewSpan> normal_view;
};

struct VatPrimitiveMeta {
    bool present = false;
    bool single_clip_object = true;
    std::optional<std::string> schema;
    std::optional<int64_t> version;
    std::optional<std::string> generator;
    std::optional<double> fps;
    std::optional<int64_t> frame_count;
    std::optional<int64_t> vertex_count;
    std::optional<std::array<double, 3>> bounds_min;
    std::optional<std::array<double, 3>> bounds_max;
    std::optional<bool> loop;
    std::optional<int64_t> position_view;
    std::optional<int64_t> normal_view;
};

std::optional<VatPrimitiveInfo> parseVatPrimitiveExtras(
    const VatPrimitiveMeta &primitive_meta,
    uint32_t actual_vertex_count,
    std::span<const VatBufferViewInfo> buffer_views);

} // namespace Pelican
