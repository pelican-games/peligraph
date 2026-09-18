#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "../renderer/modelinstance.hpp"

#include <optional>

namespace Pelican {
struct SceneLoadPhaseTrace;

DECLARE_MODULE(VatPlayer) {
    bool enabled = false;
    std::optional<ModelTemplate> vat_model;
    std::optional<ModelInstanceId> instance;

  public:
    struct PreparedSceneState {
        bool enabled = false;
        std::optional<ModelInstanceId> instance;
    };

    VatPlayer();

    void releaseInstanceForSceneLoad();
    PreparedSceneState snapshotPreparedScene() const noexcept;
    PreparedSceneState prepareSceneLoad() const noexcept;
    void publishPreparedScene(PreparedSceneState &&prepared) noexcept;
    void publishPreparedScene(PreparedSceneState &&prepared,
                              SceneLoadPhaseTrace &phase_trace) noexcept;
    bool isEnabled() const { return enabled; }
};

} // namespace Pelican
