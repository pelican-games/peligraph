#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace Pelican {
class ProjectBasicConfig;
class PathResolver;
class ProceduralGeometryContainer;
class PolygonInstanceContainer;
class VulkanManageCore;

// Prepared on the editor thread at the publication boundary. Keep scene/runtime
// authority unchanged between prepare and commit; discarding this object aborts.
class PreparedProceduralAssetPublication {
    struct State;
    std::unique_ptr<State> state_;
    explicit PreparedProceduralAssetPublication(std::unique_ptr<State> state);
public:
    ~PreparedProceduralAssetPublication();
    // `resolve_ray_rows` false (RTL-1a: edit_ray_layout's procedural records,
    // published just before the layout whose records they are) leaves the
    // scene's ray rows to be resolved by the next frame, with the layout that
    // frame's generation carries, instead of with the live one now.
    static std::unique_ptr<PreparedProceduralAssetPublication> prepare(
        ProjectBasicConfig &config, PathResolver &resolver,
        ProceduralGeometryContainer &procedural, PolygonInstanceContainer &renderer,
        VulkanManageCore *vkcore, std::string_view scene_id, std::string_view raw_assets,
        bool resolve_ray_rows = true);
    // source_commit performs the final fallible operation (including disk CAS).
    // Once it returns, publication and retirement are noexcept.
    void commit(const std::function<void()> &source_commit);
};
} // namespace Pelican
