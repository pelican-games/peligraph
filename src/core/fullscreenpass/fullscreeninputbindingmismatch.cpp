#include "fullscreeninputbindingmismatch.hpp"
#include "../shader/pelican_sets.hpp"

#include <string_view>
#include <utility>

namespace Pelican {

namespace {

// "sampled_image" -> "a sampled image", "input_attachment" -> "an input
// attachment", "uniform_buffer" -> "a uniform buffer".
std::string describedDescriptor(std::string_view type) {
    std::string words{type};
    for (auto &ch : words) {
        if (ch == '_') ch = ' ';
    }
    const auto vowel_sound = !words.empty() &&
                             std::string_view{"aeio"}.find(words.front()) !=
                                 std::string_view::npos;
    return (vowel_sound ? "an " : "a ") + words;
}

std::string declaredCountTail(
    const FullscreenInputBindingMismatchInfo &info) {
    const auto count = info.shader_pass_inputs.size();
    return " (shader declares " + std::to_string(count) +
           (count == 1 ? " pass input)" : " pass inputs)");
}

// Q-14f: the shader declares a binding past the inputs the pass supplies.
std::string unsuppliedMessage(
    const FullscreenInputBindingMismatchInfo &info) {
    const auto supplied =
        info.pass_texture_inputs + info.pass_buffer_inputs;
    std::string message =
        "Fullscreen shader declares a pass input the pass does not "
        "supply: ";
    message += info.pass.empty() ? std::string{"the pass"}
                                 : "pass '" + info.pass + "'";
    message += " supplies " + std::to_string(supplied) +
               (supplied == 1 ? " pass input" : " pass inputs") + ", but ";
    message += info.fragment.empty()
                   ? std::string{"the shader"}
                   : "fragment '" + info.fragment + "'";
    const FullscreenShaderPassInput *declared = nullptr;
    for (const auto &input : info.shader_pass_inputs) {
        if (input.binding == info.binding) {
            declared = &input;
            break;
        }
    }
    message += " declares " +
               (declared == nullptr ? std::string{"a pass input"}
                                    : describedDescriptor(declared->type)) +
               " at set " + std::to_string(PELICAN_SET_PASS_INPUT) +
               " binding " + std::to_string(info.binding);
    message += declaredCountTail(info);
    return message;
}

std::string mismatchMessage(
    const FullscreenInputBindingMismatchInfo &info) {
    if (info.direction == fullscreenInputShaderInputUnsupplied) {
        return unsuppliedMessage(info);
    }
    std::string message =
        info.kind == "buffer"
            ? "Fullscreen pass input buffer does not match shader reflection"
            : "Fullscreen pass input texture/local-read contract does not "
              "match shader reflection";
    message += ": ";
    if (!info.pass.empty()) message += "pass '" + info.pass + "' ";
    message += "input " + std::to_string(info.binding);
    if (!info.resource.empty()) {
        message += info.kind == "buffer"
                       ? " (buffer '" + info.resource + "')"
                       : " ('" + info.resource + "')";
    }
    message += " expects " + describedDescriptor(info.expected) +
               " at set " + std::to_string(PELICAN_SET_PASS_INPUT) +
               " binding " + std::to_string(info.binding) + ", but ";
    message += info.fragment.empty()
                   ? std::string{"the shader"}
                   : "fragment '" + info.fragment + "'";
    const FullscreenShaderPassInput *declared = nullptr;
    for (const auto &input : info.shader_pass_inputs) {
        if (input.binding == info.binding) declared = &input;
    }
    message += declared == nullptr
                   ? std::string{" declares no pass input at that binding"}
                   : " declares " + describedDescriptor(declared->type) +
                         " there";
    message += declaredCountTail(info);
    return message;
}

} // namespace

FullscreenInputBindingMismatch::FullscreenInputBindingMismatch(
    FullscreenInputBindingMismatchInfo info)
    : std::runtime_error{mismatchMessage(info)},
      info_{std::move(info)} {}

FullscreenInputBindingMismatch FullscreenInputBindingMismatch::inPass(
    std::string pass, std::string fragment) const {
    auto named = info_;
    named.pass = std::move(pass);
    named.fragment = std::move(fragment);
    return FullscreenInputBindingMismatch{std::move(named)};
}

} // namespace Pelican
