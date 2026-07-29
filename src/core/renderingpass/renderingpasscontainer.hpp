#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "renderingpass.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct RendererRuntimeGeneration;
struct RendererRuntimePublication;

enum class MaterialPassShaderInputKind : std::uint8_t {
    screen_input,
    material_resource,
};

struct MaterialPassShaderInputRequest {
    MaterialPassShaderInputKind kind =
        MaterialPassShaderInputKind::screen_input;
    std::string name;

    bool operator==(
        const MaterialPassShaderInputRequest &) const = default;
};

// A missing index means that the physical plan materializes the image and the
// shader must use its sampled-image fallback. A present index is the
// VkRenderingInputAttachmentIndexInfoKHR index, not the descriptor binding.
struct MaterialPassShaderInputBinding {
    MaterialPassShaderInputRequest input;
    std::optional<std::uint32_t> input_attachment_index;
    // Physical shader-visible image shape selected by the active graph.
    // The surface compiler uses this independently from the logical port
    // declaration so the same authored algorithm can target a scalar 2D
    // image or a view-family array.
    PassInputViewDimension view_dimension =
        PassInputViewDimension::shared_2d;
    // Descriptor/image-view shape is independent from the scheduler's view
    // family layout. In particular a cube remains one shared logical
    // resource while exposing a samplerCube descriptor.
    ImageSubresourceViewDimension descriptor_dimension =
        ImageSubresourceViewDimension::two_d;

    bool operator==(
        const MaterialPassShaderInputBinding &) const = default;
};

struct MaterialPassRenderingBinding {
    std::string pass_name;
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    CompiledPassRenderingContract rendering;
    std::optional<MaterialOutputSchema> output_schema;
    std::vector<MaterialOutputAttachmentState> output_states;
    std::vector<MaterialPassShaderInputBinding> shader_inputs;

    bool operator==(
        const MaterialPassRenderingBinding &) const =
        default;
};

DECLARE_MODULE(RenderingPassContainer) {
    ResourceContainer<RenderingPassId, CompiledRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;
    std::vector<RenderingPassId> registered_pass_ids;
    std::vector<std::string> enabled_feature_names;
    std::atomic<
        std::shared_ptr<const RendererRuntimePublication>>
        runtime_publication;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    RenderingPassId registerCompiledRenderingPass(CompiledRenderingPass pass);
    RenderingPassId getRenderingPassIdByName(const std::string &name) const;
    // These reference-returning legacy views are valid only until the next
    // publication. Concurrent/frame-spanning readers must inspect snapshot()
    // directly.
    const CompiledRenderingPass &getCompiledRenderingPass(RenderingPassId rendering_pass_id) const;
    const std::vector<RenderingPassId> &getRegisteredPassIds() const;
    void setEnabledFeatures(std::vector<std::string> feature_names);
    bool isFeatureEnabled(std::string_view feature_name) const;
    const std::vector<std::string> &getEnabledFeatures() const;
    void bindRuntimePublication(
        std::shared_ptr<const RendererRuntimePublication>
            publication) noexcept;
    std::shared_ptr<const RendererRuntimeGeneration>
    snapshot() const noexcept;
    std::uint64_t activeGeneration() const noexcept;
    bool hasMaterialPasses() const;
    bool supportsMaterialPass(MaterialRouteClass route,
                              MaterialShaderContract shader_contract,
                              const std::optional<std::string> &exact_pass = std::nullopt) const;
    std::vector<MaterialPassRenderingBinding>
    materialPassRenderingBindings(
        MaterialRouteClass route,
        MaterialShaderContract shader_contract,
        const std::optional<std::string> &exact_pass =
            std::nullopt) const;
    // Resolves the shader-visible input implementation selected by every
    // active graph variant. The same material shader cannot mix a sampler and
    // an input attachment (or different input-attachment indices) across
    // variants, so disagreement is rejected before shader compilation.
    std::vector<MaterialPassShaderInputBinding>
    materialPassShaderInputBindings(
        MaterialRouteClass route,
        MaterialShaderContract shader_contract,
        std::span<const MaterialPassShaderInputRequest> inputs,
        const std::optional<std::string> &exact_pass =
            std::nullopt) const;
    // Resolves the strategy-private fragment-output ABI selected by a route.
    // All graph variants visible to the container must agree.
    std::optional<MaterialOutputSchema>
    materialOutputSchema(
        MaterialRouteClass route,
        const std::optional<std::string> &exact_pass =
            std::nullopt) const;
    vk::SampleCountFlagBits materialRasterizationSamples(
        MaterialShaderContract shader_contract) const;
};

} // namespace Pelican
