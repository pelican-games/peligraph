#pragma once

#include "frameplanner.hpp"
#include "../../project/graphvariantpolicy.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/render/render_strategy_abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

struct RenderStrategyRegistryState;

inline constexpr std::string_view
    builtinAuthoredRenderStrategyProvider =
        "builtin.authored_config_v1";

struct ResolvedRenderStrategy {
    std::string config_json;
    RenderStrategySelection selection;
};

class RenderStrategyRegistrySnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit RenderStrategyRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept;
    friend class RenderStrategyRegistry;

  public:
    RenderStrategyRegistrySnapshot() noexcept;
    ~RenderStrategyRegistrySnapshot();
    RenderStrategyRegistrySnapshot(
        RenderStrategyRegistrySnapshot &&) noexcept;
    RenderStrategyRegistrySnapshot &operator=(
        RenderStrategyRegistrySnapshot &&) noexcept;
    RenderStrategyRegistrySnapshot(
        const RenderStrategyRegistrySnapshot &) = delete;
    RenderStrategyRegistrySnapshot &operator=(
        const RenderStrategyRegistrySnapshot &) = delete;

    explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

    ResolvedRenderStrategy resolveStrategy(
        std::string_view strategy_name,
        std::string_view parameters_json,
        std::string_view seed_config_json,
        std::uint64_t seed_config_fingerprint,
        const CompiledGraphVariantPolicy
            &graph_variant_policy,
        bool runtime_shader_compiler_enabled,
        const std::optional<std::string>
            &requested_provider) const;
};

class RenderStrategyRegistry {
    std::unique_ptr<RenderStrategyRegistryState> impl_;

  public:
    RenderStrategyRegistry();
    ~RenderStrategyRegistry();
    RenderStrategyRegistry(
        const RenderStrategyRegistry &) = delete;
    RenderStrategyRegistry &operator=(
        const RenderStrategyRegistry &) = delete;

    RenderStrategy::Status registerProvider(
        const RenderStrategy::ProviderV1 &provider,
        internal::RegistrationOwner owner,
        RenderStrategy::ProviderHandleV1
            &out_handle) noexcept;
    RenderStrategy::Status unregisterProvider(
        RenderStrategy::ProviderHandleV1 handle,
        internal::RegistrationOwner owner) noexcept;

    RenderStrategyRegistrySnapshot snapshot() const;

    void activateOwner(
        internal::RegistrationOwner owner) noexcept;
    void releaseOwner(
        internal::RegistrationOwner owner) noexcept;
};

struct ResolvedRenderStrategyConfig {
    nlohmann::json config;
    std::optional<RenderStrategySelection>
        selection;
};

ResolvedRenderStrategyConfig resolveRenderStrategy(
    const nlohmann::json &config,
    const CompiledGraphVariantPolicy
        &graph_variant_policy,
    bool runtime_shader_compiler_enabled,
    const RenderStrategyRegistrySnapshot &providers);

void applyResolvedRenderStrategySelection(
    std::span<FrameGraphDefinition> frame_graphs,
    const std::optional<RenderStrategySelection>
        &selection);

RenderStrategyRegistry &renderStrategyRegistry();

namespace render_strategy_internal {
void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept;
}

} // namespace Pelican
