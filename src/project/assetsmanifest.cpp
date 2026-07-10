#include "assetsmanifest.hpp"

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view assets_schema = "pelican.assets";
constexpr int assets_version = 1;
constexpr std::string_view cache_schema = "pelican.assets_cache";
constexpr int cache_version = 1;

struct CachedHash {
    std::uintmax_t size = 0;
    std::string mtime;
    std::string sha256;
};

using HashCache = std::map<std::string, CachedHash>;

struct ScannedFile {
    std::string relative;
    std::filesystem::path absolute;
    std::uintmax_t size = 0;
    std::string mtime;
};

struct ScanResult {
    std::vector<ScannedFile> files;
    std::vector<AssetManifestIssue> issues;
};

std::string pathString(const std::filesystem::path &path) { return path.string(); }

bool isHexSha256(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

bool isNonNegativeInteger(const nlohmann::json &value) {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

std::uintmax_t nonNegativeInteger(const nlohmann::json &value) {
    return value.is_number_unsigned() ? value.get<std::uintmax_t>()
                                      : static_cast<std::uintmax_t>(value.get<std::int64_t>());
}

std::string lowerAscii(std::string_view value) {
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool hasAbsoluteSyntax(std::string_view value) {
    const std::filesystem::path path{std::string{value}};
    return path.is_absolute() || path.has_root_name() || value.starts_with('/') || value.starts_with('\\');
}

void validateRelativeFile(std::string_view file, std::string_view context) {
    if (file.empty()) {
        throw std::runtime_error(std::string{context} + " file must not be empty");
    }
    if (file.find('\\') != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " file must use forward slashes: " +
                                 std::string{file});
    }
    if (file.find("://") != std::string_view::npos || hasAbsoluteSyntax(file)) {
        throw std::runtime_error(std::string{context} + " file must be relative: " + std::string{file});
    }

    const std::filesystem::path path{std::string{file}};
    if (path.filename().empty() || path == ".") {
        throw std::runtime_error(std::string{context} + " file must name a file: " + std::string{file});
    }
    for (const auto &component : path) {
        if (component == "..") {
            throw std::runtime_error(std::string{context} + " file must not escape the store: " +
                                     std::string{file});
        }
    }
    if (path.lexically_normal().generic_string() != file) {
        throw std::runtime_error(std::string{context} + " file is not normalized: " + std::string{file});
    }
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + pathString(path));
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeTextFile(const std::filesystem::path &path, std::string_view contents) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("failed to create directory for " + pathString(path) + ": " +
                                     ec.message());
        }
    }

    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp");
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file.is_open()) {
            throw std::runtime_error("failed to write file: " + pathString(temporary));
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) {
            throw std::runtime_error("failed to finish writing file: " + pathString(temporary));
        }
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("failed to replace file: " + pathString(path) + " (" + ec.message() + ")");
    }
}

std::string fileMtime(const std::filesystem::path &path, std::error_code &ec) {
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return {};
    }
    return std::to_string(time.time_since_epoch().count());
}

std::string fileSha256(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to hash file: " + pathString(path));
    }
    return picosha2::hash256_hex_string(std::istreambuf_iterator<char>{file},
                                        std::istreambuf_iterator<char>{});
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path &path) {
    std::error_code ec;
    const auto normalized = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path).lexically_normal() : normalized;
}

bool sameNormalizedPath(const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
    auto left = normalizedAbsolute(lhs).generic_string();
    auto right = normalizedAbsolute(rhs).generic_string();
#ifdef _WIN32
    left = lowerAscii(left);
    right = lowerAscii(right);
#endif
    return left == right;
}

bool isWithinNormalizedRoot(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    auto root_string = normalizedAbsolute(root).generic_string();
    auto candidate_string = normalizedAbsolute(candidate).generic_string();
#ifdef _WIN32
    root_string = lowerAscii(root_string);
    candidate_string = lowerAscii(candidate_string);
#endif
    if (candidate_string == root_string) {
        return true;
    }
    if (!root_string.ends_with('/')) {
        root_string.push_back('/');
    }
    return candidate_string.starts_with(root_string);
}

ScanResult scanStore(const std::filesystem::path &store_root,
                     const std::filesystem::path &manifest_path,
                     const std::filesystem::path &cache_path,
                     bool tolerate_errors) {
    ScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(store_root, ec) || ec) {
        const auto message = "asset store directory is not accessible: " + pathString(store_root);
        if (!tolerate_errors) {
            throw std::runtime_error(message);
        }
        result.issues.push_back({AssetManifestIssueSeverity::warning,
                                 AssetManifestIssueKind::unreadable,
                                 {},
                                 message});
        return result;
    }

    std::filesystem::recursive_directory_iterator it{
        store_root, std::filesystem::directory_options::none, ec};
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        const auto message = "failed to scan asset store " + pathString(store_root) + ": " + ec.message();
        if (!tolerate_errors) {
            throw std::runtime_error(message);
        }
        result.issues.push_back({AssetManifestIssueSeverity::warning,
                                 AssetManifestIssueKind::unreadable,
                                 {},
                                 message});
        return result;
    }

    while (it != end) {
        const auto entry = *it;
        it.increment(ec);
        if (ec) {
            const auto message = "failed while scanning asset store " + pathString(store_root) + ": " +
                                 ec.message();
            if (!tolerate_errors) {
                throw std::runtime_error(message);
            }
            result.issues.push_back({AssetManifestIssueSeverity::warning,
                                     AssetManifestIssueKind::unreadable,
                                     {},
                                     message});
            ec.clear();
        }

        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec)) {
            if (entry_ec && tolerate_errors) {
                result.issues.push_back({AssetManifestIssueSeverity::warning,
                                         AssetManifestIssueKind::unreadable,
                                         {},
                                         "failed to inspect asset path: " + pathString(entry.path())});
            } else if (entry_ec) {
                throw std::runtime_error("failed to inspect asset path: " + pathString(entry.path()));
            }
            continue;
        }
        if (sameNormalizedPath(entry.path(), manifest_path) || sameNormalizedPath(entry.path(), cache_path)) {
            continue;
        }

        if (!isWithinNormalizedRoot(store_root, entry.path())) {
            const auto message = "asset path resolves outside the store: " + pathString(entry.path());
            if (!tolerate_errors) {
                throw std::runtime_error(message);
            }
            result.issues.push_back({AssetManifestIssueSeverity::warning,
                                     AssetManifestIssueKind::unreadable,
                                     {},
                                     message});
            continue;
        }

        const auto relative_path = entry.path().lexically_relative(store_root);
        const auto relative = relative_path.generic_string();
        try {
            validateRelativeFile(relative, "asset store");
        } catch (const std::exception &err) {
            if (!tolerate_errors) {
                throw;
            }
            result.issues.push_back({AssetManifestIssueSeverity::warning,
                                     AssetManifestIssueKind::unreadable,
                                     relative,
                                     err.what()});
            continue;
        }

        const auto size = entry.file_size(entry_ec);
        if (entry_ec) {
            const auto message = "failed to read asset size: " + relative;
            if (!tolerate_errors) {
                throw std::runtime_error(message);
            }
            result.issues.push_back({AssetManifestIssueSeverity::warning,
                                     AssetManifestIssueKind::unreadable,
                                     relative,
                                     message});
            continue;
        }
        const auto mtime = fileMtime(entry.path(), entry_ec);
        if (entry_ec) {
            const auto message = "failed to read asset mtime: " + relative;
            if (!tolerate_errors) {
                throw std::runtime_error(message);
            }
            result.issues.push_back({AssetManifestIssueSeverity::warning,
                                     AssetManifestIssueKind::unreadable,
                                     relative,
                                     message});
            continue;
        }
        result.files.push_back({relative, entry.path(), size, mtime});
    }

    std::sort(result.files.begin(), result.files.end(), [](const ScannedFile &lhs, const ScannedFile &rhs) {
        return lhs.relative < rhs.relative;
    });
    return result;
}

std::string cacheKey(std::string_view cache_namespace, std::string_view relative) {
    return std::string{cache_namespace} + ":" + std::string{relative};
}

HashCache loadCache(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return {};
    }

    try {
        const auto json = nlohmann::json::parse(readTextFile(path));
        if (!json.is_object() || json.value("schema", std::string{}) != cache_schema ||
            json.value("version", 0) != cache_version || !json.contains("entries") ||
            !json.at("entries").is_object()) {
            return {};
        }

        HashCache cache;
        for (const auto &[key, value] : json.at("entries").items()) {
            if (!value.is_object() || !value.contains("size") || !isNonNegativeInteger(value.at("size")) ||
                !value.contains("mtime") || !value.at("mtime").is_string() ||
                !value.contains("sha256") || !value.at("sha256").is_string()) {
                continue;
            }
            const auto sha = lowerAscii(value.at("sha256").get<std::string>());
            if (!isHexSha256(sha)) {
                continue;
            }
            cache.emplace(key, CachedHash{nonNegativeInteger(value.at("size")),
                                          value.at("mtime").get<std::string>(), sha});
        }
        return cache;
    } catch (const std::exception &) {
        return {};
    }
}

void saveCache(const std::filesystem::path &path, const HashCache &cache) {
    nlohmann::ordered_json entries = nlohmann::ordered_json::object();
    for (const auto &[key, value] : cache) {
        entries[key] = {
            {"size", value.size},
            {"mtime", value.mtime},
            {"sha256", value.sha256},
        };
    }
    const nlohmann::ordered_json json{
        {"schema", cache_schema},
        {"version", cache_version},
        {"entries", std::move(entries)},
    };
    writeTextFile(path, json.dump(2) + "\n");
}

std::string cachedOrFreshHash(const ScannedFile &file,
                              HashCache &cache,
                              std::string_view cache_namespace,
                              bool force_rehash,
                              bool &cache_hit) {
    const auto key = cacheKey(cache_namespace, file.relative);
    if (!force_rehash) {
        const auto found = cache.find(key);
        if (found != cache.end() && found->second.size == file.size && found->second.mtime == file.mtime &&
            isHexSha256(found->second.sha256)) {
            cache_hit = true;
            return lowerAscii(found->second.sha256);
        }
    }

    cache_hit = false;
    const auto sha = fileSha256(file.absolute);
    cache[key] = CachedHash{file.size, file.mtime, sha};
    return sha;
}

AssetManifestIssueSeverity issueSeverity(bool strict, AssetManifestIssueSeverity normal) {
    return strict ? AssetManifestIssueSeverity::error : normal;
}

void addIssue(AssetManifestVerificationResult &result,
              const AssetManifestVerifyOptions &options,
              AssetManifestIssueSeverity severity,
              AssetManifestIssueKind kind,
              std::string file,
              std::string message) {
    result.issues.push_back({issueSeverity(options.strict, severity), kind, std::move(file),
                             std::move(message)});
}

} // namespace

AssetsManifest parseAssetsManifestJson(std::string_view json_text) {
    const auto json = nlohmann::json::parse(json_text);
    if (!json.is_object()) {
        throw std::runtime_error("assets manifest must be an object");
    }
    if (json.value("schema", std::string{}) != assets_schema) {
        throw std::runtime_error("assets manifest schema is not supported");
    }
    if (!json.contains("version") || !json.at("version").is_number_integer() ||
        json.at("version").get<int>() != assets_version) {
        throw std::runtime_error("assets manifest version is not supported");
    }
    if (!json.contains("files") || !json.at("files").is_array()) {
        throw std::runtime_error("assets manifest requires files array");
    }
    if (!json.contains("tool") || !json.at("tool").is_object() ||
        !json.at("tool").contains("name") || !json.at("tool").at("name").is_string()) {
        throw std::runtime_error("assets manifest requires tool.name");
    }

    AssetsManifest manifest;
    std::set<std::string> seen;
    std::optional<std::string> previous;
    manifest.files.reserve(json.at("files").size());
    for (size_t i = 0; i < json.at("files").size(); ++i) {
        const auto &entry = json.at("files").at(i);
        const auto context = "assets manifest files[" + std::to_string(i) + "]";
        if (!entry.is_object() || !entry.contains("file") || !entry.at("file").is_string() ||
            !entry.contains("size") || !isNonNegativeInteger(entry.at("size")) ||
            !entry.contains("sha256") || !entry.at("sha256").is_string()) {
            throw std::runtime_error(context + " requires string file, unsigned size, and string sha256");
        }

        AssetManifestEntry parsed{
            entry.at("file").get<std::string>(),
            nonNegativeInteger(entry.at("size")),
            lowerAscii(entry.at("sha256").get<std::string>()),
        };
        validateRelativeFile(parsed.file, context);
        if (!isHexSha256(parsed.sha256)) {
            throw std::runtime_error(context + " sha256 must be 64 hex characters: " + parsed.file);
        }
        if (!seen.insert(parsed.file).second) {
            throw std::runtime_error("assets manifest contains duplicate file: " + parsed.file);
        }
        if (previous && *previous >= parsed.file) {
            throw std::runtime_error("assets manifest files must be sorted by relative path");
        }
        previous = parsed.file;
        manifest.files.push_back(std::move(parsed));
    }
    return manifest;
}

std::string serializeAssetsManifest(const AssetsManifest &manifest) {
    nlohmann::ordered_json files = nlohmann::ordered_json::array();
    std::optional<std::string> previous;
    for (const auto &entry : manifest.files) {
        validateRelativeFile(entry.file, "assets manifest");
        if (!isHexSha256(entry.sha256)) {
            throw std::runtime_error("assets manifest sha256 must be 64 hex characters: " + entry.file);
        }
        if (previous && *previous >= entry.file) {
            throw std::runtime_error("assets manifest files must be uniquely sorted by relative path");
        }
        previous = entry.file;
        files.push_back({
            {"file", entry.file},
            {"size", entry.size},
            {"sha256", lowerAscii(entry.sha256)},
        });
    }

    const nlohmann::ordered_json json{
        {"schema", assets_schema},
        {"version", assets_version},
        {"tool", {{"name", "pelican_cli assets manifest"}}},
        {"files", std::move(files)},
    };
    return json.dump(2) + "\n";
}

AssetManifestGenerationResult generateAssetsManifest(const std::filesystem::path &store_root,
                                                      const std::filesystem::path &manifest_path,
                                                      const std::filesystem::path &cache_path,
                                                      std::string_view cache_namespace,
                                                      bool full) {
    const auto scan = scanStore(store_root, manifest_path, cache_path, false);
    auto cache = loadCache(cache_path);
    std::set<std::string> live_cache_keys;

    AssetsManifest manifest;
    manifest.files.reserve(scan.files.size());
    AssetManifestGenerationResult result;
    for (const auto &file : scan.files) {
        bool cache_hit = false;
        const auto sha = cachedOrFreshHash(file, cache, cache_namespace, full, cache_hit);
        live_cache_keys.insert(cacheKey(cache_namespace, file.relative));
        manifest.files.push_back({file.relative, file.size, sha});
        ++result.files;
        if (cache_hit) {
            ++result.cache_hits;
        } else {
            ++result.hashed;
        }
    }

    const auto prefix = std::string{cache_namespace} + ":";
    std::erase_if(cache, [&](const auto &entry) {
        return entry.first.starts_with(prefix) && !live_cache_keys.contains(entry.first);
    });

    writeTextFile(manifest_path, serializeAssetsManifest(manifest));
    saveCache(cache_path, cache);
    return result;
}

AssetManifestVerificationResult verifyAssetsManifest(const std::filesystem::path &store_root,
                                                      const std::filesystem::path &manifest_path,
                                                      const std::filesystem::path &cache_path,
                                                      std::string_view cache_namespace,
                                                      const AssetManifestVerifyOptions &options) {
    AssetManifestVerificationResult result;
    AssetsManifest manifest;
    try {
        manifest = parseAssetsManifestJson(readTextFile(manifest_path));
    } catch (const std::exception &err) {
        addIssue(result, options, AssetManifestIssueSeverity::warning,
                 AssetManifestIssueKind::invalid_manifest, {},
                 "assets manifest is not readable: " + pathString(manifest_path) + " (" + err.what() + ")");
        return result;
    }

    const auto scan = scanStore(store_root, manifest_path, cache_path, true);
    for (const auto &issue : scan.issues) {
        addIssue(result, options, issue.severity, issue.kind, issue.file, issue.message);
    }

    std::map<std::string, const ScannedFile *> actual;
    std::unordered_map<std::string, std::vector<const ScannedFile *>> actual_casefolded;
    for (const auto &file : scan.files) {
        actual.emplace(file.relative, &file);
        actual_casefolded[lowerAscii(file.relative)].push_back(&file);
    }

    std::set<std::string> manifest_paths;
    std::set<std::string> case_matched_actual;
    auto cache = loadCache(cache_path);
    bool cache_changed = false;
    for (const auto &expected : manifest.files) {
        manifest_paths.insert(expected.file);
        const auto found = actual.find(expected.file);
        if (found == actual.end()) {
            const auto folded = actual_casefolded.find(lowerAscii(expected.file));
            if (folded != actual_casefolded.end() && !folded->second.empty()) {
                const auto &actual_file = *folded->second.front();
                case_matched_actual.insert(actual_file.relative);
                addIssue(result, options, AssetManifestIssueSeverity::warning,
                         AssetManifestIssueKind::case_mismatch, expected.file,
                         "asset path case mismatch: expected " + expected.file + ", found " +
                             actual_file.relative);
            } else {
                addIssue(result, options, AssetManifestIssueSeverity::warning,
                         AssetManifestIssueKind::missing, expected.file,
                         "asset is missing: " + expected.file);
            }
            continue;
        }

        ++result.matched;
        if (!options.include_content) {
            continue;
        }

        const auto &file = *found->second;
        try {
            bool cache_hit = false;
            const auto actual_sha = cachedOrFreshHash(file, cache, cache_namespace,
                                                      options.force_rehash, cache_hit);
            cache_changed = cache_changed || !cache_hit;
            if (file.size != expected.size || actual_sha != lowerAscii(expected.sha256)) {
                addIssue(result, options, AssetManifestIssueSeverity::info,
                         AssetManifestIssueKind::content_mismatch, expected.file,
                         "asset content differs: " + expected.file + " (expected size " +
                             std::to_string(expected.size) + ", sha256 " + expected.sha256 +
                             "; found size " + std::to_string(file.size) + ", sha256 " + actual_sha +
                             ") - run `pelican_cli assets manifest` to accept the update");
            }
        } catch (const std::exception &err) {
            addIssue(result, options, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::unreadable, expected.file, err.what());
        }
    }

    for (const auto &[path, file] : actual) {
        (void)file;
        if (!manifest_paths.contains(path) && !case_matched_actual.contains(path)) {
            addIssue(result, options, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::extra, path,
                     "asset is not listed in the manifest: " + path);
        }
    }

    if (cache_changed) {
        try {
            saveCache(cache_path, cache);
        } catch (const std::exception &err) {
            addIssue(result, options, AssetManifestIssueSeverity::warning,
                     AssetManifestIssueKind::unreadable, {}, err.what());
        }
    }
    return result;
}

int countAssetManifestIssues(const AssetManifestVerificationResult &result,
                             AssetManifestIssueSeverity severity) {
    return static_cast<int>(std::count_if(result.issues.begin(), result.issues.end(),
                                          [&](const AssetManifestIssue &issue) {
                                              return issue.severity == severity;
                                          }));
}

std::string_view assetManifestIssueSeverityName(AssetManifestIssueSeverity severity) {
    switch (severity) {
    case AssetManifestIssueSeverity::info:
        return "INFO";
    case AssetManifestIssueSeverity::warning:
        return "WARNING";
    case AssetManifestIssueSeverity::error:
        return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace Pelican
