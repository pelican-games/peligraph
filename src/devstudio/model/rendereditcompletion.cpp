#include "rendereditcompletion.hpp"

namespace PelicanStudio {

std::string renderEditRestartLine(std::string_view code) {
    std::string line = "Restarting the engine to apply this change";
    if (!code.empty()) {
        line += " (";
        line += code;
        line += ")";
    }
    return line + "...";
}

RenderEditCompletion readRenderEditCompletion(const nlohmann::json &response) {
    RenderEditCompletion reading;
    if (!response.is_object()) return reading;
    if (const auto status = response.find("status");
        status != response.end() && status->is_string())
        reading.status = status->get<std::string>();
    const auto flag = [&response](const char *key) {
        const auto found = response.find(key);
        return found != response.end() && found->is_boolean() && found->get<bool>();
    };
    reading.committed = reading.status == "committed" && flag("committed");
    if (!reading.committed) return reading;
    reading.no_change = flag("no_change");
    if (const auto generation = response.find("published_generation");
        generation != response.end() && generation->is_number_unsigned())
        reading.published_generation = generation->get<std::uint64_t>();
    if (const auto digest = response.find("source_digest");
        digest != response.end() && digest->is_object()) {
        const auto hex = digest->find("hex");
        if (hex != digest->end() && hex->is_string())
            reading.source_digest = hex->get<std::string>();
    }
    reading.restart_required = flag("restart_required");
    if (!reading.restart_required) return reading;
    if (const auto reason = response.find("restart_reason");
        reason != response.end() && reason->is_object()) {
        const auto code = reason->find("code");
        if (code != reason->end() && code->is_string())
            reading.restart_code = code->get<std::string>();
    }
    reading.restart_line = renderEditRestartLine(reading.restart_code);
    return reading;
}

RenderEditCompletion completeRenderEdit(
    const nlohmann::json &response,
    const std::function<void(const std::string &line)> &restart) {
    auto reading = readRenderEditCompletion(response);
    if (reading.restart_required && restart) restart(reading.restart_line);
    return reading;
}

} // namespace PelicanStudio
