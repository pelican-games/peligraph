#include "projectionjitter.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

float halton(std::uint32_t index, std::uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index != 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

std::uint32_t nextEpoch(std::uint32_t epoch) {
    if (epoch == std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("projection_jitter temporal reset epoch overflow");
    }
    return epoch + 1;
}

} // namespace

ProjectionJitterSample projectionJitterSample(const ProjectionJitterSettings &settings,
                                               std::uint64_t frame_index,
                                               std::uint32_t width,
                                               std::uint32_t height) {
    const auto provider = settings.provider.empty() ? std::string{"<unnamed>"} : settings.provider;
    if (settings.pattern != "halton23" && settings.pattern != "table") {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' has unsupported pattern: " + settings.pattern);
    }
    if (settings.phases < 1 || settings.phases > 64) {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' phases must be in range 1..64");
    }
    if (settings.pattern == "table" && settings.offsets_px.size() != settings.phases) {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' table offsets_px length must match phases");
    }
    if (frame_index == 0) {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' cannot sample frame_index 0");
    }
    if (width == 0 || height == 0) {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' requires non-zero framebuffer width and height");
    }

    const auto sample_index =
        static_cast<std::uint32_t>(((frame_index - 1) % settings.phases) + 1);
    const glm::vec2 offset_px = settings.pattern == "table"
                                    ? settings.offsets_px.at(sample_index - 1)
                                    : glm::vec2{halton(sample_index, 2) - 0.5f,
                                                halton(sample_index, 3) - 0.5f};
    if (!std::isfinite(offset_px.x) || !std::isfinite(offset_px.y) || offset_px.x < -0.5f ||
        offset_px.x >= 0.5f || offset_px.y < -0.5f || offset_px.y >= 0.5f) {
        throw std::runtime_error("projection_jitter provider '" + provider +
                                 "' table offset must be finite and in range [-0.5, 0.5)");
    }
    // A positive NDC y maps towards increasing framebuffer y for Pelican's
    // positive-height Vulkan viewport, so CPU/shader/image signs stay equal.
    const glm::vec2 jitter_ndc{2.0f * offset_px.x / static_cast<float>(width),
                               2.0f * offset_px.y / static_cast<float>(height)};
    return ProjectionJitterSample{sample_index, offset_px, jitter_ndc};
}

glm::mat4 applyProjectionJitter(const glm::mat4 &projection, glm::vec2 jitter_ndc) {
    auto result = projection;
    for (glm::length_t column = 0; column < 4; ++column) {
        result[column][0] += jitter_ndc.x * projection[column][3];
        result[column][1] += jitter_ndc.y * projection[column][3];
    }
    return result;
}

RenderFrameSnapshot buildRenderFrameSnapshot(const TemporalFrameHistory &history,
                                             const glm::mat4 &projection_non_jittered,
                                             const glm::mat4 &view,
                                             glm::vec3 camera_position,
                                             glm::vec2 jitter_ndc,
                                             bool reset_requested) {
    RenderFrameSnapshot snapshot;
    snapshot.projection_non_jittered = projection_non_jittered;
    snapshot.view_projection_non_jittered = projection_non_jittered * view;
    snapshot.projection_jittered = applyProjectionJitter(projection_non_jittered, jitter_ndc);
    snapshot.view_projection_jittered = snapshot.projection_jittered * view;
    snapshot.view = view;
    snapshot.camera_position = camera_position;
    snapshot.jitter_ndc = jitter_ndc;

    const bool reset = reset_requested || !history.valid;
    if (reset) {
        snapshot.previous_projection_non_jittered = snapshot.projection_non_jittered;
        snapshot.previous_view_projection_non_jittered = snapshot.view_projection_non_jittered;
        snapshot.previous_projection_jittered = snapshot.projection_jittered;
        snapshot.previous_view_projection_jittered = snapshot.view_projection_jittered;
        snapshot.previous_view = snapshot.view;
        snapshot.previous_camera_position = snapshot.camera_position;
        snapshot.previous_jitter_ndc = snapshot.jitter_ndc;
        snapshot.previous_temporal_reset_epoch = history.temporal_reset_epoch;
        snapshot.temporal_reset_epoch = nextEpoch(history.temporal_reset_epoch);
    } else {
        snapshot.previous_projection_non_jittered = history.projection_non_jittered;
        snapshot.previous_view_projection_non_jittered = history.view_projection_non_jittered;
        snapshot.previous_projection_jittered = history.projection_jittered;
        snapshot.previous_view_projection_jittered = history.view_projection_jittered;
        snapshot.previous_view = history.view;
        snapshot.previous_camera_position = history.camera_position;
        snapshot.previous_jitter_ndc = history.jitter_ndc;
        snapshot.temporal_reset_epoch = history.temporal_reset_epoch;
        snapshot.previous_temporal_reset_epoch = history.temporal_reset_epoch;
    }
    return snapshot;
}

void commitRenderFrameSnapshot(TemporalFrameHistory &history,
                               const RenderFrameSnapshot &snapshot) {
    history.valid = true;
    history.projection_non_jittered = snapshot.projection_non_jittered;
    history.view_projection_non_jittered = snapshot.view_projection_non_jittered;
    history.projection_jittered = snapshot.projection_jittered;
    history.view_projection_jittered = snapshot.view_projection_jittered;
    history.view = snapshot.view;
    history.camera_position = snapshot.camera_position;
    history.jitter_ndc = snapshot.jitter_ndc;
    history.temporal_reset_epoch = snapshot.temporal_reset_epoch;
}

} // namespace Pelican
