#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

std::optional<std::string_view> engineResource(std::string_view id);
std::span<const std::string_view> registeredEngineResourceIds();
std::string registeredEngineResourceIdsMessage();
std::string engineResourceOrThrow(std::string_view id);

} // namespace Pelican
