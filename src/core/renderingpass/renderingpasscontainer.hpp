#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "renderingpass.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct RendererRuntimeGeneration;
struct RendererRuntimePublication;

struct MaterialPassRenderingBinding {
    std::string pass_name;
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    CompiledPassRenderingContract rendering;

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
    vk::SampleCountFlagBits materialRasterizationSamples(
        MaterialShaderContract shader_contract) const;
};

} // namespace Pelican
