#pragma once

#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/scene.hpp"

#include <utility>

namespace Pelican::test_support {

inline void loadScene(SceneLoader &loader, SceneId scene_id) {
    auto &config =
        FastModuleContainer::get<ProjectBasicConfig>();
    auto request = SceneLoadRequest::borrowed(
        std::move(scene_id), config.sceneProjectionState());
    (void)requireSceneLoadCommitted(
        loader.load(std::move(request)));
}

} // namespace Pelican::test_support
