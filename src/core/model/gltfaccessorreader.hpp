#pragma once

#include <tiny_gltf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Pelican::GltfInternal {

// One accessor's element bytes, after every bounds check the importer makes.
// `data` is null exactly when `count` is zero; otherwise element `i` begins at
// `data + stride * i` and is `element_size` bytes long.
struct AccessorByteRange {
    const unsigned char *data = nullptr;
    std::size_t count = 0;
    std::size_t stride = 0;
    std::size_t element_size = 0;
    int component_type = 0;
    bool normalized = false;
};

// The diagnostic prefix every accessor error carries. The wording is part of
// the loader's observable contract, so it is spelled in exactly one place.
std::string accessorContext(const std::string &source_path, int accessor_index);

// Walks accessor -> bufferView -> buffer for `accessor_index` and rejects
// everything the importer does not decode: an out-of-range index, an accessor
// whose `type` is not `expected_type`, a sparse accessor, a missing or invalid
// bufferView/buffer, an unsupported component encoding, a stride narrower than
// one element, and any byte range that leaves its bufferView or its buffer.
// `context` comes from `accessorContext`. Throws std::runtime_error.
AccessorByteRange resolveAccessorByteRange(const tinygltf::Model &model,
                                           const std::string &context,
                                           int accessor_index,
                                           int expected_type);

// Typed decode of one glTF accessor. Holds references only: construct it at
// the call site, beside the model it reads.
struct GltfAccessorReader {
    const tinygltf::Model &model;
    const std::string &source_path;

    template <class InType, class OutType>
    std::vector<OutType> readComponentByType(const unsigned char *p_data,
                                             std::size_t count,
                                             std::size_t stride,
                                             std::size_t element_size) {
        if (sizeof(InType) < element_size) {
            throw std::runtime_error(
                "glTF accessor element does not fit its decoded component type");
        }
        std::vector<OutType> buf(count);
        for (std::size_t i = 0; i < count; ++i) {
            InType value{};
            std::memcpy(&value, p_data + stride * i, element_size);
            buf[i] = static_cast<OutType>(value);
        }
        return buf;
    }

    template <class InType>
    static float normalizedIntegerComponent(InType value) {
        static_assert(std::is_integral_v<InType>);
        if constexpr (std::is_unsigned_v<InType>) {
            return static_cast<float>(value) /
                   static_cast<float>(
                       std::numeric_limits<InType>::max());
        } else {
            return std::max(
                static_cast<float>(value) /
                    static_cast<float>(
                        std::numeric_limits<InType>::max()),
                -1.0F);
        }
    }

    template <class InType, class OutType>
    std::vector<OutType>
    readNormalizedScalar(const unsigned char *p_data,
                         std::size_t count,
                         std::size_t stride,
                         std::size_t element_size) {
        if (element_size != sizeof(InType)) {
            throw std::runtime_error(
                "glTF normalized scalar has an invalid element size");
        }
        std::vector<OutType> result(count);
        for (std::size_t index = 0; index < count; ++index) {
            InType value{};
            std::memcpy(
                &value, p_data + stride * index,
                sizeof(value));
            result[index] = static_cast<OutType>(
                normalizedIntegerComponent(value));
        }
        return result;
    }

    template <class InType, class OutType,
              std::size_t ComponentCount>
    std::vector<OutType>
    readNormalizedVector(const unsigned char *p_data,
                         std::size_t count,
                         std::size_t stride,
                         std::size_t element_size) {
        if (element_size !=
            sizeof(InType) * ComponentCount) {
            throw std::runtime_error(
                "glTF normalized vector has an invalid element size");
        }
        using OutComponent = typename OutType::value_type;
        std::vector<OutType> result(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto &decoded = result[index];
            for (std::size_t component = 0;
                 component < ComponentCount;
                 ++component) {
                InType value{};
                std::memcpy(
                    &value,
                    p_data + stride * index +
                        sizeof(InType) * component,
                    sizeof(value));
                decoded[
                    static_cast<glm::length_t>(component)] =
                    static_cast<OutComponent>(
                        normalizedIntegerComponent(value));
            }
        }
        return result;
    }

    template <int expected_type, class T> std::vector<T> getDataFromAccessor(int accessor_index) {
        const auto context = accessorContext(source_path, accessor_index);
        const auto range =
            resolveAccessorByteRange(model, context, accessor_index, expected_type);
        const auto count = range.count;
        const auto stride = range.stride;
        const auto element_size = range.element_size;
        const auto *p_data = range.data;

        if (range.normalized) {
            if constexpr (
                expected_type ==
                TINYGLTF_TYPE_SCALAR) {
                if constexpr (
                    !std::is_floating_point_v<T>) {
                    throw std::runtime_error(
                        context +
                        " is normalized but its destination is "
                        "not floating-point");
                }
            } else if constexpr (
                expected_type == TINYGLTF_TYPE_VEC2 ||
                expected_type == TINYGLTF_TYPE_VEC3 ||
                expected_type == TINYGLTF_TYPE_VEC4) {
                if constexpr (
                    !std::is_floating_point_v<
                        typename T::value_type>) {
                    throw std::runtime_error(
                        context +
                        " is normalized but its destination is "
                        "not floating-point");
                }
            } else {
                throw std::runtime_error(
                    context +
                    " uses normalized components for an "
                    "unsupported accessor shape");
            }

            const auto read_normalized =
                [&]<class InType>() -> std::vector<T> {
                if constexpr (
                    expected_type ==
                    TINYGLTF_TYPE_SCALAR) {
                    return readNormalizedScalar<
                        InType, T>(
                        p_data, count, stride,
                        element_size);
                } else if constexpr (
                    expected_type ==
                    TINYGLTF_TYPE_VEC2) {
                    return readNormalizedVector<
                        InType, T, 2>(
                        p_data, count, stride,
                        element_size);
                } else if constexpr (
                    expected_type ==
                    TINYGLTF_TYPE_VEC3) {
                    return readNormalizedVector<
                        InType, T, 3>(
                        p_data, count, stride,
                        element_size);
                } else if constexpr (
                    expected_type ==
                    TINYGLTF_TYPE_VEC4) {
                    return readNormalizedVector<
                        InType, T, 4>(
                        p_data, count, stride,
                        element_size);
                } else {
                    throw std::logic_error(
                        "unreachable normalized accessor shape");
                }
            };
            switch (range.component_type) {
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return read_normalized
                    .template operator()<std::int8_t>();
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return read_normalized
                    .template operator()<std::int16_t>();
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return read_normalized
                    .template operator()<std::uint8_t>();
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return read_normalized
                    .template operator()<std::uint16_t>();
            default:
                throw std::runtime_error(
                    context +
                    " uses normalized components with an "
                    "unsupported component type " +
                    std::to_string(
                        range.component_type));
            }
        }

        if constexpr (expected_type == TINYGLTF_TYPE_SCALAR) {
            switch (range.component_type) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<float, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<double, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<int8_t, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<int16_t, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<int32_t, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<uint8_t, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<uint16_t, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<uint32_t, T>(p_data, count, stride, element_size);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC2) {
            switch (range.component_type) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec2, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec2, T>(p_data, count, stride, element_size);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC3) {
            switch (range.component_type) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec3, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec3, T>(p_data, count, stride, element_size);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC4) {
            switch (range.component_type) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec4, T>(p_data, count, stride, element_size);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec4, T>(p_data, count, stride, element_size);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_MAT4) {
            if (range.component_type == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                return readComponentByType<glm::mat4, T>(p_data, count, stride, element_size);
            }
        }
        throw std::runtime_error(
            context + " has unsupported component type " +
            std::to_string(range.component_type));
    }

};

} // namespace Pelican::GltfInternal
