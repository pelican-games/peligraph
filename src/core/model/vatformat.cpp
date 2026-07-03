#include "vatformat.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

constexpr std::string_view vatSchema = "pelican.vat";
constexpr int vatVersion = 1;
constexpr size_t rgba16fBytesPerSample = 4 * sizeof(uint16_t);

template <class T>
const T &requiredField(const std::optional<T> &value, const char *field) {
    if (!value) {
        throw std::runtime_error(std::string{"pelican.vat requires field: "} + field);
    }
    return *value;
}

uint32_t requiredUint32(const std::optional<int64_t> &value, const char *field) {
    const auto parsed = requiredField(value, field);
    if (parsed < 0 || parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string{"pelican.vat field is out of uint32 range: "} + field);
    }
    return static_cast<uint32_t>(parsed);
}

int requiredNonNegativeInt(const std::optional<int64_t> &value, const char *field) {
    const auto parsed = requiredField(value, field);
    if (parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string{"pelican.vat field is out of bufferView range: "} + field);
    }
    return static_cast<int>(parsed);
}

glm::vec3 requiredVec3(const std::optional<std::array<double, 3>> &value, const char *field) {
    const auto &vec = requiredField(value, field);
    return glm::vec3{
        static_cast<float>(vec[0]),
        static_cast<float>(vec[1]),
        static_cast<float>(vec[2]),
    };
}

size_t requiredVatTextureBytes(uint32_t vertex_count, uint32_t frame_count) {
    const auto max_size = std::numeric_limits<size_t>::max();
    if (vertex_count != 0 && static_cast<size_t>(vertex_count) > max_size / frame_count / rgba16fBytesPerSample) {
        throw std::runtime_error("pelican.vat texture byte size overflow");
    }
    return static_cast<size_t>(vertex_count) * frame_count * rgba16fBytesPerSample;
}

VatBufferViewSpan resolveBufferView(std::span<const VatBufferViewInfo> buffer_views, int index,
                                    size_t required_bytes, const char *field) {
    const auto found = std::find_if(buffer_views.begin(), buffer_views.end(), [index](const auto &view) {
        return view.index == index;
    });
    if (found == buffer_views.end()) {
        throw std::runtime_error(std::string{"pelican.vat "} + field +
                                 " references missing bufferView: " + std::to_string(index));
    }
    if (found->buffer < 0) {
        throw std::runtime_error(std::string{"pelican.vat "} + field +
                                 " references invalid buffer index: " + std::to_string(index));
    }
    if (found->byte_length < required_bytes) {
        throw std::runtime_error(std::string{"pelican.vat "} + field + " bufferView " +
                                 std::to_string(index) + " is too small: need " +
                                 std::to_string(required_bytes) + " bytes, got " +
                                 std::to_string(found->byte_length));
    }
    return VatBufferViewSpan{
        .index = found->index,
        .buffer = found->buffer,
        .byte_offset = found->byte_offset,
        .byte_length = found->byte_length,
    };
}

} // namespace

std::optional<VatPrimitiveInfo> parseVatPrimitiveExtras(
    const VatPrimitiveMeta &primitive_meta,
    uint32_t actual_vertex_count,
    std::span<const VatBufferViewInfo> buffer_views) {
    if (!primitive_meta.present) {
        return std::nullopt;
    }

    if (!primitive_meta.single_clip_object) {
        throw std::runtime_error("pelican.vat must be a single object clip");
    }

    if (requiredField(primitive_meta.schema, "schema") != vatSchema) {
        throw std::runtime_error("pelican.vat schema is not supported");
    }
    if (requiredUint32(primitive_meta.version, "version") != vatVersion) {
        throw std::runtime_error("pelican.vat version is not supported");
    }

    const auto fps = requiredField(primitive_meta.fps, "fps");
    if (fps <= 0.0) {
        throw std::runtime_error("pelican.vat fps must be positive");
    }

    const auto frame_count = requiredUint32(primitive_meta.frame_count, "frame_count");
    if (frame_count == 0) {
        throw std::runtime_error("pelican.vat frame_count must be positive");
    }

    const auto vertex_count = requiredUint32(primitive_meta.vertex_count, "vertex_count");
    if (vertex_count == 0) {
        throw std::runtime_error("pelican.vat vertex_count must be positive");
    }
    if (vertex_count > maxVatVertexCount) {
        throw std::runtime_error("pelican.vat vertex_count exceeds max 8192");
    }
    if (vertex_count != actual_vertex_count) {
        throw std::runtime_error("pelican.vat vertex_count must match primitive POSITION count");
    }

    const auto bounds_min = requiredVec3(primitive_meta.bounds_min, "bounds_min");
    const auto bounds_max = requiredVec3(primitive_meta.bounds_max, "bounds_max");
    if (bounds_max.x <= bounds_min.x || bounds_max.y <= bounds_min.y || bounds_max.z <= bounds_min.z) {
        throw std::runtime_error("pelican.vat bounds_max must be greater than bounds_min on every axis");
    }

    const auto required_bytes = requiredVatTextureBytes(vertex_count, frame_count);
    const auto position_view =
        resolveBufferView(buffer_views, requiredNonNegativeInt(primitive_meta.position_view, "position_view"), required_bytes,
                          "position_view");

    std::optional<VatBufferViewSpan> normal_view;
    if (primitive_meta.normal_view) {
        normal_view =
            resolveBufferView(buffer_views, requiredNonNegativeInt(primitive_meta.normal_view, "normal_view"), required_bytes,
                              "normal_view");
    }

    return VatPrimitiveInfo{
        .generator = primitive_meta.generator.value_or(std::string{}),
        .fps = fps,
        .frame_count = frame_count,
        .vertex_count = vertex_count,
        .bounds_min = bounds_min,
        .bounds_max = bounds_max,
        .loop = requiredField(primitive_meta.loop, "loop"),
        .position_view = position_view,
        .normal_view = normal_view,
    };
}

} // namespace Pelican
