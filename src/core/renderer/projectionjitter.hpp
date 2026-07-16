#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Pelican {

struct ProjectionJitterSettings {
    std::string provider;
    std::string pattern = "halton23";
    std::uint32_t phases = 8;
    std::vector<glm::vec2> offsets_px;
};

struct ProjectionJitterSample {
    std::uint32_t sample_index = 0;
    glm::vec2 offset_px{0.0f};
    glm::vec2 jitter_ndc{0.0f};
};

ProjectionJitterSample projectionJitterSample(const ProjectionJitterSettings &settings,
                                               std::uint64_t frame_index,
                                               std::uint32_t width,
                                               std::uint32_t height);
glm::mat4 applyProjectionJitter(const glm::mat4 &projection, glm::vec2 jitter_ndc);

struct RenderFrameSnapshot {
    glm::mat4 projection_non_jittered{1.0f};
    glm::mat4 view_projection_non_jittered{1.0f};
    glm::mat4 previous_projection_non_jittered{1.0f};
    glm::mat4 previous_view_projection_non_jittered{1.0f};
    glm::mat4 projection_jittered{1.0f};
    glm::mat4 view_projection_jittered{1.0f};
    glm::mat4 previous_projection_jittered{1.0f};
    glm::mat4 previous_view_projection_jittered{1.0f};
    glm::mat4 view{1.0f};
    glm::mat4 previous_view{1.0f};
    glm::vec2 jitter_ndc{0.0f};
    glm::vec2 previous_jitter_ndc{0.0f};
    std::uint32_t temporal_reset_epoch = 0;
    std::uint32_t previous_temporal_reset_epoch = 0;

    bool historyValid() const {
        return temporal_reset_epoch == previous_temporal_reset_epoch;
    }
};

struct TemporalFrameHistory {
    bool valid = false;
    glm::mat4 projection_non_jittered{1.0f};
    glm::mat4 view_projection_non_jittered{1.0f};
    glm::mat4 projection_jittered{1.0f};
    glm::mat4 view_projection_jittered{1.0f};
    glm::mat4 view{1.0f};
    glm::vec2 jitter_ndc{0.0f};
    std::uint32_t temporal_reset_epoch = 0;
};

RenderFrameSnapshot buildRenderFrameSnapshot(const TemporalFrameHistory &history,
                                             const glm::mat4 &projection_non_jittered,
                                             const glm::mat4 &view,
                                             glm::vec2 jitter_ndc,
                                             bool reset_requested);
void commitRenderFrameSnapshot(TemporalFrameHistory &history,
                               const RenderFrameSnapshot &snapshot);

} // namespace Pelican
