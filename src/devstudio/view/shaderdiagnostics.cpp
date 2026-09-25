#include "shaderdiagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <system_error>

namespace PelicanStudio {
namespace {

constexpr std::string_view CompileFailureHeader = "Shader compile failed: ";
constexpr std::string_view ShaderReloadParticipant = "pelican.shaders";

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string firstLine(std::string_view log) {
    std::size_t start = 0;
    while (start <= log.size()) {
        const auto end = log.find('\n', start);
        const auto line = trim(log.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start));
        if (!line.empty()) return std::string{line};
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

std::string normalisedSpelling(const std::filesystem::path &path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    auto text = absolute.lexically_normal().generic_string();
#ifdef _WIN32
    std::ranges::transform(text, text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return text;
}

std::uint64_t counter(const nlohmann::json &object, const char *key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_unsigned()
               ? found->get<std::uint64_t>()
               : 0;
}

} // namespace

bool containsShaderCompileFailure(std::string_view message) {
    return message.find(CompileFailureHeader) != std::string_view::npos;
}

std::vector<ShaderCompileFailure> parseShaderCompileFailures(
    std::string_view message) {
    std::vector<ShaderCompileFailure> result;
    auto at = message.find(CompileFailureHeader);
    while (at != std::string_view::npos) {
        const auto path_start = at + CompileFailureHeader.size();
        const auto next = message.find(CompileFailureHeader, path_start);
        const auto segment_end =
            next == std::string_view::npos ? message.size() : next;
        auto segment =
            message.substr(path_start, segment_end - path_start);
        // Two failures joined by the reload service's "; " separator: the
        // separator belongs to neither log.
        if (next != std::string_view::npos && segment.ends_with("; "))
            segment.remove_suffix(2);
        const auto newline = segment.find('\n');
        ShaderCompileFailure failure;
        failure.path = std::string{trim(segment.substr(0, newline))};
        if (newline != std::string_view::npos) {
            auto log = segment.substr(newline + 1);
            while (!log.empty() &&
                   (log.back() == '\n' || log.back() == '\r'))
                log.remove_suffix(1);
            failure.log = std::string{log};
        }
        failure.first_line = firstLine(failure.log);
        if (failure.first_line.empty())
            failure.first_line =
                std::string{CompileFailureHeader} + failure.path;
        result.push_back(std::move(failure));
        at = next;
    }
    return result;
}

std::vector<ShaderLogLocation> shaderLogLocations(std::string_view log) {
    // shaderc reports "<file>:<line>: error: ..."; the file may hold spaces
    // and a drive colon, so the stage extension is what ends it.
    static const std::regex location{
        R"((.*?\.(?:frag|vert|comp|glsl|rgen|rmiss|rchit|rahit|rint|rcall)):([0-9]+):)",
        std::regex::icase};
    std::vector<ShaderLogLocation> result;
    std::size_t start = 0;
    while (start < log.size()) {
        const auto end = log.find('\n', start);
        const auto line_end = end == std::string_view::npos ? log.size() : end;
        const std::string line{log.substr(start, line_end - start)};
        std::smatch match;
        if (std::regex_search(line, match, location)) {
            const auto path_offset = static_cast<std::size_t>(match.position(1));
            const auto leading = line.find_first_not_of(" \t", path_offset);
            const auto skipped =
                leading == std::string::npos ? 0 : leading - path_offset;
            result.push_back(ShaderLogLocation{
                .offset = start + path_offset + skipped,
                .length = static_cast<std::size_t>(match.length(1)) +
                          1 + static_cast<std::size_t>(match.length(2)) -
                          skipped,
                .path = match.str(1).substr(skipped),
                .line = std::stoi(match.str(2)),
            });
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

ShaderReloadReading readShaderReloadStatus(const nlohmann::json &status) {
    ShaderReloadReading reading;
    if (!status.is_object()) return reading;
    const auto reload = status.find("reload");
    if (reload == status.end() || !reload->is_object()) return reading;
    const auto runtime = reload->find("runtime");
    if (runtime == reload->end() || !runtime->is_object()) return reading;
    const auto shaders = runtime->find(std::string{ShaderReloadParticipant});
    if (shaders == runtime->end() || !shaders->is_object()) return reading;
    reading.reported = true;
    reading.attempted = counter(*shaders, "attempted");
    reading.applied = counter(*shaders, "applied");
    reading.failed = counter(*shaders, "failed");
    if (const auto error = shaders->find("last_error");
        error != shaders->end() && error->is_string())
        reading.last_error = error->get<std::string>();
    if (const auto code = shaders->find("error_code");
        code != shaders->end() && code->is_string())
        reading.error_code = code->get<std::string>();
    if (const auto details = shaders->find("details");
        details != shaders->end() && details->is_object()) {
        reading.enabled = details->value("enabled", false);
        if (const auto reason = details->find("gate_reason");
            reason != details->end() && reason->is_string())
            reading.gate_reason = reason->get<std::string>();
    }
    return reading;
}

bool sameShaderSource(const std::filesystem::path &left,
                      const std::filesystem::path &right) {
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error)
        return true;
    return normalisedSpelling(left) == normalisedSpelling(right);
}

ShaderReloadMarks shaderReloadMarks(const ShaderReloadReading &reading,
                                    const NodeShaderSources &sources) {
    ShaderReloadMarks marks;
    if (reading.last_error.empty()) return marks;
    auto failures = parseShaderCompileFailures(reading.last_error);
    if (failures.empty()) {
        // A shader reload that failed for a reason other than a compile (a
        // pipeline candidate, a lost dependency) names no file to hang on.
        marks.unmatched.push_back(firstLine(reading.last_error));
        return marks;
    }
    for (const auto &failure : failures) {
        bool owned = false;
        for (const auto &[node, paths] : sources) {
            const bool reads = std::ranges::any_of(
                paths, [&](const std::filesystem::path &path) {
                    return sameShaderSource(
                        std::filesystem::path{failure.path}, path);
                });
            if (!reads) continue;
            owned = true;
            marks.nodes.try_emplace(node, failure.first_line);
        }
        if (!owned) marks.unmatched.push_back(failure.first_line);
    }
    return marks;
}

} // namespace PelicanStudio
