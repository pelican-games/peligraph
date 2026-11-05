#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

// VO-1b: the bytes a file holds. `raw` is the VO-1a PVO1 container; `png`
// is a standard PNG with four channels copied from the input. Neither applies
// a colour transform: an sRGB format stores its encoded bytes, a UNORM format stores
// its linear bytes.
enum class OutputFileEncoding {
    raw,
    png,
};

enum class OutputFileWriteStatus {
    written,
    exists,
    failed,
    // The encoded file did not fit the caller's remaining byte budget. The
    // partial file was removed and no final file exists.
    limit_reached,
};

struct OutputFileWriteResult {
    OutputFileWriteStatus status = OutputFileWriteStatus::failed;
    std::string message;
    // The published file's real size. Zero when nothing was published.
    std::uint64_t bytes_written = 0;

    bool operator==(const OutputFileWriteResult &) const = default;
};

// Writes `<dir>/<name>_<frame, six digits>.<ext>`: `.rgba` for raw and
// `.png` for PNG. The final name is created only after its temporary
// `.partial` is complete; an existing final name is never overwritten.
// With `byte_budget`, the encoded partial is measured first; a file larger
// than the budget is deleted and reported as `limit_reached` without making
// the final name.
OutputFileWriteResult writeOutputFrameFile(
    const std::filesystem::path &directory, std::string_view name,
    std::uint64_t frame, const OutputFileImage &image,
    OutputFileEncoding encoding = OutputFileEncoding::raw,
    std::optional<std::uint64_t> byte_budget = std::nullopt);

} // namespace Pelican
