#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// F-3. What the engine already says about a shader that did not compile, read
// back into things Studio can put where the author is looking. Nothing here
// asks the engine anything: these are pure readings of text the engine sent.

// One "Shader compile failed: <path>\n<log>" the engine wrote
// (shaderlibrary.cpp). A message can carry several, joined by "; ", and can
// carry them after a prefix of its own.
struct ShaderCompileFailure {
    std::string path;
    std::string log;
    // The first non-empty log line, or the header when the log is empty: what
    // a tooltip or a one-line notice shows.
    std::string first_line;

    bool operator==(const ShaderCompileFailure &) const = default;
};

bool containsShaderCompileFailure(std::string_view message);
std::vector<ShaderCompileFailure> parseShaderCompileFailures(
    std::string_view message);

// A `<file>.<stage ext>:<line>` a compile log names, with where it sits in the
// log so a view can emphasise exactly that span.
struct ShaderLogLocation {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string path;
    int line = 0;

    bool operator==(const ShaderLogLocation &) const = default;
};

std::vector<ShaderLogLocation> shaderLogLocations(std::string_view log);

// get_status -> reload.runtime["pelican.shaders"], the participant the file
// watcher's shader transactions report to (reloadservice.cpp).
struct ShaderReloadReading {
    bool reported = false;
    // details.enabled: false while the engine's reload gate is shut. Under
    // Studio's `--rpc` launch the gate reports "rpc driver".
    bool enabled = false;
    std::string gate_reason;
    std::uint64_t attempted = 0;
    std::uint64_t applied = 0;
    std::uint64_t failed = 0;
    std::string last_error;
    // VI-3a: "shader_reload_refused@1" when the last reload was refused for a
    // pass whose inputs the new shader no longer matches (VI-1b). The Frame
    // Plan canvas says that on the pass itself.
    std::string error_code;

    bool operator==(const ShaderReloadReading &) const = default;
};

ShaderReloadReading readShaderReloadStatus(const nlohmann::json &status);

// Each node's shader sources, as physical files. Studio resolves them from the
// frame plan's source_open_ref the same way its Open button does.
using NodeShaderSources =
    std::map<std::string, std::vector<std::filesystem::path>, std::less<>>;

struct ShaderReloadMarks {
    // node name -> the first line of the log of the failure that hit it
    std::map<std::string, std::string, std::less<>> nodes;
    // first lines of failures no drawn node reads
    std::vector<std::string> unmatched;

    bool operator==(const ShaderReloadMarks &) const = default;
};

ShaderReloadMarks shaderReloadMarks(const ShaderReloadReading &reading,
                                    const NodeShaderSources &sources);

// The engine writes the path it compiled; Studio holds the path it resolved.
// Same file when the filesystem says so, else when the normalised spellings
// agree (case-insensitively on Windows).
bool sameShaderSource(const std::filesystem::path &left,
                      const std::filesystem::path &right);

} // namespace PelicanStudio
