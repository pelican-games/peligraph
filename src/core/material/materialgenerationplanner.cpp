#include "materialgenerationplanner.hpp"
#include "materialdrawlooptable.hpp"
#include "materiallimits.hpp"
#include "../loader/basicconfig.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/surfacecompiler.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace Pelican {

namespace {

std::string activeRenderingConfigReference() {
    const auto *project_config =
        FastModuleContainer::tryGet<ProjectBasicConfig>();
    return project_config == nullptr
               ? std::string{"<active rendering configuration>"}
               : project_config->renderingConfigReference();
}

vk::Format resolveMaterialColorFormat(
    GlobalRenderTargetId target) {
    if (isConcreteRenderTarget(target)) {
        return GET_MODULE(RenderTargetContainer)
            .getMetadata(target)
            .format;
    }
    if (isSwapchainRenderTarget(target)) {
        return GET_MODULE(RenderTarget)
            .getSwapchainFormat();
    }
    throw std::runtime_error(
        "material physical rendering contract has an invalid "
        "color target");
}

vk::BlendFactor toVkBlendFactor(
    MaterialOutputBlendFactor factor) {
    switch (factor) {
    case MaterialOutputBlendFactor::zero:
        return vk::BlendFactor::eZero;
    case MaterialOutputBlendFactor::one:
        return vk::BlendFactor::eOne;
    case MaterialOutputBlendFactor::source_color:
        return vk::BlendFactor::eSrcColor;
    case MaterialOutputBlendFactor::one_minus_source_color:
        return vk::BlendFactor::eOneMinusSrcColor;
    case MaterialOutputBlendFactor::destination_color:
        return vk::BlendFactor::eDstColor;
    case MaterialOutputBlendFactor::
        one_minus_destination_color:
        return vk::BlendFactor::eOneMinusDstColor;
    case MaterialOutputBlendFactor::source_alpha:
        return vk::BlendFactor::eSrcAlpha;
    case MaterialOutputBlendFactor::one_minus_source_alpha:
        return vk::BlendFactor::eOneMinusSrcAlpha;
    case MaterialOutputBlendFactor::destination_alpha:
        return vk::BlendFactor::eDstAlpha;
    case MaterialOutputBlendFactor::
        one_minus_destination_alpha:
        return vk::BlendFactor::eOneMinusDstAlpha;
    case MaterialOutputBlendFactor::source_alpha_saturate:
        return vk::BlendFactor::eSrcAlphaSaturate;
    }
    throw std::runtime_error(
        "unknown material output blend factor");
}

vk::BlendOp toVkBlendOperation(
    MaterialOutputBlendOperation operation) {
    switch (operation) {
    case MaterialOutputBlendOperation::add:
        return vk::BlendOp::eAdd;
    case MaterialOutputBlendOperation::subtract:
        return vk::BlendOp::eSubtract;
    case MaterialOutputBlendOperation::reverse_subtract:
        return vk::BlendOp::eReverseSubtract;
    case MaterialOutputBlendOperation::minimum:
        return vk::BlendOp::eMin;
    case MaterialOutputBlendOperation::maximum:
        return vk::BlendOp::eMax;
    }
    throw std::runtime_error(
        "unknown material output blend operation");
}

vk::ColorComponentFlags toVkWriteMask(
    std::uint8_t mask) {
    vk::ColorComponentFlags result;
    if ((mask & materialOutputWriteRed) != 0)
        result |= vk::ColorComponentFlagBits::eR;
    if ((mask & materialOutputWriteGreen) != 0)
        result |= vk::ColorComponentFlagBits::eG;
    if ((mask & materialOutputWriteBlue) != 0)
        result |= vk::ColorComponentFlagBits::eB;
    if ((mask & materialOutputWriteAlpha) != 0)
        result |= vk::ColorComponentFlagBits::eA;
    return result;
}

GraphicsPipelineColorAttachmentState
defaultMaterialColorAttachmentState(
    SurfaceBlendMode blend) {
    GraphicsPipelineColorAttachmentState result;
    if (blend == SurfaceBlendMode::blend) {
        result.blend_enabled = true;
        result.source_color =
            vk::BlendFactor::eSrcAlpha;
        result.destination_color =
            vk::BlendFactor::eOneMinusSrcAlpha;
        result.source_alpha =
            vk::BlendFactor::eOne;
        result.destination_alpha =
            vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (blend ==
               SurfaceBlendMode::additive) {
        result.blend_enabled = true;
        result.source_color =
            vk::BlendFactor::eSrcAlpha;
        result.destination_color =
            vk::BlendFactor::eOne;
        result.source_alpha =
            vk::BlendFactor::eOne;
        result.destination_alpha =
            vk::BlendFactor::eOne;
    }
    return result;
}

void appendSurfaceResourceLocalReadDefines(
    std::vector<std::string> &defines,
    std::size_t resource_index,
    std::uint32_t input_attachment_index,
    const std::optional<vk::Extent2D> &extent,
    std::string_view resource_name) {
    if (!extent) {
        throw std::runtime_error(
            "material resource port '" +
            std::string{resource_name} +
            "' input attachment has no resolved extent");
    }
    const auto generated =
        makeSurfaceResourceLocalReadDefines(
            resource_index,
            input_attachment_index, *extent);
    defines.insert(
        defines.end(), generated.begin(),
        generated.end());
}

} // namespace

MaterialPipelineRenderingContract
defaultMaterialPipelineRenderingContract(
    MaterialShaderContract shader_contract,
    vk::SampleCountFlagBits samples) {
    MaterialPipelineRenderingContract result;
    if (shader_contract ==
        MaterialShaderContract::forward_scene_color_v1) {
        result.color_formats.push_back(
            forwardMaterialPassColorAttachmentFormat);
    } else {
        const auto &formats =
            materialPassColorAttachmentFormats(
                GET_MODULE(RenderingPassContainer)
                    .isFeatureEnabled("hdr"));
        result.color_formats.assign(
            formats.begin(), formats.end());
    }
    result.depth_format =
        materialPassDepthAttachmentFormat;
    result.rasterization_samples = samples;
    return result;
}

MaterialPipelineRenderingContract
resolveMaterialPassRenderingBinding(
    const MaterialPassRenderingBinding &binding,
    MaterialShaderContract shader_contract) {
    if (binding.rendering.color_attachments.empty()) {
        auto result =
            defaultMaterialPipelineRenderingContract(
                shader_contract,
                binding.rasterization_samples);
        result.output_states = binding.output_states;
        return result;
    }

    MaterialPipelineRenderingContract result;
    result.rasterization_samples =
        binding.rasterization_samples;
    result.output_states = binding.output_states;
    result.color_formats.reserve(
        binding.rendering.color_attachments.size());
    for (const auto target :
         binding.rendering.color_attachments) {
        result.color_formats.push_back(
            resolveMaterialColorFormat(target));
    }
    if (isConcreteRenderTarget(
            binding.rendering.depth_attachment)) {
        result.depth_format =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(
                    binding.rendering.depth_attachment)
                .format;
    } else if (isSwapchainRenderTarget(
                   binding.rendering.depth_attachment)) {
        throw std::runtime_error(
            "material physical rendering contract cannot use the "
            "swapchain as a depth attachment: " +
            binding.pass_name);
    }

    result.local_read.enabled =
        binding.rendering.local_read_scope;
    if (!result.local_read.enabled) {
        return result;
    }
    result.local_read.color_attachment_locations =
        binding.rendering.color_attachment_locations;
    result.local_read.color_attachment_input_indices =
        binding.rendering
            .color_attachment_input_indices;
    result.local_read.depth_attachment_input_index =
        binding.rendering
            .depth_attachment_input_index;
    return result;
}

MaterialPipelineRenderingContract
resolveMaterialPipelineRenderingContract(
    const MaterialInfo &info) {
    const auto &rendering_passes =
        GET_MODULE(RenderingPassContainer);
    const auto bindings =
        rendering_passes
            .materialPassRenderingBindings(
                info.route, info.shader_contract,
                info.exact_pass);
    if (bindings.empty()) {
        if (rendering_passes.hasMaterialPasses()) {
            // G-4: the same rule, code and hint as an edit ticket's
            // candidate check; at load it stays fatal, in readable text.
            auto text = materialRouteUnservedText(
                {UnservedMaterialRoute{
                    info.route, info.shader_contract,
                    {describeUnservedMaterial(
                        info.name, info.source_document,
                        info.source_asset)}}},
                activeRenderingConfigReference());
            if (info.exact_pass) {
                text += " (the material selects exact pass '" +
                        *info.exact_pass + "')";
            }
            throw std::runtime_error(text);
        }
        return defaultMaterialPipelineRenderingContract(
            info.shader_contract,
            rendering_passes
                .materialRasterizationSamples(
                    info.shader_contract));
    }

    for (const auto &binding : bindings) {
        if (binding.output_schema !=
            info.output_schema) {
            const auto shader_schema =
                info.output_schema
                    ? "'" + info.output_schema->name + "'"
                    : std::string{"<built-in>"};
            const auto pass_schema =
                binding.output_schema
                    ? "'" + binding.output_schema->name + "'"
                    : std::string{"<built-in>"};
            throw std::runtime_error(
                "material fragment-output schema " +
                shader_schema +
                " is incompatible with pass '" +
                binding.pass_name + "' schema " +
                pass_schema);
        }
    }

    auto result =
        resolveMaterialPassRenderingBinding(
            bindings.front(),
            info.shader_contract);
    for (std::size_t index = 1;
         index < bindings.size(); ++index) {
        const auto candidate =
            resolveMaterialPassRenderingBinding(
                bindings[index],
                info.shader_contract);
        if (candidate != result) {
            throw std::runtime_error(
                "material route resolves to pipeline-incompatible "
                "physical rendering contracts: " +
                bindings.front().pass_name + " and " +
                bindings[index].pass_name);
        }
    }
    return result;
}

std::vector<GraphicsPipelineColorAttachmentState>
resolveMaterialColorAttachmentStates(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract &rendering) {
    std::vector<GraphicsPipelineColorAttachmentState>
        result(
            rendering.color_formats.size(),
            defaultMaterialColorAttachmentState(
                info.render_state.blend));
    if (rendering.output_states.empty()) {
        return result;
    }
    if (!info.output_schema) {
        throw std::logic_error(
            "material output attachment states require an "
            "output schema");
    }
    if (info.output_schema->outputs.size() !=
        result.size()) {
        throw std::runtime_error(
            "material output attachment state count is "
            "incompatible with the physical color attachments");
    }
    validateMaterialOutputAttachmentStates(
        rendering.output_states,
        *info.output_schema,
        "material pipeline output states");
    for (const auto &override :
         rendering.output_states) {
        const auto output = std::find_if(
            info.output_schema->outputs.begin(),
            info.output_schema->outputs.end(),
            [&](const auto &field) {
                return field.name == override.output;
            });
        if (output ==
            info.output_schema->outputs.end()) {
            throw std::logic_error(
                "validated material output state is absent "
                "from its schema");
        }
        const auto location =
            static_cast<std::size_t>(
                std::distance(
                    info.output_schema->outputs.begin(),
                    output));
        auto &state = result[location];
        if (override.blend) {
            state.blend_enabled =
                override.blend->enabled;
            state.source_color =
                toVkBlendFactor(
                    override.blend->color.source);
            state.destination_color =
                toVkBlendFactor(
                    override.blend->color.destination);
            state.color_operation =
                toVkBlendOperation(
                    override.blend->color.operation);
            state.source_alpha =
                toVkBlendFactor(
                    override.blend->alpha.source);
            state.destination_alpha =
                toVkBlendFactor(
                    override.blend->alpha.destination);
            state.alpha_operation =
                toVkBlendOperation(
                    override.blend->alpha.operation);
        }
        if (override.write_mask) {
            state.write_mask =
                toVkWriteMask(
                    *override.write_mask);
        }
    }
    return result;
}

void validateMaterialCapabilities(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering) {
    const auto physical_device = GET_MODULE(VulkanManageCore).getPhysDevice();
    const auto limits = physical_device.getProperties().limits;
    if (rendering.color_formats.size() >
        limits.maxColorAttachments) {
        throw std::runtime_error(
            "material output schema requires " +
            std::to_string(
                rendering.color_formats.size()) +
            " color attachments but device "
            "maxColorAttachments is " +
            std::to_string(limits.maxColorAttachments));
    }
    const auto reserved = info.vat ? vatMaterialTextureBindingCount : baseMaterialTextureBindingCount;
    const auto sampler_count = static_cast<std::uint32_t>(reserved + info.custom_textures.size());
    if (sampler_count > limits.maxPerStageDescriptorSamplers ||
        sampler_count > limits.maxDescriptorSetSamplers) {
        throw std::runtime_error("material custom textures require " +
                                 std::to_string(sampler_count) +
                                 " samplers but device descriptor limit is " +
                                 std::to_string(std::min(limits.maxPerStageDescriptorSamplers,
                                                         limits.maxDescriptorSetSamplers)));
    }
    if (!info.render_state.depth_test && info.render_state.depth_write) {
        throw std::runtime_error(
            "material render_state requests depth_write while depth_test is disabled");
    }
    const auto attachment_states =
        resolveMaterialColorAttachmentStates(
            info, rendering);
    const auto independent_states =
        attachment_states.size() > 1 &&
        std::any_of(
            attachment_states.begin() + 1,
            attachment_states.end(),
            [&](const auto &state) {
                return state !=
                       attachment_states.front();
            });
    if (independent_states &&
        !GET_MODULE(VulkanManageCore)
             .getRuntimeCapabilities()
             .independent_blend) {
        throw std::runtime_error(
            "material output attachment states differ but the "
            "device does not support independentBlend");
    }
    for (std::size_t location = 0;
         location < rendering.color_formats.size();
         ++location) {
        const auto format =
            rendering.color_formats[location];
        const auto features =
            physical_device.getFormatProperties(format)
                .optimalTilingFeatures;
        if (!(features &
              vk::FormatFeatureFlagBits::
                  eColorAttachment)) {
            throw std::runtime_error(
                "material output format lacks device color "
                "attachment capability: " +
                vk::to_string(format));
        }
        if (attachment_states[location]
                .blend_enabled) {
            if (!(features & vk::FormatFeatureFlagBits::eColorAttachmentBlend)) {
                throw std::runtime_error(
                    "material output blend lacks device "
                    "capability for color format " +
                    vk::to_string(format) +
                    " at attachment " +
                    std::to_string(location));
            }
        }
    }
    if (info.render_state.depth_test ||
        info.render_state.depth_write) {
        if (!rendering.depth_format) {
            throw std::runtime_error(
                "material render_state requires a depth attachment");
        }
        const auto depth_features =
            physical_device
                .getFormatProperties(
                    *rendering.depth_format)
                .optimalTilingFeatures;
        if (!(depth_features &
              vk::FormatFeatureFlagBits::
                  eDepthStencilAttachment)) {
            throw std::runtime_error(
                "material render_state depth lacks device "
                "capability for format " +
                vk::to_string(
                    *rendering.depth_format));
        }
    }
}

std::vector<MaterialPassRenderingBinding>
materialBindingsForGeneration(
    const RendererRuntimeGeneration &generation,
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) {
    std::vector<MaterialPassRenderingBinding> bindings;
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Render-pipeline candidate pass table is "
                "inconsistent");
        }
        for (const auto &compiled :
             program->rendering_pass.passes) {
            const auto &pass = compiled.definition;
            if (!pass.isMaterial() ||
                !materialPassAcceptsMaterial(
                    pass.materialInfo().contract,
                    pass.name, route,
                    shader_contract, exact_pass)) {
                continue;
            }

            std::vector<MaterialPassShaderInputBinding>
                shader_inputs;
            const auto input_attachment_index =
                [&](GlobalRenderTargetId target,
                    bool history,
                    const LogicalReadFootprint &footprint)
                -> std::optional<std::uint32_t> {
                if (history) {
                    return std::nullopt;
                }
                const auto target_position = std::find(
                    pass.input_targets.begin(),
                    pass.input_targets.end(), target);
                if (target_position ==
                    pass.input_targets.end()) {
                    throw std::runtime_error(
                        "material pass input is absent from "
                        "the positional physical input table: " +
                        pass.name);
                }
                const auto index =
                    static_cast<std::uint32_t>(
                        target_position -
                        pass.input_targets.begin());
                const auto local =
                    std::find(
                        compiled.rendering
                            .color_attachment_input_indices
                            .begin(),
                        compiled.rendering
                            .color_attachment_input_indices
                            .end(),
                        index) !=
                        compiled.rendering
                            .color_attachment_input_indices
                            .end() ||
                    compiled.rendering
                            .depth_attachment_input_index ==
                        index;
                if (!local) {
                    return std::nullopt;
                }
                if (footprint.kind !=
                    LogicalReadFootprintKind::same_pixel) {
                    throw std::logic_error(
                        "material pass selected a local "
                        "attachment for a non-same-pixel "
                        "input: " +
                        pass.name);
                }
                return index;
            };
            const auto append_input =
                [&](MaterialPassShaderInputKind kind,
                    std::string name,
                    GlobalRenderTargetId target,
                    bool history,
                    const LogicalReadFootprint &footprint,
                    std::optional<
                        ShaderResourcePortView>
                        resource_view =
                            std::nullopt) {
                    auto view_dimension =
                        passInputViewDimensionForTarget(
                            pass, target);
                    if (resource_view ==
                        ShaderResourcePortView::
                            family_array) {
                        view_dimension =
                            PassInputViewDimension::
                                family_2d_array;
                    } else if (
                        resource_view ==
                        ShaderResourcePortView::
                            shared_2d) {
                        view_dimension =
                            PassInputViewDimension::
                                shared_2d;
                    }
                    const auto attachment =
                        input_attachment_index(
                            target, history,
                            footprint);
                    const auto descriptor_dimension =
                        attachment
                            ? ImageSubresourceViewDimension::
                                  two_d
                        : resource_view ==
                                  ShaderResourcePortView::
                                      cube
                            ? ImageSubresourceViewDimension::
                                  cube
                        : view_dimension ==
                                  PassInputViewDimension::
                                      layered_2d_array ||
                              view_dimension ==
                                  PassInputViewDimension::
                                      family_2d_array
                            ? ImageSubresourceViewDimension::
                                  two_d_array
                            : ImageSubresourceViewDimension::
                                  two_d;
                    shader_inputs.push_back({
                        .input = {
                            .kind = kind,
                            .name = std::move(name),
                        },
                        .input_attachment_index =
                            attachment,
                        .input_attachment_extent =
                            attachment
                                ? compiled.rendering
                                      .local_read_extent
                                : std::nullopt,
                        .view_dimension =
                            view_dimension,
                        .descriptor_dimension =
                            descriptor_dimension,
                    });
                };
            for (const auto &input :
                 pass.materialInfo().screen_inputs) {
                append_input(
                    MaterialPassShaderInputKind::
                        screen_input,
                    input.contract.name, input.target,
                    input.history,
                    input.contract.footprint);
            }
            for (const auto &resource :
                 pass.materialInfo()
                     .material_resources) {
                if (!resource.isImage()) {
                    continue;
                }
                append_input(
                    MaterialPassShaderInputKind::
                        material_resource,
                    resource.port.name,
                    resource.target,
                    resource.history,
                    resource.footprint,
                    resource.port.view);
            }

            bindings.push_back(
                MaterialPassRenderingBinding{
                    .pass_name = pass.name,
                    .ray_query = pass.ray_query,
                    .rasterization_samples =
                        pass.rasterization_samples,
                    .rendering = compiled.rendering,
                    .output_schema =
                        pass.materialInfo()
                            .output_schema,
                    .output_states =
                        pass.materialInfo()
                            .output_states,
                    .shader_inputs =
                        std::move(shader_inputs),
                });
        }
    }
    return bindings;
}

static bool generationHasMaterialPasses(
    const RendererRuntimeGeneration &generation) {
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Render-pipeline candidate pass table is "
                "inconsistent");
        }
        for (const auto &compiled :
             program->rendering_pass.passes) {
            if (compiled.definition.isMaterial()) return true;
        }
    }
    return false;
}

static bool generationServesMaterial(
    const RendererRuntimeGeneration &generation,
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) {
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) continue;
        for (const auto &compiled :
             program->rendering_pass.passes) {
            const auto &pass = compiled.definition;
            if (pass.isMaterial() &&
                materialPassAcceptsMaterial(
                    pass.materialInfo().contract, pass.name,
                    route, shader_contract, exact_pass)) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::vector<MaterialPassRenderingBinding>>
MaterialGenerationPlanner::resolveLiveMaterialBindings(
    const MaterialContainer &container,
    const RendererRuntimeGeneration &generation,
    const MaterialContainer::InternalMaterialInfo &material) {
    // A material the candidate's material_route_default moves to another
    // route keeps its live pipeline until the restart lowers it again;
    // requireEveryMaterialServed already judged it by its new route.
    if (!candidateRoute(container, material).live) {
        return std::nullopt;
    }
    auto bindings = materialBindingsForGeneration(
        generation, material.route,
        material.shader_contract, material.exact_pass);
    if (!bindings.empty()) {
        return bindings;
    }
    // G-4: the same rule as registration. A graph with one material pass must
    // serve every registered material, whether or not the published graph
    // did. A graph with no material pass at all (an RT-only project) draws
    // no material, but may not drop a route the published graph consumed;
    // MaterialGenerationPlanner::requireEveryMaterialServed has refused both
    // already, and this is the per-material backstop for the first.
    if (!generationHasMaterialPasses(generation)) {
        return std::nullopt;
    }
    throw std::runtime_error(materialRouteUnservedMessage(
        {UnservedMaterialRoute{
            material.route, material.shader_contract,
            {describeUnservedMaterial(material.name,
                                      material.source_document,
                                      material.source_asset)}}},
        activeRenderingConfigReference()));
}

MaterialGenerationPlanner::CandidateRoute
MaterialGenerationPlanner::candidateRoute(
    const MaterialContainer &container,
    const MaterialContainer::InternalMaterialInfo &material) {
    if (!material.default_routes) {
        return {material.route, material.shader_contract, true};
    }
    const auto route_default =
        container.candidate_route_default.value_or(
            projectMaterialLoweringCapabilities().route_default);
    const auto route = material.default_routes->under(route_default);
    if (route == material.route) {
        return {material.route, material.shader_contract, true};
    }
    return {route, materialShaderContractForRoute(route), false};
}

void MaterialGenerationPlanner::requireEveryMaterialServed(
    const MaterialContainer &container,
    const RendererRuntimeGeneration &generation) {
    // Two rules, one code. A candidate with any material pass must serve
    // every registered material. A candidate with none (an RT-only graph)
    // may draw no material, but it may not drop a route the published graph
    // did consume. With nothing published yet (a ray-tracing startup
    // generation stays staged until its first submitted frame) no route was
    // consumed, so there is nothing to drop.
    const bool has_material_passes =
        generationHasMaterialPasses(generation);
    std::shared_ptr<const RendererRuntimeGeneration> previous;
    if (!has_material_passes) {
        previous = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        if (previous == nullptr) return;
    }
    std::vector<UnservedMaterialRoute> unserved;
    container.materials.forEach(
        [&](GlobalMaterialId,
            const MaterialContainer::InternalMaterialInfo &material) {
            // A candidate that declares a new material_route_default is
            // judged by the routes the restart will lower materials to.
            const auto candidate = candidateRoute(container, material);
            if (has_material_passes) {
                if (generationServesMaterial(
                        generation, candidate.route,
                        candidate.shader_contract,
                        material.exact_pass)) {
                    return;
                }
            } else if (materialBindingsForGeneration(
                           *previous, material.route,
                           material.shader_contract,
                           material.exact_pass)
                           .empty()) {
                // The published graph did not draw it either.
                return;
            }
            auto entry = std::find_if(
                unserved.begin(), unserved.end(),
                [&](const UnservedMaterialRoute &existing) {
                    return existing.route == candidate.route &&
                           existing.shader_contract ==
                               candidate.shader_contract;
                });
            if (entry == unserved.end()) {
                unserved.push_back(
                    {candidate.route, candidate.shader_contract, {}});
                entry = std::prev(unserved.end());
            }
            entry->materials.push_back(describeUnservedMaterial(
                material.name, material.source_document,
                material.source_asset));
        });
    // LP-3a (docs/design_graph_loops.md 4.2.5): a graph.loop over draw_items
    // must draw every material its over and where take in. A branch whose
    // body draws nothing leaves the material unserved; `"body": null` is a
    // decision not to draw it and counts as served. Only graph loops are
    // judged here: a pass's material_filter still decides alone what it
    // leaves out.
    if (has_material_passes) {
        MaterialDrawLoopRules rules;
        for (const auto rendering_pass_id : generation.rendering_pass_ids) {
            const auto *program = generation.find(rendering_pass_id);
            if (program == nullptr ||
                program->frame_graph.render_pipeline == nullptr ||
                program->frame_graph.render_pipeline->material_draw_loops ==
                    nullptr) {
                continue;
            }
            for (const auto &[pass, rule] :
                 program->frame_graph.render_pipeline->material_draw_loops
                     ->passes) {
                rules.passes.insert_or_assign(pass, rule);
            }
        }
        if (!rules.empty()) {
            const auto ids = container.drawLoopMaterials();
            std::vector<MaterialDrawSubject> subjects;
            subjects.reserve(ids.size());
            for (const auto id : ids) {
                subjects.push_back(container.drawLoopSubject(id));
            }
            for (const auto index :
                 materialDrawLoopUnservedSubjects(rules, subjects)) {
                const auto &material = container.materials.get(ids[index]);
                auto entry = std::find_if(
                    unserved.begin(), unserved.end(),
                    [&](const UnservedMaterialRoute &existing) {
                        return existing.route == material.route &&
                               existing.shader_contract ==
                                   material.shader_contract;
                    });
                if (entry == unserved.end()) {
                    unserved.push_back(
                        {material.route, material.shader_contract, {}});
                    entry = std::prev(unserved.end());
                }
                entry->materials.push_back(describeUnservedMaterial(
                    material.name, material.source_document,
                    material.source_asset));
            }
        }
    }
    if (unserved.empty()) return;
    throw std::runtime_error(materialRouteUnservedMessage(
        unserved, activeRenderingConfigReference()));
}

std::vector<MaterialPassShaderInputBinding>
resolveGenerationShaderInputs(
    std::span<const MaterialPassRenderingBinding> passes,
    std::span<const MaterialPassShaderInputRequest> inputs) {
    std::vector<MaterialPassShaderInputBinding> result;
    result.reserve(inputs.size());
    for (const auto &input : inputs) {
        result.push_back({
            .input = input,
            .input_attachment_index = std::nullopt,
            .view_dimension =
                PassInputViewDimension::shared_2d,
        });
    }
    if (passes.empty()) {
        return result;
    }

    for (std::size_t input_index = 0;
         input_index < inputs.size(); ++input_index) {
        std::optional<std::uint32_t> expected;
        std::optional<vk::Extent2D>
            expected_extent;
        auto expected_view =
            PassInputViewDimension::shared_2d;
        auto expected_descriptor_dimension =
            ImageSubresourceViewDimension::two_d;
        std::string expected_pass;
        bool initialized = false;
        for (const auto &pass : passes) {
            const auto found = std::find_if(
                pass.shader_inputs.begin(),
                pass.shader_inputs.end(),
                [&](const auto &candidate) {
                    return candidate.input ==
                           inputs[input_index];
                });
            if (found == pass.shader_inputs.end()) {
                throw std::runtime_error(
                    "material pass '" + pass.pass_name +
                    "' does not provide shader input '" +
                    inputs[input_index].name + "'");
            }
            if (!initialized) {
                expected =
                    found->input_attachment_index;
                expected_extent =
                    found->input_attachment_extent;
                expected_view =
                    found->view_dimension;
                expected_descriptor_dimension =
                    found->descriptor_dimension;
                expected_pass =
                    pass.pass_name;
                initialized = true;
                continue;
            }
            if (expected !=
                    found->input_attachment_index ||
                expected_extent !=
                    found->input_attachment_extent ||
                expected_view !=
                    found->view_dimension ||
                expected_descriptor_dimension !=
                    found->descriptor_dimension) {
                throw std::runtime_error(
                    "material shader input '" +
                    inputs[input_index].name +
                    "' resolves to different sampled/local-read ABIs "
                    "or image-view ABIs across render graph variants: '" +
                    expected_pass + "' (view " +
                    std::to_string(
                        static_cast<int>(
                            expected_view)) +
                    ", descriptor " +
                    std::string{
                        imageSubresourceViewDimensionName(
                            expected_descriptor_dimension)} +
                    ") and '" + pass.pass_name +
                    "' (view " +
                    std::to_string(
                        static_cast<int>(
                            found->view_dimension)) +
                    ", descriptor " +
                    std::string{
                        imageSubresourceViewDimensionName(
                            found->descriptor_dimension)} +
                    ")");
            }
        }
        result[input_index].input_attachment_index =
            expected;
        result[input_index].input_attachment_extent =
            expected_extent;
        result[input_index].view_dimension =
            expected_view;
        result[input_index].descriptor_dimension =
            expected_descriptor_dimension;
    }
    return result;
}

RequestedShaderInputs collectRequestedShaderInputs(
    std::span<const MaterialScreenInputContract>
        declared_screen_inputs,
    std::span<const SurfaceResourcePortDefinition>
        declared_resource_ports) {
    RequestedShaderInputs result;
    result.requests.reserve(
        declared_screen_inputs.size() +
        declared_resource_ports.size());
    for (const auto &input : declared_screen_inputs) {
        result.requests.push_back({
            .kind = MaterialPassShaderInputKind::
                screen_input,
            .name = input.name,
        });
    }
    for (std::size_t index = 0;
         index < declared_resource_ports.size();
         ++index) {
        const auto &resource =
            declared_resource_ports[index];
        if (resource.kind !=
            SurfaceResourcePortKind::image) {
            continue;
        }
        result.requests.push_back({
            .kind = MaterialPassShaderInputKind::
                material_resource,
            .name = resource.name,
        });
        result.image_resource_indices.push_back(index);
    }
    return result;
}

ScreenInputAbiPlan planScreenInputAbi(
    std::span<const MaterialScreenInputContract> declared,
    std::span<const MaterialPassInputState> live,
    std::span<const MaterialPassShaderInputBinding>
        physical_inputs) {
    ScreenInputAbiPlan plan;
    plan.updated_bindings.assign(live.begin(),
                                 live.end());
    for (std::size_t index = 0;
         index < declared.size(); ++index) {
        const auto &name = declared[index].name;
        const auto byName = [&](const auto &input) {
            return input.contract.name == name;
        };
        const auto current = std::find_if(
            live.begin(), live.end(), byName);
        const auto candidate = std::find_if(
            plan.updated_bindings.begin(),
            plan.updated_bindings.end(), byName);
        if (current == live.end() ||
            candidate ==
                plan.updated_bindings.end()) {
            throw std::runtime_error(
                "live material screen-input metadata is "
                "incomplete for '" +
                name + "'");
        }
        if (current->input_attachment_index) {
            plan.defines_before.push_back(
                makeSurfaceScreenInputLocalReadDefine(
                    index,
                    *current->input_attachment_index));
        }
        const auto attachment =
            physical_inputs[index]
                .input_attachment_index;
        candidate->descriptor_type =
            attachment
                ? vk::DescriptorType::eInputAttachment
                : vk::DescriptorType::
                      eCombinedImageSampler;
        candidate->input_attachment_index = attachment;
        if (attachment) {
            plan.defines_after.push_back(
                makeSurfaceScreenInputLocalReadDefine(
                    index, *attachment));
        }
    }
    return plan;
}

ResourcePortAbiPlan planResourcePortAbi(
    std::span<const SurfaceResourcePortDefinition>
        declared,
    std::span<const ShaderResourceInterfaceBinding> live,
    std::span<const MaterialPassShaderInputBinding>
        physical_inputs,
    std::size_t physical_offset,
    std::span<const std::size_t> image_resource_indices) {
    ResourcePortAbiPlan plan;
    plan.updated_bindings.assign(live.begin(),
                                 live.end());
    for (std::size_t position = 0;
         position < image_resource_indices.size();
         ++position) {
        const auto resource_index =
            image_resource_indices[position];
        const auto &name =
            declared[resource_index].name;
        const auto byName = [&](const auto &resource) {
            return resource.port.name == name;
        };
        const auto current = std::find_if(
            live.begin(), live.end(), byName);
        const auto candidate = std::find_if(
            plan.updated_bindings.begin(),
            plan.updated_bindings.end(), byName);
        if (current == live.end() ||
            candidate ==
                plan.updated_bindings.end()) {
            throw std::runtime_error(
                "live material resource metadata is "
                "incomplete for '" +
                name + "'");
        }
        if (current->input_attachment_index) {
            appendSurfaceResourceLocalReadDefines(
                plan.defines_before, resource_index,
                *current->input_attachment_index,
                current->input_attachment_extent, name);
        } else if (current->image_view_dimension ==
                   ReflectedImageViewDimension::cube) {
            plan.defines_before.push_back(
                makeSurfaceResourceCubeDefine(
                    resource_index));
        } else if (current->image_view_dimension ==
                   ReflectedImageViewDimension::
                       two_d_array) {
            plan.defines_before.push_back(
                makeSurfaceResourceLayeredDefine(
                    resource_index));
        }

        // The physical input table is screen inputs first, then the image
        // resource ports in declaration order: this offset is the one
        // index-alignment the two plans have to agree on.
        const auto &physical =
            physical_inputs[physical_offset + position];
        const auto attachment =
            physical.input_attachment_index;
        const auto cube =
            !attachment &&
            physical.descriptor_dimension ==
                ImageSubresourceViewDimension::cube;
        const auto layered =
            !attachment && !cube &&
            physical.descriptor_dimension ==
                ImageSubresourceViewDimension::
                    two_d_array;
        candidate->descriptor =
            attachment
                ? ShaderResourceDescriptorKind::
                      input_attachment
                : ShaderResourceDescriptorKind::
                      combined_image_sampler;
        candidate->input_attachment_index = attachment;
        candidate->input_attachment_extent =
            attachment
                ? physical.input_attachment_extent
                : std::nullopt;
        candidate->image_view_dimension =
            cube ? ReflectedImageViewDimension::cube
            : layered
                ? ReflectedImageViewDimension::two_d_array
                : ReflectedImageViewDimension::two_d;
        candidate->port.view =
            cube ? ShaderResourcePortView::cube
            : layered
                ? ShaderResourcePortView::family_array
                : ShaderResourcePortView::shared_2d;
        if (attachment) {
            appendSurfaceResourceLocalReadDefines(
                plan.defines_after, resource_index,
                *attachment,
                physical.input_attachment_extent, name);
        } else if (cube) {
            plan.defines_after.push_back(
                makeSurfaceResourceCubeDefine(
                    resource_index));
        } else if (layered) {
            plan.defines_after.push_back(
                makeSurfaceResourceLayeredDefine(
                    resource_index));
        }
    }
    return plan;
}

MaterialPipelineRenderingContract
MaterialGenerationPlanner::liveRenderingContract(
    const MaterialContainer::InternalMaterialInfo
        &material) {
    return MaterialPipelineRenderingContract{
        .color_formats =
            material.pipeline_color_formats,
        .depth_format =
            material.pipeline_depth_format,
        .rasterization_samples =
            material.pipeline_rasterization_samples,
        .local_read = material.pipeline_local_read,
        .output_states =
            material.pipeline_output_states,
    };
}

void MaterialGenerationPlanner::validateCompatibility(
    const MaterialContainer &container,
    const RendererRuntimeGeneration &generation) {
    requireEveryMaterialServed(container, generation);
    container.materials.forEach(
        [&](GlobalMaterialId material_id,
            const MaterialContainer::InternalMaterialInfo
                &material) {
            const auto bindings =
                resolveLiveMaterialBindings(
                    container, generation, material);
            if (!bindings) return;

            const auto live =
                liveRenderingContract(material);
            for (const auto &binding : *bindings) {
                if (!binding.ray_query && GET_MODULE(PipelineFactory).pipelineUsesRayQueryFrameSet(material.pipeline))
                    throw std::runtime_error("Material pass '" + binding.pass_name + "' uses an acceleration structure but does not declare ray_query");
                if (binding.output_schema !=
                    material.output_schema) {
                    const auto live_schema =
                        material.output_schema
                            ? "'" +
                                  material.output_schema
                                      ->name +
                                  "'"
                            : std::string{"<built-in>"};
                    const auto candidate_schema =
                        binding.output_schema
                            ? "'" +
                                  binding.output_schema->name +
                                  "'"
                            : std::string{"<built-in>"};
                    throw std::runtime_error(
                        "render-pipeline candidate pass '" +
                        binding.pass_name +
                        "' changes live material " +
                        std::to_string(material_id.value) +
                        " output schema from " +
                        live_schema + " to " +
                        candidate_schema +
                        "; reload the material surface "
                        "transactionally with the graph");
                }

                const auto candidate =
                    resolveMaterialPassRenderingBinding(
                        binding,
                        material.shader_contract);
                if (candidate != live) {
                    throw std::runtime_error(
                        "render-pipeline candidate pass '" +
                        binding.pass_name +
                        "' changes the physical rendering "
                        "contract of live material " +
                        std::to_string(material_id.value) +
                        "; color/depth formats, sample count, "
                        "local-read mapping, or attachment "
                        "state require a "
                        "transactional material pipeline "
                        "rebuild");
                }
            }
        });
}

MaterialRuntimeGenerationReloadPlan
MaterialGenerationPlanner::prepareReload(
    MaterialContainer &container,
    const RendererRuntimeGeneration &generation) {
    using InternalMaterialInfo =
        MaterialContainer::InternalMaterialInfo;
    // Called inside the renderer publication hook. G-4: the acceptance rule
    // does not depend on the live generation; every registered material
    // needs a pass whenever the candidate has any material pass.
    requireEveryMaterialServed(container, generation);
    struct MetadataUpdate {
        GlobalMaterialId material;
        std::optional<MaterialOutputSchema>
            output_schema;
        MaterialPipelineRenderingContract rendering;
        std::vector<MaterialPassInputState> pass_inputs;
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface;
        bool descriptor_abi_changed = false;
    };

    MaterialRuntimeGenerationReloadPlan plan;
    std::unordered_map<std::uint64_t, bool> pipeline_ray_query;
    container.materials.forEach([&](GlobalMaterialId, const InternalMaterialInfo &material) {
        const auto bindings = materialBindingsForGeneration(
            generation, material.route, material.shader_contract, material.exact_pass);
        const bool allowed = !bindings.empty() && std::all_of(
            bindings.begin(), bindings.end(), [](const auto &binding) { return binding.ray_query; });
        auto [entry, inserted] = pipeline_ray_query.emplace(material.pipeline.value, allowed);
        if (!inserted) entry->second = entry->second && allowed;
    });
    std::vector<MetadataUpdate> metadata_updates;
    // LP-2: what a base pipeline this plan rebuilds is rebuilt for, so the
    // draw-loop overrides derived from it follow.
    struct OverriddenBase {
        MaterialInfo material;
        MaterialPipelineRenderingContract rendering;
    };
    std::unordered_map<std::uint64_t, OverriddenBase> overridden_bases;
    auto &pipeline_factory =
        GET_MODULE(PipelineFactory);

    const auto append_shader_override =
        [&](SurfaceShaderReloadOverride candidate) {
            const auto found = std::find_if(
                plan.shader_overrides.begin(),
                plan.shader_overrides.end(),
                [&](const auto &existing) {
                    return existing.fragment ==
                           candidate.fragment;
                });
            if (found ==
                plan.shader_overrides.end()) {
                plan.shader_overrides.push_back(
                    std::move(candidate));
                return;
            }
            if (*found != candidate) {
                throw std::runtime_error(
                    "live materials sharing fragment shader " +
                    std::to_string(
                        candidate.fragment.value) +
                    " require incompatible render-graph "
                    "ABIs");
            }
        };
    const auto append_pipeline_override =
        [&](GraphicsPipelineReloadOverride candidate) {
            const auto found = std::find_if(
                plan.pipeline_overrides.begin(),
                plan.pipeline_overrides.end(),
                [&](const auto &existing) {
                    return existing.handle ==
                           candidate.handle;
                });
            if (found ==
                plan.pipeline_overrides.end()) {
                plan.pipeline_overrides.push_back(
                    std::move(candidate));
                return;
            }
            if (found->desc != candidate.desc) {
                throw std::runtime_error(
                    "live materials sharing pipeline " +
                    std::to_string(
                        candidate.handle.value) +
                    " require incompatible render-graph "
                    "contracts");
            }
        };

    container.materials.forEach(
        [&](GlobalMaterialId material_id,
            const InternalMaterialInfo &material) {
            // lookup
            const auto bindings =
                resolveLiveMaterialBindings(
                    container, generation, material);
            if (!bindings) return;

            const auto candidate_schema =
                bindings->front().output_schema;
            auto candidate_rendering =
                resolveMaterialPassRenderingBinding(
                    bindings->front(),
                    material.shader_contract);
            for (std::size_t index = 1;
                 index < bindings->size(); ++index) {
                if ((*bindings)[index].output_schema !=
                    candidate_schema) {
                    throw std::runtime_error(
                        "material route resolves to different "
                        "output schemas across render graph "
                        "variants: " +
                        bindings->front().pass_name + " and " +
                        (*bindings)[index].pass_name);
                }
                const auto rendering =
                    resolveMaterialPassRenderingBinding(
                        (*bindings)[index],
                        material.shader_contract);
                if (rendering !=
                    candidate_rendering) {
                    throw std::runtime_error(
                        "material route resolves to "
                        "pipeline-incompatible physical rendering "
                        "contracts: " +
                        bindings->front().pass_name + " and " +
                        (*bindings)[index].pass_name);
                }
            }

            // two plans
            const auto requested =
                collectRequestedShaderInputs(
                    material.declared_screen_inputs,
                    material.declared_resource_ports);
            const auto physical_inputs =
                resolveGenerationShaderInputs(
                    *bindings, requested.requests);
            auto screen_plan = planScreenInputAbi(
                material.declared_screen_inputs,
                material.pass_inputs, physical_inputs);
            auto resource_plan = planResourcePortAbi(
                material.declared_resource_ports,
                material.resource_interface,
                physical_inputs,
                material.declared_screen_inputs.size(),
                requested.image_resource_indices);

            // diff
            auto current_physical_defines =
                std::move(screen_plan.defines_before);
            current_physical_defines.insert(
                current_physical_defines.end(),
                resource_plan.defines_before.begin(),
                resource_plan.defines_before.end());
            auto candidate_physical_defines =
                std::move(screen_plan.defines_after);
            candidate_physical_defines.insert(
                candidate_physical_defines.end(),
                resource_plan.defines_after.begin(),
                resource_plan.defines_after.end());
            auto candidate_pass_inputs =
                std::move(screen_plan.updated_bindings);
            auto candidate_resource_interface =
                std::move(resource_plan.updated_bindings);

            auto pipeline_desc =
                pipeline_factory.graphicsDesc(
                    material.pipeline);
            if (!pipeline_desc.frag) {
                throw std::logic_error(
                    "material pipeline has no fragment shader");
            }
            if (candidate_schema !=
                    material.output_schema ||
                candidate_physical_defines !=
                    current_physical_defines) {
                append_shader_override({
                    .fragment =
                        *pipeline_desc.frag,
                    .physical_defines =
                        std::move(
                            candidate_physical_defines),
                    .material_output_schema =
                        candidate_schema,
                });
            }

            MaterialInfo validation_info;
            validation_info.output_schema =
                candidate_schema;
            validation_info.render_state =
                material.render_state;
            validation_info.vat = material.vat;
            validation_info.custom_textures.resize(
                material
                    .custom_sampler_resolutions
                    .size());
            validateMaterialCapabilities(
                validation_info,
                candidate_rendering);

            auto candidate_pipeline_desc =
                pipeline_desc;
            candidate_pipeline_desc.allow_ray_query = pipeline_ray_query.at(material.pipeline.value);
            candidate_pipeline_desc.color_formats =
                candidate_rendering.color_formats;
            candidate_pipeline_desc.depth_format =
                candidate_rendering.depth_format;
            candidate_pipeline_desc
                .rasterization_samples =
                candidate_rendering
                    .rasterization_samples;
            candidate_pipeline_desc.local_read =
                candidate_rendering.local_read;
            candidate_pipeline_desc.resource_interface =
                candidate_resource_interface;
            candidate_pipeline_desc
                .color_attachment_states =
                candidate_rendering
                        .output_states.empty()
                    ? std::vector<
                          GraphicsPipelineColorAttachmentState>{}
                    : resolveMaterialColorAttachmentStates(
                          validation_info,
                          candidate_rendering);
            if (candidate_pipeline_desc !=
                pipeline_desc) {
                append_pipeline_override({
                    .handle = material.pipeline,
                    .desc = std::move(
                        candidate_pipeline_desc),
                });
                overridden_bases.try_emplace(
                    material.pipeline.value,
                    OverriddenBase{validation_info,
                                   candidate_rendering});
            }

            // accumulate
            const auto live =
                liveRenderingContract(material);
            const auto descriptor_abi_changed =
                candidate_pass_inputs !=
                    material.pass_inputs ||
                candidate_resource_interface !=
                    material.resource_interface;
            if (candidate_schema !=
                    material.output_schema ||
                candidate_rendering != live ||
                descriptor_abi_changed) {
                metadata_updates.push_back({
                    .material = material_id,
                    .output_schema =
                        candidate_schema,
                    .rendering =
                        std::move(
                            candidate_rendering),
                    .pass_inputs =
                        std::move(
                            candidate_pass_inputs),
                    .resource_interface =
                        std::move(
                            candidate_resource_interface),
                    .descriptor_abi_changed =
                        descriptor_abi_changed,
                });
            }
        });

    // LP-2: a draw-loop override is the base pipeline with another render
    // state (and, for defines, its own shader pair). When the base is
    // rebuilt in place for this generation, every live table's override of
    // it is rebuilt from the base's candidate description, and an
    // override's own fragment takes the base fragment's new physical
    // defines. Otherwise the rebuild would pair the new shaders with the
    // override's old resource interface and ray-query frame set.
    MaterialDrawLoopTable::forEachLive([&](MaterialDrawLoopTable &table) {
        table.forEachOverride([&](const MaterialDrawLoopTable::OverridePipeline
                                      &entry) {
            const auto base_desc =
                pipeline_factory.graphicsDesc(entry.base_pipeline);
            if (entry.define_bundles && base_desc.frag) {
                const auto shader = std::find_if(
                    plan.shader_overrides.begin(),
                    plan.shader_overrides.end(),
                    [&](const auto &candidate) {
                        return candidate.fragment == *base_desc.frag;
                    });
                if (shader != plan.shader_overrides.end()) {
                    auto follow = *shader;
                    follow.fragment = entry.define_bundles->fragment;
                    append_shader_override(std::move(follow));
                }
            }
            const auto base_override = std::find_if(
                plan.pipeline_overrides.begin(),
                plan.pipeline_overrides.end(),
                [&](const auto &candidate) {
                    return candidate.handle == entry.base_pipeline;
                });
            if (base_override == plan.pipeline_overrides.end()) return;
            const auto &base = overridden_bases.at(
                entry.base_pipeline.value);
            auto desc = materialDrawOverridePipelineDesc(
                base_override->desc, base.material, entry.render_state,
                base.rendering);
            if (entry.define_bundles) {
                desc.vert = entry.define_bundles->vertex;
                desc.frag = entry.define_bundles->fragment;
            }
            append_pipeline_override({
                .handle = entry.pipeline,
                .desc = std::move(desc),
            });
        });
    });

    // commit
    const auto overridden_pipelines =
        plan.pipeline_overrides;
    plan.commit =
        [owner = &container,
         updates = std::move(metadata_updates),
         overridden_pipelines]() mutable {
            const auto descriptor_abi_changed =
                std::any_of(
                    updates.begin(), updates.end(),
                    [](const auto &update) {
                        return update
                            .descriptor_abi_changed;
                    });
            if (descriptor_abi_changed) {
                GET_MODULE(VulkanManageCore)
                    .waitIdle();
            }
            for (auto &update : updates) {
                auto &material =
                    owner->materials.get(
                        update.material);
                material.output_schema =
                    std::move(
                        update.output_schema);
                material.pipeline_color_formats =
                    std::move(
                        update.rendering
                            .color_formats);
                material.pipeline_depth_format =
                    update.rendering.depth_format;
                material
                    .pipeline_rasterization_samples =
                    update.rendering
                        .rasterization_samples;
                material.pipeline_local_read =
                    std::move(
                        update.rendering.local_read);
                material.pipeline_output_states =
                    std::move(
                        update.rendering
                            .output_states);
                material.pass_inputs =
                    std::move(update.pass_inputs);
                material.resource_interface =
                    std::move(
                        update.resource_interface);
                if (update.descriptor_abi_changed) {
                    material
                        .screen_input_descriptors
                        .clear();
                    ++material.descriptor_revision;
                }
            }
            std::erase_if(
                owner->pipelines,
                [&](const auto &entry) {
                    return std::any_of(
                        overridden_pipelines.begin(),
                        overridden_pipelines.end(),
                        [&](const auto &override) {
                            return entry.second ==
                                   override.handle;
                        });
                });
        };
    return plan;
}

void MaterialContainer::validateRuntimeGenerationCompatibility(
    const RendererRuntimeGeneration &generation) const {
    MaterialGenerationPlanner::validateCompatibility(
        *this, generation);
}

MaterialRuntimeGenerationReloadPlan
MaterialContainer::prepareRuntimeGenerationReload(
    const RendererRuntimeGeneration &generation) {
    return MaterialGenerationPlanner::prepareReload(
        *this, generation);
}

} // namespace Pelican
