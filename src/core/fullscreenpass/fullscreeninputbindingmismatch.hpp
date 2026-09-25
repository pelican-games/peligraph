#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

// One binding of the pass-input set (PELICAN_SET_PASS_INPUT) as a shader's
// reflection declares it.
struct FullscreenShaderPassInput {
    std::uint32_t binding = 0;
    // "sampled_image" (a combined image sampler), "input_attachment",
    // "storage_buffer", or another descriptor type's snake_case name.
    std::string type;
};

// The two ways a pass's inputs and its shader's pass-input set disagree.
// Q-14e: the pass lists an input at a binding the shader does not declare
// with that type.
inline constexpr const char *fullscreenInputPassInputUndeclared =
    "pass_input_undeclared";
// Q-14f: the shader declares a pass-input binding past the inputs the pass
// supplies, of a type the engine cannot fill with a default resource (an
// input attachment, a uniform buffer, a storage image, ...). A sampled image,
// separate image, sampler or storage buffer there is filled instead, and the
// pass compiler logs this refusal's words as a warning.
inline constexpr const char *fullscreenInputShaderInputUnsupplied =
    "shader_input_unsupplied";

// Q-14e / Q-14f: a fullscreen / raster pass's inputs and its shader's
// pass-input set (PELICAN_SET_PASS_INPUT) disagree. Pass inputs bind in
// order: textures from 0, then buffers.
struct FullscreenInputBindingMismatchInfo {
    // The pass and its fragment shader reference. The pass compiler adds
    // them; they are empty when the container is called without a pass
    // (the XR mirror's output transform).
    std::string pass;
    std::string fragment;
    // fullscreenInputPassInputUndeclared or
    // fullscreenInputShaderInputUnsupplied.
    std::string direction = fullscreenInputPassInputUndeclared;
    // pass_input_undeclared: the pass input's kind, "texture" or "buffer".
    // shader_input_unsupplied: the shader declaration's kind, "texture"
    // (an image, sampler or input attachment), "buffer" (a storage,
    // uniform or texel buffer) or "other".
    std::string kind;
    // pass_input_undeclared: the pass input's binding.
    // shader_input_unsupplied: the lowest binding the pass does not supply.
    std::uint32_t binding = 0;
    // The render target or buffer the input names; empty when unknown, and
    // always empty for shader_input_unsupplied (no input names it).
    std::string resource;
    // pass_input_undeclared: "sampled_image", "input_attachment" or
    // "storage_buffer". shader_input_unsupplied: "none".
    std::string expected;
    std::size_t pass_texture_inputs = 0;
    std::size_t pass_buffer_inputs = 0;
    // Binding order.
    std::vector<FullscreenShaderPassInput> shader_pass_inputs;
};

// pass_input_undeclared: what() starts with the sentence this refusal has
// always had ("Fullscreen pass input texture/local-read contract does not
// match shader reflection" or "Fullscreen pass input buffer does not match
// shader reflection") and goes on to name the pass, the input, and what the
// shader declares instead.
// shader_input_unsupplied: what() starts with "Fullscreen shader declares a
// pass input the pass does not supply" and names the pass, how many inputs it
// supplies, and the declaration past them.
class FullscreenInputBindingMismatch : public std::runtime_error {
  public:
    explicit FullscreenInputBindingMismatch(
        FullscreenInputBindingMismatchInfo info);

    [[nodiscard]] const FullscreenInputBindingMismatchInfo &
    info() const noexcept {
        return info_;
    }

    // The same mismatch, named by the pass that listed the inputs.
    [[nodiscard]] FullscreenInputBindingMismatch inPass(
        std::string pass, std::string fragment) const;

  private:
    FullscreenInputBindingMismatchInfo info_;
};

} // namespace Pelican
