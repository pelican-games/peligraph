#include "fullscreenshadertemplate.hpp"

#include "battery/embed.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <system_error>

namespace PelicanStudio {
namespace {

bool shaderNameCharacter(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_' || value == '-';
}

void replaceAll(std::string &text, std::string_view token,
                std::string_view value) {
    for (std::size_t at = text.find(token); at != std::string::npos;
         at = text.find(token, at + value.size())) {
        text.replace(at, token.size(), value);
    }
}

// A GLSL identifier for one input: `in_` and the resource name, with every
// other character folded to '_' and no run of two underscores (GLSL reserves
// those). The prefix keeps a resource called `input` or `texture` from
// colliding with a keyword or a built-in.
std::string inputIdentifier(std::string_view resource,
                            std::set<std::string> &taken) {
    std::string history_free{resource};
    const auto at = history_free.find('@');
    std::string suffix;
    if (at != std::string::npos) {
        suffix = "_" + history_free.substr(at + 1);
        history_free.erase(at);
    }
    std::string raw = "in_" + history_free + suffix;
    std::string folded;
    folded.reserve(raw.size());
    for (const char value : raw) {
        const auto byte = static_cast<unsigned char>(value);
        const char next = std::isalnum(byte) != 0 ? value : '_';
        if (next == '_' && !folded.empty() && folded.back() == '_') continue;
        folded.push_back(next);
    }
    while (folded.size() > 3 && folded.back() == '_') folded.pop_back();
    std::string identifier = folded;
    for (int index = 2; !taken.insert(identifier).second; ++index)
        identifier = folded + "_" + std::to_string(index);
    return identifier;
}

std::string shippedTemplate() {
    std::string text = b::embed<"fullscreen_pass.frag.in">().str();
    // The checkout decides the resource's line ends; the shader this writes
    // does not inherit them.
    std::erase(text, '\r');
    return text;
}

} // namespace

std::string fullscreenFragmentShaderTemplate(
    std::string_view pass_name,
    std::span<const FullscreenShaderTemplateInput> inputs) {
    std::set<std::string> taken;
    std::string declarations;
    std::string body;
    std::optional<std::size_t> first_image;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto &input = inputs[index];
        const std::string number = std::to_string(index);
        if (!input.image) {
            declarations += "// input " + number + " (" + input.resource +
                            ") is a buffer: declare it yourself at set "
                            "PELICAN_SET_PASS_INPUT, binding " +
                            number + ".\n";
            continue;
        }
        if (index >= FullscreenShaderTemplateMacroInputs) {
            declarations += "// input " + number + " (" + input.resource +
                            "): pelican_view.glsl declares inputs 0-7 only; "
                            "declare binding " +
                            number + " yourself.\n";
            continue;
        }
        const std::string identifier =
            inputIdentifier(input.resource, taken);
        declarations += "PELICAN_DECLARE_INPUT_" + number + "(" +
                        identifier + ");  // " + input.resource + "\n";
        body += "    vec4 value" + number + " = PELICAN_TEXTURE_2D_" +
                number + "(" + identifier + ", inUV);\n";
        if (!first_image) first_image = index;
    }
    if (first_image) {
        body += "    outColor = value" + std::to_string(*first_image) + ";";
    } else {
        body += "    // Nothing to read yet: a gradient across the screen, so "
                "the pass shows it runs.\n"
                "    outColor = vec4(inUV, 0.0, 1.0);";
    }
    std::string text = shippedTemplate();
    replaceAll(text, "@PASS_NAME@", pass_name);
    replaceAll(text, "@INPUT_DECLARATIONS@", declarations);
    replaceAll(text, "@MAIN_BODY@", body);
    return text;
}

bool isValidShaderTemplateName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '-') return false;
    return std::ranges::all_of(name, shaderNameCharacter);
}

std::string suggestedShaderTemplateName(std::string_view pass_name) {
    std::string result;
    result.reserve(pass_name.size());
    for (const char value : pass_name)
        result.push_back(shaderNameCharacter(value) ? value : '_');
    while (!result.empty() && result.front() == '-') result.erase(0, 1);
    if (result.find_first_not_of('_') == std::string::npos) return {};
    return isValidShaderTemplateName(result) ? result : std::string{};
}

std::string shaderTemplateReference(std::string_view name) {
    return "project://shaders/" + std::string{name};
}

std::filesystem::path shaderTemplatePath(
    const std::filesystem::path &project_root, std::string_view name) {
    if (!isValidShaderTemplateName(name)) {
        throw std::invalid_argument(
            "a shader name is letters, digits, '_' and '-': " +
            std::string{name});
    }
    return project_root / "shaders" / (std::string{name} + ".frag");
}

ShaderTemplateWrite writeShaderTemplate(
    const std::filesystem::path &project_root, std::string_view name,
    std::string_view contents) {
    ShaderTemplateWrite result{
        .path = shaderTemplatePath(project_root, name),
        .reference = shaderTemplateReference(name),
    };
    std::error_code error;
    // A file the author already has is theirs: it is offered for opening,
    // never replaced by the template.
    if (std::filesystem::exists(result.path, error)) return result;
    std::filesystem::create_directories(result.path.parent_path(), error);
    if (error) {
        throw std::runtime_error("could not create " +
                                 result.path.parent_path().string() + ": " +
                                 error.message());
    }
    std::ofstream output{result.path, std::ios::binary};
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("could not write " + result.path.string());
    }
    result.created = true;
    return result;
}

} // namespace PelicanStudio
