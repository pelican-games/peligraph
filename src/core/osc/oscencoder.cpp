#include "oscencoder.hpp"

#include "oscaddress.hpp"

#include <stdexcept>

namespace Pelican::Osc {

std::vector<std::uint8_t> encodeMessage(std::string_view address, SendValue value) {
    if (!isExactOscAddress(address))
        throw std::invalid_argument("invalid or oversized OSC send address");
    const std::size_t address_bytes = (address.size() + 4) & ~std::size_t{3};
    const bool boolean = value.kind != ValueGraph::OscSendKind::floating &&
                         value.kind != ValueGraph::OscSendKind::integer;
    std::vector<std::uint8_t> bytes(address_bytes + (boolean ? 4 : 8), 0);
    for (std::size_t i = 0; i < address.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(address[i]);
    bytes[address_bytes] = ',';
    bytes[address_bytes + 1] = value.kind == ValueGraph::OscSendKind::floating
        ? 'f'
        : value.kind == ValueGraph::OscSendKind::integer
            ? 'i'
            : value.bits != 0 ? 'T' : 'F';
    if (!boolean) {
        bytes[address_bytes + 4] = static_cast<std::uint8_t>(value.bits >> 24);
        bytes[address_bytes + 5] = static_cast<std::uint8_t>(value.bits >> 16);
        bytes[address_bytes + 6] = static_cast<std::uint8_t>(value.bits >> 8);
        bytes[address_bytes + 7] = static_cast<std::uint8_t>(value.bits);
    }
    return bytes;
}

} // namespace Pelican::Osc
