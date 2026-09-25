#include "shaderinputcontract.hpp"

#include <algorithm>
#include <cstdint>
#include <regex>

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

// Q-14f: the other direction's sentence. It is not one of the above.
constexpr std::string_view UnsuppliedSentence =
    "Fullscreen shader declares a pass input the pass does not supply";

bool unsuppliedInputMessage(std::string_view message) {
    return message.find(UnsuppliedSentence) != std::string_view::npos;
}

// "sampled_image" -> "sampled image". "none" and the unnamed "pass input"
// are no type.
std::string typeWords(std::string type) {
    std::ranges::replace(type, '_', ' ');
    return type == "none" || type == "pass input" ? std::string{} : type;
}

// "... but fragment '<f>' declares an input attachment at set 1 binding 2 ..."
struct UnsuppliedDeclaration {
    std::string type;
    std::optional<std::size_t> binding;
};
UnsuppliedDeclaration unsuppliedFromMessage(const std::string &message) {
    static const std::regex pattern{
        R"(declares (?:an? )?(.+?) at set \d+ binding (\d+))"};
    UnsuppliedDeclaration found;
    const auto sentence = message.find(UnsuppliedSentence);
    if (sentence == std::string::npos) return found;
    const auto rest = message.substr(sentence + UnsuppliedSentence.size());
    std::smatch match;
    if (!std::regex_search(rest, match, pattern)) return found;
    found.type = typeWords(match[1].str());
    try {
        found.binding = static_cast<std::size_t>(std::stoul(match[2].str()));
    } catch (const std::exception &) {
    }
    return found;
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

} // namespace

std::vector<ShaderInputContractAction> shaderInputContractActions(
    std::string_view direction) {
    std::vector<ShaderInputContractAction> actions;
    // engine://passthrough reads what the pass gives. It cannot make the
    // pass give what the engine has no way to fill.
    if (direction != ShaderInputContractShaderInputUnsupplied)
        actions.push_back({std::string{ShaderInputContractUsePassthrough},
                           "Use engine://passthrough"});
    actions.push_back(
        {std::string{ShaderInputContractOpenShader}, "Open the shader"});
    return actions;
}

bool shaderInputContractOffers(const ShaderInputContractNotice &notice,
                               std::string_view action) {
    return std::ranges::any_of(notice.actions, [action](const auto &offered) {
        return offered.id == action;
    });
}

std::string shaderInputUnsuppliedLine(std::string_view pass,
                                      std::string_view fragment,
                                      std::string_view type,
                                      std::optional<std::size_t> binding) {
    std::string line = std::string{pass} + "'s shader (" + stemOf(fragment) +
                       ") declares an input the pass cannot supply";
    std::string where{type};
    if (binding) {
        if (!where.empty()) where += ' ';
        where += (where.empty() ? "binding " : "at binding ") +
                 std::to_string(*binding);
    }
    if (!where.empty()) line += " (" + where + ")";
    line += ". Remove that declaration from the shader.";
    return line;
}

std::string shaderInputUnsuppliedOpenedLine(std::optional<std::size_t> binding) {
    return "Remove the pass-input declaration" +
           (binding ? " at binding " + std::to_string(*binding) : std::string{}) +
           " from the shader and save it, and it reloads by itself.";
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
    std::optional<std::size_t> declared_by_shader) {
    ShaderInputContractNotice notice;
    if (!error.is_object()) return notice;
    const auto code = textOr(error, "code");
    if (!code.empty() && code != "render_pipeline_preflight_failed")
        return notice;
    const auto message = textOr(error, "message");
    const auto structured = error.find("shader_input_mismatch");
    const bool has_structured =
        structured != error.end() && structured->is_object();
    if (!has_structured && !fullscreenInputContractMessage(message) &&
        !unsuppliedInputMessage(message))
        return notice;

    notice.matched = true;
    notice.pass = std::string{pass};
    notice.fragment = std::string{fragment};
    // Q-15: the engine's direction when it gives one. Without the object, the
    // sentence says which; without a direction, it is the Q-14e one.
    if (has_structured) {
        if (const auto direction = textOr(*structured, "direction");
            !direction.empty())
            notice.direction = direction;
    } else if (unsuppliedInputMessage(message)) {
        notice.direction = std::string{ShaderInputContractShaderInputUnsupplied};
    }
    const bool unsupplied =
        notice.direction == ShaderInputContractShaderInputUnsupplied;
    if (has_structured) {
        // Q-14e: the engine's own account of the mismatch.
        if (const auto named = textOr(*structured, "pass"); !named.empty())
            notice.pass = named;
        if (const auto named = textOr(*structured, "fragment"); !named.empty())
            notice.fragment = named;
        notice.resource = textOr(*structured, "resource");
        const auto binding = structured->find("binding");
        if (binding != structured->end() && binding->is_number_integer() &&
            binding->get<std::int64_t>() >= 0)
            notice.binding = binding->get<std::size_t>();
        const auto inputs = structured->find("shader_pass_inputs");
        if (inputs != structured->end() && inputs->is_array()) {
            notice.declared = inputs->size();
            // Q-15: the type of the declaration the pass cannot supply.
            if (unsupplied && notice.binding)
                for (const auto &input : *inputs)
                    if (input.is_object() && input.value("binding", Json{}) ==
                                                 Json(*notice.binding))
                        notice.declared_type = typeWords(textOr(input, "type"));
        }
    }
    if (unsupplied) {
        // What the object did not say, the sentence may.
        const auto from_message = unsuppliedFromMessage(message);
        if (!notice.binding) notice.binding = from_message.binding;
        if (notice.declared_type.empty() &&
            (!from_message.binding || from_message.binding == notice.binding))
            notice.declared_type = from_message.type;
    }
    if (!notice.declared) notice.declared = declaredFromMessage(message);
    if (!notice.declared) notice.declared = declared_by_shader;
    notice.fragment_stem = stemOf(notice.fragment);
    notice.line = unsupplied
                      ? shaderInputUnsuppliedLine(notice.pass, notice.fragment,
                                                  notice.declared_type,
                                                  notice.binding)
                      : shaderInputContractLine(notice.pass, notice.fragment,
                                                notice.declared);
    notice.actions = shaderInputContractActions(notice.direction);
    notice.can_open = notice.fragment.starts_with("project://") ||
                      notice.fragment == "engine://passthrough";
    return notice;
}

std::size_t shaderInputCapacity(const FramePlanPassInputs &inputs) {
    std::size_t capacity = 0;
    for (const auto &declaration : inputs.declarations)
        if (!declaration.variadic) ++capacity;
    if (inputs.variadic_range) capacity += inputs.variadic_range->max;
    return capacity;
}

std::size_t shaderInputFixedImageCount(const FramePlanPassInputs &inputs) {
    return static_cast<std::size_t>(std::ranges::count_if(
        inputs.declarations, [](const FramePlanPassInputDeclaration &entry) {
            return !entry.variadic && !entry.buffer();
        }));
}

bool shaderInputDeclarationsAreFor(const FramePlanPassInputs &inputs,
                                   std::string_view fragment) {
    const auto bare = [](std::string_view reference) {
        constexpr std::string_view extension = ".frag";
        if (reference.ends_with(extension))
            reference.remove_suffix(extension.size());
        return reference;
    };
    return !fragment.empty() && bare(inputs.fragment) == bare(fragment);
}

std::optional<std::size_t> shaderInputDeclaredFor(
    const FramePlanPassInputs *inputs, std::string_view fragment) {
    if (inputs == nullptr || !inputs->declared() ||
        !shaderInputDeclarationsAreFor(*inputs, fragment))
        return std::nullopt;
    return shaderInputCapacity(*inputs);
}

std::string shaderInputPreSaveLine(std::string_view pass,
                                   std::string_view draft_fragment,
                                   std::size_t listed,
                                   const FramePlanPassInputs *current) {
    const auto declared = shaderInputDeclaredFor(current, draft_fragment);
    if (!declared || *declared >= listed) return {};
    return shaderInputContractLine(pass, draft_fragment, declared);
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

} // namespace PelicanStudio
