#pragma once

#include "fullscreeninputbindingmismatch.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct ShaderReflection;

// VI-1: PELICAN_DECLARE_VARIADIC_INPUTS names each input of its group
// pelican_variadic_input_<k>; the engine tells the group apart from fixed
// declarations by that prefix in reflection (pelican_view.glsl).
inline constexpr std::string_view passVariadicInputNamePrefix =
    "pelican_variadic_input_";
// The largest `max` the macro expands (PELICAN_VARIADIC_INPUTS_LIMIT).
inline constexpr std::uint32_t passVariadicInputLimit = 16;

// The words a pass-input declaration's descriptor type is reported in
// ("sampled_image", "input_attachment", "storage_buffer", ...).
std::string passInputDescriptorTypeName(vk::DescriptorType type);
// "texture" (an image, sampler or input attachment), "buffer" (a storage,
// uniform or texel buffer) or "other".
std::string passInputDeclarationKind(vk::DescriptorType type);

// A shader's pass-input set (PELICAN_SET_PASS_INPUT) as the pass binds it.
struct PassInputLayout {
    // Binding order; a variadic declaration has `variadic` set.
    std::vector<FullscreenShaderPassInput> declarations;
    std::optional<PassVariadicInputRange> variadic;
};

// Reads the pass-input set from reflection and checks the variadic layout
// rule: fixed texture inputs first, at most one variadic group and nothing
// but buffers past it. A shader that breaks it is refused as a
// FullscreenInputBindingMismatch (direction variadic_layout_invalid) that
// names neither the pass nor the fragment; the pass compiler adds them.
PassInputLayout readPassInputLayout(const ShaderReflection &reflection);

// The lowest variadic group the declarations name, without checking the
// rule (readPassInputLayout does). The pass compiler asks this of a shader
// it compiled with provisional buffer bindings.
std::optional<PassVariadicInputRange> findPassVariadicInputRange(
    const ShaderReflection &reflection);

// VI-1: the one rule for where a pass's buffer inputs bind. Without a
// variadic group, buffer i follows the texture inputs (texture count + i);
// with one, it follows the group's last binding (first + max + i), however
// many textures the pass supplies. The pass container's check and
// descriptor writes and the pass compiler's resource-port include all ask
// this.
std::uint32_t passBufferInputBinding(
    std::size_t texture_inputs,
    const std::optional<PassVariadicInputRange> &variadic,
    std::size_t buffer_index);

// VI-1: PELICAN_DECLARE_VARIADIC_INPUTS used twice does not compile (its
// first constant is redefined); this reads that out of a compile log.
bool compileLogShowsMultipleVariadicGroups(std::string_view log);
// The refusal for it, without the pass and fragment.
FullscreenInputBindingMismatch multipleVariadicGroupsCompileMismatch();

} // namespace Pelican
