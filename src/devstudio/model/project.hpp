#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace PelicanStudio {

struct OutlinerObjectKey {
    std::string scene_id;
    std::size_t declaration_index = 0;

    bool operator==(const OutlinerObjectKey &) const = default;
};

struct OutlinerObject {
    OutlinerObjectKey key;
    std::string display_name;
    std::optional<std::size_t> parent_declaration_index;
    std::vector<std::size_t> child_declaration_indices;
};

struct OutlinerScene {
    std::string scene_id;
    std::vector<OutlinerObject> objects;
    std::vector<std::size_t> root_declaration_indices;
};

// Project metadata plus the most recent typed scene_tree RPC projection.
// Object identity is always (scene id, declaration index); authored names are
// presentation and parent-reference data only.
class ProjectOutlinerModel {
    std::filesystem::path project_root_;
    std::optional<std::string> project_name_;
    std::vector<OutlinerScene> scenes_;
    std::vector<std::string> warnings_;
    std::optional<std::uint64_t> scene_revision_;

  public:
    static ProjectOutlinerModel open(const std::filesystem::path &project_path);
    bool updateSceneTree(const nlohmann::json &scene_tree);

    const std::filesystem::path &projectRoot() const noexcept {
        return project_root_;
    }
    const std::optional<std::string> &projectName() const noexcept {
        return project_name_;
    }
    const std::vector<OutlinerScene> &scenes() const noexcept {
        return scenes_;
    }
    const std::vector<std::string> &warnings() const noexcept {
        return warnings_;
    }
    const std::optional<std::uint64_t> &sceneRevision() const noexcept {
        return scene_revision_;
    }

    const OutlinerObject *findObject(const OutlinerObjectKey &key) const noexcept;
};

} // namespace PelicanStudio
