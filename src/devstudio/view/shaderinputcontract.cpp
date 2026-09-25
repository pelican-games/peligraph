#include "shaderinputcontract.hpp"

#include "fullscreenshadertemplate.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <system_error>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string textOr(const Json &value, const char *key) {
    if (!value.is_object()) return {};
    const auto found = value.find(key);
    return found != value.end() && found->is_string()
               ? found->get<std::string>()
               : std::string{};
}

// The refusal Q-14 is about: the engine's fullscreen pass input contract
// (fullscreenpasscontainer.cpp, requireInputBindings). The other "does not
// match shader reflection" sentences -- a material's descriptors, a raster
// pass's vertex layout -- are not about a pass's input list.
bool fullscreenInputContractMessage(std::string_view message) {
    return message.find("Fullscreen pass input") != std::string_view::npos &&
           message.find("does not match shader reflection") !=
               std::string_view::npos;
}

// "(shader declares N pass inputs)", the engine's planned tail (Q-14e).
std::optional<std::size_t> declaredFromMessage(const std::string &message) {
    static const std::regex pattern{R"(shader declares (\d+) pass input)"};
    std::smatch match;
    if (!std::regex_search(message, match, pattern)) return std::nullopt;
    try {
        return static_cast<std::size_t>(std::stoul(match[1].str()));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::string stemOf(std::string_view fragment) {
    std::string_view rest = fragment;
    if (const auto scheme = rest.find("://"); scheme != std::string_view::npos)
        rest.remove_prefix(scheme + 3);
    if (const auto slash = rest.find_last_of('/'); slash != std::string_view::npos)
        rest.remove_prefix(slash + 1);
    if (const auto dot = rest.find_last_of('.'); dot != std::string_view::npos &&
                                                 dot != 0)
        rest = rest.substr(0, dot);
    return rest.empty() ? std::string{fragment} : std::string{rest};
}

std::string_view trimmed(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
        line.remove_prefix(1);
    return line;
}

} // namespace

std::vector<ShaderInputContractAction> shaderInputContractActions() {
    return {
        {std::string{ShaderInputContractMakePassThrough},
         "Make a pass-through shader"},
        {std::string{ShaderInputContractOpenShader}, "Open the shader"},
    };
}

std::string shaderInputContractLine(std::string_view pass,
                                    std::string_view fragment,
                                    std::optional<std::size_t> declared) {
    std::string line = std::string{pass} + "'s shader (" + stemOf(fragment) + ")";
    if (!declared)
        line += " does not read the inputs the pass lists.";
    else if (*declared == 0)
        line += " reads no inputs.";
    else if (*declared == 1)
        line += " reads only 1 input.";
    else
        line += " reads only " + std::to_string(*declared) + " inputs.";
    line += " To change its inputs, change the shader too.";
    return line;
}

ShaderInputContractNotice shaderInputContractNotice(
    const Json &error, std::string_view pass, std::string_view fragment,
    std::optional<std::size_t> declared_in_file) {
    ShaderInputContractNotice notice;
    if (!error.is_object()) return notice;
    const auto code = textOr(error, "code");
    if (!code.empty() && code != "render_pipeline_preflight_failed")
        return notice;
    const auto message = textOr(error, "message");
    const auto structured = error.find("shader_input_mismatch");
    const bool has_structured =
        structured != error.end() && structured->is_object();
    if (!has_structured && !fullscreenInputContractMessage(message))
        return notice;

    notice.matched = true;
    notice.pass = std::string{pass};
    notice.fragment = std::string{fragment};
    if (has_structured) {
        // Q-14e: the engine's own account of the mismatch.
        if (const auto named = textOr(*structured, "pass"); !named.empty())
            notice.pass = named;
        if (const auto named = textOr(*structured, "fragment"); !named.empty())
            notice.fragment = named;
        notice.resource = textOr(*structured, "resource");
        const auto inputs = structured->find("shader_pass_inputs");
        if (inputs != structured->end() && inputs->is_array())
            notice.declared = inputs->size();
    }
    if (!notice.declared) notice.declared = declaredFromMessage(message);
    if (!notice.declared) notice.declared = declared_in_file;
    notice.fragment_stem = stemOf(notice.fragment);
    notice.line = shaderInputContractLine(notice.pass, notice.fragment,
                                          notice.declared);
    notice.actions = shaderInputContractActions();
    notice.can_open = notice.fragment.starts_with("project://");
    return notice;
}

std::vector<std::size_t> shaderInputDeclaredIndices(std::string_view source) {
    static const std::regex macro{R"(PELICAN_DECLARE_INPUT_(\d+)\s*\()"};
    static const std::regex manual{
        R"(set\s*=\s*PELICAN_SET_PASS_INPUT\s*,\s*binding\s*=\s*(\d+))"};
    std::set<std::size_t> indices;
    std::size_t start = 0;
    while (start <= source.size()) {
        const auto end = source.find('\n', start);
        std::string_view line = source.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start);
        start = end == std::string_view::npos ? source.size() + 1 : end + 1;
        line = trimmed(line);
        if (line.starts_with("#") || line.starts_with("//")) continue;
        if (const auto comment = line.find("//"); comment != std::string_view::npos)
            line = line.substr(0, comment);
        const std::string text{line};
        for (const auto *pattern : {&macro, &manual}) {
            for (std::sregex_iterator match{text.begin(), text.end(), *pattern}, last;
                 match != last; ++match) {
                try {
                    indices.insert(
                        static_cast<std::size_t>(std::stoul((*match)[1].str())));
                } catch (const std::exception &) {
                }
            }
        }
    }
    return {indices.begin(), indices.end()};
}

std::size_t shaderInputDeclarationCount(std::string_view source) {
    return shaderInputDeclaredIndices(source).size();
}

std::optional<std::filesystem::path> projectFragmentSourcePath(
    const std::filesystem::path &project_root, std::string_view fragment) {
    constexpr std::string_view scheme = "project://";
    if (!fragment.starts_with(scheme)) return std::nullopt;
    const std::filesystem::path relative{
        std::string{fragment.substr(scheme.size())}};
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        return std::nullopt;
    for (const auto &part : relative)
        if (part == "..") return std::nullopt;
    auto path = project_root / relative;
    if (!relative.has_extension()) path += ".frag";
    return path;
}

std::optional<std::size_t> projectFragmentDeclarationCount(
    const std::filesystem::path &project_root, std::string_view fragment) {
    const auto path = projectFragmentSourcePath(project_root, fragment);
    if (!path) return std::nullopt;
    std::ifstream input{*path, std::ios::binary};
    if (!input) return std::nullopt;
    const std::string source{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
    return shaderInputDeclarationCount(source);
}

std::string passThroughShaderName(const std::filesystem::path &project_root,
                                  std::string_view pass) {
    auto base = suggestedShaderTemplateName(pass);
    if (base.empty()) base = "fullscreen_pass";
    base += "_pass_through";
    std::string candidate = base;
    std::error_code error;
    for (int index = 2;
         std::filesystem::exists(shaderTemplatePath(project_root, candidate), error);
         ++index)
        candidate = base + "_" + std::to_string(index);
    return candidate;
}

} // namespace PelicanStudio
