#include "osclistenerconfig.hpp"

#include <charconv>
#include <stdexcept>
#include <system_error>

namespace Pelican {
namespace {

[[noreturn]] void invalid(const std::string &detail) {
    throw std::runtime_error("osc.config_invalid@1: " + detail);
}

std::string textOf(std::string_view value) {
    return std::string{value};
}

bool parseOctet(std::string_view text, std::uint32_t &value) noexcept {
    if (text.empty() || text.size() > 3) return false;
    unsigned parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    if (parsed > 255) return false;
    value = parsed;
    return true;
}

std::uint16_t parsePort(std::string_view text) {
    if (text.empty()) invalid("--osc-port must not be empty");
    unsigned parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        invalid("--osc-port must be a decimal integer in 0..65535: " +
                textOf(text));
    if (parsed > 65535)
        invalid("--osc-port must be in 0..65535: " + textOf(text));
    return static_cast<std::uint16_t>(parsed);
}

} // namespace

bool parseOscListenerIpv4(std::string_view text,
                          std::uint32_t &host_order) noexcept {
    std::uint32_t octets[4]{};
    std::size_t begin = 0;
    for (int part = 0; part < 4; ++part) {
        const auto end = text.find('.', begin);
        const bool last = part == 3;
        if ((last && end != std::string_view::npos) ||
            (!last && end == std::string_view::npos))
            return false;
        const auto piece = last ? text.substr(begin)
                                : text.substr(begin, end - begin);
        if (!parseOctet(piece, octets[part])) return false;
        if (!last) begin = end + 1;
    }
    host_order = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) |
                 octets[3];
    return true;
}

OscListenerConfig
parseOscListenerConfig(std::string_view port, std::string_view interface_address,
                       std::optional<std::string_view> allow) {
    OscListenerConfig config;
    config.port = parsePort(port);

    std::uint32_t interface_host = 0;
    if (!parseOscListenerIpv4(interface_address, interface_host)) {
        invalid("--osc-interface must be a dotted-decimal IPv4 address: " +
                textOf(interface_address));
    }
    config.interface_address = textOf(interface_address);

    if (allow.has_value()) {
        const auto list = *allow;
        if (list.empty())
            invalid("--osc-allow must contain at least one IPv4 address");
        std::size_t begin = 0;
        while (true) {
            const auto end = list.find(',', begin);
            const auto token = end == std::string_view::npos
                                   ? list.substr(begin)
                                   : list.substr(begin, end - begin);
            std::uint32_t host = 0;
            if (!parseOscListenerIpv4(token, host)) {
                invalid("--osc-allow must be a comma-separated list of "
                        "dotted-decimal IPv4 addresses: " +
                        textOf(list));
            }
            config.allow.push_back(textOf(token));
            if (end == std::string_view::npos) break;
            begin = end + 1;
            if (begin == list.size())
                invalid("--osc-allow must not end with a comma: " +
                        textOf(list));
        }
    }
    return config;
}

} // namespace Pelican
