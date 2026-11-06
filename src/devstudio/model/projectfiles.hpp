#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

enum class ProjectFileKind {
    Shader,
    Prefab,
    Material,
    Texture,
    Mesh,
    Sound,
    Scene,
    Graph,
    Feature,
    Other,
};

std::string projectFileKindName(ProjectFileKind kind);

enum class PrefabRegistration { NotPrefab, Registered, NotRegistered, Unknown };

struct ProjectFileEntry {
    std::string path;
    std::string name;
    int parent = -1;
    bool directory = false;
    bool link = false;
    bool cloud_placeholder = false;
    bool unreadable = false;
    ProjectFileKind kind = ProjectFileKind::Other;
    PrefabRegistration registration = PrefabRegistration::NotPrefab;
};

struct ProjectListing {
    std::vector<ProjectFileEntry> entries;
    bool truncated = false;
    bool cancelled = false;
    bool root_unreadable = false;
    bool declarations_unread = false;
    bool json_budget_exhausted = false;
    bool examined_limit_reached = false;
    std::size_t max_entries = 0;
    std::size_t examined_entries = 0;
    std::uintmax_t json_bytes_inspected = 0;
};

struct ProjectScanOptions {
    std::size_t max_entries = 10000;
    std::size_t max_examined_entries = 200000;
    std::uintmax_t max_json_bytes = 8 * 1024 * 1024;
    std::uintmax_t max_json_total_bytes = 256 * 1024 * 1024;
    std::function<bool()> should_stop;
};

ProjectListing scanProjectFolder(const std::filesystem::path &root,
                                 const ProjectScanOptions &options = {});

std::string projectFileKindLabel(const ProjectFileEntry &entry);

struct ProjectFilter {
    std::string text;
    std::optional<ProjectFileKind> kind;
};

std::vector<char> projectFilterVisible(const ProjectListing &listing,
                                       const ProjectFilter &filter);

struct ProjectTreeEdit {
    enum class Op { Remove, Insert, Update } op = Op::Insert;
    std::string path;
    std::string parent_path;
    int row = 0;
    ProjectFileEntry entry;
};

std::vector<ProjectTreeEdit> diffProjectListings(const ProjectListing &shown,
                                                 const ProjectListing &next);

struct ProjectScanTag {
    std::uint64_t project = 0;
    std::uint64_t generation = 0;
};

class ProjectScanGate {
    std::uint64_t project_ = 0;
    std::uint64_t generation_ = 0;

  public:
    void openProject() {
        ++project_;
        ++generation_;
    }
    ProjectScanTag beginScan() {
        ++generation_;
        return {project_, generation_};
    }
    void invalidate() { ++generation_; }
    [[nodiscard]] bool isCurrent(ProjectScanTag tag) const {
        return tag.project == project_ && tag.generation == generation_;
    }
};

} // namespace PelicanStudio
