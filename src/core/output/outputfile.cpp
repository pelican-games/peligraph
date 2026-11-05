#include "outputfile.hpp"

// VO-1b: keep a private, single-TU stb PNG encoder. The shared implementation
// carries stb's process-wide flip switch; a private copy means an unrelated
// caller of stbi_flip_vertically_on_write() cannot reorder this writer's rows.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Pelican {
namespace {

std::string frameFileName(std::string_view name, std::uint64_t frame,
                          OutputFileEncoding encoding) {
    std::ostringstream out;
    out << name << '_' << std::setw(6) << std::setfill('0') << frame
        << (encoding == OutputFileEncoding::png ? ".png" : ".rgba");
    return out.str();
}

bool imageIsValid(const OutputFileImage &image) {
    if (image.width == 0 || image.height == 0) return false;
    const auto width = static_cast<std::uint64_t>(image.width);
    const auto height = static_cast<std::uint64_t>(image.height);
    if (height > (std::numeric_limits<std::uint64_t>::max)() / width / 4) {
        return false;
    }
    return image.pixels.size() == width * height * 4;
}

std::vector<std::uint8_t> encodeRaw(const OutputFileImage &image) {
    std::vector<std::uint8_t> bytes(output_file_header_bytes +
                                    image.pixels.size());
    std::size_t at = 0;
    for (const char magic : {'P', 'V', 'O', '1'})
        bytes[at++] = static_cast<std::uint8_t>(magic);
    const auto put = [&](std::uint32_t value) {
        bytes[at++] = static_cast<std::uint8_t>(value & 0xffu);
        bytes[at++] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
        bytes[at++] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
        bytes[at++] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    };
    put(image.width);
    put(image.height);
    put(static_cast<std::uint32_t>(image.format));
    std::copy(image.pixels.begin(), image.pixels.end(), bytes.begin() + at);
    return bytes;
}

void appendPngBytes(void *context, void *data, int size) {
    if (size <= 0) return;
    auto &out = *static_cast<std::vector<std::uint8_t> *>(context);
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    out.insert(out.end(), bytes, bytes + size);
}

std::vector<std::uint8_t> encodePng(const OutputFileImage &image) {
    // stb stores four channels in PNG order R, G, B, A. A BGRA target is a
    // channel reorder only: sRGB encoded values stay sRGB and UNORM linear
    // values stay linear. Alpha is copied unchanged; whether it is straight is
    // the input's assumption. Colour conversion is VO-2 and later.
    const bool bgra = image.format == OutputFileFormat::bgra8_unorm ||
                      image.format == OutputFileFormat::bgra8_srgb;
    const auto *source = image.pixels.data();
    std::vector<std::uint8_t> reordered;
    if (bgra) {
        reordered.resize(image.pixels.size());
        for (std::size_t at = 0; at + 3 < image.pixels.size(); at += 4) {
            reordered[at + 0] = image.pixels[at + 2];
            reordered[at + 1] = image.pixels[at + 1];
            reordered[at + 2] = image.pixels[at + 0];
            reordered[at + 3] = image.pixels[at + 3];
        }
        source = reordered.data();
    }
    const auto stride = static_cast<std::uint64_t>(image.width) * 4;
    if (stride > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) ||
        image.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(image.pixels.size() / 2 + 256);
    // Row 0 is the first row, matching raw. The private stb instance has its
    // flip switch at zero and this writer never changes it. No sRGB/gAMA
    // chunk is written, so the file does not claim a colour space.
    if (stbi_write_png_to_func(appendPngBytes, &bytes,
                               static_cast<int>(image.width),
                               static_cast<int>(image.height), 4, source,
                               static_cast<int>(stride)) == 0) {
        return {};
    }
    return bytes;
}

std::string systemReason(int code) {
    return std::system_category().message(code);
}

#ifdef _WIN32

std::string windowsReason(DWORD code) {
    return std::system_category().message(static_cast<int>(code));
}

bool createAndWritePartial(const std::filesystem::path &path,
                           const std::vector<std::uint8_t> &bytes,
                           std::string &reason, bool &created) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reason = windowsReason(GetLastError());
        return false;
    }
    created = true;
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            reason = windowsReason(GetLastError());
            ok = false;
            break;
        }
        offset += written;
    }
    if (!CloseHandle(file) && ok) {
        reason = windowsReason(GetLastError());
        ok = false;
    }
    return ok;
}

OutputFileWriteResult publishPartial(const std::filesystem::path &partial,
                                     const std::filesystem::path &final,
                                     std::uint64_t bytes) {
    if (!MoveFileExW(partial.c_str(), final.c_str(), 0)) {
        const auto error = GetLastError();
        DeleteFileW(partial.c_str());
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            return {OutputFileWriteStatus::exists,
                    "output.exists@1: " + final.string(), 0};
        }
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot publish " + final.string() +
                    ": " + windowsReason(error),
                0};
    }
    return {OutputFileWriteStatus::written, {}, bytes};
}

void removePartial(const std::filesystem::path &path) {
    DeleteFileW(path.c_str());
}

#else

std::string posixReason(int code) {
    return std::generic_category().message(code);
}

bool createAndWritePartial(const std::filesystem::path &path,
                           const std::vector<std::uint8_t> &bytes,
                           std::string &reason, bool &created) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        reason = posixReason(errno);
        return false;
    }
    created = true;
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written =
            ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            reason = posixReason(errno);
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0 && ok) {
        reason = posixReason(errno);
        ok = false;
    }
    return ok;
}

OutputFileWriteResult publishPartial(const std::filesystem::path &partial,
                                     const std::filesystem::path &final,
                                     std::uint64_t bytes) {
    // link() publishes the complete inode without ever replacing an existing
    // final name. std::filesystem::rename is deliberately not used.
    if (::link(partial.c_str(), final.c_str()) != 0) {
        const auto error = errno;
        ::unlink(partial.c_str());
        if (error == EEXIST) {
            return {OutputFileWriteStatus::exists,
                    "output.exists@1: " + final.string(), 0};
        }
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot publish " + final.string() +
                    ": " + posixReason(error),
                0};
    }
    ::unlink(partial.c_str());
    return {OutputFileWriteStatus::written, {}, bytes};
}

void removePartial(const std::filesystem::path &path) {
    ::unlink(path.c_str());
}

#endif

OutputFileWriteResult writePrepared(
    const std::filesystem::path &directory, const std::string &final_name,
    const std::string &partial_name, const std::vector<std::uint8_t> &bytes,
    std::optional<std::uint64_t> byte_budget) {
    const auto final_path = directory / final_name;
    const auto partial_path = directory / partial_name;
    std::error_code ignored;
    if (std::filesystem::exists(final_path, ignored)) {
        return {OutputFileWriteStatus::exists,
                "output.exists@1: " + final_path.string(), 0};
    }
    std::string reason;
    bool created = false;
    if (!createAndWritePartial(partial_path, bytes, reason, created)) {
        // Only clean up a partial this call created. If CREATE_NEW failed
        // because another file was already there, that file is not ours.
        if (created) removePartial(partial_path);
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot write " + partial_path.string() +
                    ": " + reason,
                0};
    }
    std::error_code size_error;
    const auto encoded_size = std::filesystem::file_size(partial_path, size_error);
    if (size_error) {
        removePartial(partial_path);
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot measure " + partial_path.string() +
                    ": " + size_error.message(),
                0};
    }
    if (byte_budget && encoded_size > *byte_budget) {
        removePartial(partial_path);
        return {OutputFileWriteStatus::limit_reached,
                "output.limit_reached@1: " + final_path.string() + " needs " +
                    std::to_string(encoded_size) + " bytes but only " +
                    std::to_string(*byte_budget) + " remain",
                0};
    }
    return publishPartial(partial_path, final_path, encoded_size);
}

} // namespace

OutputFileWriteResult writeOutputFrameFile(
    const std::filesystem::path &directory, std::string_view name,
    std::uint64_t frame, const OutputFileImage &image,
    OutputFileEncoding encoding, std::optional<std::uint64_t> byte_budget) {
    if (!imageIsValid(image)) {
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: pixel buffer does not match the image",
                0};
    }
    const auto final_name = frameFileName(name, frame, encoding);
    const auto partial_name = final_name + ".partial";
    auto bytes = encoding == OutputFileEncoding::png ? encodePng(image)
                                                      : encodeRaw(image);
    if (bytes.empty()) {
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot encode the image", 0};
    }
    return writePrepared(directory, final_name, partial_name, bytes, byte_budget);
}

} // namespace Pelican
