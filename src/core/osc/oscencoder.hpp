#pragma once

#include "../userpublic/valuegraph.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Pelican::Osc {

struct SendValue {
    ValueGraph::OscSendKind kind{};
    std::uint32_t bits{};
    bool operator==(const SendValue &) const = default;
};

std::vector<std::uint8_t> encodeMessage(std::string_view address, SendValue value);

} // namespace Pelican::Osc
