#include "renderdocumentreload.hpp"

#include "inspectormodel.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace PelicanStudio {
namespace {

constexpr std::string_view ReloadMethod = "reload_render_documents";
constexpr std::string_view ReloadBusyCode = "reload_busy";
constexpr std::string_view ExternalModificationCode = "external_modification";
// A file that cannot be read is often one an editor is replacing: it is
// looked at again this many times before the reload is asked anyway.
constexpr int UnreadableChecks = 3;
// IA-1s, decision 5: what an engine before IA-2 does with a changed asset.
constexpr std::string_view UnreadAssetWords =
    " changed \xE2\x80\x94 the engine reads it on its next start.";

std::atomic<bool> refused_on_disk{false};

std::string textOf(const nlohmann::json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

std::string joined(const std::vector<std::string> &names) {
    std::string result;
    for (const auto &name : names) {
        if (!result.empty()) result += ", ";
        result += name;
    }
    return result;
}

std::vector<std::string> fileNames(const std::vector<std::string> &references) {
    std::vector<std::string> names;
    for (const auto &reference : references) {
        auto name = RenderDocumentReload::fileName(reference);
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }
    return names;
}

} // namespace

bool renderDocumentsRefusedOnDisk() noexcept { return refused_on_disk.load(); }

void setRenderDocumentsRefusedOnDisk(bool refused) noexcept { refused_on_disk.store(refused); }

RenderDocumentReload::RenderDocumentReload(Transport transport)
    : transport_{std::move(transport)} {}

std::optional<std::filesystem::path>
RenderDocumentReload::documentPath(const std::filesystem::path &project_root,
                                   std::string_view reference) {
    if (reference.empty() || reference.find('#') != std::string_view::npos) return std::nullopt;
    constexpr std::string_view project = "project://";
    if (reference.starts_with(project)) reference.remove_prefix(project.size());
    else if (reference.find("://") != std::string_view::npos) return std::nullopt;
    if (reference.empty()) return std::nullopt;
    const std::filesystem::path path{std::u8string{
        reinterpret_cast<const char8_t *>(reference.data()), reference.size()}};
    if (path.is_absolute()) return path.lexically_normal();
    if (project_root.empty()) return std::nullopt;
    return (project_root / path).lexically_normal();
}

std::string RenderDocumentReload::fileName(std::string_view reference) {
    const auto slash = reference.find_last_of("/\\");
    return std::string{slash == std::string_view::npos ? reference : reference.substr(slash + 1)};
}

std::int64_t RenderDocumentReload::send(const std::string &method, const Json &params,
                                        std::string *error) {
    if (!transport_.request) return 0;
    return transport_.request(method, params, error);
}

void RenderDocumentReload::say(Notice::Kind kind, std::string line, std::string detail) {
    if (notice) notice(Notice{kind, std::move(line), std::move(detail)});
}

void RenderDocumentReload::setRefused(bool refused) {
    refused_ = refused;
    setRenderDocumentsRefusedOnDisk(refused);
}

void RenderDocumentReload::open(std::filesystem::path project_root) {
    engineGone();
    project_root_ = std::move(project_root);
    documents_.clear();
    differing_.clear();
    differing_assets_.clear();
    placeAssets();
    busy_said_ = false;
    setRefused(false);
    if (watchChanged) watchChanged();
}

void RenderDocumentReload::engineReady() {
    if (project_root_.empty()) return;
    list_request_ = send("get_render_documents", Json::object(), nullptr);
}

void RenderDocumentReload::engineGone() {
    ++epoch_;
    list_request_ = 0;
    reload_request_ = 0;
    poll_request_ = 0;
    ticket_.clear();
    retrying_ = false;
    forced_ = false;
    again_ = false;
    unreadable_ = 0;
    // The engine that starts reads the assets as they are.
    differing_assets_.clear();
}

std::vector<std::filesystem::path> RenderDocumentReload::watchedFiles() const {
    std::vector<std::filesystem::path> files;
    for (const auto *watched : {&documents_, &assets_})
        for (const auto &[reference, document] : *watched) {
            (void)reference;
            if (std::find(files.begin(), files.end(), document.path) == files.end())
                files.push_back(document.path);
        }
    return files;
}

void RenderDocumentReload::setInitialAssets(const std::vector<std::string> &references) {
    if (references == asset_references_) return;
    asset_references_ = references;
    placeAssets();
}

void RenderDocumentReload::placeAssets() {
    std::map<std::string, Document> assets;
    for (const auto &reference : asset_references_) {
        const auto path = documentPath(project_root_, reference);
        if (!path) continue;
        // One already watched keeps the digest it was last seen with.
        if (const auto known = assets_.find(reference);
            known != assets_.end() && known->second.path == *path) {
            assets.insert_or_assign(reference, known->second);
            continue;
        }
        const auto digest = transport_.file_digest ? transport_.file_digest(*path) : std::nullopt;
        assets.insert_or_assign(reference, Document{*path, digest.value_or(std::string{})});
    }
    const bool moved = assets.size() != assets_.size() ||
                       !std::equal(assets.begin(), assets.end(), assets_.begin(),
                                   [](const auto &left, const auto &right) {
                                       return left.first == right.first &&
                                              left.second.path == right.second.path;
                                   });
    assets_ = std::move(assets);
    if (moved && watchChanged) watchChanged();
}

void RenderDocumentReload::fileChanged() {
    if (project_root_.empty() || !transport_.schedule) return;
    const auto serial = ++change_serial_;
    const auto epoch = epoch_;
    transport_.schedule(ReloadDebounceMs, [this, serial, epoch] {
        // A later write has its own check; this one is stale.
        if (serial != change_serial_ || epoch != epoch_) return;
        check(false);
    });
}

void RenderDocumentReload::reloadNow() { check(true); }

std::vector<std::string> RenderDocumentReload::differingDocuments() {
    std::vector<std::string> differing;
    bool unreadable = false;
    for (const auto &[reference, document] : documents_) {
        const auto digest =
            transport_.file_digest ? transport_.file_digest(document.path) : std::nullopt;
        if (!digest) unreadable = true;
        if (!digest || *digest != document.digest) differing.push_back(reference);
    }
    unreadable_ = unreadable ? unreadable_ + 1 : 0;
    return differing;
}

void RenderDocumentReload::check(bool forced) {
    if (project_root_.empty()) return;
    // Decision 1: a file an editor replaced is out of the watch; it goes back.
    if (watchChanged) watchChanged();
    if (reloadInFlight()) {
        again_ = true;
        forced_ = forced_ || forced;
        return;
    }
    differing_ = differingDocuments();
    if (!forced && unreadable_ > 0 && unreadable_ <= UnreadableChecks) {
        // Being replaced, most likely: look again once the writes settle.
        fileChanged();
        return;
    }
    // IA-1s, decision 5: an initial asset rewritten. What it holds now is
    // what this reload is asked about, so it is seen from here on; it stays
    // among the differing ones until an answer (a reload_busy retry asks
    // about it again).
    for (auto &[reference, asset] : assets_) {
        const auto digest =
            transport_.file_digest ? transport_.file_digest(asset.path) : std::nullopt;
        if (!digest || *digest == asset.digest) continue;
        asset.digest = *digest;
        if (std::find(differing_assets_.begin(), differing_assets_.end(), reference) ==
            differing_assets_.end())
            differing_assets_.push_back(reference);
    }
    if (differing_.empty() && differing_assets_.empty() && !forced) {
        // Decision 2: Studio's own write, or the file is back to what the
        // engine holds -- a refusal said about it no longer applies.
        if (refused_ || busy_said_) {
            setRefused(false);
            busy_said_ = false;
            say(Notice::Kind::None, {});
        }
        return;
    }
    forced_ = forced;
    sendReload();
}

void RenderDocumentReload::sendReload() {
    again_ = false;
    std::string error;
    reload_request_ = send(std::string{ReloadMethod}, Json::object(), &error);
    // No engine to ask: the one that starts reads the disk as it is.
    if (reload_request_ == 0) forced_ = false;
}

void RenderDocumentReload::poll() {
    if (ticket_.empty() || !transport_.schedule) return;
    const auto epoch = epoch_;
    transport_.schedule(TicketPollMs, [this, epoch] {
        if (epoch != epoch_ || ticket_.empty()) return;
        poll_request_ = send("get_edit_result", Json{{"ticket", ticket_}}, nullptr);
        if (poll_request_ == 0) ticket_.clear();
    });
}

bool RenderDocumentReload::receiveResult(std::int64_t id, const Json &result) {
    if (id == 0) return false;
    if (id == list_request_) {
        list_request_ = 0;
        readListing(result);
        return true;
    }
    if (id == reload_request_) {
        reload_request_ = 0;
        const auto status = textOf(result, "status");
        if (status == "accepted" && !textOf(result, "ticket").empty()) {
            ticket_ = textOf(result, "ticket");
            poll();
        } else if (status == "committed") {
            committed(result);
        } else {
            const auto error = result.is_object() && result.contains("error")
                                   ? result.at("error")
                                   : Json::object();
            if (textOf(error, "code") == ReloadBusyCode) busy(error);
            else refuse(error);
        }
        return true;
    }
    if (id == poll_request_) {
        poll_request_ = 0;
        const auto status = textOf(result, "status");
        if (status == "pending") {
            poll();
            return true;
        }
        ticket_.clear();
        if (status == "committed") {
            committed(result);
        } else {
            const auto error = result.is_object() && result.contains("error")
                                   ? result.at("error")
                                   : Json::object();
            // The disk moved again after the engine read it: whatever it is
            // now is checked once the writes stop.
            if (textOf(error, "code") == ExternalModificationCode) {
                again_ = false;
                fileChanged();
            } else {
                refuse(error);
            }
        }
        return true;
    }
    return false;
}

bool RenderDocumentReload::receiveFailure(std::int64_t id, const std::string &message) {
    if (id == 0) return false;
    if (id == list_request_) {
        list_request_ = 0;
        return true;
    }
    if (id != reload_request_ && id != poll_request_) return false;
    reload_request_ = 0;
    poll_request_ = 0;
    ticket_.clear();
    forced_ = false;
    say(Notice::Kind::Error, "Could not reload the render documents from disk.", message);
    return true;
}

void RenderDocumentReload::readListing(const Json &result) {
    const auto listed = result.is_object() ? result.find("documents") : result.end();
    if (!result.is_object() || listed == result.end() || !listed->is_array()) return;
    std::map<std::string, Document> documents;
    for (const auto &entry : *listed) {
        const auto reference = textOf(entry, "reference");
        const auto digest = textOf(entry, "digest");
        const auto path = documentPath(project_root_, reference);
        if (!path || digest.empty()) continue;
        documents.insert_or_assign(reference, Document{*path, digest});
    }
    documents_ = std::move(documents);
    if (watchChanged) watchChanged();
}

void RenderDocumentReload::observeReply(const Json &result) {
    if (!result.is_object() || textOf(result, "status") != "committed") return;
    const auto written = result.find("documents");
    if (written == result.end() || !written->is_object()) return;
    bool moved = false;
    for (const auto &[reference, digest] : written->items()) {
        if (digest.is_null()) {
            moved = documents_.erase(reference) > 0 || moved;
            continue;
        }
        if (!digest.is_string()) continue;
        if (const auto found = documents_.find(reference); found != documents_.end()) {
            found->second.digest = digest.get<std::string>();
            continue;
        }
        // A document the edit created (a fragment add_authored_pass wrote).
        const auto path = documentPath(project_root_, reference);
        if (!path) continue;
        documents_.insert_or_assign(reference, Document{*path, digest.get<std::string>()});
        moved = true;
    }
    if (moved && watchChanged) watchChanged();
}

void RenderDocumentReload::committed(const Json &commit) {
    // What the reload read is the whole set the engine composes now.
    if (const auto read = commit.find("documents"); read != commit.end() && read->is_object()) {
        std::map<std::string, Document> documents;
        for (const auto &[reference, digest] : read->items()) {
            if (!digest.is_string()) continue;
            const auto path = documentPath(project_root_, reference);
            if (!path) continue;
            documents.insert_or_assign(reference, Document{*path, digest.get<std::string>()});
        }
        documents_ = std::move(documents);
        if (watchChanged) watchChanged();
    }
    const bool said = refused_ || busy_said_;
    setRefused(false);
    busy_said_ = false;
    forced_ = false;
    // IA-1s, decision 5: an engine that compares the initial assets too
    // (IA-2) says which it read again; one that does not says nothing of
    // them, and reads a changed asset only when it starts.
    const auto assets_listed = commit.find("changed_assets");
    const bool knows_assets = assets_listed != commit.end() && assets_listed->is_array();
    std::vector<std::string> changed_assets;
    if (knows_assets)
        for (const auto &reference : *assets_listed)
            if (reference.is_string()) changed_assets.push_back(reference.get<std::string>());
    const auto unread = knows_assets ? std::vector<std::string>{} : differing_assets_;
    const auto no_change = commit.find("no_change");
    if (no_change != commit.end() && no_change->is_boolean() && no_change->get<bool>() &&
        changed_assets.empty()) {
        if (!unread.empty())
            say(Notice::Kind::Information, joined(unread) + std::string{UnreadAssetWords});
        else if (said)
            say(Notice::Kind::None, {});
    } else {
        std::vector<std::string> changed;
        if (const auto listed = commit.find("changed");
            listed != commit.end() && listed->is_array())
            for (const auto &reference : *listed)
                if (reference.is_string()) changed.push_back(reference.get<std::string>());
        if (reloaded) reloaded(commit);
        auto names = joined(fileNames(changed.empty() && changed_assets.empty() ? differing_
                                                                                 : changed));
        if (!changed_assets.empty())
            names += (names.empty() ? "" : ", ") + joined(changed_assets);
        std::string line = names.empty() ? std::string{"Picked up the change on disk."}
                                         : "Picked up the change to " + names + ".";
        if (!unread.empty()) line += " " + joined(unread) + std::string{UnreadAssetWords};
        say(Notice::Kind::Success, std::move(line));
    }
    differing_assets_.clear();
    if (std::exchange(again_, false)) check(false);
}

void RenderDocumentReload::refuse(const Json &error) {
    forced_ = false;
    const auto code = textOf(error, "code");
    const auto message = textOf(error, "message");
    const auto participant = textOf(error, "participant");
    const auto reference = textOf(error, "reference");
    auto differing = differing_;
    differing.insert(differing.end(), differing_assets_.begin(), differing_assets_.end());
    differing_assets_.clear();
    auto file = reference.empty() ? joined(fileNames(differing)) : fileName(reference);
    if (file.empty()) file = "the render documents";
    std::string where;
    if (!participant.empty()) where = participant;
    if (!reference.empty()) where += (where.empty() ? "" : ", ") + reference;
    std::string detail = "The engine keeps the previous version of " + file +
                         "; fix the file and save again.";
    // IA-1s, decision 5: a refusal Studio has a sentence for (an initial
    // asset that does not fit its image, IA-2) says it first.
    if (const auto sentence = graphLoopRefusalDescription(error); !sentence.empty())
        detail = sentence + "\n" + detail;
    if (!code.empty() || !message.empty()) {
        detail += "\n" + (code.empty() ? std::string{"refused"} : code);
        if (!where.empty()) detail += " (" + where + ")";
        detail += ": " + message;
    }
    setRefused(true);
    say(Notice::Kind::Error, "Could not load " + file + ": fix the file and save again.",
        std::move(detail));
    if (std::exchange(again_, false)) check(false);
}

void RenderDocumentReload::busy(const Json &error) {
    // Decision 3: the edit Studio has in flight lands first (its commit may
    // turn out to be the change), or the preview is closed; then again.
    if (textOf(error, "reason") == "preview_open" && !busy_said_) {
        busy_said_ = true;
        const auto names = joined(fileNames(differing_));
        say(Notice::Kind::Information,
            names.empty() ? std::string{"Close the scene preview to pick up the change on disk."}
                          : "Close the scene preview to pick up the change to " + names + ".");
    }
    if (!transport_.schedule) return;
    retrying_ = true;
    const auto epoch = epoch_;
    transport_.schedule(BusyRetryMs, [this, epoch] {
        if (epoch != epoch_) return;
        retrying_ = false;
        again_ = false;
        check(forced_);
    });
}

} // namespace PelicanStudio
