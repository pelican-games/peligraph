#include "oscparser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace Pelican::Osc {
namespace {

constexpr std::array<std::uint8_t, 8> bundle_header{'#', 'b', 'u', 'n', 'd', 'l',
                                                    'e', '\0'};

std::uint32_t readU32(const std::uint8_t *bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::int32_t readI32(const std::uint8_t *bytes) noexcept {
    return std::bit_cast<std::int32_t>(readU32(bytes));
}

std::uint64_t readU64(const std::uint8_t *bytes) noexcept {
    return (static_cast<std::uint64_t>(readU32(bytes)) << 32) |
           static_cast<std::uint64_t>(readU32(bytes + 4));
}

bool isPrintable(std::uint8_t value) noexcept {
    return value >= 0x20 && value <= 0x7e;
}

// OSC string: printable ASCII, NUL terminated, then zero padding to a 4-byte
// boundary. On success `cursor` lands after the padding.
bool readString(std::span<const std::uint8_t> bytes, std::size_t &cursor,
                std::size_t end, std::string &out) {
    const auto begin = cursor;
    std::size_t nul = begin;
    while (nul < end && bytes[nul] != 0) {
        if (!isPrintable(bytes[nul])) return false;
        ++nul;
    }
    if (nul == end) return false;
    const auto padded = (nul - begin + 1 + 3u) & ~std::size_t{3u};
    if (begin > end || padded > end - begin) return false;
    for (std::size_t i = nul + 1; i < begin + padded; ++i) {
        if (bytes[i] != 0) return false;
    }
    out.assign(reinterpret_cast<const char *>(bytes.data() + begin), nul - begin);
    cursor = begin + padded;
    return true;
}

bool hasBundleHeader(std::span<const std::uint8_t> bytes, std::size_t begin,
                     std::size_t end) noexcept {
    if (end - begin < bundle_header.size()) return false;
    return std::memcmp(bytes.data() + begin, bundle_header.data(),
                       bundle_header.size()) == 0;
}

bool addressIsPattern(const std::string &address) noexcept {
    return address.find_first_of("*?[]{}") != std::string::npos;
}

enum class Check : std::uint8_t {
    ok,
    unsupported,
    malformed,
    limit,
};

// Validate one message element. This does not decode values; it only proves
// the address/tag strings, every payload and the exact end of the element.
Check validateMessage(std::span<const std::uint8_t> bytes, std::size_t begin,
                      std::size_t end) {
    std::size_t cursor = begin;
    std::string address;
    if (!readString(bytes, cursor, end, address)) return Check::malformed;
    if (address.empty() || address.front() != '/') return Check::malformed;
    // An OSC 1.0 message always carries a type tag string, even when it has
    // no arguments. Without one the element is not a complete message.
    if (cursor == end) return Check::malformed;
    if (bytes[cursor] != ',') return Check::malformed;

    std::string tags;
    if (!readString(bytes, cursor, end, tags)) return Check::malformed;
    if (tags.empty() || tags.front() != ',') return Check::malformed;
    if (tags.size() - 1 > max_arguments_per_message) return Check::limit;

    for (std::size_t i = 1; i < tags.size(); ++i) {
        const char tag = tags[i];
        switch (tag) {
        case 'T':
        case 'F':
        case 'N':
        case 'I':
            break;
        case 'i':
        case 'f':
        case 'c':
        case 'r':
        case 'm':
            if (end - cursor < 4) return Check::malformed;
            cursor += 4;
            break;
        case 'h':
        case 't':
        case 'd':
            if (end - cursor < 8) return Check::malformed;
            cursor += 8;
            break;
        case 's': {
            std::string ignored;
            if (!readString(bytes, cursor, end, ignored)) return Check::malformed;
            break;
        }
        case 'b': {
            if (end - cursor < 4) return Check::malformed;
            const auto size = readI32(bytes.data() + cursor);
            cursor += 4;
            if (size < 0) return Check::malformed;
            const auto blob_size = static_cast<std::uint32_t>(size);
            const auto padded_size = (blob_size + 3u) & ~std::uint32_t{3u};
            if (padded_size > end - cursor) return Check::malformed;
            for (std::uint32_t i = blob_size; i < padded_size; ++i)
                if (bytes[cursor + i] != 0) return Check::malformed;
            cursor += padded_size;
            break;
        }
        // OSC 1.1 arrays and any tag this parser does not know cannot have
        // their payload size checked. The element is still structurally
        // unbounded, so mark it unsupported and stop here rather than guess.
        case '[':
        case ']':
        default:
            return Check::unsupported;
        }
    }
    return cursor == end ? Check::ok : Check::malformed;
}

Check validateBundle(std::span<const std::uint8_t> bytes, std::size_t begin,
                     std::size_t end, std::size_t depth,
                     std::size_t &message_count) {
    if (depth > max_bundle_depth) return Check::limit;
    if (end - begin < 16) return Check::malformed;
    std::size_t cursor = begin + 8;
    const auto timetag = readU64(bytes.data() + cursor);
    (void)timetag;
    cursor += 8;
    while (cursor < end) {
        if (end - cursor < 4) return Check::malformed;
        const auto signed_size = readI32(bytes.data() + cursor);
        if (signed_size <= 0) return Check::malformed;
        if ((signed_size & 3) != 0) return Check::malformed;
        const auto size = static_cast<std::uint32_t>(signed_size);
        cursor += 4;
        if (size > end - cursor) return Check::malformed;
        const auto element_end = cursor + size;
        ++message_count;
        if (message_count > max_messages_per_packet) return Check::limit;
        if (hasBundleHeader(bytes, cursor, element_end)) {
            const auto check =
                validateBundle(bytes, cursor, element_end, depth + 1, message_count);
            if (check != Check::ok) return check;
        } else {
            const auto check = validateMessage(bytes, cursor, element_end);
            if (check == Check::limit) return check;
            if (check == Check::malformed) return check;
        }
        cursor = element_end;
    }
    return cursor == end ? Check::ok : Check::malformed;
}

void decodeMessage(std::span<const std::uint8_t> bytes, std::size_t begin,
                   std::size_t end, Message &message) {
    std::size_t cursor = begin;
    std::string address;
    if (!readString(bytes, cursor, end, address)) return;
    message.address = std::move(address);
    if (addressIsPattern(message.address)) {
        message.reject = MessageReject::pattern_unsupported;
        return;
    }
    if (cursor == end) return;
    std::string tags;
    if (!readString(bytes, cursor, end, tags)) return;
    message.arguments.reserve(tags.size() - 1);
    for (std::size_t i = 1; i < tags.size(); ++i) {
        Argument argument;
        argument.tag = tags[i];
        switch (argument.tag) {
        case 'T': argument.boolean = true; break;
        case 'F': argument.boolean = false; break;
        case 'N':
        case 'I':
            message.reject = MessageReject::unsupported_type;
            message.arguments.clear();
            return;
        case 'i':
            if (end - cursor < 4) return;
            argument.integer = readI32(bytes.data() + cursor);
            cursor += 4;
            break;
        case 'f':
            if (end - cursor < 4) return;
            argument.number = static_cast<double>(
                std::bit_cast<float>(readU32(bytes.data() + cursor)));
            cursor += 4;
            break;
        case 'h':
            if (end - cursor < 8) return;
            argument.integer = static_cast<std::int64_t>(
                readU64(bytes.data() + cursor));
            cursor += 8;
            break;
        case 't':
            if (end - cursor < 8) return;
            argument.integer = static_cast<std::int64_t>(
                readU64(bytes.data() + cursor));
            cursor += 8;
            break;
        case 'd':
            if (end - cursor < 8) return;
            argument.number =
                std::bit_cast<double>(readU64(bytes.data() + cursor));
            cursor += 8;
            break;
        case 'c':
            if (end - cursor < 4) return;
            argument.integer = readI32(bytes.data() + cursor);
            cursor += 4;
            break;
        case 'r':
        case 'm':
            if (end - cursor < 4) return;
            argument.integer = readU32(bytes.data() + cursor);
            cursor += 4;
            break;
        case 's': {
            if (!readString(bytes, cursor, end, argument.text)) return;
            break;
        }
        case 'b': {
            if (end - cursor < 4) return;
            const auto size = readI32(bytes.data() + cursor);
            cursor += 4;
            if (size < 0) return;
            const auto blob_size = static_cast<std::uint32_t>(size);
            const auto padded_size = (blob_size + 3u) & ~std::uint32_t{3u};
            if (padded_size > end - cursor) return;
            argument.blob.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                 bytes.begin() +
                                     static_cast<std::ptrdiff_t>(cursor + blob_size));
            cursor += padded_size;
            break;
        }
        default:
            message.reject = MessageReject::unsupported_type;
            message.arguments.clear();
            return;
        }
        message.arguments.push_back(std::move(argument));
    }
}

void decodeBundle(std::span<const std::uint8_t> bytes, std::size_t begin,
                  std::size_t end, Packet &packet) {
    std::size_t cursor = begin + 16;
    while (cursor < end) {
        const auto size = static_cast<std::uint32_t>(
            readI32(bytes.data() + cursor));
        cursor += 4;
        const auto element_end = cursor + size;
        if (hasBundleHeader(bytes, cursor, element_end)) {
            Message nested;
            nested.address = "#bundle";
            nested.reject = MessageReject::nested_bundle;
            packet.messages.push_back(std::move(nested));
        } else {
            Message message;
            decodeMessage(bytes, cursor, element_end, message);
            packet.messages.push_back(std::move(message));
        }
        cursor = element_end;
    }
}

} // namespace

Packet parsePacket(std::span<const std::uint8_t> bytes) {
    Packet packet;
    if (bytes.size() > max_packet_bytes) {
        packet.drop = PacketDrop::limit;
        return packet;
    }
    if (bytes.empty() || (bytes.size() & 3u) != 0) {
        packet.drop = PacketDrop::malformed;
        return packet;
    }

    if (hasBundleHeader(bytes, 0, bytes.size())) {
        packet.bundle = true;
        if (bytes.size() < 16) {
            packet.drop = PacketDrop::malformed;
            return packet;
        }
        packet.timetag = readU64(bytes.data() + 8);
        std::size_t message_count = 0;
        const auto check =
            validateBundle(bytes, 0, bytes.size(), 0, message_count);
        if (check == Check::limit) {
            packet.drop = PacketDrop::limit;
            return packet;
        }
        if (check != Check::ok) {
            packet.drop = PacketDrop::malformed;
            return packet;
        }
        decodeBundle(bytes, 0, bytes.size(), packet);
        return packet;
    }

    const auto check = validateMessage(bytes, 0, bytes.size());
    if (check == Check::limit) {
        packet.drop = PacketDrop::limit;
        return packet;
    }
    if (check == Check::malformed) {
        packet.drop = PacketDrop::malformed;
        return packet;
    }
    Message message;
    decodeMessage(bytes, 0, bytes.size(), message);
    packet.messages.push_back(std::move(message));
    return packet;
}

} // namespace Pelican::Osc
