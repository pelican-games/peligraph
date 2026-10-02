#pragma once

#include "assetkey.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace Pelican::watch {

enum class DigestReadStatus { stable, missing, retry, cancelled, error };

struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t file = 0;
    std::uint64_t size = 0;
    std::filesystem::file_time_type mtime{};

    auto operator<=>(const FileIdentity &) const = default;
};

struct ContentDigestResult {
    DigestReadStatus status = DigestReadStatus::error;
    std::string sha256;
    std::uint64_t byte_count = 0;
    std::optional<FileIdentity> identity;
    std::string error;
};

using CancelCheck = std::function<bool()>;

// Reads in 1 MiB chunks (Sha256Stream) and accepts a digest only if identity,
// size and mtime remain unchanged for the whole read. Sharing violations and
// an unstable file are retryable, not permanent failures.
ContentDigestResult readStableContentDigest(const std::filesystem::path &path,
                                            const CancelCheck &cancel = {});

struct SelfWriteToken {
    std::string expected_digest;
    std::uint64_t epoch = 0;
    bool runtime_applied = false;
};

struct SourceDigestState {
    std::optional<std::string> observed_digest;
    std::optional<std::string> live_digest;
    std::optional<std::string> pending_digest;
    std::optional<SelfWriteToken> self_write;
    // FW-1: the source was present when the watcher started but was not
    // hashed. Its live bytes are unknown and equal no digest, so its first
    // observation queues a reload; a removal is still reported.
    bool live_unknown = false;
};

enum class ObserveDisposition { unchanged, queue_reload, consumed_self_write };

class ContentDigestState {
  public:
    SourceDigestState snapshot(const AssetKey &key) const;
    void seedLive(const AssetKey &key, const std::string &digest);
    void seedLiveUnknown(const AssetKey &key);
    void registerSelfWrite(const AssetKey &key, std::string expected_digest,
                           std::uint64_t epoch, bool runtime_apply_succeeded);
    ObserveDisposition observe(const AssetKey &key, const std::string &digest,
                               std::uint64_t epoch);
    bool observeMissing(const AssetKey &key);
    void commitSucceeded(const AssetKey &key, const std::optional<std::string> &digest);
    void commitFailed(const AssetKey &key);
    void erase(const AssetKey &key);

  private:
    mutable std::mutex mutex_;
    std::map<AssetKey, SourceDigestState> states_;
};

} // namespace Pelican::watch
