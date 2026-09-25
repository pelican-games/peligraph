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

// Q-14e: a fullscreen / raster pass lists an input at a pass-input binding
// its shader does not declare with that type. Pass inputs bind in order:
// textures from 0, then buffers.
struct FullscreenInputBindingMismatchInfo {
    // The pass and its fragment shader reference. The pass compiler adds
    // them; they are empty when the container is called without a pass
    // (the XR mirror's output transform).
    std::string pass;
    std::string fragment;
    // "texture" or "buffer".
    std::string kind;
    std::uint32_t binding = 0;
    // The render target or buffer the input names; empty when unknown.
    std::string resource;
    // "sampled_image", "input_attachment" or "storage_buffer".
    std::string expected;
    std::size_t pass_texture_inputs = 0;
    std::size_t pass_buffer_inputs = 0;
    // Binding order.
    std::vector<FullscreenShaderPassInput> shader_pass_inputs;
};

// what() starts with the sentence this refusal has always had ("Fullscreen
// pass input texture/local-read contract does not match shader reflection" or
// "Fullscreen pass input buffer does not match shader reflection") and goes
// on to name the pass, the input, and what the shader declares instead.
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
