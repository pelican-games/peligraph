#include "oscbindings.hpp"
#include "oscaddress.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Pelican::Osc {
namespace {

[[noreturn]] void refuse(const std::string &detail) {
    throw std::runtime_error("osc.binding_invalid@1: " + detail);
}

const std::string &requireKey(const nlohmann::json &object, const char *key,
                              const std::string &context) {
    if (!object.contains(key) || !object[key].is_string())
        refuse(context + " requires a string '" + key + "'");
    return object[key].get_ref<const std::string &>();
}

std::array<double, 2> readRange(const nlohmann::json &object, const char *key,
                                const std::string &context) {
    if (!object.contains(key))
        refuse(context + " requires a '" + key + "' array");
    const auto &value = object[key];
    if (!value.is_array() || value.size() != 2)
        refuse(context + " '" + key + "' must be an array of two numbers");
    std::array<double, 2> range{};
    for (std::size_t i = 0; i < 2; ++i) {
        if (!value[i].is_number())
            refuse(context + " '" + key + "' must contain numbers");
        range[i] = value[i].get<double>();
        if (!std::isfinite(range[i]))
            refuse(context + " '" + key + "' must be finite");
    }
    return range;
}

} // namespace

std::vector<OscBinding>
parseOscBindings(const nlohmann::json &profile,
                 std::span<const LiveInputSpec> declarations) {
    std::vector<OscBinding> bindings;
    if (!profile.contains("live")) return bindings;
    if (!profile["live"].is_array())
        refuse("profile 'live' must be an array");
    std::unordered_set<std::string> seen_addresses;
    for (const auto &entry : profile["live"]) {
        if (!entry.is_object())
            refuse("every live binding must be an object");
        for (const auto &[key, unused] : entry.items()) {
            (void)unused;
            if (key != "address" && key != "input" && key != "map")
                refuse("unknown live binding key '" + key + "'");
        }
        OscBinding binding;
        binding.address = requireKey(entry, "address", "live binding");
        if (!isExactOscAddress(binding.address))
            refuse("live binding address '" + binding.address +
                   "' must be an exact, printable OSC address starting with '/'");
        binding.input = requireKey(entry, "input", "live binding '" +
                                                        binding.address + "'");
        const auto declared = std::find_if(
            declarations.begin(), declarations.end(),
            [&](const LiveInputSpec &spec) { return spec.name == binding.input; });
        if (declared == declarations.end())
            refuse("live binding '" + binding.address +
                   "' names undeclared live input '" + binding.input + "'");
        binding.input_type = declared->type;
        if (declared->type == LiveInputType::boolean && entry.contains("map"))
            refuse("bool live input '" + binding.input +
                   "' cannot have a linear map");
        if (entry.contains("map")) {
            const auto &map = entry["map"];
            if (!map.is_object())
                refuse("live binding '" + binding.address +
                       "' map must be an object");
            for (const auto &[key, unused] : map.items()) {
                (void)unused;
                if (key != "from" && key != "to")
                    refuse("unknown map key '" + key + "' in live binding '" +
                           binding.address + "'");
            }
            binding.has_map = true;
            const auto context = "live binding '" + binding.address + "' map";
            binding.map.from = readRange(map, "from", context);
            binding.map.to = readRange(map, "to", context);
            if (binding.map.from[0] == binding.map.from[1])
                refuse(context + " 'from' endpoints must differ");
        }
        if (!seen_addresses.insert(binding.address).second)
            refuse("live binding address '" + binding.address +
                   "' is declared twice");
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

} // namespace Pelican::Osc
