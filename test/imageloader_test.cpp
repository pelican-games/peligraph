#include "../src/core/loader/imageloader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <tinyexr.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

namespace {

struct TempDir {
    std::filesystem::path root;

    TempDir() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("pelican_imageloader_test_" + std::to_string(suffix));
        std::filesystem::create_directories(root);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void writeBytes(const std::filesystem::path &path, const unsigned char *bytes, size_t size) {
    std::ofstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open test file: " + path.string());
    }
    file.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(size));
}

void throwTinyExrSaveError(const std::filesystem::path &path, const char *error) {
    std::string message = error ? error : "unknown error";
    if (error) {
        FreeEXRErrorMessage(error);
    }
    throw std::runtime_error("failed to write test EXR " + path.string() + ": " + message);
}

void saveFloatExr(const std::filesystem::path &path, int width, int height, const std::vector<float> &rgba,
                  bool fp16) {
    const auto path_string = path.string();
    const char *error = nullptr;
    const int ret = SaveEXR(rgba.data(), width, height, 4, fp16 ? 1 : 0, path_string.c_str(), &error);
    if (ret != TINYEXR_SUCCESS) {
        throwTinyExrSaveError(path, error);
    }
}

void setChannelName(EXRChannelInfo &channel, const char *name) {
    std::snprintf(channel.name, sizeof(channel.name), "%s", name);
    channel.x_sampling = 1;
    channel.y_sampling = 1;
    channel.p_linear = 0;
}

void saveUintExr(const std::filesystem::path &path) {
    EXRHeader header;
    InitEXRHeader(&header);
    header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
    header.num_channels = 4;
    header.channels = static_cast<EXRChannelInfo *>(std::malloc(sizeof(EXRChannelInfo) * 4));
    header.pixel_types = static_cast<int *>(std::malloc(sizeof(int) * 4));
    header.requested_pixel_types = static_cast<int *>(std::malloc(sizeof(int) * 4));
    std::memset(header.channels, 0, sizeof(EXRChannelInfo) * 4);

    setChannelName(header.channels[0], "A");
    setChannelName(header.channels[1], "B");
    setChannelName(header.channels[2], "G");
    setChannelName(header.channels[3], "R");
    for (int i = 0; i < 4; ++i) {
        header.pixel_types[i] = TINYEXR_PIXELTYPE_UINT;
        header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_UINT;
    }

    std::array<unsigned int, 1> a{255};
    std::array<unsigned int, 1> b{64};
    std::array<unsigned int, 1> g{128};
    std::array<unsigned int, 1> r{255};
    std::array<unsigned char *, 4> channels{
        reinterpret_cast<unsigned char *>(a.data()),
        reinterpret_cast<unsigned char *>(b.data()),
        reinterpret_cast<unsigned char *>(g.data()),
        reinterpret_cast<unsigned char *>(r.data()),
    };

    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = 4;
    image.width = 1;
    image.height = 1;
    image.images = channels.data();

    const auto path_string = path.string();
    const char *error = nullptr;
    const int ret = SaveEXRImageToFile(&image, &header, path_string.c_str(), &error);
    FreeEXRHeader(&header);
    if (ret != TINYEXR_SUCCESS) {
        throwTinyExrSaveError(path, error);
    }
}

template <class T>
T readComponent(const LoadedImage &image, size_t component_index) {
    T value{};
    std::memcpy(&value, image.pixels.data() + component_index * sizeof(T), sizeof(T));
    return value;
}

template <class T> void appendLe(std::vector<std::byte> &bytes, T value) {
    const auto old_size = bytes.size();
    bytes.resize(old_size + sizeof(T));
    std::memcpy(bytes.data() + old_size, &value, sizeof(T));
}

void writeLe32(std::vector<std::byte> &bytes, size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void writeLe64(std::vector<std::byte> &bytes, size_t offset, std::uint64_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::byte> makeRgba8Ktx2(std::uint32_t vk_format = 43, std::uint32_t level_count = 2,
                                     std::uint32_t supercompression = 0) {
    static constexpr std::array<std::uint8_t, 12> id{
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    std::vector<std::byte> bytes;
    for (auto value : id) bytes.push_back(static_cast<std::byte>(value));
    appendLe(bytes, vk_format);
    appendLe(bytes, std::uint32_t{1}); // typeSize
    appendLe(bytes, std::uint32_t{2});
    appendLe(bytes, std::uint32_t{2});
    appendLe(bytes, std::uint32_t{0}); // depth
    appendLe(bytes, std::uint32_t{0}); // layers
    appendLe(bytes, std::uint32_t{1}); // faces
    appendLe(bytes, level_count);
    appendLe(bytes, supercompression);
    const auto dfd_offset = std::uint32_t{80 + level_count * 24};
    appendLe(bytes, dfd_offset);
    appendLe(bytes, std::uint32_t{92});
    appendLe(bytes, std::uint32_t{0}); // KVD
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint64_t{0}); // SGD
    appendLe(bytes, std::uint64_t{0});
    bytes.resize(dfd_offset);
    const auto data_offset = std::uint64_t{dfd_offset + 92};
    if (level_count >= 1) {
        writeLe64(bytes, 80, data_offset + (level_count >= 2 ? 4 : 0));
        writeLe64(bytes, 88, 16);
        writeLe64(bytes, 96, 16);
    }
    if (level_count >= 2) {
        writeLe64(bytes, 104, data_offset);
        writeLe64(bytes, 112, 4);
        writeLe64(bytes, 120, 4);
    }
    appendLe(bytes, std::uint32_t{92}); // DFD totalSize
    appendLe(bytes, std::uint32_t{0});  // Khronos vendor/basic descriptor
    appendLe(bytes, std::uint16_t{2});  // descriptor version
    appendLe(bytes, std::uint16_t{88});
    bytes.push_back(std::byte{1}); // RGBSDA model
    bytes.push_back(std::byte{1}); // BT.709 primaries
    bytes.push_back(vk_format == 43 ? std::byte{2} : std::byte{1});
    bytes.push_back(std::byte{0});
    bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    appendLe(bytes, std::uint32_t{4}); // bytesPlane[0..3]
    appendLe(bytes, std::uint32_t{0}); // bytesPlane[4..7]
    for (std::uint16_t channel = 0; channel < 4; ++channel) {
        appendLe(bytes, static_cast<std::uint16_t>(channel * 8));
        bytes.push_back(std::byte{7});
        bytes.push_back(static_cast<std::byte>(channel == 3 ? 15 : channel));
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{255});
    }
    if (level_count >= 2) bytes.insert(bytes.end(), 4, std::byte{188});
    bytes.insert(bytes.end(), 16, std::byte{188});
    return bytes;
}

} // namespace

TEST_CASE("image loader keeps PNG path on RGBA8 stb decode", "[imageloader]") {
    static constexpr std::array<unsigned char, 68> png{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C, 0x02, 0x00, 0x00, 0x00,
        0x0B, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0xA7, 0xDB, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };

    TempDir temp;
    const auto path = temp.root / "pixel.png";
    writeBytes(path, png.data(), png.size());

    const auto image = loadImageFile(path);

    REQUIRE(image.width == 1);
    REQUIRE(image.height == 1);
    REQUIRE(image.format == ImagePixelFormat::Rgba8Unorm);
    REQUIRE(image.bytesPerPixel() == 4);
    REQUIRE(image.pixels.size() == 4);
}

TEST_CASE("image loader reads half EXR as RGBA16F", "[imageloader]") {
    TempDir temp;
    const auto path = temp.root / "half.exr";
    saveFloatExr(path, 2, 1, {
        0.25f, 0.5f, 0.75f, 1.0f,
        1.0f, 0.75f, 0.5f, 0.25f,
    }, true);

    const auto image = loadImageFile(path);

    REQUIRE(image.width == 2);
    REQUIRE(image.height == 1);
    REQUIRE(image.format == ImagePixelFormat::Rgba16Sfloat);
    REQUIRE(image.bytesPerPixel() == 8);
    REQUIRE(image.pixels.size() == 16);
    REQUIRE(readComponent<uint16_t>(image, 0) == 0x3400);
    REQUIRE(readComponent<uint16_t>(image, 1) == 0x3800);
    REQUIRE(readComponent<uint16_t>(image, 2) == 0x3A00);
    REQUIRE(readComponent<uint16_t>(image, 3) == 0x3C00);
}

TEST_CASE("image loader reads float EXR as RGBA32F", "[imageloader]") {
    TempDir temp;
    const auto path = temp.root / "float.exr";
    saveFloatExr(path, 1, 1, {1.25f, 2.5f, 3.75f, 0.5f}, false);

    const auto image = loadImageFile(path);

    REQUIRE(image.width == 1);
    REQUIRE(image.height == 1);
    REQUIRE(image.format == ImagePixelFormat::Rgba32Sfloat);
    REQUIRE(image.bytesPerPixel() == 16);
    REQUIRE(image.pixels.size() == 16);
    REQUIRE(readComponent<float>(image, 0) == Catch::Approx(1.25f));
    REQUIRE(readComponent<float>(image, 1) == Catch::Approx(2.5f));
    REQUIRE(readComponent<float>(image, 2) == Catch::Approx(3.75f));
    REQUIRE(readComponent<float>(image, 3) == Catch::Approx(0.5f));
}

TEST_CASE("image loader rejects unsupported EXR pixel type with useful error", "[imageloader]") {
    TempDir temp;
    const auto path = temp.root / "uint.exr";
    saveUintExr(path);

    try {
        (void)loadImageFile(path);
        FAIL("UINT EXR should be rejected");
    } catch (const std::runtime_error &error) {
        REQUIRE(contains(error.what(), "unsupported EXR pixel type"));
        REQUIRE(contains(error.what(), "half/float"));
    }
}

TEST_CASE("KTX2 test writer round-trips complete RGBA8 mip levels and sRGB known value", "[imageloader][ktx2]") {
    const auto bytes = makeRgba8Ktx2();
    const auto again = makeRgba8Ktx2();
    REQUIRE(std::equal(bytes.begin(), bytes.end(), again.begin(), again.end()));

    const auto image = loadImageMemory(bytes, "known-188.ktx2");
    REQUIRE(image.width == 2);
    REQUIRE(image.height == 2);
    REQUIRE(image.format == ImagePixelFormat::Rgba8Srgb);
    REQUIRE(image.mipLevels() == 2);
    REQUIRE(image.levels[0].size == 16);
    REQUIRE(image.levels[1].size == 4);
    REQUIRE(static_cast<unsigned>(image.pixels[0]) == 188);
    const auto encoded = 188.0 / 255.0;
    const auto linear = std::pow((encoded + 0.055) / 1.055, 2.4);
    REQUIRE(linear == Catch::Approx(0.502886).margin(0.00001));
}

TEST_CASE("KTX2 parser rejects subset violations with the texture name", "[imageloader][ktx2]") {
    const auto expect_error = [](const std::vector<std::byte> &bytes, std::string_view detail) {
        try {
            (void)loadImageMemory(bytes, "bad_named.ktx2");
            FAIL("invalid KTX2 should be rejected");
        } catch (const std::runtime_error &error) {
            REQUIRE(contains(error.what(), "bad_named.ktx2"));
            REQUIRE(contains(error.what(), detail));
        }
    };
    expect_error(makeRgba8Ktx2(43, 2, 2), "supercompression");
    expect_error(makeRgba8Ktx2(43, 1), "incomplete mip chain");
    expect_error(makeRgba8Ktx2(147, 2), "outside the WP92");
}

TEST_CASE("committed BC7 and BC5 KTX2 fixtures retain compressed blocks", "[imageloader][ktx2]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "ktx2";
    const auto bc7 = loadImageFile(root / "bc7_srgb_1x1.ktx2");
    REQUIRE(bc7.format == ImagePixelFormat::Bc7Srgb);
    REQUIRE(bc7.isBlockCompressed());
    REQUIRE(bc7.mipLevels() == 1);
    REQUIRE(bc7.pixels.size() == 16);

    const auto bc5 = loadImageFile(root / "bc5_unorm_1x1.ktx2");
    REQUIRE(bc5.format == ImagePixelFormat::Bc5Unorm);
    REQUIRE(bc5.isBlockCompressed());
    REQUIRE(bc5.mipLevels() == 1);
    REQUIRE(bc5.pixels.size() == 16);
}

} // namespace Pelican
