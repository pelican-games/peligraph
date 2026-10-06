#include "rendereditcompletion.hpp"

#include <algorithm>

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
    if (const auto ticket = response.find("ticket");
        ticket != response.end() && ticket->is_string())
        reading.ticket = ticket->get<std::string>();
    reading.committed = reading.status == "committed" && flag("committed");
    if (!reading.committed) return reading;
    // RT-C: read before the restart part, which returns early.
    if (const auto error = response.find("post_commit_error");
        error != response.end())
        reading.post_commit_error = error->is_string() ? error->get<std::string>()
                                                     : "post_commit_error is not a string";
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

std::string renderEditPostCommitErrorLine(const RenderEditCompletion &completion) {
    if (completion.post_commit_error.empty()) return {};
    auto line = "Saved. Applying it in the running engine failed after the commit: " +
                completion.post_commit_error;
    if (completion.restart_required) line += " " + completion.restart_line;
    return line;
}

std::string RenderEditPostCommitErrors::take(const nlohmann::json &response) {
    const auto completion = readRenderEditCompletion(response);
    auto line = renderEditPostCommitErrorLine(completion);
    if (line.empty()) return {};
    const auto key = completion.ticket + "@" + std::to_string(completion.published_generation) + "@" +
                     completion.source_digest;
    if (std::ranges::find(said_, key) != said_.end()) return {};
    said_.push_back(key);
    constexpr std::size_t Kept = 64;
    while (said_.size() > Kept) said_.pop_front();
    return line;
}

} // namespace PelicanStudio
