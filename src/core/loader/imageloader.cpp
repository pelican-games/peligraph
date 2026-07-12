#include "imageloader.hpp"
#include "ktx2.hpp"

#include "../build_features.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#if PELICAN_WITH_EXR
    #define TINYEXR_USE_MINIZ 0
    #define TINYEXR_USE_STB_ZLIB 1
    #define TINYEXR_IMPLEMENTATION
    #include <tinyexr.h>
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Pelican {

namespace {

std::string lowerExtension(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

bool isExrPath(const std::filesystem::path &path) {
    return lowerExtension(path) == ".exr";
}

bool isKtx2Path(const std::filesystem::path &path) {
    return lowerExtension(path) == ".ktx2";
}

ImagePixelFormat imageFormat(Ktx2Format format) {
    switch (format) {
    case Ktx2Format::Rgba8Unorm: return ImagePixelFormat::Rgba8Unorm;
    case Ktx2Format::Rgba8Srgb: return ImagePixelFormat::Rgba8Srgb;
    case Ktx2Format::Bc5Unorm: return ImagePixelFormat::Bc5Unorm;
    case Ktx2Format::Bc7Unorm: return ImagePixelFormat::Bc7Unorm;
    case Ktx2Format::Bc7Srgb: return ImagePixelFormat::Bc7Srgb;
    }
    throw std::runtime_error("Unknown KTX2 pixel format");
}

LoadedImage loadedKtx2(Ktx2Image parsed) {
    LoadedImage loaded;
    loaded.width = parsed.width;
    loaded.height = parsed.height;
    loaded.format = imageFormat(parsed.format);
    loaded.pixels = std::move(parsed.payload);
    loaded.levels.reserve(parsed.levels.size());
    for (const auto &level : parsed.levels)
        loaded.levels.push_back({level.offset, level.size, level.width, level.height});
    return loaded;
}

LoadedImage loadKtx2File(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) throw std::runtime_error("Failed to open KTX2 texture '" + path.string() + "'");
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("Failed to size KTX2 texture '" + path.string() + "'");
    std::vector<std::byte> bytes(static_cast<size_t>(end));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Failed to read KTX2 texture '" + path.string() + "'");
    return loadedKtx2(parseKtx2(bytes, path.string()));
}

size_t checkedPixelCount(int width, int height, const std::filesystem::path &path) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Invalid image dimensions in " + path.string());
    }

    const auto w = static_cast<size_t>(width);
    const auto h = static_cast<size_t>(height);
    if (w > std::numeric_limits<size_t>::max() / h) {
        throw std::runtime_error("Image is too large: " + path.string());
    }
    return w * h;
}

void ensureByteSize(size_t pixel_count, size_t bytes_per_pixel, const std::filesystem::path &path) {
    if (pixel_count > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
        throw std::runtime_error("Image is too large: " + path.string());
    }
}

#if PELICAN_WITH_EXR

constexpr uint16_t halfOne = 0x3c00;

struct TinyExrError {
    const char *message = nullptr;

    ~TinyExrError() {
        if (message) {
            FreeEXRErrorMessage(message);
        }
    }

    const char **out() { return &message; }

    std::string stringOr(std::string_view fallback) const {
        return message ? std::string{message} : std::string{fallback};
    }
};

struct ExrHeaderHandle {
    EXRHeader header{};

    ExrHeaderHandle() { InitEXRHeader(&header); }
    ~ExrHeaderHandle() { FreeEXRHeader(&header); }

    ExrHeaderHandle(const ExrHeaderHandle &) = delete;
    ExrHeaderHandle &operator=(const ExrHeaderHandle &) = delete;
};

struct ExrImageHandle {
    EXRImage image{};
    bool loaded = false;

    ExrImageHandle() { InitEXRImage(&image); }
    ~ExrImageHandle() {
        if (loaded) {
            FreeEXRImage(&image);
        }
    }

    ExrImageHandle(const ExrImageHandle &) = delete;
    ExrImageHandle &operator=(const ExrImageHandle &) = delete;
};

std::runtime_error exrError(const std::filesystem::path &path, std::string_view message) {
    return std::runtime_error("Unsupported EXR texture " + path.string() + ": " + std::string{message});
}

int findChannel(const EXRHeader &header, std::string_view name) {
    for (int i = 0; i < header.num_channels; ++i) {
        if (name == header.channels[i].name) {
            return i;
        }
    }
    return -1;
}

bool allChannelsAreHalf(const EXRHeader &header) {
    for (int i = 0; i < header.num_channels; ++i) {
        if (header.pixel_types[i] != TINYEXR_PIXELTYPE_HALF) {
            return false;
        }
    }
    return true;
}

void validateExrVersion(const EXRVersion &version, const std::filesystem::path &path) {
    if (version.multipart) {
        throw exrError(path, "multi-part EXR is not supported");
    }
    if (version.non_image) {
        throw exrError(path, "deep/non-image EXR is not supported");
    }
    if (version.tiled) {
        throw exrError(path, "tiled EXR is not supported");
    }
}

void validateExrHeader(const EXRHeader &header, const std::filesystem::path &path) {
    if (header.multipart) {
        throw exrError(path, "multi-part EXR is not supported");
    }
    if (header.non_image) {
        throw exrError(path, "deep/non-image EXR is not supported");
    }
    if (header.tiled) {
        throw exrError(path, "tiled EXR is not supported");
    }
    if (header.num_channels <= 0) {
        throw exrError(path, "no image channels were found");
    }

    for (int i = 0; i < header.num_channels; ++i) {
        const int pixel_type = header.pixel_types[i];
        if (pixel_type != TINYEXR_PIXELTYPE_HALF && pixel_type != TINYEXR_PIXELTYPE_FLOAT) {
            throw exrError(path, "unsupported EXR pixel type; only half/float channels are supported");
        }
    }

    if (header.num_channels > 1 &&
        (findChannel(header, "R") < 0 || findChannel(header, "G") < 0 || findChannel(header, "B") < 0)) {
        throw exrError(path, "RGB channels are required for multi-channel EXR textures");
    }
}

template <class T>
void writeComponent(std::vector<std::byte> &pixels, size_t pixel_index, size_t component_index, T value) {
    const auto offset = (pixel_index * 4 + component_index) * sizeof(T);
    std::memcpy(pixels.data() + offset, &value, sizeof(T));
}

template <class T>
const T *channelData(const EXRImage &image, int channel_index) {
    return reinterpret_cast<const T *>(image.images[channel_index]);
}

LoadedImage packHalfExr(const EXRHeader &header, const EXRImage &image, const std::filesystem::path &path) {
    const auto pixel_count = checkedPixelCount(image.width, image.height, path);
    constexpr size_t bytes_per_pixel = sizeof(uint16_t) * 4;
    ensureByteSize(pixel_count, bytes_per_pixel, path);

    LoadedImage loaded;
    loaded.width = static_cast<uint32_t>(image.width);
    loaded.height = static_cast<uint32_t>(image.height);
    loaded.format = ImagePixelFormat::Rgba16Sfloat;
    loaded.pixels.resize(pixel_count * bytes_per_pixel);

    const int r = findChannel(header, "R");
    const int g = findChannel(header, "G");
    const int b = findChannel(header, "B");
    const int a = findChannel(header, "A");
    const int single = header.num_channels == 1 ? 0 : -1;

    for (size_t i = 0; i < pixel_count; ++i) {
        if (single >= 0) {
            const auto value = channelData<uint16_t>(image, single)[i];
            writeComponent(loaded.pixels, i, 0, value);
            writeComponent(loaded.pixels, i, 1, value);
            writeComponent(loaded.pixels, i, 2, value);
            writeComponent(loaded.pixels, i, 3, halfOne);
        } else {
            writeComponent(loaded.pixels, i, 0, channelData<uint16_t>(image, r)[i]);
            writeComponent(loaded.pixels, i, 1, channelData<uint16_t>(image, g)[i]);
            writeComponent(loaded.pixels, i, 2, channelData<uint16_t>(image, b)[i]);
            writeComponent(loaded.pixels, i, 3, a >= 0 ? channelData<uint16_t>(image, a)[i] : halfOne);
        }
    }

    loaded.levels.push_back({0, loaded.pixels.size(), loaded.width, loaded.height});
    return loaded;
}

LoadedImage packFloatExr(const EXRHeader &header, const EXRImage &image, const std::filesystem::path &path) {
    const auto pixel_count = checkedPixelCount(image.width, image.height, path);
    constexpr size_t bytes_per_pixel = sizeof(float) * 4;
    ensureByteSize(pixel_count, bytes_per_pixel, path);

    LoadedImage loaded;
    loaded.width = static_cast<uint32_t>(image.width);
    loaded.height = static_cast<uint32_t>(image.height);
    loaded.format = ImagePixelFormat::Rgba32Sfloat;
    loaded.pixels.resize(pixel_count * bytes_per_pixel);

    const int r = findChannel(header, "R");
    const int g = findChannel(header, "G");
    const int b = findChannel(header, "B");
    const int a = findChannel(header, "A");
    const int single = header.num_channels == 1 ? 0 : -1;

    for (size_t i = 0; i < pixel_count; ++i) {
        if (single >= 0) {
            const auto value = channelData<float>(image, single)[i];
            writeComponent(loaded.pixels, i, 0, value);
            writeComponent(loaded.pixels, i, 1, value);
            writeComponent(loaded.pixels, i, 2, value);
            writeComponent(loaded.pixels, i, 3, 1.0f);
        } else {
            writeComponent(loaded.pixels, i, 0, channelData<float>(image, r)[i]);
            writeComponent(loaded.pixels, i, 1, channelData<float>(image, g)[i]);
            writeComponent(loaded.pixels, i, 2, channelData<float>(image, b)[i]);
            writeComponent(loaded.pixels, i, 3, a >= 0 ? channelData<float>(image, a)[i] : 1.0f);
        }
    }

    loaded.levels.push_back({0, loaded.pixels.size(), loaded.width, loaded.height});
    return loaded;
}

LoadedImage loadExrFile(const std::filesystem::path &path) {
    const auto path_string = path.string();

    EXRVersion version{};
    if (const int ret = ParseEXRVersionFromFile(&version, path_string.c_str()); ret != TINYEXR_SUCCESS) {
        throw std::runtime_error("Failed to read EXR texture " + path_string + ": invalid EXR version (" +
                                 std::to_string(ret) + ")");
    }
    validateExrVersion(version, path);

    TinyExrError error;
    ExrHeaderHandle header;
    if (const int ret = ParseEXRHeaderFromFile(&header.header, &version, path_string.c_str(), error.out());
        ret != TINYEXR_SUCCESS) {
        throw std::runtime_error("Failed to read EXR texture " + path_string + ": " +
                                 error.stringOr("invalid EXR header"));
    }
    validateExrHeader(header.header, path);

    const bool use_half_storage = allChannelsAreHalf(header.header);
    for (int i = 0; i < header.header.num_channels; ++i) {
        header.header.requested_pixel_types[i] =
            use_half_storage ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
    }

    ExrImageHandle image;
    if (const int ret = LoadEXRImageFromFile(&image.image, &header.header, path_string.c_str(), error.out());
        ret != TINYEXR_SUCCESS) {
        throw std::runtime_error("Failed to load EXR texture " + path_string + ": " +
                                 error.stringOr("image decode failed"));
    }
    image.loaded = true;

    return use_half_storage ? packHalfExr(header.header, image.image, path)
                            : packFloatExr(header.header, image.image, path);
}

#endif

LoadedImage packStbPixels(std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels,
                          int width, int height, std::string_view name) {
    if (!pixels) {
        const char *reason = stbi_failure_reason();
        throw std::runtime_error("Failed to load image " + std::string{name} + ": " +
                                 (reason ? reason : "unknown error"));
    }

    const auto pixel_count = checkedPixelCount(width, height, std::filesystem::path{std::string{name}});
    constexpr size_t bytes_per_pixel = 4;
    ensureByteSize(pixel_count, bytes_per_pixel, std::filesystem::path{std::string{name}});

    LoadedImage loaded;
    loaded.width = static_cast<uint32_t>(width);
    loaded.height = static_cast<uint32_t>(height);
    loaded.format = ImagePixelFormat::Rgba8Unorm;
    loaded.pixels.resize(pixel_count * bytes_per_pixel);
    std::memcpy(loaded.pixels.data(), pixels.get(), loaded.pixels.size());
    loaded.levels.push_back({0, loaded.pixels.size(), loaded.width, loaded.height});
    return loaded;
}

LoadedImage loadStbImageFile(const std::filesystem::path &path) {
    const auto path_string = path.string();
    int width = 0;
    int height = 0;
    int components = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load(path_string.c_str(), &width, &height, &components, STBI_rgb_alpha),
        stbi_image_free,
    };
    return packStbPixels(std::move(pixels), width, height, path_string);
}

} // namespace

size_t LoadedImage::bytesPerPixel() const {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm:
    case ImagePixelFormat::Rgba8Srgb:
        return 4;
    case ImagePixelFormat::Rgba16Sfloat:
        return 8;
    case ImagePixelFormat::Rgba32Sfloat:
        return 16;
    case ImagePixelFormat::Bc5Unorm:
    case ImagePixelFormat::Bc7Unorm:
    case ImagePixelFormat::Bc7Srgb:
        throw std::runtime_error("Block-compressed images do not have a fixed bytes-per-pixel value");
    }
    throw std::runtime_error("Unknown image pixel format");
}

bool LoadedImage::isBlockCompressed() const noexcept {
    return format == ImagePixelFormat::Bc5Unorm || format == ImagePixelFormat::Bc7Unorm ||
           format == ImagePixelFormat::Bc7Srgb;
}

LoadedImage loadImageFile(const std::filesystem::path &path) {
    if (isKtx2Path(path)) return loadKtx2File(path);
    if (isExrPath(path)) {
#if PELICAN_WITH_EXR
        return loadExrFile(path);
#else
        throwBuildFeatureDisabled("PELICAN_WITH_EXR", ".exr texture loading is unavailable: " + path.string());
#endif
    }
    return loadStbImageFile(path);
}

LoadedImage loadImageMemory(std::span<const std::byte> data, std::string_view name) {
    if (data.empty()) {
        throw std::runtime_error("Image memory is empty: " + std::string{name});
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Image memory is too large: " + std::string{name});
    }

    const auto name_path = std::filesystem::path{std::string{name}};
    if (isKtx2Path(name_path)) return loadedKtx2(parseKtx2(data, name));

    int width = 0;
    int height = 0;
    int components = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(data.data()),
                              static_cast<int>(data.size()), &width, &height,
                              &components, STBI_rgb_alpha),
        stbi_image_free,
    };
    return packStbPixels(std::move(pixels), width, height, name);
}

} // namespace Pelican
