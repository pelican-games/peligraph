// CV-1: holds a frame the player drew from a lowered canvas against the CPU
// reference of the authored document.
//
//   pelican_test_canvas_png_compare <document.json> <canvas.output id> <frame.png>
//       --tolerance <0..255> --min-fraction <0..1> [<node>.<port>=<value> ...]
//       [--image <key>=<file.png> ...] [--image-color <key>=<r>,<g>,<b>,<a> ...]
//       [--against <reference.png>]
//
// The frame is what the player writes: the premultiplied linear canvas,
// sRGB-encoded, alpha kept. The reference is encoded the same way and every
// channel of every pixel compared; the run passes when at least
// `min-fraction` of the pixels are within `tolerance` in all four channels.
// Prints {pixels, within, fraction, max_difference, max_at: [x, y],
// mean_difference} as JSON.
//
// CV-2a: --image hands the reference an image the items sample (keyed as
// CanvasReferenceImages is: a file's asset, or <node>.<port>), decoded from a
// PNG; --image-color a 1 x 1 one. --against holds the frame against another
// PNG instead of the CPU reference (an opaque RGB file counts as alpha 255).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "canvas/canvaslowering.hpp"
#include "canvas/canvasreference.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int encode(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    const auto srgb = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(srgb * 255.0));
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "usage: pelican_test_canvas_png_compare <document.json> <output> <frame.png> "
                     "--tolerance N --min-fraction F [node.port=value ...]"
                  << std::endl;
        return 2;
    }
    try {
        int tolerance = -1;
        double min_fraction = -1.0;
        Pelican::CanvasValues values;
        Pelican::CanvasReferenceImages images;
        std::string against;
        const auto keyed = [](const std::string &argument) {
            const auto equals = argument.find('=');
            if (equals == std::string::npos) throw std::invalid_argument("expected key=value: " + argument);
            return std::pair{argument.substr(0, equals), argument.substr(equals + 1)};
        };
        for (int i = 4; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--image" && i + 1 < argc) {
                const auto [key, file] = keyed(argv[++i]);
                int width = 0, height = 0, channels = 0;
                auto *pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
                if (pixels == nullptr) throw std::runtime_error("cannot decode " + file);
                images[key] = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                               std::vector<std::uint8_t>(pixels, pixels + std::size_t(width) * height * 4)};
                stbi_image_free(pixels);
            } else if (argument == "--image-color" && i + 1 < argc) {
                const auto [key, rgba] = keyed(argv[++i]);
                Pelican::CanvasReferenceImage image{1, 1, {}};
                std::size_t at = 0;
                for (int channel = 0; channel < 4; ++channel) {
                    const auto comma = rgba.find(',', at);
                    image.rgba.push_back(static_cast<std::uint8_t>(std::stoi(rgba.substr(at, comma - at))));
                    at = comma == std::string::npos ? rgba.size() : comma + 1;
                }
                images[key] = std::move(image);
            } else if (argument == "--against" && i + 1 < argc) {
                against = argv[++i];
            } else if (argument == "--tolerance" && i + 1 < argc) {
                tolerance = std::stoi(argv[++i]);
            } else if (argument == "--min-fraction" && i + 1 < argc) {
                min_fraction = std::stod(argv[++i]);
            } else {
                const auto equals = argument.find('=');
                const auto dot = argument.rfind('.', equals);
                if (equals == std::string::npos || dot == std::string::npos)
                    throw std::invalid_argument("expected node.port=value: " + argument);
                values[{argument.substr(0, dot), argument.substr(dot + 1, equals - dot - 1)}] =
                    std::stod(argument.substr(equals + 1));
            }
        }
        if (tolerance < 0 || min_fraction < 0.0) throw std::invalid_argument("--tolerance and --min-fraction are required");
        int width = 0, height = 0, channels = 0;
        auto *pixels = stbi_load(argv[3], &width, &height, &channels, 4);
        if (pixels == nullptr) throw std::runtime_error(std::string{"cannot decode "} + argv[3]);
        // The expected bytes of every pixel, RGBA.
        std::vector<int> expected(static_cast<std::size_t>(width) * height * 4);
        if (!against.empty()) {
            int against_width = 0, against_height = 0, against_channels = 0;
            auto *reference = stbi_load(against.c_str(), &against_width, &against_height, &against_channels, 4);
            if (reference == nullptr) throw std::runtime_error("cannot decode " + against);
            if (against_width != width || against_height != height)
                throw std::runtime_error("the frame and " + against + " differ in size");
            std::copy(reference, reference + expected.size(), expected.begin());
            stbi_image_free(reference);
        } else {
            std::ifstream in(argv[1], std::ios::binary);
            if (!in) throw std::runtime_error(std::string{"cannot read "} + argv[1]);
            const auto document = nlohmann::json::parse(in);
            const auto ir = Pelican::buildCanvasIr(document);
            const auto output =
                std::find_if(ir.outputs.begin(), ir.outputs.end(),
                             [&](const Pelican::CanvasOutput &candidate) { return candidate.id == argv[2]; });
            if (output == ir.outputs.end()) throw std::runtime_error(std::string{"no canvas.output "} + argv[2]);
            const auto reference = Pelican::renderCanvasReference(*output, static_cast<std::uint32_t>(width),
                                                                  static_cast<std::uint32_t>(height), values, images);
            for (std::size_t at = 0; at < expected.size(); ++at) {
                const double value = reference.pixels[at];
                expected[at] = at % 4 == 3 ? static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0))
                                           : encode(value);
            }
        }
        std::size_t within = 0;
        int max_difference = 0;
        std::size_t worst_pixel = 0;
        double total_difference = 0.0;
        const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t pixel = 0; pixel < count; ++pixel) {
            int worst = 0;
            for (std::size_t channel = 0; channel < 4; ++channel)
                worst = std::max(worst, std::abs(expected[pixel * 4 + channel] -
                                                 static_cast<int>(pixels[pixel * 4 + channel])));
            if (worst <= tolerance) ++within;
            if (worst > max_difference) {
                max_difference = worst;
                worst_pixel = pixel;
            }
            total_difference += worst;
        }
        stbi_image_free(pixels);
        const auto fraction = static_cast<double>(within) / static_cast<double>(count);
        std::cout << nlohmann::json{{"pixels", count}, {"within", within}, {"fraction", fraction},
                                    {"max_difference", max_difference},
                                    {"max_at", {worst_pixel % static_cast<std::size_t>(width),
                                                worst_pixel / static_cast<std::size_t>(width)}},
                                    {"mean_difference", total_difference / static_cast<double>(count)}}
                         .dump()
                  << std::endl;
        return fraction >= min_fraction ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}
