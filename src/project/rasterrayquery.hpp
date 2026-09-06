#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

namespace Pelican {
// A boolean is the compact scene-wide form; named queries select one TLAS.
inline std::optional<std::string> parseRasterRayQuery(const nlohmann::json &pass) {
    const auto found = pass.find("ray_query");
    if (found == pass.end()) return std::nullopt;
    if (found->is_boolean())
        return found->get<bool>() ? std::optional<std::string>{"scene"} : std::nullopt;
    if (!found->is_object() || found->size() != 1 ||
        !found->contains("acceleration_structure") ||
        !found->at("acceleration_structure").is_string() ||
        found->at("acceleration_structure").get_ref<const std::string &>().empty())
        throw std::invalid_argument("Raster ray_query requires a boolean or an object with a nonempty acceleration_structure name");
    return found->at("acceleration_structure").get<std::string>();
}
} // namespace Pelican
