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
    static std::unique_ptr<PreparedProceduralAssetPublication> prepare(
        ProjectBasicConfig &config, PathResolver &resolver,
        ProceduralGeometryContainer &procedural, PolygonInstanceContainer &renderer,
        VulkanManageCore *vkcore, std::string_view scene_id, std::string_view raw_assets);
    // source_commit performs the final fallible operation (including disk CAS).
    // Once it returns, publication and retirement are noexcept.
    void commit(const std::function<void()> &source_commit);
};
} // namespace Pelican
