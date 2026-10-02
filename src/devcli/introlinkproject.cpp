#include "introlink.hpp"

#include "processrunner.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Pelican::DevCli::IntroLink {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

std::string utf8(const fs::path &path) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

std::string readFile(const fs::path &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) throw std::runtime_error("failed to open " + utf8(path));
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<fs::path> sortedEntries(const fs::path &dir) {
    std::vector<fs::path> entries;
    std::error_code error;
    if (!fs::is_directory(dir, error)) return entries;
    for (const auto &entry : fs::directory_iterator{dir}) {
        if (entry.is_regular_file()) entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

[[noreturn]] void curveInvalid(const std::string &document, const std::string &node,
                               const std::string &why) {
    throw Error("intro_link.curve_invalid@1",
                "curve \"" + node + "\" in " + document + ": " + why);
}

float finiteFloat(const Json &value, const std::string &document, const std::string &node,
                  const char *field) {
    if (!value.is_number()) curveInvalid(document, node, std::string{field} + " must be a number");
    const auto result = static_cast<float>(value.get<double>());
    if (!std::isfinite(result)) {
        curveInvalid(document, node, std::string{field} + " does not fit a 32-bit float");
    }
    return result;
}

std::vector<Key> readKeys(const Json &node, const std::string &document, const std::string &id) {
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) curveInvalid(document, id, "keys are empty");
    const auto keys = parameters->find("keys");
    if (keys == parameters->end() || !keys->is_array() || keys->empty()) {
        curveInvalid(document, id, "keys are empty");
    }
    std::vector<Key> result;
    for (const auto &authored : *keys) {
        if (!authored.is_object()) curveInvalid(document, id, "a key is not an object");
        const auto time = authored.find("time");
        const auto value = authored.find("value");
        if (time == authored.end() || value == authored.end()) {
            curveInvalid(document, id, "a key lacks time or value");
        }
        Key key;
        key.time = finiteFloat(*time, document, id, "time");
        key.value = finiteFloat(*value, document, id, "value");
        if (const auto found = authored.find("in_tangent"); found != authored.end()) {
            key.in_tangent = finiteFloat(*found, document, id, "in_tangent");
        }
        if (const auto found = authored.find("out_tangent"); found != authored.end()) {
            key.out_tangent = finiteFloat(*found, document, id, "out_tangent");
        }
        std::string interpolation = "linear";
        if (const auto found = authored.find("interpolation"); found != authored.end()) {
            if (!found->is_string()) curveInvalid(document, id, "interpolation must be a string");
            interpolation = found->get<std::string>();
        }
        if (interpolation == "linear") key.interpolation = kLinear;
        else if (interpolation == "step") key.interpolation = kStep;
        else if (interpolation == "cubic") key.interpolation = kCubic;
        else curveInvalid(document, id, "unknown interpolation \"" + interpolation + "\"");
        // Checked after the float conversion: two distinct doubles that land
        // on one float would make a zero-width segment in pil_eval.
        if (!result.empty() && !(result.back().time < key.time)) {
            curveInvalid(document, id, "key times must be strictly increasing");
        }
        result.push_back(key);
    }
    return result;
}

void addTrack(std::map<std::string, Track> &tracks, const Json &node, const std::string &document) {
    const auto id_field = node.find("id");
    const std::string id = id_field != node.end() && id_field->is_string() ? id_field->get<std::string>() : "";
    if (!isIdentifier(id)) {
        throw Error("intro_link.name_not_identifier@1",
                    "track \"" + id + "\" in " + document + " is not a C identifier");
    }
    if (const auto existing = tracks.find(id); existing != tracks.end()) {
        throw Error("intro_link.name_duplicate@1", "track \"" + id + "\" is defined in " +
                                                       existing->second.document + " and " + document);
    }
    tracks.emplace(id, Track{document, readKeys(node, document, id)});
}

void collectNodes(std::map<std::string, Track> &tracks, const Json &nodes, const std::string &document) {
    if (!nodes.is_array()) return;
    for (const auto &node : nodes) {
        if (node.is_object() && node.value("op", std::string{}) == "value.curve") {
            addTrack(tracks, node, document);
        }
    }
}

std::string stageOf(const fs::path &path) {
    const auto extension = path.extension().string();
    if (extension == ".vert") return "vert";
    if (extension == ".frag") return "frag";
    if (extension == ".comp") return "comp";
    return {};
}

std::string floatLiteral(float value) {
    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) throw std::runtime_error("failed to format a float");
    std::string text{buffer, end};
    if (text.find_first_of(".e") == std::string::npos) text += ".0";
    return text + "f";
}

std::optional<fs::path> executableIn(const fs::path &dir) {
#ifdef _WIN32
    const fs::path candidate = dir / "glslc.exe";
#else
    const fs::path candidate = dir / "glslc";
#endif
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) return candidate;
    return std::nullopt;
}

std::optional<std::string> environment(const char *name) {
#ifdef _WIN32
    char *buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char *value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string{value};
#endif
}

unsigned long currentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// IL-3: every shader is built with glslc -Os. When spirv-opt sits next to
// glslc it is also built "no-inline": glslc -O0, then spirv-opt's own -Os
// list *without* --inline-entry-points-exhaustive, so a function called from
// several places stays one function (glslc -Os inlines it at every call site),
// then the constants are unified and the ids compacted. The smaller module is
// used (a tie keeps -Os): no-inline wins on raymarchers that call a distance
// function from several places, -Os on small shaders whose helpers are called
// once. serve and bake both come through compileShader, so they agree.
const char *const kSizePasses[] = {
    "--strip-debug", "--strip-nonsemantic", "--wrap-opkill", "--eliminate-dead-branches", "--merge-return",
    "--eliminate-dead-functions", "--private-to-local", "--scalar-replacement=0", "--ssa-rewrite", "--ccp",
    "--eliminate-dead-branches", "--simplify-instructions", "--scalar-replacement=0",
    "--eliminate-local-single-store", "--if-conversion", "--simplify-instructions",
    "--eliminate-dead-code-aggressive", "--eliminate-dead-branches", "--merge-blocks",
    "--convert-local-access-chains", "--eliminate-local-single-block", "--eliminate-dead-code-aggressive",
    "--copy-propagate-arrays", "--vector-dce", "--eliminate-dead-inserts", "--eliminate-local-single-store",
    "--merge-blocks", "--ssa-rewrite", "--redundancy-elimination", "--simplify-instructions",
    "--eliminate-dead-code-aggressive", "--cfg-cleanup", "--unify-const", "--eliminate-dead-const",
    "--remove-duplicates", "--compact-ids",
};

std::optional<fs::path> spirvOptBeside(const fs::path &glslc) {
#ifdef _WIN32
    const auto candidate = glslc.parent_path() / "spirv-opt.exe";
#else
    const auto candidate = glslc.parent_path() / "spirv-opt";
#endif
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) return candidate;
    return std::nullopt;
}

// A uniquely named file in the temp directory, removed when it goes out of scope.
struct TemporaryFile {
    fs::path path;
    TemporaryFile() {
        static std::atomic<unsigned> counter{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("pelican_intro_link_" + std::to_string(currentProcessId()) + "_" +
                                            std::to_string(tick) + "_" + std::to_string(counter++) + ".spv");
    }
    ~TemporaryFile() {
        std::error_code error;
        fs::remove(path, error);
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;
};

// Runs glslc or spirv-opt with the module on stdout. Returns the module, or
// nothing with the reason added to `log` (the tool's stderr always is).
std::optional<std::string> runSpirvTool(const fs::path &executable, const std::string &name,
                                        std::vector<fs::path> arguments, std::string &log) {
    ProcessOptions options;
    options.executable = executable;
    options.arguments = std::move(arguments);
    options.timeout = std::chrono::milliseconds{60000};
    options.name = name;
    auto process = runProcess(options);
    log += process.stderr_text;
    if (process.timed_out) {
        log += name + " timed out\n";
        return std::nullopt;
    }
    if (process.exit_code != 0) {
        if (name != "glslc") log += name + " exited with " + std::to_string(process.exit_code) + "\n";
        return std::nullopt;
    }
    if (process.stdout_text.size() < 20 || process.stdout_text.size() % 4 != 0) {
        log += name + " wrote " + std::to_string(process.stdout_text.size()) + " bytes, not a SPIR-V module\n";
        return std::nullopt;
    }
    return std::move(process.stdout_text);
}

} // namespace

Error::Error(std::string code, const std::string &detail)
    : std::runtime_error(code + ": " + detail), code_(std::move(code)) {}

fs::path resolveProjectRoot(const std::string &argument) {
    auto path = fs::path{argument};
    if (path.is_relative()) path = fs::current_path() / path;
    std::error_code error;
    if (fs::is_regular_file(path, error) && path.filename() == "project.json") {
        return path.parent_path().lexically_normal();
    }
    if (!fs::is_regular_file(path / "project.json", error)) {
        throw std::runtime_error("<project> must be a directory holding project.json, or project.json: " +
                                 utf8(path));
    }
    return path.lexically_normal();
}

fs::path resolveShadersDir(const fs::path &root, const std::string &argument) {
    fs::path path{argument};
    return (path.is_absolute() ? path : root / path).lexically_normal();
}

bool isIdentifier(const std::string &name) {
    if (name.empty()) return false;
    const auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (!alpha(name.front())) return false;
    return std::all_of(name.begin(), name.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

std::string relativeName(const fs::path &root, const fs::path &path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return utf8(path);
    return utf8(relative);
}

std::map<std::string, Track> collectTracks(const fs::path &root) {
    std::map<std::string, Track> tracks;
    for (const auto &path : sortedEntries(root / "passes")) {
        if (path.extension() != ".json") continue;
        const auto document = relativeName(root, path);
        Json json;
        try {
            json = Json::parse(readFile(path));
        } catch (const Json::exception &error) {
            throw std::runtime_error("failed to read graph document " + document + ": " + error.what());
        }
        if (!json.is_object() || json.value("schema", std::string{}) != "pelican.graph") continue;
        if (const auto nodes = json.find("nodes"); nodes != json.end()) collectNodes(tracks, *nodes, document);
        if (const auto definitions = json.find("definitions");
            definitions != json.end() && definitions->is_object()) {
            for (const auto &[name, definition] : definitions->items()) {
                (void)name;
                if (!definition.is_object()) continue;
                if (const auto nodes = definition.find("nodes"); nodes != definition.end()) {
                    collectNodes(tracks, *nodes, document);
                }
            }
        }
    }
    return tracks;
}

std::map<std::string, ShaderSource> collectShaders(const fs::path &root, const fs::path &shaders_dir) {
    std::map<std::string, ShaderSource> shaders;
    for (const auto &path : sortedEntries(shaders_dir)) {
        const auto stage = stageOf(path);
        if (stage.empty()) continue;
        const auto relative = relativeName(root, path);
        const auto stem = path.stem().string();
        if (!isIdentifier(stem)) {
            throw Error("intro_link.name_not_identifier@1",
                        "shader \"" + stem + "_" + stage + "\" from " + relative + " is not a C identifier");
        }
        const auto name = stem + "_" + stage;
        if (const auto existing = shaders.find(name); existing != shaders.end()) {
            throw Error("intro_link.name_duplicate@1", "shader \"" + name + "\" comes from " +
                                                           existing->second.relative + " and " + relative);
        }
        shaders.emplace(name, ShaderSource{path, relative});
    }
    return shaders;
}

fs::path findGlslc(const std::string &explicit_path) {
    std::vector<std::string> searched;
    std::error_code error;
    if (!explicit_path.empty()) {
        fs::path path{explicit_path};
        if (path.is_relative()) path = fs::current_path() / path;
        if (fs::is_regular_file(path, error)) return path;
        throw Error("intro_link.glslc_not_found@1", "--glslc " + utf8(path) + " is not a file");
    }
    if (const auto sdk = environment("VULKAN_SDK"); sdk && !sdk->empty()) {
#ifdef _WIN32
        const std::vector<const char *> bins{"Bin"};
#else
        const std::vector<const char *> bins{"Bin", "bin"};
#endif
        for (const char *bin : bins) {
            const auto dir = fs::path{*sdk} / bin;
            if (const auto found = executableIn(dir)) return *found;
            searched.push_back(utf8(dir));
        }
    } else {
        searched.push_back("$VULKAN_SDK (unset)");
    }
    if (const auto path_env = environment("PATH")) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::string_view rest{*path_env};
        while (!rest.empty()) {
            const auto end = rest.find(separator);
            const auto entry = rest.substr(0, end);
            if (!entry.empty()) {
                if (const auto found = executableIn(fs::path{std::string{entry}})) return *found;
            }
            if (end == std::string_view::npos) break;
            rest.remove_prefix(end + 1);
        }
        searched.push_back("PATH");
    } else {
        searched.push_back("PATH (unset)");
    }
    std::string list;
    for (const auto &place : searched) list += (list.empty() ? "" : ", ") + place;
    throw Error("intro_link.glslc_not_found@1", "searched --glslc, " + list);
}

CompileResult compileShader(const fs::path &glslc, const fs::path &shaders_dir, const ShaderSource &shader) {
    const auto compile = [&](const char *level, std::string &log) {
        return runSpirvTool(glslc, "glslc",
                            {"--target-env=vulkan1.2", level, "-I", shaders_dir, "-I", shaders_dir / "include",
                             shader.path, "-o", "-"},
                            log);
    };
    CompileResult result;
    auto chosen = compile("-Os", result.log);
    result.variant = "Os";
    const auto optimizer = spirvOptBeside(glslc);
    if (chosen && optimizer) {
        // The no-inline build: glslc -O0, then spirv-opt through a temporary
        // file (runProcess gives the child no stdin). Its messages repeat the
        // -Os build's or explain a spirv-opt failure, in which case -Os stands.
        std::string ignored;
        if (const auto unoptimised_module = compile("-O0", ignored)) {
            const TemporaryFile unoptimised;
            {
                std::ofstream file{unoptimised.path, std::ios::binary | std::ios::trunc};
                file << *unoptimised_module;
                if (!file) throw std::runtime_error("failed to write " + utf8(unoptimised.path));
            }
            std::vector<fs::path> arguments{"--target-env=vulkan1.2"};
            arguments.insert(arguments.end(), std::begin(kSizePasses), std::end(kSizePasses));
            arguments.insert(arguments.end(), {unoptimised.path, "-o", "-"});
            auto no_inline = runSpirvTool(*optimizer, "spirv-opt", arguments, ignored);
            // Strictly smaller wins; a tie keeps -Os.
            if (no_inline && no_inline->size() < chosen->size()) {
                chosen = std::move(no_inline);
                result.variant = "no-inline";
            }
        }
    }
    if (!chosen) return result;
    const auto &bytes = *chosen;
    result.words.resize(bytes.size() / 4);
    for (std::size_t i = 0; i < result.words.size(); ++i) {
        const auto *p = reinterpret_cast<const unsigned char *>(bytes.data()) + i * 4;
        result.words[i] = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                          (std::uint32_t{p[3]} << 24);
    }
    result.ok = true;
    return result;
}

std::string renderBakedHeader(const std::map<std::string, Track> &tracks,
                              const std::map<std::string, ShaderBinary> &shaders) {
    std::ostringstream out;
    out << "/* Generated by pelican_cli intro-link bake. Do not edit. */\n"
           "/* Include this before pelican_intro_link.h with PELICAN_INTRO_LINK_BAKED defined. */\n"
           "#ifndef PELICAN_INTRO_LINK_BAKED_DATA_H\n"
           "#define PELICAN_INTRO_LINK_BAKED_DATA_H\n"
           "\n"
           "#include <stdint.h>\n"
           "\n"
           "#define PIL_BAKED_DATA_INCLUDED 1\n"
           "\n"
           "#ifndef PIL_KEY_DEFINED\n"
           "#define PIL_KEY_DEFINED\n"
           "typedef struct pil_key {\n"
           "    float time;\n"
           "    float value;\n"
           "    float in_tangent;\n"
           "    float out_tangent;\n"
           "    uint32_t interpolation; /* 0 linear, 1 step, 2 cubic */\n"
           "} pil_key;\n"
           "#endif\n"
           "\n";

    out << "#define PIL_BAKED_TRACK_COUNT " << tracks.size() << "\n";
    if (!tracks.empty()) {
        out << "enum pil_baked_track_id {\n";
        std::size_t index = 0;
        for (const auto &[name, track] : tracks) {
            out << "    PIL_TRACK_" << name << " = " << index++ << ",\n";
        }
        out << "};\n";
        out << "static const pil_key pil_baked_keys[] = {\n";
        for (const auto &[name, track] : tracks) {
            out << "    /* " << name << ": " << track.document << " */\n";
            for (const auto &key : track.keys) {
                out << "    {" << floatLiteral(key.time) << ", " << floatLiteral(key.value) << ", "
                    << floatLiteral(key.in_tangent) << ", " << floatLiteral(key.out_tangent) << ", "
                    << key.interpolation << "u},\n";
            }
        }
        out << "};\n";
        out << "static const uint32_t pil_baked_track_first[] = {";
        std::size_t first = 0;
        bool comma = false;
        for (const auto &[name, track] : tracks) {
            out << (comma ? ", " : "") << first << "u";
            first += track.keys.size();
            comma = true;
        }
        out << "};\n";
        out << "static const uint32_t pil_baked_track_count[] = {";
        comma = false;
        for (const auto &[name, track] : tracks) {
            out << (comma ? ", " : "") << track.keys.size() << "u";
            comma = true;
        }
        out << "};\n";
    }
    out << "\n";

    out << "#define PIL_BAKED_SHADER_COUNT " << shaders.size() << "\n";
    if (!shaders.empty()) {
        out << "enum pil_baked_shader_id {\n";
        std::size_t index = 0;
        for (const auto &[name, shader] : shaders) {
            out << "    PIL_SHADER_" << name << " = " << index++ << ",\n";
        }
        out << "};\n";
        for (const auto &[name, shader] : shaders) {
            out << "static const uint32_t pil_baked_shader_" << name << "[] = {\n";
            for (std::size_t i = 0; i < shader.words.size(); ++i) {
                out << (i % 8 == 0 ? "    " : " ") << "0x" << std::hex << std::setw(8) << std::setfill('0')
                    << shader.words[i] << std::dec << "u,";
                if (i % 8 == 7 || i + 1 == shader.words.size()) out << "\n";
            }
            out << "};\n";
        }
        out << "static const uint32_t *const pil_baked_shader_words[] = {";
        bool comma = false;
        for (const auto &[name, shader] : shaders) {
            out << (comma ? ", " : "") << "pil_baked_shader_" << name;
            comma = true;
        }
        out << "};\n";
        out << "static const uint32_t pil_baked_shader_word_count[] = {";
        comma = false;
        for (const auto &[name, shader] : shaders) {
            out << (comma ? ", " : "") << shader.words.size() << "u";
            comma = true;
        }
        out << "};\n";
    }
    out << "\n#endif /* PELICAN_INTRO_LINK_BAKED_DATA_H */\n";
    return out.str();
}

} // namespace Pelican::DevCli::IntroLink
