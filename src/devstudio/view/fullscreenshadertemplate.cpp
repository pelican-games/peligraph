#include "fullscreenshadertemplate.hpp"

#include "battery/embed.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

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

// The checkout decides a resource's line ends; the shader "New..." writes does
// not inherit them.
std::string withoutCarriageReturns(std::string text) {
    std::erase(text, '\r');
    return text;
}

std::string shippedTemplate(FullscreenShaderTemplateKind kind) {
    switch (kind) {
    case FullscreenShaderTemplateKind::deferred_lighting:
        return withoutCarriageReturns(
            b::embed<"fullscreen_lighting_pass.frag.in">().str());
    case FullscreenShaderTemplateKind::tone_map:
        return withoutCarriageReturns(
            b::embed<"fullscreen_tonemap_pass.frag.in">().str());
    case FullscreenShaderTemplateKind::pass_through:
        break;
    }
    return withoutCarriageReturns(
        b::embed<"fullscreen_pass.frag.in">().str());
}

// How a role-reading template reads one role: the local it declares, the
// components it keeps, and what it holds when the pass has no image there.
struct RoleRead {
    FullscreenShaderTemplateRole role;
    std::string_view declaration;
    std::string_view components;
    std::string_view fallback;
};

constexpr std::array deferred_lighting_reads{
    RoleRead{{"albedo"}, "vec3 albedo", ".rgb", "vec3(0.8)"},
    RoleRead{{"normal"}, "vec3 normal_encoded", ".rgb", "vec3(0.5, 0.5, 1.0)"},
    RoleRead{{"material"}, "vec4 material", "", "vec4(0.5, 0.0, 1.0, 0.0)"},
    RoleRead{{"world position"}, "vec3 world_position", ".rgb", "vec3(0.0)"},
    RoleRead{{"emissive"}, "vec3 emissive", ".rgb", "vec3(0.0)"},
    RoleRead{{"ambient occlusion", true}, "float ambient_occlusion", ".r", "1.0"},
};
constexpr std::array tone_map_reads{
    RoleRead{{"scene colour"}, "vec3 scene_color", ".rgb", "vec3(inUV, 0.5)"},
};

std::span<const RoleRead> roleReads(FullscreenShaderTemplateKind kind) {
    switch (kind) {
    case FullscreenShaderTemplateKind::deferred_lighting:
        return deferred_lighting_reads;
    case FullscreenShaderTemplateKind::tone_map:
        return tone_map_reads;
    case FullscreenShaderTemplateKind::pass_through:
        break;
    }
    return {};
}

constexpr std::array deferred_lighting_roles{
    deferred_lighting_reads[0].role, deferred_lighting_reads[1].role,
    deferred_lighting_reads[2].role, deferred_lighting_reads[3].role,
    deferred_lighting_reads[4].role, deferred_lighting_reads[5].role,
};
constexpr std::array tone_map_roles{tone_map_reads[0].role};

// The declarations of every image input, in list order, and the identifier
// each declared input got (none for a buffer or an input past the macros).
struct DeclaredInputs {
    std::string text;
    std::vector<std::optional<std::string>> identifiers;
};

DeclaredInputs declareInputs(
    std::span<const FullscreenShaderTemplateInput> inputs,
    std::span<const RoleRead> reads) {
    DeclaredInputs result;
    std::set<std::string> taken;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto &input = inputs[index];
        const std::string number = std::to_string(index);
        result.identifiers.emplace_back();
        if (!input.image) {
            result.text += "// input " + number + " (" + input.resource +
                           ") is a buffer: declare it yourself at set "
                           "PELICAN_SET_PASS_INPUT, binding " +
                           number + ".\n";
            continue;
        }
        if (index >= FullscreenShaderTemplateMacroInputs) {
            result.text += "// input " + number + " (" + input.resource +
                           "): pelican_view.glsl declares inputs 0-7 only; "
                           "declare binding " +
                           number + " yourself.\n";
            continue;
        }
        const std::string identifier =
            inputIdentifier(input.resource, taken);
        std::string note = input.resource;
        if (!reads.empty()) {
            note += index < reads.size()
                        ? ": " + std::string{reads[index].role.name}
                        : std::string{": not read below"};
        }
        result.text += "PELICAN_DECLARE_INPUT_" + number + "(" + identifier +
                       ");  // " + note + "\n";
        result.identifiers.back() = identifier;
    }
    return result;
}

std::string passThroughBody(
    std::span<const FullscreenShaderTemplateInput> inputs,
    const DeclaredInputs &declared) {
    std::string body;
    std::optional<std::size_t> first_image;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (!declared.identifiers[index]) continue;
        const std::string number = std::to_string(index);
        body += "    vec4 value" + number + " = PELICAN_TEXTURE_2D_" +
                number + "(" + *declared.identifiers[index] + ", inUV);\n";
        if (!first_image) first_image = index;
    }
    if (first_image) {
        body += "    outColor = value" + std::to_string(*first_image) + ";";
    } else {
        body += "    // Nothing to read yet: a gradient across the screen, so "
                "the pass shows it runs.\n"
                "    outColor = vec4(inUV, 0.0, 1.0);";
    }
    return body;
}

// One local per role, read from the input at the role's position, or the
// role's constant when the pass has no image there.
std::string roleReadsText(std::span<const FullscreenShaderTemplateInput> inputs,
                          const DeclaredInputs &declared,
                          std::span<const RoleRead> reads) {
    std::string text;
    for (std::size_t index = 0; index < reads.size(); ++index) {
        const auto &read = reads[index];
        const std::string number = std::to_string(index);
        text += "    " + std::string{read.declaration} + " = ";
        if (index < inputs.size() && declared.identifiers[index]) {
            text += "PELICAN_TEXTURE_2D_" + number + "(" +
                    *declared.identifiers[index] + ", inUV)" +
                    std::string{read.components} + ";\n";
            continue;
        }
        const std::string_view why =
            index < inputs.size() ? "not an image"
            : read.role.optional  ? "optional, not in the input list"
                                  : "not in the input list";
        text += std::string{read.fallback} + ";  // input " + number + " (" +
                std::string{read.role.name} + "): " + std::string{why} + "\n";
    }
    if (!text.empty()) text.pop_back();
    return text;
}

} // namespace

std::string_view fullscreenShaderTemplateKindName(
    FullscreenShaderTemplateKind kind) {
    switch (kind) {
    case FullscreenShaderTemplateKind::deferred_lighting:
        return "deferred_lighting";
    case FullscreenShaderTemplateKind::tone_map:
        return "tone_map";
    case FullscreenShaderTemplateKind::pass_through:
        break;
    }
    return "pass_through";
}

std::optional<FullscreenShaderTemplateKind>
fullscreenShaderTemplateKindFromName(std::string_view name) {
    for (const auto kind : FullscreenShaderTemplateKinds)
        if (fullscreenShaderTemplateKindName(kind) == name) return kind;
    return std::nullopt;
}

std::string_view fullscreenShaderTemplateKindLabel(
    FullscreenShaderTemplateKind kind) {
    switch (kind) {
    case FullscreenShaderTemplateKind::deferred_lighting:
        return "Deferred lighting";
    case FullscreenShaderTemplateKind::tone_map:
        return "Tone map";
    case FullscreenShaderTemplateKind::pass_through:
        break;
    }
    return "Pass-through";
}

std::span<const FullscreenShaderTemplateRole> fullscreenShaderTemplateInputRoles(
    FullscreenShaderTemplateKind kind) {
    switch (kind) {
    case FullscreenShaderTemplateKind::deferred_lighting:
        return deferred_lighting_roles;
    case FullscreenShaderTemplateKind::tone_map:
        return tone_map_roles;
    case FullscreenShaderTemplateKind::pass_through:
        break;
    }
    return {};
}

FullscreenShaderTemplatePassSettings fullscreenShaderTemplatePassSettings(
    FullscreenShaderTemplateKind kind) {
    if (kind == FullscreenShaderTemplateKind::deferred_lighting)
        return {.uses_light_data = true, .push_constants = "camera_position"};
    return {};
}

std::string fullscreenFragmentShaderTemplate(
    std::string_view pass_name,
    std::span<const FullscreenShaderTemplateInput> inputs,
    FullscreenShaderTemplateKind kind) {
    const auto reads = roleReads(kind);
    const auto declared = declareInputs(inputs, reads);
    std::string text = shippedTemplate(kind);
    replaceAll(text, "@PASS_NAME@", pass_name);
    replaceAll(text, "@INPUT_DECLARATIONS@", declared.text);
    if (kind == FullscreenShaderTemplateKind::pass_through) {
        replaceAll(text, "@MAIN_BODY@", passThroughBody(inputs, declared));
    } else {
        replaceAll(text, "@INPUT_READS@",
                   roleReadsText(inputs, declared, reads));
    }
    return text;
}

std::string fullscreenShaderInputSnippet(
    std::span<const FullscreenShaderTemplateInput> inputs, std::size_t first) {
    // Declared over the whole list, so each input keeps the identifier the
    // template would give it; only the lines from `first` on are kept.
    const auto declared = declareInputs(inputs, {});
    std::string declarations;
    std::string reads;
    std::size_t line_start = 0;
    std::size_t index = 0;
    for (std::size_t at = declared.text.find('\n'); at != std::string::npos;
         at = declared.text.find('\n', line_start), ++index) {
        if (index >= first)
            declarations += declared.text.substr(line_start, at - line_start + 1);
        line_start = at + 1;
    }
    for (std::size_t input = first; input < inputs.size(); ++input) {
        if (!declared.identifiers[input]) continue;
        const std::string number = std::to_string(input);
        reads += "    vec4 value" + number + " = PELICAN_TEXTURE_2D_" + number +
                 "(" + *declared.identifiers[input] + ", inUV);\n";
    }
    if (declarations.empty()) return {};
    // A shader that reads nothing (the Blank pipeline's present) has no
    // includes yet, and the macros come from them.
    std::string text =
        "// The macros need these after #version, if the shader lacks them:\n"
        "//   #extension GL_GOOGLE_include_directive : require\n"
        "//   #include \"pelican_sets.glsl\"\n"
        "//   #include \"pelican_view.glsl\"\n"
        "// Add after them:\n" +
        declarations;
    if (!reads.empty()) text += "// and in main():\n" + reads;
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

std::vector<std::string> fullscreenShaderChoices(
    FullscreenShaderStage stage,
    const std::optional<std::filesystem::path> &project_root) {
    std::vector<std::string> result;
    if (project_root) {
        const std::string extension =
            stage == FullscreenShaderStage::vertex ? ".vert" : ".frag";
        std::vector<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator entry{*project_root / "shaders", error}, end;
             !error && entry != end; entry.increment(error)) {
            std::error_code type_error;
            if (!entry->is_regular_file(type_error) ||
                entry->path().extension().string() != extension)
                continue;
            const auto stem = entry->path().stem().string();
            if (isValidShaderTemplateName(stem)) names.push_back(stem);
        }
        std::ranges::sort(names);
        for (const auto &name : names) result.push_back(shaderTemplateReference(name));
    }
    // The fullscreen shaders the render compiler knows by name
    // (vulkanrendercompilerprogram.cpp), and the one vertex they all draw.
    if (stage == FullscreenShaderStage::vertex) {
        result.emplace_back("engine://fullscreen");
    } else {
        for (const char *reference :
             {"engine://fullscreen", "engine://scene_present",
              "engine://output_transform", "engine://ssao", "engine://ssao_blur"})
            result.emplace_back(reference);
    }
    return result;
}

std::string proposedColorTargetName(std::string_view pass_name,
                                    const std::vector<std::string> &taken) {
    std::string base;
    for (const char value : pass_name) {
        const auto byte = static_cast<unsigned char>(value);
        base.push_back(std::isalnum(byte) != 0 ? value : '_');
    }
    if (base.empty()) base = "fullscreen_pass";
    base += "_color";
    std::string candidate = base;
    for (int index = 2; std::ranges::find(taken, candidate) != taken.end(); ++index)
        candidate = base + "_" + std::to_string(index);
    return candidate;
}

std::vector<std::string> fullscreenTemplateInputFill(
    FullscreenShaderTemplateKind kind,
    const std::vector<std::string> &declared) {
    struct RoleTarget {
        std::string_view name;
        std::array<std::string_view, 2> words;
    };
    // materialPassSlots' G-buffer names, in the order deferred_lighting_reads
    // reads them.
    static constexpr std::array deferred_targets{
        RoleTarget{"gbuffer_albedo", {"albedo", ""}},
        RoleTarget{"gbuffer_normal", {"normal", ""}},
        RoleTarget{"gbuffer_material", {"material", ""}},
        RoleTarget{"gbuffer_worldpos", {"worldpos", "world_position"}},
        RoleTarget{"gbuffer_emissive", {"emissive", ""}},
    };
    std::vector<std::string> filled;
    if (kind != FullscreenShaderTemplateKind::deferred_lighting) return filled;
    for (const auto &role : deferred_targets) {
        std::string chosen;
        if (std::ranges::find(declared, role.name) != declared.end()) {
            chosen = std::string{role.name};
        } else {
            std::vector<std::string> carrying;
            for (const auto &name : declared)
                for (const auto word : role.words)
                    if (!word.empty() && name.find(word) != std::string::npos &&
                        std::ranges::find(filled, name) == filled.end()) {
                        carrying.push_back(name);
                        break;
                    }
            if (carrying.size() == 1) chosen = carrying.front();
        }
        if (chosen.empty()) break;
        filled.push_back(std::move(chosen));
    }
    return filled;
}

} // namespace PelicanStudio
