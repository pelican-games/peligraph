#include "renderingpasscontainer.hpp"
#include "framegraphruntime.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

template <typename Visitor>
void visitPublishedRenderingPasses(
    const RendererRuntimeGeneration &generation,
    Visitor &&visitor) {
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Published render pipeline pass table is inconsistent");
        }
        visitor(program->rendering_pass);
    }
}

} // namespace

RenderingPassContainer::RenderingPassContainer()
    : runtime_publication{nullptr} {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerCompiledRenderingPass(CompiledRenderingPass pass) {
    if (auto it = name_to_id.find(pass.name); it != name_to_id.end()) {
        return it->second;
    }

    const auto pass_name = pass.name;
    auto id = rendering_passes.reg(std::move(pass));
    name_to_id.emplace(pass_name, id);
    registered_pass_ids.push_back(id);
    return id;
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string &name) const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        const auto found = generation->name_to_id.find(name);
        return found != generation->name_to_id.end()
                   ? found->second
                   : invalidRenderingPassId();
    }
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return invalidRenderingPassId();
}

const CompiledRenderingPass &RenderingPassContainer::getCompiledRenderingPass(RenderingPassId rendering_pass_id) const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        const auto *program =
            generation->find(rendering_pass_id);
        if (program == nullptr) {
            throw std::out_of_range(
                "Published rendering pass not found");
        }
        return program->rendering_pass;
    }
    return rendering_passes.get(rendering_pass_id);
}

const std::vector<RenderingPassId> &RenderingPassContainer::getRegisteredPassIds() const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        return generation->rendering_pass_ids;
    }
    return registered_pass_ids;
}

void RenderingPassContainer::setEnabledFeatures(std::vector<std::string> feature_names) {
    enabled_feature_names = std::move(feature_names);
}

bool RenderingPassContainer::isFeatureEnabled(std::string_view feature_name) const {
    if (const auto generation = snapshot()) {
        return std::find(
                   generation->enabled_feature_names.begin(),
                   generation->enabled_feature_names.end(),
                   feature_name) !=
               generation->enabled_feature_names.end();
    }
    const auto &features = getEnabledFeatures();
    return std::find(features.begin(), features.end(),
                     feature_name) != features.end();
}

const std::vector<std::string> &RenderingPassContainer::getEnabledFeatures() const {
    const auto generation = snapshot();
    if (generation != nullptr) {
        return generation->enabled_feature_names;
    }
    return enabled_feature_names;
}

void RenderingPassContainer::bindRuntimePublication(
    std::shared_ptr<const RendererRuntimePublication>
        publication) noexcept {
    runtime_publication.store(
        std::move(publication), std::memory_order_release);
}

std::shared_ptr<const RendererRuntimeGeneration>
RenderingPassContainer::snapshot() const noexcept {
    const auto publication =
        runtime_publication.load(std::memory_order_acquire);
    return publication != nullptr
               ? publication->active_generation.load(
                     std::memory_order_acquire)
               : nullptr;
}

std::uint64_t
RenderingPassContainer::activeGeneration() const noexcept {
    const auto generation = snapshot();
    return generation != nullptr ? generation->generation : 0;
}

bool RenderingPassContainer::hasMaterialPasses() const {
    if (const auto generation = snapshot()) {
        bool found = false;
        visitPublishedRenderingPasses(
            *generation,
            [&found](const CompiledRenderingPass &rendering_pass) {
                found = found ||
                        std::any_of(
                            rendering_pass.passes.begin(),
                            rendering_pass.passes.end(),
                            [](const auto &pass) {
                                return pass.definition.isMaterial();
                            });
            });
        return found;
    }
    for (const auto rendering_pass_id :
         registered_pass_ids) {
        const auto &rendering_pass =
            rendering_passes.get(rendering_pass_id);
        if (std::any_of(rendering_pass.passes.begin(), rendering_pass.passes.end(),
                        [](const auto &pass) { return pass.definition.isMaterial(); })) {
            return true;
        }
    }
    return false;
}

bool RenderingPassContainer::supportsMaterialPass(
    MaterialRouteClass route, MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) const {
    return !materialPassRenderingBindings(
                 route, shader_contract, exact_pass)
                 .empty();
}

std::vector<MaterialPassRenderingBinding>
RenderingPassContainer::materialPassRenderingBindings(
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) const {
    std::vector<MaterialPassRenderingBinding> result;
    const auto collect =
        [&](const CompiledRenderingPass &rendering_pass) {
            for (const auto &compiled :
                 rendering_pass.passes) {
                const auto &pass = compiled.definition;
                if (!pass.isMaterial() ||
                    (exact_pass &&
                     pass.name != *exact_pass) ||
                    !materialPassAcceptsMaterial(
                        pass.materialInfo().contract,
                        pass.name, route,
                        shader_contract,
                        exact_pass)) {
                    continue;
                }
                result.push_back(
                    MaterialPassRenderingBinding{
                        .pass_name = pass.name,
                        .rasterization_samples =
                            pass.rasterization_samples,
                        .rendering =
                            compiled.rendering,
                        .output_schema =
                            pass.materialInfo()
                                .output_schema,
                        .output_states =
                            pass.materialInfo()
                                .output_states,
                        .shader_inputs = [&] {
                            std::vector<
                                MaterialPassShaderInputBinding>
                                inputs;
                            const auto input_attachment_index =
                                [&](GlobalRenderTargetId target,
                                    bool history,
                                    const LogicalReadFootprint
                                        &footprint)
                                -> std::optional<
                                    std::uint32_t> {
                                if (history) {
                                    return std::nullopt;
                                }
                                const auto target_position =
                                    std::find(
                                        pass.input_targets.begin(),
                                        pass.input_targets.end(),
                                        target);
                                if (target_position ==
                                    pass.input_targets.end()) {
                                    throw std::runtime_error(
                                        "material pass input is absent "
                                        "from the positional physical "
                                        "input table: " +
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
                                    LogicalReadFootprintKind::
                                        same_pixel) {
                                    throw std::logic_error(
                                        "material pass selected a local "
                                        "attachment for a non-same-pixel "
                                        "input: " +
                                        pass.name);
                                }
                                return index;
                            };
                            const auto append =
                                [&](MaterialPassShaderInputKind kind,
                                    std::string name,
                                    GlobalRenderTargetId target,
                                    bool history,
                                    const LogicalReadFootprint
                                        &footprint,
                                    std::optional<
                                        ShaderResourcePortView>
                                        resource_view =
                                            std::nullopt) {
                                    const auto target_position =
                                        std::find(
                                            pass.input_targets.begin(),
                                            pass.input_targets.end(),
                                            target);
                                    auto view_dimension =
                                        target_position !=
                                                    pass.input_targets.end() &&
                                                pass.input_target_views.size() ==
                                                    pass.input_targets.size()
                                            ? pass.input_target_views.at(
                                                  static_cast<std::size_t>(
                                                      target_position -
                                                      pass.input_targets
                                                          .begin()))
                                            : PassInputViewDimension::
                                                  shared_2d;
                                    if (resource_view ==
                                        ShaderResourcePortView::
                                            family_array) {
                                        // family_array is an algorithm ABI,
                                        // even when a one-view family is
                                        // physically represented by a
                                        // scalar image.
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
                                    const auto
                                        descriptor_dimension =
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
                                    inputs.push_back(
                                        MaterialPassShaderInputBinding{
                                            .input = {
                                                .kind = kind,
                                                .name =
                                                    std::move(name),
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
                                 pass.materialInfo()
                                     .screen_inputs) {
                                append(
                                    MaterialPassShaderInputKind::
                                        screen_input,
                                    input.contract.name,
                                    input.target, input.history,
                                    input.contract.footprint);
                            }
                            for (const auto &resource :
                                 pass.materialInfo()
                                     .material_resources) {
                                if (!resource.isImage()) {
                                    continue;
                                }
                                append(
                                    MaterialPassShaderInputKind::
                                        material_resource,
                                    resource.port.name,
                                    resource.target,
                                    resource.history,
                                    resource.footprint,
                                    resource.port.view);
                            }
                            return inputs;
                        }(),
                    });
            }
        };
    if (const auto generation = snapshot()) {
        visitPublishedRenderingPasses(
            *generation, collect);
    } else {
        for (const auto rendering_pass_id :
             registered_pass_ids) {
            collect(rendering_passes.get(
                rendering_pass_id));
        }
    }
    return result;
}

std::vector<MaterialPassShaderInputBinding>
RenderingPassContainer::materialPassShaderInputBindings(
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    std::span<const MaterialPassShaderInputRequest> inputs,
    const std::optional<std::string> &exact_pass) const {
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

    const auto passes = materialPassRenderingBindings(
        route, shader_contract, exact_pass);
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
                    "or input-attachment extent/image-view ABIs across "
                    "render graph variants: '" +
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

std::optional<MaterialOutputSchema>
RenderingPassContainer::materialOutputSchema(
    MaterialRouteClass route,
    const std::optional<std::string> &exact_pass) const {
    std::optional<MaterialOutputSchema> result;
    bool selected = false;
    const auto collect =
        [&](const CompiledRenderingPass &rendering_pass) {
            for (const auto &compiled :
                 rendering_pass.passes) {
                const auto &pass = compiled.definition;
                if (!pass.isMaterial() ||
                    (exact_pass &&
                     pass.name != *exact_pass)) {
                    continue;
                }
                const auto pass_route =
                    materialPassRoute(
                        pass.materialInfo().contract);
                if (!pass_route || *pass_route != route) {
                    continue;
                }
                const auto &candidate =
                    pass.materialInfo().output_schema;
                if (!selected) {
                    result = candidate;
                    selected = true;
                    continue;
                }
                if (result != candidate) {
                    throw std::runtime_error(
                        "material route '" +
                        std::string{
                            materialRouteClassName(route)} +
                        "' resolves to different material_outputs "
                        "schemas across graph variants");
                }
            }
        };
    if (const auto generation = snapshot()) {
        visitPublishedRenderingPasses(
            *generation, collect);
    } else {
        for (const auto rendering_pass_id :
             registered_pass_ids) {
            collect(rendering_passes.get(
                rendering_pass_id));
        }
    }
    return result;
}

vk::SampleCountFlagBits
RenderingPassContainer::materialRasterizationSamples(
    MaterialShaderContract shader_contract) const {
    std::optional<vk::SampleCountFlagBits> samples;
    const auto collect =
        [&](const CompiledRenderingPass &rendering_pass) {
            for (const auto &compiled :
                 rendering_pass.passes) {
                const auto &pass = compiled.definition;
                if (!pass.isMaterial()) continue;
                const auto pass_shader_contract =
                    materialPassShaderContract(
                        pass.materialInfo().contract);
                const bool legacy_compatible =
                    pass.materialInfo().contract ==
                        MaterialPassContract::legacy_gbuffer_v1 &&
                    (shader_contract ==
                         MaterialShaderContract::legacy_gbuffer_v1 ||
                     shader_contract ==
                         MaterialShaderContract::gbuffer_v1);
                if (pass_shader_contract != shader_contract &&
                    !legacy_compatible) {
                    continue;
                }
                if (samples &&
                    *samples != pass.rasterization_samples) {
                    throw std::runtime_error(
                        "material shader contract is bound to passes with "
                        "different rasterization sample counts: " +
                        std::string{
                            materialShaderContractName(
                                shader_contract)});
                }
                samples = pass.rasterization_samples;
            }
        };
    if (const auto generation = snapshot()) {
        visitPublishedRenderingPasses(*generation, collect);
    } else {
        for (const auto rendering_pass_id :
             registered_pass_ids) {
            collect(rendering_passes.get(rendering_pass_id));
        }
    }
    return samples.value_or(vk::SampleCountFlagBits::e1);
}

} // namespace Pelican
