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

std::string mismatchMessage(
    const FullscreenInputBindingMismatchInfo &info) {
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
    const auto count = info.shader_pass_inputs.size();
    message += " (shader declares " + std::to_string(count) +
               (count == 1 ? " pass input)" : " pass inputs)");
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
