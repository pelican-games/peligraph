#include "sceneauthoringadapter.hpp"

#include "authoringsceneauthority.hpp"

#include <algorithm>

namespace Pelican::internal {

std::string encodeAuthoringSceneSemantic(
    const AuthoringSceneDocument &document) {
    return AuthoringSceneAuthority::encodeSemantic(document);
}

bool authoringSceneContains(
    const AuthoringSceneDocument &document, std::string_view scene_id) {
    const auto scenes = AuthoringSceneAuthority::query(document);
    return std::any_of(scenes.begin(), scenes.end(),
                       [scene_id](const auto &scene) {
                           return scene.scene_id == scene_id;
                       });
}

} // namespace Pelican::internal
