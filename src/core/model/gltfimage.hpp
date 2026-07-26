#pragma once

#include <tiny_gltf.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::GltfInternal {

struct EncodedImage {
    std::vector<unsigned char> bytes;
    int requested_width = 0;
    int requested_height = 0;
};

using EncodedImages = std::vector<std::optional<EncodedImage>>;

bool retainEncodedImage(tinygltf::Image *image, int image_index,
                        std::string *error, std::string *warning,
                        int requested_width, int requested_height,
                        const unsigned char *bytes, int byte_count,
                        void *user_data);

void decodeImagesInParallel(tinygltf::Model &model,
                            const EncodedImages &encoded);

// The material texture path uses RGBA8 images so it can expose linear and
// sRGB views of the same storage. This validates the decoder/upload boundary,
// including the exact byte count expected by the Vulkan format.
void validateRgba8Image(const tinygltf::Image &image,
                        std::string_view source_name);

} // namespace Pelican::GltfInternal
