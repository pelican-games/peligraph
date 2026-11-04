#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// VO-1a: the four 8-bit formats the raw file header can name. The values are
// the header's `format` field and never change.
// The header is `PVO1`, width, height and format, each a little-endian u32.
inline constexpr std::size_t output_file_header_bytes = 16;

enum class OutputFileFormat : std::uint32_t {
    rgba8_unorm = 0,
    rgba8_srgb = 1,
    bgra8_unorm = 2,
    bgra8_srgb = 3,
};

struct OutputFileImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    OutputFileFormat format = OutputFileFormat::rgba8_unorm;
    // Tightly packed rows, stride == width * 4, row 0 first, unconverted.
    std::vector<std::uint8_t> pixels;
};

enum class OutputFileWriteStatus {
    written,
    exists,
    failed,
};

struct OutputFileWriteResult {
    OutputFileWriteStatus status = OutputFileWriteStatus::failed;
    std::string message;

    bool operator==(const OutputFileWriteResult &) const = default;
};

// Writes `<dir>/<name>_<frame, six digits>.rgba`: the 16-byte `PVO1` header
// followed by the pixels. The final name is created only after its temporary
// `.partial` is complete; an existing final name is never overwritten.
OutputFileWriteResult writeOutputFrameFile(const std::filesystem::path &directory,
                                           std::string_view name,
                                           std::uint64_t frame,
                                           const OutputFileImage &image);

} // namespace Pelican
