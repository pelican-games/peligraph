#include "logicalframegraphadapter.hpp"

#include "frameplanner.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican {

namespace {

LogicalGraphNodeKind logicalNodeKind(FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render: return LogicalGraphNodeKind::render;
    case FramePlanNodeKind::compute: return LogicalGraphNodeKind::compute;
    case FramePlanNodeKind::anchor: return LogicalGraphNodeKind::anchor;
    case FramePlanNodeKind::snapshot_copy:
        return LogicalGraphNodeKind::snapshot_copy;
    case FramePlanNodeKind::output_transform:
        return LogicalGraphNodeKind::output_transform;
    }
    throw std::runtime_error("unknown frame-plan node kind for logical shadow graph");
}

LogicalPortContract makePort(const LogicalTypeRegistry &types, std::string name,
                             LogicalPortDirection direction,
                             const LogicalType &type) {
    return LogicalPortContract{std::move(name), direction,
                               exactLogicalTypePattern(types, type), {}};
}

} // namespace

CompiledLogicalRenderGraph compileLogicalFrameGraphShadow(
    const FrameGraphDefinition &definition,
    const LogicalTypeRegistry &types,
    const LogicalFrameGraphShadowOptions &options) {
    if (definition.name.empty()) {
        throw std::runtime_error("logical shadow graph source name must not be empty");
    }

    std::set<std::string, std::less<>> declared;
    std::set<std::string, std::less<>> known_resources;
    std::set<std::string, std::less<>> regular_read_resources;
    std::vector<std::string> resource_order;
    const auto append_resource = [&](const std::string &resource) {
        if (resource.empty()) {
            throw std::runtime_error(
                "logical shadow graph resource name must not be empty");
        }
        if (known_resources.insert(resource).second) {
            resource_order.push_back(resource);
        }
    };
    for (const auto &resource : definition.declared_resources) {
        if (!declared.insert(resource).second) {
            throw std::runtime_error("logical shadow graph has duplicate resource: " +
                                     resource);
        }
        append_resource(resource);
        regular_read_resources.insert(resource);
    }
    std::set<std::string, std::less<>> history_resources;
    for (const auto &history : definition.history_resources) {
        if (!history_resources.insert(history).second) {
            throw std::runtime_error(
                "logical shadow graph has duplicate history resource: " + history);
        }
        append_resource(history);
    }
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.writes) {
            append_resource(resource);
            regular_read_resources.insert(resource);
        }
    }
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.reads) {
            if (resource == "swapchain") {
                append_resource(resource);
                regular_read_resources.insert(resource);
            }
            if (!resource.empty() &&
                !regular_read_resources.contains(resource)) {
                throw std::runtime_error("logical shadow graph node '" + node.name +
                                         "' reads unknown resource '" + resource + "'");
            }
        }
        for (const auto &resource : node.reads_history) {
            if (!history_resources.contains(resource)) {
                throw std::runtime_error("logical shadow graph node '" + node.name +
                                         "' reads unknown history resource '" +
                                         resource + "'");
            }
        }
    }

    std::map<std::string, const LogicalShadowResourceType *, std::less<>> overrides;
    for (const auto &override_type : options.resource_types) {
        if (!known_resources.contains(override_type.resource)) {
            throw std::runtime_error("logical shadow graph type override names unknown resource: " +
                                     override_type.resource);
        }
        types.requireCanonical(override_type.type);
        if (!overrides.emplace(override_type.resource, &override_type).second) {
            throw std::runtime_error("logical shadow graph has duplicate type override: " +
                                     override_type.resource);
        }
    }

    CompiledLogicalRenderGraph result;
    result.name = definition.name;
    result.resources.reserve(resource_order.size());
    std::map<std::string, LogicalType, std::less<>> resource_types;
    for (const auto &resource : resource_order) {
        const auto override_type = overrides.find(resource);
        const auto used_fallback = override_type == overrides.end();
        if (used_fallback && !options.allow_legacy_type_fallback) {
            throw std::runtime_error("logical shadow graph resource lacks an explicit type: " +
                                     resource);
        }
        auto type = used_fallback
                        ? legacyOpaqueResourceV1(types)
                        : override_type->second->type;
        auto materialization = resource == "swapchain"
                                   ? LogicalMaterializationRequirement::external
                                   : LogicalMaterializationRequirement::virtual_resource;
        if (!used_fallback && override_type->second->materialization) {
            materialization = *override_type->second->materialization;
        }
        if (resource == "swapchain" &&
            materialization != LogicalMaterializationRequirement::external) {
            throw std::runtime_error(
                "logical shadow graph swapchain materialization must be external");
        }
        if (history_resources.contains(resource) &&
            materialization != LogicalMaterializationRequirement::external) {
            materialization = LogicalMaterializationRequirement::required;
            result.decisions.push_back(LogicalCompileDecision{
                "history_materialization_required", resource,
                "legacy history resource must survive across frames"});
        }
        resource_types.emplace(resource, type);
        result.resources.push_back(
            LogicalResourceDesc{resource, std::move(type), materialization});
        if (used_fallback) {
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_type_fallback", resource,
                "source FrameGraphDefinition has no semantic resource type"});
        }
        if (resource == "swapchain") {
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_external_resource", resource,
                "implicit FrameGraphDefinition swapchain is modeled as external"});
        } else if (history_resources.contains(resource) &&
                   !declared.contains(resource)) {
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_history_resource_inferred", resource,
                "history read declares an implicit cross-frame resource"});
        } else if (!declared.contains(resource)) {
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_implicit_write_resource", resource,
                "FrameGraphDefinition permits resources introduced by a write"});
        }
    }

    std::map<std::string, std::uint32_t, std::less<>> current_versions;
    std::map<std::string, LogicalValueImportKind, std::less<>> imported_resources;
    const auto add_import = [&](const std::string &resource,
                                LogicalValueImportKind kind) {
        const auto [found, inserted] = imported_resources.emplace(resource, kind);
        if (!inserted && found->second != kind) {
            throw std::runtime_error("logical shadow graph resource '" + resource +
                                     "' has conflicting import kinds");
        }
        if (inserted) {
            result.imports.push_back(
                LogicalValueImport{LogicalValueId{resource, 0}, kind});
        }
    };
    for (const auto &resource : resource_order) {
        current_versions.emplace(resource, 0);
        if (history_resources.contains(resource)) {
            add_import(resource, LogicalValueImportKind::previous_epoch);
        } else if (resource == "swapchain") {
            add_import(resource, LogicalValueImportKind::external);
        } else if (declared.contains(resource)) {
            add_import(resource, LogicalValueImportKind::legacy_implicit);
        }
    }

    bool used_conservative_footprint = false;
    result.nodes.reserve(definition.nodes.size());
    for (const auto &source : definition.nodes) {
        LogicalGraphNode node;
        node.name = source.name;
        node.kind = logicalNodeKind(source.kind);
        node.declaration_index = source.declaration_index;
        node.after = source.after;
        node.before = source.before;
        node.region_tags = source.region_tags;
        const auto legacy_region =
            "legacy." +
            std::string{logicalGraphNodeKindName(node.kind)};
        if (std::find(
                node.region_tags.begin(),
                node.region_tags.end(),
                legacy_region) ==
            node.region_tags.end()) {
            node.region_tags.push_back(legacy_region);
        }
        if (source.kind == FramePlanNodeKind::snapshot_copy) {
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_snapshot_point", source.name, source.snapshot_after});
            result.decisions.push_back(LogicalCompileDecision{
                "legacy_physical_byte_size_omitted", source.name,
                std::to_string(source.byte_size) +
                    " bytes remain owned by the existing FramePlan"});
        }

        std::map<std::string, LogicalReadFootprint,
                 std::less<>>
            declared_footprints;
        for (const auto &declaration :
             source.read_footprints) {
            if (declaration.resource.empty() ||
                std::find(
                    source.reads.begin(),
                    source.reads.end(),
                    declaration.resource) ==
                    source.reads.end()) {
                throw std::runtime_error(
                    "logical shadow graph node '" +
                    source.name +
                    "' declares a footprint for a non-current "
                    "read resource: " +
                    declaration.resource);
            }
            if (declaration.footprint.kind ==
                    LogicalReadFootprintKind::none ||
                declaration.footprint.kind ==
                    LogicalReadFootprintKind::temporal) {
                throw std::runtime_error(
                    "logical shadow graph node '" +
                    source.name +
                    "' declares an invalid current-read "
                    "footprint for: " +
                    declaration.resource);
            }
            if (!declared_footprints
                     .emplace(declaration.resource,
                              declaration.footprint)
                     .second) {
                throw std::runtime_error(
                    "logical shadow graph node '" +
                    source.name +
                    "' declares duplicate read footprints for: " +
                    declaration.resource);
            }
        }
        std::set<std::string, std::less<>>
            loaded_attachment_resources;
        for (const auto &attachment :
             source.attachments) {
            if (attachment.load_op ==
                FrameGraphAttachmentLoadOp::load) {
                loaded_attachment_resources.insert(
                    attachment.resource);
            }
        }
        std::set<std::string, std::less<>>
            merged_attachment_resources;
        for (const auto &resource : source.reads) {
            const auto type = resource_types.find(resource);
            if (type == resource_types.end()) {
                throw std::runtime_error("logical shadow graph node '" + source.name +
                                         "' reads undeclared resource '" + resource + "'");
            }
            const auto attachment_read_write =
                loaded_attachment_resources.contains(
                    resource) &&
                std::find(
                    source.writes.begin(),
                    source.writes.end(),
                    resource) !=
                    source.writes.end();
            auto port =
                std::string{
                    attachment_read_write
                        ? "inout."
                        : "in."} +
                resource;
            node.ports.push_back(makePort(
                types, port,
                attachment_read_write
                    ? LogicalPortDirection::
                          input_output
                    : LogicalPortDirection::input,
                type->second));
            const auto version = current_versions.at(resource);
            if (version == 0 && !imported_resources.contains(resource)) {
                add_import(resource, LogicalValueImportKind::legacy_implicit);
                result.decisions.push_back(LogicalCompileDecision{
                    "legacy_unproduced_value_import", resource,
                    "legacy declaration order reads the resource before its first write"});
            }
            const auto declared =
                declared_footprints.find(resource);
            const auto footprint =
                declared != declared_footprints.end()
                    ? declared->second
                : loaded_attachment_resources.contains(
                      resource)
                    ? LogicalReadFootprint{
                          LogicalReadFootprintKind::
                              same_pixel,
                          std::nullopt}
                    :
                    LogicalReadFootprint{
                        LogicalReadFootprintKind::arbitrary,
                        std::nullopt};
            if (attachment_read_write) {
                auto &output_version =
                    current_versions.at(resource);
                if (output_version ==
                    std::numeric_limits<
                        std::uint32_t>::max()) {
                    throw std::runtime_error(
                        "logical shadow graph resource "
                        "version overflow: " +
                        resource);
                }
                ++output_version;
                node.uses.push_back(
                    makeLogicalReadWriteUse(
                        std::move(port),
                        LogicalValueId{
                            resource, version},
                        LogicalValueId{
                            resource,
                            output_version},
                        footprint,
                        LogicalAccessIntent::
                            attachment));
                merged_attachment_resources
                    .insert(resource);
            } else {
                node.uses.push_back(
                    makeLogicalReadUse(
                        std::move(port),
                        LogicalValueId{
                            resource, version},
                        footprint));
            }
            used_conservative_footprint =
                used_conservative_footprint ||
                (declared ==
                     declared_footprints.end() &&
                 !loaded_attachment_resources
                      .contains(resource));
        }
        for (const auto &resource : source.reads_history) {
            const auto type = resource_types.find(resource);
            if (type == resource_types.end()) {
                throw std::runtime_error("logical shadow graph node '" + source.name +
                                         "' reads undeclared history resource '" +
                                         resource + "'");
            }
            auto port = "history." + resource;
            node.ports.push_back(makePort(types, port, LogicalPortDirection::input,
                                          type->second));
            node.uses.push_back(makeLogicalReadUse(
                std::move(port), LogicalValueId{resource, 0},
                LogicalReadFootprint{LogicalReadFootprintKind::temporal,
                                     std::nullopt}));
        }
        for (const auto &resource : source.writes) {
            if (merged_attachment_resources.contains(
                    resource)) {
                continue;
            }
            const auto type = resource_types.find(resource);
            if (type == resource_types.end()) {
                throw std::runtime_error("logical shadow graph node '" + source.name +
                                         "' writes undeclared resource '" + resource + "'");
            }
            auto port = "out." + resource;
            node.ports.push_back(makePort(types, port, LogicalPortDirection::output,
                                          type->second));
            auto &version = current_versions.at(resource);
            if (version == std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("logical shadow graph resource version overflow: " +
                                         resource);
            }
            ++version;
            node.uses.push_back(makeLogicalWriteUse(
                std::move(port), LogicalValueId{resource, version}));
        }
        result.nodes.push_back(std::move(node));
    }
    if (used_conservative_footprint) {
        result.decisions.push_back(LogicalCompileDecision{
            "legacy_read_footprint_conservative", result.name,
            "untyped FrameGraphDefinition reads lower to arbitrary footprint"});
    }
    result.decisions.push_back(LogicalCompileDecision{
        "shadow_graph_only", result.name,
        "logical graph is diagnostic-only and does not own runtime execution"});
    result.graph_transforms =
        definition.graph_transforms;
    result.subgraph_replacements =
        definition.subgraph_replacements;
    result.render_strategy =
        definition.render_strategy;

    validateCompiledLogicalRenderGraph(types, result);
    return result;
}

} // namespace Pelican
