#include "imagecommand.hpp"

#include "../core/loader/ktx2.hpp"

#include <argparse/argparse.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#include <stb_image.h>

namespace Pelican::DevCli {
namespace {

std::vector<std::byte> readFileBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.string());
    std::vector<char> chars{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(chars.size());
    if (!chars.empty()) std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

std::uint32_t positive(const std::string &text, const char *option) {
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &used);
    } catch (const std::exception &) {
        used = 0;
    }
    if (used != text.size() || value == 0 || value > 0xFFFFFFFFul)
        throw std::runtime_error(std::string{option} + " must be a positive integer: " + text);
    return static_cast<std::uint32_t>(value);
}

int runKtx2Command(int argc, char *argv[]) {
    argparse::ArgumentParser program("pelican_cli image ktx2");
    program.add_argument("--out").required().metavar("file.ktx2");
    program.add_argument("--format").default_value(std::string{}).metavar("VK name").help(
        "R8G8B8A8_UNORM, R8G8B8A8_SRGB, R16_SFLOAT, R16G16_SFLOAT, R16G16B16A16_SFLOAT, R32_SFLOAT, "
        "R32G32_SFLOAT or R32G32B32A32_SFLOAT (--hdr: R32G32B32A32_SFLOAT, the default)");
    program.add_argument("--raw").default_value(std::string{}).metavar("file").help(
        "texels, little endian, rows packed");
    program.add_argument("--width").default_value(std::string{}).metavar("W");
    program.add_argument("--height").default_value(std::string{}).metavar("H");
    program.add_argument("--hdr").default_value(std::string{}).metavar("file.hdr");
    program.parse_args(argc, argv);

    const auto raw = program.get<std::string>("--raw");
    const auto hdr = program.get<std::string>("--hdr");
    if (raw.empty() == hdr.empty()) throw std::runtime_error("give exactly one of --raw and --hdr");
    auto format_name = program.get<std::string>("--format");
    if (format_name.empty()) {
        if (!raw.empty()) throw std::runtime_error("--raw requires --format");
        format_name = "R32G32B32A32_SFLOAT";
    }
    const auto format = ktx2FormatFromName(format_name);
    if (!format || ktx2TexelBytes(*format) == 0)
        throw std::runtime_error("--format is not an uncompressed format the engine reads: " + format_name);

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> texels;
    if (!raw.empty()) {
        if (program.get<std::string>("--width").empty() || program.get<std::string>("--height").empty())
            throw std::runtime_error("--raw requires --width and --height");
        width = positive(program.get<std::string>("--width"), "--width");
        height = positive(program.get<std::string>("--height"), "--height");
        texels = readFileBytes(raw);
    } else {
        if (*format != Ktx2Format::R32G32B32A32Sfloat)
            throw std::runtime_error("--hdr writes R32G32B32A32_SFLOAT, not " + format_name);
        if (!program.get<std::string>("--width").empty() || !program.get<std::string>("--height").empty())
            throw std::runtime_error("--hdr takes its width and height from the file");
        const auto bytes = readFileBytes(hdr);
        int w = 0;
        int h = 0;
        int channels = 0;
        std::unique_ptr<float, decltype(&stbi_image_free)> pixels{
            stbi_loadf_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), static_cast<int>(bytes.size()),
                                   &w, &h, &channels, 4),
            stbi_image_free};
        if (!pixels || w <= 0 || h <= 0) {
            const char *reason = stbi_failure_reason();
            throw std::runtime_error("cannot read " + hdr + " as Radiance HDR: " + (reason ? reason : "unknown"));
        }
        width = static_cast<std::uint32_t>(w);
        height = static_cast<std::uint32_t>(h);
        texels.resize(std::size_t{width} * height * 4 * sizeof(float));
        std::memcpy(texels.data(), pixels.get(), texels.size());
    }
    const auto file = writeKtx2(*format, width, height, texels);
    const std::filesystem::path out{program.get<std::string>("--out")};
    std::ofstream output{out, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("cannot write " + out.string());
    output.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!output) throw std::runtime_error("cannot write " + out.string());
    std::cout << "ktx2 " << ktx2FormatName(*format) << " " << width << "x" << height << " -> " << out.string()
              << std::endl;
    return 0;
}

} // namespace

int runImageCommand(int argc, char *argv[]) {
    try {
        if (argc > 1 && std::string_view{argv[1]} == "ktx2") return runKtx2Command(argc - 1, argv + 1);
        std::cerr << "usage: pelican_cli image ktx2 --out <file.ktx2> --format <VK name> "
                     "(--raw <file> --width W --height H | --hdr <file.hdr>)"
                  << std::endl;
        return -1;
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        return -1;
    }
}

} // namespace Pelican::DevCli
