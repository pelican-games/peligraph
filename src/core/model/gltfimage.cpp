#include "gltfimage.hpp"

#include "../parallel_prepare.hpp"
#include <stb_image.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace Pelican::GltfInternal {
namespace {

struct DecodedImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    int component = 4;
    int bits = 8;
    int pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
};

DecodedImage decodeImage(const EncodedImage &encoded) {
    DecodedImage decoded;
    int source_components = 0;
    const auto *data = encoded.bytes.data();
    const auto size = static_cast<int>(encoded.bytes.size());
    if (stbi_is_16_bit_from_memory(data, size)) {
        auto *pixels = stbi_load_16_from_memory(
            data, size, &decoded.width, &decoded.height, &source_components, 4);
        if (pixels == nullptr) {
            throw std::runtime_error(
                std::string{"failed to decode 16-bit glTF image: "} +
                stbi_failure_reason());
        }
        const auto bytes = static_cast<std::size_t>(decoded.width) *
                           decoded.height * 4 * sizeof(stbi_us);
        const auto *begin = reinterpret_cast<const unsigned char *>(pixels);
        decoded.pixels.assign(begin, begin + bytes);
        stbi_image_free(pixels);
        decoded.bits = 16;
        decoded.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    } else {
        auto *pixels = stbi_load_from_memory(data, size, &decoded.width,
                                             &decoded.height,
                                             &source_components, 4);
        if (pixels == nullptr) {
            throw std::runtime_error(
                std::string{"failed to decode glTF image: "} +
                stbi_failure_reason());
        }
        const auto bytes = static_cast<std::size_t>(decoded.width) *
                           decoded.height * 4;
        decoded.pixels.assign(pixels, pixels + bytes);
        stbi_image_free(pixels);
    }
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
    const auto decoded = parallelPrepareOrdered<DecodedImage>(
        indices.size(),
        [&](std::size_t slot) { return decodeImage(*encoded[indices[slot]]); },
        4);
    for (std::size_t slot = 0; slot < indices.size(); ++slot) {
        auto &image = model.images.at(indices[slot]);
        image.image = decoded[slot].pixels;
        image.width = decoded[slot].width;
        image.height = decoded[slot].height;
        image.component = decoded[slot].component;
        image.bits = decoded[slot].bits;
        image.pixel_type = decoded[slot].pixel_type;
    }
}

} // namespace Pelican::GltfInternal
