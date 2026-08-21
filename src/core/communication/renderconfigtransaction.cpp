#include "renderconfigtransaction.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
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

void atomicReplace(const std::filesystem::path &source,
                   const std::filesystem::path &destination,
                   std::string_view expected_digest = {}) {
#ifdef _WIN32
    // A heavily parallel ctest run can make Windows Defender retain a newly
    // staged JSON file for longer than two seconds. Keep rechecking the CAS on
    // every attempt, but allow that bounded sharing lock up to ten seconds.
    constexpr unsigned int retry_count = 5000;
    DWORD last_error = ERROR_SUCCESS;
    for (unsigned int attempt = 0; attempt < retry_count; ++attempt) {
        if (!expected_digest.empty()) {
            std::error_code state_error;
            if (!std::filesystem::is_regular_file(destination,
                                                  state_error) ||
                state_error ||
                renderConfigSourceDigest(readBytes(destination)) !=
                    expected_digest) {
                throw RenderConfigExternalModification{
                    "render authoring document changed while an atomic replacement was retried: " +
                    destination.string()};
            }
        }
        if (MoveFileExW(source.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING |
                            MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        last_error = GetLastError();
        if (last_error != ERROR_ACCESS_DENIED &&
            last_error != ERROR_SHARING_VIOLATION &&
            last_error != ERROR_LOCK_VIOLATION) {
            break;
        }
        // File watchers and virus scanners can briefly retain a sharing
        // handle after preflight. Rechecking the digest on every attempt
        // preserves the destination CAS while tolerating that transient.
        Sleep(2);
    }
    const std::error_code error{static_cast<int>(last_error),
                                std::system_category()};
    throw std::runtime_error(
        "atomic render authoring replace failed for " +
        destination.string() + ": " + error.message());
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw std::runtime_error("atomic render authoring replace failed: " +
                                 error.message());
    }
#endif
}

void atomicCreate(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::create_hard_link(source, destination, error);
    if (error) {
        throw RenderConfigExternalModification{
            "render authoring create destination is no longer missing: " +
            destination.string()};
    }
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

std::string operationName(RenderConfigDocumentOperation operation) {
    switch (operation) {
    case RenderConfigDocumentOperation::create: return "create";
    case RenderConfigDocumentOperation::replace: return "replace";
    case RenderConfigDocumentOperation::erase: return "delete";
    }
    throw std::logic_error("unknown render config document operation");
}

RenderConfigDocumentOperation operationFromName(std::string_view value) {
    if (value == "create") return RenderConfigDocumentOperation::create;
    if (value == "replace") return RenderConfigDocumentOperation::replace;
    if (value == "delete") return RenderConfigDocumentOperation::erase;
    throw std::runtime_error("render authoring recovery manifest has unknown operation");
}

void requireExpectedState(const RenderConfigCandidateDocument &document) {
    std::error_code error;
    const bool exists = std::filesystem::exists(document.path, error);
    if (error) {
        throw std::runtime_error("could not inspect render authoring document: " +
                                 error.message());
    }
    if (document.expected.existence ==
        RenderConfigDocumentExistence::missing) {
        if (exists) {
            throw RenderConfigExternalModification{
                "render authoring document was created outside the editor: " +
                document.reference};
        }
        return;
    }
    if (!exists || !std::filesystem::is_regular_file(document.path, error) ||
        error) {
        throw RenderConfigExternalModification{
            "render authoring document was removed outside the editor: " +
            document.reference};
    }
    const auto digest = renderConfigSourceDigest(readBytes(document.path));
    if (digest != document.expected.digest) {
        throw RenderConfigExternalModification{
            "render authoring document changed outside the editor: " +
            document.reference};
    }
}

void writeManifest(const std::filesystem::path &directory, const Json &manifest,
                   bool replace_existing) {
    const auto destination = directory / "manifest.json";
    const auto temporary = directory / "manifest.next";
    writeBytes(temporary, manifest.dump(2) + "\n");
    if (replace_existing) {
        atomicReplace(temporary, destination);
    } else {
        atomicCreate(temporary, destination);
    }
}

struct RecoveryEntry {
    std::filesystem::path destination;
    RenderConfigDocumentOperation operation;
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

void restoreBytes(const std::filesystem::path &directory,
                  const std::filesystem::path &destination,
                  std::string_view bytes, std::size_t index,
                  bool destination_exists) {
    const auto temporary = directory /
                           ("restore-" + std::to_string(index) + ".next");
    writeBytes(temporary, bytes);
    if (destination_exists) {
        atomicReplace(temporary, destination,
                      renderConfigSourceDigest(readBytes(destination)));
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
                                        ? renderConfigSourceDigest(
                                              readBytes(entry.destination))
                                        : std::string{};
        if (entry.operation == RenderConfigDocumentOperation::create) {
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
            (entry.operation == RenderConfigDocumentOperation::replace &&
             exists && current_digest == entry.next_digest) ||
            (entry.operation == RenderConfigDocumentOperation::erase &&
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
        throw RenderConfigExternalModification{conflict};
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
    for (unsigned int attempt = 0; attempt < retry_count; ++attempt) {
        error.clear();
        std::filesystem::remove_all(directory, error);
        if (!error) return;
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

std::filesystem::path renderConfigTransactionDirectory(
    const std::filesystem::path &project_root) {
    return project_root / ".pelican" / "render-authoring.transaction";
}

RenderConfigDocumentCommitReceipt commitRenderConfigCandidateDocuments(
    const std::filesystem::path &project_root_input,
    const RenderConfigCandidateDocumentSet &documents,
    const RenderConfigDocumentCommitFault &fault) {
    const auto project_root = weaklyCanonical(
        project_root_input, "could not normalize render authoring project root");
    RenderConfigDocumentCommitReceipt receipt{.project_root = project_root};

    // A prior committed transaction may have left only cleanup artifacts
    // because a scanner held the metadata directory. It is safe to verify and
    // clean that terminal state. A prepared/no-manifest directory may belong
    // to an active writer and remains an exclusive transaction lock.
    const auto existing_directory =
        renderConfigTransactionDirectory(project_root);
    std::error_code existing_error;
    if (std::filesystem::exists(existing_directory, existing_error)) {
        if (existing_error) {
            throw std::runtime_error(existing_error.message());
        }
        const auto existing_manifest =
            existing_directory / "manifest.json";
        if (std::filesystem::is_regular_file(existing_manifest,
                                             existing_error) &&
            !existing_error &&
            Json::parse(readBytes(existing_manifest))
                    .value("status", std::string{}) == "committed") {
            recoverRenderConfigDocumentTransaction(project_root);
        } else {
            throw std::runtime_error(
                "another render authoring transaction or recovery is active");
        }
    } else if (existing_error) {
        throw std::runtime_error(existing_error.message());
    }

    struct Changed {
        const RenderConfigCandidateDocument *document = nullptr;
        std::optional<std::string> before;
        std::string next_digest;
    };
    std::vector<Changed> changed;
    for (const auto &document : documents.documents()) {
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
            RenderConfigDocumentExistence::present) {
            before = readBytes(document.path);
        }
        const bool same =
            document.operation == RenderConfigDocumentOperation::replace &&
            before && *before == document.bytes;
        if (!same) {
            changed.push_back(Changed{
                .document = &document,
                .before = std::move(before),
                .next_digest =
                    document.operation == RenderConfigDocumentOperation::erase
                        ? std::string{}
                        : renderConfigSourceDigest(document.bytes),
            });
        }
    }
    if (changed.empty()) return receipt;

    const auto directory = renderConfigTransactionDirectory(project_root);
    std::error_code error;
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
            if (document.operation != RenderConfigDocumentOperation::erase) {
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
        for (const auto &document : documents.documents()) {
            requireExpectedState(document);
        }
        writeManifest(directory, manifest, false);

        for (const auto &missing : missing_directories) {
            if (!std::filesystem::create_directory(missing, error) && error) {
                throw std::runtime_error(
                    "could not create managed render fragment directory: " +
                    error.message());
            }
        }

        for (std::size_t index = 0; index < changed.size(); ++index) {
            const auto &document = *changed[index].document;
            requireExpectedState(document);
            const auto &encoded = manifest["documents"].at(index);
            if (document.operation == RenderConfigDocumentOperation::create) {
                atomicCreate(directory /
                                 pathFromUtf8(encoded.at("stage").get<std::string>()),
                             document.path);
            } else if (document.operation ==
                       RenderConfigDocumentOperation::replace) {
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
            receipt.entries.push_back(RenderConfigDocumentCommitReceiptEntry{
                .path = item.document->path,
                .before_bytes = item.before,
                .after_bytes =
                    item.document->operation ==
                            RenderConfigDocumentOperation::erase
                        ? std::optional<std::string>{}
                        : std::optional<std::string>{item.document->bytes},
            });
        }
        // Complete every potentially-throwing receipt allocation before the
        // durable decision. Once this marker is visible, no failure may enter
        // the prepared-state rollback path.
        manifest["status"] = "committed";
        writeManifest(directory, manifest, true);

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

void rollbackCommittedRenderConfigDocuments(
    RenderConfigDocumentCommitReceipt receipt) {
    const auto directory = renderConfigTransactionDirectory(
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
                if (!exists || renderConfigSourceDigest(readBytes(entry.path)) !=
                                   renderConfigSourceDigest(*entry.after_bytes)) {
                    throw RenderConfigExternalModification{
                        "render authoring document changed before defensive rollback: " +
                        entry.path.string()};
                }
            } else if (exists) {
                throw RenderConfigExternalModification{
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

void recoverRenderConfigDocumentTransaction(
    const std::filesystem::path &project_root_input) {
    const auto project_root = weaklyCanonical(
        project_root_input, "could not normalize recovery project root");
    const auto directory = renderConfigTransactionDirectory(project_root);
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) throw std::runtime_error(error.message());
        return;
    }
    const auto manifest_path = directory / "manifest.json";
    if (!std::filesystem::is_regular_file(manifest_path, error) || error) {
        // Destination mutation begins only after manifest.json exists.
        removeTransactionDirectory(directory);
        return;
    }
    const auto manifest = Json::parse(readBytes(manifest_path));
    const auto status = manifest.value("status", std::string{});
    if (status == "prepared") {
        rollbackPrepared(project_root, directory, manifest);
        removeCreatedDirectories(project_root, manifest);
    } else if (status == "committed") {
        for (const auto &entry : recoveryEntries(project_root, directory,
                                                  manifest)) {
            const bool exists = std::filesystem::exists(entry.destination, error);
            if (error) throw std::runtime_error(error.message());
            if (entry.operation == RenderConfigDocumentOperation::erase) {
                if (exists) {
                    throw RenderConfigExternalModification{
                        "committed render fragment deletion was not durable: " +
                        entry.destination.string()};
                }
            } else if (!exists ||
                       renderConfigSourceDigest(readBytes(entry.destination)) !=
                           entry.next_digest) {
                throw RenderConfigExternalModification{
                    "committed render authoring document changed before recovery: " +
                    entry.destination.string()};
            }
        }
    } else {
        throw std::runtime_error(
            "render authoring recovery manifest has unknown status");
    }
    removeTransactionDirectory(directory);
}

} // namespace Pelican
