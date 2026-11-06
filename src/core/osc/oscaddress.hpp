#pragma once

#include <cstddef>
#include <string_view>

namespace Pelican::Osc {

inline constexpr std::size_t max_osc_address_bytes = 1024;
inline constexpr std::size_t max_osc_message_bytes = max_osc_address_bytes + 12;

inline bool isExactOscAddress(std::string_view address) noexcept {
    if (address.size() < 2 || address.size() > max_osc_address_bytes ||
        address.front() != '/')
        return false;
    for (const char value : address) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x21 || byte > 0x7e) return false;
        if (value == '*' || value == '?' || value == '[' || value == ']' ||
            value == '{' || value == '}')
            return false;
    }
    return true;
}

} // namespace Pelican::Osc
