#include "contentdigest.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <picosha2.h>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Pelican::watch {
namespace {

std::string canonicalRelative(std::filesystem::path path) {
    path = path.lexically_normal();
    auto result = path.generic_u8string();
    std::string narrow{reinterpret_cast<const char *>(result.data()), result.size()};
    while (narrow.starts_with("./")) narrow.erase(0, 2);
    return narrow;
}

#ifdef _WIN32
std::wstring extendedPath(const std::filesystem::path &input) {
    auto path = std::filesystem::absolute(input).native();
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::optional<FileIdentity> identityFromHandle(HANDLE handle, std::string &error) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        error = "GetFileInformationByHandle failed: " + std::to_string(GetLastError());
        return std::nullopt;
    }
    ULARGE_INTEGER size{};
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    const std::uint64_t file_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                    info.ftLastWriteTime.dwLowDateTime;
    return FileIdentity{info.dwVolumeSerialNumber,
                        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
                        size.QuadPart,
                        std::filesystem::file_time_type{std::filesystem::file_time_type::duration{file_time}}};
}
#endif

} // namespace

AssetKey makeAssetKey(std::string store, const std::filesystem::path &relative_path) {
    return {std::move(store), canonicalRelative(relative_path)};
}

std::string assetKeyString(const AssetKey &key) { return key.store + ":/" + key.path; }

ContentDigestResult readStableContentDigest(const std::filesystem::path &path, const CancelCheck &cancel) {
    if (cancel && cancel()) return {.status = DigestReadStatus::cancelled};
#ifdef _WIN32
    const HANDLE file = CreateFileW(extendedPath(path).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return {.status = DigestReadStatus::missing};
        }
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
            code == ERROR_ACCESS_DENIED) {
            return {.status = DigestReadStatus::retry, .error = "file is busy: " + std::to_string(code)};
        }
        return {.status = DigestReadStatus::error, .error = "CreateFileW failed: " + std::to_string(code)};
    }
    struct CloseHandleGuard { HANDLE value; ~CloseHandleGuard() { CloseHandle(value); } } guard{file};
    std::string identity_error;
    const auto before = identityFromHandle(file, identity_error);
    if (!before) return {.status = DigestReadStatus::retry, .error = std::move(identity_error)};

    picosha2::hash256_one_by_one hasher;
    hasher.init();
    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint64_t bytes = 0;
    for (;;) {
        if (cancel && cancel()) return {.status = DigestReadStatus::cancelled};
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            const auto code = GetLastError();
            return {.status = code == ERROR_OPERATION_ABORTED ? DigestReadStatus::cancelled
                                                               : DigestReadStatus::retry,
                    .error = "ReadFile failed: " + std::to_string(code)};
        }
        if (read == 0) break;
        hasher.process(buffer.begin(), buffer.begin() + read);
        bytes += read;
    }
    const auto after = identityFromHandle(file, identity_error);
    if (!after || *before != *after || bytes != after->size) {
        return {.status = DigestReadStatus::retry, .byte_count = bytes,
                .error = "file changed while hashing"};
    }
    hasher.finish();
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    hasher.get_hash_bytes(digest.begin(), digest.end());
    return {.status = DigestReadStatus::stable,
            .sha256 = picosha2::bytes_to_hex_string(digest.begin(), digest.end()),
            .byte_count = bytes, .identity = after};
#else
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {.status = DigestReadStatus::missing};
    const auto before_size = std::filesystem::file_size(path, ec);
    if (ec) return {.status = DigestReadStatus::retry, .error = ec.message()};
    const auto before_time = std::filesystem::last_write_time(path, ec);
    if (ec) return {.status = DigestReadStatus::retry, .error = ec.message()};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {.status = DigestReadStatus::retry, .error = "open failed"};
    picosha2::hash256_one_by_one hasher;
    hasher.init();
    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint64_t bytes = 0;
    while (stream) {
        if (cancel && cancel()) return {.status = DigestReadStatus::cancelled};
        stream.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const auto count = static_cast<std::size_t>(stream.gcount());
        hasher.process(buffer.begin(), buffer.begin() + count);
        bytes += count;
    }
    const auto after_size = std::filesystem::file_size(path, ec);
    const auto after_time = std::filesystem::last_write_time(path, ec);
    if (ec || before_size != after_size || before_time != after_time || bytes != after_size)
        return {.status = DigestReadStatus::retry, .byte_count = bytes, .error = "file changed while hashing"};
    hasher.finish();
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    hasher.get_hash_bytes(digest.begin(), digest.end());
    return {.status = DigestReadStatus::stable,
            .sha256 = picosha2::bytes_to_hex_string(digest.begin(), digest.end()),
            .byte_count = bytes,
            .identity = FileIdentity{0, 0, bytes, after_time}};
#endif
}

SourceDigestState ContentDigestState::snapshot(const AssetKey &key) const {
    std::scoped_lock lock{mutex_};
    if (const auto found = states_.find(key); found != states_.end()) return found->second;
    return {};
}

void ContentDigestState::seedLive(const AssetKey &key, const std::string &digest) {
    std::scoped_lock lock{mutex_};
    auto &state = states_[key];
    state.observed_digest = digest;
    state.live_digest = digest;
    state.pending_digest.reset();
}

void ContentDigestState::registerSelfWrite(const AssetKey &key, std::string expected_digest,
                                           std::uint64_t epoch, bool runtime_apply_succeeded) {
    std::scoped_lock lock{mutex_};
    auto &state = states_[key];
    if (runtime_apply_succeeded) state.live_digest = expected_digest;
    state.self_write = SelfWriteToken{std::move(expected_digest), epoch, runtime_apply_succeeded};
}

ObserveDisposition ContentDigestState::observe(const AssetKey &key, const std::string &digest,
                                                std::uint64_t epoch) {
    std::scoped_lock lock{mutex_};
    auto &state = states_[key];
    state.observed_digest = digest;
    state.pending_digest = digest;
    if (state.self_write) {
        const bool match = state.self_write->expected_digest == digest && state.self_write->epoch == epoch;
        const bool may_consume = match && state.self_write->runtime_applied && state.live_digest == digest;
        state.self_write.reset(); // one observation consumes or invalidates the token
        if (may_consume) {
            state.pending_digest.reset();
            return ObserveDisposition::consumed_self_write;
        }
        // A hash match from another epoch is deliberately not allowed to fall
        // through to the live-digest equality shortcut. The old transaction
        // cannot suppress work in the new gate epoch.
        if (!match) return ObserveDisposition::queue_reload;
    }
    if (state.live_digest == digest) {
        state.pending_digest.reset();
        return ObserveDisposition::unchanged;
    }
    return ObserveDisposition::queue_reload;
}

bool ContentDigestState::observeMissing(const AssetKey &key) {
    std::scoped_lock lock{mutex_};
    auto &state = states_[key];
    state.observed_digest.reset();
    state.pending_digest.reset();
    state.self_write.reset();
    return state.live_digest.has_value();
}

void ContentDigestState::commitSucceeded(const AssetKey &key, const std::optional<std::string> &digest) {
    std::scoped_lock lock{mutex_};
    auto &state = states_[key];
    state.live_digest = digest;
    state.pending_digest.reset();
}

void ContentDigestState::commitFailed(const AssetKey &key) {
    std::scoped_lock lock{mutex_};
    if (auto found = states_.find(key); found != states_.end()) found->second.pending_digest.reset();
}

void ContentDigestState::erase(const AssetKey &key) {
    std::scoped_lock lock{mutex_};
    states_.erase(key);
}

} // namespace Pelican::watch
