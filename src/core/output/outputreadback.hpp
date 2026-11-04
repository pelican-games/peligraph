#pragma once

#include "./outputfile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// VO-1a: the four formats PVO1 can name, and nothing else. Public because the
// readback pre-check and the file writer must agree on exactly one table.
inline std::optional<OutputFileFormat> outputFileFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return OutputFileFormat::rgba8_unorm;
    case vk::Format::eR8G8B8A8Srgb:
        return OutputFileFormat::rgba8_srgb;
    case vk::Format::eB8G8R8A8Unorm:
        return OutputFileFormat::bgra8_unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return OutputFileFormat::bgra8_srgb;
    default:
        return std::nullopt;
    }
}

enum class OutputReadStatus : std::uint8_t {
    ready,
    // The sink's graph is not the published generation, or its target is not
    // active there. No read is attempted.
    inactive,
    // The target is retained and active but has not been laid out by a frame
    // yet; the frame is skipped and no error is raised.
    no_contents,
    // The target's format or shape cannot be written as a PVO1 frame. The
    // sink refuses by the exact code in `error`; no pixels are read.
    refused,
};

struct OutputReadResult {
    OutputReadStatus status = OutputReadStatus::inactive;
    vk::Extent2D extent{};
    vk::Format format = vk::Format::eUndefined;
    // A leading output.*@1 code and detail when status is refused.
    std::string error;
    // Tightly packed rows, row 0 first, unconverted.
    std::vector<std::uint8_t> bytes;
};

} // namespace Pelican
