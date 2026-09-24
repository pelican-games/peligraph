#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace PelicanStudio {

// OL-1: an object is the authoring id the engine gave it (`scene_tree`'s
// `authoring_object_id`), which a duplicate, a delete, a reparent or an undo
// does not move. Its declaration index is where it stands in the scene's
// order right now, which any of those can move. A key read from the Outliner
// carries both; a key with no id (0) -- a pick, an older caller -- names the
// object at that index, and the project answers it with the full key.
struct OutlinerObjectKey {
    std::string scene_id;
    std::size_t declaration_index = 0;
    std::uint64_t authoring_object_id = 0;

    bool operator==(const OutlinerObjectKey &) const = default;
};

struct OutlinerObject {
    OutlinerObjectKey key;
    std::string display_name;
    // The authored name; empty for an unnamed object (which the engine
    // cannot reparent, and which no other object can name as its parent).
    std::string name;
    std::optional<std::size_t> parent_declaration_index;
    std::vector<std::size_t> child_declaration_indices;
};

struct OutlinerScene {
    std::string scene_id;
    std::vector<OutlinerObject> objects;
    std::vector<std::size_t> root_declaration_indices;
};

struct OfflinePrefabExpansion {
    std::string scene_id;
    std::string object_name;
    std::string instance_id;
    std::vector<std::string> resolved_component_json;
};

// Project metadata plus the most recent typed scene_tree RPC projection.
// Object identity is (scene id, authoring object id); the declaration index is
// the display order, and authored names are presentation and parent-reference
// data only.
class ProjectOutlinerModel {
    std::filesystem::path project_root_;
    std::optional<std::string> project_name_;
    std::vector<OutlinerScene> scenes_;
    std::vector<std::string> warnings_;
    std::vector<OfflinePrefabExpansion> offline_prefab_expansions_;
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
    const std::vector<OfflinePrefabExpansion> &offlinePrefabExpansions() const
        noexcept {
        return offline_prefab_expansions_;
    }
    const std::optional<std::uint64_t> &sceneRevision() const noexcept {
        return scene_revision_;
    }

    // By authoring id when the key has one, wherever the object stands now;
    // by declaration index when it does not.
    const OutlinerObject *findObject(const OutlinerObjectKey &key) const noexcept;
    const OutlinerScene *findScene(const std::string &scene_id) const noexcept;
    const OutlinerObject *parentOf(const OutlinerObject &object) const noexcept;
    // Whether `candidate` is `ancestor` or sits somewhere under it.
    bool isSelfOrDescendant(const OutlinerObject &ancestor,
                            const OutlinerObject &candidate) const noexcept;
    // Children, grandchildren and so on; not the object itself.
    std::size_t descendantCount(const OutlinerObject &object) const noexcept;
};

} // namespace PelicanStudio
