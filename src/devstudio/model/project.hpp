#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

// Engine-independent, read-only projection of project.json and its scene
// document. Object identity is always (scene id, declaration index); authored
// names are presentation and parent-reference data only.
class ProjectOutlinerModel {
    std::filesystem::path project_root_;
    std::optional<std::string> project_name_;
    std::vector<OutlinerScene> scenes_;
    std::vector<std::string> warnings_;

  public:
    static ProjectOutlinerModel open(const std::filesystem::path &project_path);

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
};

} // namespace PelicanStudio
