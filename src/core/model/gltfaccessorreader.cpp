#include "gltfaccessorreader.hpp"

namespace Pelican::GltfInternal {

std::string accessorContext(const std::string &source_path, int accessor_index) {
    return "glTF accessor " + std::to_string(accessor_index) + " in '" +
           source_path + "'";
}

AccessorByteRange resolveAccessorByteRange(const tinygltf::Model &model,
                                           const std::string &context,
                                           int accessor_index,
                                           int expected_type) {
    if (accessor_index < 0 ||
        accessor_index >= static_cast<int>(model.accessors.size())) {
        throw std::runtime_error(context + " is out of range");
    }
    const auto &accessor = model.accessors.at(
        static_cast<std::size_t>(accessor_index));
    if (expected_type != accessor.type) {
        throw std::runtime_error(context + " has unexpected accessor type " +
                                 std::to_string(accessor.type));
    }
    if (accessor.sparse.isSparse) {
        throw std::runtime_error(context +
                                 " uses an unsupported sparse accessor");
    }
    if (accessor.bufferView < 0 ||
        accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
        throw std::runtime_error(context + " has no valid bufferView");
    }
    const auto &buffer_view = model.bufferViews.at(
        static_cast<std::size_t>(accessor.bufferView));
    if (buffer_view.buffer < 0 ||
        buffer_view.buffer >= static_cast<int>(model.buffers.size())) {
        throw std::runtime_error(context +
                                 " bufferView references an invalid buffer");
    }
    const auto &buffer = model.buffers.at(
        static_cast<std::size_t>(buffer_view.buffer));
    const auto component_size =
        tinygltf::GetComponentSizeInBytes(accessor.componentType);
    const auto component_count =
        tinygltf::GetNumComponentsInType(accessor.type);
    if (component_size <= 0 || component_count <= 0) {
        throw std::runtime_error(context +
                                 " has an unsupported component encoding");
    }
    const auto element_size = static_cast<std::size_t>(component_size) *
                              static_cast<std::size_t>(component_count);
    const auto stride_value = accessor.ByteStride(buffer_view);
    if (stride_value < 0 ||
        static_cast<std::size_t>(stride_value) < element_size) {
        throw std::runtime_error(context + " has an invalid byte stride");
    }
    const auto stride = static_cast<std::size_t>(stride_value);
    const auto view_begin = static_cast<std::size_t>(buffer_view.byteOffset);
    const auto view_size = static_cast<std::size_t>(buffer_view.byteLength);
    if (view_begin > buffer.data.size() ||
        view_size > buffer.data.size() - view_begin) {
        throw std::runtime_error(context + " bufferView exceeds its buffer");
    }
    const auto accessor_offset =
        static_cast<std::size_t>(accessor.byteOffset);
    if (accessor_offset > view_size) {
        throw std::runtime_error(context +
                                 " byteOffset exceeds its bufferView");
    }
    const auto count = static_cast<std::size_t>(accessor.count);
    std::size_t required = 0;
    if (count != 0) {
        if ((count - 1) >
            (std::numeric_limits<std::size_t>::max() - element_size) /
                stride) {
            throw std::runtime_error(context + " byte range overflows");
        }
        required = (count - 1) * stride + element_size;
    }
    const auto available = view_size - accessor_offset;
    const auto begin = view_begin + accessor_offset;
    if (required > available || required > buffer.data.size() - begin) {
        throw std::runtime_error(context +
                                 " data exceeds its bufferView");
    }
    const auto *p_data = count == 0 ? nullptr : buffer.data.data() + begin;
    return AccessorByteRange{p_data,
                             count,
                             stride,
                             element_size,
                             accessor.componentType,
                             accessor.normalized};
}

} // namespace Pelican::GltfInternal
