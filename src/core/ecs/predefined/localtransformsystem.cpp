#include "localtransformsystem.hpp"

#include "../../geomhelper/geomhelper.hpp"

namespace Pelican {

void LocalTransformSystem::process(Query chunks) {
    for (auto &chunk : chunks) {
        auto transforms = std::get<TransformComponent *>(chunk.components);
        auto localtransforms = std::get<LocalTransformComponent *>(chunk.components);

        for (int i = 0; i < chunk.count; i++) {
            transforms[i].pos = to_glm(localtransforms[i].pos);
            transforms[i].rotation = to_glm(localtransforms[i].rotation);
            transforms[i].scale = to_glm(localtransforms[i].scale);
        }
    }
}

} // namespace Pelican
