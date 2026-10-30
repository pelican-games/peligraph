#include "targetrenderplanning.hpp"
#include "graphcomposition.hpp"
#include "renderpipeline.hpp"

#include "imagesubresourcejson.hpp"
#include "stablefingerprint.hpp"

#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {

namespace {

void requireNonEmpty(
    std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(
            std::string{subject} + " must not be empty");
    }
}

void appendFingerprintBoolean(
    StableFingerprint64 &fingerprint, bool value) {
    fingerprint.appendUnsigned(value ? 1u : 0u);
}

void appendFingerprintSigned(
    StableFingerprint64 &fingerprint, std::int64_t value) {
    fingerprint.appendUnsigned(
        static_cast<std::uint64_t>(value));
}

void appendFingerprintFloat(
    StableFingerprint64 &fingerprint, float value) {
    fingerprint.appendUnsigned(
        std::bit_cast<std::uint32_t>(value));
}

void appendFingerprintDouble(
    StableFingerprint64 &fingerprint, double value) {
    fingerprint.appendUnsigned(
        std::bit_cast<std::uint64_t>(value));
}

template <typename Range, typename Append>
void appendFingerprintRange(
    StableFingerprint64 &fingerprint,
    const Range &values, Append append) {
    fingerprint.appendUnsigned(values.size());
    for (const auto &value : values) {
        append(fingerprint, value);
    }
}

template <typename Range>
void appendFingerprintStrings(
    StableFingerprint64 &fingerprint,
    const Range &values) {
    appendFingerprintRange(
        fingerprint, values,
        [](StableFingerprint64 &target,
           const auto &value) {
            target.appendString(value);
        });
}

template <typename Value, typename Append>
void appendFingerprintOptional(
    StableFingerprint64 &fingerprint,
    const std::optional<Value> &value,
    Append append) {
    appendFingerprintBoolean(
        fingerprint, value.has_value());
    if (value) append(fingerprint, *value);
}

void appendSemanticTypeIdFingerprint(
    StableFingerprint64 &fingerprint,
    const SemanticTypeId &id) {
    fingerprint.appendString(id.name_space);
    fingerprint.appendString(id.name);
    fingerprint.appendUnsigned(id.major_version);
}

void appendRationalFingerprint(
    StableFingerprint64 &fingerprint,
    const Rational &value) {
    appendFingerprintSigned(
        fingerprint, value.numerator);
    fingerprint.appendUnsigned(value.denominator);
}

template <typename>
inline constexpr bool unsupportedFingerprintValue = false;

void appendTypeArgumentValueFingerprint(
    StableFingerprint64 &fingerprint,
    const TypeArgumentValue &value) {
    fingerprint.appendString(
        typeArgumentValueKindName(
            typeArgumentValueKind(value)));
    std::visit(
        [&](const auto &entry) {
            using Entry =
                std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<Entry, bool>) {
                appendFingerprintBoolean(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, std::int64_t>) {
                appendFingerprintSigned(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, std::uint64_t>) {
                fingerprint.appendUnsigned(entry);
            } else if constexpr (
                std::is_same_v<Entry, Rational>) {
                appendRationalFingerprint(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, EnumValueId> ||
                std::is_same_v<Entry, SymbolId>) {
                fingerprint.appendString(entry.value);
            } else if constexpr (
                std::is_same_v<Entry, SemanticTypeId>) {
                appendSemanticTypeIdFingerprint(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, IntegerInterval>) {
                appendFingerprintSigned(
                    fingerprint, entry.minimum);
                appendFingerprintSigned(
                    fingerprint, entry.maximum);
            } else if constexpr (
                std::is_same_v<Entry, EnumValueSet>) {
                appendFingerprintRange(
                    fingerprint, entry.values,
                    [](StableFingerprint64 &target,
                       const EnumValueId &item) {
                        target.appendString(item.value);
                    });
            } else {
                static_assert(
                    unsupportedFingerprintValue<Entry>);
            }
        },
        value);
}

void appendLogicalTypeFingerprint(
    StableFingerprint64 &fingerprint,
    const LogicalType &type) {
    fingerprint.appendString(
        logicalTypeConstructorName(type.constructor));
    appendSemanticTypeIdFingerprint(
        fingerprint, type.semantic);
    appendFingerprintRange(
        fingerprint, type.arguments,
        [](StableFingerprint64 &target,
           const LogicalTypeArgument &argument) {
            target.appendString(argument.name);
            target.appendString(
                typeArgumentRoleName(argument.role));
            appendTypeArgumentValueFingerprint(
                target, argument.value);
        });
}

void appendTargetTopologyFingerprint(
    StableFingerprint64 &fingerprint,
    const TargetTopologySnapshot &topology) {
    fingerprint.appendString(topology.name);
    appendFingerprintRange(
        fingerprint, topology.endpoints,
        [](StableFingerprint64 &target,
           const TargetEndpoint &endpoint) {
            target.appendString(endpoint.id);
            target.appendString(
                targetEndpointKindName(endpoint.kind));
            appendFingerprintStrings(
                target, endpoint.capabilities);
            appendFingerprintRange(
                target, endpoint.facts,
                [](StableFingerprint64 &fact_hash,
                   const TargetFact &fact) {
                    fact_hash.appendString(fact.name);
                    fact_hash.appendString(fact.value);
                });
        });
    appendFingerprintRange(
        fingerprint, topology.links,
        [](StableFingerprint64 &target,
           const TargetEndpointLink &link) {
            target.appendString(link.id);
            target.appendString(link.source_endpoint);
            target.appendString(
                link.destination_endpoint);
            appendFingerprintStrings(
                target, link.capabilities);
            appendFingerprintStrings(
                target, link.bridge_offers);
        });
}

void appendProviderFingerprint(
    StableFingerprint64 &fingerprint,
    const CompilerProviderDescriptor &provider) {
    fingerprint.appendString(provider.id);
    fingerprint.appendString(
        compilerProviderKindName(provider.kind));
    fingerprint.appendUnsigned(provider.owner_identity);
    fingerprint.appendUnsigned(provider.owner_generation);
    fingerprint.appendString(provider.content_hash);
    // This is also part of backendSelectionToJson and validates the provider.
    fingerprint.appendString(
        compilerProviderFingerprint(provider));
}

void appendCostFingerprint(
    StableFingerprint64 &fingerprint,
    const BackendCostEstimate &cost) {
    fingerprint.appendUnsigned(cost.rendering_scopes);
    fingerprint.appendUnsigned(
        cost.materialized_resources);
    fingerprint.appendUnsigned(cost.external_stores);
    fingerprint.appendUnsigned(cost.transient_bytes);
    fingerprint.appendUnsigned(cost.bandwidth_class);
}

void appendDiagnosticFingerprint(
    StableFingerprint64 &fingerprint,
    const PlanningDiagnostic &diagnostic) {
    fingerprint.appendString(diagnostic.id);
    fingerprint.appendString(
        planningDiagnosticSeverityName(
            diagnostic.severity));
    fingerprint.appendString(diagnostic.subject);
    fingerprint.appendString(diagnostic.detail);
}

void appendBackendSelectionFingerprint(
    StableFingerprint64 &fingerprint,
    const BackendSelection &selection) {
    requireNonEmpty(
        selection.selected_candidate,
        "selected backend candidate");
    fingerprint.appendString(
        selection.selected_candidate);
    appendFingerprintRange(
        fingerprint, selection.candidates,
        [](StableFingerprint64 &target,
           const BackendProbeResult &candidate) {
            target.appendString(candidate.candidate);
            appendProviderFingerprint(
                target, candidate.provider);
            target.appendString(candidate.endpoint);
            appendFingerprintBoolean(
                target, candidate.feasible);
            appendFingerprintStrings(
                target,
                candidate.required_physical_features);
            appendCostFingerprint(target, candidate.cost);
            appendFingerprintOptional(
                target, candidate.selected_link,
                [](StableFingerprint64 &nested,
                   const std::string &link) {
                    nested.appendString(link);
                });
            appendFingerprintStrings(
                target, candidate.bridge_offers);
            appendFingerprintRange(
                target, candidate.failures,
                [](StableFingerprint64 &nested,
                   const BackendConstraintFailure &failure) {
                    nested.appendString(failure.id);
                    nested.appendString(failure.subject);
                    nested.appendString(failure.detail);
                });
            appendFingerprintRange(
                target, candidate.diagnostics,
                appendDiagnosticFingerprint);
        });
    appendFingerprintRange(
        fingerprint, selection.diagnostics,
        appendDiagnosticFingerprint);
    // Backend decisions are explanatory output and were explicitly removed
    // from the former canonical JSON fingerprint payload.
}

void appendLifetimeFingerprint(
    StableFingerprint64 &fingerprint,
    const TargetResourceLifetime &lifetime) {
    appendFingerprintBoolean(
        fingerprint, lifetime.used);
    if (lifetime.used) {
        fingerprint.appendUnsigned(lifetime.first_use);
        fingerprint.appendUnsigned(lifetime.last_use);
    }
    if (!lifetime.retain.empty()) {
        fingerprint.appendUnsigned(lifetime.retain.size());
        for (const auto &reason : lifetime.retain) fingerprint.appendString(reason);
    }
}

void appendExtentFingerprint(
    StableFingerprint64 &fingerprint,
    const ResourceExtentPlan &extent) {
    fingerprint.appendString(
        resourceExtentKindName(extent.kind));
    appendFingerprintFloat(fingerprint, extent.scale_x);
    appendFingerprintFloat(fingerprint, extent.scale_y);
    fingerprint.appendUnsigned(extent.width);
    fingerprint.appendUnsigned(extent.height);
}

void appendMipLevelsFingerprint(
    StableFingerprint64 &fingerprint,
    const ImageMipLevelCount &mip_levels) {
    fingerprint.appendString(
        imageMipLevelModeName(mip_levels.mode));
    fingerprint.appendUnsigned(mip_levels.count);
}

void appendTargetLoweringGraphFingerprint(
    StableFingerprint64 &fingerprint,
    const TargetLoweringGraph &graph) {
    validateTargetLoweringGraphDialect(
        graph, graph.stage);
    fingerprint.appendString(graph.name);
    fingerprint.appendString(
        targetLoweringStageName(graph.stage));
    appendFingerprintRange(
        fingerprint, graph.nodes,
        [](StableFingerprint64 &target,
           const TargetLoweringNode &node) {
            target.appendString(node.logical.name);
            target.appendString(
                logicalGraphNodeKindName(
                    node.logical.kind));
            target.appendString(
                targetIrDialectName(node.dialect));
            appendFingerprintStrings(
                target, node.source_nodes);
            appendFingerprintStrings(
                target, node.logical.region_tags);
            appendFingerprintStrings(
                target,
                node.required_physical_features);
            if (!node.logical.view_select.empty()) {
                target.appendString("view_select");
                target.appendUnsigned(
                    node.logical.view_select.size());
                for (const auto &[axis, value] :
                     node.logical.view_select) {
                    target.appendString(axis);
                    target.appendUnsigned(value);
                }
            }
        });
    appendFingerprintRange(
        fingerprint, graph.resources,
        [](StableFingerprint64 &target,
           const TargetLoweringResource &resource) {
            target.appendString(resource.logical.name);
            target.appendString(
                targetIrDialectName(resource.dialect));
            appendLogicalTypeFingerprint(
                target, resource.logical.type);
            target.appendString(
                logicalMaterializationRequirementName(
                    resource.logical.materialization));
            target.appendString(resource.pattern.id);
            appendFingerprintRange(
                target,
                resource.pattern.format_candidates,
                [](StableFingerprint64 &nested,
                   const ResourceFormatCandidate &candidate) {
                    nested.appendString(
                        imageFormatName(candidate.format));
                    appendFingerprintStrings(
                        nested,
                        candidate.required_capabilities);
                });
            appendFingerprintBoolean(
                target,
                resource.pattern.prefer_transient);
            appendFingerprintBoolean(
                target,
                resource.pattern.allow_tile_local);
            appendFingerprintBoolean(
                target, resource.pattern.allow_alias);
            appendFingerprintBoolean(
                target, resource.pattern.require_store);
            target.appendString(
                resourcePatternFallbackName(
                    resource.pattern
                        .local_read_fallback));
            target.appendUnsigned(
                resource.pattern.estimated_bytes);
            target.appendString(
                resource.pattern.provenance);
            appendFingerprintOptional(
                target, resource.extent,
                appendExtentFingerprint);
            appendMipLevelsFingerprint(
                target, resource.mip_levels);
            target.appendUnsigned(resource.array_layers);
            target.appendString(
                imageResourceDimensionName(
                    resource.dimension));
            appendFingerprintBoolean(
                target, resource.uses.read);
            appendFingerprintBoolean(
                target, resource.uses.written);
            appendFingerprintBoolean(
                target,
                resource.uses.attachment_access);
            appendFingerprintBoolean(
                target, resource.uses.sampled_access);
            appendFingerprintBoolean(
                target, resource.uses.storage_access);
            appendFingerprintBoolean(
                target, resource.uses.transfer_access);
            appendFingerprintBoolean(
                target, resource.uses.host_access);
            appendFingerprintBoolean(
                target,
                resource.uses.non_render_access);
            appendFingerprintBoolean(
                target,
                resource.uses.produced_by_snapshot);
            target.appendString(
                logicalReadFootprintKindName(
                    resource.uses.widest_read));
            appendLifetimeFingerprint(
                target, resource.lifetime);
            appendFingerprintStrings(
                target,
                resource.required_physical_features);
        });
    appendFingerprintRange(
        fingerprint, graph.decisions,
        [](StableFingerprint64 &target,
           const PlanningDecision &decision) {
            target.appendString(decision.id);
            target.appendString(decision.subject);
            target.appendString(decision.selected);
            target.appendString(decision.detail);
        });
}

void appendPhysicalResourceFingerprint(
    StableFingerprint64 &fingerprint,
    const VulkanPhysicalResourcePlan &resource) {
    fingerprint.appendString(resource.logical_resource);
    fingerprint.appendString(resource.pattern);
    fingerprint.appendString(
        imageFormatNameOrEmpty(resource.format));
    fingerprint.appendString(
        vulkanResourceRepresentationName(
            resource.representation));
    fingerprint.appendString(
        logicalReadFootprintKindName(
            resource.widest_read));
    appendLifetimeFingerprint(
        fingerprint, resource.lifetime);
    appendFingerprintBoolean(
        fingerprint, resource.stored);
    appendFingerprintBoolean(
        fingerprint, resource.aliasable);
    appendFingerprintStrings(
        fingerprint,
        resource.required_physical_features);
    fingerprint.appendString(resource.reason);
    fingerprint.appendUnsigned(
        resource.rasterization_samples);
    appendFingerprintBoolean(
        fingerprint, resource.resolve_required);
    fingerprint.appendString(
        vulkanResourceViewLayoutName(
            resource.view_layout));
    appendMipLevelsFingerprint(
        fingerprint, resource.mip_levels);
    fingerprint.appendUnsigned(resource.array_layers);
    fingerprint.appendString(
        imageResourceDimensionName(resource.dimension));
    appendFingerprintOptional(
        fingerprint, resource.extent,
        appendExtentFingerprint);
}

void appendPhysicalScopeFingerprint(
    StableFingerprint64 &fingerprint,
    const VulkanPhysicalScopePlan &scope) {
    fingerprint.appendString(scope.id);
    fingerprint.appendString(
        vulkanPhysicalScopeKindName(scope.kind));
    appendFingerprintStrings(fingerprint, scope.nodes);
    appendFingerprintBoolean(
        fingerprint,
        scope.single_rendering_instance);
    appendFingerprintStrings(
        fingerprint, scope.local_reads);
    appendFingerprintStrings(
        fingerprint, scope.region_tags);
    fingerprint.appendUnsigned(
        scope.rasterization_samples);
    fingerprint.appendString(
        vulkanScopeViewExecutionName(
            scope.view_execution));
    fingerprint.appendUnsigned(scope.view_count);
    fingerprint.appendUnsigned(scope.execution_count);
    fingerprint.appendUnsigned(scope.view_mask);
    if (!scope.view_select.empty()) {
        fingerprint.appendString("view_select");
        fingerprint.appendUnsigned(scope.view_select.size());
        for (const auto &[axis, value] : scope.view_select) {
            fingerprint.appendString(axis);
            fingerprint.appendUnsigned(value);
        }
    }
}

void appendImageSubresourceFingerprint(
    StableFingerprint64 &fingerprint,
    const ImageSubresourceRange &range) {
    fingerprint.appendUnsigned(range.base_mip_level);
    fingerprint.appendUnsigned(range.base_array_layer);
    fingerprint.appendUnsigned(range.layer_count);
    const auto remaining =
        range.mip_count_mode ==
        ImageSubresourceMipCountMode::remaining;
    appendFingerprintBoolean(fingerprint, remaining);
    if (!remaining) {
        fingerprint.appendUnsigned(range.level_count);
    }
}

void appendPhysicalAttachmentFingerprint(
    StableFingerprint64 &fingerprint,
    const VulkanPhysicalAttachmentPlan &attachment) {
    fingerprint.appendString(attachment.node);
    fingerprint.appendString(
        attachment.logical_resource);
    fingerprint.appendString(
        attachmentAspectName(
            attachment.aspect));
    fingerprint.appendString(
        attachmentLoadOpPlanName(
            attachment.operations.load_op));
    fingerprint.appendString(
        attachmentStoreOpPlanName(
            attachment.operations.store_op));
    appendFingerprintOptional(
        fingerprint, attachment.subresource,
        appendImageSubresourceFingerprint);
}

void appendGpuMeasurementFingerprint(
    StableFingerprint64 &fingerprint,
    const XrMultiviewGpuMeasurement &measurement) {
    appendFingerprintDouble(
        fingerprint, measurement.sequential_gpu_ms);
    appendFingerprintDouble(
        fingerprint, measurement.multiview_gpu_ms);
    fingerprint.appendUnsigned(measurement.sample_count);
    fingerprint.appendString(measurement.source);
}

void appendResolvedAutoPolicyFingerprint(
    StableFingerprint64 &fingerprint,
    const ResolvedXrMultiviewAutoPolicy &policy) {
    fingerprint.appendString(
        xrMultiviewAutoSelectionName(
            policy.selection));
    fingerprint.appendString(policy.profile_id);
    appendFingerprintBoolean(
        fingerprint, policy.matched_profile);
    fingerprint.appendUnsigned(policy.device.vendor_id);
    fingerprint.appendUnsigned(policy.device.device_id);
    fingerprint.appendUnsigned(
        policy.device.driver_version);
    fingerprint.appendString(policy.device.device_name);
    fingerprint.appendString(policy.graph);
    appendFingerprintDouble(
        fingerprint, policy.measured_gain_percent);
    fingerprint.appendString(policy.reason);
    appendFingerprintOptional(
        fingerprint, policy.measurement,
        appendGpuMeasurementFingerprint);
}

void appendViewExecutionFingerprint(
    StableFingerprint64 &fingerprint,
    const VulkanViewExecutionPlan &plan) {
    fingerprint.appendUnsigned(plan.view_count);
    fingerprint.appendString(
        xrViewExecutionPreferenceName(plan.requested));
    appendFingerprintBoolean(
        fingerprint,
        plan.endpoint_supports_multiview);
    fingerprint.appendUnsigned(
        plan.max_multiview_view_count);
    appendFingerprintBoolean(
        fingerprint, plan.uses_multiview);
    appendFingerprintBoolean(
        fingerprint, plan.mixed_execution);
    appendResolvedAutoPolicyFingerprint(
        fingerprint, plan.automatic_policy);
    fingerprint.appendString(plan.reason);
}

void appendSampleCountPlanFingerprint(
    StableFingerprint64 &fingerprint,
    const ResolvedSampleCountPlan &plan) {
    fingerprint.appendString(
        sampleCountRequestModeName(plan.request.mode));
    fingerprint.appendUnsigned(plan.request.samples);
    appendFingerprintRange(
        fingerprint, plan.groups,
        [](StableFingerprint64 &target,
           const ResolvedSampleCountGroup &group) {
            target.appendString(group.id);
            target.appendUnsigned(
                group.requested_samples);
            target.appendUnsigned(
                group.selected_samples);
            appendFingerprintBoolean(
                target, group.fallback);
            appendFingerprintStrings(
                target, group.limiting_resources);
            target.appendString(group.reason);
        });
    appendFingerprintRange(
        fingerprint, plan.resources,
        [](StableFingerprint64 &target,
           const ResolvedSampleCountResource &resource) {
            target.appendString(resource.resource);
            target.appendString(imageFormatNameOrEmpty(resource.format));
            target.appendUnsigned(resource.samples);
        });
}

} // namespace

void validateVulkanPhysicalAttachmentPlans(
    const CompiledLogicalRenderGraph &canonical_graph,
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::span<const VulkanPhysicalAttachmentPlan> attachments) {
    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : canonical_graph.nodes) {
        nodes.emplace(node.name, &node);
    }
    std::map<
        std::string,
        const VulkanPhysicalResourcePlan *,
        std::less<>>
        physical_resources;
    for (const auto &resource : resources) {
        physical_resources.emplace(
            resource.logical_resource, &resource);
    }
    std::set<
        std::pair<std::string, std::string>>
        identities;
    for (const auto &attachment : attachments) {
        requireNonEmpty(
            attachment.node,
            "Vulkan physical attachment node");
        requireNonEmpty(
            attachment.logical_resource,
            "Vulkan physical attachment resource");
        if (!identities.emplace(
                attachment.node,
                attachment.logical_resource)
                 .second) {
            throw std::runtime_error(
                "Vulkan physical attachment plan has duplicate "
                "node/resource identity: " +
                attachment.node + " -> " +
                attachment.logical_resource);
        }
        const auto physical_resource =
            physical_resources.find(
                attachment.logical_resource);
        if (physical_resource ==
            physical_resources.end()) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references an "
                "unknown physical resource: " +
                attachment.logical_resource);
        }
        if (attachment.subresource) {
            const auto &range =
                *attachment.subresource;
            if (attachment.logical_resource ==
                    "swapchain" ||
                range.mip_count_mode !=
                    ImageSubresourceMipCountMode::fixed ||
                range.level_count != 1 ||
                range.layer_count == 0 ||
                range.base_array_layer >=
                    physical_resource->second
                        ->array_layers ||
                range.layer_count >
                    physical_resource->second
                            ->array_layers -
                        range.base_array_layer ||
                (physical_resource->second
                         ->mip_levels.mode ==
                     ImageMipLevelMode::fixed &&
                 range.base_mip_level >=
                     physical_resource->second
                         ->mip_levels.count) ||
                (physical_resource->second
                         ->rasterization_samples >
                     1 &&
                 range.base_mip_level != 0)) {
                throw std::runtime_error(
                    "Vulkan physical attachment plan has an "
                    "unsupported image subresource: " +
                    attachment.node + " -> " +
                    attachment.logical_resource);
            }
        }
        const auto found_node =
            nodes.find(attachment.node);
        if (found_node == nodes.end()) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references an "
                "unknown logical node: " +
                attachment.node);
        }
        if (found_node->second->kind !=
                LogicalGraphNodeKind::render &&
            found_node->second->kind !=
                LogicalGraphNodeKind::output_transform) {
            throw std::runtime_error(
                "Vulkan physical attachment plan references a "
                "non-raster logical node: " +
                attachment.node);
        }
        const auto writes_resource =
            std::any_of(
                found_node->second->uses.begin(),
                found_node->second->uses.end(),
                [&](const LogicalResourceUse &use) {
                    return use.output_value &&
                           use.output_value->resource ==
                               attachment.logical_resource;
                });
        if (!writes_resource) {
            throw std::runtime_error(
                "Vulkan physical attachment plan resource is not "
                "written by its logical node: " +
                attachment.node + " -> " +
                attachment.logical_resource);
        }
        const auto loads_existing_attachment =
            std::any_of(
                found_node->second->uses.begin(),
                found_node->second->uses.end(),
                [&](const LogicalResourceUse &use) {
                    return use.access ==
                               LogicalAccessMode::read_write &&
                           use.input_value &&
                           use.output_value &&
                           use.input_value->resource ==
                               attachment.logical_resource &&
                           use.output_value->resource ==
                               attachment.logical_resource &&
                           use.footprint.kind ==
                               LogicalReadFootprintKind::same_pixel &&
                           (use.intent ==
                                LogicalAccessIntent::automatic ||
                            use.intent ==
                                LogicalAccessIntent::attachment);
                });
        const auto physically_loads =
            attachment.operations.load_op ==
            AttachmentLoadOp::load;
        if (loads_existing_attachment != physically_loads) {
            throw std::runtime_error(
                physically_loads
                    ? "Vulkan physical attachment Load has no "
                      "logical read dependency: " +
                          attachment.node + " -> " +
                          attachment.logical_resource
                    : "Vulkan physical attachment cannot discard or "
                      "clear a logical read dependency: " +
                          attachment.node + " -> " +
                          attachment.logical_resource);
        }
    }
}

std::uint64_t vulkanAutomaticTargetPlanFingerprint(
    const TargetTopologySnapshot &topology,
    const VulkanTargetPlan &automatic_plan) {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(
        "pelican.vulkan_automatic_target_plan@2");
    const auto canonical_topology =
        canonicalizeTargetTopology(topology);
    appendTargetTopologyFingerprint(
        fingerprint, canonical_topology);
    fingerprint.appendString(automatic_plan.graph);
    fingerprint.appendUnsigned(
        automatic_plan.logical_graph_fingerprint);
    appendBackendSelectionFingerprint(
        fingerprint,
        automatic_plan.backend_selection);
    appendTargetLoweringGraphFingerprint(
        fingerprint, automatic_plan.lowering_graph);
    appendFingerprintStrings(
        fingerprint,
        automatic_plan.required_physical_features);
    appendFingerprintRange(
        fingerprint, automatic_plan.resources,
        appendPhysicalResourceFingerprint);
    appendFingerprintRange(
        fingerprint, automatic_plan.scopes,
        appendPhysicalScopeFingerprint);
    appendFingerprintRange(
        fingerprint, automatic_plan.attachments,
        appendPhysicalAttachmentFingerprint);
    appendFingerprintRange(
        fingerprint, automatic_plan.alias_groups,
        [](StableFingerprint64 &target,
           const VulkanAliasGroupPlan &group) {
            target.appendString(group.id);
            appendFingerprintStrings(
                target, group.resources);
        });
    appendViewExecutionFingerprint(
        fingerprint,
        automatic_plan.view_execution_plan);
    appendFingerprintOptional(
        fingerprint, automatic_plan.sample_count_plan,
        appendSampleCountPlanFingerprint);
    appendFingerprintOptional(
        fingerprint,
        automatic_plan.external_depth_export,
        [](StableFingerprint64 &target,
           const VulkanExternalDepthExportPlan &depth) {
            target.appendString(depth.source_resource);
            target.appendString(imageFormatName(depth.format));
            target.appendString(
                vulkanResourceViewLayoutName(
                    depth.view_layout));
            target.appendUnsigned(depth.array_layers);
        });
    return fingerprint.value();
}

} // namespace Pelican
