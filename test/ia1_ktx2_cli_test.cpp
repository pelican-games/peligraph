// IA-1: the KTX2 writer and the float formats the loader reads, and
// `pelican_cli image ktx2` (--raw / --hdr) writing files the loader reads.
#include "../src/core/loader/imageloader.hpp"
#include "../src/core/loader/ktx2.hpp"
#include "../src/devcli/imagecommand.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Pelican {
namespace {

std::vector<std::byte> patternTexels(std::size_t count) {
    std::vector<std::byte> texels(count);
    for (std::size_t index = 0; index < count; ++index)
        texels[index] = static_cast<std::byte>((index * 37 + 11) & 0xFF);
    return texels;
}

std::filesystem::path freshDirectory(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("pelican_" + std::string{name} + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeBytes(const std::filesystem::path &path, const void *data, std::size_t size) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    REQUIRE(output.good());
}

int runCli(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "image");
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return DevCli::runImageCommand(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("IA-1 the KTX2 writer's single-level files load with their format, extent and texels",
          "[imageloader][ktx2][ia1]") {
    struct Expected {
        Ktx2Format format;
        ImagePixelFormat loaded;
        const char *name;
    };
    const std::array<Expected, 8> formats{{
        {Ktx2Format::Rgba8Unorm, ImagePixelFormat::Rgba8Unorm, "R8G8B8A8_UNORM"},
        {Ktx2Format::Rgba8Srgb, ImagePixelFormat::Rgba8Srgb, "R8G8B8A8_SRGB"},
        {Ktx2Format::R16Sfloat, ImagePixelFormat::R16Sfloat, "R16_SFLOAT"},
        {Ktx2Format::R16G16Sfloat, ImagePixelFormat::R16G16Sfloat, "R16G16_SFLOAT"},
        {Ktx2Format::R16G16B16A16Sfloat, ImagePixelFormat::Rgba16Sfloat, "R16G16B16A16_SFLOAT"},
        {Ktx2Format::R32Sfloat, ImagePixelFormat::R32Sfloat, "R32_SFLOAT"},
        {Ktx2Format::R32G32Sfloat, ImagePixelFormat::R32G32Sfloat, "R32G32_SFLOAT"},
        {Ktx2Format::R32G32B32A32Sfloat, ImagePixelFormat::Rgba32Sfloat, "R32G32B32A32_SFLOAT"},
    }};
    for (const auto &expected : formats) {
        INFO(expected.name);
        CHECK(std::string{ktx2FormatName(expected.format)} == expected.name);
        CHECK(ktx2FormatFromName(expected.name) == expected.format);
        // An odd extent: rows are packed, and the data offset keeps the
        // texel alignment whatever the DFD length.
        const auto texels = patternTexels(std::size_t{5} * 3 * ktx2TexelBytes(expected.format));
        const auto file = writeKtx2(expected.format, 5, 3, texels);
        const auto loaded = loadImageMemory(file, "written.ktx2");
        CHECK(loaded.format == expected.loaded);
        CHECK(std::string{imagePixelFormatName(loaded.format)} == expected.name);
        CHECK(loaded.color_space_declared);
        CHECK(loaded.width == 5);
        CHECK(loaded.height == 3);
        CHECK(loaded.mipLevels() == 1);
        CHECK(loaded.bytesPerPixel() == ktx2TexelBytes(expected.format));
        const bool same_texels = loaded.pixels == texels;
        CHECK(same_texels);
    }
    CHECK_THROWS_WITH(writeKtx2(Ktx2Format::Bc7Unorm, 4, 4, patternTexels(16)),
                      Catch::Matchers::ContainsSubstring("uncompressed formats only"));
    CHECK_THROWS_WITH(writeKtx2(Ktx2Format::R32Sfloat, 4, 4, patternTexels(15)),
                      Catch::Matchers::ContainsSubstring("expected 64 bytes"));
    CHECK_FALSE(ktx2FormatFromName("R8G8B8_UNORM").has_value());
}

TEST_CASE("IA-1 pelican_cli image ktx2 writes --raw and --hdr files the loader reads", "[devcli][ktx2][ia1]") {
    const auto dir = freshDirectory("ia1_cli");

    SECTION("--raw: little-endian texels, rows packed") {
        const std::array<float, 6> values{0.0f, 1.5f, -2.25f, 1.0e6f, 3.0e-5f, 42.0f};
        writeBytes(dir / "values.raw", values.data(), sizeof(values));
        REQUIRE(runCli({"ktx2", "--out", (dir / "values.ktx2").string(), "--format", "R32_SFLOAT", "--raw",
                        (dir / "values.raw").string(), "--width", "3", "--height", "2"}) == 0);
        const auto loaded = loadImageFile(dir / "values.ktx2");
        CHECK(loaded.format == ImagePixelFormat::R32Sfloat);
        CHECK(loaded.width == 3);
        CHECK(loaded.height == 2);
        REQUIRE(loaded.pixels.size() == sizeof(values));
        std::array<float, 6> read{};
        std::memcpy(read.data(), loaded.pixels.data(), sizeof(read));
        CHECK(read == values);

        // The byte count must match the extent and format; nothing is padded.
        CHECK(runCli({"ktx2", "--out", (dir / "bad.ktx2").string(), "--format", "R32G32_SFLOAT", "--raw",
                      (dir / "values.raw").string(), "--width", "3", "--height", "2"}) != 0);
        CHECK_FALSE(std::filesystem::exists(dir / "bad.ktx2"));
        // A block-compressed or unknown format is not written.
        CHECK(runCli({"ktx2", "--out", (dir / "bad.ktx2").string(), "--format", "BC7_UNORM_BLOCK", "--raw",
                      (dir / "values.raw").string(), "--width", "1", "--height", "1"}) != 0);
    }
    SECTION("--hdr: a Radiance file read as R32G32B32A32_SFLOAT") {
        // 3x2 flat (not run-length encoded) RGBE texels: (128, 64, 32, e=129)
        // is (1.0, 0.5, 0.25) and (64, 128, 0, e=130) is (1.0, 2.0, 0.0).
        std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 3\n";
        for (int texel = 0; texel < 6; ++texel) {
            const auto rgbe = texel % 2 == 0 ? std::array<unsigned char, 4>{128, 64, 32, 129}
                                             : std::array<unsigned char, 4>{64, 128, 0, 130};
            hdr.append(reinterpret_cast<const char *>(rgbe.data()), rgbe.size());
        }
        writeBytes(dir / "sky.hdr", hdr.data(), hdr.size());
        REQUIRE(runCli({"ktx2", "--out", (dir / "sky.ktx2").string(), "--hdr", (dir / "sky.hdr").string()}) == 0);
        const auto loaded = loadImageFile(dir / "sky.ktx2");
        CHECK(loaded.format == ImagePixelFormat::Rgba32Sfloat);
        CHECK(loaded.width == 3);
        CHECK(loaded.height == 2);
        REQUIRE(loaded.pixels.size() == std::size_t{3} * 2 * 16);
        std::vector<float> read(loaded.pixels.size() / sizeof(float));
        std::memcpy(read.data(), loaded.pixels.data(), loaded.pixels.size());
        for (std::size_t texel = 0; texel < 6; ++texel) {
            INFO(texel);
            const auto expected = texel % 2 == 0 ? std::array<float, 4>{1.0f, 0.5f, 0.25f, 1.0f}
                                                 : std::array<float, 4>{1.0f, 2.0f, 0.0f, 1.0f};
            CHECK(std::array<float, 4>{read[texel * 4], read[texel * 4 + 1], read[texel * 4 + 2],
                                       read[texel * 4 + 3]} == expected);
        }
        // --hdr writes RGBA32F only.
        CHECK(runCli({"ktx2", "--out", (dir / "bad.ktx2").string(), "--format", "R16G16B16A16_SFLOAT", "--hdr",
                      (dir / "sky.hdr").string()}) != 0);
    }
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

} // namespace Pelican
