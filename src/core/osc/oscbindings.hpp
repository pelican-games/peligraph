#pragma once

#include "../os/liveinputs.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican::Osc {

// A linear map from the OSC argument range [from0, from1] to the live value
// range [to0, to1]. `from0 == from1` is refused; reversed ranges are allowed.
struct OscBindingMap {
    std::array<double, 2> from{0.0, 1.0};
    std::array<double, 2> to{0.0, 1.0};
};

struct OscBinding {
    std::string address;
    std::string input;
    LiveInputType input_type = LiveInputType::floating;
    bool has_map = false;
    OscBindingMap map;
};

// Parse the `live` member of one pelican.input_profile document. The action
// half is parsed by parseInputProfileJson; this half only sees the addresses,
// live input names and optional maps. Throws
// `osc.binding_invalid@1: <detail>` for any shape, name or map error.
std::vector<OscBinding>
parseOscBindings(const nlohmann::json &profile,
                 std::span<const LiveInputSpec> declarations);

} // namespace Pelican::Osc
