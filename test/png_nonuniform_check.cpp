#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr int rgbaChannels = 4;

bool parseSize(std::string_view text, std::size_t &value) {
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size();
}

bool parseRatio(std::string_view text, double &value) {
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size() &&
           std::isfinite(value);
}

std::uint32_t packedRgba(const stbi_uc *pixel) {
    std::uint32_t packed = 0;
    for (int channel = 0; channel < rgbaChannels; ++channel) {
        packed |= static_cast<std::uint32_t>(pixel[channel])
                  << (channel * 8);
    }
    return packed;
}

// `--changed <first.png> <second.png> <minimum-changed-pixels>`: the two
// images have the same size and at least that many pixels differ. The catalog
// smoke uses it to see that a project which declares animation moved, rather
// than that a single edge row happened to cross a pixel centre.
int changedPixels(const char *first_path, const char *second_path,
                  const char *minimum_text) {
    std::size_t minimum_changed_pixels = 0;
    if (!parseSize(minimum_text, minimum_changed_pixels) ||
        minimum_changed_pixels == 0) {
        std::cerr << "minimum changed pixels must be a positive integer: "
                  << minimum_text << '\n';
        return 64;
    }
    int widths[2] = {0, 0};
    int heights[2] = {0, 0};
    stbi_uc *images[2] = {nullptr, nullptr};
    const char *paths[2] = {first_path, second_path};
    for (int index = 0; index < 2; ++index) {
        int source_channels = 0;
        images[index] = stbi_load(paths[index], &widths[index], &heights[index],
                                  &source_channels, rgbaChannels);
        if (images[index] == nullptr || widths[index] <= 0 ||
            heights[index] <= 0) {
            const auto *reason = stbi_failure_reason();
            std::cerr << "failed to decode PNG '" << paths[index] << "': "
                      << (reason == nullptr ? "no pixels" : reason) << '\n';
            for (auto *image : images) stbi_image_free(image);
            return 65;
        }
    }
    if (widths[0] != widths[1] || heights[0] != heights[1]) {
        std::cerr << "PNG sizes differ: " << first_path << " is " << widths[0]
                  << 'x' << heights[0] << ", " << second_path << " is "
                  << widths[1] << 'x' << heights[1] << '\n';
        for (auto *image : images) stbi_image_free(image);
        return 65;
    }
    const auto pixel_count = static_cast<std::size_t>(widths[0]) *
                             static_cast<std::size_t>(heights[0]);
    std::size_t changed_pixels = 0;
    for (std::size_t index = 0; index < pixel_count; ++index) {
        if (packedRgba(images[0] + index * rgbaChannels) !=
            packedRgba(images[1] + index * rgbaChannels)) {
            ++changed_pixels;
        }
    }
    for (auto *image : images) stbi_image_free(image);

    const bool moved = changed_pixels >= minimum_changed_pixels;
    auto &stream = moved ? std::cout : std::cerr;
    stream << (moved ? "moved" : "unmoved") << " PNG pair: " << first_path
           << " -> " << second_path << " (" << widths[0] << 'x' << heights[0]
           << ", changed_pixels=" << changed_pixels
           << ", minimum_changed_pixels=" << minimum_changed_pixels << ")\n";
    return moved ? 0 : 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::string_view{argv[1]} == "--changed") {
        return changedPixels(argv[2], argv[3], argv[4]);
    }
    if (argc != 4) {
        std::cerr << "usage: png_nonuniform_check <image.png> "
                     "<minimum-distinct-colors> <minimum-non-modal-ratio>\n"
                     "       png_nonuniform_check --changed <first.png> "
                     "<second.png> <minimum-changed-pixels>\n";
        return 64;
    }

    std::size_t minimum_distinct_colors = 0;
    double minimum_non_modal_ratio = 0.0;
    if (!parseSize(argv[2], minimum_distinct_colors) ||
        minimum_distinct_colors == 0) {
        std::cerr << "minimum distinct colors must be a positive integer: "
                  << argv[2] << '\n';
        return 64;
    }
    if (!parseRatio(argv[3], minimum_non_modal_ratio) ||
        minimum_non_modal_ratio < 0.0 || minimum_non_modal_ratio > 1.0) {
        std::cerr << "minimum non-modal ratio must be between 0 and 1: "
                  << argv[3] << '\n';
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
    std::unordered_map<std::uint32_t, std::size_t> color_counts;
    color_counts.reserve(pixel_count);
    std::size_t modal_pixel_count = 0;
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const auto count =
            ++color_counts[packedRgba(pixels + index * rgbaChannels)];
        modal_pixel_count = std::max(modal_pixel_count, count);
    }
    stbi_image_free(pixels);

    const auto distinct_colors = color_counts.size();
    const auto non_modal_pixels = pixel_count - modal_pixel_count;
    const auto non_modal_ratio =
        static_cast<double>(non_modal_pixels) /
        static_cast<double>(pixel_count);
    const bool enough_colors =
        distinct_colors >= minimum_distinct_colors;
    const bool enough_non_modal =
        non_modal_ratio >= minimum_non_modal_ratio;

    auto &stream = enough_colors && enough_non_modal ? std::cout : std::cerr;
    stream << (enough_colors && enough_non_modal ? "accepted" : "rejected")
           << " PNG: " << argv[1] << " (" << width << 'x' << height
           << ", distinct_colors=" << distinct_colors
           << ", minimum_distinct_colors=" << minimum_distinct_colors
           << ", non_modal_pixels=" << non_modal_pixels
           << ", non_modal_ratio=" << std::fixed << std::setprecision(6)
           << non_modal_ratio
           << ", minimum_non_modal_ratio=" << minimum_non_modal_ratio
           << ")\n";
    return enough_colors && enough_non_modal ? 0 : 2;
}
