#include "renderingpassvalidation.hpp"
#include "materialpassattachments.hpp"
#include "rendertargetcontainer.hpp"
#include <array>
#include <optional>
#include <stdexcept>

namespace Pelican {

void validatePassInputs(const PassDefinition &pass_def) {
    if (!pass_def.input_targets.empty() && !pass_def.isFullscreen()) {
        throw std::runtime_error("Only fullscreen passes support input targets: " + pass_def.name);
    }

    for (const auto &input_rt : pass_def.input_targets) {
        for (const auto &output_rt : pass_def.output_color) {
            if (input_rt == output_rt) {
                throw std::runtime_error("Pass cannot read and write the same color target: " + pass_def.name);
            }
        }

        if (input_rt == pass_def.output_depth) {
            throw std::runtime_error("Pass cannot read and write the same depth target: " + pass_def.name);
        }
    }
}

void validatePassTargetUsage(const PassDefinition &pass_def, RenderTargetContainer &rt_container) {
    for (const auto &rt_id : pass_def.output_color) {
        if (isSpecialRenderTarget(rt_id)) {
            continue;
        }

        const auto &rt = rt_container.get(rt_id);
        if (!(rt.usage & vk::ImageUsageFlagBits::eColorAttachment)) {
            throw std::runtime_error("Color output target missing COLOR_ATTACHMENT usage: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }

    if (isConcreteRenderTarget(pass_def.output_depth)) {
        const auto &rt = rt_container.get(pass_def.output_depth);
        if (!(rt.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
            throw std::runtime_error("Depth output target missing DEPTH_STENCIL_ATTACHMENT usage: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }

    for (const auto &rt_id : pass_def.input_targets) {
        const auto &rt = rt_container.get(rt_id);
        if (!(rt.usage & vk::ImageUsageFlagBits::eSampled)) {
            throw std::runtime_error("Input target missing SAMPLED usage: " + rt.name + " in pass: " +
                                     pass_def.name);
        }
    }
}

namespace {

std::string renderTargetDisplayName(GlobalRenderTargetId rt_id, RenderTargetContainer &rt_container) {
    if (isSwapchainRenderTarget(rt_id)) {
        return "swapchain";
    }
    return rt_container.get(rt_id).name;
}

} // namespace

void validateUniqueRenderTargets(const std::vector<GlobalRenderTargetId> &targets,
                                 const std::string &target_kind,
                                 const PassDefinition &pass_def,
                                 RenderTargetContainer &rt_container) {
    std::unordered_set<GlobalRenderTargetId, GlobalRenderTargetId::Hash> seen_targets;
    for (const auto &rt_id : targets) {
        if (!seen_targets.insert(rt_id).second) {
            throw std::runtime_error("Pass has duplicate " + target_kind + " target: " +
                                     renderTargetDisplayName(rt_id, rt_container) + " in pass: " + pass_def.name);
        }
    }
}

void validatePassOutputExtents(const PassDefinition &pass_def, RenderTargetContainer &rt_container) {
    std::optional<vk::Extent3D> expected_extent;

    const auto check_extent = [&](const auto &rt) {
        if (!expected_extent.has_value()) {
            expected_extent = rt.image.extent;
            return;
        }
        if (rt.image.extent.width != expected_extent->width ||
            rt.image.extent.height != expected_extent->height) {
            throw std::runtime_error("Pass output target extent mismatch: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    };

    for (const auto &rt_id : pass_def.output_color) {
        if (isConcreteRenderTarget(rt_id)) {
            check_extent(rt_container.get(rt_id));
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        check_extent(rt_container.get(pass_def.output_depth));
    }
}

void validateMaterialPassAttachments(const PassDefinition &pass_def, RenderTargetContainer &rt_container) {
    if (!pass_def.isMaterial()) {
        return;
    }

    if (pass_def.output_color.size() != materialPassColorAttachmentFormats.size()) {
        throw std::runtime_error("Material pass requires exactly five color outputs: " + pass_def.name);
    }
    if (!isConcreteRenderTarget(pass_def.output_depth)) {
        throw std::runtime_error("Material pass requires depth output: " + pass_def.name);
    }

    for (size_t i = 0; i < pass_def.output_color.size(); ++i) {
        const auto rt_id = pass_def.output_color[i];
        if (isSpecialRenderTarget(rt_id)) {
            throw std::runtime_error("Material pass does not support swapchain color output: " + pass_def.name);
        }

        const auto &rt = rt_container.get(rt_id);
        if (rt.image.format != materialPassColorAttachmentFormats[i]) {
            throw std::runtime_error("Material pass color output format mismatch: " + rt.name + " in pass: " +
                                     pass_def.name);
        }
    }

    const auto &depth_rt = rt_container.get(pass_def.output_depth);
    if (depth_rt.image.format != materialPassDepthAttachmentFormat) {
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

    if (pass_def.isUi()) {
        if (pass_def.output_color.size() != 1) {
            throw std::runtime_error("UI pass requires exactly one color output: " + pass_def.name);
        }
        if (isConcreteRenderTarget(pass_def.output_depth)) {
            throw std::runtime_error("UI pass does not support depth output: " + pass_def.name);
        }
    }
}

void validatePassSpecificFields(const PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() && pass_json.contains("material_range")) {
        throw std::runtime_error("Only material passes support material_range: " + pass_def.name);
    }

    if (pass_def.isFullscreen()) {
        return;
    }

    static constexpr std::array fullscreen_fields{
        "shader",
        "push_constants",
        "uses_light_data",
        "needs_projection_matrix",
    };
    for (const char *field : fullscreen_fields) {
        if (pass_json.contains(field)) {
            throw std::runtime_error("Only fullscreen passes support " + std::string{field} + ": " + pass_def.name);
        }
    }
}

void validatePassInputsProduced(const PassDefinition &pass_def,
                                const ProducedColorTargetSet &produced_color_targets,
                                RenderTargetContainer &rt_container) {
    for (const auto &rt_id : pass_def.input_targets) {
        if (produced_color_targets.find(rt_id) == produced_color_targets.end()) {
            const auto &rt = rt_container.get(rt_id);
            throw std::runtime_error("Pass input target is not produced as an earlier color output: " + rt.name +
                                     " in pass: " + pass_def.name);
        }
    }
}

void recordPassOutputs(const PassDefinition &pass_def, ProducedColorTargetSet &produced_color_targets) {
    for (const auto &rt_id : pass_def.output_color) {
        if (isConcreteRenderTarget(rt_id)) {
            produced_color_targets.insert(rt_id);
        }
    }
}

} // namespace Pelican
