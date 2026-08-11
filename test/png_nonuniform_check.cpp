#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

constexpr int rgbaChannels = 4;

bool samePixel(const stbi_uc *left, const stbi_uc *right) {
    for (int channel = 0; channel < rgbaChannels; ++channel) {
        if (left[channel] != right[channel]) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: png_nonuniform_check <image.png>\n";
        return 64;
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;
    stbi_uc *pixels =
        stbi_load(argv[1], &width, &height, &source_channels, rgbaChannels);
    if (pixels == nullptr) {
        const auto *reason = stbi_failure_reason();
        std::cerr << "failed to decode PNG '" << argv[1] << "': "
                  << (reason == nullptr ? "unknown stb_image error" : reason)
                  << '\n';
        return 65;
    }

    if (width <= 0 || height <= 0) {
        std::cerr << "decoded PNG has no pixels: " << argv[1] << '\n';
        stbi_image_free(pixels);
        return 65;
    }

    const auto pixel_count = static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(height);
    for (std::size_t index = 1; index < pixel_count; ++index) {
        if (!samePixel(pixels, pixels + index * rgbaChannels)) {
            std::cout << "non-uniform PNG: " << argv[1] << " (" << width
                      << 'x' << height << ")\n";
            stbi_image_free(pixels);
            return 0;
        }
    }

    std::cerr << "uniform PNG: " << argv[1] << " (" << width << 'x'
              << height << ", rgba=" << static_cast<unsigned>(pixels[0])
              << ',' << static_cast<unsigned>(pixels[1]) << ','
              << static_cast<unsigned>(pixels[2]) << ','
              << static_cast<unsigned>(pixels[3]) << ")\n";
    stbi_image_free(pixels);
    return 2;
}
