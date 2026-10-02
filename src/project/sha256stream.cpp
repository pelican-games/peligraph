#include "sha256stream.hpp"

#include <picosha2.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace Pelican {

// picosha2 needs ~20 s for 73 MB in a Debug build, which outlasts the
// Studio's wait for the player's window; Windows hashes with CNG instead.
struct Sha256Stream::Impl {
    bool finished = false;
#ifdef _WIN32
    BCRYPT_HASH_HANDLE handle = nullptr;

    Impl() {
        if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &handle, nullptr, 0, nullptr, 0, 0))) {
            throw std::runtime_error("SHA-256 failed (BCryptCreateHash)");
        }
    }
    ~Impl() {
        if (handle != nullptr) BCryptDestroyHash(handle);
    }
#else
    picosha2::hash256_one_by_one hasher;

    Impl() { hasher.init(); }
#endif
};

Sha256Stream::Sha256Stream() : impl_(std::make_unique<Impl>()) {}

Sha256Stream::~Sha256Stream() = default;

void Sha256Stream::update(const void *data, std::size_t size) {
    if (impl_->finished) throw std::logic_error("Sha256Stream::update after finishHex");
    const auto *bytes = static_cast<const unsigned char *>(data);
#ifdef _WIN32
    while (size > 0) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
        if (!BCRYPT_SUCCESS(BCryptHashData(impl_->handle, const_cast<PUCHAR>(bytes), count, 0))) {
            throw std::runtime_error("SHA-256 failed (BCryptHashData)");
        }
        bytes += count;
        size -= count;
    }
#else
    impl_->hasher.process(bytes, bytes + size);
#endif
}

std::string Sha256Stream::finishHex() {
    if (impl_->finished) throw std::logic_error("Sha256Stream::finishHex called twice");
    impl_->finished = true;
    std::array<unsigned char, picosha2::k_digest_size> digest{};
#ifdef _WIN32
    if (!BCRYPT_SUCCESS(BCryptFinishHash(impl_->handle, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        throw std::runtime_error("SHA-256 failed (BCryptFinishHash)");
    }
#else
    impl_->hasher.finish();
    impl_->hasher.get_hash_bytes(digest.begin(), digest.end());
#endif
    return picosha2::bytes_to_hex_string(digest.begin(), digest.end());
}

std::string sha256Hex(std::string_view bytes) {
    Sha256Stream stream;
    stream.update(bytes.data(), bytes.size());
    return stream.finishHex();
}

} // namespace Pelican
