#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace Pelican {

// Stable serialization fingerprints are diagnostic/provenance identities,
// not security hashes. Length-prefixing every string avoids concatenation
// ambiguity and the explicit byte order keeps values host-independent.
class StableFingerprint64 {
    std::uint64_t value_ = 14695981039346656037ULL;

  public:
    void appendByte(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }

    void appendUnsigned(std::uint64_t value) noexcept {
        for (std::uint32_t shift = 0; shift < 64; shift += 8) {
            appendByte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void appendString(std::string_view value) noexcept {
        appendUnsigned(value.size());
        for (const auto byte : value) {
            appendByte(static_cast<std::uint8_t>(
                static_cast<unsigned char>(byte)));
        }
    }

    std::uint64_t value() const noexcept {
        return value_;
    }
};

inline std::string stableFingerprint64String(
    std::uint64_t fingerprint) {
    constexpr std::string_view prefix = "fnv1a64:";
    std::array<char, 16> digits{};
    const auto converted = std::to_chars(
        digits.data(), digits.data() + digits.size(),
        fingerprint, 16);
    if (converted.ec != std::errc{}) {
        throw std::runtime_error(
            "failed to encode stable fingerprint");
    }
    std::string result{prefix};
    result.append(
        digits.size() -
            static_cast<std::size_t>(
                converted.ptr - digits.data()),
        '0');
    result.append(digits.data(), converted.ptr);
    return result;
}

inline std::uint64_t parseStableFingerprint64(
    std::string_view encoded, std::string_view context) {
    constexpr std::string_view prefix = "fnv1a64:";
    if (!encoded.starts_with(prefix) ||
        encoded.size() != prefix.size() + 16) {
        throw std::runtime_error(
            std::string{context} +
            " must use fnv1a64: followed by 16 lowercase hex "
            "digits");
    }
    const auto first = encoded.data() + prefix.size();
    const auto last = encoded.data() + encoded.size();
    std::uint64_t result = 0;
    const auto parsed =
        std::from_chars(first, last, result, 16);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != last ||
        std::any_of(
            first, last, [](char character) {
                return character >= 'A' &&
                       character <= 'F';
            })) {
        throw std::runtime_error(
            std::string{context} +
            " must use fnv1a64: followed by 16 lowercase hex "
            "digits");
    }
    return result;
}

} // namespace Pelican
