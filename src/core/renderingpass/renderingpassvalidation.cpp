#include "renderingpassvalidation.hpp"
#include "materialpassattachments.hpp"
#include "rendertargetmetadataresolver.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace Pelican {

void validatePassInputs(const PassDefinition &pass_def) {
    if ((!pass_def.input_targets.empty() || !pass_def.input_buffers.empty()) &&
        !pass_def.isFullscreen() &&
        !pass_def.isGenericRaster() &&
        !pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only fullscreen, raster, and material passes support input targets: " +
            pass_def.name);
    }
    if (pass_def.isMaterial()) {
        if (!pass_def.input_buffers.empty()) {
            for (const auto &buffer :
                 pass_def.input_buffers) {
                const auto named = std::find_if(
                    pass_def.materialInfo()
                        .material_resources.begin(),
                    pass_def.materialInfo()
                        .material_resources.end(),
                    [&](const auto &resource) {
                        return resource.buffer == buffer;
                    });
                if (named ==
                    pass_def.materialInfo()
                        .material_resources.end()) {
                    throw std::runtime_error(
                        "Material pass buffer inputs must use named "
                        "material_resources: " +
                        pass_def.name);
                }
            }
        }
        if (!pass_def.input_targets.empty() &&
            pass_def.materialInfo().screen_inputs.empty() &&
            pass_def.materialInfo().surface_resources.empty() &&
            pass_def.materialInfo().material_resources.empty()) {
            throw std::runtime_error(
                "Material pass inputs must use named screen_inputs or "
                "surface_resources/material_resources: " +
                pass_def.name);
        }
    }

    if (pass_def.input_target_history.size() != pass_def.input_targets.size()) {
        throw std::runtime_error("Pass input history metadata is inconsistent: " + pass_def.name);
    }
    for (size_t input_index = 0; input_index < pass_def.input_targets.size(); ++input_index) {
        const auto input_rt = pass_def.input_targets[input_index];
        if (pass_def.input_target_history[input_index]) continue;
        for (const auto &output_rt : pass_def.output_color) {
            if (input_rt.value ==
                output_rt.target.value) {
                throw std::runtime_error("Pass cannot read and write the same color target: " + pass_def.name);
            }
        }

        if (input_rt.value ==
            pass_def.output_depth.target.value) {
            throw std::runtime_error("Pass cannot read and write the same depth target: " + pass_def.name);
        }
    }
}

void validatePassTargetUsage(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata) {
    for (const auto &rt_id : pass_def.output_color) {
        if (isSpecialRenderTarget(rt_id.target)) {
            continue;
        }

        const auto rt =
            rt_metadata.get(rt_id.target);
        if (!(rt.usage & vk::ImageUsageFlagBits::eColorAttachment)) {
            throw std::runtime_error("Color output target missing COLOR_ATTACHMENT usage: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }

    if (isConcreteRenderTarget(pass_def.output_depth)) {
        const auto rt = rt_metadata.get(pass_def.output_depth);
        if (!(rt.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
            throw std::runtime_error("Depth output target missing DEPTH_STENCIL_ATTACHMENT usage: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }

    for (size_t i = 0; i < pass_def.input_targets.size(); ++i) {
        const auto rt_id = pass_def.input_targets[i];
        const auto rt = rt_metadata.get(rt_id);
        const auto local_read =
            rt.storage_mode ==
                RenderTargetStorageMode::
                    tile_local_attachment &&
            bool(
                rt.usage &
                vk::ImageUsageFlagBits::
                    eInputAttachment);
        if (!(rt.usage &
              vk::ImageUsageFlagBits::eSampled) &&
            !local_read) {
            throw std::runtime_error(
                "Input target missing SAMPLED or tile-local "
                "INPUT_ATTACHMENT usage: " +
                rt.name + " in pass: " +
                pass_def.name);
        }
        if (pass_def.input_target_history.at(i) && !rt.history) {
            throw std::runtime_error("@history input requires a history render target: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }
}

namespace {

std::string renderTargetDisplayName(GlobalRenderTargetId rt_id, const RenderTargetMetadataResolver &rt_metadata) {
    if (isSwapchainRenderTarget(rt_id)) {
        return "swapchain";
    }
    return rt_metadata.get(rt_id).name;
}

} // namespace

void validateUniqueRenderTargets(const std::vector<RasterAttachmentView> &targets,
                                 const std::string &target_kind,
                                 const PassDefinition &pass_def,
                                 const RenderTargetMetadataResolver &rt_metadata) {
    std::unordered_set<GlobalRenderTargetId, GlobalRenderTargetId::Hash> seen_targets;
    for (const auto &attachment : targets) {
        if (!seen_targets
                 .insert(attachment.target)
                 .second) {
            throw std::runtime_error("Pass has duplicate " + target_kind + " target: " +
                                     renderTargetDisplayName(attachment.target, rt_metadata) + " in pass: " + pass_def.name);
        }
    }
}

void validateUniqueRenderTargets(
    const std::vector<GlobalRenderTargetId> &targets,
    const std::string &target_kind,
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata) {
    std::unordered_set<
        GlobalRenderTargetId,
        GlobalRenderTargetId::Hash>
        seen_targets;
    for (const auto target : targets) {
        if (!seen_targets.insert(target).second) {
            throw std::runtime_error(
                "Pass has duplicate " + target_kind +
                " target: " +
                renderTargetDisplayName(
                    target, rt_metadata) +
                " in pass: " + pass_def.name);
        }
    }
}

void validatePassOutputExtents(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata) {
    std::optional<vk::Extent2D> expected_extent;

    const auto check_extent =
        [&](const RenderTargetMetadata &rt,
            const std::optional<
                ImageSubresourceRange>
                &subresource) {
        if (subresource &&
            (subresource->mip_count_mode !=
                 ImageSubresourceMipCountMode::fixed ||
             subresource->level_count != 1 ||
             !validImageSubresourceRange(
                 *subresource, rt.mip_levels,
                 rt.array_layers) ||
             (rt.samples > 1 &&
              subresource->base_mip_level != 0))) {
            throw std::runtime_error(
                "Pass output target has an invalid raster "
                "subresource: " +
                rt.name + " in pass: " +
                pass_def.name);
        }
        const auto mip =
            subresource
                ? subresource->base_mip_level
                : 0u;
        const vk::Extent2D extent{
            std::max(1u, rt.extent.width >> mip),
            std::max(1u, rt.extent.height >> mip),
        };
        if (!expected_extent.has_value()) {
            expected_extent = extent;
            return;
        }
        if (extent != *expected_extent) {
            throw std::runtime_error("Pass output target extent mismatch: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    };

    for (const auto &attachment :
         pass_def.output_color) {
        if (isConcreteRenderTarget(
                attachment.target)) {
            check_extent(
                rt_metadata.get(
                    attachment.target),
                attachment.subresource);
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        check_extent(
            rt_metadata.get(
                pass_def.output_depth.target),
            pass_def.output_depth.subresource);
    }
}

vk::SampleCountFlagBits resolvePassOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata) {
    std::optional<std::uint32_t> expected;
    const auto check = [&](std::uint32_t samples,
                           const std::string &target) {
        if (!expected) {
            expected = samples;
            return;
        }
        if (*expected != samples) {
            throw std::runtime_error(
                "Pass output target sample-count mismatch at " + target +
                " in pass: " + pass_def.name);
        }
    };
    for (const auto &attachment :
         pass_def.output_color) {
        if (isSwapchainRenderTarget(
                attachment.target)) {
            check(1, "swapchain");
        } else if (isConcreteRenderTarget(
                       attachment.target)) {
            const auto metadata =
                rt_metadata.get(
                    attachment.target);
            check(metadata.samples, metadata.name);
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        const auto metadata = rt_metadata.get(pass_def.output_depth);
        check(metadata.samples, metadata.name);
    }
    switch (expected.value_or(1)) {
    case 1: return vk::SampleCountFlagBits::e1;
    case 2: return vk::SampleCountFlagBits::e2;
    case 4: return vk::SampleCountFlagBits::e4;
    case 8: return vk::SampleCountFlagBits::e8;
    case 16: return vk::SampleCountFlagBits::e16;
    case 32: return vk::SampleCountFlagBits::e32;
    case 64: return vk::SampleCountFlagBits::e64;
    default:
        throw std::runtime_error(
            "Pass output target has invalid sample count in pass: " +
            pass_def.name);
    }
}

void validateMaterialPassAttachments(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_def.isMaterial()) {
        return;
    }

    const auto contract = pass_def.materialInfo().contract;
    const auto &output_schema =
        pass_def.materialInfo().output_schema;
    const auto &output_states =
        pass_def.materialInfo().output_states;
    if (!output_states.empty() && !output_schema) {
        throw std::runtime_error(
            "Material pass material_output_states requires "
            "material_outputs: " +
            pass_def.name);
    }
    if (output_schema) {
        validateMaterialOutputAttachmentStates(
            output_states, *output_schema,
            "Material pass '" + pass_def.name +
                "' material_output_states");
        if (contract ==
            MaterialPassContract::legacy_gbuffer_v1) {
            throw std::runtime_error(
                "Material pass material_outputs requires an "
                "explicit non-legacy material_contract: " +
                pass_def.name);
        }
        if (contract ==
                MaterialPassContract::
                    deferred_geometry_v1 &&
            std::any_of(
                output_schema->outputs.begin(),
                output_schema->outputs.end(),
                [](const auto &output) {
                    return output.source ==
                           MaterialOutputSource::
                               lighting_scene_color;
                })) {
            throw std::runtime_error(
                "Deferred material pass material_outputs cannot "
                "use lighting.scene_color: " +
                pass_def.name);
        }
        if (pass_def.output_color.size() !=
            output_schema->outputs.size()) {
            throw std::runtime_error(
                "Material pass output.color count must match "
                "material_outputs.outputs count (" +
                std::to_string(
                    output_schema->outputs.size()) +
                "): " + pass_def.name);
        }
        if (!isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error(
                "Material pass requires depth output: " +
                pass_def.name);
        }
        for (std::size_t location = 0;
             location < pass_def.output_color.size();
             ++location) {
            const auto target =
                pass_def.output_color[location];
            if (isSpecialRenderTarget(
                    target.target)) {
                throw std::runtime_error(
                    "Material pass does not support swapchain "
                    "color output: " +
                    pass_def.name);
            }
            const auto metadata =
                rt_metadata.get(target.target);
            const auto format_name =
                vk::to_string(metadata.format);
            const auto format_class =
                format_name.ends_with("Sint")
                    ? MaterialOutputNumericClass::
                          signed_integer
                    : format_name.ends_with("Uint")
                          ? MaterialOutputNumericClass::
                                unsigned_integer
                          : MaterialOutputNumericClass::
                                floating;
            const auto output_class =
                materialOutputNumericClass(
                    output_schema->outputs[location].type);
            if (format_class != output_class) {
                throw std::runtime_error(
                    "Material pass output '" +
                    output_schema->outputs[location].name +
                    "' numeric class is incompatible with render "
                    "target format " +
                    format_name + ": " + metadata.name +
                    " in pass: " + pass_def.name);
            }
        }
        return;
    }
    const bool forward = materialPassShaderContract(contract) ==
                         MaterialShaderContract::forward_scene_color_v1;
    const auto expected_color_count = forward ? std::size_t{1}
                                              : materialPassColorAttachmentFormatsSdr.size();
    if (pass_def.output_color.size() != expected_color_count) {
        throw std::runtime_error("Material pass contract '" +
                                 std::string{materialPassContractName(contract)} +
                                 "' requires exactly " + std::to_string(expected_color_count) +
                                 " color output" + (expected_color_count == 1 ? "" : "s") +
                                 ": " + pass_def.name);
    }
    if (!isConcreteRenderTarget(pass_def.output_depth)) {
        throw std::runtime_error("Material pass requires depth output: " + pass_def.name);
    }

    const auto first_format =
        rt_metadata
            .get(pass_def.output_color.front().target)
            .format;
    const bool hdr = first_format == vk::Format::eR16G16B16A16Sfloat;
    const auto &expected_formats = materialPassColorAttachmentFormats(hdr);
    for (size_t i = 0; i < pass_def.output_color.size(); ++i) {
        const auto rt_id = pass_def.output_color[i];
        if (isSpecialRenderTarget(rt_id.target)) {
            throw std::runtime_error("Material pass does not support swapchain color output: " + pass_def.name);
        }

        const auto rt =
            rt_metadata.get(rt_id.target);
        const auto expected_format = forward ? forwardMaterialPassColorAttachmentFormat
                                             : expected_formats[i];
        if (rt.format != expected_format) {
            throw std::runtime_error("Material pass color output format mismatch: " + rt.name + " in pass: " +
                                     pass_def.name);
        }
    }

    const auto depth_rt = rt_metadata.get(pass_def.output_depth);
    if (depth_rt.format != materialPassDepthAttachmentFormat) {
        throw std::runtime_error("Material pass depth output format mismatch: " + depth_rt.name +
                                 " in pass: " + pass_def.name);
    }
}

void validatePassOutputs(const PassDefinition &pass_def) {
    if (pass_def.output_color.empty() && !isConcreteRenderTarget(pass_def.output_depth)) {
        throw std::runtime_error("Pass must output color or depth: " + pass_def.name);
    }

    if (pass_def.isFullscreen()) {
        if (pass_def.output_color.size() != 1) {
            throw std::runtime_error("Fullscreen pass requires exactly one color output: " + pass_def.name);
        }
        if (isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error("Fullscreen pass does not support depth output: " + pass_def.name);
        }
    }

    if (pass_def.isShadowDepth()) {
        if (!pass_def.output_color.empty()) {
            throw std::runtime_error("Shadow depth pass does not support color output: " + pass_def.name);
        }
        if (!isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error("Shadow depth pass requires depth output: " + pass_def.name);
        }
    }

    if (pass_def.isVelocity()) {
        if (pass_def.output_color.size() != 1 || !isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error("Velocity pass requires one color and one depth output: " +
                                     pass_def.name);
        }
    }

    if (pass_def.isDebugDraw() || pass_def.isDebugText() || pass_def.isUi()
#if PELICAN_WITH_IMGUI
        || pass_def.isImGui()
#endif
    ) {
        if (pass_def.output_color.size() != 1) {
            throw std::runtime_error("Single-color pass requires exactly one color output: " + pass_def.name);
        }
        if (isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error("Single-color pass does not support depth output: " + pass_def.name);
        }
    }
}

void validatePassSpecificFields(const PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() && pass_json.contains("material_range")) {
        throw std::runtime_error("Only material passes support material_range: " + pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("material_filter")) {
        throw std::runtime_error(
            "Only material passes support material_filter: " +
            pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("material_variant")) {
        throw std::runtime_error(
            "Only material passes support material_variant: " +
            pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("material_outputs")) {
        throw std::runtime_error(
            "Only material passes support material_outputs: " +
            pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("material_output_states")) {
        throw std::runtime_error(
            "Only material passes support material_output_states: " +
            pass_def.name);
    }
    if (!pass_def.isMaterial() && pass_json.contains("screen_inputs")) {
        throw std::runtime_error("Only material passes support screen_inputs: " +
                                 pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("surface_resources")) {
        throw std::runtime_error(
            "Only material passes support surface_resources: " +
            pass_def.name);
    }
    if (!pass_def.isMaterial() &&
        pass_json.contains("material_resources")) {
        throw std::runtime_error(
            "Only material passes support material_resources: " +
            pass_def.name);
    }
    if (!pass_def.isFullscreen() &&
        pass_json.contains("input_sampling")) {
        throw std::runtime_error(
            "Only fullscreen passes support input_sampling: " +
            pass_def.name);
    }
    if (!pass_def.isFullscreen() &&
        !pass_def.isGenericRaster() &&
        pass_json.contains("resource_ports")) {
        throw std::runtime_error(
            "Only fullscreen and raster passes support resource_ports: " +
            pass_def.name);
    }
    if (!pass_def.isGenericRaster() &&
        pass_json.contains("draw")) {
        throw std::runtime_error(
            "Only raster passes support draw: " +
            pass_def.name);
    }
    if (!pass_def.isGenericRaster() &&
        pass_json.contains("raster_state")) {
        throw std::runtime_error(
            "Only raster passes support raster_state: " +
            pass_def.name);
    }

    if (pass_json.contains("needs_projection_matrix")) {
        throw std::runtime_error(
            "Pass field needs_projection_matrix is deprecated; use push_constants: projection_view: " +
            pass_def.name);
    }

    if (!pass_def.isFullscreen() &&
        !pass_def.isGenericRaster() &&
        !pass_def.isDebugDraw() && !pass_def.isDebugText() &&
        !pass_def.isShadowDepth() && !pass_def.isVelocity() &&
        pass_json.contains("shader")) {
        throw std::runtime_error("Only fullscreen, raster, debug_draw, debug_text, shadow_depth, and velocity passes support shader: " +
                                 pass_def.name);
    }

    if (pass_def.isFullscreen()) {
        return;
    }

    static constexpr std::array fullscreen_only_fields{
        "push_constants",
        "uses_light_data",
    };
    for (const char *field : fullscreen_only_fields) {
        if (pass_json.contains(field)) {
            throw std::runtime_error("Only fullscreen passes support " + std::string{field} + ": " + pass_def.name);
        }
    }
}

void validatePassInputsProduced(const PassDefinition &pass_def,
                                const ProducedRenderTargetSet &produced_targets,
                                const RenderTargetMetadataResolver &rt_metadata) {
    for (size_t i = 0; i < pass_def.input_targets.size(); ++i) {
        const auto rt_id = pass_def.input_targets[i];
        if (pass_def.input_target_history.at(i)) continue;
        if (produced_targets.find(rt_id) == produced_targets.end()) {
            const auto rt = rt_metadata.get(rt_id);
            throw std::runtime_error("Pass input target is not produced as an earlier output: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }
}

void recordPassOutputs(const PassDefinition &pass_def, ProducedRenderTargetSet &produced_targets) {
    for (const auto &rt_id : pass_def.output_color) {
        if (isConcreteRenderTarget(
                rt_id.target)) {
            produced_targets.insert(
                rt_id.target);
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        produced_targets.insert(
            pass_def.output_depth.target);
    }
}

} // namespace Pelican
