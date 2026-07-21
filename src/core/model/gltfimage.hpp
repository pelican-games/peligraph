#pragma once

#include <tiny_gltf.h>

#include <optional>
#include <string>
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

} // namespace Pelican::GltfInternal
