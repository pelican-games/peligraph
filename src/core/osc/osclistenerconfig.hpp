#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// OSC-2b: the UDP listener is configured process-wide from player options,
// never from a project file.  Every field is already validated; the listener
// only repeats the checks because the module also has a public start().
struct OscSendConfig {
    std::string host;
    std::uint16_t port = 0;
    bool operator==(const OscSendConfig &) const = default;
};

struct OscListenerConfig {
    std::uint16_t port = 0;
    std::string interface_address = "127.0.0.1";
    // Empty means every sender is accepted.  Non-empty is an exact match on
    // the sender's IPv4 address; it is a filter, not authentication.
    std::vector<std::string> allow;

    bool operator==(const OscListenerConfig &) const = default;
};

// Strict dotted-decimal IPv4: exactly four decimal octets, each 0..255, no
// host names, no signs, no whitespace.  Returns host byte order.
bool parseOscListenerIpv4(std::string_view text,
                          std::uint32_t &host_order) noexcept;

// Throws std::runtime_error("osc.config_invalid@1: ...").  `allow` absent means
// every sender; a present but empty or malformed list is an error.  The port
// text accepts only decimal digits and must fit 0..65535.
OscListenerConfig
parseOscListenerConfig(std::string_view port, std::string_view interface_address,
                       std::optional<std::string_view> allow);

OscSendConfig parseOscSendConfig(std::string_view text);

} // namespace Pelican
