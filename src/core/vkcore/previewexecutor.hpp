#pragma once

#include "../loader/editorpreviewprojection.hpp"
#include "../renderingpass/previewgraph.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

enum class PreviewPixelEncoding : std::uint8_t {
    rgba8_srgb,
    png,
};

struct PreviewEngineTimeSnapshot {
    double time = 0.0;
    double delta = 0.0;
    std::uint64_t frame_index = 0;
};

struct PreviewCaptureRequest {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PreviewPixelEncoding pixel_encoding = PreviewPixelEncoding::png;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    std::uint64_t graph_generation = 0;
    std::size_t max_bytes = 0;
    std::string preview_request_id;
};

struct PreviewCaptureResult {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string pixel_encoding;
    std::vector<std::uint8_t> bytes;
    nlohmann::ordered_json timing;
};

class PreviewCaptureTooLarge : public std::runtime_error {
    std::size_t actual_;
    std::size_t limit_;

  public:
    PreviewCaptureTooLarge(std::size_t actual, std::size_t limit);
    std::size_t actual() const noexcept { return actual_; }
    std::size_t limit() const noexcept { return limit_; }
};

constexpr std::uint32_t preview_capture_max_dimension = 2048;
constexpr std::size_t preview_capture_hard_max_bytes = 16 * 1024 * 1024;

// Literal WP172 ownership inventory.  The order is part of the diagnostic
// contract and is deliberately shared by RPC, tests, and the design report.
nlohmann::ordered_json previewStateInventory();

class PreviewExecutor {
  public:
    PreviewCaptureResult execute(const PreviewGraphProgram &program,
                                 const PreparedProjection &projection,
                                 const PreviewCaptureRequest &request,
                                 const PreviewEngineTimeSnapshot &engine_time) const;
};

} // namespace Pelican
