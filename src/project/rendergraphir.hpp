#pragma once

#include "attachmentoperations.hpp"
#include "computetaskdeclaration.hpp"
#include "framegraphbufferdefinition.hpp"
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
#include "materialoutput.hpp"
#include "nodeschedule.hpp"
#include "passfieldownership.hpp"
#include "passdeclaration.hpp"
#include "rasterpass.hpp"
#include "renderpipeline.hpp"
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
// Planning and execution share these values: core derives pass and task
// definitions from NodeIR::info, uses and the generation's resources.

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

// Where the node was declared: in its graph's `passes` / `compute_tasks`,
// in the config's `compute_tasks` (shared: every graph has a copy, after its
// own), or made by the engine (the acceleration-structure builds).
enum class NodeDeclarationScope : std::uint8_t {
    graph,
    shared,
    synthesized,
};

// Execution values; resource ports are derived from the node's uses.
struct FullscreenNodeInfo : PassShaderDeclaration {
    std::optional<std::string> implementation_provider;
    std::optional<PipelineState> raster_state;
    FullscreenPushConstantData push_constants = FullscreenPushConstantData::eNone;
    bool uses_light_data = false;
    std::vector<ShaderResourcePortSampling> input_sampling;
    std::vector<std::string> shader_defines;

    bool operator==(const FullscreenNodeInfo &) const = default;
};

struct RasterNodeInfo : PassShaderDeclaration {
    bool draw_declared = false;
    RasterPassContract contract;
    std::string shader_implementation{"pelican.raster.authored_shader@1"};
    std::vector<std::string> shader_defines;

    bool operator==(const RasterNodeInfo &) const = default;
};

struct AuxiliaryNodeInfo : PassShaderDeclaration {
    bool operator==(const AuxiliaryNodeInfo &) const = default;
};

struct MaterialNodeInfo {
    std::optional<MaterialDrawTagFilter> filter;
    std::optional<std::string> variant;
    std::optional<GpuDrawSourceDeclaration> gpu_draw_source;
    // RC-3c2a: what the material pass's attachment check reads
    // (`material_contract`, defaultMaterialPassContract when absent;
    // `material_outputs`).
    MaterialPassContract contract = defaultMaterialPassContract;
    std::optional<MaterialOutputSchema> output_schema;
    std::vector<MaterialOutputAttachmentState> output_states;
    std::uint32_t material_start = 0;
    std::uint32_t material_count = 0;

    bool operator==(const MaterialNodeInfo &) const = default;
};

// RC-3c2b1: a compute task's execution values (core makes its
// ComputeTaskDefinition from these and the node's uses: reads, writes,
// resource ports, the indirect buffer).
struct ComputeNodeInfo {
    // `shader` of a compute shader task (nullopt for a ray tracing one; a
    // compute task without one plans, and computeTaskDefinitionsOf refuses
    // it).
    std::optional<std::string> shader;
    // `ray_tracing` (a ray pipeline): its declaration, with the acceleration
    // structure it traces (`ray_tracing.acceleration_structure`, or the
    // config's `ray_task_acceleration_structures`; "scene" when neither
    // names one).
    std::optional<RayTracingTaskDeclaration> ray_tracing;
    // `ray_query.acceleration_structure` of a compute shader that queries.
    std::optional<std::string> ray_query_acceleration_structure;
    ComputeDispatchDefinition dispatch;

    bool operator==(const ComputeNodeInfo &) const = default;
};

struct SnapshotNodeInfo {
    std::string source;
    std::string destination;
    std::string snapshot_after;
    // The copy's bytes: the destination's extent as the generation resolves
    // it (resolveResourceExtent at the output extent) times its format's
    // texel bytes, for the copy contract's 12 formats. RC-3c2a: set by
    // finalizeRenderGraph (0 in a revision before it).
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
                              RasterNodeInfo, AuxiliaryNodeInfo,
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
    NodeSchedule schedule = defaultNodeSchedule;
    RenderResolutionDomain resolution_domain =
        RenderResolutionDomain::unclassified;
    // The acceleration structure a pass of a raster family queries
    // (`ray_query`: true is "scene"); nullopt when it does not query.
    std::optional<std::string> ray_query;
    // Color overrides are indexed by attachment location. An absent
    // clear_colors is empty; an explicit {} expands the default per output.
    std::array<double, 4> clear_color{0.0, 0.0, 0.0, 1.0};
    std::vector<std::array<double, 4>> color_clear_values;
    std::vector<ShaderResourceInputSetting> input_settings;
    bool shader_declaration_parsed = false;
    NodeInfo info;

    bool operator==(const NodeIR &) const = default;
};

// A render target: the config's render_targets, the images registration
// makes, with their whole declaration. RC-3c2a: the one read of a render
// target declaration (core makes its RenderTargetDefinition from this,
// renderTargetDefinitionOf; the GPU registration from the generation's
// resource description, generationresources.hpp). A fixed extent keeps no
// extent_scale (it sizes nothing).
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
    // A config image with a `format_class` and no `format` yet (before the
    // runtime package resolves the class: the editor's composition, the
    // preview). `format` is then not the image's; what needs it
    // (finalizeRenderGraph, the device's formats) refuses
    // render_graph.image_format_unresolved@1 {resource}.
    bool format_unresolved = false;

    bool operator==(const ImageResource &) const = default;
};

// A buffer declaration: the config's buffers, with their whole declaration
// (RC-3c2a: the one read of it; the execution's FrameGraphBufferDefinition is
// this type). A size_from_extent buffer has size 0 here: the generation's
// resource description sizes it (resolveGenerationResources).
using BufferResource = FrameGraphBufferDefinition;

// RC-3c2a: a graph declares no render_targets or buffers of its own (nothing
// composes them; parseRenderGraph refuses them,
// render_graph.graph_declaration_unsupported@1). Every resource is the
// config's. RC-3c2b1: nor compute_tasks (the same refusal): every compute
// task is the config's, copied into every graph.
struct GraphIR {
    std::string name;
    // Its passes, then the config's compute tasks.
    std::vector<NodeIR> nodes;
    // Provenance: what made this graph (the render strategy, the graph
    // transforms, this graph's tagged subgraph replacements). The compiler
    // writes it after the provider chain; a parse leaves it empty, so the
    // logical graph a provider receives carries none. The physical plan's
    // logical-graph fingerprint includes it.
    std::optional<RenderStrategySelection> render_strategy;
    std::vector<LogicalGraphTransformSelection> graph_transforms;
    std::vector<LogicalSubgraphReplacementSelection> subgraph_replacements;

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
    // The config's compute tasks, by name, in declaration order
    // (namespaceSharedComputeTasks gives them the variant's suffix).
    std::vector<std::string> shared_compute_tasks;
    // RC-3c2b1: the same tasks by the names they were authored with (no
    // rename touches these): shared_compute_tasks[i] is
    // authored_shared_compute_tasks[i] renamed.
    std::vector<std::string> authored_shared_compute_tasks;
    std::vector<GraphIR> graphs;
    // `frame_node_order`, in its graphs' (sorted) order.
    std::vector<FrameNodeOrderIR> frame_node_order;
    // `ray_task_acceleration_structures` as written (task -> acceleration
    // structure, in the table's order). The parse holds its form; the tasks
    // it names may arrive later from a provider, so finalizeRenderGraph
    // resolves it.
    std::vector<std::pair<std::string, std::string>> ray_task_acceleration_structures;
    // RC-3c2b1: `ray_task_callable_records` as written (task -> its records'
    // values, in the table's order). Like the structures' table, the final
    // revision resolves it (resolveRenderGraphReferences): each task's
    // records packed by its callable record schema.
    std::vector<std::pair<std::string, RayCallableRecordValues>> ray_task_callable_records;

    bool operator==(const RenderGraphIR &) const = default;
};

const GraphIR *findGraph(const RenderGraphIR &ir, std::string_view name);
// The config's declaration of `name`; nullptr for none.
const ImageResource *findConfigImage(const RenderGraphIR &ir, std::string_view name);
const BufferResource *findConfigBuffer(const RenderGraphIR &ir, std::string_view name);

// §2.3: a read the shader ABI takes as a local read candidate (sampled
// descriptor or input attachment): a fullscreen or raster pass's current
// `input`, a material pass's screen input, a material resource that is an
// image read without storage access. A surface resource, a history read, any
// other pass type's input and every compute read are not.
bool isLocalReadShaderInput(const NodeIR &node, const ResourceUse &use);

// The planner's view of one graph, derived from its uses: what the frame
// planner, the logical shadow graph and target planning read (RC-3c1b: the
// physical plan's parts that still read a FrameGraphDefinition read this),
// with the graph's provenance.
FrameGraphDefinition frameGraphDefinitionOf(const RenderGraphIR &ir,
                                            const GraphIR &graph);

// The frame plan (planFrameGraph's rules and named refusals).
FramePlan planRenderGraph(const RenderGraphIR &ir, const GraphIR &graph);

// What a pass's authored inputs need: every current `input` of a pass of the
// graph (and a snapshot copy's `source`) is written by a node the plan runs
// before it, unless the resource is a config buffer or a config image that
// holds its contents before the frame (`initial`, a file `source`). A
// compute task's reads, a history read and every other use keep their import
// semantics. Refuses render_graph.input_not_produced@1 {graph, node,
// resource}.
void requireRenderGraphInputsProduced(const RenderGraphIR &ir, const GraphIR &graph,
                                      const FramePlan &plan);

// RC-3c2b1: the config's compute tasks, one node each (the first graph's
// copy), with the name it was authored with, in declaration order. What
// every compiler program's IR must hold for this is checked first, by name
// (render_graph.compute_task_invalid@1 {node, field: shared_compute_tasks,
// reason, graph?}): the authored names match the index one for one, no name
// is indexed twice, every graph has one copy of each indexed task
// (missing_copy, duplicate) and no shared node the index does not name
// (unindexed, not_a_compute_task), every compute node is a shared one (a
// graph's own: graph_scope; any other: not_shared), and the copies run the
// same but for their edges (copies_differ).
struct SharedComputeTask {
    const NodeIR *node = nullptr;
    std::string_view authored_name;
};
std::vector<SharedComputeTask> sharedComputeTasksOf(const RenderGraphIR &ir);

// RC-3c2b1: a compute task's indirect dispatch against the buffers as the
// generation's resource description sized them: the buffer is declared, the
// task does not write it, its command_layout is the task's
// (compute_dispatch, or ray_dispatch for a ray tracing task) and the command
// at the (4-byte aligned) offset fits. Refuses
// render_graph.compute_task_invalid@1 {node, field:
// "dispatch.indirect.buffer" | "dispatch.indirect.offset", reason:
// written_by_task | unknown_buffer | command_layout_mismatch |
// not_4_byte_aligned | command_exceeds_buffer} (with buffer, and expected /
// offset / size).
void validateComputeTaskBufferContracts(const RenderGraphIR &ir,
                                        std::span<const FrameGraphBufferDefinition> buffers);

// RC-3c2b1: the compiled pipeline's provenance for the final revision, from
// its IR: one pass entry per node a graph declares (each name once, in graph
// order) and one per config compute task (named as the variant renamed it,
// found by the name it was authored with, so its authored_name, feature and
// graph / loop origin stay), then one resource entry per config image,
// buffer and the swapchain of output_transform. An entry the provenance did
// not have is the engine's; a known image keeps its usage as spelled unless
// the final revision declares another set.
void synchronizeRenderGraphProvenance(const RenderGraphIR &ir,
                                      std::vector<RenderPassProvenance> &passes,
                                      std::vector<RenderResourceProvenance> &resources);

// The facts of an image declaration the physical plan's eligibility rules
// read. The one definition of each rule is here, over these facts; core
// describes its render targets with the same facts (one copy).
struct ImageDeclarationFacts {
    bool history = false;
    bool persistent = false;
    bool has_source = false;
    ImageUsage usage;
    std::string_view format_class;
};
ImageDeclarationFacts imageDeclarationFactsOf(const ImageResource &image);
// An attachment used for nothing else, neither history nor persistent: it
// may be a transient attachment.
bool isTransientAttachmentCandidate(const ImageDeclarationFacts &image);
// An attachment used for nothing but attachments, sampling and input
// attachments, neither history nor persistent: it may stay tile-local.
bool isTileLocalAttachmentCandidate(const ImageDeclarationFacts &image);
// A color attachment that is only sampled, not display, history, persistent
// or a file: it may share memory with another (before physical planning;
// after it, an aliased image must also still be materialized).
bool isRuntimeAliasEligible(const ImageDeclarationFacts &image);
// The node's read of `resource` is same_pixel.
bool nodeHasSamePixelRead(const FrameGraphNodeDefinition &node, std::string_view resource);

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
// The graph's structure is checked first (requireFrameGraphStructure), so a
// graph the planner refuses whatever the final order (a cycle, an unknown
// node or resource, ...) is refused by name before the logical graph is
// built; two unordered writers are left to planRenderGraph after the final
// frame_node_order. The graph's provenance is the logical graph's.
CompiledLogicalRenderGraph toLogicalGraph(
    const RenderGraphIR &ir, const GraphIR &graph,
    const ToLogicalGraphOptions &options = {});

// What the engine adds to the final revision before planning, once after the
// provider chain (docs/design_render_graph_ir.md §2.2), in this order:
//   every config image's format is resolved
//     (render_graph.image_format_unresolved@1 {resource});
//   every snapshot copy is sized at the output extent (snapshotCopyBytes;
//     render_graph.snapshot_size_unavailable@1 {graph, node, resource} for
//     a destination outside the copy contract);
//   resolveRenderGraphReferences (by the authored names);
//   requireRenderGraphRayQueryCapability; namespaceSharedComputeTasks;
//   insertAccelerationStructureNodes; applyFrameNodeOrder.
// The checks a revision needs in itself stay in parseRenderGraph; these are
// the ones only the final revision must pass.
struct FinalizeRenderGraphRequest {
    std::span<const SceneAccelerationStructureDefinition> named;
    bool ray_tracing_project = false;
    // The variant's suffix on the config's compute tasks ("#xr").
    std::string_view compute_task_suffix;
    // frame_node_order names only this variant's graphs when false (the
    // other variants' keys are checked across the program).
    bool require_all_graphs = false;
    // RC-3c2a: the output's extent a snapshot copy's destination is resolved
    // at (the generation's resource description resolves every image at it).
    ResolvedResourceExtent output_extent;
};
RenderGraphIR finalizeRenderGraph(RenderGraphIR ir, const FinalizeRenderGraphRequest &request,
                                  TargetPlanningPolicy &target_planning);
// RC-3c2a: a snapshot copy's bytes into `destination` at `output_extent`
// (resolveResourceExtent); 0 for a format outside the copy contract or an
// unresolved one.
std::size_t snapshotCopyBytes(const ImageResource &destination, ResolvedResourceExtent output_extent);
// RC-3c2a: whether a snapshot copy can copy into `destination`'s format.
bool snapshotCopiesFormatOf(const ImageResource &destination);

// The final revision's references, which every variant resolves (the
// preview too, which goes no further): the config's task tables
// (applyRayTaskTables), then every structure a config task or a
// raster-family pass selects is declared
// (requireSelectedAccelerationStructuresDeclared).
RenderGraphIR resolveRenderGraphReferences(
    RenderGraphIR ir, std::span<const SceneAccelerationStructureDefinition> named);
// The config's task tables against its compute tasks, by the authored names
// (every graph's copy of a task takes the value):
// ray_task_acceleration_structures selects the structure a task traces or
// queries (render_graph.compute_task_invalid@1 {node, field, reason:
// unknown_task | task_declares_no_ray_work}); ray_task_callable_records
// replaces a task's records, packed by its callable record schema
// (render_graph.compute_task_invalid@1 {node, field, reason: unknown_task |
// task_declares_no_callable_record}; the packing's refusals as for inline
// records). The only place either table is applied.
RenderGraphIR applyRayTaskTables(RenderGraphIR ir);

// RC-3c2b1: how many table entries and compute task renames this thread has
// applied (a test's count: each once per variant's final revision).
struct RenderGraphApplicationCounts {
    std::size_t acceleration_structure_entries = 0;
    std::size_t callable_record_entries = 0;
    std::size_t compute_task_renames = 0;
};
RenderGraphApplicationCounts renderGraphApplicationCounts() noexcept;
// "scene" or one of `named`: render_graph.acceleration_structure_unknown@1
// {node, acceleration_structure}.
void requireSelectedAccelerationStructuresDeclared(
    const RenderGraphIR &ir, std::span<const SceneAccelerationStructureDefinition> named);

// The steps of finalizeRenderGraph. IR in, IR out; no JSON.
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
// render_graph.acceleration_structure_reserved@1 {graph, node}. Every
// selected structure is one of `named` or "scene" already: finalize checks
// that first (resolveRenderGraphReferences, by the author's task names), and
// a direct caller does the same.
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
