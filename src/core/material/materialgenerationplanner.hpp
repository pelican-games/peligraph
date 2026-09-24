#pragma once

#include "materialcontainer.hpp"
#include "materialdescriptorbuilder.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderresourceinterface.hpp"
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RendererRuntimeGeneration;

// The physical rendering contract a material pipeline is built against.
// Used both when a material is first registered and when a private render
// generation is re-planned against live materials.
struct MaterialPipelineRenderingContract {
    std::vector<vk::Format> color_formats;
    std::optional<vk::Format> depth_format;
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    GraphicsPipelineRenderingLocalReadContract local_read;
    std::vector<MaterialOutputAttachmentState>
        output_states;

    bool operator==(
        const MaterialPipelineRenderingContract &) const =
        default;
};

MaterialPipelineRenderingContract
defaultMaterialPipelineRenderingContract(
    MaterialShaderContract shader_contract,
    vk::SampleCountFlagBits samples);
MaterialPipelineRenderingContract
resolveMaterialPassRenderingBinding(
    const MaterialPassRenderingBinding &binding,
    MaterialShaderContract shader_contract);
MaterialPipelineRenderingContract
resolveMaterialPipelineRenderingContract(
    const MaterialInfo &info);
std::vector<GraphicsPipelineColorAttachmentState>
resolveMaterialColorAttachmentStates(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract &rendering);
void validateMaterialCapabilities(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract &rendering);

// Every pass of a render generation that would draw this material route.
std::vector<MaterialPassRenderingBinding>
materialBindingsForGeneration(
    const RendererRuntimeGeneration &generation,
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass);


std::vector<MaterialPassShaderInputBinding>
resolveGenerationShaderInputs(
    std::span<const MaterialPassRenderingBinding> passes,
    std::span<const MaterialPassShaderInputRequest>
        inputs);

// What the shader inputs a material declares resolve to in one candidate
// generation, plus the positions of the image-kind resource ports (the
// physical input table is screen inputs followed by image resource ports).
struct RequestedShaderInputs {
    std::vector<MaterialPassShaderInputRequest> requests;
    std::vector<std::size_t> image_resource_indices;
};
RequestedShaderInputs collectRequestedShaderInputs(
    std::span<const MaterialScreenInputContract>
        declared_screen_inputs,
    std::span<const SurfaceResourcePortDefinition>
        declared_resource_ports);

// One half of the ABI diff: the physical defines the live material was
// built with, the ones the candidate needs, and the binding metadata the
// candidate would carry. Keeping the two index-aligned walks apart is the
// point -- each owns exactly one physical-input range.
struct ScreenInputAbiPlan {
    std::vector<std::string> defines_before;
    std::vector<std::string> defines_after;
    std::vector<MaterialPassInputState> updated_bindings;
};
struct ResourcePortAbiPlan {
    std::vector<std::string> defines_before;
    std::vector<std::string> defines_after;
    std::vector<ShaderResourceInterfaceBinding>
        updated_bindings;
};

ScreenInputAbiPlan planScreenInputAbi(
    std::span<const MaterialScreenInputContract> declared,
    std::span<const MaterialPassInputState> live,
    std::span<const MaterialPassShaderInputBinding>
        physical_inputs);
ResourcePortAbiPlan planResourcePortAbi(
    std::span<const SurfaceResourcePortDefinition>
        declared,
    std::span<const ShaderResourceInterfaceBinding> live,
    std::span<const MaterialPassShaderInputBinding>
        physical_inputs,
    std::size_t physical_offset,
    std::span<const std::size_t> image_resource_indices);

// The two live-generation entry points of MaterialContainer. They need the
// material table, so they take the container and reach its material
// records as a friend; everything else above is free of it.
struct MaterialGenerationPlanner {
    static void validateCompatibility(
        const MaterialContainer &container,
        const RendererRuntimeGeneration &generation);
    static MaterialRuntimeGenerationReloadPlan
    prepareReload(
        MaterialContainer &container,
        const RendererRuntimeGeneration &generation);

  private:
    // G-4: the route and shader contract a live material has under the
    // candidate's material_route_default. `live` is false when the material
    // follows the default and the candidate moves it to another route; such a
    // material keeps its live pipeline until the engine restarts.
    struct CandidateRoute {
        MaterialRouteClass route;
        MaterialShaderContract shader_contract;
        bool live = true;
    };
    static CandidateRoute candidateRoute(
        const MaterialContainer &container,
        const MaterialContainer::InternalMaterialInfo &material);
    // G-4: every registered material, built-ins included, needs a receiving
    // pass whenever the candidate has any material pass. A candidate with no
    // material pass at all is refused when it drops a route the published
    // generation consumed. Collects all the materials that are left without
    // a pass and throws material_route_unserved@1 once.
    static void requireEveryMaterialServed(
        const MaterialContainer &container,
        const RendererRuntimeGeneration &generation);
    // The acceptance rule shared by registration, the validator and the
    // planner (G-4): the passes a candidate generation offers this material,
    // or nullopt when the generation has no material pass at all (an RT-only
    // graph draws no material). A generation with any material pass must
    // serve every registered material; otherwise this throws
    // material_route_unserved@1.
    static std::optional<std::vector<MaterialPassRenderingBinding>>
    resolveLiveMaterialBindings(
        const MaterialContainer &container,
        const RendererRuntimeGeneration &generation,
        const MaterialContainer::InternalMaterialInfo &material);
    // The live pipeline contract of one registered material, spelled once
    // instead of once per caller.
    static MaterialPipelineRenderingContract
    liveRenderingContract(
        const MaterialContainer::InternalMaterialInfo
            &material);
};

} // namespace Pelican
