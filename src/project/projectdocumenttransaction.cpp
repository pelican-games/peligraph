#include "projectdocumenttransaction.hpp"

#include <picosha2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>

#include <cstddef>
#include <cstring>

// Windows 10 RS1 named these. Spell out the values when the SDK in use
// predates them, so the file still compiles and the call simply fails with
// ERROR_INVALID_PARAMETER at runtime, which takes the named fallback below.
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001
#endif
#ifndef FILE_RENAME_FLAG_POSIX_SEMANTICS
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x00000002
#endif
#endif

namespace Pelican {
namespace {

using Json = nlohmann::ordered_json;

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("could not open render authoring document: " +
                                 path.string());
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error("could not open render authoring staging file: " +
                                 path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not flush render authoring staging file: " +
                                 path.string());
    }
    output.close();
    if (!output) {
        throw std::runtime_error("could not close render authoring staging file: " +
                                 path.string());
    }
}

std::mutex &routeMutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<ProjectDocumentTransactionWarningSink> &warningSink() {
    static std::atomic<ProjectDocumentTransactionWarningSink> sink{nullptr};
    return sink;
}

ProjectDocumentAtomicReplaceRoutes &routeState() {
    static ProjectDocumentAtomicReplaceRoutes state;
    return state;
}

void notePosixRename() {
    const std::lock_guard<std::mutex> guard{routeMutex()};
    ++routeState().posix_renames;
}

void noteLegacyFallback(const std::filesystem::path &destination,
                        const std::string &reason) {
    {
        const std::lock_guard<std::mutex> guard{routeMutex()};
        auto &state = routeState();
        ++state.legacy_fallbacks;
        state.last_fallback_reason = reason;
    }
    if (const auto sink = warningSink().load()) {
        sink("render authoring atomic replace fell back to MoveFileExW for " +
             destination.string() + ": " + reason);
    }
}

#ifdef _WIN32
std::string systemErrorMessage(DWORD code) {
    const std::error_code error{static_cast<int>(code),
                                std::system_category()};
    return error.message() + " (" + std::to_string(code) + ")";
}

// A sharing handle another process holds may go away on its own.
bool isTransientReplaceError(DWORD code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
           code == ERROR_LOCK_VIOLATION;
}

// FileRenameInfoEx is a Windows 10 RS1 information class that a pre-RS1 kernel
// and a filesystem that does not implement it reject outright. These are the
// answers that mean "this route does not exist here", not "this replacement
// failed".
bool isUnsupportedInformationClass(DWORD code) {
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED ||
           code == ERROR_INVALID_FUNCTION || code == ERROR_CALL_NOT_IMPLEMENTED;
}

// Replace `destination` with `source` in one directory operation that
// supersedes a destination another process still holds open.
//
// MoveFileExW(MOVEFILE_REPLACE_EXISTING) cannot do that. A real-time scanner
// opens the file it is inspecting with FILE_SHARE_READ | FILE_SHARE_WRITE |
// FILE_SHARE_DELETE -- sharing everything, precisely so it does not break the
// application -- and the classic rename still refuses to supersede a target
// that has any open handle, returning ERROR_ACCESS_DENIED for as long as that
// handle lives. POSIX semantics changes that answer: the link is dropped from
// the directory immediately, the superseded file stays alive under its open
// handles, and later openers see the new file. It does not bypass the share
// mode -- a holder that withheld FILE_SHARE_DELETE still yields
// ERROR_SHARING_VIOLATION -- so the retry loop around this call stays.
// "replaces a manifest a real-time scanner holds open" measures both halves.
//
// This does NOT close the intermittent "Access is denied" on manifest.json.
// An interleaved A/B of the two routes under a saturated scanner (704 runs of
// the same multi-section editor test each) failed 36 times on MoveFileExW and
// 24 times on this route, always ERROR_ACCESS_DENIED on the destination after
// the full retry window. Whatever is left is a second cause, and it is not a
// memory-mapped destination (POSIX rename supersedes one of those) nor a
// delete-pending one. The structural answer was to stop superseding a live
// file for the transaction's own decision at all: the committed marker is now
// a separate name published by linkExclusive below, manifest.json is written
// once and never replaced, and the remaining callers of this route replace
// authored documents rather than transaction metadata.
bool posixReplace(const std::filesystem::path &source,
                  const std::filesystem::path &destination,
                  DWORD &last_error, bool &staged_open_failed) {
    // Ask for exactly what a rename needs and nothing more. MoveFileExW opens
    // its source with DELETE | SYNCHRONIZE, so this open cannot be refused
    // anywhere that one would have succeeded -- which matters when the handle
    // a scanner holds is on the staged file rather than on the destination.
    // Widening this (FILE_WRITE_DATA, to flush the staged bytes and recover
    // what MOVEFILE_WRITE_THROUGH stood for) would trade that guarantee away.
    // The sharing flags are the other direction: granting all three only makes
    // this open tolerate handles other processes already hold.
    const HANDLE handle = CreateFileW(
        source.c_str(), DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // The staged file, not the destination. Say which, because the two
        // read identically in a failure message and lead to different causes.
        last_error = GetLastError();
        staged_open_failed = true;
        return false;
    }

    // FileRenameInfoEx wants a fully qualified target: RootDirectory is null,
    // so a relative path would be resolved against nothing.
    std::error_code absolute_error;
    const auto target_path =
        destination.is_absolute()
            ? destination
            : std::filesystem::absolute(destination, absolute_error);
    if (absolute_error) {
        CloseHandle(handle);
        last_error = ERROR_BAD_PATHNAME;
        return false;
    }
    const std::wstring target = target_path.wstring();
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes +
                                   sizeof(wchar_t));
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    info->Flags =
        FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, target.c_str(),
                name_bytes + sizeof(wchar_t));

    const BOOL renamed = SetFileInformationByHandle(
        handle, FileRenameInfoEx, info,
        static_cast<DWORD>(storage.size()));
    if (!renamed) last_error = GetLastError();
    CloseHandle(handle);
    return renamed != FALSE;
}
#endif

void atomicReplace(const std::filesystem::path &source,
                   const std::filesystem::path &destination,
                   std::string_view expected_digest = {}) {
#ifdef _WIN32
    // A heavily parallel ctest run can make Windows Defender retain a newly
    // staged JSON file for longer than two seconds. Keep rechecking the CAS on
    // every attempt, but allow that bounded sharing lock up to ten seconds.
    constexpr unsigned int retry_count = 5000;
    bool posix_available = true;
    std::string fallback_reason;
    DWORD last_error = ERROR_SUCCESS;
    const char *last_route = "FileRenameInfoEx";
    for (unsigned int attempt = 0; attempt < retry_count; ++attempt) {
        if (!expected_digest.empty()) {
            std::error_code state_error;
            if (!std::filesystem::is_regular_file(destination,
                                                  state_error) ||
                state_error ||
                projectDocumentDigest(readBytes(destination)) !=
                    expected_digest) {
                throw ProjectDocumentExternalModification{
                    "render authoring document changed while an atomic replacement was retried: " +
                    destination.string()};
            }
        }
        if (posix_available) {
            DWORD posix_error = ERROR_SUCCESS;
            bool staged_open_failed = false;
            if (posixReplace(source, destination, posix_error,
                             staged_open_failed)) {
                notePosixRename();
                return;
            }
            last_error = posix_error;
            last_route = staged_open_failed
                             ? "opening the staged file for rename"
                             : "FileRenameInfoEx";
            if (isTransientReplaceError(posix_error)) {
                // A holder that withheld FILE_SHARE_DELETE. MoveFileExW cannot
                // get past that either, so keep retrying this route rather
                // than reporting a fallback that would not help.
                Sleep(2);
                continue;
            }
            // Anything else means this route is not usable here. Say so once,
            // by name, and finish the replacement the old way.
            posix_available = false;
            fallback_reason =
                (staged_open_failed
                     ? "could not open the staged file for a POSIX rename: "
                 : isUnsupportedInformationClass(posix_error)
                     ? "FileRenameInfoEx is unavailable on this Windows or "
                       "filesystem: "
                     : "POSIX rename failed: ") +
                systemErrorMessage(posix_error);
        }
        if (MoveFileExW(source.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING |
                            MOVEFILE_WRITE_THROUGH)) {
            noteLegacyFallback(destination, fallback_reason);
            return;
        }
        last_error = GetLastError();
        last_route = "MoveFileExW";
        if (!isTransientReplaceError(last_error)) break;
        // File watchers and virus scanners can briefly retain a sharing
        // handle after preflight. Rechecking the digest on every attempt
        // preserves the destination CAS while tolerating that transient.
        Sleep(2);
    }
    std::string detail = systemErrorMessage(last_error);
    if (!fallback_reason.empty()) {
        detail += " [after fallback: " + fallback_reason + "]";
    }
    throw std::runtime_error(
        "atomic render authoring replace failed for " +
        destination.string() + " via " + last_route + ": " + detail);
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw std::runtime_error("atomic render authoring replace failed: " +
                                 error.message());
    }
    // rename(2) already has the semantics the Windows route had to ask for.
    notePosixRename();
#endif
}

// Create-if-absent, not replace. create_hard_link is the only Win32 primitive
// that publishes a name and fails when it is already taken, in one operation:
// it is the exclusive-create half of this transaction, so a destination that
// appeared behind the editor's back is reported as an external modification
// instead of being overwritten. A rename cannot express that -- neither
// MoveFileExW without MOVEFILE_REPLACE_EXISTING (it is not atomic against a
// concurrent creator on every filesystem) nor a POSIX rename, which replaces
// by definition. Nothing is copied; only a directory entry appears.
//
// Because it never supersedes a live file, it also never has to ask the
// question the replace route keeps losing under a saturated scanner: there is
// no destination handle to break. That is why the transaction's own durable
// decision is published through this primitive and not through a rename.
void linkExclusive(const std::filesystem::path &source,
                   const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::create_hard_link(source, destination, error);
    if (error) {
        throw ProjectDocumentExternalModification{
            "render authoring create destination is no longer missing: " +
            destination.string()};
    }
}

// linkExclusive plus the staging name's removal, for a caller that has not yet
// made a durable decision and may still roll back: a staging file left behind
// here would be a leak, so the failure is reported.
void atomicCreate(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
    linkExclusive(source, destination);
    std::error_code error;
    std::filesystem::remove(source, error);
    if (error) {
        throw std::runtime_error(
            "could not remove linked render authoring staging file: " +
            error.message());
    }
}

std::filesystem::path weaklyCanonical(const std::filesystem::path &path,
                                      std::string_view context) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw std::runtime_error(std::string{context} + ": " +
                                 error.message());
    }
    return result;
}

bool isWithin(const std::filesystem::path &root,
              const std::filesystem::path &path) {
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *root_it != *path_it) return false;
    }
    return true;
}

std::string relativeString(const std::filesystem::path &root,
                           const std::filesystem::path &path) {
    const auto relative = path.lexically_relative(root).generic_u8string();
    return {reinterpret_cast<const char *>(relative.data()), relative.size()};
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    const auto *begin = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path{
        std::u8string{begin, begin + value.size()}};
}

std::string operationName(ProjectDocumentOperation operation) {
    switch (operation) {
    case ProjectDocumentOperation::create: return "create";
    case ProjectDocumentOperation::replace: return "replace";
    case ProjectDocumentOperation::erase: return "delete";
    }
    throw std::logic_error("unknown render config document operation");
}

ProjectDocumentOperation operationFromName(std::string_view value) {
    if (value == "create") return ProjectDocumentOperation::create;
    if (value == "replace") return ProjectDocumentOperation::replace;
    if (value == "delete") return ProjectDocumentOperation::erase;
    throw std::runtime_error("render authoring recovery manifest has unknown operation");
}

void requireExpectedState(const ProjectCandidateDocument &document) {
    std::error_code error;
    const bool exists = std::filesystem::exists(document.path, error);
    if (error) {
        throw std::runtime_error("could not inspect render authoring document: " +
                                 error.message());
    }
    if (document.expected.existence ==
        ProjectDocumentExistence::missing) {
        if (exists) {
            throw ProjectDocumentExternalModification{
                "render authoring document was created outside the editor: " +
                document.reference};
        }
        return;
    }
    if (!exists || !std::filesystem::is_regular_file(document.path, error) ||
        error) {
        throw ProjectDocumentExternalModification{
            "render authoring document was removed outside the editor: " +
            document.reference};
    }
    const auto digest = projectDocumentDigest(readBytes(document.path));
    if (digest != document.expected.digest) {
        throw ProjectDocumentExternalModification{
            "render authoring document changed outside the editor: " +
            document.reference};
    }
}

// The name of the transaction's durable decision. Its presence -- not a field
// inside manifest.json -- is what makes a commit committed.
constexpr const char *committedMarkerName = "committed";

std::filesystem::path committedMarkerPath(
    const std::filesystem::path &directory) {
    return directory / committedMarkerName;
}

bool hasCommittedMarker(const std::filesystem::path &directory) {
    std::error_code error;
    const bool present =
        std::filesystem::is_regular_file(committedMarkerPath(directory), error);
    return present && !error;
}

// manifest.json is written exactly once, by exclusive create, and is never
// superseded afterwards. Returns the digest of the bytes that were published,
// which is what the committed marker binds itself to.
std::string writeManifest(const std::filesystem::path &directory,
                          const Json &manifest) {
    const auto destination = directory / "manifest.json";
    const auto temporary = directory / "manifest.next";
    const auto bytes = manifest.dump(2) + "\n";
    writeBytes(temporary, bytes);
    atomicCreate(temporary, destination);
    return projectDocumentDigest(bytes);
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

// The durable decision, published as a name that did not exist before. It
// binds itself to the prepared manifest by digest, so a marker that belongs to
// a different manifest is a named rejection at recovery rather than a silent
// rollback of work that was already committed.
//
// The staging file's removal is deliberately best-effort here: once the link
// exists the transaction is committed, and no failure after that point may
// re-enter the prepared-state rollback path. The whole directory is removed
// next anyway, and recovery removes it if that did not happen.
void publishCommittedMarker(const std::filesystem::path &directory,
                            std::string_view manifest_digest) {
    const Json marker{
        {"schema", "pelican.render_authoring_transaction_committed"},
        {"version", 1},
        {"manifest_digest", std::string{manifest_digest}},
        {"committed_at", utcTimestamp()},
    };
    const auto temporary = directory / "committed.next";
    writeBytes(temporary, marker.dump(2) + "\n");
    linkExclusive(temporary, committedMarkerPath(directory));
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

enum class TransactionDecision { prepared, committed };

std::string utf8PathString(const std::filesystem::path &path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

// MG-2a: the previous format wrote its decision into manifest.json as
// `status: "committed"`. Such a directory is neither rolled back nor read as
// committed; both readers stop on it by name and leave it in place.
void rejectLegacyTransactionDirectory(const std::filesystem::path &directory,
                                      const Json &manifest) {
    if (manifest.is_object() &&
        manifest.value("status", std::string{}) == "committed") {
        throw RenderConfigLegacyTransactionDirectory{directory};
    }
}

// What a transaction directory decided, read from disk. The marker is the
// only record of a commit.
TransactionDecision transactionDecision(
    const std::filesystem::path &directory, const Json &manifest,
    std::string_view manifest_bytes) {
    rejectLegacyTransactionDirectory(directory, manifest);
    if (hasCommittedMarker(directory)) {
        const auto marker =
            Json::parse(readBytes(committedMarkerPath(directory)));
        const auto recorded =
            marker.value("manifest_digest", std::string{});
        const auto actual = projectDocumentDigest(manifest_bytes);
        if (recorded != actual) {
            throw ProjectDocumentExternalModification{
                "render_authoring_manifest_mismatch@1: the committed marker in " +
                directory.string() + " was written for manifest digest " +
                recorded + " but manifest.json now digests " + actual};
        }
        return TransactionDecision::committed;
    }
    const auto status = manifest.value("status", std::string{});
    if (status != "prepared") {
        throw std::runtime_error(
            "render authoring recovery manifest has unknown status");
    }
    return TransactionDecision::prepared;
}

struct RecoveryEntry {
    std::filesystem::path destination;
    ProjectDocumentOperation operation;
    std::string expected_digest;
    std::string next_digest;
    std::filesystem::path backup;
};

std::vector<RecoveryEntry> recoveryEntries(
    const std::filesystem::path &project_root,
    const std::filesystem::path &directory, const Json &manifest) {
    if (!manifest.is_object() || manifest.value("schema", std::string{}) !=
                                     "pelican.render_authoring_transaction" ||
        manifest.value("version", 0) != 1 ||
        !manifest.contains("documents") ||
        !manifest.at("documents").is_array()) {
        throw std::runtime_error(
            "render authoring recovery manifest is invalid");
    }
    std::vector<RecoveryEntry> result;
    for (const auto &item : manifest.at("documents")) {
        const auto relative =
            pathFromUtf8(item.at("destination").get<std::string>());
        const auto destination = weaklyCanonical(
            project_root / relative,
            "could not normalize recovery destination");
        if (!isWithin(project_root, destination)) {
            throw std::runtime_error(
                "render authoring recovery destination escapes the project root");
        }
        result.push_back(RecoveryEntry{
            .destination = destination,
            .operation = operationFromName(
                item.at("operation").get<std::string>()),
            .expected_digest =
                item.value("expected_digest", std::string{}),
            .next_digest = item.value("next_digest", std::string{}),
            .backup = item.contains("backup")
                          ? directory /
                                pathFromUtf8(item.at("backup").get<std::string>())
                          : std::filesystem::path{},
        });
    }
    return result;
}

// One of the two remaining replace routes, and the only one left that carries
// a transaction outcome. It cannot be an exclusive create: rollback puts an
// authored document that still exists back to its previous bytes, and there is
// no free name to take. What it restores is content, not the decision -- the
// decision was already made by the absence of the committed marker -- so an
// intermittent failure here is reported and retried by the next recovery, not
// silently reinterpreted.
void restoreBytes(const std::filesystem::path &directory,
                  const std::filesystem::path &destination,
                  std::string_view bytes, std::size_t index,
                  bool destination_exists) {
    const auto temporary = directory /
                           ("restore-" + std::to_string(index) + ".next");
    writeBytes(temporary, bytes);
    if (destination_exists) {
        atomicReplace(temporary, destination,
                      projectDocumentDigest(readBytes(destination)));
    } else {
        atomicCreate(temporary, destination);
    }
}

void rollbackPrepared(
    const std::filesystem::path &project_root,
    const std::filesystem::path &directory, const Json &manifest) {
    auto entries = recoveryEntries(project_root, directory, manifest);
    std::string conflict;
    for (std::size_t reverse = entries.size(); reverse > 0; --reverse) {
        const auto index = reverse - 1;
        const auto &entry = entries[index];
        std::error_code error;
        const bool exists = std::filesystem::exists(entry.destination, error);
        if (error) {
            if (conflict.empty()) conflict = error.message();
            continue;
        }
        const auto current_digest = exists
                                        ? projectDocumentDigest(
                                              readBytes(entry.destination))
                                        : std::string{};
        if (entry.operation == ProjectDocumentOperation::create) {
            if (!exists) continue;
            if (current_digest == entry.next_digest) {
                std::filesystem::remove(entry.destination, error);
                if (error && conflict.empty()) conflict = error.message();
            } else if (conflict.empty()) {
                conflict = "created render authoring document changed before rollback: " +
                           entry.destination.string();
            }
            continue;
        }

        if (exists && current_digest == entry.expected_digest) continue;
        const bool is_candidate_state =
            (entry.operation == ProjectDocumentOperation::replace &&
             exists && current_digest == entry.next_digest) ||
            (entry.operation == ProjectDocumentOperation::erase &&
             !exists);
        if (!is_candidate_state) {
            if (conflict.empty()) {
                conflict =
                    "render authoring document changed before rollback: " +
                    entry.destination.string();
            }
            continue;
        }
        if (entry.backup.empty() ||
            !std::filesystem::is_regular_file(entry.backup, error) || error) {
            if (conflict.empty()) {
                conflict = "render authoring rollback backup is missing: " +
                           entry.destination.string();
            }
            continue;
        }
        const auto backup = readBytes(entry.backup);
        restoreBytes(directory, entry.destination, backup, index, exists);
    }
    if (!conflict.empty()) {
        throw ProjectDocumentExternalModification{conflict};
    }
}

void removeCreatedDirectories(const std::filesystem::path &project_root,
                              const Json &manifest) noexcept {
    if (!manifest.contains("created_directories") ||
        !manifest.at("created_directories").is_array()) {
        return;
    }
    for (auto it = manifest.at("created_directories").rbegin();
         it != manifest.at("created_directories").rend(); ++it) {
        try {
            const auto path = weaklyCanonical(
                project_root / pathFromUtf8(it->get<std::string>()),
                "could not normalize created directory");
            if (isWithin(project_root, path)) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } catch (...) {
        }
    }
}

void removeTransactionDirectory(const std::filesystem::path &directory) {
    std::error_code error;
#ifdef _WIN32
    constexpr unsigned int retry_count = 5000;
#else
    constexpr unsigned int retry_count = 1;
#endif
    // Order matters, and this is the order. A removal that stops halfway must
    // not leave a prepared manifest standing without its committed marker:
    // recovery reads exactly that pair as a transaction to roll back, and
    // would undo work that is already durable. The directory is enumerated in
    // collation order, which puts `committed` before `manifest.json` -- the
    // wrong one first -- and manifest.json is the entry a scanner is most
    // likely to be holding, so that residue is the likely one, not a corner.
    // Drop manifest.json first and the worst residue means nothing at all.
    const auto manifest = directory / "manifest.json";
    for (unsigned int attempt = 0; attempt < retry_count; ++attempt) {
        error.clear();
        std::filesystem::remove(manifest, error);
        if (!error) {
            std::filesystem::remove_all(directory, error);
            if (!error) return;
        }
#ifdef _WIN32
        if (error.value() != ERROR_ACCESS_DENIED &&
            error.value() != ERROR_SHARING_VIOLATION &&
            error.value() != ERROR_LOCK_VIOLATION &&
            error.value() != ERROR_DIR_NOT_EMPTY) {
            break;
        }
        Sleep(2);
#endif
    }
    throw std::runtime_error(
        "could not remove render authoring transaction directory: " +
        error.message());
}

} // namespace

std::string projectDocumentDigest(std::string_view bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

void setProjectDocumentTransactionWarningSink(
    ProjectDocumentTransactionWarningSink sink) noexcept {
    warningSink().store(sink);
}

RenderConfigLegacyTransactionDirectory::RenderConfigLegacyTransactionDirectory(
    std::filesystem::path directory)
    : std::runtime_error{[&directory] {
          const auto path = utf8PathString(directory);
          return std::string{renderConfigLegacyTransactionDirCode} + ": " +
                 Json{{"path", path},
                      {"message",
                       "the render authoring transaction directory '" + path +
                           "' was written by an engine older than v0 and is "
                           "not read; its documents are already committed, so "
                           "delete this directory by hand and open the project "
                           "again"}}
                     .dump();
      }()},
      path_{std::move(directory)} {}

nlohmann::json RenderConfigLegacyTransactionDirectory::errorFields() const {
    return {{"path", utf8PathString(path_)}};
}

std::filesystem::path projectDocumentTransactionDirectory(
    const std::filesystem::path &project_root) {
    return project_root / ".pelican" / "render-authoring.transaction";
}

ProjectDocumentAtomicReplaceRoutes projectDocumentAtomicReplaceRoutesForTesting() {
    const std::lock_guard<std::mutex> guard{routeMutex()};
    return routeState();
}

void projectDocumentAtomicReplaceForTesting(
    const std::filesystem::path &source,
    const std::filesystem::path &destination) {
    atomicReplace(source, destination);
}

namespace {

// The commit itself. `take_over` is empty for a writer that takes the
// transaction directory here; for one that already holds it (a
// ProjectDocumentTransactionLock), it is called once, right before the first
// byte is written into that directory, and from then on the directory is this
// commit's to remove or to leave for recovery.
ProjectDocumentCommitReceipt commitDocuments(
    const std::filesystem::path &project_root,
    std::span<const ProjectCandidateDocument> documents,
    const ProjectDocumentCommitFault &fault,
    const std::function<void()> &take_over) {
    ProjectDocumentCommitReceipt receipt{.project_root = project_root};
    const bool directory_held = static_cast<bool>(take_over);

    // A prior committed transaction may have left only cleanup artifacts
    // because a scanner held the metadata directory. It is safe to verify and
    // clean that terminal state. A prepared/no-manifest directory may belong
    // to an active writer and remains an exclusive transaction lock.
    const auto existing_directory =
        projectDocumentTransactionDirectory(project_root);
    std::error_code existing_error;
    if (directory_held) {
        // The directory is the caller's own lock, and holds nothing yet.
    } else if (std::filesystem::exists(existing_directory, existing_error)) {
        if (existing_error) {
            throw std::runtime_error(existing_error.message());
        }
        const auto existing_manifest =
            existing_directory / "manifest.json";
        const bool has_manifest = std::filesystem::is_regular_file(
                                      existing_manifest, existing_error) &&
                                  !existing_error;
        // The marker is the decision. A directory in the previous format stops
        // here by name before anything else reads it. Recovery, not this
        // guard, verifies the marker's digest, so a mismatch surfaces here by
        // its name rather than as "another transaction is active".
        if (has_manifest) {
            rejectLegacyTransactionDirectory(
                existing_directory,
                Json::parse(readBytes(existing_manifest)));
        }
        if (has_manifest && hasCommittedMarker(existing_directory)) {
            recoverProjectDocumentTransaction(project_root);
        } else {
            throw std::runtime_error(
                "another render authoring transaction or recovery is active");
        }
    } else if (existing_error) {
        throw std::runtime_error(existing_error.message());
    }

    struct Changed {
        const ProjectCandidateDocument *document = nullptr;
        std::optional<std::string> before;
        std::string next_digest;
    };
    std::vector<Changed> changed;
    for (const auto &document : documents) {
        const auto destination = weaklyCanonical(
            document.path, "could not normalize render authoring destination");
        if (!isWithin(project_root, destination)) {
            throw std::runtime_error(
                "render authoring transaction destination is outside the project root: " +
                document.path.string());
        }
        requireExpectedState(document);
        std::optional<std::string> before;
        if (document.expected.existence ==
            ProjectDocumentExistence::present) {
            before = readBytes(document.path);
        }
        const bool same =
            document.operation == ProjectDocumentOperation::replace &&
            before && *before == document.bytes;
        if (!same) {
            changed.push_back(Changed{
                .document = &document,
                .before = std::move(before),
                .next_digest =
                    document.operation == ProjectDocumentOperation::erase
                        ? std::string{}
                        : projectDocumentDigest(document.bytes),
            });
        }
    }
    if (changed.empty()) return receipt;

    const auto directory = projectDocumentTransactionDirectory(project_root);
    std::error_code error;
    if (directory_held) {
        take_over();
    } else {
        std::filesystem::create_directories(directory.parent_path(), error);
        if (error) {
            throw std::runtime_error(
                "could not create render authoring metadata directory: " +
                error.message());
        }
        if (!std::filesystem::create_directory(directory, error) || error) {
            throw std::runtime_error(
                "another render authoring transaction or recovery is active");
        }
    }

    Json manifest{
        {"schema", "pelican.render_authoring_transaction"},
        {"version", 1},
        {"status", "prepared"},
        {"documents", Json::array()},
        {"created_directories", Json::array()},
    };
    try {
        std::vector<std::filesystem::path> missing_directories;
        for (std::size_t index = 0; index < changed.size(); ++index) {
            const auto &item = changed[index];
            const auto &document = *item.document;
            Json encoded{
                {"destination", relativeString(project_root, document.path)},
                {"operation", operationName(document.operation)},
                {"expected_digest", document.expected.digest},
                {"next_digest", item.next_digest},
            };
            if (item.before) {
                const auto backup_name =
                    std::to_string(index) + ".before";
                writeBytes(directory / backup_name, *item.before);
                encoded["backup"] = backup_name;
            }
            if (document.operation != ProjectDocumentOperation::erase) {
                const auto stage_name = std::to_string(index) + ".next";
                writeBytes(directory / stage_name, document.bytes);
                encoded["stage"] = stage_name;
            }
            manifest["documents"].push_back(std::move(encoded));

            auto parent = document.path.parent_path();
            std::vector<std::filesystem::path> chain;
            while (isWithin(project_root, weaklyCanonical(
                       parent, "could not normalize render authoring parent")) &&
                   parent != project_root &&
                   !std::filesystem::exists(parent, error)) {
                if (error) throw std::runtime_error(error.message());
                chain.push_back(parent);
                parent = parent.parent_path();
            }
            std::ranges::reverse(chain);
            for (const auto &missing : chain) {
                if (std::find(missing_directories.begin(),
                              missing_directories.end(), missing) ==
                    missing_directories.end()) {
                    missing_directories.push_back(missing);
                    manifest["created_directories"].push_back(
                        relativeString(project_root, missing));
                }
            }
        }

        // Recheck every expected document after staging, including unchanged
        // overlay documents, before the first destination mutation.
        for (const auto &document : documents) {
            requireExpectedState(document);
        }
        const auto manifest_digest = writeManifest(directory, manifest);

        for (const auto &missing : missing_directories) {
            if (!std::filesystem::create_directory(missing, error) && error) {
                throw std::runtime_error(
                    "could not create managed render fragment directory: " +
                    error.message());
            }
        }

        // Authored documents, not transaction metadata. A `replace` operation
        // is a replacement by definition -- the user's file exists and has to
        // end up holding other bytes -- so this is the one place in the commit
        // path that still supersedes a live file, and a failure here is still
        // a rollback because the decision has not been made yet.
        for (std::size_t index = 0; index < changed.size(); ++index) {
            const auto &document = *changed[index].document;
            requireExpectedState(document);
            const auto &encoded = manifest["documents"].at(index);
            if (document.operation == ProjectDocumentOperation::create) {
                atomicCreate(directory /
                                 pathFromUtf8(encoded.at("stage").get<std::string>()),
                             document.path);
            } else if (document.operation ==
                       ProjectDocumentOperation::replace) {
                atomicReplace(directory /
                                  pathFromUtf8(encoded.at("stage").get<std::string>()),
                              document.path,
                              document.expected.digest);
            } else {
                const auto deleted = directory /
                                     (std::to_string(index) + ".deleted");
                std::filesystem::rename(document.path, deleted, error);
                if (error) {
                    throw std::runtime_error(
                        "could not stage render fragment deletion: " +
                        error.message());
                }
            }
            if (fault) fault(index, document);
        }

        for (const auto &item : changed) {
            receipt.entries.push_back(ProjectDocumentCommitReceiptEntry{
                .path = item.document->path,
                .before_bytes = item.before,
                .after_bytes =
                    item.document->operation ==
                            ProjectDocumentOperation::erase
                        ? std::optional<std::string>{}
                        : std::optional<std::string>{item.document->bytes},
            });
        }
        // Complete every potentially-throwing receipt allocation before the
        // durable decision. Once this marker is visible, no failure may enter
        // the prepared-state rollback path. The marker takes a name that did
        // not exist; manifest.json stays exactly as it was published, so the
        // decision never has to supersede a file a scanner holds open.
        publishCommittedMarker(directory, manifest_digest);

        // The terminal step of the fault seam: a fixture that stops the
        // process here leaves the one on-disk state no document loop can
        // produce -- committed, not yet cleaned up. A throw from it would
        // re-enter rollback and invert a durable decision, so it cannot.
        if (fault) {
            try {
                fault(changed.size(), *changed.back().document);
            } catch (...) {
            }
        }

        // The committed marker is the durable decision. Failure to remove
        // metadata cannot turn it back into a rollback; startup/the next edit
        // will verify and clean the committed transaction.
        try {
            removeTransactionDirectory(directory);
        } catch (...) {
        }
        return receipt;
    } catch (...) {
        const auto original = std::current_exception();
        try {
            if (std::filesystem::exists(directory / "manifest.json")) {
                const auto recorded = Json::parse(
                    readBytes(directory / "manifest.json"));
                rollbackPrepared(project_root, directory, recorded);
                removeCreatedDirectories(project_root, recorded);
            }
            removeTransactionDirectory(directory);
        } catch (const std::exception &rollback_error) {
            throw std::runtime_error(
                std::string{"render authoring transaction rollback failed: "} +
                rollback_error.what());
        }
        std::rethrow_exception(original);
    }
}

// An empty directory this process created, removed again. A scanner that
// looked in may hold it for a moment; anything inside it means it is no
// longer only a lock, and it stays.
void removeEmptyLockDirectory(const std::filesystem::path &directory) noexcept {
#ifdef _WIN32
    constexpr unsigned int retry_count = 500;
#else
    constexpr unsigned int retry_count = 1;
#endif
    for (unsigned int attempt = 0; attempt < retry_count; ++attempt) {
        std::error_code error;
        std::filesystem::remove(directory, error);
        if (!error) return;
#ifdef _WIN32
        if (error.value() != ERROR_ACCESS_DENIED &&
            error.value() != ERROR_SHARING_VIOLATION &&
            error.value() != ERROR_LOCK_VIOLATION) {
            return;
        }
        Sleep(2);
#endif
    }
}

} // namespace

ProjectDocumentCommitReceipt commitProjectDocuments(
    const std::filesystem::path &project_root,
    std::span<const ProjectCandidateDocument> documents,
    const ProjectDocumentCommitFault &fault) {
    return commitDocuments(
        weaklyCanonical(project_root,
                        "could not normalize render authoring project root"),
        documents, fault, {});
}

ProjectDocumentCommitReceipt commitProjectDocuments(
    ProjectDocumentTransactionLock &lock,
    std::span<const ProjectCandidateDocument> documents,
    const ProjectDocumentCommitFault &fault) {
    if (!lock.held_) {
        throw std::logic_error(
            "commitProjectDocuments was given a transaction lock that no longer "
            "holds its directory");
    }
    return commitDocuments(lock.project_root_, documents, fault,
                           [&lock] { lock.held_ = false; });
}

ProjectDocumentTransactionBusy::ProjectDocumentTransactionBusy(
    std::filesystem::path directory)
    : std::runtime_error{"the project document transaction directory is in use: " +
                         utf8PathString(directory)},
      path_{std::move(directory)} {}

ProjectDocumentTransactionLock::ProjectDocumentTransactionLock(
    std::filesystem::path project_root, std::filesystem::path directory,
    bool created_metadata_directory) noexcept
    : project_root_{std::move(project_root)}, directory_{std::move(directory)},
      held_{true}, created_metadata_directory_{created_metadata_directory} {}

ProjectDocumentTransactionLock::ProjectDocumentTransactionLock(
    ProjectDocumentTransactionLock &&other) noexcept
    : project_root_{std::move(other.project_root_)},
      directory_{std::move(other.directory_)}, held_{other.held_},
      created_metadata_directory_{other.created_metadata_directory_} {
    other.held_ = false;
}

ProjectDocumentTransactionLock::~ProjectDocumentTransactionLock() { release(); }

ProjectDocumentTransactionLock ProjectDocumentTransactionLock::acquire(
    const std::filesystem::path &project_root_input) {
    auto project_root = weaklyCanonical(
        project_root_input, "could not normalize render authoring project root");
    auto directory = projectDocumentTransactionDirectory(project_root);
    const auto metadata = directory.parent_path();
    std::error_code error;
    const bool created_metadata =
        std::filesystem::create_directory(metadata, error);
    if (error) {
        throw std::runtime_error(
            "could not create render authoring metadata directory: " +
            error.message());
    }
    // The one operation that decides who holds the directory: it fails when
    // the name is taken, whoever took it and whatever it holds.
    if (std::filesystem::create_directory(directory, error) && !error) {
        return ProjectDocumentTransactionLock{std::move(project_root),
                                              std::move(directory),
                                              created_metadata};
    }
    std::error_code exists_error;
    const bool taken = std::filesystem::exists(directory, exists_error);
    if (created_metadata) {
        std::error_code ignored;
        std::filesystem::remove(metadata, ignored);
    }
    if (taken) throw ProjectDocumentTransactionBusy{std::move(directory)};
    throw std::runtime_error(
        "could not create the project document transaction directory " +
        utf8PathString(directory) + ": " + error.message());
}

void ProjectDocumentTransactionLock::release() noexcept {
    if (!held_) return;
    held_ = false;
    removeEmptyLockDirectory(directory_);
    if (created_metadata_directory_) {
        // Only while empty: a backup or another writer's directory keeps it.
        std::error_code ignored;
        std::filesystem::remove(directory_.parent_path(), ignored);
    }
}

void rollbackCommittedProjectDocuments(
    ProjectDocumentCommitReceipt receipt) {
    const auto directory = projectDocumentTransactionDirectory(
        weaklyCanonical(receipt.project_root,
                        "could not normalize rollback project root"));
    std::error_code error;
    std::filesystem::create_directories(directory.parent_path(), error);
    if (error || !std::filesystem::create_directory(directory, error)) {
        throw std::runtime_error(
            "could not create defensive render authoring rollback directory");
    }
    try {
        for (std::size_t reverse = receipt.entries.size(); reverse > 0;
             --reverse) {
            const auto index = reverse - 1;
            const auto &entry = receipt.entries[index];
            const bool exists = std::filesystem::exists(entry.path, error);
            if (error) throw std::runtime_error(error.message());
            if (entry.after_bytes) {
                if (!exists || projectDocumentDigest(readBytes(entry.path)) !=
                                   projectDocumentDigest(*entry.after_bytes)) {
                    throw ProjectDocumentExternalModification{
                        "render authoring document changed before defensive rollback: " +
                        entry.path.string()};
                }
            } else if (exists) {
                throw ProjectDocumentExternalModification{
                    "deleted render authoring document was recreated before defensive rollback: " +
                    entry.path.string()};
            }

            if (!entry.before_bytes) {
                if (exists && !std::filesystem::remove(entry.path, error)) {
                    throw std::runtime_error(error.message());
                }
            } else {
                restoreBytes(directory, entry.path, *entry.before_bytes, index,
                             exists);
            }
        }
        removeTransactionDirectory(directory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        throw;
    }
}

void recoverProjectDocumentTransaction(
    const std::filesystem::path &project_root_input) {
    const auto project_root = weaklyCanonical(
        project_root_input, "could not normalize recovery project root");
    const auto directory = projectDocumentTransactionDirectory(project_root);
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) throw std::runtime_error(error.message());
        return;
    }
    const auto manifest_path = directory / "manifest.json";
    if (!std::filesystem::is_regular_file(manifest_path, error) || error) {
        // Either destination mutation never began -- it starts only after
        // manifest.json exists -- or a finished transaction's cleanup got as
        // far as removing it, which it does first. Both leave nothing to
        // decide and nothing to undo; a marker left next to it says only that
        // the decision was made, which is already reflected on disk.
        removeTransactionDirectory(directory);
        return;
    }
    const auto manifest_bytes = readBytes(manifest_path);
    const auto manifest = Json::parse(manifest_bytes);
    if (transactionDecision(directory, manifest, manifest_bytes) ==
        TransactionDecision::prepared) {
        rollbackPrepared(project_root, directory, manifest);
        removeCreatedDirectories(project_root, manifest);
    } else {
        for (const auto &entry : recoveryEntries(project_root, directory,
                                                  manifest)) {
            const bool exists = std::filesystem::exists(entry.destination, error);
            if (error) throw std::runtime_error(error.message());
            if (entry.operation == ProjectDocumentOperation::erase) {
                if (exists) {
                    throw ProjectDocumentExternalModification{
                        "committed render fragment deletion was not durable: " +
                        entry.destination.string()};
                }
            } else if (!exists ||
                       projectDocumentDigest(readBytes(entry.destination)) !=
                           entry.next_digest) {
                throw ProjectDocumentExternalModification{
                    "committed render authoring document changed before recovery: " +
                    entry.destination.string()};
            }
        }
    }
    removeTransactionDirectory(directory);
}

} // namespace Pelican
