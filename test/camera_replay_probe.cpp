#include "../src/core/renderer/camera.hpp"

#include <gamesystem.hpp>

namespace {

class CameraReplayProbe {
    bool initialized = false;
    Pelican::vec3 position{};

  public:
    void update(Pelican::GameContext &context) {
        auto &camera = Pelican::FastModuleContainer::get<Pelican::Camera>();
        if (!initialized) {
            const auto current = camera.getPos();
            position = Pelican::vec3{current.x, current.y, current.z};
            initialized = true;
        }
        if (context.actionsConfigured()) {
            const auto move = context.actionAxis2("move");
            position.x += move.y * 3.0f * static_cast<float>(context.deltaTime());
        }
        camera.setPos({position.x, position.y, position.z});
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(CameraReplayProbe, 20000);
