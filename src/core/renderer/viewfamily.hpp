#pragma once

#include "projectionjitter.hpp"
#include "../../project/graphvariantpolicy.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace Pelican {

inline constexpr std::string_view mainRenderViewFamilyId = "$main";
inline constexpr std::string_view monoRenderViewId = "$mono";

// A provider-owned, non-jittered view. view_id is stable across frames and is
// the identity used to select temporal state; vector position is only the
// execution order for the current frame.
struct RenderViewParameters {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    // XR eye views set this true. Flat and mirror/observer views retain the
    // default so VRM head geometry remains visible.
    bool first_person_view = false;
    std::string view_id;
};

// The logical unit supplied to one graph execution. A render graph normally
// consumes $main; later passes may name shadow, reflection, or capture
// families without extending Camera or the Vulkan execution path.
struct RenderViewFamily {
    std::string family_id{mainRenderViewFamilyId};
    std::vector<RenderViewParameters> views;
};

RenderViewFamily makeMainRenderViewFamily(RenderViewParameters view);

void validateRenderViewFamily(
    const RenderViewFamily &family,
    const CompiledGraphVariantPolicy &policy);

// History is keyed by provider identity instead of current execution index.
// This keeps previous matrices attached to the same eye/cascade when a
// provider changes the order in which its views execute.
struct TemporalViewFamilyHistory {
    std::string family_id;
    std::map<std::string, TemporalFrameHistory, std::less<>> views;
    std::vector<std::string> execution_order;

    bool hasValidView() const noexcept;
};

enum class TemporalViewFamilyChange {
    none,
    execution_order,
    topology,
};

// A membership change rebuilds identity storage. An order-only change keeps
// the ID-keyed matrices but is still reported: current graph history images
// remain execution-indexed and the renderer must reset those resources.
TemporalViewFamilyChange synchronizeTemporalViewFamilyHistory(
    TemporalViewFamilyHistory &history,
    const RenderViewFamily &family);

struct RenderViewFamilyProjectionModifiers {
    std::optional<ProjectionJitterSettings> projection_jitter;
};

// Applies family-level projection modifiers and resolves previous state in the
// family's current execution order.
std::vector<RenderFrameSnapshot> buildRenderViewFamilySnapshots(
    const TemporalViewFamilyHistory &history,
    const RenderViewFamily &family,
    const RenderViewFamilyProjectionModifiers &modifiers,
    const CompiledGraphVariantPolicy &policy,
    std::uint64_t frame_index,
    std::uint32_t render_width,
    std::uint32_t render_height,
    bool reset_requested);

void commitRenderViewFamilySnapshots(
    TemporalViewFamilyHistory &history,
    const RenderViewFamily &family,
    const std::vector<RenderFrameSnapshot> &snapshots);

} // namespace Pelican
