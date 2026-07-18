#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include <details/ecs/entity.hpp>

#include <filesystem>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

using SceneId = std::string;

struct SceneObjectTransform {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;
};

DECLARE_MODULE(SceneLoader) {
    struct ObjectBinding {
        GameObjectId object_id;
    };

    std::unordered_map<std::string, ObjectBinding> object_bindings;
    std::vector<ModelTemplate> transient_models;
    SceneId current_scene_id;
    std::optional<SceneId> pending_scene_id;
    bool runtime_only_changes = false;

    void bindObjectTransform(const std::string &name, GameObjectId object_id);
    void clearRuntimeScene();
    void releaseTransientModels(bool deferred) noexcept;

  public:
    SceneLoader();
    ~SceneLoader();

    void load(SceneId scene_id);
    void requestLoad(SceneId scene_id);
    bool applyPendingLoad();
    const SceneId &currentScene() const;
    std::optional<GameObjectId> objectId(std::string_view name) const;
    bool hasObjectTransform(std::string_view name) const;
    SceneObjectTransform objectTransform(std::string_view name) const;
    void applyObjectTransform(std::string_view name, const SceneObjectTransform &transform);
    std::filesystem::path loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name);
    bool hasRuntimeOnlyChanges() const noexcept { return runtime_only_changes; }
};

} // namespace Pelican
