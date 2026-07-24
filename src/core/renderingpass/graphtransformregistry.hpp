#pragma once

#include "frameplanner.hpp"
#include "../../project/logicalrendergraph.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/render/graph_transform_abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct GraphTransformRegistryState;

inline constexpr std::string_view
    builtinLogicalGraphTransformProvider =
        "builtin.identity_v1";

struct LogicalGraphSetBoundaryPortContract {
    std::string graph;
    std::string resource;
    std::string type_json;
    RenderGraphTransform::BoundaryRoleV1 role =
        RenderGraphTransform::BoundaryRoleV1::
            retained_resource;
    RenderGraphTransform::MaterializationV1
        materialization =
            RenderGraphTransform::MaterializationV1::
                virtual_resource;
    RenderGraphTransform::ImportKindV1 import_kind =
        RenderGraphTransform::ImportKindV1::none;

    bool operator==(
        const LogicalGraphSetBoundaryPortContract &) const =
        default;
};

struct LogicalGraphSetContract {
    std::string id;
    std::vector<std::string> graph_names;
    std::vector<LogicalGraphSetBoundaryPortContract>
        boundary_ports;
    std::uint64_t fingerprint = 0;
};

LogicalGraphSetContract makeLogicalGraphSetContract(
    std::span<const CompiledLogicalRenderGraph> graphs);

std::uint64_t logicalGraphSetFingerprint(
    std::span<const CompiledLogicalRenderGraph> graphs);

struct ResolvedLogicalGraphTransform {
    std::string config_json;
    LogicalGraphTransformSelection selection;
};

class GraphTransformRegistrySnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit GraphTransformRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept;
    friend class GraphTransformRegistry;

  public:
    GraphTransformRegistrySnapshot() noexcept;
    ~GraphTransformRegistrySnapshot();
    GraphTransformRegistrySnapshot(
        GraphTransformRegistrySnapshot &&) noexcept;
    GraphTransformRegistrySnapshot &operator=(
        GraphTransformRegistrySnapshot &&) noexcept;
    GraphTransformRegistrySnapshot(
        const GraphTransformRegistrySnapshot &) = delete;
    GraphTransformRegistrySnapshot &operator=(
        const GraphTransformRegistrySnapshot &) = delete;

    explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

    ResolvedLogicalGraphTransform resolveTransform(
        const LogicalGraphSetContract &contract,
        std::string_view transform_name,
        std::uint32_t transform_index,
        std::string_view parameters_json,
        std::string_view config_json,
        std::string_view logical_graphs_json,
        std::uint64_t logical_graphs_fingerprint,
        const std::optional<std::string>
            &requested_provider) const;
};

class GraphTransformRegistry {
    std::unique_ptr<GraphTransformRegistryState> impl_;

  public:
    GraphTransformRegistry();
    ~GraphTransformRegistry();
    GraphTransformRegistry(
        const GraphTransformRegistry &) = delete;
    GraphTransformRegistry &operator=(
        const GraphTransformRegistry &) = delete;

    RenderGraphTransform::Status registerProvider(
        const RenderGraphTransform::ProviderV1 &provider,
        internal::RegistrationOwner owner,
        RenderGraphTransform::ProviderHandleV1
            &out_handle) noexcept;
    RenderGraphTransform::Status unregisterProvider(
        RenderGraphTransform::ProviderHandleV1 handle,
        internal::RegistrationOwner owner) noexcept;

    GraphTransformRegistrySnapshot snapshot() const;

    void activateOwner(
        internal::RegistrationOwner owner) noexcept;
    void releaseOwner(
        internal::RegistrationOwner owner) noexcept;
};

struct ResolvedLogicalGraphTransformConfig {
    nlohmann::json config;
    std::vector<LogicalGraphTransformSelection>
        selections;
};

ResolvedLogicalGraphTransformConfig
resolveLogicalGraphTransforms(
    const nlohmann::json &config,
    const GraphTransformRegistrySnapshot &providers);

void applyResolvedLogicalGraphTransformSelections(
    std::span<FrameGraphDefinition> frame_graphs,
    std::span<const LogicalGraphTransformSelection>
        selections);

GraphTransformRegistry &graphTransformRegistry();

namespace render_graph_transform_internal {
void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept;
}

} // namespace Pelican
