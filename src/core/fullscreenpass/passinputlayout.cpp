#include "passinputlayout.hpp"

#include "../shader/pelican_sets.hpp"
#include "../shader/shaderreflection.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <utility>

namespace Pelican {

std::string passInputDescriptorTypeName(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eCombinedImageSampler:
        return "sampled_image";
    case vk::DescriptorType::eInputAttachment:
        return "input_attachment";
    case vk::DescriptorType::eStorageBuffer:
        return "storage_buffer";
    case vk::DescriptorType::eStorageBufferDynamic:
        return "storage_buffer_dynamic";
    case vk::DescriptorType::eUniformBuffer:
        return "uniform_buffer";
    case vk::DescriptorType::eUniformBufferDynamic:
        return "uniform_buffer_dynamic";
    case vk::DescriptorType::eSampledImage:
        return "separate_image";
    case vk::DescriptorType::eSampler:
        return "sampler";
    case vk::DescriptorType::eStorageImage:
        return "storage_image";
    case vk::DescriptorType::eUniformTexelBuffer:
        return "uniform_texel_buffer";
    case vk::DescriptorType::eStorageTexelBuffer:
        return "storage_texel_buffer";
    case vk::DescriptorType::eAccelerationStructureKHR:
        return "acceleration_structure";
    default:
        return "other";
    }
}

std::string passInputDeclarationKind(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eInputAttachment:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eSampler:
        return "texture";
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eStorageBufferDynamic:
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return "buffer";
    default:
        return "other";
    }
}

namespace {

// k of pelican_variadic_input_<k>; nullopt for any other name.
std::optional<std::uint32_t> variadicIndex(std::string_view name) {
    if (!name.starts_with(passVariadicInputNamePrefix)) return std::nullopt;
    const auto digits = name.substr(passVariadicInputNamePrefix.size());
    if (digits.empty() || digits.size() > 4) return std::nullopt;
    std::uint32_t value = 0;
    for (const auto ch : digits) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    return value;
}

[[noreturn]] void refuseLayout(PassInputLayout layout,
                               const ReflectedBinding &offending,
                               const char *rule) {
    FullscreenInputBindingMismatchInfo info{
        .direction = fullscreenInputVariadicLayoutInvalid,
        .kind = passInputDeclarationKind(offending.type),
        .binding = offending.binding,
        .expected = "none",
        .shader_pass_inputs = std::move(layout.declarations),
        .variadic_range = layout.variadic,
        .variadic_rule = rule,
    };
    throw FullscreenInputBindingMismatch{std::move(info)};
}

} // namespace

PassInputLayout readPassInputLayout(const ShaderReflection &reflection) {
    std::vector<const ReflectedBinding *> declared;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == PELICAN_SET_PASS_INPUT) {
            declared.push_back(&reflected);
        }
    }
    std::stable_sort(declared.begin(), declared.end(),
                     [](const ReflectedBinding *a, const ReflectedBinding *b) {
                         return a->binding < b->binding;
                     });

    PassInputLayout layout;
    // The group's first binding each variadic declaration implies
    // (binding - k), and its k.
    std::map<std::uint32_t, std::vector<std::uint32_t>> groups;
    for (const auto *reflected : declared) {
        const auto k = variadicIndex(reflected->name);
        layout.declarations.push_back(FullscreenShaderPassInput{
            .binding = reflected->binding,
            .type = passInputDescriptorTypeName(reflected->type),
            .name = reflected->name,
            .variadic = k.has_value(),
            .accessed = reflected->accessed,
        });
        if (k && *k <= reflected->binding) {
            groups[reflected->binding - *k].push_back(*k);
        }
    }
    if (groups.empty()) {
        // A name with the prefix whose k exceeds its binding cannot be one
        // the macro made.
        for (const auto *reflected : declared) {
            if (variadicIndex(reflected->name)) {
                refuseLayout(std::move(layout), *reflected,
                             passVariadicRuleMalformed);
            }
        }
        return layout;
    }
    layout.variadic = PassVariadicInputRange{
        .first = groups.begin()->first,
        .max = static_cast<std::uint32_t>(groups.begin()->second.size()),
    };
    if (groups.size() > 1) {
        const auto second = std::next(groups.begin())->first;
        const auto offending = std::find_if(
            declared.begin(), declared.end(),
            [&](const ReflectedBinding *reflected) {
                const auto k = variadicIndex(reflected->name);
                return k && *k <= reflected->binding &&
                       reflected->binding - *k == second;
            });
        refuseLayout(std::move(layout), **offending,
                     passVariadicRuleMultipleGroups);
    }
    const auto range = *layout.variadic;
    // Every declaration the macro made: a 2D sampled image, one per k with
    // no gap.
    std::vector<bool> seen(range.max, false);
    for (const auto *reflected : declared) {
        const auto k = variadicIndex(reflected->name);
        if (!k) continue;
        const bool well_formed =
            *k <= reflected->binding &&
            reflected->binding - *k == range.first && *k < range.max &&
            !seen[*k] &&
            reflected->type == vk::DescriptorType::eCombinedImageSampler &&
            reflected->image_view_dimension ==
                ReflectedImageViewDimension::two_d &&
            reflected->count == 1;
        if (!well_formed) {
            refuseLayout(std::move(layout), *reflected,
                         passVariadicRuleMalformed);
        }
        seen[*k] = true;
    }
    // Fixed declarations: textures before the group, buffers after it.
    for (const auto *reflected : declared) {
        if (variadicIndex(reflected->name)) continue;
        if (reflected->binding < range.first) continue;
        const bool inside = reflected->binding < range.end();
        if (inside || passInputDeclarationKind(reflected->type) != "buffer") {
            refuseLayout(std::move(layout), *reflected,
                         passVariadicRuleDeclarationAfterGroup);
        }
    }
    return layout;
}

std::optional<PassVariadicInputRange> findPassVariadicInputRange(
    const ShaderReflection &reflection) {
    std::map<std::uint32_t, std::uint32_t> groups;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set != PELICAN_SET_PASS_INPUT) continue;
        const auto k = variadicIndex(reflected.name);
        if (k && *k <= reflected.binding) ++groups[reflected.binding - *k];
    }
    if (groups.empty()) return std::nullopt;
    return PassVariadicInputRange{.first = groups.begin()->first,
                                  .max = groups.begin()->second};
}

std::uint32_t passBufferInputBinding(
    std::size_t texture_inputs,
    const std::optional<PassVariadicInputRange> &variadic,
    std::size_t buffer_index) {
    const auto base = variadic ? static_cast<std::size_t>(variadic->end())
                               : texture_inputs;
    return static_cast<std::uint32_t>(base + buffer_index);
}

bool compileLogShowsMultipleVariadicGroups(std::string_view log) {
    return log.find("'pelican_variadic_first' : redefinition") !=
           std::string_view::npos;
}

FullscreenInputBindingMismatch multipleVariadicGroupsCompileMismatch() {
    return FullscreenInputBindingMismatch{FullscreenInputBindingMismatchInfo{
        .direction = fullscreenInputVariadicLayoutInvalid,
        .kind = "texture",
        .binding = 0,
        .expected = "none",
        .variadic_rule = passVariadicRuleMultipleGroups,
    }};
}

} // namespace Pelican
