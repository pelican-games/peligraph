#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Pelican {

// Streaming SHA-256: init (construct), update, finish. Windows hashes with CNG
// (BCrypt), elsewhere with picosha2; the lowercase hex is the same either way.
// The asset manifest check (assetFileSha256) and the file watcher
// (readStableContentDigest) both hash through this. Errors throw
// std::runtime_error.
class Sha256Stream {
  public:
    // The read unit both callers use.
    static constexpr std::size_t read_chunk_bytes = std::size_t{1} << 20;

    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream &) = delete;
    Sha256Stream &operator=(const Sha256Stream &) = delete;

    void update(const void *data, std::size_t size);
    // Lowercase hex of the digest. Once; update and finishHex throw after it.
    std::string finishHex();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-shot convenience over Sha256Stream.
std::string sha256Hex(std::string_view bytes);

} // namespace Pelican
