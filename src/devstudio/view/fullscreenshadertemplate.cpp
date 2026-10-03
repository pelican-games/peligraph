#include "fullscreenshadertemplate.hpp"

#include "battery/embed.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
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

} // namespace

bool isTemplateShadowMapInput(std::string_view resource) {
    return resource == ShadowMapInput ||
           (resource.size() > ShadowMapInput.size() && resource.ends_with(ShadowMapInput) &&
            resource[resource.size() - ShadowMapInput.size() - 1] == '_');
}

bool isTemplateShadowMaskInput(std::string_view resource) {
    return resource == ShadowMaskInput ||
           (resource.size() > ShadowMaskInput.size() && resource.ends_with(ShadowMaskInput) &&
            resource[resource.size() - ShadowMaskInput.size() - 1] == '_');
}

namespace {

// The declarations of every image input, in list order, and the identifier
// each declared input got (none for a buffer or an input past the macros).
struct DeclaredInputs {
    std::string text;
    std::vector<std::optional<std::string>> identifiers;
    // UX-5b (RS-10): the input that is the directional shadow map, which no
    // role reads.
    std::vector<bool> shadow;
    // UX-6a, decision 6: the input that is the shadow mask, which no role
    // reads either.
    std::vector<bool> mask;
};

// The lighting template's mask at `number`: declared, and read through
// PELICAN_TEMPLATE_SHADOW_MASK_READ (the read macro carries the binding).
std::string shadowMaskDeclaration(const std::string &number, const std::string &identifier,
                                  std::string_view note) {
    return "PELICAN_DECLARE_INPUT_" + number + "(" + identifier + ");  // " + std::string{note} +
           "\n#define PELICAN_TEMPLATE_SHADOW_MASK_READ(uv) PELICAN_TEXTURE_2D_" + number + "(" +
           identifier + ", uv)\n";
}

DeclaredInputs declareInputs(
    std::span<const FullscreenShaderTemplateInput> inputs,
    std::span<const RoleRead> reads, bool lighting = false) {
    DeclaredInputs result;
    std::set<std::string> taken;
    bool ports_included = false;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto &input = inputs[index];
        const std::string number = std::to_string(index);
        result.identifiers.emplace_back();
        result.shadow.push_back(false);
        result.mask.push_back(false);
        if (!input.image) {
            // UX-13a, decision 2: no binding written by hand. A buffer is
            // read through the include the pass's ports generate, which
            // declares pelican_resource_<port> where the engine binds it
            // (after the images, in the order of the buffers' names). One
            // line per input; the first buffer's carries the include.
            result.text += std::string{ports_included ? "" : "#include \"pelican_resource_ports.glsl\"  "} +
                           "// input " + number + " (" + input.resource +
                           ") is a buffer: map it to a buffer port of the pass and read it as "
                           "pelican_resource_<port> (pelican_load_<port>(i)).\n";
            ports_included = true;
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
        if (lighting && isTemplateShadowMapInput(input.resource) &&
            std::ranges::find(result.shadow, true) == result.shadow.end()) {
            // The lighting template's shadow: declared whatever the feature
            // says (the binding is the pass's), sampled only under
            // PELICAN_FEATURE_SHADOW.
            result.text += "PELICAN_DECLARE_INPUT_" + number + "(" + identifier + ");  // " +
                           input.resource + ": the directional shadow map\n" +
                           "#define PELICAN_TEMPLATE_SHADOW_MAP " + identifier + "\n";
            result.identifiers.back() = identifier;
            result.shadow.back() = true;
            continue;
        }
        if (lighting && isTemplateShadowMaskInput(input.resource) &&
            std::ranges::find(result.mask, true) == result.mask.end()) {
            result.text += shadowMaskDeclaration(
                number, identifier, input.resource + ": the shadow mask (r: 1 lit, 0 shadowed)");
            result.identifiers.back() = identifier;
            result.mask.back() = true;
            continue;
        }
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
    // UX-6a, decision 6: a pass with no mask among its inputs gets the
    // binding after them, so its node offers the socket to connect one to.
    if (lighting && std::ranges::find(result.mask, true) == result.mask.end() &&
        inputs.size() < FullscreenShaderTemplateMacroInputs) {
        const auto number = std::to_string(inputs.size());
        result.text += shadowMaskDeclaration(
            number, inputIdentifier(ShadowMaskInput, taken),
            "shadow_mask (optional): connect an R8 mask here, 1 lit, 0 shadowed");
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
        const bool shadow = index < declared.shadow.size() && declared.shadow[index];
        const bool mask = index < declared.mask.size() && declared.mask[index];
        if (index < inputs.size() && declared.identifiers[index] && !shadow && !mask) {
            text += "PELICAN_TEXTURE_2D_" + number + "(" +
                    *declared.identifiers[index] + ", inUV)" +
                    std::string{read.components} + ";\n";
            continue;
        }
        const std::string_view why =
            shadow                ? "the shadow map is there"
            : mask                ? "the shadow mask is there"
            : index < inputs.size() ? "not an image"
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
    const auto declared =
        declareInputs(inputs, reads, kind == FullscreenShaderTemplateKind::deferred_lighting);
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

std::string withLoopElementInclude(std::string_view source) {
    std::string text{source};
    if (text.find(LoopElementInclude) != std::string::npos) return text;
    // After the sets include (pelican_loop.glsl needs its offsets), else
    // after the #extension or #version line, else at the top.
    std::size_t at = std::string::npos;
    for (const std::string_view anchor :
         {std::string_view{"#include \"pelican_sets.glsl\""},
          std::string_view{"#extension GL_GOOGLE_include_directive"},
          std::string_view{"#version"}}) {
        const auto found = text.find(anchor);
        if (found == std::string::npos) continue;
        const auto end = text.find('\n', found);
        at = end == std::string::npos ? text.size() : end + 1;
        break;
    }
    if (at == std::string::npos) at = 0;
    std::string lines;
    if (at == text.size() && !text.empty() && text.back() != '\n') lines += '\n';
    lines += std::string{LoopElementInclude} + "\n" + std::string{LoopElementUsage} + "\n";
    text.insert(at, lines);
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
    // A shader that reads nothing (one written without inputs, like the
    // gradient a project starts a pass with) has no includes yet, and the
    // macros come from them.
    std::string text =
        "// The macros need these after #version, if the shader lacks them:\n"
        "//   #extension GL_GOOGLE_include_directive : require\n"
        "//   #include \"pelican_sets.glsl\"\n"
        "//   #include \"pelican_view.glsl\"\n" +
        std::string{std::ranges::any_of(inputs.subspan(std::min(first, inputs.size())),
                                        [](const auto &input) { return !input.image; })
                        ? "//   #include \"pelican_resource_ports.glsl\"\n"
                        : ""} +
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
    // UX-5b: the engine's own list, by stage, when a catalog handed it over.
    if (std::ranges::any_of(engineShaderList(), [&](const EngineShaderListEntry &entry) {
            return entry.stage == stage;
        })) {
        for (const auto &entry : engineShaderList())
            if (entry.stage == stage) result.push_back(entry.reference);
        return result;
    }
    // The fullscreen shaders the render compiler knows by name
    // (vulkanrendercompilerprogram.cpp), and the one vertex they all draw.
    if (stage == FullscreenShaderStage::vertex) {
        result.emplace_back("engine://fullscreen");
    } else {
        // VI-3a: engine://passthrough takes any number of inputs and shows
        // one of them (the Blank pipeline's present draws with it).
        for (const char *reference :
             {"engine://passthrough", "engine://fullscreen",
              "engine://scene_present", "engine://output_transform",
              "engine://ssao", "engine://ssao_blur"})
            result.emplace_back(reference);
    }
    return result;
}

namespace {

std::vector<EngineShaderListEntry> &engineShaderListStore() {
    static std::vector<EngineShaderListEntry> entries;
    return entries;
}

std::function<void()> &engineShaderListRequester() {
    static std::function<void()> requester;
    return requester;
}

std::map<std::size_t, std::function<void()>> &engineShaderListObservers() {
    static std::map<std::size_t, std::function<void()>> observers;
    return observers;
}

} // namespace

bool setEngineShaderList(std::vector<EngineShaderListEntry> entries) {
    auto &store = engineShaderListStore();
    if (store == entries) return false;
    store = std::move(entries);
    // A copy: an observer may take itself out.
    const auto observers = engineShaderListObservers();
    for (const auto &[id, observer] : observers)
        if (observer) observer();
    return true;
}

void setEngineShaderListRequester(std::function<void()> requester) {
    engineShaderListRequester() = std::move(requester);
}

void requestEngineShaderList() {
    if (const auto &requester = engineShaderListRequester()) requester();
}

std::size_t addEngineShaderListObserver(std::function<void()> observer) {
    static std::size_t next = 1;
    const auto id = next++;
    engineShaderListObservers().emplace(id, std::move(observer));
    return id;
}

void removeEngineShaderListObserver(std::size_t id) {
    engineShaderListObservers().erase(id);
}

const std::vector<EngineShaderListEntry> &engineShaderList() {
    return engineShaderListStore();
}

bool engineShaderPortsByHand(std::string_view reference) {
    return std::ranges::any_of(engineShaderList(), [&](const EngineShaderListEntry &entry) {
        return entry.ports_by_hand && entry.reference == reference;
    });
}

std::string shaderChoiceLabel(std::string_view reference) {
    std::string label{reference};
    if (engineShaderPortsByHand(reference)) label += "  (ports by hand)";
    return label;
}

std::string shaderChoiceTooltip(std::string_view reference) {
    if (!engineShaderPortsByHand(reference)) return std::string{reference};
    return std::string{reference} +
           ": the engine reads its ports from the pass that runs it (a feature's JSON "
           "declares them), so it has no sockets of its own -- connect its ports yourself.";
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
            // UX-5a: the name's end first -- the socket a Graph image is
            // named after (`render_material_pass__material`), or a word after
            // the last '_' -- and only then the word anywhere in the name: a
            // Graph pass's five images all carry "material" in the middle.
            const auto ends = [](const std::string &name, std::string_view word) {
                return name == word ||
                       (name.size() > word.size() && name.ends_with(word) &&
                        name[name.size() - word.size() - 1] == '_');
            };
            const auto inside = [](const std::string &name, std::string_view word) {
                return name.find(word) != std::string::npos;
            };
            for (const auto matches : {+ends, +inside}) {
                std::vector<std::string> carrying;
                for (const auto &name : declared)
                    for (const auto word : role.words)
                        if (!word.empty() && matches(name, word) &&
                            std::ranges::find(filled, name) == filled.end()) {
                            carrying.push_back(name);
                            break;
                        }
                // Two names of one kind is no answer, and a looser test
                // would find at least the same two.
                if (carrying.size() == 1) chosen = carrying.front();
                if (!carrying.empty()) break;
            }
        }
        if (chosen.empty()) break;
        filled.push_back(std::move(chosen));
    }
    // UX-5b (RS-10): with the shadow_directional feature its shadow_map is
    // declared, and the lighting reads it after the G-buffer -- only behind a
    // whole G-buffer, for the inputs are read by position.
    if (filled.size() == deferred_targets.size()) {
        const auto shadow = std::ranges::find_if(declared, [](const std::string &name) {
            return name == ShadowMapInput;
        });
        if (shadow != declared.end()) filled.push_back(*shadow);
    }
    return filled;
}


// --- VI-3b.

// The bytes of src/core/resources/passthrough.frag, written at build time by
// src/devstudio/resources/embed_engine_shader.cmake.
extern const unsigned char EnginePassthroughFragmentBytes[];
extern const std::size_t EnginePassthroughFragmentSize;

namespace {

// One line of a source: [begin, content_end) is its text, [content_end, end)
// its line ending ("\r\n", "\n", or nothing on a last line without one).
struct SourceLine {
    std::size_t begin = 0;
    std::size_t content_end = 0;
    std::size_t end = 0;
};

std::vector<SourceLine> sourceLines(std::string_view source) {
    std::vector<SourceLine> lines;
    std::size_t begin = 0;
    while (begin < source.size()) {
        const auto newline = source.find('\n', begin);
        SourceLine line;
        line.begin = begin;
        if (newline == std::string_view::npos) {
            line.content_end = line.end = source.size();
        } else {
            line.end = newline + 1;
            line.content_end =
                newline > begin && source[newline - 1] == '\r' ? newline - 1
                                                               : newline;
        }
        lines.push_back(line);
        begin = line.end;
    }
    return lines;
}

std::string_view lineText(std::string_view source, const SourceLine &line) {
    return source.substr(line.begin, line.content_end - line.begin);
}

std::size_t indentLength(std::string_view content) {
    std::size_t length = 0;
    while (length < content.size() &&
           (content[length] == ' ' || content[length] == '\t'))
        ++length;
    return length;
}

std::string_view trimmedRight(std::string_view text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

// `PELICAN_DECLARE_VARIADIC_INPUTS(` at the start of the line (after its
// indentation). A line that is a comment does not start with it.
bool variadicMacroLine(std::string_view content) {
    constexpr std::string_view macro = "PELICAN_DECLARE_VARIADIC_INPUTS";
    content.remove_prefix(indentLength(content));
    if (!content.starts_with(macro)) return false;
    content.remove_prefix(macro.size());
    content.remove_prefix(indentLength(content));
    return content.starts_with('(');
}

struct FixedBlock {
    std::size_t begin_line = 0;
    std::size_t end_line = 0;
};

std::optional<FixedBlock> findFixedBlock(std::string_view source,
                                         const std::vector<SourceLine> &lines) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        auto opening = lineText(source, lines[index]);
        opening.remove_prefix(indentLength(opening));
        if (!opening.starts_with(FixedSocketsBegin)) continue;
        for (std::size_t end = index + 1; end < lines.size(); ++end) {
            auto closing = lineText(source, lines[end]);
            closing.remove_prefix(indentLength(closing));
            if (trimmedRight(closing) == FixedSocketsEnd)
                return FixedBlock{index, end};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string_view> engineFragmentSource(std::string_view reference) {
    if (reference != EnginePassthroughFragment) return std::nullopt;
    return std::string_view{
        reinterpret_cast<const char *>(EnginePassthroughFragmentBytes),
        EnginePassthroughFragmentSize};
}

std::string engineShaderCopyName(const std::filesystem::path &project_root,
                                 std::string_view pass) {
    auto base = suggestedShaderTemplateName(pass);
    if (base.empty()) base = "fullscreen_pass";
    std::string candidate = base;
    std::error_code error;
    for (int index = 2;
         std::filesystem::exists(shaderTemplatePath(project_root, candidate), error);
         ++index)
        candidate = base + "_" + std::to_string(index);
    return candidate;
}

ShaderSocketRewrite fixShaderSockets(std::string_view source, std::size_t first,
                                     const std::vector<std::string> &inputs,
                                     const std::vector<std::string> &taken) {
    ShaderSocketRewrite result;
    if (inputs.empty()) {
        result.refusal = "Connect at least one input before fixing the sockets.";
        return result;
    }
    const auto lines = sourceLines(source);
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!variadicMacroLine(lineText(source, lines[index]))) continue;
        if (found) {
            result.refusal = "The shader has more than one "
                             "PELICAN_DECLARE_VARIADIC_INPUTS line.";
            return result;
        }
        found = index;
    }
    if (!found) {
        result.refusal =
            "The shader has no PELICAN_DECLARE_VARIADIC_INPUTS line to fix.";
        return result;
    }
    const auto &line = lines[*found];
    const auto content = lineText(source, line);
    const std::string indent{content.substr(0, indentLength(content))};
    const auto text = content.substr(indent.size());
    const std::string eol{
        source.substr(line.content_end, line.end - line.content_end)};
    // Inside the block every line ends the way the file's lines do.
    const std::string newline =
        !eol.empty() ? eol
                     : (source.find("\r\n") != std::string_view::npos ? "\r\n"
                                                                     : "\n");
    std::set<std::string> names{taken.begin(), taken.end()};
    std::vector<std::string> identifiers;
    for (const auto &input : inputs)
        identifiers.push_back(inputIdentifier(input, names));

    std::string block = indent + std::string{FixedSocketsBegin} +
                        std::string{text} + newline;
    for (std::size_t k = 0; k < identifiers.size(); ++k)
        block += indent + "layout(set = PELICAN_SET_PASS_INPUT, binding = " +
                 std::to_string(first + k) + ") uniform sampler2D " +
                 identifiers[k] + ";" + newline;
    // What PELICAN_DECLARE_VARIADIC_INPUTS made besides the declarations, so
    // PELICAN_VARIADIC_TEXTURE and PELICAN_VARIADIC_INPUT_COUNT still mean
    // the same thing to the body.
    block += indent +
             "vec4 pelican_variadic_texture(int pelican_variadic_index, vec2 "
             "pelican_variadic_uv) {" +
             newline;
    for (std::size_t k = 0; k < identifiers.size(); ++k)
        block += indent + "    if (pelican_variadic_index == " +
                 std::to_string(k) + ") return texture(" + identifiers[k] +
                 ", pelican_variadic_uv);" + newline;
    block += indent + "    return vec4(0.0);" + newline;
    block += indent + "}" + newline;
    block += indent + "const int pelican_variadic_input_count = " +
             std::to_string(identifiers.size()) + ";" + newline;
    block += indent + std::string{FixedSocketsEnd} + eol;

    result.source = std::string{source.substr(0, line.begin)} + block +
                    std::string{source.substr(line.end)};
    return result;
}

ShaderSocketRewrite variadicShaderSockets(std::string_view source) {
    ShaderSocketRewrite result;
    const auto lines = sourceLines(source);
    const auto block = findFixedBlock(source, lines);
    if (!block) {
        result.refusal = "The shader has no sockets Studio fixed.";
        return result;
    }
    const auto &opening = lines[block->begin_line];
    const auto &closing = lines[block->end_line];
    const auto content = lineText(source, opening);
    const auto indent = content.substr(0, indentLength(content));
    const auto was = content.substr(indent.size() + FixedSocketsBegin.size());
    result.source = std::string{source.substr(0, opening.begin)} +
                    std::string{indent} + std::string{was} +
                    std::string{source.substr(
                        closing.content_end, closing.end - closing.content_end)} +
                    std::string{source.substr(closing.end)};
    return result;
}

bool shaderHasFixedSockets(std::string_view source) {
    return findFixedBlock(source, sourceLines(source)).has_value();
}

// --- CT-6s, decision 3: the ray tracing set.

std::string_view rayTracingTemplateExtension(RayTracingTemplateStage stage) {
    switch (stage) {
    case RayTracingTemplateStage::raygen: return ".rgen";
    case RayTracingTemplateStage::miss: return ".rmiss";
    case RayTracingTemplateStage::closest_hit: return ".rchit";
    }
    return ".rgen";
}

std::string rayTracingShaderTemplate(RayTracingTemplateStage stage,
                                     std::string_view name) {
    const std::string heading =
        "// " + std::string{name} + std::string{rayTracingTemplateExtension(stage)} +
        ": a ray tracing set Studio wrote (raygen + miss + closest hit).\n";
    // The payload every stage of the set shares: where the ray hit (xyz) and
    // whether it hit (w).
    constexpr std::string_view payload_comment =
        "// The payload the three stages share: xyz is where the ray hit, w is 1\n"
        "// on a hit and 0 when the miss shader ran.\n";
    switch (stage) {
    case RayTracingTemplateStage::raygen:
        return "#version 460\n"
               "#extension GL_GOOGLE_include_directive : enable\n"
               "#extension GL_EXT_ray_tracing : require\n"
               "\n" +
               heading +
               "// One ray per pixel of the image it writes (the task's dispatch.rays_from),\n"
               "// from the camera through that pixel; the colour is the normal of the\n"
               "// surface it hits, or a dim sky where it hits nothing.\n"
               "\n"
               "#include \"pelican_sets.glsl\"\n"
               "#include \"pelican_frame.glsl\"\n"
               "\n"
               "// The image the task writes, port \"" +
               std::string{RayTracingTemplatePort} +
               "\": an image of this format (STORAGE and\n"
               "// SAMPLED) is made beside the task when it is placed.\n"
               "layout(rgba16f, set = PELICAN_SET_PASS_INPUT, binding = 0)\n"
               "    writeonly uniform image2D pelican_resource_" +
               std::string{RayTracingTemplatePort} +
               ";\n"
               "\n"
               "// The scene's acceleration structure (the task's acceleration_structure).\n"
               "layout(set = PELICAN_SET_FRAME, binding = PELICAN_RAY_QUERY_TLAS_BINDING)\n"
               "    uniform accelerationStructureEXT pelicanRayTracingScene;\n"
               "\n" +
               std::string{payload_comment} +
               "layout(location = 0) rayPayloadEXT vec4 hit;\n"
               "\n"
               "const float RAY_MIN_DISTANCE = 0.001;\n"
               "const float RAY_MAX_DISTANCE = 10000.0;\n"
               "\n"
               "// The direction from the camera through a point of the image, in pixels.\n"
               "vec3 cameraRay(vec2 pixel) {\n"
               "    vec2 ndc = pixel / vec2(gl_LaunchSizeEXT.xy) * 2.0 - 1.0;\n"
               "    vec4 target = inverse(pelicanFrame.projection) * vec4(ndc, 0.5, 1.0);\n"
               "    vec3 direction = normalize(target.xyz / target.w);\n"
               "    return normalize((inverse(pelicanFrame.view) * vec4(direction, 0.0)).xyz);\n"
               "}\n"
               "\n"
               "vec4 trace(vec3 origin, vec3 direction) {\n"
               "    hit = vec4(0.0);\n"
               "    traceRayEXT(pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0,\n"
               "                origin, RAY_MIN_DISTANCE, direction, RAY_MAX_DISTANCE, 0);\n"
               "    return hit;\n"
               "}\n"
               "\n"
               "void main() {\n"
               "    vec2 pixel = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);\n"
               "    vec3 origin = (inverse(pelicanFrame.view) * vec4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
               "    vec3 direction = cameraRay(pixel);\n"
               "    vec4 center = trace(origin, direction);\n"
               "    vec3 color;\n"
               "    if (center.w < 0.5) {\n"
               "        // Nothing hit: a dim sky, lighter upwards.\n"
               "        color = mix(vec3(0.04, 0.05, 0.07), vec3(0.18, 0.26, 0.36),\n"
               "                    clamp(direction.y * 0.5 + 0.5, 0.0, 1.0));\n"
               "    } else {\n"
               "        // The surface's normal: the two rays beside this one span it.\n"
               "        vec4 right = trace(origin, cameraRay(pixel + vec2(1.0, 0.0)));\n"
               "        vec4 below = trace(origin, cameraRay(pixel + vec2(0.0, 1.0)));\n"
               "        vec3 normal = -direction;\n"
               "        vec3 spanned = cross(right.xyz - center.xyz, below.xyz - center.xyz);\n"
               "        if (right.w > 0.5 && below.w > 0.5 && dot(spanned, spanned) > 1e-14)\n"
               "            normal = normalize(spanned);\n"
               "        if (dot(normal, direction) > 0.0) normal = -normal;\n"
               "        color = normal * 0.5 + 0.5;\n"
               "    }\n"
               "    imageStore(pelican_resource_" +
               std::string{RayTracingTemplatePort} +
               ", ivec2(gl_LaunchIDEXT.xy), vec4(color, 1.0));\n"
               "}\n";
    case RayTracingTemplateStage::miss:
        return "#version 460\n"
               "#extension GL_EXT_ray_tracing : require\n"
               "\n" +
               heading + "// Runs where a ray hits nothing.\n\n" + std::string{payload_comment} +
               "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
               "\n"
               "void main() {\n"
               "    hit.w = 0.0;\n"
               "}\n";
    case RayTracingTemplateStage::closest_hit:
        return "#version 460\n"
               "#extension GL_EXT_ray_tracing : require\n"
               "\n" +
               heading +
               "// Runs at the nearest surface a ray hits. It says where: the normal needs\n"
               "// the triangle's vertices, which a set without a ray group layout cannot\n"
               "// read, so the ray generation shader takes it from the rays beside it.\n"
               "\n" +
               std::string{payload_comment} +
               "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
               "\n"
               "void main() {\n"
               "    hit = vec4(gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT, 1.0);\n"
               "}\n";
    }
    return {};
}

namespace {

std::filesystem::path rayTracingTemplatePath(const std::filesystem::path &project_root,
                                             std::string_view name, RayTracingTemplateStage stage) {
    if (!isValidShaderTemplateName(name))
        throw std::invalid_argument("a shader name is letters, digits, '_' and '-': " + std::string{name});
    return project_root / "shaders" / (std::string{name} + std::string{rayTracingTemplateExtension(stage)});
}

bool rayTracingTemplateTaken(const std::filesystem::path &project_root, std::string_view name) {
    std::error_code error;
    return std::ranges::any_of(RayTracingTemplateStages, [&](RayTracingTemplateStage stage) {
        return std::filesystem::exists(rayTracingTemplatePath(project_root, name, stage), error);
    });
}

} // namespace

std::string rayTracingTemplateName(const std::filesystem::path &project_root, std::string_view base) {
    auto folded = suggestedShaderTemplateName(base);
    if (folded.empty()) folded = "ray_tracing";
    auto candidate = folded;
    for (int index = 2; rayTracingTemplateTaken(project_root, candidate); ++index)
        candidate = folded + "_" + std::to_string(index);
    return candidate;
}

RayTracingTemplateWrite writeRayTracingTemplate(const std::filesystem::path &project_root,
                                                std::string_view name) {
    RayTracingTemplateWrite result;
    for (const auto stage : RayTracingTemplateStages)
        result.paths.push_back(rayTracingTemplatePath(project_root, name, stage));
    result.reference = shaderTemplateReference(name);
    // A file the author already has is theirs; the set is written whole or
    // not at all.
    if (rayTracingTemplateTaken(project_root, name)) return result;
    std::error_code error;
    std::filesystem::create_directories(result.paths.front().parent_path(), error);
    if (error)
        throw std::runtime_error("could not create " + result.paths.front().parent_path().string() + ": " +
                                 error.message());
    for (std::size_t index = 0; index < RayTracingTemplateStages.size(); ++index) {
        const auto contents = rayTracingShaderTemplate(RayTracingTemplateStages[index], name);
        std::ofstream output{result.paths[index], std::ios::binary};
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) throw std::runtime_error("could not write " + result.paths[index].string());
    }
    result.created = true;
    return result;
}

} // namespace PelicanStudio
