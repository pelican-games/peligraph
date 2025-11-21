#pragma once

#include "../container.hpp"

namespace Pelican {

using SceneId = std::string;

DECLARE_MODULE(SceneLoader) {
  public:
    SceneLoader();
    ~SceneLoader();

    void load(SceneId scene_id);
};

} // namespace Pelican
