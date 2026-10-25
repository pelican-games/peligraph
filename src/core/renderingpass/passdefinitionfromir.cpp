#include "passdefinitionfromir.hpp"

#include "../../project/graphcomposition.hpp"
#include "../../project/materialscreeninput.hpp"
#include "../userpublic/render/pass_implementation_abi_v1.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace Pelican {
namespace {

static_assert(maximumPassImplementationProviderNameBytes == RenderPass::maximumProviderNameBytesV1);

[[noreturn]] void refuse(const GraphIR &graph, const NodeIR &node,
                         std::string_view field, std::string_view reason,
                         nlohmann::json more = nlohmann::json::object()) {
    more["graph"] = graph.name;
    more["node"] = node.name;
    more["field"] = field;
    more["reason"] = reason;
    throw GraphRegistryError("render_graph.pass_execution_invalid@1", std::move(more));
}

template<class Info>
const Info &infoOf(const GraphIR &graph, const NodeIR &node) {
    const auto *info = std::get_if<Info>(&node.info);
    if (!info) refuse(graph, node, "info", "shape_mismatch");
    return *info;
}

ShaderResourcePortDefinition portOf(const ResourceUse &use) {
    return ShaderResourcePortDefinition{
        .name = use.binding.port,
        .resource = use.resource + (use.history ? "@history" : ""),
        .kind = use.binding.kind,
        .buffer_element = use.binding.element,
        .access = use.binding.access,
        .view = use.binding.view,
        .sampling = use.binding.sampling,
        .subresource = use.subresource,
        .domain = use.binding.domain,
    };
}

bool isRenderingPass(RenderNodeShape shape) {
    switch (shape) {
    case RenderNodeShape::anchor:
    case RenderNodeShape::snapshot_copy:
    case RenderNodeShape::compute:
    case RenderNodeShape::acceleration_structure: return false;
    default: return true; // Unknown values reach a named refusal below.
    }
}

RenderPassType passType(const GraphIR &graph, const NodeIR &node) {
    for (const auto &entry : passFieldOwnershipTable()) {
        if (renderNodeShapeOfPassType(entry.type) == node.shape) return entry.type;
    }
    refuse(graph, node, "shape", "not_a_rendering_pass");
}

} // namespace

RenderTargetNameResolver generationRenderTargetNames(const GenerationResources &resources) {
    return RenderTargetNameResolver{[&resources](const std::string &name) {
        for (std::size_t i = 0; i < resources.images.size(); ++i) {
            if (resources.images[i].declaration.name == name) {
                if (i > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    throw GraphRegistryError("render_graph.resource_index_invalid@1", {{"resource", name}});
                }
                return GlobalRenderTargetId{static_cast<int>(i)};
            }
        }
        return noRenderTargetId();
    }};
}

PassDefinition passDefinitionOf(const RenderGraphIR &ir, const GraphIR &graph,
                                 const NodeIR &node, const GenerationResources &resources,
                                 const RenderTargetNameResolver &names) {
    if (!isRenderingPass(node.shape)) refuse(graph, node, "shape", "not_a_rendering_pass");
    PassDefinition result;
    result.name = node.name;
    if (result.name.empty()) refuse(graph, node, "name", "empty");
    result.applyDefaultAttachmentOperations(passType(graph, node));
    result.region_tags = node.regions;
    result.view_family = node.view_family;
    result.view_select = node.view_select;
    result.resolution_domain = node.resolution_domain;
    result.once = node.schedule == NodeSchedule::once;
    result.ray_query = node.ray_query.has_value();
    result.ray_query_acceleration_structure = node.ray_query.value_or("scene");
    result.clear_color = node.clear_color;
    result.color_clear_values = node.color_clear_values;
    result.input_target_settings = node.input_settings;
    result.shader_declaration_parsed = node.shader_declaration_parsed;

    const auto target = [&](const ResourceUse &use, bool allow_swapchain) {
        if (allow_swapchain && use.resource == "swapchain") return swapchainRenderTargetId();
        if (!findConfigImage(ir, use.resource) || !resources.findImage(use.resource)) {
            refuse(graph, node, resourceUseRoleName(use.role), "unknown_image", {{"resource", use.resource}});
        }
        const auto id = names.resolve(use.resource);
        if (!isConcreteRenderTarget(id)) refuse(graph, node, resourceUseRoleName(use.role), "unresolved_image", {{"resource", use.resource}});
        return id;
    };
    std::optional<std::uint32_t> samples;
    std::vector<ShaderResourcePortDefinition> ports;
    for (const auto &use : node.uses) {
        if (use.role == ResourceUseRole::color_attachment || use.role == ResourceUseRole::depth_attachment) {
            if (!use.operations) refuse(graph, node, "attachment.operations", "missing", {{"resource", use.resource}});
            const auto id = target(use, true);
            RasterAttachmentView attachment{id, use.subresource, use.operations->load_op, use.operations->store_op};
            if (use.role == ResourceUseRole::color_attachment) {
                if (use.binding.location != result.output_color.size()) refuse(graph, node, "attachment.location", "not_contiguous");
                result.output_color.push_back(std::move(attachment));
            } else {
                if (isConcreteRenderTarget(result.output_depth) || isSwapchainRenderTarget(result.output_depth)) refuse(graph, node, "output.depth", "duplicate");
                result.output_depth = std::move(attachment);
            }
            if (!samples) samples = isSwapchainRenderTarget(id) ? 1 : resources.findImage(use.resource)->samples;
        } else if (use.role == ResourceUseRole::input) {
            if (findConfigBuffer(ir, use.resource)) {
                if (use.history) refuse(graph, node, "input", "buffer_history", {{"resource", use.resource}});
                result.input_buffers.push_back(use.resource);
            } else {
                result.input_targets.push_back(target(use, true));
                result.input_target_history.push_back(use.history);
            }
        } else if (use.role == ResourceUseRole::resource_port) {
            ports.push_back(portOf(use));
        }
    }
    switch (samples.value_or(1)) {
    case 1: case 2: case 4: case 8: case 16: case 32: case 64:
        result.rasterization_samples = static_cast<vk::SampleCountFlagBits>(samples.value_or(1)); break;
    default: refuse(graph, node, "samples", "invalid_sample_count");
    }

    const auto shader = [&](const std::optional<std::string> &ref, ShaderStage stage, const char *field) {
        if (!ref) refuse(graph, node, field, "required");
        try { return makeShaderReference(*ref, stage); }
        catch (const std::exception &error) { refuse(graph, node, field, "invalid_reference", {{"detail", error.what()}}); }
    };
    const PassShaderDeclaration *declared = nullptr;
    switch (node.shape) {
    case RenderNodeShape::fullscreen:
    case RenderNodeShape::output_transform: {
        const auto &info = infoOf<FullscreenNodeInfo>(graph, node);
        declared = &info;
        result.requested_implementation_provider = info.implementation_provider;
        FullscreenPassInfo out;
        out.vert_shader = shader(info.vertex_shader, ShaderStage::vertex, "shader.vertex");
        out.frag_shader = shader(info.fragment_shader, ShaderStage::fragment, "shader.fragment");
        out.raster_state = info.raster_state;
        out.push_constants = info.push_constants;
        out.uses_light_data = info.uses_light_data;
        out.input_sampling = info.input_sampling;
        out.shader_defines = info.shader_defines;
        out.resource_ports = std::move(ports);
        result.pass_info = std::move(out);
        break;
    }
    case RenderNodeShape::raster: {
        const auto &info = infoOf<RasterNodeInfo>(graph, node);
        if (!info.draw_declared) refuse(graph, node, "draw", "required");
        declared = &info;
        GenericRasterPassInfo out;
        out.contract = info.contract;
        out.shader_implementation = info.shader_implementation;
        out.vert_shader = shader(info.vertex_shader, ShaderStage::vertex, "shader.vertex");
        if (info.fragment_shader || !result.output_color.empty()) out.frag_shader = shader(info.fragment_shader, ShaderStage::fragment, "shader.fragment");
        out.shader_defines = info.shader_defines;
        out.resource_ports = std::move(ports);
        result.pass_info = std::move(out);
        break;
    }
    case RenderNodeShape::material: {
        const auto &info = infoOf<MaterialNodeInfo>(graph, node);
        MaterialPassInfo out;
        out.material_start = info.material_start;
        out.material_count = info.material_count;
        out.contract = info.contract;
        out.output_schema = info.output_schema;
        out.output_states = info.output_states;
        out.material_filter = info.filter;
        out.material_variant = info.variant;
        if (info.gpu_draw_source) out.gpu_draw_source = GpuDrawSourceDefinition{*info.gpu_draw_source};
        const auto types = makeBuiltinLogicalTypeRegistry();
        const auto conversions = makeBuiltinLogicalTypeConversionRegistry(types);
        const auto append_image = [&](GlobalRenderTargetId id, const ResourceUse &use) {
            const auto found = std::find(result.input_targets.begin(), result.input_targets.end(), id);
            if (found == result.input_targets.end()) {
                result.input_targets.push_back(id);
                result.input_target_history.push_back(use.history);
            } else if (result.input_target_history.at(static_cast<std::size_t>(found - result.input_targets.begin())) != use.history) {
                refuse(graph, node, resourceUseRoleName(use.role), "current_and_history", {{"resource", use.resource}});
            }
        };
        for (const auto &use : node.uses) {
            if (use.role == ResourceUseRole::screen_input || use.role == ResourceUseRole::surface_resource) {
                const auto id = target(use, false);
                auto contract = use.role == ResourceUseRole::screen_input
                    ? makeBuiltinMaterialScreenInputContract(types, use.binding.port)
                    : makeBuiltinMaterialPassInputContract(types, use.binding.port);
                try {
                    contract = resolveMaterialScreenInputContract(types, conversions, std::move(contract),
                        materialInputSourceType(types, resources.findImage(use.resource)->format, use.resource)).contract;
                } catch (const std::exception &error) {
                    refuse(graph, node, resourceUseRoleName(use.role), "invalid_contract", {{"port", use.binding.port}, {"detail", error.what()}});
                }
                auto &bindings = use.role == ResourceUseRole::screen_input ? out.screen_inputs : out.surface_resources;
                bindings.push_back(MaterialPassInputBinding{std::move(contract), id, false});
                append_image(id, use);
            } else if (use.role == ResourceUseRole::material_resource) {
                MaterialPassResourceBinding binding;
                binding.port = portOf(use);
                binding.history = use.history;
                binding.footprint = use.footprint.value_or(undeclaredReadFootprint(use.history));
                if (findConfigBuffer(ir, use.resource)) {
                    binding.buffer = use.resource;
                    if (std::find(result.input_buffers.begin(), result.input_buffers.end(), use.resource) == result.input_buffers.end()) result.input_buffers.push_back(use.resource);
                } else {
                    binding.target = target(use, false);
                    append_image(binding.target, use);
                }
                out.material_resources.push_back(std::move(binding));
            }
        }
        result.pass_info = std::move(out);
        break;
    }
    case RenderNodeShape::debug_draw:
    case RenderNodeShape::gizmo:
    case RenderNodeShape::debug_text:
    case RenderNodeShape::shadow_depth:
    case RenderNodeShape::velocity:
    case RenderNodeShape::picking: {
        const auto &info = infoOf<AuxiliaryNodeInfo>(graph, node);
        declared = &info;
        if (node.shape == RenderNodeShape::shadow_depth) {
            result.pass_info = ShadowDepthPassInfo{shader(info.shader_declared ? info.vertex_shader : std::optional<std::string>{"engine://shadow_depth"}, ShaderStage::vertex, "shader.vertex")};
            break;
        }
        const bool velocity = node.shape == RenderNodeShape::velocity;
        const bool picking = node.shape == RenderNodeShape::picking;
        const std::string stem = velocity ? "engine://velocity" : "engine://picking";
        const auto vert = shader((velocity || picking) && !info.shader_declared ? std::optional<std::string>{stem} : info.vertex_shader, ShaderStage::vertex, "shader.vertex");
        const auto frag = shader((velocity || picking) && !info.shader_declared ? std::optional<std::string>{stem} : info.fragment_shader, ShaderStage::fragment, "shader.fragment");
        if (velocity || picking) {
            const auto skinned = shader(info.skinned_vertex_shader.value_or(stem + "_skinned"), ShaderStage::vertex, "shader.skinned_vertex");
            if (velocity) result.pass_info = VelocityPassInfo{vert, skinned, frag};
            else result.pass_info = PickingPassInfo{vert, skinned, frag};
        } else if (node.shape == RenderNodeShape::debug_draw) result.pass_info = DebugDrawPassInfo{vert, frag};
        else if (node.shape == RenderNodeShape::gizmo) result.pass_info = GizmoPassInfo{vert, frag};
        else result.pass_info = DebugTextPassInfo{vert, frag};
        break;
    }
    case RenderNodeShape::ui: result.pass_info = UiPassInfo{}; break;
    case RenderNodeShape::imgui:
#if PELICAN_WITH_IMGUI
        result.pass_info = ImGuiPassInfo{}; break;
#else
        refuse(graph, node, "type", "imgui_disabled");
#endif
    default: refuse(graph, node, "shape", "not_a_rendering_pass");
    }
    if (declared) {
        const auto append = [&](const std::optional<std::string> &ref, DeclaredShaderStage stage) {
            if (ref) result.declared_shader_refs.push_back(DeclaredShaderReference{.stage = stage, .ref = *ref});
        };
        append(declared->vertex_shader, DeclaredShaderStage::vertex);
        append(declared->skinned_vertex_shader, DeclaredShaderStage::skinned_vertex);
        append(declared->fragment_shader, DeclaredShaderStage::fragment);
    }
    return result;
}

RenderingPassDefinition renderingPassDefinitionOf(const RenderGraphIR &ir, const GraphIR &graph,
                                                   const GenerationResources &resources,
                                                   const RenderTargetNameResolver &names) {
    RenderingPassDefinition result;
    result.name = graph.name;
    std::set<std::string, std::less<>> unique;
    for (const auto &node : graph.nodes) {
        if (!unique.insert(node.name).second) refuse(graph, node, "name", "duplicate");
        if (isRenderingPass(node.shape)) result.passes.push_back(passDefinitionOf(ir, graph, node, resources, names));
    }
    return result;
}

std::vector<RenderingPassDefinition> renderingPassDefinitionsOf(const RenderGraphIR &ir,
                                                               const GenerationResources &resources,
                                                               const RenderTargetNameResolver &names) {
    std::vector<RenderingPassDefinition> result;
    for (const auto &graph : ir.graphs) result.push_back(renderingPassDefinitionOf(ir, graph, resources, names));
    return result;
}

} // namespace Pelican
