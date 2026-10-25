#include "rendergraphir.hpp"
#include "stablefingerprint.hpp"

#include <bit>
#include <tuple>
#include <type_traits>

namespace Pelican {
namespace {

// Canonical field order, including optional presence and authored stage
// presence. No object bytes, pointers, JSON roundtrip, or derived planner
// view: those would lose execution values (notably disabled blend state).
#define IR_FIELDS(Type, ...) \
    auto fields(const Type &v) { return std::tie(__VA_ARGS__); }

IR_FIELDS(ResourceExtentPlan, v.kind, v.scale_x, v.scale_y, v.width, v.height)
IR_FIELDS(ImageMipLevelCount, v.mode, v.count)
IR_FIELDS(ImageSourceDeclaration, v.asset, v.color_space, v.alpha, v.mips)
auto fields(const ImageUsage &v) { return std::tuple{v.bits()}; }
IR_FIELDS(ImageResource, v.name, v.format_class, v.role, v.format,
          v.format_candidates, v.usage, v.extent, v.mip_levels,
          v.array_layers, v.dimension, v.history, v.persistent,
          v.clear_color, v.initial, v.source, v.format_unresolved)
IR_FIELDS(FrameGraphBufferExtentSizeDefinition, v.resource, v.tile_width,
          v.tile_height, v.header_bytes, v.bytes_per_tile, v.copies)
IR_FIELDS(FrameGraphBufferDefinition, v.name, v.size, v.persistent,
          v.host_source, v.extent_size, v.command_layout, v.host_view_family,
          v.index_buffer, v.initial_f32)
IR_FIELDS(ImageSubresourceRange, v.base_mip_level, v.level_count,
          v.base_array_layer, v.layer_count, v.mip_count_mode)
IR_FIELDS(LogicalReadFootprint, v.kind, v.radius)
IR_FIELDS(ShaderResourcePortSampling, v.filter, v.address_mode)
IR_FIELDS(ShaderResourceInputSetting, v.sampling, v.subresource)
IR_FIELDS(ResourceBinding, v.port, v.location, v.kind, v.access,
          v.view, v.sampling, v.element, v.domain)
IR_FIELDS(PassAttachmentOperations, v.load_op, v.store_op)
IR_FIELDS(ResourceUse, v.resource, v.role, v.access, v.intent, v.history,
          v.subresource, v.footprint, v.binding, v.operations)
IR_FIELDS(PassShaderDeclaration, v.shader_declared, v.vertex_shader,
          v.skinned_vertex_shader, v.fragment_shader)
IR_FIELDS(MaterialOutputBlendEquation, v.source, v.destination, v.operation)
IR_FIELDS(MaterialOutputBlendState, v.enabled, v.color, v.alpha)
IR_FIELDS(MaterialOutputField, v.name, v.type, v.source)
IR_FIELDS(MaterialOutputSchema, v.name, v.outputs)
IR_FIELDS(MaterialOutputAttachmentState, v.output, v.blend, v.write_mask)
IR_FIELDS(RasterColorAttachmentState, v.blend, v.write_mask)
IR_FIELDS(RasterDepthBounds, v.min, v.max)
IR_FIELDS(RasterDepthState, v.test, v.write, v.compare, v.bounds)
IR_FIELDS(RasterDepthBias, v.constant, v.slope, v.clamp)
IR_FIELDS(RasterStencilFaceState, v.fail, v.pass, v.depth_fail, v.compare,
          v.compare_mask, v.write_mask, v.reference)
IR_FIELDS(RasterStencilState, v.front, v.back)
IR_FIELDS(PipelineState, v.topology, v.primitive_restart, v.cull, v.front_face,
          v.polygon_mode, v.depth_clamp, v.depth_bias, v.line_width,
          v.rasterizer_discard, v.conservative, v.sample_shading,
          v.sample_mask, v.alpha_to_coverage, v.alpha_to_one, v.depth,
          v.stencil, v.logic_op, v.blend_constants, v.color_attachments)
IR_FIELDS(RasterDirectDrawOperation, v.vertex_count, v.instance_count,
          v.first_vertex, v.first_instance)
IR_FIELDS(RasterIndexedDrawOperation, v.index_buffer, v.index_count,
          v.instance_count, v.first_index, v.first_instance)
IR_FIELDS(RasterGeometryContract, v.implementation, v.operation)
IR_FIELDS(RasterPassContract, v.schema_version, v.geometry, v.state, v.clip_y)
IR_FIELDS(FullscreenNodeInfo, static_cast<const PassShaderDeclaration &>(v),
          v.implementation_provider, v.raster_state, v.push_constants,
          v.uses_light_data, v.input_sampling, v.shader_defines)
IR_FIELDS(RasterNodeInfo, static_cast<const PassShaderDeclaration &>(v),
          v.draw_declared, v.contract, v.shader_implementation, v.shader_defines)
IR_FIELDS(AuxiliaryNodeInfo, static_cast<const PassShaderDeclaration &>(v))
IR_FIELDS(MaterialDrawTagFilterId, v.value)
IR_FIELDS(MaterialDrawTagFilter, v.include, v.exclude, v.id)
IR_FIELDS(GpuDrawSourceDeclaration, v.commands, v.count, v.segments,
          v.max_draw_count, v.command_offset, v.count_offset, v.layout,
          v.fallback, v.execution)
IR_FIELDS(MaterialNodeInfo, v.filter, v.variant, v.gpu_draw_source, v.contract,
          v.output_schema, v.output_states, v.material_start, v.material_count)
IR_FIELDS(ComputeImageExtentDispatchDefinition, v.port)
IR_FIELDS(ComputeIndirectDispatchDefinition, v.buffer, v.offset)
IR_FIELDS(ComputeDispatchDefinition, v.groups_x, v.groups_y, v.groups_z,
          v.groups_from, v.rays_from, v.rays, v.indirect)
IR_FIELDS(AuthoredShaderField, v.form, v.value)
IR_FIELDS(AuthoredShaderList, v.form, v.entries)
IR_FIELDS(AuthoredHitGroup, v.group, v.is_object, v.closesthit, v.anyhit,
          v.intersection, v.declares_type, v.has_unknown_field)
IR_FIELDS(AuthoredHitShaders, v.ray_type, v.is_object, v.groups)
IR_FIELDS(RayRecordMemberLayout, v.name, v.type, v.offset, v.size, v.alignment)
IR_FIELDS(RayCallableRecordLayout, v.members, v.data_size)
IR_FIELDS(RayRecordValue, v.kind, v.signed_integer, v.unsigned_integer,
          v.floating, v.list)
IR_FIELDS(RayTracingTaskDeclaration, v.keys, v.raygen, v.miss, v.closesthit,
          v.traced_ray_types, v.misses_form, v.misses, v.hit_shaders_form,
          v.hit_shaders, v.callables, v.callable_record, v.callable_records,
          v.max_recursion, v.acceleration_structure)
IR_FIELDS(ComputeNodeInfo, v.shader, v.ray_tracing,
          v.ray_query_acceleration_structure, v.dispatch)
IR_FIELDS(SnapshotNodeInfo, v.source, v.destination, v.snapshot_after, v.byte_size)
IR_FIELDS(AccelerationStructureNodeInfo, v.named)
IR_FIELDS(NodeIR, v.name, v.declaration_index, v.shape, v.scope, v.uses,
          v.after, v.before, v.regions, v.view_family, v.view_select,
          v.schedule, v.resolution_domain, v.ray_query, v.clear_color,
          v.color_clear_values, v.input_settings, v.shader_declaration_parsed, v.info)
IR_FIELDS(RenderStrategySelection, v.name, v.provider, v.implementation,
          v.contract, v.output_contract, v.graph_variant, v.facade_capability_bits,
          v.input_config_fingerprint, v.output_config_fingerprint, v.provider_owner,
          v.provider_identity, v.provider_generation, v.provider_version,
          v.provider_capability_bits, v.explicitly_selected)
IR_FIELDS(LogicalGraphTransformSelection, v.name, v.provider, v.implementation,
          v.contract, v.boundary_fingerprint, v.input_graph_fingerprint,
          v.output_graph_fingerprint, v.provider_owner, v.provider_identity,
          v.provider_generation, v.provider_version, v.provider_capability_bits,
          v.transform_index, v.explicitly_selected)
IR_FIELDS(LogicalSubgraphReplacementSelection, v.region, v.provider,
          v.implementation, v.contract, v.contract_fingerprint, v.provider_owner,
          v.provider_identity, v.provider_generation, v.provider_version,
          v.provider_capability_bits, v.explicitly_selected, v.source_nodes,
          v.replacement_nodes)
IR_FIELDS(GraphIR, v.name, v.nodes, v.render_strategy, v.graph_transforms,
          v.subgraph_replacements)
IR_FIELDS(FrameNodeOrderIR, v.graph, v.nodes)
IR_FIELDS(RenderGraphIR, v.images, v.buffers, v.shared_compute_tasks,
          v.authored_shared_compute_tasks, v.graphs, v.frame_node_order,
          v.ray_task_acceleration_structures, v.ray_task_callable_records)

#undef IR_FIELDS

class GraphFingerprint {
    StableFingerprint64 hash_;

  public:
    template <typename T> void append(const T &value) {
        if constexpr (std::is_enum_v<T> || std::is_integral_v<T>) {
            hash_.appendUnsigned(static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, float>) {
            hash_.appendUnsigned(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            hash_.appendUnsigned(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, std::string> ||
                             std::is_same_v<T, std::string_view>) {
            hash_.appendString(value);
        } else if constexpr (std::is_same_v<T, std::monostate>) {
            // The enclosing variant already wrote its alternative index.
        } else if constexpr (requires { fields(value); }) {
            std::apply([&](const auto &...field) { (append(field), ...); }, fields(value));
        } else if constexpr (requires { value.has_value(); }) {
            append(value.has_value());
            if (value) append(*value);
        } else if constexpr (requires { std::variant_size<T>::value; }) {
            append(value.index());
            std::visit([&](const auto &entry) { append(entry); }, value);
        } else if constexpr (requires { value.first; value.second; }) {
            append(value.first);
            append(value.second);
        } else {
            // Vectors, arrays and ordered maps carry their size. No hash
            // table iteration order participates in the canonical stream.
            append(value.size());
            for (const auto &entry : value) append(entry);
        }
    }

    std::uint64_t value() const { return hash_.value(); }
};

} // namespace

std::uint64_t renderGraphFingerprint(const RenderGraphIR &ir) {
    GraphFingerprint fingerprint;
    fingerprint.append(std::string_view{"pelican.render_graph_ir@1"});
    fingerprint.append(ir);
    return fingerprint.value();
}

} // namespace Pelican
