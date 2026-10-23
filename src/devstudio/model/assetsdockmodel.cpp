#include "assetsdockmodel.hpp"

#include "appliedstate.hpp"
#include "inspectormodel.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t *>(text.data()),
                                               text.size()}};
}

std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - start);
        if (!part.empty()) parts.emplace_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

// `inner` is `outer` or inside it, component by component.
bool within(const std::vector<std::string> &inner, const std::vector<std::string> &outer) {
    return outer.size() <= inner.size() && std::equal(outer.begin(), outer.end(), inner.begin());
}

std::uint64_t counter(const Json &object, const char *key) {
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }
    return 0;
}

std::string text(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::vector<std::string> pathList(const Json &value) {
    std::vector<std::string> paths;
    if (!value.is_array()) return paths;
    for (const auto &entry : value)
        if (entry.is_string()) paths.push_back(normalizedProjectPath(entry.get<std::string>()));
    return paths;
}

void flatten(const std::vector<AssetEntry> &entries,
             std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> &files) {
    for (const auto &entry : entries) {
        if (entry.directory) flatten(entry.children, files);
        else files[entry.logical] = {entry.modified, entry.size};
    }
}

std::vector<AssetEntry> scanFolder(const std::filesystem::path &physical, const std::string &logical) {
    std::vector<AssetEntry> entries;
    std::error_code error;
    std::filesystem::directory_iterator iterator{physical, error};
    if (error) return entries;
    for (const auto &item : iterator) {
        const auto name = utf8(item.path().filename());
        if (name == ".gitkeep") continue;
        AssetEntry entry;
        entry.name = name;
        entry.logical = logical + "/" + name;
        entry.physical = item.path();
        std::error_code status_error;
        entry.directory = item.is_directory(status_error);
        if (entry.directory) {
            entry.children = scanFolder(entry.physical, entry.logical);
        } else {
            entry.size = item.file_size(status_error);
            if (status_error) entry.size = 0;
            std::error_code time_error;
            const auto written = item.last_write_time(time_error);
            if (!time_error) entry.modified = written.time_since_epoch().count();
        }
        entries.push_back(std::move(entry));
    }
    std::ranges::sort(entries, [](const AssetEntry &left, const AssetEntry &right) {
        if (left.directory != right.directory) return left.directory;
        return left.name < right.name;
    });
    return entries;
}

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string{count == 1 ? one : many};
}

// The kinds in the order the line and the list name them.
constexpr std::string_view IssueKinds[] = {"content_mismatch", "missing", "extra",
                                           "case_mismatch", "unreadable", "invalid_manifest"};

} // namespace

std::string normalizedProjectPath(std::string_view path) {
    std::string spelled{path};
    std::ranges::replace(spelled, '\\', '/');
    std::string joined;
    for (const auto &part : components(spelled)) {
        if (part == ".") continue;
        if (!joined.empty()) joined += '/';
        joined += part;
    }
    return joined;
}

AssetWatchState assetWatchState(std::string_view logical, const std::vector<std::string> &watch) {
    const auto row = components(normalizedProjectPath(logical));
    AssetWatchState state;
    std::size_t closest = 0;
    for (const auto &listed : watch) {
        const auto parts = components(normalizedProjectPath(listed));
        if (parts.empty() || !within(row, parts)) continue;
        if (parts.size() == row.size()) return {AssetWatchMark::Direct, {}};
        if (parts.size() > closest) {
            closest = parts.size();
            state = {AssetWatchMark::Inherited, normalizedProjectPath(listed)};
        }
    }
    return state;
}

std::vector<std::string> watchWithPath(const std::vector<std::string> &watch, std::string_view logical,
                                       bool on) {
    const auto path = normalizedProjectPath(logical);
    const auto parts = components(path);
    std::vector<std::string> next;
    for (const auto &listed : watch) {
        const auto normal = normalizedProjectPath(listed);
        if (normal == path) continue;
        // Switched on, a folder covers what is inside it.
        if (on && within(components(normal), parts)) continue;
        if (std::ranges::find(next, normal) == next.end()) next.push_back(normal);
    }
    if (on) next.push_back(path);
    return next;
}

std::vector<AssetRoot> assetRoots(const std::filesystem::path &project_root, const Json &asset_stores,
                                  const Json &status_stores) {
    std::vector<AssetRoot> roots;
    if (project_root.empty()) return roots;
    roots.push_back({"assets", project_root / "assets"});
    if (!asset_stores.is_object()) return roots;
    for (const auto &[name, store] : asset_stores.items()) {
        const auto mount = normalizedProjectPath(text(store, "mount"));
        if (mount.empty()) continue;
        const auto parts = components(mount);
        const bool covered = std::ranges::any_of(
            roots, [&](const AssetRoot &root) { return within(parts, components(root.logical)); });
        if (covered) continue;
        std::filesystem::path physical = project_root / pathFromUtf8(mount);
        if (status_stores.is_object())
            if (const auto found = status_stores.find(name);
                found != status_stores.end() && found->is_string())
                physical = pathFromUtf8(found->get<std::string>());
        roots.push_back({mount, physical});
    }
    return roots;
}

std::vector<AssetEntry> scanAssetRoots(const std::vector<AssetRoot> &roots) {
    std::vector<AssetEntry> entries;
    for (const auto &root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root.physical, error)) continue;
        AssetEntry entry;
        entry.logical = root.logical;
        entry.name = root.logical;
        entry.physical = root.physical;
        entry.directory = true;
        entry.children = scanFolder(root.physical, root.logical);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<std::string> changedAssetFiles(const std::vector<AssetEntry> &before,
                                           const std::vector<AssetEntry> &after) {
    std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> old_files;
    std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> new_files;
    flatten(before, old_files);
    flatten(after, new_files);
    std::vector<std::pair<std::int64_t, std::string>> changed;
    for (const auto &[logical, stamp] : new_files) {
        const auto found = old_files.find(logical);
        if (found == old_files.end() || found->second != stamp) changed.emplace_back(stamp.first, logical);
    }
    std::ranges::sort(changed);
    std::vector<std::string> logical;
    for (auto &[modified, path] : changed) logical.push_back(std::move(path));
    return logical;
}

std::string assetPathKey(const std::filesystem::path &path) {
    auto key = utf8(path.lexically_normal());
    while (key.size() > 1 && key.back() == '/') key.pop_back();
#ifdef _WIN32
    for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return key;
}

AssetsVerificationSummary parseAssetsVerificationSummary(const Json &status) {
    AssetsVerificationSummary summary;
    if (!status.is_object()) return summary;
    const auto found = status.find("assets_verification");
    if (found == status.end() || !found->is_object()) return summary;
    const auto state = text(*found, "state");
    using State = AssetsVerificationSummary::State;
    if (state == "not_run") summary.state = State::NotRun;
    else if (state == "running") summary.state = State::Running;
    else if (state == "verified") summary.state = State::Verified;
    else return summary;
    summary.reason = text(*found, "reason");
    summary.stores = counter(*found, "stores");
    summary.matched = counter(*found, "matched");
    summary.info = counter(*found, "info");
    summary.warnings = counter(*found, "warnings");
    summary.errors = counter(*found, "errors");
    return summary;
}

std::vector<AssetStoreVerification> parseAssetsVerificationDetail(const Json &detail) {
    std::vector<AssetStoreVerification> stores;
    if (!detail.is_object()) return stores;
    const auto listed = detail.find("stores");
    if (listed == detail.end() || !listed->is_array()) return stores;
    for (const auto &store : *listed) {
        if (!store.is_object()) continue;
        AssetStoreVerification result;
        result.name = text(store, "name");
        result.root = pathFromUtf8(text(store, "root"));
        result.manifest = pathFromUtf8(text(store, "manifest"));
        result.matched = counter(store, "matched");
        if (const auto issues = store.find("issues"); issues != store.end() && issues->is_array())
            for (const auto &issue : *issues) {
                if (!issue.is_object()) continue;
                AssetIssue entry;
                entry.kind = text(issue, "kind");
                entry.severity = text(issue, "severity");
                entry.file = text(issue, "file");
                entry.message = text(issue, "message");
                if (!entry.file.empty()) entry.physical = result.root / pathFromUtf8(entry.file);
                result.issues.push_back(std::move(entry));
            }
        stores.push_back(std::move(result));
    }
    return stores;
}

std::string assetIssueKindLabel(std::string_view kind) {
    if (kind == "content_mismatch") return "content differs";
    if (kind == "missing") return "missing";
    if (kind == "extra") return "extra";
    if (kind == "case_mismatch") return "case differs";
    if (kind == "unreadable") return "unreadable";
    if (kind == "invalid_manifest") return "manifest unreadable";
    return std::string{kind};
}

std::string assetsVerificationLine(const AssetsVerificationSummary &summary,
                                   const std::vector<AssetStoreVerification> &detail) {
    using State = AssetsVerificationSummary::State;
    if (summary.state == State::Running) return "Checking asset contents\xE2\x80\xA6";
    if (summary.state != State::Verified) return {};
    const auto issues = summary.issues();
    if (issues == 0)
        return "Asset contents match the manifest (" + plural(summary.matched, "file", "files") + ").";
    std::string line = plural(issues, "file differs", "files differ") + " from the manifest";
    std::map<std::string, std::uint64_t> kinds;
    for (const auto &store : detail)
        for (const auto &issue : store.issues) ++kinds[issue.kind];
    std::string counts;
    const auto add = [&](const std::string &kind, std::uint64_t count) {
        if (!counts.empty()) counts += ", ";
        counts += std::to_string(count) + " " + assetIssueKindLabel(kind);
    };
    for (const auto kind : IssueKinds)
        if (const auto found = kinds.find(std::string{kind}); found != kinds.end()) {
            add(found->first, found->second);
            kinds.erase(found);
        }
    for (const auto &[kind, count] : kinds) add(kind, count);
    if (!counts.empty()) line += ": " + counts;
    return line + ".";
}

std::string manifestUpdateLine(const AssetStoreVerification &store) {
    const auto manifest = store.manifest.empty() ? std::string{"the manifest"}
                                                 : utf8(store.manifest.filename());
    const auto changing = static_cast<std::uint64_t>(std::ranges::count_if(
        store.issues, [](const AssetIssue &issue) { return issue.kind != "invalid_manifest"; }));
    if (changing == 0) return "Nothing differs; " + manifest + " is written again as it is.";
    return plural(changing, "file", "files") + " will change in " + manifest + ".";
}

std::string assetsWatchLine(const std::vector<std::string> &watch,
                            const std::function<bool(const std::string &)> &is_file,
                            const std::string &last_reload) {
    if (watch.empty()) return {};
    std::uint64_t folders = 0;
    std::uint64_t files = 0;
    for (const auto &path : watch) (is_file && is_file(path) ? files : folders) += 1;
    std::string line = "Watching ";
    if (folders > 0) line += plural(folders, "folder", "folders");
    if (folders > 0 && files > 0) line += " and ";
    if (files > 0) line += plural(files, "file", "files");
    if (!last_reload.empty()) {
        std::string shown = last_reload;
        constexpr std::string_view assets = "assets/";
        if (shown.starts_with(assets) && shown.size() > assets.size()) shown.erase(0, assets.size());
        line += " \xC2\xB7 last reload: " + shown;
    }
    return line;
}

bool assetStoresDeclareManifest(const Json &asset_stores) {
    if (!asset_stores.is_object()) return false;
    for (const auto &[name, store] : asset_stores.items())
        if (!text(store, "manifest").empty()) return true;
    return false;
}

Json assetStoresWithManifest(const Json &asset_stores) {
    if (!asset_stores.is_object() || asset_stores.empty())
        return Json{{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    Json stores = asset_stores;
    std::set<std::string> used;
    for (const auto &[name, store] : stores.items())
        if (const auto manifest = text(store, "manifest"); !manifest.empty()) used.insert(manifest);
    for (auto &[name, store] : stores.items()) {
        if (!store.is_object() || !text(store, "manifest").empty()) continue;
        std::string manifest = "assets.manifest.json";
        if (used.contains(manifest)) manifest = "assets." + name + ".manifest.json";
        used.insert(manifest);
        store["manifest"] = manifest;
    }
    return stores;
}

// --- the session

AssetsDockSession::AssetsDockSession(Transport transport) : transport_{std::move(transport)} {}

void AssetsDockSession::notify() {
    if (changed) changed();
}

void AssetsDockSession::setNotice(std::string line, std::string detail) {
    notice_ = std::move(line);
    notice_detail_ = std::move(detail);
}

std::int64_t AssetsDockSession::send(const std::string &method, const Json &params) {
    if (!transport_.request) return 0;
    std::string error;
    const auto id = transport_.request(method, params, &error);
    if (id == 0 && !error.empty()) setNotice("Could not ask the engine (" + method + "): " + error);
    return id;
}

void AssetsDockSession::forgetRequests() {
    ++generation_;
    settings_request_ = 0;
    edit_request_ = 0;
    status_request_ = 0;
    status_again_ = false;
    status_scheduled_ = false;
    detail_request_ = 0;
    verify_request_ = 0;
    update_request_ = 0;
    verification_reads_left_ = 0;
    verification_started_ = false;
    reload_reads_left_ = 0;
    detail_wanted_ = false;
}

void AssetsDockSession::open(std::filesystem::path project_root) {
    forgetRequests();
    root_ = std::move(project_root);
    ready_ = false;
    settings_read_ = false;
    digest_.clear();
    hot_reload_ = nullptr;
    asset_stores_ = nullptr;
    watch_.clear();
    status_stores_ = Json::object();
    hashed_files_.reset();
    summary_ = {};
    detail_.clear();
    reload_candidates_.clear();
    last_reload_.clear();
    setNotice({});
    restart_reason_.clear();
    undo_.clear();
    edit_.reset();
    watch_switched_on_ = false;
    notify();
}

void AssetsDockSession::engineReady() {
    forgetRequests();
    ready_ = true;
    edit_.reset();
    restart_reason_.clear();
    // A new engine read project.json at its start: what it watches is all
    // that was saved before it, and nothing it missed.
    watch_switched_on_ = false;
    hashed_files_.reset();
    readSettings();
    readStatus();
    notify();
}

void AssetsDockSession::engineGone() {
    forgetRequests();
    ready_ = false;
    settings_read_ = false;
    edit_.reset();
    summary_ = {};
    detail_.clear();
    hashed_files_.reset();
    reload_candidates_.clear();
    notify();
}

bool AssetsDockSession::startEdit(PendingEdit edit) {
    if (!ready_ || edit_) return false;
    setNotice({});
    edit_ = std::move(edit);
    readSettings();
    notify();
    return edit_.has_value();
}

bool AssetsDockSession::setWatched(const std::string &logical, bool on) {
    return startEdit({PendingEdit::Kind::Watch, normalizedProjectPath(logical), on});
}

bool AssetsDockSession::trackContents() {
    if (!trackOffered()) return false;
    return startEdit({PendingEdit::Kind::Track, {}, false});
}

bool AssetsDockSession::undo() {
    if (undo_.empty()) return false;
    return startEdit({PendingEdit::Kind::Undo, {}, false});
}

void AssetsDockSession::readSettings() {
    if (settings_request_ != 0) return;
    settings_request_ = send("get_project_settings", Json::object());
    if (settings_request_ == 0) edit_.reset();
}

void AssetsDockSession::readSettingsResult(const Json &result) {
    settings_read_ = true;
    digest_ = text(result, "project_digest");
    hot_reload_ = result.is_object() ? result.value("hot_reload", Json{}) : Json{};
    asset_stores_ = result.is_object() ? result.value("asset_stores", Json{}) : Json{};
    if (edit_) sendEdit();
}

void AssetsDockSession::sendEdit() {
    Json params{{"base_project_digest", digest_}};
    switch (edit_->kind) {
    case PendingEdit::Kind::Watch: {
        const auto current = pathList(hot_reload_);
        const auto next = watchWithPath(current, edit_->logical, edit_->on);
        if (next == current) {
            edit_.reset();
            return;
        }
        // The last one switched off leaves project.json as it was before any:
        // no hot_reload at all.
        params["hot_reload"] = next.empty() ? Json(nullptr) : Json(next);
        break;
    }
    case PendingEdit::Kind::Track:
        if (assetStoresDeclareManifest(asset_stores_)) {
            edit_.reset();
            return;
        }
        params["asset_stores"] = assetStoresWithManifest(asset_stores_);
        break;
    case PendingEdit::Kind::Undo:
        if (undo_.empty()) {
            edit_.reset();
            return;
        }
        for (const auto &[key, value] : undo_.back().previous.items()) params[key] = value;
        break;
    }
    edit_request_ = send("edit_project_settings", params);
    if (edit_request_ == 0) edit_.reset();
}

void AssetsDockSession::editResult(const Json &result) {
    const auto edit = *edit_;
    edit_.reset();
    if (const auto digest = text(result, "project_digest"); !digest.empty()) digest_ = digest;
    if (const auto applied = result.find("applied"); applied != result.end() && applied->is_object()) {
        hot_reload_ = applied->value("hot_reload", Json{});
        asset_stores_ = applied->value("asset_stores", Json{});
        // Applied in place; get_status says the same once it is read.
        watch_ = pathList(hot_reload_);
    }
    const bool changed_now = result.value("changed", false);
    if (edit.kind == PendingEdit::Kind::Undo) {
        if (!undo_.empty()) undo_.pop_back();
    } else if (changed_now) {
        const char *key = edit.kind == PendingEdit::Kind::Watch ? "hot_reload" : "asset_stores";
        UndoEntry entry;
        entry.description = edit.kind == PendingEdit::Kind::Watch
                                ? std::string{"Reload when saved outside ("} + edit.logical + ")"
                                : std::string{"Track asset contents"};
        const auto previous = result.find("previous");
        entry.previous[key] = previous != result.end() && previous->is_object()
                                  ? previous->value(key, Json{})
                                  : Json{};
        undo_.push_back(std::move(entry));
    }
    if (edit.kind == PendingEdit::Kind::Watch && edit.on && changed_now) watch_switched_on_ = true;
    // RT-C: restart_required read by appliedstate (`applied` here is the
    // settings object read above, never "unusable").
    if (const auto state = readAppliedState(result, "project.json changed");
        state.kind == AppliedState::Kind::RestartRequired) {
        restart_reason_ = state.restart_reason;
    } else {
        restart_reason_.clear();
    }
    if (const auto verification = result.find("assets_verification");
        verification != result.end() && text(*verification, "state") == "running") {
        detail_wanted_ = true;
        startVerificationFollow();
        return;
    }
    readStatus();
}

bool AssetsDockSession::verificationRunning() const noexcept {
    return summary_.state == AssetsVerificationSummary::State::Running || verification_started_ ||
           verify_request_ != 0 || update_request_ != 0 ||
           (edit_ && edit_->kind == PendingEdit::Kind::Track);
}

bool AssetsDockSession::verifyAgain() {
    if (!ready_ || verificationRunning()) return false;
    setNotice({});
    verify_request_ = send("verify_assets", Json::object());
    notify();
    return verify_request_ != 0;
}

bool AssetsDockSession::updateManifest(const std::string &store) {
    if (!ready_ || verificationRunning() || store.empty()) return false;
    setNotice({});
    update_request_ = send("update_assets_manifest", Json{{"store", store}});
    notify();
    return update_request_ != 0;
}

void AssetsDockSession::startVerificationFollow() {
    verification_started_ = true;
    verification_reads_left_ = VerificationPollLimit;
    readStatus();
}

void AssetsDockSession::readStatus() {
    if (!ready_) return;
    if (status_request_ != 0) {
        status_again_ = true;
        return;
    }
    status_request_ = send("get_status", Json::object());
}

void AssetsDockSession::scheduleStatusRead(int milliseconds) {
    if (status_scheduled_ || !transport_.schedule) return;
    status_scheduled_ = true;
    transport_.schedule(milliseconds, [this, generation = generation_] {
        if (generation != generation_) return;
        status_scheduled_ = false;
        readStatus();
    });
}

void AssetsDockSession::readDetail() {
    detail_wanted_ = false;
    if (detail_request_ != 0) return;
    detail_request_ = send("get_assets_verification", Json::object());
}

void AssetsDockSession::filesChanged(const std::vector<std::string> &logical) {
    bool watched = false;
    for (const auto &path : logical) {
        if (assetWatchState(path, watch_).mark == AssetWatchMark::None) continue;
        std::erase(reload_candidates_, path);
        reload_candidates_.push_back(path);
        watched = true;
    }
    if (!watched || !ready_) return;
    reload_reads_left_ = ReloadFollowReads;
    scheduleStatusRead(ReloadFollowMs);
}

void AssetsDockSession::say(std::string line) {
    setNotice(std::move(line));
    notify();
}

bool AssetsDockSession::owns(std::int64_t id) const noexcept {
    return id != 0 && (id == settings_request_ || id == edit_request_ || id == status_request_ ||
                       id == detail_request_ || id == verify_request_ || id == update_request_);
}

void AssetsDockSession::observeStatus(std::int64_t id, const Json &status) {
    const bool ours = id != 0 && id == status_request_;
    if (ours) status_request_ = 0;
    // Every panel's get_status passes here, several a second while the GPU
    // time is shown: the dock is told only when something it shows moved.
    const auto watch_before = watch_;
    const auto summary_before = summary_;
    const auto last_reload_before = last_reload_;
    const auto stores_before = status_stores_;
    if (status.is_object()) {
        if (const auto reload = status.find("reload"); reload != status.end() && reload->is_object()) {
            if (const auto watch = reload->find("watch"); watch != reload->end())
                watch_ = pathList(*watch);
            const auto hashed = counter(*reload, "hashed_files");
            if (hashed_files_ && hashed > *hashed_files_ && !reload_candidates_.empty()) {
                // The newest file written under the watch is what was read.
                for (auto candidate = reload_candidates_.rbegin(); candidate != reload_candidates_.rend();
                     ++candidate)
                    if (assetWatchState(*candidate, watch_).mark != AssetWatchMark::None) {
                        last_reload_ = *candidate;
                        break;
                    }
                reload_candidates_.clear();
                reload_reads_left_ = 0;
            }
            hashed_files_ = hashed;
        }
        if (const auto stores = status.find("stores"); stores != status.end() && stores->is_object())
            status_stores_ = *stores;
        const auto summary = parseAssetsVerificationSummary(status);
        using State = AssetsVerificationSummary::State;
        if (summary.state != State::Unknown) {
            const bool moved = !(summary == summary_);
            summary_ = summary;
            if (summary.state == State::Running) {
                if (verification_reads_left_ == 0) {
                    verification_started_ = true;
                    verification_reads_left_ = VerificationPollLimit;
                }
            } else {
                verification_reads_left_ = 0;
                verification_started_ = false;
                if (summary.state == State::NotRun) {
                    detail_.clear();
                    detail_wanted_ = false;
                } else if (detail_wanted_ || moved) {
                    readDetail();
                }
            }
        }
    }
    if (ours) {
        if (std::exchange(status_again_, false)) {
            readStatus();
        } else if (summary_.state == AssetsVerificationSummary::State::Running &&
                   verification_reads_left_ > 0) {
            --verification_reads_left_;
            scheduleStatusRead(VerificationPollMs);
        } else if (reload_reads_left_ > 0) {
            --reload_reads_left_;
            scheduleStatusRead(ReloadFollowMs);
        }
    }
    if (ours || watch_ != watch_before || !(summary_ == summary_before) ||
        last_reload_ != last_reload_before || status_stores_ != stores_before)
        notify();
}

bool AssetsDockSession::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    if (id == status_request_) {
        observeStatus(id, result);
        return true;
    }
    if (id == settings_request_) {
        settings_request_ = 0;
        readSettingsResult(result);
    } else if (id == edit_request_) {
        edit_request_ = 0;
        if (edit_) editResult(result);
    } else if (id == detail_request_) {
        detail_request_ = 0;
        detail_ = parseAssetsVerificationDetail(result);
    } else if (id == verify_request_ || id == update_request_) {
        if (id == verify_request_) verify_request_ = 0;
        else update_request_ = 0;
        if (text(result, "state") == "running") {
            detail_wanted_ = true;
            startVerificationFollow();
        } else {
            readStatus();
        }
    }
    notify();
    return true;
}

bool AssetsDockSession::receiveRefusal(std::int64_t id, const std::string &error_code,
                                       const std::string &message, const Json &error_fields) {
    if (!owns(id)) return false;
    const Json error{{"code", error_code}, {"message", message},
                     {"error_fields", error_fields.is_object() ? error_fields : Json::object()}};
    if (id == edit_request_) {
        edit_request_ = 0;
        const bool undo = edit_ && edit_->kind == PendingEdit::Kind::Undo;
        edit_.reset();
        auto sentence = projectSettingsRefusalDescription(error, undo);
        setNotice(sentence.empty() ? message : sentence, message);
        // The digest it was written against is old: read the file again.
        if (error_code == "external_modification") readSettings();
    } else if (id == verify_request_ || id == update_request_) {
        if (id == verify_request_) verify_request_ = 0;
        else update_request_ = 0;
        auto sentence = projectSettingsRefusalDescription(error);
        setNotice(sentence.empty() ? message : sentence, message);
        readStatus();
    } else {
        return receiveFailure(id, message);
    }
    notify();
    return true;
}

bool AssetsDockSession::receiveFailure(std::int64_t id, const std::string &message) {
    if (!owns(id)) return false;
    if (id == settings_request_) {
        settings_request_ = 0;
        if (edit_) setNotice("Could not read project.json's settings: " + message, message);
        edit_.reset();
    } else if (id == edit_request_) {
        edit_request_ = 0;
        edit_.reset();
        setNotice("Could not change project.json: " + message, message);
    } else if (id == status_request_) {
        status_request_ = 0;
        status_again_ = false;
    } else if (id == detail_request_) {
        detail_request_ = 0;
    } else if (id == verify_request_ || id == update_request_) {
        if (id == verify_request_) verify_request_ = 0;
        else update_request_ = 0;
        setNotice("Could not ask the engine to check the assets: " + message, message);
    }
    notify();
    return true;
}

std::vector<std::string> AssetsDockSession::shownWatch() const {
    if (edit_ && edit_->kind == PendingEdit::Kind::Watch)
        return watchWithPath(watch_, edit_->logical, edit_->on);
    return watch_;
}

bool AssetsDockSession::trackOffered() const noexcept {
    return ready_ && settings_read_ && !assetStoresDeclareManifest(asset_stores_) &&
           !(edit_ && edit_->kind == PendingEdit::Kind::Track);
}

} // namespace PelicanStudio
