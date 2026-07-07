#pragma once

#include "../container.hpp"

#include <filesystem>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Pelican {

using SceneId = std::string;

struct SceneObjectTransform {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;
};

DECLARE_MODULE(SceneLoader) {
    struct ObjectBinding {
        void *transform = nullptr;
        void *simple_model_view = nullptr;
    };

    std::unordered_map<std::string, ObjectBinding> object_bindings;

    void bindObjectTransform(const std::string &name, void *transform, void *simple_model_view);

  public:
    SceneLoader();
    ~SceneLoader();

    void load(SceneId scene_id);
    bool hasObjectTransform(std::string_view name) const;
    SceneObjectTransform objectTransform(std::string_view name) const;
    void applyObjectTransform(std::string_view name, const SceneObjectTransform &transform);
    std::filesystem::path loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name);
};

} // namespace Pelican
