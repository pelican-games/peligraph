#include "engineresources.hpp"

#include <array>
#include <sstream>
#include <stdexcept>

#include "battery/embed.hpp"

namespace Pelican {

namespace {

constexpr std::array<std::string_view, 1> registered_ids{
    "default_config.json",
};

} // namespace

std::optional<std::string_view> engineResource(std::string_view id) {
    if (id == "default_config.json") {
        static const std::string default_config = b::embed<"default_config.json">().str();
        return std::string_view{default_config};
    }
    return std::nullopt;
}

std::span<const std::string_view> registeredEngineResourceIds() {
    return std::span<const std::string_view>{registered_ids.data(), registered_ids.size()};
}

std::string registeredEngineResourceIdsMessage() {
    std::ostringstream stream;
    bool first = true;
    for (const auto id : registeredEngineResourceIds()) {
        if (!first) {
            stream << ", ";
        }
        stream << id;
        first = false;
    }
    return stream.str();
}

std::string engineResourceOrThrow(std::string_view id) {
    if (const auto resource = engineResource(id)) {
        return std::string{*resource};
    }

    throw std::runtime_error("Unknown engine resource id: " + std::string{id} +
                             ". Registered ids: " + registeredEngineResourceIdsMessage());
}

} // namespace Pelican
