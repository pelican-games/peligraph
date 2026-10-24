#pragma once

#include "attachmentoperations.hpp"
#include "frameplan.hpp"
#include "gpudrawsource.hpp"
#include "imageformat.hpp"
#include "imagesource.hpp"
#include "imageresourcedimension.hpp"
#include "imagesubresource.hpp"
#include "imageusage.hpp"
#include "logicalframegraphshadow.hpp"
#include "logicalrendergraph.hpp"
#include "materialdrawtag.hpp"
#include "passfieldownership.hpp"
#include "renderresolutiondomain.hpp"
#include "resourceextent.hpp"
#include "sceneaccelerationstructure.hpp"
#include "shaderresourceport.hpp"
#include "targetplanning.hpp"
#include "viewfamilyrelation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Pelican {

// RC-3c1a: the neutral render graph (docs/design_render_graph_ir.md §2.2).
// One completed graph's JSON is read into it once, by parseRenderGraph
// (rendergraphparse.hpp, the only JSON parser of a completed graph); the
// frame plan and the logical graph are derived from it here. The values are
// project vocabulary only (no Vulkan type).
//
// RC-3c1a carries what planning needs; the execution values (the pipeline
// state, shader defines, the per-pass details registration reads) arrive with
// RC-3c2 in NodeIR::info and in the resources.

// What a node is. Every authored pass type, a compute task, and the scene
// acceleration-structure build the engine inserts (never authored).
enum class RenderNodeShape : std::uint8_t {
    material,
    fullscreen,
    raster,
    output_transform,
    debug_draw,
    gizmo,
    debug_text,
    shadow_depth,
    velocity,
    picking,
    ui,
    imgui,
    anchor,
    snapshot_copy,
    compute,
    acceleration_structure,
};

std::string_view renderNodeShapeName(RenderNodeShape shape);
RenderNodeShape renderNodeShapeOfPassType(RenderPassType type);
FramePlanNodeKind framePlanNodeKindOfShape(RenderNodeShape shape);

// Where a use comes from. A use is the one record of a node touching a
// resource; the planner's reads / writes, the footprints, the access intents
// and the attachments are all derived from the uses (frameGraphDefinitionOf).
enum class ResourceUseRole : std::uint8_t {
    // A pass's `input`, a compute task's `reads` (a history read: `history`).
    input,
    // A compute task's `writes`.
    output,
    // A `resource_ports` entry: a typed binding of a resource an input or
    // output use names. It adds no read or write; it states the access.
    resource_port,
    // A material pass's `screen_inputs` / `surface_resources` /
    // `material_resources` entry (the binding's port is the entry's key).
    screen_input,
    surface_resource,
    material_resource,
    color_attachment,
    depth_attachment,
    // gpu_draw_source's commands / count, dispatch.indirect.buffer.
    indirect,
    // A snapshot copy's source / destination.
    copy_source,
    copy_destination,
};

std::string_view resourceUseRoleName(ResourceUseRole role);

enum class ResourceUseAccess : std::uint8_t {
    read,
    write,
    read_write,
};

// The values a descriptor or an attachment needs that belong to the use.
// `port` is the shader's name for it (a resource port's, a material entry's
// key); `location` an attachment's index among the pass's color outputs.
struct ResourceBinding {
    std::string port;
    std::optional<std::uint32_t> location;
    ShaderResourcePortKind kind = ShaderResourcePortKind::automatic;
    ShaderResourcePortAccess access = ShaderResourcePortAccess::automatic;
    ShaderResourcePortView view = ShaderResourcePortView::shared_2d;
    ShaderResourcePortSampling sampling;
    std::optional<ShaderResourceBufferElement> element;
    std::optional<ShaderResourceAttributeDomain> domain;

    bool operator==(const ResourceBinding &) const = default;
};

struct ResourceUse {
    // The resource's name without `@history` (a screen input or a surface
    // resource keeps its value as written).
    std::string resource;
    ResourceUseRole role = ResourceUseRole::input;
    ResourceUseAccess access = ResourceUseAccess::read;
    LogicalAccessIntent intent = LogicalAccessIntent::automatic;
    // The previous frame's value (`@history`).
    bool history = false;
    std::optional<ImageSubresourceRange> subresource;
    // How far from the invocation the read reaches. Declared
    // (input_footprints / read_footprints / material_resources.*.footprint)
    // or derived (§2.3: a history read is temporal, a loading attachment is
    // same_pixel, a screen input and a surface resource take their
    // contract's, a material resource without one is arbitrary;
    // undeclaredReadFootprint). nullopt only for a current input / reads /
    // indirect read that declares none (read as arbitrary; the logical
    // graph notes it as conservative) and for the uses that read nothing.
    std::optional<LogicalReadFootprint> footprint;
    ResourceBinding binding;
    // An attachment's resolved load / store (RC-3a2: attachment > pass >
    // the pass type's defaults); nullopt for every other role.
    std::optional<PassAttachmentOperations> operations;

    bool operator==(const ResourceUse &) const = default;
};

enum class NodeSchedule : std::uint8_t {
    per_frame,
    per_view,
    once,
};

// Where the node was declared: in its graph's `passes` / `compute_tasks`,
// in the config's `compute_tasks` (shared: every graph has a copy, after its
// own), or made by the engine (the acceleration-structure builds).
enum class NodeDeclarationScope : std::uint8_t {
    graph,
    shared,
    synthesized,
};

// A fullscreen or output-transform pass: the shader pair and whether the
// author named an implementation (`implementation`; the provider as written,
// "" when it is not a string). The provider is resolved later.
struct FullscreenNodeInfo {
    std::optional<std::string> implementation_provider;
    std::optional<std::string> vertex_shader;
    std::optional<std::string> fragment_shader;

    bool operator==(const FullscreenNodeInfo &) const = default;
};

struct MaterialNodeInfo {
    std::optional<MaterialDrawTagFilter> filter;
    std::optional<std::string> variant;
    std::optional<GpuDrawSourceDeclaration> gpu_draw_source;

    bool operator==(const MaterialNodeInfo &) const = default;
};

struct ComputeNodeInfo {
    // `ray_tracing` (a ray pipeline) and the acceleration structure it traces
    // (`ray_tracing.acceleration_structure`, or the config's
    // `ray_task_acceleration_structures`; "scene" when neither names one).
    bool ray_tracing = false;
    std::string ray_tracing_acceleration_structure = "scene";
    // `ray_query.acceleration_structure` of a compute shader that queries.
    std::optional<std::string> ray_query_acceleration_structure;

    bool operator==(const ComputeNodeInfo &) const = default;
};

struct SnapshotNodeInfo {
    std::string source;
    std::string destination;
    std::string snapshot_after;
    // The copy's bytes: the destination's extent times its format's texel
    // bytes, for the copy contract's 12 formats. 0 while the destination
    // waits on the output extent.
    std::size_t byte_size = 0;

    bool operator==(const SnapshotNodeInfo &) const = default;
};

// The scene's acceleration-structure build the engine inserts before the
// passes that query it: the scene's own (no name) or a named one.
struct AccelerationStructureNodeInfo {
    std::optional<std::string> named;

    bool operator==(const AccelerationStructureNodeInfo &) const = default;
};

using NodeInfo = std::variant<std::monostate, FullscreenNodeInfo,
                              MaterialNodeInfo, ComputeNodeInfo,
                              SnapshotNodeInfo,
                              AccelerationStructureNodeInfo>;

struct NodeIR {
    std::string name;
    std::size_t declaration_index = 0;
    RenderNodeShape shape = RenderNodeShape::fullscreen;
    NodeDeclarationScope scope = NodeDeclarationScope::graph;
    std::vector<ResourceUse> uses;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> regions;
    std::string view_family{mainRenderViewFamilyId};
    RenderViewSelect view_select;
    NodeSchedule schedule = NodeSchedule::per_frame;
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;
    // The acceleration structure a pass of a raster family queries
    // (`ray_query`: true is "scene"); nullopt when it does not query.
    std::optional<std::string> ray_query;
    NodeInfo info;

    bool operator==(const NodeIR &) const = default;
};

// A render target. RenderGraphIR::images (the config's render_targets, the
// images registration makes) carry the whole declaration, read as the render
// target parser reads it. GraphIR::images (a graph's own render_targets) are
// planning declarations, as the planner always read them: the name,
// `history`, and what a snapshot copy sizes; nothing registers them.
struct ImageResource {
    std::string name;
    std::string format_class;
    std::string role;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    // The format first, then the explicit candidates.
    std::vector<ImageFormat> format_candidates;
    ImageUsage usage;
    ResourceExtentPlan extent;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension = ImageResourceDimension::two_d;
    bool history = false;
    // `lifetime: "persistent"`.
    bool persistent = false;
    std::array<double, 4> clear_color{0.0, 0.0, 0.0, 0.0};
    // `initial` is declared: the image holds an asset before any pass runs.
    bool initial = false;
    std::optional<ImageSourceDeclaration> source;
    // A snapshot copy's size of the image (the copy contract): its bytes,
    // or that the extent waits on the output's.
    std::optional<std::size_t> snapshot_copy_bytes;
    bool snapshot_extent_pending = false;

    bool operator==(const ImageResource &) const = default;
};

// A buffer declaration. RC-3c1a reads the name (all planning uses); the size,
// layout, host source and view family arrive with RC-3c2.
struct BufferResource {
    std::string name;

    bool operator==(const BufferResource &) const = default;
};

struct GraphIR {
    std::string name;
    // The graph's own declarations; a name is looked up here first, then in
    // RenderGraphIR (the order the planner always used).
    std::vector<ImageResource> images;
    std::vector<BufferResource> buffers;
    // Its passes, its own compute tasks, then the config's compute tasks.
    std::vector<NodeIR> nodes;
    // Some own image declares width, height or extent_scale (a snapshot copy
    // is then sized from these images).
    bool images_declare_extent = false;

    bool operator==(const GraphIR &) const = default;
};

struct FrameNodeOrderIR {
    std::string graph;
    std::vector<std::string> nodes;

    bool operator==(const FrameNodeOrderIR &) const = default;
};

struct RenderGraphIR {
    std::vector<ImageResource> images;
    std::vector<BufferResource> buffers;
    // The config's compute tasks, by name, in declaration order.
    std::vector<std::string> shared_compute_tasks;
    std::vector<GraphIR> graphs;
    // `frame_node_order`, in its graphs' (sorted) order.
    std::vector<FrameNodeOrderIR> frame_node_order;
    bool images_declare_extent = false;

    bool operator==(const RenderGraphIR &) const = default;
};

const GraphIR *findGraph(const RenderGraphIR &ir, std::string_view name);
// The graph's own declaration of `name`, then the config's; nullptr for none.
const ImageResource *findImage(const RenderGraphIR &ir, const GraphIR &graph,
                               std::string_view name);

// §2.3: a read the shader ABI takes as a local read candidate (sampled
// descriptor or input attachment): a fullscreen or raster pass's current
// `input`, a material pass's screen input, a material resource that is an
// image read without storage access. A surface resource, a history read, any
// other pass type's input and every compute read are not.
bool isLocalReadShaderInput(const NodeIR &node, const ResourceUse &use);

// The planner's view of one graph, derived from its uses: what the frame
// planner, the logical shadow graph and target planning read. Node by node it
// is the FrameGraphNodeDefinition core's JSON parse makes (RC-3c1a's bridge
// test compares every field).
FrameGraphDefinition frameGraphDefinitionOf(const RenderGraphIR &ir,
                                            const GraphIR &graph);

// The frame plan (planFrameGraph's rules, refusals by name: see
// planFrameGraphWithNamedRefusals).
FramePlan planRenderGraph(const RenderGraphIR &ir, const GraphIR &graph);

// What the device is asked about an image in one of its formats: the image's
// usage and whether it is a cube (cube-compatible images), as core asks
// Vulkan for the format's properties.
struct RenderGraphImageCapabilityQuery {
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    ImageUsage usage;
    bool cube_compatible = false;

    bool operator==(const RenderGraphImageCapabilityQuery &) const = default;
};

// Neutral device facts for the logical graph's materialization: whether the
// device keeps attachments tile-local (transient attachments and dynamic
// rendering local read), and whether it supports an image of this format,
// usage and kind as a local-read attachment (the usage supported and the
// local read supported). No query: nothing is supported.
struct RenderGraphDeviceFacts {
    bool tile_local_attachments = false;
    std::function<bool(const RenderGraphImageCapabilityQuery &)>
        supports_local_read_attachment;
};

using RenderGraphTileLocalFormats =
    std::set<std::pair<std::string, ImageFormat>, std::less<>>;

// The (image, format) pairs the device could keep tile-local: a config image
// that is an attachment, neither history nor persistent, used for nothing but
// attachments, sampling and input attachments, in one of its declared formats
// the device supports for it as a local-read attachment.
RenderGraphTileLocalFormats renderGraphDeviceTileLocalFormats(
    const RenderGraphIR &ir, const RenderGraphDeviceFacts &device);
// Of those, the ones this graph can use: no node reads the image's history;
// every reader is a render node that reads it as a local read shader input,
// same_pixel, with all its attachments config images of the image's extent;
// every writer attaches it without a subresource; and it is both read and
// written.
RenderGraphTileLocalFormats renderGraphTileLocalFormats(
    const RenderGraphIR &ir, const GraphIR &graph,
    const RenderGraphTileLocalFormats &device_formats);
// The logical graph's resource types: the swapchain's display encoding,
// a config image's depth or opaque image type, virtual when it may be
// transient, tile-local or aliased, otherwise required.
LogicalFrameGraphShadowOptions renderGraphShadowOptions(
    const RenderGraphIR &ir, const FrameGraphDefinition &definition,
    const LogicalTypeRegistry &types,
    const RenderGraphTileLocalFormats &tile_local_formats);

struct ToLogicalGraphOptions {
    RenderGraphDeviceFacts device;
};
CompiledLogicalRenderGraph toLogicalGraph(
    const RenderGraphIR &ir, const GraphIR &graph,
    const ToLogicalGraphOptions &options = {});

// What the engine adds to a parsed graph before planning (RC-3c1b's
// finalizeRenderGraph runs them in this order). IR in, IR out; no JSON.
//
// A variant's suffix on the config's compute tasks (and on every after /
// before naming one). Refuses render_graph.compute_task_duplicate@1 {task}
// and render_graph.compute_task_name_collision@1 {graph, node}.
RenderGraphIR namespaceSharedComputeTasks(RenderGraphIR ir,
                                          std::string_view suffix);

// The compiler's rule before the builds: when a pass of a raster family or a
// config compute task queries the scene, or the pipeline names scene
// acceleration structures, every graph's target planning requires
// pelican.vulkan.ray_query@1 (added once, the list kept sorted).
void requireRenderGraphRayQueryCapability(
    const RenderGraphIR &ir,
    std::span<const SceneAccelerationStructureDefinition> named,
    TargetPlanningPolicy &target_planning);

struct AccelerationStructureNodeRequest {
    // Graphs whose target planning requires ray query or a ray pipeline.
    std::span<const PlanningGraphConstraints> planning_graphs;
    bool ray_tracing_project = false;
    std::span<const SceneAccelerationStructureDefinition> named;
};
// The scene's acceleration-structure builds ("__scene_acceleration_structure"
// and one "__scene_acceleration_structure:<name>" per named one, after it) at
// the head of every graph when anything queries or traces (a raster-family
// pass's ray_query, a config task's ray_query or ray_tracing, the target
// planning's capability, a ray tracing project), and the passes and
// tasks that do waiting on them; declaration order is renumbered. Refuses
// render_graph.acceleration_structure_reserved@1 {graph, node} and
// render_graph.acceleration_structure_unknown@1 {node, acceleration_structure}.
RenderGraphIR insertAccelerationStructureNodes(
    RenderGraphIR ir, const AccelerationStructureNodeRequest &request);

// `frame_node_order`: each listed node after the one listed before it.
// Refuses render_graph.frame_node_order_unknown@1 {graph, node?} (a graph
// only when require_all_graphs) and frame_node_order.conflicts_dependency@1.
RenderGraphIR applyFrameNodeOrder(RenderGraphIR ir, bool require_all_graphs);

// The nodes an XR variant may run once for all views (multiview): a
// fullscreen pass without `implementation` drawing engine://fullscreen with
// a builtin fragment, in every graph.
std::set<std::string, std::less<>> renderGraphMultiviewCapableNodes(
    const RenderGraphIR &ir, bool enable_multiview_runtime);

} // namespace Pelican
