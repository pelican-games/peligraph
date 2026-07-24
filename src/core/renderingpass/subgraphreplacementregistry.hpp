#pragma once

#include "frameplanner.hpp"
#include "../../project/logicalrendergraph.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/render/subgraph_replacement_abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SubgraphReplacementRegistryState;

inline constexpr std::string_view
    builtinTaggedSubgraphReplacementProvider =
        "builtin.identity_v1";

struct TaggedRegionBoundaryPortContract {
    std::string resource;
    std::string type_json;
    RenderSubgraph::BoundaryDirectionV1 direction =
        RenderSubgraph::BoundaryDirectionV1::input;
    RenderSubgraph::MaterializationV1 materialization =
        RenderSubgraph::MaterializationV1::virtual_resource;

    bool operator==(
        const TaggedRegionBoundaryPortContract &) const = default;
};

struct TaggedRegionContract {
    std::string id;
    std::string graph_name;
    std::string region_tag;
    std::vector<TaggedRegionBoundaryPortContract>
        boundary_ports;
    std::vector<std::string> node_names;
    std::uint64_t fingerprint = 0;
};

struct ResolvedTaggedRegionReplacement {
    std::string subgraph_json;
    LogicalSubgraphReplacementSelection selection;
};

TaggedRegionContract makeTaggedRegionContract(
    const CompiledLogicalRenderGraph &graph,
    std::string_view region_tag);

class SubgraphReplacementRegistrySnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit SubgraphReplacementRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept;
    friend class SubgraphReplacementRegistry;

  public:
    SubgraphReplacementRegistrySnapshot() noexcept;
    ~SubgraphReplacementRegistrySnapshot();
    SubgraphReplacementRegistrySnapshot(
        SubgraphReplacementRegistrySnapshot &&) noexcept;
    SubgraphReplacementRegistrySnapshot &operator=(
        SubgraphReplacementRegistrySnapshot &&) noexcept;
    SubgraphReplacementRegistrySnapshot(
        const SubgraphReplacementRegistrySnapshot &) = delete;
    SubgraphReplacementRegistrySnapshot &operator=(
        const SubgraphReplacementRegistrySnapshot &) = delete;

    explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

    ResolvedTaggedRegionReplacement resolveRegion(
        const TaggedRegionContract &contract,
        std::string_view authored_subgraph_json,
        const std::optional<std::string>
            &requested_provider) const;
};

class SubgraphReplacementRegistry {
    std::unique_ptr<SubgraphReplacementRegistryState> impl_;

  public:
    SubgraphReplacementRegistry();
    ~SubgraphReplacementRegistry();
    SubgraphReplacementRegistry(
        const SubgraphReplacementRegistry &) = delete;
    SubgraphReplacementRegistry &operator=(
        const SubgraphReplacementRegistry &) = delete;

    RenderSubgraph::Status registerProvider(
        const RenderSubgraph::ProviderV1 &provider,
        internal::RegistrationOwner owner,
        RenderSubgraph::ProviderHandleV1 &out_handle) noexcept;
    RenderSubgraph::Status unregisterProvider(
        RenderSubgraph::ProviderHandleV1 handle,
        internal::RegistrationOwner owner) noexcept;

    SubgraphReplacementRegistrySnapshot snapshot() const;

    void activateOwner(
        internal::RegistrationOwner owner) noexcept;
    void releaseOwner(
        internal::RegistrationOwner owner) noexcept;
};

struct ResolvedTaggedSubgraphGraph {
    std::string graph;
    std::vector<LogicalSubgraphReplacementSelection>
        selections;
};

struct ResolvedTaggedSubgraphConfig {
    nlohmann::json config;
    std::vector<ResolvedTaggedSubgraphGraph> graphs;
};

ResolvedTaggedSubgraphConfig
resolveTaggedSubgraphReplacements(
    const nlohmann::json &config,
    const SubgraphReplacementRegistrySnapshot &providers);

void applyResolvedTaggedSubgraphSelections(
    std::span<FrameGraphDefinition> frame_graphs,
    std::span<const ResolvedTaggedSubgraphGraph>
        resolved_graphs);

SubgraphReplacementRegistry &
subgraphReplacementRegistry();

namespace render_subgraph_internal {
void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept;
}

} // namespace Pelican
