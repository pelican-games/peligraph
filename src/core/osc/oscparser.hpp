#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Pelican::Osc {

// Defensive parser limits. They are large enough for ordinary OSC 1.0 traffic
// and small enough that a single datagram cannot allocate unbounded state.
inline constexpr std::size_t max_packet_bytes = 65535;
inline constexpr std::size_t max_bundle_depth = 8;
inline constexpr std::size_t max_messages_per_packet = 256;
inline constexpr std::size_t max_arguments_per_message = 64;

// A packet-level reason to drop the whole datagram. Structural damage anywhere
// wins over every message-level drop.
enum class PacketDrop : std::uint8_t {
    none,
    malformed,
    limit,
    future_timetag,
};

// A message-level reason to drop one element of a structurally valid bundle.
enum class MessageReject : std::uint8_t {
    none,
    nested_bundle,
    unsupported_type,
    pattern_unsupported,
};

struct Argument {
    char tag = '\0';
    std::int64_t integer = 0;
    double number = 0.0;
    bool boolean = false;
    std::string text;
    std::vector<std::uint8_t> blob;
};

struct Message {
    std::string address;
    std::vector<Argument> arguments;
    MessageReject reject = MessageReject::none;
};

struct Packet {
    PacketDrop drop = PacketDrop::none;
    bool bundle = false;
    std::uint64_t timetag = 0;
    std::vector<Message> messages;
};

// Parse one OSC 1.0 packet. The receiver decides when a bundle's 32.32 NTP
// timetag is applied. Parsing allocates but never throws for bad input.
Packet parsePacket(std::span<const std::uint8_t> bytes);

} // namespace Pelican::Osc
