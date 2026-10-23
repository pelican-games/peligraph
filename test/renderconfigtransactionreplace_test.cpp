// The render authoring transaction's destination replacement, measured
// against the thing that made it fail intermittently on Windows: another
// process holding the destination open.
//
// A real-time scanner opens the file it is inspecting with FILE_SHARE_READ |
// FILE_SHARE_WRITE | FILE_SHARE_DELETE -- it shares everything, precisely so
// it does not break the application that owns the file. The classic
// MoveFileExW(MOVEFILE_REPLACE_EXISTING) still refuses to supersede a target
// that has any open handle and answers ERROR_ACCESS_DENIED for as long as that
// handle lives, which is the "Access is denied" the GPU gate kept hitting on
// .pelican/render-authoring.transaction/manifest.json. The holder in these
// tests is that scanner, and each test carries its own control showing the old
// primitive failing in the same conditions the new one survives.
#include "../src/core/communication/renderconfigtransaction.hpp"
#include "../src/project/renderconfigdocument.hpp"
#include "../src/core/communication/materialdocumentrpc.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("failed to open test file: " + path.string());
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(static_cast<bool>(output));
}

struct TemporaryDirectory {
    std::filesystem::path root;

    explicit TemporaryDirectory(std::string_view name) {
        root = std::filesystem::temp_directory_path() /
               ("pelican-" + std::string{name} + "-" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
};

// A prepared and a committed manifest of deliberately different lengths, so a
// reader that caught a half-published destination could not mistake the tail
// of the longer one for a complete document.
const std::string preparedManifest =
    Json{{"schema", "pelican.render_authoring_transaction"},
         {"version", 1},
         {"status", "prepared"},
         {"documents", Json::array()}}
        .dump(2) +
    "\n";
const std::string committedManifest =
    Json{{"schema", "pelican.render_authoring_transaction"},
         {"version", 1},
         {"status", "committed"},
         {"documents",
          Json::array({Json{{"destination", "passes/main.json"},
                            {"operation", "replace"},
                            {"expected_digest", std::string(64, 'a')},
                            {"next_digest", std::string(64, 'b')},
                            {"backup", "0.before"}}})}}
        .dump(2) +
    "\n";

// Both replace routes together. The transaction takes this route for authored
// documents only; nothing it writes about itself may add to this number.
long long replaceRouteTotal() {
    const auto routes = renderConfigAtomicReplaceRoutesForTesting();
    return static_cast<long long>(routes.posix_renames) +
           static_cast<long long>(routes.legacy_fallbacks);
}

// The fixture the crash tests commit, shared by the parent and by the child
// process that dies in the middle of it. Two documents in one transaction: a
// replace of an existing root and a create whose parent directory does not
// exist yet, so a rollback has both a backup to restore and a directory to
// remove.
const std::string crashOriginalRoot =
    Json{{"schema", "pelican.render_pipeline"}, {"version", 1},
         {"features", Json::array()}}
        .dump(2) +
    "\n";
const std::string crashCandidateRoot =
    Json{{"schema", "pelican.render_pipeline"}, {"version", 1},
         {"features", Json::array({"project://passes/authoring/pass-crash.json"})}}
        .dump(2) +
    "\n";
const std::string crashFragment =
    Json{{"schema", "pelican.render_feature"}, {"version", 1},
         {"name", "crash"}, {"passes", Json::array()}}
        .dump(2) +
    "\n";

std::filesystem::path crashRootDocument(const std::filesystem::path &root) {
    return root / "passes" / "main.json";
}
std::filesystem::path crashFragmentDocument(const std::filesystem::path &root) {
    return root / "passes" / "authoring" / "pass-crash.json";
}

void seedCrashProject(const std::filesystem::path &root) {
    std::filesystem::create_directories(root / "passes");
    writeBytes(crashRootDocument(root), crashOriginalRoot);
}

RenderConfigCandidateDocumentSet crashCandidateSet(
    const std::filesystem::path &root) {
    const std::string root_reference = "project://passes/main.json";
    const std::string fragment_reference =
        "project://passes/authoring/pass-crash.json";
    return RenderConfigCandidateDocumentSet{
        root_reference,
        {{.reference = root_reference,
          .normalized_reference = root_reference,
          .path = crashRootDocument(root),
          .operation = RenderConfigDocumentOperation::replace,
          .expected = {.existence = RenderConfigDocumentExistence::present,
                       .digest = renderConfigSourceDigest(crashOriginalRoot)},
          .bytes = crashCandidateRoot},
         {.reference = fragment_reference,
          .normalized_reference = fragment_reference,
          .path = crashFragmentDocument(root),
          .operation = RenderConfigDocumentOperation::create,
          .expected = {.existence = RenderConfigDocumentExistence::missing},
          .bytes = crashFragment}}};
}

constexpr int crashExitCode = 42;

std::string childEnvironment(const char *name) {
#ifdef _WIN32
    char *buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    const std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

#ifdef _WIN32
// Opens and closes the destination the way a real-time scanner does: read
// access, every kind of sharing granted. This is the handle MoveFileExW cannot
// rename over and FileRenameInfoEx can.
class ScannerHolder {
    std::filesystem::path path_;
    std::atomic<bool> stop_{false};
    std::atomic<long long> opens_{0};
    std::atomic<long long> torn_{0};
    std::atomic<long long> matched_{0};
    std::vector<std::string> expected_;
    std::thread worker_;

  public:
    ScannerHolder(std::filesystem::path path, std::vector<std::string> expected)
        : path_{std::move(path)}, expected_{std::move(expected)} {
        worker_ = std::thread{[this] { run(); }};
    }
    ~ScannerHolder() { stop(); }

    void stop() {
        if (!worker_.joinable()) return;
        stop_.store(true);
        worker_.join();
    }

    long long opens() const { return opens_.load(); }
    long long torn() const { return torn_.load(); }
    long long matched() const { return matched_.load(); }

    // Block until the scanner has actually taken the handle, so the
    // measurement below is made under contention rather than before it.
    bool waitForContention() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (opens_.load() > 8) return true;
            std::this_thread::yield();
        }
        return false;
    }

  private:
    void run() {
        while (!stop_.load()) {
            const HANDLE handle = CreateFileW(
                path_.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            opens_.fetch_add(1);
            std::string bytes;
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) &&
                   read > 0) {
                bytes.append(buffer, read);
            }
            CloseHandle(handle);
            if (expected_.empty()) continue;
            const bool whole =
                std::find(expected_.begin(), expected_.end(), bytes) !=
                expected_.end();
            if (whole) {
                matched_.fetch_add(1);
            } else {
                torn_.fetch_add(1);
            }
        }
    }
};

// The primitive this change replaces, called directly, so every test that
// claims the new route survives contention also shows the old one failing in
// exactly the same conditions.
DWORD legacyReplaceError(const std::filesystem::path &source,
                         const std::filesystem::path &destination) {
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return ERROR_SUCCESS;
    }
    return GetLastError();
}

// Runs the commit in a second copy of this test binary and stops that process
// dead at `stop`. Nothing unwinds, nothing rolls back, nothing is cleaned up:
// what the parent then finds on disk is what a crashed editor leaves behind,
// produced by the production commit path rather than assembled by hand.
void runCrashChild(const std::filesystem::path &project_root,
                   std::size_t stop) {
    wchar_t executable[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
    REQUIRE(SetEnvironmentVariableW(L"PELICAN_TRANSACTION_CRASH_ROOT",
                                    project_root.c_str()) != FALSE);
    REQUIRE(SetEnvironmentVariableW(L"PELICAN_TRANSACTION_CRASH_STEP",
                                    std::to_wstring(stop).c_str()) != FALSE);
    std::wstring command = L"\"" + std::wstring{executable} +
                           L"\" \"render authoring transaction commit stops "
                           L"this process on demand\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created =
        CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    SetEnvironmentVariableW(L"PELICAN_TRANSACTION_CRASH_ROOT", nullptr);
    SetEnvironmentVariableW(L"PELICAN_TRANSACTION_CRASH_STEP", nullptr);
    INFO("CreateProcessW error: " << create_error);
    REQUIRE(created != FALSE);
    const DWORD waited = WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 0;
    const BOOL read_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REQUIRE(waited == WAIT_OBJECT_0);
    REQUIRE(read_code != FALSE);
    // Any other code means the child finished or failed instead of being
    // stopped at the step under test, and the state below would be a fiction.
    INFO("crash child exit code: " << code);
    REQUIRE(code == static_cast<DWORD>(crashExitCode));
}
#endif

} // namespace

#ifdef _WIN32
TEST_CASE(
    "render authoring transaction replaces a manifest a real-time scanner holds open",
    "[render-config-transaction][atomic-replace][windows]") {
    TemporaryDirectory directory{"authoring-replace"};
    const auto destination = directory.root / "manifest.json";
    const auto staging = directory.root / "manifest.next";
    writeBytes(destination, preparedManifest);

    ScannerHolder scanner{destination, {}};
    REQUIRE(scanner.waitForContention());

    // Control first, and in the same conditions: the primitive that shipped
    // before this change cannot rename over the scanner's handle. If this
    // stops failing, the reproduction has gone stale and the measurement
    // below no longer means anything.
    long long legacy_failures = 0;
    long long legacy_access_denied = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        writeBytes(staging, committedManifest);
        const DWORD error = legacyReplaceError(staging, destination);
        if (error != ERROR_SUCCESS) {
            ++legacy_failures;
            if (error == ERROR_ACCESS_DENIED) ++legacy_access_denied;
            std::filesystem::remove(staging);
        }
    }
    INFO("MoveFileExW failures: " << legacy_failures << " of 20, of which "
                                  << legacy_access_denied
                                  << " were ERROR_ACCESS_DENIED");
    REQUIRE(legacy_failures > 0);
    REQUIRE(legacy_access_denied > 0);

    // The production route, under the same holder, two hundred times.
    const auto before = renderConfigAtomicReplaceRoutesForTesting();
    for (int attempt = 0; attempt < 200; ++attempt) {
        const std::string &next =
            (attempt % 2 == 0) ? committedManifest : preparedManifest;
        writeBytes(staging, next);
        REQUIRE_NOTHROW(
            renderConfigAtomicReplaceForTesting(staging, destination));
        REQUIRE_FALSE(std::filesystem::exists(staging));
        REQUIRE(readBytes(destination) == next);
    }
    const auto after = renderConfigAtomicReplaceRoutesForTesting();

    // Not just "it passed": it passed on the POSIX rename route, with no
    // fallback to the primitive the control just showed failing.
    INFO("last fallback reason: " << after.last_fallback_reason);
    REQUIRE(after.posix_renames - before.posix_renames == 200);
    REQUIRE(after.legacy_fallbacks == before.legacy_fallbacks);

    scanner.stop();
    INFO("scanner opened the destination " << scanner.opens() << " times");
    REQUIRE(scanner.opens() > 0);
}

TEST_CASE(
    "render authoring transaction replacement never exposes a torn manifest",
    "[render-config-transaction][atomic-replace][atomicity][windows]") {
    TemporaryDirectory directory{"authoring-atomicity"};
    const auto destination = directory.root / "manifest.json";
    const auto staging = directory.root / "manifest.next";
    writeBytes(destination, preparedManifest);
    REQUIRE(preparedManifest.size() != committedManifest.size());

    // The reader accepts only a whole prepared or a whole committed manifest.
    // Anything else -- an empty file, a prefix, a committed manifest with the
    // prepared one's tail behind it -- is a torn read, which is what a
    // write-in-place publication would produce.
    ScannerHolder reader{destination, {preparedManifest, committedManifest}};
    REQUIRE(reader.waitForContention());

    for (int attempt = 0; attempt < 200; ++attempt) {
        const std::string &next =
            (attempt % 2 == 0) ? committedManifest : preparedManifest;
        writeBytes(staging, next);
        REQUIRE_NOTHROW(
            renderConfigAtomicReplaceForTesting(staging, destination));
    }
    reader.stop();

    INFO("reader saw " << reader.matched() << " whole manifests and "
                       << reader.torn() << " torn ones");
    REQUIRE(reader.matched() > 0);
    REQUIRE(reader.torn() == 0);
    // The last replacement is still whole after the writer stops.
    const auto final_bytes = readBytes(destination);
    REQUIRE((final_bytes == preparedManifest ||
             final_bytes == committedManifest));
    REQUIRE(Json::parse(final_bytes).at("status").get<std::string>() ==
            (final_bytes == committedManifest ? "committed" : "prepared"));
}

TEST_CASE(
    "render authoring transaction commits while a scanner holds its manifest open",
    "[render-config-transaction][atomic-replace][commit][windows]") {
    // The same contention, but driven through the production commit path
    // rather than the primitive, on the file the GPU gate saw fail:
    // .pelican/render-authoring.transaction/manifest.json. The commit no
    // longer supersedes it -- it is created once per transaction and the
    // decision goes to a separate name -- so the scanner's handle has nothing
    // left to refuse.
    TemporaryDirectory project{"authoring-commit"};
    const auto document_path = project.root / "passes" / "main.json";
    std::filesystem::create_directories(document_path.parent_path());
    const std::string reference = "project://passes/main.json";

    std::string current =
        Json{{"schema", "pelican.render_pipeline"}, {"version", 1},
             {"features", Json::array()}}
            .dump(2) +
        "\n";
    writeBytes(document_path, current);

    // The scanner watches the manifest inside the transaction directory --
    // the exact path the failure named -- and validates every complete
    // manifest it manages to read.
    const auto manifest_path =
        renderConfigTransactionDirectory(project.root) / "manifest.json";
    std::atomic<bool> stop{false};
    std::atomic<long long> seen{0};
    std::atomic<long long> invalid{0};
    std::atomic<long long> committed_seen{0};
    std::thread scanner{[&] {
        while (!stop.load()) {
            const HANDLE handle = CreateFileW(
                manifest_path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            std::string bytes;
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) &&
                   read > 0) {
                bytes.append(buffer, read);
            }
            CloseHandle(handle);
            seen.fetch_add(1);
            try {
                const auto status =
                    Json::parse(bytes).at("status").get<std::string>();
                if (status == "committed") committed_seen.fetch_add(1);
                else if (status != "prepared") invalid.fetch_add(1);
            } catch (...) {
                invalid.fetch_add(1);
            }
        }
    }};

    const auto before = renderConfigAtomicReplaceRoutesForTesting();
    for (int round = 0; round < 40; ++round) {
        const std::string next =
            Json{{"schema", "pelican.render_pipeline"}, {"version", 1},
                 {"round", round},
                 {"features", Json::array()}}
                .dump(2) +
            "\n";
        const RenderConfigCandidateDocumentSet set{
            reference,
            {{.reference = reference,
              .normalized_reference = reference,
              .path = document_path,
              .operation = RenderConfigDocumentOperation::replace,
              .expected = {.existence = RenderConfigDocumentExistence::present,
                           .digest = renderConfigSourceDigest(current)},
              .bytes = next}}};
        INFO("round " << round);
        REQUIRE_NOTHROW(
            commitRenderConfigCandidateDocuments(project.root, set));
        REQUIRE(readBytes(document_path) == next);
        current = next;
    }
    stop.store(true);
    scanner.join();
    const auto after = renderConfigAtomicReplaceRoutesForTesting();

    INFO("manifests read: " << seen.load() << ", committed markers seen: "
                            << committed_seen.load()
                            << ", unreadable: " << invalid.load());
    // Every manifest the scanner managed to read was a complete document in
    // one of the two legal states; none was a partial write.
    REQUIRE(invalid.load() == 0);
    // The scanner has to have got in at all, or the line below says nothing.
    REQUIRE(seen.load() > 0);
    // And none of them was ever republished as committed. A manifest.json that
    // reads "committed" means the decision superseded a live file again.
    REQUIRE(committed_seen.load() == 0);
    REQUIRE(after.posix_renames > before.posix_renames);
    INFO("last fallback reason: " << after.last_fallback_reason);
    REQUIRE(after.legacy_fallbacks == before.legacy_fallbacks);

    // Whatever the scanner's handle delayed, recovery finishes: a committed
    // transaction directory is verified against the destination and removed,
    // and the destination keeps the committed bytes.
    REQUIRE_NOTHROW(recoverRenderConfigDocumentTransaction(project.root));
    REQUIRE_FALSE(
        std::filesystem::exists(renderConfigTransactionDirectory(project.root)));
    REQUIRE(readBytes(document_path) == current);
}
#endif

// The commit's own decision takes no replace route at all. A transaction whose
// documents are all creates supersedes nothing anywhere, so the replace route
// counter must not move across it -- and it does move the moment the committed
// state is published by overwriting the live manifest again.
TEST_CASE(
    "render authoring commit records its decision without superseding a live file",
    "[render-config-transaction][committed-marker][commit][route]") {
    TemporaryDirectory project{"authoring-marker-route"};
    const std::string root_reference = "project://passes/main.json";
    const std::string fragment_reference =
        "project://passes/authoring/pass-new.json";
    const auto root_path = project.root / "passes" / "main.json";
    const auto fragment_path =
        project.root / "passes" / "authoring" / "pass-new.json";

    const RenderConfigCandidateDocumentSet set{
        root_reference,
        {{.reference = root_reference,
          .normalized_reference = root_reference,
          .path = root_path,
          .operation = RenderConfigDocumentOperation::create,
          .expected = {.existence = RenderConfigDocumentExistence::missing},
          .bytes = crashCandidateRoot},
         {.reference = fragment_reference,
          .normalized_reference = fragment_reference,
          .path = fragment_path,
          .operation = RenderConfigDocumentOperation::create,
          .expected = {.existence = RenderConfigDocumentExistence::missing},
          .bytes = crashFragment}}};

    const auto before = replaceRouteTotal();
    const auto receipt =
        commitRenderConfigCandidateDocuments(project.root, set);
    const auto after = replaceRouteTotal();

    REQUIRE(receipt.changed());
    REQUIRE(readBytes(root_path) == crashCandidateRoot);
    REQUIRE(readBytes(fragment_path) == crashFragment);
    REQUIRE_FALSE(
        std::filesystem::exists(renderConfigTransactionDirectory(project.root)));
    INFO("replace route calls across the commit: " << (after - before));
    REQUIRE(after - before == 0);
}

#ifdef _WIN32
// Not a test in its own right: the body a crash fixture runs in a second
// process. Hidden, so discovery never registers it as a CTest case; the parent
// names it on the command line.
TEST_CASE("render authoring transaction commit stops this process on demand",
          "[.crash-child]") {
    const auto root = childEnvironment("PELICAN_TRANSACTION_CRASH_ROOT");
    const auto step = childEnvironment("PELICAN_TRANSACTION_CRASH_STEP");
    REQUIRE_FALSE(root.empty());
    REQUIRE_FALSE(step.empty());
    const auto stop = static_cast<std::size_t>(std::stoul(step));
    const std::filesystem::path project{root};
    commitRenderConfigCandidateDocuments(
        project, crashCandidateSet(project),
        [&](std::size_t index, const RenderConfigCandidateDocument &) {
            if (index != stop) return;
            std::_Exit(crashExitCode);
        });
    FAIL("the commit returned instead of stopping at step " << stop);
}

TEST_CASE(
    "render authoring recovery rolls back a transaction whose process died before the marker",
    "[render-config-transaction][committed-marker][recovery][crash][windows]") {
    TemporaryDirectory project{"authoring-crash-prepared"};
    seedCrashProject(project.root);
    // Stopped after the first document was applied and before the second.
    runCrashChild(project.root, 0);

    const auto directory = renderConfigTransactionDirectory(project.root);
    REQUIRE(std::filesystem::is_directory(directory));
    REQUIRE_FALSE(std::filesystem::exists(directory / "committed"));
    REQUIRE(Json::parse(readBytes(directory / "manifest.json"))
                .at("status")
                .get<std::string>() == "prepared");
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE_FALSE(std::filesystem::exists(crashFragmentDocument(project.root)));

    REQUIRE_NOTHROW(recoverRenderConfigDocumentTransaction(project.root));
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashOriginalRoot);
    REQUIRE_FALSE(std::filesystem::exists(crashFragmentDocument(project.root)));
    REQUIRE_FALSE(std::filesystem::exists(
        crashFragmentDocument(project.root).parent_path()));
    REQUIRE_FALSE(std::filesystem::exists(directory));
}

TEST_CASE(
    "render authoring recovery keeps a transaction whose marker outlived the process",
    "[render-config-transaction][committed-marker][recovery][crash][windows]") {
    TemporaryDirectory project{"authoring-crash-committed"};
    seedCrashProject(project.root);
    // Stopped immediately after the marker became visible: two documents were
    // applied, so the terminal step is index 2.
    runCrashChild(project.root, 2);

    const auto directory = renderConfigTransactionDirectory(project.root);
    REQUIRE(std::filesystem::is_regular_file(directory / "committed"));
    // The decision did not touch manifest.json: it is still the prepared
    // document the transaction published once, and the marker names its digest.
    const auto manifest_bytes = readBytes(directory / "manifest.json");
    REQUIRE(Json::parse(manifest_bytes).at("status").get<std::string>() ==
            "prepared");
    const auto marker = Json::parse(readBytes(directory / "committed"));
    REQUIRE(marker.at("manifest_digest").get<std::string>() ==
            renderConfigSourceDigest(manifest_bytes));
    REQUIRE_FALSE(marker.at("committed_at").get<std::string>().empty());

    REQUIRE_NOTHROW(recoverRenderConfigDocumentTransaction(project.root));
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE(readBytes(crashFragmentDocument(project.root)) == crashFragment);
    REQUIRE_FALSE(std::filesystem::exists(directory));
}

TEST_CASE(
    "render authoring cleanup never leaves a prepared manifest without its marker",
    "[render-config-transaction][committed-marker][recovery][crash][windows]") {
    TemporaryDirectory project{"authoring-crash-residue"};
    seedCrashProject(project.root);
    runCrashChild(project.root, 2);
    const auto directory = renderConfigTransactionDirectory(project.root);

    // The cleanup's own failure mode: something holds manifest.json and will
    // not share deletion. Whatever the removal manages before it gives up must
    // not be the one residue that reads as a rollback -- a prepared manifest
    // with no marker beside it -- because the entries are enumerated in an
    // order that would reach `committed` first.
    const HANDLE holder = CreateFileW(
        (directory / "manifest.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(holder != INVALID_HANDLE_VALUE);
    REQUIRE_THROWS(recoverRenderConfigDocumentTransaction(project.root));
    const bool manifest_left =
        std::filesystem::is_regular_file(directory / "manifest.json");
    const bool marker_left =
        std::filesystem::is_regular_file(directory / "committed");
    CloseHandle(holder);

    INFO("after the interrupted cleanup: manifest.json="
         << manifest_left << " committed=" << marker_left);
    REQUIRE_FALSE((manifest_left && !marker_left));

    // And the next recovery, once the handle is gone, still reads it as
    // committed and keeps both documents.
    REQUIRE_NOTHROW(recoverRenderConfigDocumentTransaction(project.root));
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE(readBytes(crashFragmentDocument(project.root)) == crashFragment);
    REQUIRE_FALSE(std::filesystem::exists(directory));
}

TEST_CASE(
    "render authoring recovery rejects a committed marker that names another manifest",
    "[render-config-transaction][committed-marker][recovery][crash][windows]") {
    TemporaryDirectory project{"authoring-crash-mismatch"};
    seedCrashProject(project.root);
    runCrashChild(project.root, 2);

    const auto directory = renderConfigTransactionDirectory(project.root);
    // The one thing the marker is allowed to be sure of, taken away.
    writeBytes(directory / "manifest.json",
               readBytes(directory / "manifest.json") + "\n");

    std::string message;
    try {
        recoverRenderConfigDocumentTransaction(project.root);
    } catch (const RenderConfigExternalModification &rejected) {
        message = rejected.what();
    }
    INFO("recovery said: " << message);
    REQUIRE(message.find("render_authoring_manifest_mismatch@1") !=
            std::string::npos);
    // Named rejection, not a silent rollback: the committed documents stand and
    // the directory is still there to be looked at.
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE(readBytes(crashFragmentDocument(project.root)) == crashFragment);
    REQUIRE(std::filesystem::is_directory(directory));
}

// MG-2a: the directory the previous format left -- no marker, the decision
// written into manifest.json as `status: "committed"` -- is not read any more.
// Both readers stop on it by name, and neither rolls it back nor removes it.
std::filesystem::path makeLegacyTransactionDirectory(
    const std::filesystem::path &project_root) {
    seedCrashProject(project_root);
    runCrashChild(project_root, 2);
    const auto directory = renderConfigTransactionDirectory(project_root);
    auto manifest = Json::parse(readBytes(directory / "manifest.json"));
    manifest["status"] = "committed";
    std::filesystem::remove(directory / "committed");
    writeBytes(directory / "manifest.json", manifest.dump(2) + "\n");
    return directory;
}

void requireLegacyDirectoryRefusal(
    const RenderConfigLegacyTransactionDirectory &refused,
    const std::filesystem::path &directory) {
    const std::string message = refused.what();
    INFO("refusal: " << message);
    REQUIRE(message.starts_with("render_config.legacy_transaction_dir@1: "));
    REQUIRE(std::filesystem::equivalent(refused.path(), directory));
    const auto fields = refused.errorFields();
    REQUIRE(fields.size() == 1);
    REQUIRE(std::filesystem::equivalent(
        std::filesystem::path{fields.at("path").get<std::string>()},
        directory));
    // The message says what to delete by hand.
    const auto carried = Json::parse(
        message.substr(std::string_view{"render_config.legacy_transaction_dir@1: "}.size()));
    REQUIRE(carried.at("path") == fields.at("path"));
    REQUIRE(carried.at("message").get<std::string>().find("delete this directory") !=
            std::string::npos);
}

TEST_CASE(
    "render authoring recovery stops by name on a directory the previous format wrote",
    "[render-config-transaction][committed-marker][recovery][crash][legacy][MG-2a][windows]") {
    TemporaryDirectory project{"authoring-crash-legacy"};
    const auto directory = makeLegacyTransactionDirectory(project.root);
    const auto manifest_before = readBytes(directory / "manifest.json");

    bool refused = false;
    try {
        recoverRenderConfigDocumentTransaction(project.root);
    } catch (const RenderConfigLegacyTransactionDirectory &error) {
        refused = true;
        requireLegacyDirectoryRefusal(error, directory);
    }
    REQUIRE(refused);
    // Neither read as committed (which would remove the directory) nor rolled
    // back (which would restore the original root): nothing moved.
    REQUIRE(std::filesystem::is_directory(directory));
    REQUIRE(readBytes(directory / "manifest.json") == manifest_before);
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE(readBytes(crashFragmentDocument(project.root)) == crashFragment);

    // Removed by hand, as the message says, the project recovers cleanly.
    std::filesystem::remove_all(directory);
    REQUIRE_NOTHROW(recoverRenderConfigDocumentTransaction(project.root));
}

TEST_CASE(
    "render authoring commit stops by name on a directory the previous format wrote",
    "[render-config-transaction][committed-marker][commit][legacy][MG-2a][windows]") {
    TemporaryDirectory project{"authoring-commit-legacy"};
    const auto directory = makeLegacyTransactionDirectory(project.root);
    const auto manifest_before = readBytes(directory / "manifest.json");

    bool refused = false;
    try {
        (void)commitRenderConfigCandidateDocuments(
            project.root, crashCandidateSet(project.root));
    } catch (const RenderConfigLegacyTransactionDirectory &error) {
        refused = true;
        requireLegacyDirectoryRefusal(error, directory);
    }
    REQUIRE(refused);
    REQUIRE(std::filesystem::is_directory(directory));
    REQUIRE(readBytes(directory / "manifest.json") == manifest_before);
    REQUIRE(readBytes(crashRootDocument(project.root)) == crashCandidateRoot);
    REQUIRE(readBytes(crashFragmentDocument(project.root)) == crashFragment);
}
#endif


namespace {
struct JournalRpc {
    PathResolver paths;
    std::istringstream input;
    std::ostringstream output;
    RpcServer server{input, output};
    explicit JournalRpc(const std::filesystem::path &root, watch::ReloadService *reload = nullptr) {
        paths.setup(root, false);
        configureMaterialDocumentRpcHandlers(server, paths, reload);
    }
    Json call(std::string_view method, const Json &params = Json::object()) {
        const auto response = Json::parse(server.processLine(Json{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump()));
        INFO(response.dump());
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
};
ProjectCandidateDocument journalEdit(const std::filesystem::path &root, const std::string &name,
                                     std::optional<std::string> next) {
    const auto path = root / name;
    const bool exists = std::filesystem::exists(path);
    return {.reference = name, .normalized_reference = name, .path = path,
        .operation = !next ? ProjectDocumentOperation::erase : exists ? ProjectDocumentOperation::replace : ProjectDocumentOperation::create,
        .expected = {exists ? ProjectDocumentExistence::present : ProjectDocumentExistence::missing,
                     exists ? projectDocumentDigest(readBytes(path)) : std::string{}},
        .bytes = next.value_or("")};
}
void journalWrite(const std::filesystem::path &root, const std::string &name, std::optional<std::string> next) {
    const auto edit = journalEdit(root, name, std::move(next));
    (void)commitProjectDocuments(root, std::span{&edit, std::size_t{1}});
}
}

TEST_CASE("RV-1 materialdocumentrpc journal restores four created replaced and deleted documents", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-four"};
    const std::string original = "\xef\xbb\xbf{ \"first\": 1, \"second\": 2 }\r\n";
    writeBytes(project.root / "pass.json", original);
    writeBytes(project.root / "bindings.json", "bindings before\n");
    writeBytes(project.root / "deleted.material", "deleted before\r\n");
    JournalRpc rpc(project.root);
    rpc.call("mark_document_baseline");
    journalWrite(project.root, "created.material", "new material");
    journalWrite(project.root, "pass.json", "pass after");
    journalWrite(project.root, "bindings.json", "bindings after");
    journalWrite(project.root, "deleted.material", std::nullopt);
    journalWrite(project.root, "pass.json", "pass edited twice");
    const auto entries = rpc.call("get_document_journal");
    REQUIRE(entries.at("documents").size() == 4);
    REQUIRE(entries.at("dirty") == true);
    const auto result = rpc.call("revert_documents_to_baseline", {{"closing", true}});
    REQUIRE(result.at("restored").size() == 4);
    REQUIRE(result.at("conflicts").empty());
    REQUIRE_FALSE(std::filesystem::exists(project.root / "created.material"));
    REQUIRE(readBytes(project.root / "deleted.material") == "deleted before\r\n");
    REQUIRE(readBytes(project.root / "pass.json") == original);
    REQUIRE(readBytes(project.root / "bindings.json") == "bindings before\n");
    REQUIRE(rpc.call("get_document_journal").at("dirty") == false);
    REQUIRE_FALSE(std::filesystem::exists(project.root / ".pelican/document_journal"));
}

TEST_CASE("RV-1 materialdocumentrpc revert preserves external edits and restores other paths", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-conflict"};
    JournalRpc rpc(project.root);
    writeBytes(project.root / "changed", "before");
    writeBytes(project.root / "deleted", "before");
    journalWrite(project.root, "changed", "engine");
    journalWrite(project.root, "deleted", std::nullopt);
    journalWrite(project.root, "other", "engine");
    // A truncated material is still an external edit, independent of JSON parsing.
    const std::string broken = "{\"materials\": []";
    writeBytes(project.root / "changed", broken);
    REQUIRE(rpc.call("get_document_journal").at("dirty") == true);
    writeBytes(project.root / "deleted", "external recreation");
    const auto result = rpc.call("revert_documents_to_baseline", {{"closing", true}});
    REQUIRE(result.at("conflicts") == Json::array({"changed", "deleted"}));
    REQUIRE(readBytes(project.root / "changed") == broken);
    REQUIRE(readBytes(project.root / "deleted") == "external recreation");
    REQUIRE_FALSE(std::filesystem::exists(project.root / "other"));
}

TEST_CASE("RV-1 materialdocumentrpc Save advances baseline and restart keeps it", "[RV-1][document-journal][save]") {
    TemporaryDirectory project{"rv1-restart"};
    writeBytes(project.root / "graph", "opened");
    {
        JournalRpc rpc(project.root);
        journalWrite(project.root, "graph", "saved");
        rpc.call("mark_document_baseline");
        REQUIRE(rpc.call("get_document_journal").at("dirty") == false);
        journalWrite(project.root, "graph", "restart_required");
    }
    JournalRpc restarted(project.root);
    REQUIRE(restarted.call("get_document_journal").at("dirty") == true);
    journalWrite(project.root, "graph", "after restart");
    restarted.call("revert_documents_to_baseline", {{"closing", true}});
    REQUIRE(readBytes(project.root / "graph") == "saved");
}

TEST_CASE("RV-1 document journal rolls back with failed transactions and publication", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-rollback"};
    JournalRpc rpc(project.root);
    writeBytes(project.root / "graph", "before");
    auto edit = journalEdit(project.root, "graph", "after");
    SECTION("transaction failure leaves no journal or source changes") {
        REQUIRE_THROWS(commitProjectDocuments(project.root, std::span{&edit, std::size_t{1}},
            [](std::size_t index, const auto &) { if (index == 1) throw std::runtime_error("fault"); }));
    }
    SECTION("defensive publication rollback includes journal metadata") {
        auto receipt = commitProjectDocuments(project.root, std::span{&edit, std::size_t{1}});
        REQUIRE(rpc.call("get_document_journal").at("dirty") == true);
        rollbackCommittedProjectDocuments(std::move(receipt));
    }
    REQUIRE(readBytes(project.root / "graph") == "before");
    REQUIRE(rpc.call("get_document_journal").at("dirty") == false);
}

TEST_CASE("RV-1 player transaction without RPC does not create a document journal", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-player"};
    journalWrite(project.root, "graph", "after");
    REQUIRE_FALSE(std::filesystem::exists(project.root / ".pelican/document_journal"));
}

TEST_CASE("RV-1 materialdocumentrpc running revert uses normal file reload participants", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-reload"};
    watch::ReloadService reload;
    bool reloaded = false;
    reload.registerParticipant({.name = "rv1.test.documents", .file = watch::FileReloadRole{
        .claims = [](const watch::ReloadRequest &request) { return request.key == watch::makeAssetKey("project://graph"); },
        .apply = watch::BatchReloadApply{.apply_batch = [&](std::span<const watch::ReloadRequest>) {
            reloaded = readBytes(project.root / "graph") == "before";
            return reloaded;
        }}}});
    JournalRpc rpc(project.root, &reload);
    writeBytes(project.root / "graph", "before");
    journalWrite(project.root, "graph", "after");
    const auto result = rpc.call("revert_documents_to_baseline");
    REQUIRE(reloaded);
    REQUIRE(result.at("reload_pending_restart").empty());
}


TEST_CASE("RV-1 materialdocumentrpc restart retains unsaved opened baseline", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-restart-opened"};
    writeBytes(project.root / "graph", "opened");
    {
        JournalRpc rpc(project.root);
        journalWrite(project.root, "graph", "restart_required");
    }
    JournalRpc restarted(project.root);
    REQUIRE(restarted.call("get_document_journal").at("dirty") == true);
    restarted.call("revert_documents_to_baseline", {{"closing", true}});
    REQUIRE(readBytes(project.root / "graph") == "opened");
}

TEST_CASE("RV-1 document journal keeps binary bytes and rejects stale CAS without recording it", "[RV-1][document-journal]") {
    TemporaryDirectory project{"rv1-binary"};
    JournalRpc rpc(project.root);
    const std::string original{"\0\xff\xfe\x80\r\n", 6};
    writeBytes(project.root / "asset.bin", original);
    auto stale = journalEdit(project.root, "asset.bin", "not committed");
    writeBytes(project.root / "asset.bin", "external");
    REQUIRE_THROWS_AS(commitProjectDocuments(project.root, std::span{&stale, std::size_t{1}}), ProjectDocumentExternalModification);
    REQUIRE(rpc.call("get_document_journal").at("dirty") == false);
    writeBytes(project.root / "asset.bin", original);
    journalWrite(project.root, "asset.bin", "new binary");
    rpc.call("revert_documents_to_baseline", {{"closing", true}});
    REQUIRE(readBytes(project.root / "asset.bin") == original);
}

} // namespace Pelican
