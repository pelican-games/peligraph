#include "rendergraphir.hpp"

#include "graphcomposition.hpp"
#include "logicalrendertype.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

namespace {

[[noreturn]] void refuse(std::string_view code, nlohmann::json detail) {
    throw GraphRegistryError(std::string{code}, std::move(detail));
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

int footprintRank(LogicalReadFootprintKind kind) {
    switch (kind) {
    case LogicalReadFootprintKind::none: return 0;
    case LogicalReadFootprintKind::same_pixel: return 1;
    case LogicalReadFootprintKind::neighborhood: return 2;
    case LogicalReadFootprintKind::arbitrary: return 3;
    case LogicalReadFootprintKind::temporal: return 4;
    }
    throw std::runtime_error("Unknown frame graph read footprint kind");
}

// One footprint per resource: the widest of the node's reads of it (a
// neighborhood keeps the larger radius, or none when one has none).
void appendReadFootprint(FrameGraphNodeDefinition &node, const std::string &resource,
                         const LogicalReadFootprint &footprint) {
    const auto found = std::find_if(node.read_footprints.begin(), node.read_footprints.end(),
                                    [&](const auto &entry) { return entry.resource == resource; });
    if (found == node.read_footprints.end()) {
        node.read_footprints.push_back({resource, footprint});
        return;
    }
    const auto existing_rank = footprintRank(found->footprint.kind);
    const auto incoming_rank = footprintRank(footprint.kind);
    if (incoming_rank > existing_rank) {
        found->footprint = footprint;
        return;
    }
    if (incoming_rank == existing_rank &&
        footprint.kind == LogicalReadFootprintKind::neighborhood) {
        if (!found->footprint.radius || !footprint.radius) {
            found->footprint.radius = std::nullopt;
        } else {
            found->footprint.radius = std::max(*found->footprint.radius, *footprint.radius);
        }
    }
}

bool hasRasterQueryPolicy(RenderNodeShape shape) {
    switch (shape) {
    case RenderNodeShape::material:
    case RenderNodeShape::fullscreen:
    case RenderNodeShape::raster:
    case RenderNodeShape::debug_draw:
    case RenderNodeShape::debug_text:
    case RenderNodeShape::gizmo:
    case RenderNodeShape::shadow_depth:
    case RenderNodeShape::velocity:
    case RenderNodeShape::picking:
    case RenderNodeShape::output_transform:
        return true;
    default:
        return false;
    }
}

// The planner's ray-query policy of a node: true / false for a pass of a
// raster family (ui and imgui never query), nullopt for any other node.
std::optional<bool> rayQueryPolicy(const NodeIR &node) {
    if (hasRasterQueryPolicy(node.shape)) return node.ray_query.has_value();
    if (node.shape == RenderNodeShape::ui || node.shape == RenderNodeShape::imgui) return false;
    return std::nullopt;
}

FrameGraphNodeDefinition nodeDefinitionOf(const NodeIR &node) {
    FrameGraphNodeDefinition result;
    result.name = node.name;
    result.kind = framePlanNodeKindOfShape(node.shape);
    result.declaration_index = node.declaration_index;
    for (const auto &use : node.uses) {
        const auto local_read = isLocalReadShaderInput(node, use);
        switch (use.role) {
        case ResourceUseRole::input:
        case ResourceUseRole::indirect:
            if (use.history) {
                appendUnique(result.reads_history, use.resource);
                break;
            }
            appendUnique(result.reads, use.resource);
            if (use.footprint) appendReadFootprint(result, use.resource, *use.footprint);
            if (local_read) appendUnique(result.local_read_shader_inputs, use.resource);
            break;
        case ResourceUseRole::resource_port: {
            const auto key = use.history ? use.resource + "@history" : use.resource;
            const auto existing =
                std::find_if(result.resource_accesses.begin(), result.resource_accesses.end(),
                             [&](const auto &candidate) { return candidate.resource == key; });
            if (existing == result.resource_accesses.end()) {
                result.resource_accesses.push_back({key, use.intent});
            } else if (existing->intent != use.intent) {
                // Disjoint sampled and storage subresources of one resource:
                // the logical use keeps the conservative base intent.
                existing->intent = LogicalAccessIntent::storage;
            }
            break;
        }
        case ResourceUseRole::screen_input:
        case ResourceUseRole::surface_resource:
            appendUnique(result.reads, use.resource);
            if (local_read) appendUnique(result.local_read_shader_inputs, use.resource);
            if (use.footprint) appendReadFootprint(result, use.resource, *use.footprint);
            break;
        case ResourceUseRole::material_resource:
            if (use.history) {
                appendUnique(result.reads_history, use.resource);
            } else {
                appendUnique(result.reads, use.resource);
                if (local_read) appendUnique(result.local_read_shader_inputs, use.resource);
                if (use.footprint) appendReadFootprint(result, use.resource, *use.footprint);
            }
            result.resource_accesses.push_back(
                {use.history ? use.resource + "@history" : use.resource, use.intent});
            break;
        case ResourceUseRole::copy_source:
            result.reads.push_back(use.resource);
            break;
        case ResourceUseRole::copy_destination:
        case ResourceUseRole::output:
            result.writes.push_back(use.resource);
            break;
        case ResourceUseRole::color_attachment:
        case ResourceUseRole::depth_attachment: {
            appendUnique(result.writes, use.resource);
            const auto operations = use.operations.value();
            if (!use.resource.empty()) {
                result.attachments.push_back(FrameGraphAttachmentDefinition{
                    .resource = use.resource,
                    .subresource = use.subresource,
                    .aspect = use.role == ResourceUseRole::color_attachment
                                  ? AttachmentAspect::color
                                  : AttachmentAspect::depth,
                    .operations = operations,
                });
            }
            if (operations.load_op == AttachmentLoadOp::load) {
                appendUnique(result.reads, use.resource);
                // The parser states it (§2.3: same_pixel); nothing re-derives
                // it here.
                if (use.footprint) appendReadFootprint(result, use.resource, *use.footprint);
            }
            break;
        }
        }
    }
    result.after = node.after;
    result.before = node.before;
    result.region_tags = node.regions;
    if (const auto *snapshot = std::get_if<SnapshotNodeInfo>(&node.info)) {
        result.snapshot_after = snapshot->snapshot_after;
        result.byte_size = snapshot->byte_size;
    }
    result.raster_geometry =
        node.shape == RenderNodeShape::material || node.shape == RenderNodeShape::shadow_depth ||
        node.shape == RenderNodeShape::velocity || node.shape == RenderNodeShape::picking ||
        node.shape == RenderNodeShape::raster;
    result.ray_query = rayQueryPolicy(node);
    result.ray_query_acceleration_structure = node.ray_query.value_or("scene");
    result.resolution_domain = node.resolution_domain;
    if (const auto *material = std::get_if<MaterialNodeInfo>(&node.info)) {
        result.material_filter = material->filter;
        result.material_variant = material->variant;
    }
    result.view_family = node.view_family;
    result.view_select = node.view_select;
    result.once = node.schedule == NodeSchedule::once;
    return result;
}

const ImageResource *findConfigImage(const RenderGraphIR &ir, std::string_view name) {
    const auto found = std::find_if(ir.images.begin(), ir.images.end(),
                                    [&](const auto &image) { return image.name == name; });
    return found == ir.images.end() ? nullptr : &*found;
}

bool nodeAttachmentsMatchExtent(const FrameGraphNodeDefinition &node, const ImageResource &local,
                                const RenderGraphIR &ir) {
    if (node.attachments.empty()) return false;
    for (const auto &attachment : node.attachments) {
        if (attachment.resource == "swapchain") return false;
        const auto *target = findConfigImage(ir, attachment.resource);
        if (target == nullptr || target->extent != local.extent) return false;
    }
    return true;
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

ImageDeclarationFacts imageDeclarationFactsOf(const ImageResource &image) {
    return ImageDeclarationFacts{
        .history = image.history,
        .persistent = image.persistent,
        .has_source = image.source.has_value(),
        .usage = image.usage,
        .format_class = image.format_class,
    };
}

bool isTransientAttachmentCandidate(const ImageDeclarationFacts &image) {
    const auto attachment_usage =
        ImageUsageBit::color_attachment | ImageUsageBit::depth_stencil_attachment;
    return !image.history && !image.persistent && bool(image.usage & attachment_usage) &&
           !bool(image.usage & ~ImageUsage{attachment_usage});
}

bool isTileLocalAttachmentCandidate(const ImageDeclarationFacts &image) {
    const auto attachment_usage =
        ImageUsageBit::color_attachment | ImageUsageBit::depth_stencil_attachment;
    const auto allowed_usage =
        attachment_usage | ImageUsageBit::sampled | ImageUsageBit::input_attachment;
    return !image.history && !image.persistent && bool(image.usage & attachment_usage) &&
           !bool(image.usage & ~ImageUsage{allowed_usage});
}

bool isRuntimeAliasEligible(const ImageDeclarationFacts &image) {
    return !image.history && !image.has_source && !image.persistent &&
           image.format_class != "display" &&
           image.usage == (ImageUsageBit::color_attachment | ImageUsageBit::sampled);
}

bool nodeHasSamePixelRead(const FrameGraphNodeDefinition &node, std::string_view resource) {
    const auto footprint =
        std::find_if(node.read_footprints.begin(), node.read_footprints.end(),
                     [&](const auto &candidate) { return candidate.resource == resource; });
    return footprint != node.read_footprints.end() &&
           footprint->footprint.kind == LogicalReadFootprintKind::same_pixel;
}

std::string_view renderNodeShapeName(RenderNodeShape shape) {
    switch (shape) {
    case RenderNodeShape::material: return "material";
    case RenderNodeShape::fullscreen: return "fullscreen";
    case RenderNodeShape::raster: return "raster";
    case RenderNodeShape::output_transform: return "output_transform";
    case RenderNodeShape::debug_draw: return "debug_draw";
    case RenderNodeShape::gizmo: return "gizmo";
    case RenderNodeShape::debug_text: return "debug_text";
    case RenderNodeShape::shadow_depth: return "shadow_depth";
    case RenderNodeShape::velocity: return "velocity";
    case RenderNodeShape::picking: return "picking";
    case RenderNodeShape::ui: return "ui";
    case RenderNodeShape::imgui: return "imgui";
    case RenderNodeShape::anchor: return "anchor";
    case RenderNodeShape::snapshot_copy: return "snapshot_copy";
    case RenderNodeShape::compute: return "compute";
    case RenderNodeShape::acceleration_structure: return "acceleration_structure";
    }
    throw std::runtime_error("unknown render node shape");
}

RenderNodeShape renderNodeShapeOfPassType(RenderPassType type) {
    switch (type) {
    case RenderPassType::material: return RenderNodeShape::material;
    case RenderPassType::fullscreen: return RenderNodeShape::fullscreen;
    case RenderPassType::raster: return RenderNodeShape::raster;
    case RenderPassType::output_transform: return RenderNodeShape::output_transform;
    case RenderPassType::debug_draw: return RenderNodeShape::debug_draw;
    case RenderPassType::gizmo: return RenderNodeShape::gizmo;
    case RenderPassType::debug_text: return RenderNodeShape::debug_text;
    case RenderPassType::shadow_depth: return RenderNodeShape::shadow_depth;
    case RenderPassType::velocity: return RenderNodeShape::velocity;
    case RenderPassType::picking: return RenderNodeShape::picking;
    case RenderPassType::ui: return RenderNodeShape::ui;
    case RenderPassType::imgui: return RenderNodeShape::imgui;
    case RenderPassType::canonical_anchor: return RenderNodeShape::anchor;
    case RenderPassType::snapshot_copy: return RenderNodeShape::snapshot_copy;
    }
    throw std::runtime_error("unknown render pass type");
}

FramePlanNodeKind framePlanNodeKindOfShape(RenderNodeShape shape) {
    switch (shape) {
    case RenderNodeShape::anchor: return FramePlanNodeKind::anchor;
    case RenderNodeShape::snapshot_copy: return FramePlanNodeKind::snapshot_copy;
    case RenderNodeShape::output_transform: return FramePlanNodeKind::output_transform;
    case RenderNodeShape::compute: return FramePlanNodeKind::compute;
    case RenderNodeShape::acceleration_structure: return FramePlanNodeKind::acceleration_structure;
    default: return FramePlanNodeKind::render;
    }
}

std::string_view resourceUseRoleName(ResourceUseRole role) {
    switch (role) {
    case ResourceUseRole::input: return "input";
    case ResourceUseRole::output: return "output";
    case ResourceUseRole::resource_port: return "resource_port";
    case ResourceUseRole::screen_input: return "screen_input";
    case ResourceUseRole::surface_resource: return "surface_resource";
    case ResourceUseRole::material_resource: return "material_resource";
    case ResourceUseRole::color_attachment: return "color_attachment";
    case ResourceUseRole::depth_attachment: return "depth_attachment";
    case ResourceUseRole::indirect: return "indirect";
    case ResourceUseRole::copy_source: return "copy_source";
    case ResourceUseRole::copy_destination: return "copy_destination";
    }
    throw std::runtime_error("unknown resource use role");
}

const GraphIR *findGraph(const RenderGraphIR &ir, std::string_view name) {
    const auto found = std::find_if(ir.graphs.begin(), ir.graphs.end(),
                                    [&](const auto &graph) { return graph.name == name; });
    return found == ir.graphs.end() ? nullptr : &*found;
}

const ImageResource *findImage(const RenderGraphIR &ir, const GraphIR &graph,
                               std::string_view name) {
    const auto local = std::find_if(graph.images.begin(), graph.images.end(),
                                    [&](const auto &image) { return image.name == name; });
    if (local != graph.images.end()) return &*local;
    return findConfigImage(ir, name);
}

bool isLocalReadShaderInput(const NodeIR &node, const ResourceUse &use) {
    if (use.history) return false;
    switch (use.role) {
    case ResourceUseRole::input:
        return node.shape == RenderNodeShape::fullscreen || node.shape == RenderNodeShape::raster;
    case ResourceUseRole::screen_input:
        return true;
    case ResourceUseRole::material_resource:
        return use.binding.access != ShaderResourcePortAccess::storage &&
               use.binding.kind != ShaderResourcePortKind::buffer;
    default:
        return false;
    }
}

FrameGraphDefinition frameGraphDefinitionOf(const RenderGraphIR &ir, const GraphIR &graph) {
    FrameGraphDefinition result;
    result.name = graph.name;
    for (const auto &image : graph.images) appendUnique(result.declared_resources, image.name);
    for (const auto &image : graph.images) {
        if (image.history) appendUnique(result.history_resources, image.name);
    }
    for (const auto &buffer : graph.buffers) appendUnique(result.declared_resources, buffer.name);
    for (const auto &image : ir.images) appendUnique(result.declared_resources, image.name);
    for (const auto &image : ir.images) {
        if (image.history) appendUnique(result.history_resources, image.name);
    }
    for (const auto &buffer : ir.buffers) appendUnique(result.declared_resources, buffer.name);
    result.nodes.reserve(graph.nodes.size());
    for (const auto &node : graph.nodes) result.nodes.push_back(nodeDefinitionOf(node));
    result.graph_transforms = graph.graph_transforms;
    result.subgraph_replacements = graph.subgraph_replacements;
    result.render_strategy = graph.render_strategy;
    return result;
}

FramePlan planRenderGraph(const RenderGraphIR &ir, const GraphIR &graph) {
    return planFrameGraph(frameGraphDefinitionOf(ir, graph));
}

void requireRenderGraphInputsProduced(const RenderGraphIR &ir, const GraphIR &graph,
                                      const FramePlan &plan) {
    // A config buffer, and a config image holding an asset (IA-1 `initial`)
    // or a file's pixels (EN-2 `source`), hold their contents before any pass
    // of the frame runs.
    std::unordered_set<std::string> held;
    for (const auto &buffer : ir.buffers) held.insert(buffer.name);
    for (const auto &image : ir.images) {
        if (image.initial || image.source) held.insert(image.name);
    }
    // Only a pass's current `input` and a snapshot copy's `source`.
    std::unordered_map<std::string, std::vector<std::string>> required;
    for (const auto &node : graph.nodes) {
        if (node.scope != NodeDeclarationScope::graph || node.shape == RenderNodeShape::compute) {
            continue;
        }
        auto &inputs = required[node.name];
        for (const auto &use : node.uses) {
            const bool input = node.shape == RenderNodeShape::snapshot_copy
                                   ? use.role == ResourceUseRole::copy_source
                                   : use.role == ResourceUseRole::input && !use.history;
            if (input) inputs.push_back(use.resource);
        }
    }
    std::unordered_set<std::string> produced;
    for (const auto &node : plan.nodes) {
        if (const auto inputs = required.find(node.name); inputs != required.end()) {
            for (const auto &resource : inputs->second) {
                if (held.contains(resource) || produced.contains(resource)) continue;
                refuse("render_graph.input_not_produced@1",
                       {{"graph", graph.name}, {"node", node.name}, {"resource", resource}});
            }
        }
        produced.insert(node.writes.begin(), node.writes.end());
    }
}

RenderGraphTileLocalFormats renderGraphDeviceTileLocalFormats(
    const RenderGraphIR &ir, const RenderGraphDeviceFacts &device) {
    RenderGraphTileLocalFormats result;
    if (!device.tile_local_attachments) return result;
    for (const auto &image : ir.images) {
        if (!isTileLocalAttachmentCandidate(imageDeclarationFactsOf(image))) continue;
        // The device is asked about the image's format.
        if (image.format_unresolved) {
            refuse("render_graph.image_format_unresolved@1", {{"resource", image.name}});
        }
        std::vector<ImageFormat> formats{image.format};
        for (const auto format : image.format_candidates) {
            if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
                formats.push_back(format);
            }
        }
        for (const auto format : formats) {
            if (device.supports_local_read_attachment &&
                device.supports_local_read_attachment(RenderGraphImageCapabilityQuery{
                    .format = format,
                    .usage = image.usage,
                    .cube_compatible = image.dimension == ImageResourceDimension::cube,
                })) {
                result.emplace(image.name, format);
            }
        }
    }
    return result;
}

RenderGraphTileLocalFormats renderGraphTileLocalFormats(
    const RenderGraphIR &ir, const GraphIR &graph,
    const RenderGraphTileLocalFormats &device_formats) {
    const auto definition = frameGraphDefinitionOf(ir, graph);
    RenderGraphTileLocalFormats result;
    for (const auto &image : ir.images) {
        if (!device_formats.contains({image.name, image.format})) continue;
        bool read = false;
        bool written = false;
        bool compatible = true;
        for (const auto &node : definition.nodes) {
            if (contains(node.reads_history, image.name)) {
                compatible = false;
                break;
            }
            if (contains(node.reads, image.name)) {
                read = true;
                if (node.kind != FramePlanNodeKind::render ||
                    !contains(node.local_read_shader_inputs, image.name) ||
                    !nodeHasSamePixelRead(node, image.name) ||
                    !nodeAttachmentsMatchExtent(node, image, ir)) {
                    compatible = false;
                    break;
                }
            }
            if (contains(node.writes, image.name)) {
                written = true;
                const auto attachment =
                    std::find_if(node.attachments.begin(), node.attachments.end(),
                                 [&](const auto &entry) { return entry.resource == image.name; });
                // A local-read port names an image, not one of its raster
                // subresources: an explicit attachment view stays materialized.
                if (attachment == node.attachments.end() || attachment->subresource) {
                    compatible = false;
                    break;
                }
            }
        }
        if (compatible && read && written) result.emplace(image.name, image.format);
    }
    return result;
}

LogicalFrameGraphShadowOptions renderGraphShadowOptions(
    const RenderGraphIR &ir, const FrameGraphDefinition &definition,
    const LogicalTypeRegistry &types,
    const RenderGraphTileLocalFormats &tile_local_formats) {
    std::set<std::string, std::less<>> known(definition.declared_resources.begin(),
                                             definition.declared_resources.end());
    known.insert(definition.history_resources.begin(), definition.history_resources.end());
    for (const auto &node : definition.nodes) {
        known.insert(node.reads.begin(), node.reads.end());
        known.insert(node.reads_history.begin(), node.reads_history.end());
        known.insert(node.writes.begin(), node.writes.end());
    }
    known.erase(std::string{});
    LogicalFrameGraphShadowOptions result;
    for (const auto &resource : known) {
        if (resource == "swapchain") {
            result.resource_types.push_back({
                .resource = resource,
                .type = displayEncodedV1(types),
                .materialization = LogicalMaterializationRequirement::external,
            });
            continue;
        }
        // Only the config's images are registered (and typed); a graph's own
        // declaration and a buffer keep the logical graph's fallback type.
        const auto *image = findConfigImage(ir, resource);
        if (image == nullptr) continue;
        const bool tile_local = tile_local_formats.contains({resource, image->format});
        result.resource_types.push_back({
            .resource = resource,
            .type = bool(image->usage & ImageUsageBit::depth_stencil_attachment)
                        ? deviceDepthV1(types)
                        : legacyOpaqueImageV1(types),
            .materialization = isTransientAttachmentCandidate(imageDeclarationFactsOf(*image)) ||
                                       tile_local ||
                                       isRuntimeAliasEligible(imageDeclarationFactsOf(*image))
                                   ? LogicalMaterializationRequirement::virtual_resource
                                   : LogicalMaterializationRequirement::required,
        });
    }
    return result;
}

CompiledLogicalRenderGraph toLogicalGraph(const RenderGraphIR &ir, const GraphIR &graph,
                                          const ToLogicalGraphOptions &options) {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto definition = frameGraphDefinitionOf(ir, graph);
    // The planner's refusals (by name) come before the logical graph's own.
    (void)planFrameGraph(definition);
    const auto tile_local = renderGraphTileLocalFormats(
        ir, graph, renderGraphDeviceTileLocalFormats(ir, options.device));
    return compileLogicalFrameGraphShadow(
        definition, types, renderGraphShadowOptions(ir, definition, types, tile_local));
}

RenderGraphIR namespaceSharedComputeTasks(RenderGraphIR ir, std::string_view suffix) {
    if (suffix.empty() || ir.shared_compute_tasks.empty()) return ir;
    std::unordered_map<std::string, std::string> renamed;
    for (const auto &task : ir.shared_compute_tasks) {
        if (!renamed.emplace(task, task + std::string{suffix}).second) {
            refuse("render_graph.compute_task_duplicate@1", {{"task", task}});
        }
    }
    for (const auto &graph : ir.graphs) {
        std::unordered_set<std::string> final_names;
        for (const auto &node : graph.nodes) {
            auto final_name = node.name;
            if (node.shape == RenderNodeShape::compute) {
                if (const auto found = renamed.find(node.name); found != renamed.end()) {
                    final_name = found->second;
                }
            }
            if (!final_names.insert(final_name).second) {
                refuse("render_graph.compute_task_name_collision@1",
                       {{"graph", graph.name}, {"node", final_name}});
            }
        }
    }
    const auto rename = [&](std::string &name) {
        if (const auto found = renamed.find(name); found != renamed.end()) name = found->second;
    };
    for (auto &task : ir.shared_compute_tasks) rename(task);
    for (auto &graph : ir.graphs) {
        for (auto &node : graph.nodes) {
            if (node.shape == RenderNodeShape::compute) rename(node.name);
            for (auto &after : node.after) rename(after);
            for (auto &before : node.before) rename(before);
        }
    }
    return ir;
}

void requireRenderGraphRayQueryCapability(
    const RenderGraphIR &ir, std::span<const SceneAccelerationStructureDefinition> named,
    TargetPlanningPolicy &target_planning) {
    const bool raster_query = std::any_of(ir.graphs.begin(), ir.graphs.end(), [](const auto &graph) {
        return std::any_of(graph.nodes.begin(), graph.nodes.end(),
                           [](const auto &node) { return rayQueryPolicy(node) == true; });
    });
    // The config's tasks (a copy in every graph).
    const bool task_query = std::any_of(ir.graphs.begin(), ir.graphs.end(), [](const auto &graph) {
        return std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const NodeIR &node) {
            const auto *task = std::get_if<ComputeNodeInfo>(&node.info);
            return node.scope == NodeDeclarationScope::shared && task != nullptr &&
                   task->ray_query_acceleration_structure.has_value();
        });
    });
    if (!raster_query && named.empty() && !task_query) return;
    const std::string capability = "pelican.vulkan.ray_query@1";
    for (const auto &graph : ir.graphs) {
        auto &constraints = target_planning.graphs;
        auto found = std::find_if(constraints.begin(), constraints.end(),
                                  [&](const auto &entry) { return entry.graph == graph.name; });
        if (found == constraints.end()) {
            constraints.push_back(PlanningGraphConstraints{.graph = graph.name});
            found = std::prev(constraints.end());
        }
        auto &required = found->required_capabilities;
        if (std::find(required.begin(), required.end(), capability) == required.end()) {
            required.push_back(capability);
            std::sort(required.begin(), required.end());
        }
    }
}

RenderGraphIR insertAccelerationStructureNodes(RenderGraphIR ir,
                                               const AccelerationStructureNodeRequest &request) {
    const std::string node_name = "__scene_acceleration_structure";
    const auto named_exists = [&](const std::string &name) {
        return std::any_of(request.named.begin(), request.named.end(),
                           [&](const auto &definition) { return definition.name == name; });
    };
    const auto ray_task = [](const NodeIR &node) -> const ComputeNodeInfo * {
        if (node.shape != RenderNodeShape::compute || node.scope != NodeDeclarationScope::shared) {
            return nullptr;
        }
        return std::get_if<ComputeNodeInfo>(&node.info);
    };
    // A config task's named structure must exist (the compiler's check,
    // before any graph needs the builds).
    for (const auto &graph : ir.graphs) {
        for (const auto &node : graph.nodes) {
            const auto *task = ray_task(node);
            if (task == nullptr) continue;
            const auto selected = task->ray_tracing
                                      ? task->ray_tracing_acceleration_structure
                                      : task->ray_query_acceleration_structure.value_or("scene");
            if (selected != "scene" && !named_exists(selected)) {
                refuse("render_graph.acceleration_structure_unknown@1",
                       {{"node", node.name}, {"acceleration_structure", selected}});
            }
        }
    }
    const bool requires_as =
        request.ray_tracing_project ||
        std::any_of(ir.graphs.begin(), ir.graphs.end(), [&](const GraphIR &graph) {
            const bool required_capability = std::any_of(
                request.planning_graphs.begin(), request.planning_graphs.end(),
                [&](const auto &constraints) {
                    return constraints.graph == graph.name &&
                           std::any_of(constraints.required_capabilities.begin(),
                                       constraints.required_capabilities.end(),
                                       [](const auto &capability) {
                                           return capability == "pelican.vulkan.ray_query@1" ||
                                                  capability ==
                                                      "pelican.vulkan.ray_tracing_pipeline@1";
                                       });
                });
            const bool has_ray_task =
                std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const auto &node) {
                    const auto *task = ray_task(node);
                    return task != nullptr &&
                           (task->ray_tracing || task->ray_query_acceleration_structure);
                });
            const bool raster_query =
                std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const auto &node) {
                    return rayQueryPolicy(node) == true;
                });
            return required_capability || has_ray_task || raster_query;
        });
    if (!requires_as) return ir;
    std::vector<std::string> as_nodes{node_name};
    for (const auto &definition : request.named) as_nodes.push_back(node_name + ":" + definition.name);
    for (auto &graph : ir.graphs) {
        for (const auto &node : graph.nodes) {
            if (contains(as_nodes, node.name)) {
                refuse("render_graph.acceleration_structure_reserved@1",
                       {{"graph", graph.name}, {"node", node.name}});
            }
        }
        for (auto &node : graph.nodes) {
            const auto kind = framePlanNodeKindOfShape(node.shape);
            const auto policy = rayQueryPolicy(node);
            if ((kind == FramePlanNodeKind::render || kind == FramePlanNodeKind::output_transform) &&
                policy.value_or(true)) {
                node.after.push_back(node_name);
            }
            const auto selected_raster = node.ray_query.value_or("scene");
            if (policy == true && selected_raster != "scene") {
                if (!named_exists(selected_raster)) {
                    refuse("render_graph.acceleration_structure_unknown@1",
                           {{"node", node.name}, {"acceleration_structure", selected_raster}});
                }
                node.after.push_back(node_name + ":" + selected_raster);
            }
            const auto *task = ray_task(node);
            if (task == nullptr || (!task->ray_tracing && !task->ray_query_acceleration_structure)) {
                continue;
            }
            node.after.push_back(node_name);
            const auto selected = task->ray_tracing
                                      ? task->ray_tracing_acceleration_structure
                                      : task->ray_query_acceleration_structure.value_or("scene");
            if (selected != "scene") node.after.push_back(node_name + ":" + selected);
        }
        std::vector<NodeIR> operations;
        for (const auto &name : as_nodes) {
            NodeIR operation;
            operation.name = name;
            operation.shape = RenderNodeShape::acceleration_structure;
            operation.scope = NodeDeclarationScope::synthesized;
            operation.info = AccelerationStructureNodeInfo{
                name == node_name ? std::nullopt
                                  : std::optional<std::string>{name.substr(node_name.size() + 1)}};
            if (name != node_name) operation.after.push_back(node_name);
            operations.push_back(std::move(operation));
        }
        graph.nodes.insert(graph.nodes.begin(), operations.begin(), operations.end());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            graph.nodes[index].declaration_index = index;
        }
    }
    return ir;
}

RenderGraphIR applyFrameNodeOrder(RenderGraphIR ir, bool require_all_graphs) {
    for (const auto &order : ir.frame_node_order) {
        const auto graph = std::find_if(ir.graphs.begin(), ir.graphs.end(),
                                        [&](const auto &entry) { return entry.name == order.graph; });
        if (graph == ir.graphs.end()) {
            if (require_all_graphs) {
                refuse("render_graph.frame_node_order_unknown@1", {{"graph", order.graph}});
            }
            continue;
        }
        std::vector<std::size_t> indices;
        for (const auto &name : order.nodes) {
            const auto node = std::find_if(graph->nodes.begin(), graph->nodes.end(),
                                           [&](const auto &entry) { return entry.name == name; });
            if (node == graph->nodes.end()) {
                refuse("render_graph.frame_node_order_unknown@1",
                       {{"graph", order.graph}, {"node", name}});
            }
            indices.push_back(static_cast<std::size_t>(node - graph->nodes.begin()));
        }
        requireFrameNodeOrderAgreesWithDependencies(frameGraphDefinitionOf(ir, *graph), indices);
        for (std::size_t index = 1; index < indices.size(); ++index) {
            auto &node = graph->nodes[indices[index]];
            const auto &previous = graph->nodes[indices[index - 1]].name;
            if (!contains(node.after, previous)) node.after.push_back(previous);
        }
    }
    return ir;
}

namespace {

// `ray_task_acceleration_structures` against the config's compute tasks:
// each named task exists and traces or queries, and takes the named
// structure (every graph's copy of it).
RenderGraphIR resolveRayTaskAccelerationStructures(RenderGraphIR ir) {
    for (const auto &[task, selected] : ir.ray_task_acceleration_structures) {
        const auto invalid = [&](std::string_view reason) {
            refuse("render_graph.compute_task_invalid@1",
                   {{"node", task},
                    {"field", "ray_task_acceleration_structures"},
                    {"reason", std::string{reason}}});
        };
        if (!contains(ir.shared_compute_tasks, task)) invalid("unknown_task");
        for (auto &graph : ir.graphs) {
            for (auto &node : graph.nodes) {
                if (node.scope != NodeDeclarationScope::shared || node.name != task) continue;
                auto &info = std::get<ComputeNodeInfo>(node.info);
                if (info.ray_tracing) {
                    info.ray_tracing_acceleration_structure = selected;
                } else if (info.ray_query_acceleration_structure) {
                    info.ray_query_acceleration_structure = selected;
                } else {
                    invalid("task_declares_no_ray_work");
                }
            }
        }
    }
    return ir;
}

} // namespace

RenderGraphIR finalizeRenderGraph(RenderGraphIR ir, const FinalizeRenderGraphRequest &request,
                                  TargetPlanningPolicy &target_planning) {
    for (const auto &image : ir.images) {
        if (image.format_unresolved) {
            refuse("render_graph.image_format_unresolved@1", {{"resource", image.name}});
        }
    }
    ir = resolveRayTaskAccelerationStructures(std::move(ir));
    requireRenderGraphRayQueryCapability(ir, request.named, target_planning);
    ir = namespaceSharedComputeTasks(std::move(ir), request.compute_task_suffix);
    ir = insertAccelerationStructureNodes(
        std::move(ir), AccelerationStructureNodeRequest{
                           .planning_graphs = target_planning.graphs,
                           .ray_tracing_project = request.ray_tracing_project,
                           .named = request.named,
                       });
    return applyFrameNodeOrder(std::move(ir), request.require_all_graphs);
}

std::set<std::string, std::less<>> renderGraphMultiviewCapableNodes(
    const RenderGraphIR &ir, bool enable_multiview_runtime) {
    static constexpr std::array builtin_fragments{
        std::string_view{"engine://fullscreen"},  std::string_view{"engine://ssao"},
        std::string_view{"engine://ssao_blur"},   std::string_view{"engine://scene_present"},
        std::string_view{"engine://output_transform"},
    };
    std::set<std::string, std::less<>> result;
    if (!enable_multiview_runtime) return result;
    for (const auto &graph : ir.graphs) {
        for (const auto &node : graph.nodes) {
            if (node.shape != RenderNodeShape::fullscreen) continue;
            const auto *info = std::get_if<FullscreenNodeInfo>(&node.info);
            if (info == nullptr || info->implementation_provider || !info->vertex_shader ||
                !info->fragment_shader) {
                continue;
            }
            if (*info->vertex_shader == "engine://fullscreen" &&
                std::find(builtin_fragments.begin(), builtin_fragments.end(),
                          *info->fragment_shader) != builtin_fragments.end()) {
                result.insert(node.name);
            }
        }
    }
    return result;
}

} // namespace Pelican
