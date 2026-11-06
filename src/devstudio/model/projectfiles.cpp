#include "projectfiles.hpp"

#include <projectformat.hpp>
#include <projectmigration.hpp>
#include <shadersourceresolver.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace PelicanStudio {
namespace {

std::string utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path pathFromUtf8(const std::string &value) {
    const auto *first = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path{std::u8string{first, first + value.size()}};
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                    : static_cast<char>(c);
    });
    return value;
}

bool stopped(const ProjectScanOptions &options) {
    return options.should_stop && options.should_stop();
}

enum class EntryType { Link, CloudPlaceholder, Directory, Regular, Other, Unreadable };

#ifdef _WIN32
EntryType windowsReparseType(const std::filesystem::path &path, bool directory) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return EntryType::Unreadable;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return directory ? EntryType::Directory : EntryType::Regular;
    WIN32_FIND_DATAW data{};
    const HANDLE handle = FindFirstFileW(path.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return EntryType::Unreadable;
    FindClose(handle);
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return directory ? EntryType::Directory : EntryType::Regular;
    if ((data.dwReserved0 & ~IO_REPARSE_TAG_CLOUD_MASK) == IO_REPARSE_TAG_CLOUD)
        return EntryType::CloudPlaceholder;
    return EntryType::Link;
}
#endif

EntryType entryType(const std::filesystem::directory_entry &entry) {
    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (error) return EntryType::Unreadable;
    if (std::filesystem::is_symlink(status)) return EntryType::Link;
    const bool directory = std::filesystem::is_directory(status);
    const bool regular = std::filesystem::is_regular_file(status);
#ifdef _WIN32
    // MSVC reports a junction as its own type: neither a directory nor a symlink.
    if (directory || regular || status.type() == std::filesystem::file_type::junction)
        return windowsReparseType(entry.path(), directory);
#else
    if (directory) return EntryType::Directory;
    if (regular) return EntryType::Regular;
#endif
    return EntryType::Other;
}

bool skipped(const std::string &name, bool under_root, EntryType type) {
    if (name == ".git") return true;
    if (!under_root || type != EntryType::Directory) return false;
    return name == ".pelican" || name == "build" ||
           (name.size() > 6 && name.starts_with("build-")) || name == "out" ||
           name == "output";
}

std::optional<ProjectFileKind> schemaKind(std::string_view schema) {
    if (schema == "pelican.prefab") return ProjectFileKind::Prefab;
    if (schema == "pelican.material") return ProjectFileKind::Material;
    if (schema == "pelican.scene") return ProjectFileKind::Scene;
    static constexpr std::string_view graphs[] = {
        "pelican.graph", "pelican.frame_plan", "pelican.authored_render_fragment",
        "pelican.ray_group_layout", "pelican.ray_bindings"};
    if (std::find(std::begin(graphs), std::end(graphs), schema) != std::end(graphs))
        return ProjectFileKind::Graph;
    static constexpr std::string_view features[] = {
        "pelican.render_feature", "pelican.render_feature_overlay",
        "pelican.render_feature_parameters"};
    if (std::find(std::begin(features), std::end(features), schema) != std::end(features))
        return ProjectFileKind::Feature;
    return std::nullopt;
}

ProjectFileKind extensionKind(const std::string &name) {
    const auto dot = name.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string{} : asciiLower(name.substr(dot));
    if (Pelican::hasKnownShaderSourceExtension(name) || extension == ".surface" ||
        extension == ".glsl")
        return ProjectFileKind::Shader;
    static constexpr std::string_view textures[] = {
        ".png", ".jpg", ".jpeg", ".ktx", ".ktx2", ".hdr", ".exr",
        ".dds", ".tga", ".bmp", ".webp", ".psd"};
    if (std::find(std::begin(textures), std::end(textures), extension) != std::end(textures))
        return ProjectFileKind::Texture;
    static constexpr std::string_view meshes[] = {
        ".glb", ".gltf", ".vrm", ".obj", ".fbx", ".usd", ".usda", ".usdc", ".usdz"};
    if (std::find(std::begin(meshes), std::end(meshes), extension) != std::end(meshes))
        return ProjectFileKind::Mesh;
    static constexpr std::string_view sounds[] = {".wav", ".ogg", ".mp3", ".flac"};
    if (std::find(std::begin(sounds), std::end(sounds), extension) != std::end(sounds))
        return ProjectFileKind::Sound;
    return ProjectFileKind::Other;
}

struct Declarations {
    std::unordered_set<std::string> prefabs;
    std::unordered_map<std::string, ProjectFileKind> kinds;
};

std::optional<std::string> readBounded(const std::filesystem::path &path,
                                       std::uintmax_t maximum) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) return std::nullopt;
    std::string bytes;
    char buffer[16384];
    while (input && bytes.size() <= maximum) {
        const auto remaining = maximum + 1 - bytes.size();
        input.read(buffer, static_cast<std::streamsize>(
                               std::min<std::size_t>(sizeof(buffer), remaining)));
        bytes.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    if (input.bad() || bytes.size() > maximum) return std::nullopt;
    return bytes;
}

std::string normalizedDeclaration(const std::string &path) {
    return utf8(pathFromUtf8(path).lexically_normal());
}

Declarations readDeclarations(const std::filesystem::path &root,
                              const ProjectScanOptions &options,
                              ProjectListing &listing) {
    Declarations declarations;
    const auto path = root / "project.json";
    std::error_code error;
    const std::filesystem::directory_entry entry{path, error};
    const auto allowance = options.max_json_total_bytes - listing.json_bytes_inspected;
    bool failed = error || stopped(options);
    if (!error && !failed) {
        const auto type = entryType(entry);
        failed = type != EntryType::Regular;
        if (!failed) {
            const auto size = entry.file_size(error);
            failed = error || size > options.max_json_bytes;
            if (!failed && size > allowance) {
                listing.json_budget_exhausted = true;
                failed = true;
            }
            if (!failed && !stopped(options)) {
                auto bytes = readBounded(path, options.max_json_bytes);
                listing.json_bytes_inspected += size;
                failed = !bytes.has_value();
                if (bytes) {
                    try {
                        Pelican::ProjectEnvelopeParseOptions parse_options;
                        parse_options.ignore_engine_version = true;
                        const auto parsed = Pelican::parseProjectEnvelopeText(*bytes, parse_options);
                        for (const auto &prefab : parsed.envelope.prefabs) {
                            const auto normalized = normalizedDeclaration(prefab.path);
                            declarations.prefabs.insert(normalized);
                            declarations.kinds[normalized] = ProjectFileKind::Prefab;
                        }
                        const auto &config = parsed.envelope.basic_config;
                        for (const auto &[key, kind] :
                             {std::pair{"scene_data_json", ProjectFileKind::Scene},
                              std::pair{"rendering_config_json", ProjectFileKind::Graph}}) {
                            const auto found = config.find(key);
                            if (found != config.end() && found->is_string())
                                declarations.kinds[normalizedDeclaration(found->get<std::string>())] = kind;
                        }
                    } catch (...) {
                        failed = true;
                    }
                }
            }
        }
    }
    if (stopped(options)) {
        listing.cancelled = true;
        return declarations;
    }
    listing.declarations_unread = failed;
    return declarations;
}

void applyDeclaration(ProjectFileEntry &entry, const Declarations &declarations) {
    const auto found = declarations.kinds.find(entry.path);
    if (found != declarations.kinds.end()) entry.kind = found->second;
}

void inspectJson(const std::filesystem::path &full_path, ProjectFileEntry &entry,
                 const ProjectScanOptions &options, const Declarations &declarations,
                 ProjectListing &listing) {
    std::error_code error;
    const auto size = std::filesystem::file_size(full_path, error);
    if (error) {
        entry.unreadable = true;
        return;
    }
    if (size > options.max_json_bytes) {
        applyDeclaration(entry, declarations);
        return;
    }
    if (listing.json_budget_exhausted ||
        size > options.max_json_total_bytes -
                   std::min(options.max_json_total_bytes, listing.json_bytes_inspected)) {
        listing.json_budget_exhausted = true;
        applyDeclaration(entry, declarations);
        return;
    }
    if (stopped(options)) {
        listing.cancelled = true;
        return;
    }
    listing.json_bytes_inspected += size;
    if (const auto envelope = Pelican::readDocumentEnvelope(full_path, options.max_json_bytes)) {
        if (const auto kind = schemaKind(envelope->schema)) {
            entry.kind = *kind;
            return;
        }
    }
    applyDeclaration(entry, declarations);
}

struct PendingEntry {
    std::filesystem::path full_path;
    ProjectFileEntry entry;
    EntryType type = EntryType::Other;
};

bool entryLess(const PendingEntry &lhs, const PendingEntry &rhs) {
    if (lhs.entry.directory != rhs.entry.directory) return lhs.entry.directory;
    const auto folded_lhs = asciiLower(lhs.entry.name);
    const auto folded_rhs = asciiLower(rhs.entry.name);
    if (folded_lhs != folded_rhs) return folded_lhs < folded_rhs;
    return lhs.entry.name < rhs.entry.name;
}

} // namespace

std::string projectFileKindName(ProjectFileKind kind) {
    switch (kind) {
    case ProjectFileKind::Shader: return "shader";
    case ProjectFileKind::Prefab: return "prefab";
    case ProjectFileKind::Material: return "material";
    case ProjectFileKind::Texture: return "texture";
    case ProjectFileKind::Mesh: return "mesh";
    case ProjectFileKind::Sound: return "sound";
    case ProjectFileKind::Scene: return "scene";
    case ProjectFileKind::Graph: return "graph";
    case ProjectFileKind::Feature: return "feature";
    case ProjectFileKind::Other: return "other";
    }
    return "other";
}

ProjectListing scanProjectFolder(const std::filesystem::path &root,
                                 const ProjectScanOptions &options) {
    ProjectListing listing;
    listing.max_entries = options.max_entries;
    const auto declarations = readDeclarations(root, options, listing);
    if (listing.cancelled) return listing;

    struct Folder {
        std::filesystem::path full_path;
        std::string relative_path;
        int entry_index = -1;
    };
    std::deque<Folder> folders;
    folders.push_back({root, {}, -1});
    bool stop_all = false;
    while (!folders.empty() && !listing.cancelled) {
        const auto folder = std::move(folders.front());
        folders.pop_front();
        std::vector<PendingEntry> batch;
        std::error_code error;
        std::filesystem::directory_iterator iterator{folder.full_path, error};
        const std::filesystem::directory_iterator end;
        if (error) {
            if (folder.entry_index < 0)
                listing.root_unreadable = true;
            else
                listing.entries[folder.entry_index].unreadable = true;
            continue;
        }
        while (iterator != end) {
            if (stopped(options)) {
                listing.cancelled = true;
                return listing;
            }
            if (listing.examined_entries >= options.max_examined_entries) {
                listing.examined_limit_reached = true;
                listing.truncated = true;
                stop_all = true;
                break;
            }
            ++listing.examined_entries;
            const auto current = *iterator;
            std::string name;
            try {
                name = utf8(current.path().filename());
            } catch (...) {
                iterator.increment(error);
                if (error) break;
                continue;
            }
            const auto type = entryType(current);
            if (skipped(name, folder.entry_index < 0, type)) {
                iterator.increment(error);
                if (error) break;
                continue;
            }
            if (listing.entries.size() + batch.size() >= options.max_entries) {
                listing.truncated = true;
                stop_all = true;
                break;
            }
            ProjectFileEntry entry;
            entry.name = std::move(name);
            entry.path = folder.relative_path.empty()
                             ? entry.name
                             : folder.relative_path + "/" + entry.name;
            entry.parent = folder.entry_index;
            entry.directory = type == EntryType::Directory;
            entry.link = type == EntryType::Link;
            entry.cloud_placeholder = type == EntryType::CloudPlaceholder;
            entry.unreadable = type == EntryType::Unreadable;
            if (type == EntryType::Regular) entry.kind = extensionKind(entry.name);
            batch.push_back({current.path(), std::move(entry), type});
            iterator.increment(error);
            if (error) break;
        }
        if (error) {
            if (folder.entry_index < 0)
                listing.root_unreadable = true;
            else if (!listing.entries.empty() && folder.entry_index < static_cast<int>(listing.entries.size()))
                listing.entries[folder.entry_index].unreadable = true;
        }
        std::sort(batch.begin(), batch.end(), entryLess);
        for (auto &pending : batch) {
            auto &entry = pending.entry;
            const auto dot = entry.name.find_last_of('.');
            const bool json = dot != std::string::npos &&
                              asciiLower(entry.name.substr(dot)) == ".json";
            if (pending.type == EntryType::Regular && json && entry.path != "project.json") {
                inspectJson(pending.full_path, entry, options, declarations, listing);
                if (listing.cancelled) return listing;
            } else if (pending.type == EntryType::Regular && entry.path == "project.json") {
                applyDeclaration(entry, declarations);
            }
            if (entry.kind == ProjectFileKind::Prefab) {
                entry.registration = listing.declarations_unread
                                         ? PrefabRegistration::Unknown
                                         : declarations.prefabs.contains(entry.path)
                                               ? PrefabRegistration::Registered
                                               : PrefabRegistration::NotRegistered;
            }
            const int index = static_cast<int>(listing.entries.size());
            if (entry.directory && !stop_all)
                folders.push_back({pending.full_path, entry.path, index});
            listing.entries.push_back(std::move(entry));
        }
        if (stop_all) break;
    }
    return listing;
}

std::string projectFileKindLabel(const ProjectFileEntry &entry) {
    if (entry.cloud_placeholder) return "cloud placeholder";
    if (entry.link) return "link";
    if (entry.unreadable) return "unreadable";
    if (entry.directory) return {};
    if (entry.kind != ProjectFileKind::Prefab) return projectFileKindName(entry.kind);
    switch (entry.registration) {
    case PrefabRegistration::Registered: return "prefab (registered)";
    case PrefabRegistration::NotRegistered: return "prefab (not registered)";
    case PrefabRegistration::Unknown: return "prefab (unknown)";
    case PrefabRegistration::NotPrefab: return "prefab";
    }
    return "prefab";
}

std::vector<char> projectFilterVisible(const ProjectListing &listing,
                                       const ProjectFilter &filter) {
    const auto &entries = listing.entries;
    std::vector<char> visible(entries.size(), 0);
    const bool active = !filter.text.empty() || filter.kind.has_value();
    if (!active) return std::vector<char>(entries.size(), 1);
    const auto folded_text = asciiLower(filter.text);
    const auto matches = [&](const ProjectFileEntry &entry) {
        const bool text = folded_text.empty() ||
                          asciiLower(entry.path).find(folded_text) != std::string::npos;
        const bool kind = !filter.kind || entry.kind == *filter.kind;
        return text && kind;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].directory) {
            visible[i] = matches(entries[i]);
        } else if (!filter.kind && !folded_text.empty()) {
            visible[i] = asciiLower(entries[i].path).find(folded_text) != std::string::npos;
        }
    }
    for (std::size_t i = entries.size(); i-- > 0;) {
        if (visible[i] && entries[i].parent >= 0)
            visible[static_cast<std::size_t>(entries[i].parent)] = 1;
    }
    return visible;
}

namespace {

bool sameRow(const ProjectFileEntry &lhs, const ProjectFileEntry &rhs) {
    return lhs.directory == rhs.directory && lhs.link == rhs.link &&
           lhs.cloud_placeholder == rhs.cloud_placeholder &&
           lhs.unreadable == rhs.unreadable && lhs.kind == rhs.kind &&
           lhs.registration == rhs.registration;
}

} // namespace

std::vector<ProjectTreeEdit> diffProjectListings(const ProjectListing &shown,
                                                 const ProjectListing &next) {
    std::unordered_map<std::string, int> shown_paths;
    std::unordered_map<std::string, int> next_paths;
    for (std::size_t i = 0; i < shown.entries.size(); ++i)
        shown_paths.emplace(shown.entries[i].path, static_cast<int>(i));
    for (std::size_t i = 0; i < next.entries.size(); ++i)
        next_paths.emplace(next.entries[i].path, static_cast<int>(i));

    std::vector<char> removed(shown.entries.size(), 0);
    for (std::size_t i = 0; i < shown.entries.size(); ++i) {
        const auto found = next_paths.find(shown.entries[i].path);
        removed[i] = found == next_paths.end() ||
                     shown.entries[i].directory !=
                         next.entries[static_cast<std::size_t>(found->second)].directory;
    }

    std::vector<ProjectTreeEdit> edits;
    for (std::size_t i = 0; i < shown.entries.size(); ++i) {
        if (!removed[i]) continue;
        const int parent = shown.entries[i].parent;
        if (parent >= 0 && removed[static_cast<std::size_t>(parent)]) continue;
        edits.push_back({ProjectTreeEdit::Op::Remove, shown.entries[i].path, {}, 0, {}});
    }

    std::unordered_map<int, int> sibling_rows;
    for (std::size_t i = 0; i < next.entries.size(); ++i) {
        const auto &entry = next.entries[i];
        const int row = sibling_rows[entry.parent]++;
        const auto found = shown_paths.find(entry.path);
        const bool insert = found == shown_paths.end() ||
                            removed[static_cast<std::size_t>(found->second)];
        if (insert) {
            const std::string parent = entry.parent < 0
                                           ? std::string{}
                                           : next.entries[static_cast<std::size_t>(entry.parent)].path;
            edits.push_back({ProjectTreeEdit::Op::Insert, entry.path, parent, row, entry});
        } else {
            const auto &old = shown.entries[static_cast<std::size_t>(found->second)];
            if (!sameRow(old, entry))
                edits.push_back({ProjectTreeEdit::Op::Update, entry.path, {}, row, entry});
        }
    }
    return edits;
}

} // namespace PelicanStudio
