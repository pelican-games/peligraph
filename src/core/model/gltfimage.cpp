#include "gltfimage.hpp"

#include "../parallel_prepare.hpp"
#include <stb_image.h>

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican::GltfInternal {
namespace {

struct DecodedImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    int component = 4;
};

DecodedImage decodeImage(const EncodedImage &encoded) {
    if (encoded.bytes.empty() ||
        encoded.bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(
            "encoded glTF image is empty or exceeds stb_image input limits");
    }

    DecodedImage decoded;
    int source_components = 0;
    const auto *data = encoded.bytes.data();
    const auto size = static_cast<int>(encoded.bytes.size());
    // Material storage has paired RGBA8 UNORM/SRGB views. stbi_load performs
    // the deliberate 16-bit PNG -> 8-bit conversion here, before the bytes
    // cross into the fixed RGBA8 Vulkan upload path.
    auto *pixels = stbi_load_from_memory(data, size, &decoded.width,
                                         &decoded.height,
                                         &source_components, 4);
    if (pixels == nullptr) {
        throw std::runtime_error(
            std::string{"failed to decode glTF image: "} +
            stbi_failure_reason());
    }
    if (decoded.width <= 0 || decoded.height <= 0 ||
        static_cast<std::size_t>(decoded.width) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(decoded.height) / 4) {
        stbi_image_free(pixels);
        throw std::runtime_error(
            "decoded glTF image dimensions exceed RGBA8 storage limits");
    }
    const auto bytes = static_cast<std::size_t>(decoded.width) *
                       static_cast<std::size_t>(decoded.height) * 4;
    decoded.pixels.assign(pixels, pixels + bytes);
    stbi_image_free(pixels);

    if ((encoded.requested_width != 0 &&
         decoded.width != encoded.requested_width) ||
        (encoded.requested_height != 0 &&
         decoded.height != encoded.requested_height)) {
        throw std::runtime_error(
            "decoded glTF image dimensions do not match the declared dimensions");
    }
    return decoded;
}

} // namespace

bool retainEncodedImage(tinygltf::Image *image, int image_index, std::string *,
                        std::string *, int requested_width,
                        int requested_height, const unsigned char *bytes,
                        int byte_count, void *user_data) {
    if (image == nullptr || user_data == nullptr || bytes == nullptr ||
        byte_count <= 0 || image_index < 0) {
        return false;
    }
    auto &encoded = *static_cast<EncodedImages *>(user_data);
    if (encoded.size() <= static_cast<std::size_t>(image_index)) {
        encoded.resize(static_cast<std::size_t>(image_index) + 1);
    }
    encoded[static_cast<std::size_t>(image_index)] = EncodedImage{
        .bytes = std::vector<unsigned char>{bytes, bytes + byte_count},
        .requested_width = requested_width,
        .requested_height = requested_height,
    };
    image->image.clear();
    return true;
}

void decodeImagesInParallel(tinygltf::Model &model,
                            const EncodedImages &encoded) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index]) indices.push_back(index);
    }
    auto decoded = parallelPrepareOrdered<DecodedImage>(
        indices.size(),
        [&](std::size_t slot) { return decodeImage(*encoded[indices[slot]]); },
        4);
    for (std::size_t slot = 0; slot < indices.size(); ++slot) {
        auto &image = model.images.at(indices[slot]);
        image.image = std::move(decoded[slot].pixels);
        image.width = decoded[slot].width;
        image.height = decoded[slot].height;
        image.component = decoded[slot].component;
        image.bits = 8;
        image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    }
}

void validateRgba8Image(const tinygltf::Image &image,
                        std::string_view source_name) {
    const auto label = source_name.empty()
                           ? std::string{"glTF image"}
                           : "glTF image '" + std::string{source_name} + "'";
    if (image.width <= 0 || image.height <= 0) {
        throw std::runtime_error(label +
                                 " has invalid decoded dimensions");
    }
    if (image.component != 4 || image.bits != 8 ||
        image.pixel_type !=
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        throw std::runtime_error(
            label +
            " is not decoded as the required RGBA8 unsigned-byte format");
    }

    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    if (width >
        std::numeric_limits<std::size_t>::max() / height / 4) {
        throw std::runtime_error(label +
                                 " dimensions overflow RGBA8 byte size");
    }
    const auto expected_bytes = width * height * 4;
    if (image.image.size() != expected_bytes) {
        throw std::runtime_error(
            label + " RGBA8 byte size mismatch: expected " +
            std::to_string(expected_bytes) + ", got " +
            std::to_string(image.image.size()));
    }
}

} // namespace Pelican::GltfInternal
