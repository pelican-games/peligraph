#pragma once

#include "renderingpass.hpp"
#include "../../project/logicalrendergraph.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/render/pass_implementation_abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct VulkanTargetPlan;
struct PassImplementationRegistryState;

inline constexpr std::string_view
    builtinFullscreenPassImplementationProvider =
        "builtin.fullscreen_v1";

struct PassImplementationPortContract {
    std::string name;
    std::string type_pattern_json;
    std::string relations_json;
    RenderPass::PortDirectionV1 direction =
        RenderPass::PortDirectionV1::input;
    RenderPass::AccessModeV1 access =
        RenderPass::AccessModeV1::read;
    RenderPass::AccessIntentV1 intent =
        RenderPass::AccessIntentV1::automatic;
    RenderPass::ReadFootprintV1 footprint =
        RenderPass::ReadFootprintV1::none;
    std::optional<std::uint32_t> footprint_radius;
};

struct PassImplementationContract {
    std::string id;
    std::string pass_name;
    std::uint32_t interface_flags = 0;
    std::vector<PassImplementationPortContract> ports;
    std::uint64_t fingerprint = 0;
};

struct ResolvedFullscreenPassImplementation {
    ShaderReference vertex_shader;
    ShaderReference fragment_shader;
    PassImplementationSelection selection;
};

PassImplementationContract makeFullscreenPassImplementationContract(
    const PassDefinition &pass,
    const LogicalGraphNode &logical_node);

class PassImplementationRegistrySnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit PassImplementationRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept;
    friend class PassImplementationRegistry;

  public:
    PassImplementationRegistrySnapshot() noexcept;
    ~PassImplementationRegistrySnapshot();
    PassImplementationRegistrySnapshot(
        PassImplementationRegistrySnapshot &&) noexcept;
    PassImplementationRegistrySnapshot &operator=(
        PassImplementationRegistrySnapshot &&) noexcept;
    PassImplementationRegistrySnapshot(
        const PassImplementationRegistrySnapshot &) = delete;
    PassImplementationRegistrySnapshot &operator=(
        const PassImplementationRegistrySnapshot &) = delete;

    explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

    ResolvedFullscreenPassImplementation resolveFullscreen(
        const PassDefinition &pass,
        const LogicalGraphNode &logical_node) const;
};

class PassImplementationRegistry {
    std::unique_ptr<PassImplementationRegistryState> impl_;

  public:
    PassImplementationRegistry();
    ~PassImplementationRegistry();
    PassImplementationRegistry(
        const PassImplementationRegistry &) = delete;
    PassImplementationRegistry &operator=(
        const PassImplementationRegistry &) = delete;

    RenderPass::Status registerProvider(
        const RenderPass::ProviderV1 &provider,
        internal::RegistrationOwner owner,
        RenderPass::ProviderHandleV1 &out_handle) noexcept;
    RenderPass::Status unregisterProvider(
        RenderPass::ProviderHandleV1 handle,
        internal::RegistrationOwner owner) noexcept;

    PassImplementationRegistrySnapshot snapshot() const;

    void activateOwner(
        internal::RegistrationOwner owner) noexcept;
    void releaseOwner(
        internal::RegistrationOwner owner) noexcept;
};

void resolveRenderingPassImplementations(
    RenderingPassDefinition &definition,
    const VulkanTargetPlan &target_plan,
    const PassImplementationRegistrySnapshot &providers);

PassImplementationRegistry &passImplementationRegistry();

namespace render_pass_internal {
void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept;
}

} // namespace Pelican
