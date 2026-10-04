#include "graphparameterschema.hpp"
#include "graphcomposition.hpp"
#include "graphloopexpression.hpp"
#include "renderpipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>

namespace Pelican {
namespace {
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
using Kind = GraphParameterKind;

constexpr double uint32_max = static_cast<double>((std::numeric_limits<std::uint32_t>::max)());

// One parameter, written the way the table below reads.
class P {
    GraphParameterSchema schema_;

  public:
    P(std::string name, Kind kind) {
        schema_.name = std::move(name);
        schema_.kind = kind;
    }
    P &required(std::initializer_list<const char *> alternatives = {}) {
        schema_.required = true;
        for (const auto *alternative : alternatives) schema_.alternatives.emplace_back(alternative);
        return *this;
    }
    P &fallback(Json value) {
        schema_.default_value = std::move(value);
        return *this;
    }
    P &values(std::initializer_list<const char *> names) {
        for (const auto *name : names) schema_.enum_values.emplace_back(name);
        return *this;
    }
    P &values(std::vector<std::string> names) {
        schema_.enum_values = std::move(names);
        return *this;
    }
    P &range(std::optional<double> min, std::optional<double> max) {
        schema_.min = min;
        schema_.max = max;
        return *this;
    }
    P &item(const P &shape) {
        schema_.items = std::make_shared<const GraphParameterSchema>(shape.schema_);
        return *this;
    }
    P &field(const P &member) {
        schema_.fields.push_back(member.schema_);
        return *this;
    }
    P &expression() {
        schema_.expression = true;
        return *this;
    }
    P &describe(std::string text) {
        schema_.description = std::move(text);
        return *this;
    }
    P &checkedByOp() {
        schema_.checked_by_op = true;
        return *this;
    }
    P &softRange(std::optional<double> min, std::optional<double> max) {
        schema_.soft_min = min;
        schema_.soft_max = max;
        return *this;
    }
    P &assetOf(std::string kind, std::initializer_list<const char *> extensions) {
        schema_.asset_kind = std::move(kind);
        for (const auto *extension : extensions) schema_.extensions.emplace_back(extension);
        return *this;
    }
    P &choicesFrom(std::string rpc, std::string key, std::string field = {}) {
        schema_.choices_from = GraphParameterChoices{std::move(rpc), std::move(key), std::move(field), {}};
        return *this;
    }
    // RTL-1b: a map object's keys, from an RPC's list.
    P &keyChoicesFrom(std::string rpc, std::string key, std::string field = {}) {
        schema_.key_choices_from = GraphParameterChoices{std::move(rpc), std::move(key), std::move(field), {}};
        return *this;
    }
    P &choicesFromGraph(std::initializer_list<const char *> ops) {
        GraphParameterChoices choices;
        for (const auto *op : ops) choices.graph_ops.emplace_back(op);
        schema_.choices_from = std::move(choices);
        return *this;
    }
    // CT-5: the names at `key` in the import manifest of the asset the
    // sibling parameter `parameter` names.
    P &choicesFromAssetManifest(std::string parameter, std::string key) {
        GraphParameterChoices choices;
        choices.asset_parameter = std::move(parameter);
        choices.key = std::move(key);
        schema_.choices_from = std::move(choices);
        return *this;
    }
    // CT-5: keeps only the choices whose `name` is one of `values`.
    P &where(std::string name, std::vector<std::string> values) {
        schema_.choices_from->where.emplace_back(std::move(name), std::move(values));
        return *this;
    }
    P &variant(const P &shape) {
        schema_.variants.push_back(shape.schema_);
        return *this;
    }
    const GraphParameterSchema &schema() const { return schema_; }
};

P number(std::string name) { return P{std::move(name), Kind::number}; }
P integer(std::string name) { return P{std::move(name), Kind::integer}; }
P boolean(std::string name) { return P{std::move(name), Kind::boolean}; }
P string(std::string name) { return P{std::move(name), Kind::string}; }
P enumeration(std::string name, std::initializer_list<const char *> names) {
    P result{std::move(name), Kind::enumeration};
    result.values(names);
    return result;
}
P array(std::string name, const P &item) {
    P result{std::move(name), Kind::array};
    result.item(item);
    return result;
}
P object(std::string name) { return P{std::move(name), Kind::object}; }
P asset(std::string name) { return P{std::move(name), Kind::asset}; }
P shader(std::string name) { return P{std::move(name), Kind::shader}; }
P nodeRef(std::string name) { return P{std::move(name), Kind::node_ref}; }
P color(std::string name) { return P{std::move(name), Kind::color}; }
P json(std::string name) { return P{std::move(name), Kind::json}; }
P union_(std::string name) { return P{std::move(name), Kind::union_}; }
P uint32(std::string name) { return integer(std::move(name)).range(0.0, uint32_max); }

// --- Shared by several ops, each as its parser reads it. ---

// CT-4: the lists a string parameter's value is chosen from. The authoring
// context lists the composed pipeline's names (renderconfigeditor.cpp).
constexpr const char *authoring_context = "get_render_authoring_context";

// CT-4: the image extent the engine's own features take at most: the
// `resolution` range [64, 8192] of shadow_directional / shadow_point /
// shadow_spot / planar_reflection / cube_capture
// (src/core/resources/features/*.json), and a full mip chain of it.
constexpr double image_extent_soft_max = 8192.0;
constexpr double image_mip_levels_soft_max = 14.0; // floor(log2(8192)) + 1

// CT-5: what a render.scalar may write (renderingpassconfigregistration.cpp
// publishes float32 values into a runtime_f32 buffer; lowerScalarGraph in
// valuegraph.cpp refuses a buffer whose elements are not floats).
const std::vector<std::string> render_scalar_target_elements{"float", "vec2", "vec3", "vec4", "mat4"};

// A scene's model (model.cpp), which a pose's rig is checked against.
P sceneModel(std::string name) { return asset(std::move(name)).assetOf("model", {".glb", ".gltf", ".vrm"}); }

// pelican.render.*: a logical image type (resourceType in graphmodule.cpp).
P imageType() {
    return object("type")
        .field(string("semantic").required()
                   .choicesFrom("get_graph_catalog", "modules.types", "id").where("constructor", {"image"})
                   .describe("A semantic image type id, e.g. pelican.render.color_signal@1."))
        .field(object("arguments").required().item(string("")).describe("The type's arguments, each an enum value "
                                                                          "as a string (the catalog's types list them)."))
        .describe("The image's logical type.");
}

const char *const buffer_elements[] = {"float", "vec2", "vec3", "vec4", "int", "ivec2", "ivec3",
                                      "ivec4", "uint", "uvec2", "uvec3", "uvec4", "mat4"};

P bufferElement() {
    P result{"element", Kind::enumeration};
    result.values(std::vector<std::string>(std::begin(buffer_elements), std::end(buffer_elements)));
    return result.required().describe("The element type of the buffer.");
}

// parseHostBufferSource / parseBufferCommandLayout / the buffer lifetime.
P hostSource() {
    return enumeration("host_source", {"runtime_f32", "scene_lights_v2", "view_family_matrices_v1",
                                       "scene_draw_commands_v1", "scene_draw_bounds_v1", "scene_draw_segments_v1"})
        .describe("What the engine writes into the buffer from the host each frame.");
}
P commandLayout() {
    return enumeration("command_layout", {"compute_dispatch", "ray_dispatch", "indexed_draw", "draw_count"})
        .describe("The indirect command the buffer holds.");
}
P initialF32() {
    return array("initial_f32", number("")).describe("The first contents, one float per 4 bytes (host_source "
                                                     "runtime_f32).");
}
P bufferViewFamily() {
    return string("view_family")
        .choicesFrom(authoring_context, "view_families")
        .describe("The view family whose matrices a view_family_matrices_v1 buffer holds.");
}
P bufferLifetime() {
    return enumeration("lifetime", {"persistent", "transient"}).fallback("persistent")
        .describe("Whether the contents are kept from frame to frame.");
}
P indexBuffer() { return boolean("index_buffer").fallback(false).describe("The buffer is bound as an index buffer."); }

// Any pass's views, tags, family and place.
P viewFamily() {
    return string("view_family").fallback("$main").choicesFrom(authoring_context, "view_families")
        .describe("The view family the pass renders for.");
}
P viewSelect() {
    return object("view_select")
        .item(uint32("").expression())
        .describe("One view of the family: axis name to a non-negative index.");
}
// CT-5: the region tags the composed passes declare (a new one may be
// written; the tag is the pass's own).
P regionTag() { return string("").choicesFrom(authoring_context, "graphs.passes.declaration", "regions"); }
P regionTags() {
    return array("region_tags", regionTag()).describe("Region tags (merged with `regions`).");
}
P regions() { return array("regions", regionTag()).describe("Region tags, as the pass JSON names them."); }
P passSet() {
    return string("pass_set").choicesFrom(authoring_context, "graphs", "name")
        .describe("The pass family the pass joins.");
}
P order(const char *key) {
    return array(key, nodeRef("")).describe(std::string{"Passes this one runs "} + key + ".");
}
P insert(bool declared) {
    auto result = object("insert")
        .field(nodeRef("after").describe("The pass this one follows."))
        .field(nodeRef("before").describe("The pass this one precedes."));
    if (declared)
        result.field(boolean("declared").fallback(false).describe("Among the declared passes, before any feature."));
    return result.describe("Where the pass lands: exactly one of after / before.");
}
P canonicalAnchor() {
    return string("canonical_anchor").choicesFrom(authoring_context, "canonical_anchors")
        .describe("A canonical pass anchor the pass lands at (excludes insert).");
}
P loadOp(const char *name) {
    return enumeration(name, {"clear", "load", "dont_care", "Clear", "Load", "DontCare", "dontCare"});
}
P storeOp(const char *name) { return enumeration(name, {"store", "dont_care", "Store", "DontCare", "dontCare"}); }
P resolutionDomain(const char *fallback) {
    return enumeration("resolution_domain", {"scene", "output", "independent", "unclassified", "none"})
        .fallback(fallback)
        .describe("The extent domain the pass renders in.");
}
// CT-5: "scene" or a composed scene_acceleration_structures name
// (vulkanrendercompilerprogram.cpp refuses any other).
P accelerationStructure() {
    return string("acceleration_structure").required().choicesFrom(authoring_context, "acceleration_structures");
}
P rasterRayQuery() {
    return union_("ray_query").fallback(false)
        .variant(boolean(""))
        .variant(object("").field(accelerationStructure()))
        .describe("true (the scene's acceleration structure) or {\"acceleration_structure\": \"<name>\"}.");
}
P inheritBindingsFrom() {
    return string("inherit_bindings_from").choicesFrom(authoring_context, "graphs.passes", "name")
        .describe("The pass whose bindings it takes.");
}
P clearColor() { return color("clear_color").describe("The color attachments' clear value (RGBA)."); }
P clearColors() {
    return object("clear_colors").item(color("")).describe("Clear values by color output name.");
}
P passSchedule() {
    return enumeration("schedule", {"per_frame", "once"}).fallback("per_frame").describe("When the pass runs.");
}
P shaderDefines() {
    return array("defines", string("").expression()).describe("NAME or NAME=VALUE defines.");
}

// --- CT-6: render.compute's ray_tracing and dispatch
// (computetaskdefinitionjson.cpp). ---

// One shader's path, or several (parseRayTracingShaderList).
P rayShaderList(std::string name) {
    return union_(std::move(name)).variant(shader("")).variant(array("", shader("")));
}

// parseRayTracingShaders without a project ray_group_layout reads raygen,
// miss and closesthit; parseLayoutRayTracingShaders with one reads raygen,
// traced_ray_types, misses and hit_shaders and refuses miss / closesthit.
// Either reads callable, callable_record / callable_records
// (parseCallableRecords, parseRayCallableRecordLayout, encodeRayCallableRecords),
// max_recursion (parseRayTracingRecursion) and acceleration_structure
// (parseComputeTaskDefinitionsFromConfigJson). The defaults are
// RayTracingTaskShaderDefinition's.
P rayTracing() {
    return object("ray_tracing")
        .field(shader("raygen").required().describe("The ray generation shader."))
        .field(rayShaderList("miss").describe("The miss shaders, in index order (required without a ray group "
                                              "layout; refused with one)."))
        .field(rayShaderList("closesthit").describe("The closest-hit shaders, in index order (required without a "
                                                    "ray group layout; refused with one)."))
        // RTL-1b: the names come from the ray group layout the root names
        // (get_render_authoring_context's ray_layout).
        .field(array("traced_ray_types", string("").choicesFrom(authoring_context, "ray_layout.ray_types"))
                   .describe("The layout's ray types this task traces, each once (required with a ray group "
                             "layout)."))
        .field(object("misses")
                   .keyChoicesFrom(authoring_context, "ray_layout.misses")
                   .item(shader(""))
                   .describe("The layout's miss name to its shader, one for every miss (required with a ray group "
                             "layout)."))
        .field(object("hit_shaders")
                   .keyChoicesFrom(authoring_context, "ray_layout.ray_types")
                   .item(object("")
                             .keyChoicesFrom(authoring_context, "ray_layout.groups", "name")
                             .item(object("")
                                       .field(shader("closesthit").required())
                                       .field(shader("anyhit"))
                                       .field(shader("intersection")
                                                  .describe("Required in a procedural group, refused in a "
                                                            "triangles one."))))
                   .describe("Traced ray type to layout group to its hit shaders, for every traced type and every "
                             "group (required with a ray group layout)."))
        .field(rayShaderList("callable").describe("The callable shaders, in index order."))
        .field(array("callable_record",
                     object("")
                         .field(string("name").required().describe("Matches ^[A-Za-z0-9_]{1,64}$, each once."))
                         .field(enumeration("type", {"i32", "u32", "f32", "vec2", "vec3", "vec4"}).required()))
                   .describe("The callable record's members, in order (with callable and callable_records)."))
        .field(array("callable_records",
                     object("").item(union_("").variant(number("")).variant(array("", number("")))))
                   .describe("One record per callable: each member of callable_record by name, a number or a "
                             "vector's numbers."))
        .field(uint32("max_recursion").range(1.0, uint32_max).fallback(1))
        .field(string("acceleration_structure").fallback("scene").choicesFrom(authoring_context,
                                                                              "acceleration_structures")
                   .describe("The acceleration structure it traces: \"scene\" or a composed "
                             "scene_acceleration_structures name."))
        .describe("A ray tracing pipeline (excludes shader).");
}

// parseDispatch. A ray tracing task takes exactly one of rays, rays_from or
// indirect; a compute task groups or groups_from, or indirect alone
// (local_size is refused). Absent: one group.
P computeDispatch() {
    const auto count = [](std::string name) {
        return uint32(std::move(name)).range(1.0, uint32_max).fallback(1);
    };
    return object("dispatch")
        .field(union_("groups")
                   .fallback(Json::array({1, 1, 1}))
                   .variant(array("", uint32("").range(1.0, uint32_max)))
                   .variant(object("").field(count("x")).field(count("y")).field(count("z")))
                   .describe("Workgroup counts: [x, y, z] or {x, y, z}, each positive (a compute task)."))
        .field(object("groups_from")
                   .field(string("port").required().describe("An image port of this task."))
                   .describe("As many groups as the port's image needs at the shader's workgroup size (a compute "
                             "task; excludes groups)."))
        .field(array("rays", uint32("").range(1.0, uint32_max))
                   .describe("The launch dimensions: three positive integers [x, y, z] (a ray tracing task; "
                             "alone)."))
        .field(object("rays_from")
                   .field(string("port").required().describe("An image port of this task."))
                   .describe("One ray per pixel of the port's image (a ray tracing task; alone)."))
        .field(object("indirect")
                   .field(string("buffer").required()
                              .choicesFromGraph({"render.buffer", "render.attribute"})
                              .where("command_layout", {"compute_dispatch", "ray_dispatch"})
                              .describe("A buffer the task does not write, of command_layout compute_dispatch "
                                        "(a compute task) or ray_dispatch (a ray tracing task)."))
                   .field(integer("offset").range(0.0, std::nullopt).fallback(0)
                              .describe("Where the command starts, in bytes (a multiple of 4)."))
                   .describe("The counts a buffer holds (excludes the others)."))
        .describe("How many groups or rays the task launches.");
}

P wrap() { return enumeration("wrap", {"once", "repeat", "ping_pong"}).fallback("once"); }

// A value.playback / audio.playback time-map number: a literal here, or the
// input edge of the same name.
P mapping(const char *name, std::optional<double> fallback) {
    auto result = number(name);
    if (fallback) result.fallback(*fallback);
    return result.describe("A literal, or the input edge of the same name.");
}

using Table = std::map<std::string, GraphOperationParameters, std::less<>>;

void add(Table &table, const char *op, bool complete, std::initializer_list<P> parameters,
         GraphOperationPlacement placement = GraphOperationPlacement::anywhere) {
    GraphOperationParameters entry;
    entry.complete = complete;
    entry.placement = placement;
    for (const auto &parameter : parameters) entry.parameters.push_back(parameter.schema());
    table.emplace(op, std::move(entry));
}

Table makeTable() {
    Table table;
    // pelican.value (valuegraph.cpp: lowerScalarGraph and Graph::Graph read
    // exactly these keys and refuse others).
    add(table, "value.input", true,
        {string("name").required().describe("A frame input; the catalog's input_names list them.")});
    add(table, "value.constant", true, {number("value").required()});
    add(table, "value.parameter", true,
        {string("name").required().describe("The parameter's name."), number("default").required()});
    // CT-4: it reads its owner's model (valuegraph.cpp), so it stands in an
    // owned instance.
    add(table, "value.morph_weight", true,
        {uint32("node").required().describe("The node index in the owner's model."),
         uint32("target").required().describe("The morph target index.")},
        GraphOperationPlacement::owned_instance);
    add(table, "value.add", true, {});
    add(table, "value.multiply", true, {});
    add(table, "value.curve", true,
        {array("keys", object("")
                           .field(number("time").required())
                           .field(number("value").required())
                           .field(enumeration("interpolation", {"linear", "step", "cubic"}).fallback("linear"))
                           .field(number("in_tangent").fallback(0.0))
                           .field(number("out_tangent").fallback(0.0)))
             .required()
             .describe("The curve's keys.")});
    // AB-1 (valuegraph.cpp; audioanalysisformat.hpp).
    add(table, "value.sample_table", true,
        {asset("asset").required().assetOf("analysis_table", {".ktx2"})
             .describe("The baked table (a KTX2 with its .import.json beside it, e.g. bake_audio_analysis's)."),
         union_("row").required().variant(uint32(""))
             .variant(string("").choicesFromAssetManifest("asset", "source.metadata.layout.features"))
             .describe("The feature row: its index, or its name (\"rms\", \"peak\", \"band:3\").")});
    add(table, "value.playback", true,
        {mapping("source_begin", 0.0), mapping("source_end", 1.0), mapping("duration", 1.0), mapping("speed", 1.0),
         mapping("delay", 0.0), mapping("sample_interval", 0.0), wrap()});
    add(table, "event.playback", true, {});
    add(table, "value.trigger_clock", true,
        {number("paused").fallback(0.0).describe("A literal, or the input edge of the same name."),
         number("reset").fallback(0.0).describe("A literal, or the input edge of the same name.")});
    add(table, "value.event_clock", true, {});

    // pelican.audio (lowerAudioPlayback and the binding lowering).
    add(table, "audio.playback", true,
        {asset("asset").required().assetOf("audio", {".wav", ".ogg"}).describe("The sound asset the voice plays."),
         enumeration("bus", {"se", "bgm"}).fallback("se"), wrap(), mapping("source_begin", 0.0),
         mapping("source_end", std::nullopt).describe("A literal, or the input edge of the same name; absent: the "
                                                      "sound's length."),
         mapping("speed", 1.0), mapping("delay", 0.0)});
    for (const auto *op : {"audio.volume", "audio.pitch"})
        add(table, op, true,
            {string("target").required({"voice"}).choicesFromGraph({"audio.playback"})
                 .describe("The sound it writes, unless a `voice` edge brings one.")});

    // pelican.ui.
    for (const auto *op : {"ui.value", "ui.visible"})
        add(table, op, true, {string("node").required().describe("The UI node's path in the project's UI document.")});

    // pelican.render: the publication sink (lowerScalarGraph).
    add(table, "render.scalar", true,
        {string("target").required().choicesFromGraph({"render.buffer", "render.attribute"})
             .where("host_source", {"runtime_f32"}).where("element", render_scalar_target_elements)
             .describe("The buffer it writes: a runtime_f32 buffer of float elements, as large as the sinks "
                       "writing it."),
         array("value", number("")).describe("A vec2 .. vec4 written as 2 to 4 numbers; a scalar comes in on the "
                                             "value edge."),
         integer("offset").range(0.0, std::nullopt).describe("Where the values land in the buffer, in bytes (a "
                                                              "multiple of 4).")});

    // Buffers and attributes: the graph's own keys (renderPorts, the
    // partition) and the buffer JSON's (parseFrameGraphBufferDefinitionsFromJson).
    add(table, "render.buffer", true,
        {bufferElement(),
         integer("size").range(0.0, std::nullopt).describe("The size in bytes."),
         object("size_from_extent")
             .field(string("resource").required().choicesFrom(authoring_context, "render_targets", "name")
                        .describe("The render target whose extent sizes the buffer."))
             .field(uint32("tile_width").required().range(1.0, uint32_max))
             .field(uint32("tile_height").required().range(1.0, uint32_max))
             .field(integer("header_bytes").required().range(0.0, std::nullopt))
             .field(integer("bytes_per_tile").required().range(1.0, std::nullopt))
             .field(uint32("copies").range(1.0, uint32_max).fallback(1))
             .describe("The size from a render target's extent, in tiles (excludes size)."),
         indexBuffer(), hostSource(), commandLayout(), initialF32(), bufferViewFamily(), bufferLifetime()});
    add(table, "render.attribute", true,
        {bufferElement(),
         enumeration("domain", {"vertex", "instance", "material", "index"}).required(),
         integer("count").required({"source"}).range(1.0, std::nullopt)
             .describe("The element count, unless `source` derives it."),
         object("source")
             .field(asset("asset").required().assetOf("model", {".glb", ".gltf"}))
             .field(uint32("mesh"))
             .field(uint32("primitive"))
             .field(string("semantic").describe("A vertex semantic, e.g. POSITION."))
             .field(uint32("morph_target"))
             .field(uint32("deformed_node"))
             .field(uint32("pose_node"))
             .describe("A model's data the engine fills the attribute with (excludes count and host_source)."),
         indexBuffer(), hostSource(), commandLayout(), initialF32(), bufferViewFamily(), bufferLifetime()});

    // Images (renderPorts, IA-1, PS-1 and parseRenderTargetDefinitionsFromJson).
    // Incomplete: other readers of a render target are not traced here.
    add(table, "render.image", false,
        {imageType().required(),
         object("initial")
             .field(asset("asset").required().assetOf("image", {".ktx2", ".png", ".jpg"}))
             .describe("The asset the image starts from (lifetime persistent)."),
         enumeration("lifetime", {"transient", "persistent"}).fallback("transient")
             .describe("Whether the contents are kept from frame to frame."),
         string("format").choicesFrom(authoring_context, "render_target_vocabulary.formats")
             .describe("A Vulkan format name (the catalog's image formats)."),
         string("format_class").choicesFrom(authoring_context, "render_target_vocabulary.format_classes")
             .describe("scene, display, data or explicit(<format>)."),
         number("extent_scale").expression().describe("The extent as a fraction of the output's."),
         uint32("width").range(1.0, uint32_max).softRange(std::nullopt, image_extent_soft_max).expression(),
         uint32("height").range(1.0, uint32_max).softRange(std::nullopt, image_extent_soft_max).expression(),
         uint32("layers").range(1.0, uint32_max).expression(),
         union_("mip_levels")
             .variant(uint32("").range(1.0, uint32_max).softRange(std::nullopt, image_mip_levels_soft_max))
             .variant(enumeration("", {"full"}))
             .expression()
             .describe("A positive integer, or \"full\"."),
         enumeration("dimension", {"2d", "cube"}).fallback("2d"),
         array("usage", string("").choicesFrom(authoring_context, "render_target_vocabulary.usage"))
             .describe("Image usage names, e.g. SAMPLED."),
         boolean("history").fallback(false),
         clearColor(),
         enumeration("role", {"color", "data"}),
         array("format_candidates", string("").choicesFrom(authoring_context, "render_target_vocabulary.formats"))
             .describe("Formats to try after format.")});
    add(table, "render.target_ref", true,
        {string("name").required().choicesFrom(authoring_context, "render_targets", "name")
             .describe("A target declared outside the graph."),
         imageType().required()});

    // LP-0e: the closed key sets renderPassKindPorts admits, typed as the
    // pass JSON reads them.
    add(table, "render.shadow_depth", true,
        {shader("shader")
             .field(string("vertex").required())
             .fallback(Json{{"vertex", "engine://shadow_depth"}})
             .describe("The depth pass's vertex shader (no fragment stage)."),
         rasterRayQuery(), loadOp("depth_load_op"), storeOp("depth_store_op"), resolutionDomain("independent"),
         viewFamily(), viewSelect(), regionTags(), passSet(), order("after"), order("before"), insert(false),
         canonicalAnchor()});
    add(table, "render.snapshot_copy", true,
        {nodeRef("snapshot_after").required().describe("The pass the copy runs after."), viewFamily(), viewSelect(),
         regionTags(), passSet(), order("after"), order("before"), insert(false), canonicalAnchor()});

    // Passes. Incomplete: the pass JSON parsers read more than is traced here
    // (raster_state, the draw source).
    std::vector<std::string> contracts;
    for (const auto contract : materialPassContracts) contracts.emplace_back(materialPassContractName(contract));
    {
        P material_contract{"material_contract", Kind::enumeration};
        material_contract.values(std::move(contracts)).required();
        add(table, "render.material_pass", false,
            {material_contract, json("material_filter"), json("material_variant"), json("material_range"),
             json("gpu_draw_source"), json("material_outputs"), json("material_output_states"),
             json("screen_inputs"), json("surface_resources"), json("material_resources"), rasterRayQuery(),
             loadOp("color_load_op"), storeOp("color_store_op"), loadOp("depth_load_op"), storeOp("depth_store_op"),
             clearColor(), clearColors(), viewFamily(), viewSelect(), regionTags(), regions(),
             resolutionDomain("scene"), inheritBindingsFrom(),
             passSet(), order("after"), order("before"), insert(true), canonicalAnchor()});
    }
    add(table, "render.fullscreen", false,
        {object("ports").required().describe("Port name to its declaration."),
         shader("shader")
             .field(string("vertex").required())
             .field(string("fragment").required())
             .field(shaderDefines())
             .required(),
         passSchedule(),
         // CT-5: stringToFullscreenPushConstantData's names.
         enumeration("push_constants", {"none", "camera_position", "projection_view"}).fallback("none"),
         boolean("uses_light_data").fallback(false), json("raster_state"),
         rasterRayQuery(), loadOp("color_load_op"), storeOp("color_store_op"), loadOp("depth_load_op"),
         storeOp("depth_store_op"), clearColor(), clearColors(), resolutionDomain("unclassified"),
         inheritBindingsFrom(), viewFamily(), viewSelect(),
         regionTags(), regions(), passSet(), order("after"), order("before"), insert(true), canonicalAnchor()});
    add(table, "render.raster", false,
        {object("ports").required().describe("Port name to its declaration."),
         shader("shader")
             .field(string("vertex").required())
             .field(string("fragment"))
             .field(string("implementation"))
             .field(shaderDefines())
             .required(),
         object("draw").required().describe("What the pass draws."), json("raster_state"),
         // CC-2: parseRasterPassContract's clip_y.
         enumeration("clip_y", {"down", "up"}).fallback("down")
             .describe("Which way the vertex shader's clip +y points on the image (down: the engine's). "
                       "front_face and cull mean the winding seen on the image either way."),
         passSchedule(), rasterRayQuery(), loadOp("color_load_op"), storeOp("color_store_op"),
         loadOp("depth_load_op"), storeOp("depth_store_op"), clearColor(), clearColors(), resolutionDomain("unclassified"), viewFamily(),
         viewSelect(), regionTags(), regions(), passSet(), order("after"), order("before"), insert(true),
         canonicalAnchor()});
    // CT-6: ray_tracing and dispatch typed, every key the task JSON parser
    // and the composition read is listed.
    add(table, "render.compute", true,
        {object("ports").required().describe("Port name to its declaration."),
         shader("shader").required({"ray_tracing"}).describe("The compute shader's path."),
         rayTracing(),
         object("ray_query").field(accelerationStructure()),
         computeDispatch(),
         enumeration("schedule", {"per_frame", "per_view", "once"}).fallback("per_frame")
             .describe("When the task runs."),
         viewFamily(), viewSelect(), regionTags(), regions(), order("after"), order("before")});

    // LP-3a: draw steps (checkDrawStepParameters).
    add(table, "draw.material", true, {});
    add(table, "draw.variant", true,
        {string("variant").required().choicesFrom(authoring_context, "materials", "variants")
             .describe("The material's variant to draw.")});
    add(table, "draw.override", true,
        {object("render_state")
             .field(enumeration("blend", {"opaque", "blend", "additive"}))
             .field(enumeration("cull", {"none", "front", "back"}))
             .field(enumeration("depth_compare", {"never", "less", "equal", "less_equal", "greater", "not_equal",
                                                  "greater_equal", "always"}))
             .field(boolean("depth_test").fallback(true))
             .field(boolean("depth_write")),
         array("defines", string("")).describe("Defines added to the material's shader.")});

    // pelican.pose (posegraphmodule.cpp, posegraph.cpp). CT-4: a pose writes
    // its owner's skeletal sink, so every pose op stands in an owned instance.
    constexpr auto owned = GraphOperationPlacement::owned_instance;
    add(table, "pose.clip_sample", true,
        {sceneModel("rig").required().describe("The model whose rig the pose is for."),
         string("clip").required().describe("The animation clip's name."), number("speed").fallback(1.0),
         number("start_time").fallback(0.0), boolean("loop").fallback(true),
         enumeration("root_motion", {"none", "extract"}).fallback("none"),
         uint32("root_node").checkedByOp().describe("The node root motion is extracted from (root_motion "
                                                    "extract).")},
        owned);
    add(table, "pose.blend", true, {}, owned);
    add(table, "pose.state_machine", true,
        {sceneModel("rig").required(), json("graph").required().describe("A pelican.anim_graph v1 document.")},
        owned);
    add(table, "pose.retarget", true, {string("profile").required()}, owned);
    add(table, "pose.skeletal", true, {}, owned);
    add(table, "pose.transform", true, {}, owned);
    return table;
}

const Table &table() {
    static const Table instance = makeTable();
    return instance;
}

// --- The check. ---

struct Failure {
    std::string parameter;
    const GraphParameterSchema *schema;
    std::string reason;
};

std::optional<Failure> checkValue(const Json &value, const GraphParameterSchema &schema, const std::string &path);

std::optional<Failure> checkRange(const Json &value, const GraphParameterSchema &schema, const std::string &path) {
    if (!schema.min && !schema.max) return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto number = static_cast<double>(value.get<std::uint64_t>());
        if ((schema.min && number < *schema.min) || (schema.max && number > *schema.max))
            return Failure{path, &schema, "range"};
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if ((schema.min && static_cast<double>(number) < *schema.min) ||
            (schema.max && static_cast<double>(number) > *schema.max))
            return Failure{path, &schema, "range"};
        return std::nullopt;
    }
    const auto number = value.get<double>();
    if ((schema.min && number < *schema.min) || (schema.max && number > *schema.max))
        return Failure{path, &schema, "range"};
    return std::nullopt;
}

std::optional<Failure> checkNumbers(const Json &value, std::size_t count, const GraphParameterSchema &schema,
                                    const std::string &path) {
    if (!value.is_array() || value.size() != count) return Failure{path, &schema, "type"};
    for (const auto &component : value)
        if (!component.is_number() && !isGraphLoopExpression(component)) return Failure{path, &schema, "type"};
    return std::nullopt;
}

std::optional<Failure> checkValue(const Json &value, const GraphParameterSchema &schema, const std::string &path) {
    if (schema.checked_by_op || value.is_null() || isGraphLoopExpression(value)) return std::nullopt;
    const auto type = [&] { return std::optional<Failure>{Failure{path, &schema, "type"}}; };
    switch (schema.kind) {
    case Kind::number:
        if (!value.is_number()) return type();
        return checkRange(value, schema, path);
    case Kind::integer:
        if (!value.is_number_integer()) return type();
        return checkRange(value, schema, path);
    case Kind::boolean:
        if (!value.is_boolean()) return type();
        return std::nullopt;
    case Kind::string:
    case Kind::asset:
    case Kind::node_ref:
        if (!value.is_string()) return type();
        return std::nullopt;
    case Kind::enumeration:
        if (!value.is_string()) return type();
        if (std::find(schema.enum_values.begin(), schema.enum_values.end(), value.get_ref<const std::string &>()) ==
            schema.enum_values.end())
            return Failure{path, &schema, "enum"};
        return std::nullopt;
    case Kind::vec2: return checkNumbers(value, 2, schema, path);
    case Kind::vec3: return checkNumbers(value, 3, schema, path);
    case Kind::vec4:
    case Kind::color: return checkNumbers(value, 4, schema, path);
    case Kind::array:
        if (!value.is_array()) return type();
        if (schema.items)
            for (std::size_t index = 0; index < value.size(); ++index)
                if (auto failure = checkValue(value.at(index), *schema.items, path + "[" + std::to_string(index) + "]"))
                    return failure;
        return std::nullopt;
    case Kind::shader:
        // With fields: an object of stages. Without: one shader's path.
        if (schema.fields.empty()) {
            if (value.is_string()) return std::nullopt;
            return type();
        }
        [[fallthrough]];
    case Kind::object:
        if (!value.is_object()) return type();
        for (const auto &field : schema.fields)
            if (const auto found = value.find(field.name); found != value.end())
                if (auto failure = checkValue(*found, field, path + "." + field.name)) return failure;
        if (schema.items)
            for (const auto &[key, member] : value.items())
                if (auto failure = checkValue(member, *schema.items, path + "." + key)) return failure;
        return std::nullopt;
    case Kind::expression:
    case Kind::json:
    case Kind::union_:
        return std::nullopt;
    }
    return std::nullopt;
}

// Integral bounds print as integers.
OrderedJson bound(double value) {
    if (value == static_cast<double>(static_cast<std::int64_t>(value))) return static_cast<std::int64_t>(value);
    return value;
}

// The catalog form of where a value (choices_from) or a key
// (key_choices_from) is chosen from.
OrderedJson choicesJson(const GraphParameterChoices &choices) {
    OrderedJson from;
    if (!choices.graph_ops.empty()) {
        from = OrderedJson{{"graph_nodes", choices.graph_ops}};
    } else if (!choices.asset_parameter.empty()) {
        // CT-5: derivedAssetManifestPath: the manifest beside the asset.
        from = OrderedJson{{"asset_manifest", {{"parameter", choices.asset_parameter},
                                               {"manifest", "<asset>.import.json"},
                                               {"key", choices.key}}}};
    } else {
        from = OrderedJson{{"rpc", choices.rpc}, {"key", choices.key}};
        if (!choices.field.empty()) from["field"] = choices.field;
    }
    // CT-5: one value as a string, several as an array (one of them).
    if (!choices.where.empty()) {
        auto where = OrderedJson::object();
        for (const auto &[name, values] : choices.where) {
            if (values.size() == 1) where[name] = values.front();
            else where[name] = values;
        }
        from["where"] = std::move(where);
    }
    return from;
}

OrderedJson itemJson(const GraphParameterSchema &schema) {
    auto result = graphParameterSchemaJson(schema);
    result.erase("name");
    return result;
}

} // namespace

std::string_view graphParameterKindName(GraphParameterKind kind) {
    switch (kind) {
    case Kind::number: return "number";
    case Kind::integer: return "integer";
    case Kind::boolean: return "bool";
    case Kind::string: return "string";
    case Kind::enumeration: return "enum";
    case Kind::vec2: return "vec2";
    case Kind::vec3: return "vec3";
    case Kind::vec4: return "vec4";
    case Kind::color: return "color";
    case Kind::array: return "array";
    case Kind::object: return "object";
    case Kind::asset: return "asset";
    case Kind::shader: return "shader";
    case Kind::node_ref: return "node_ref";
    case Kind::expression: return "expression";
    case Kind::json: return "json";
    case Kind::union_: return "union";
    }
    return "json";
}

std::string_view graphOperationPlacementName(GraphOperationPlacement placement) {
    switch (placement) {
    case GraphOperationPlacement::anywhere: return "anywhere";
    case GraphOperationPlacement::owned_instance: return "owned_instance";
    }
    return "anywhere";
}

const GraphOperationParameters *graphOperationParameters(std::string_view op) {
    const auto found = table().find(op);
    return found == table().end() ? nullptr : &found->second;
}

GraphOperationPlacement graphOperationPlacement(std::string_view op) {
    const auto *entry = graphOperationParameters(op);
    return entry == nullptr ? GraphOperationPlacement::anywhere : entry->placement;
}

void checkGraphOperationPlacement(const std::string &op, const std::string &node, bool owned) {
    if (owned || graphOperationPlacement(op) != GraphOperationPlacement::owned_instance) return;
    throw GraphRegistryError("graph.op_requires_owned_instance@1", {{"node", node}, {"op", op}});
}

nlohmann::ordered_json graphParameterSchemaJson(const GraphParameterSchema &parameter) {
    OrderedJson result{{"name", parameter.name},
                       {"kind", std::string{graphParameterKindName(parameter.kind)}}};
    if (parameter.required) result["required"] = true;
    if (!parameter.alternatives.empty()) result["alternatives"] = parameter.alternatives;
    if (parameter.default_value) result["default"] = OrderedJson::parse(parameter.default_value->dump());
    if (!parameter.enum_values.empty()) result["enum"] = parameter.enum_values;
    if (parameter.min) result["min"] = bound(*parameter.min);
    if (parameter.max) result["max"] = bound(*parameter.max);
    if (parameter.soft_min) result["soft_min"] = bound(*parameter.soft_min);
    if (parameter.soft_max) result["soft_max"] = bound(*parameter.soft_max);
    if (!parameter.asset_kind.empty()) result["asset_kind"] = parameter.asset_kind;
    if (!parameter.extensions.empty()) result["extensions"] = parameter.extensions;
    if (const auto &choices = parameter.choices_from) result["choices_from"] = choicesJson(*choices);
    // RTL-1b: a map object's keys, in choices_from's form.
    if (const auto &choices = parameter.key_choices_from) result["key_choices_from"] = choicesJson(*choices);
    if (parameter.items) result["items"] = itemJson(*parameter.items);
    if (!parameter.fields.empty()) {
        auto fields = OrderedJson::array();
        for (const auto &field : parameter.fields) fields.push_back(graphParameterSchemaJson(field));
        result["fields"] = std::move(fields);
    }
    if (!parameter.variants.empty()) {
        auto variants = OrderedJson::array();
        for (const auto &variant : parameter.variants) variants.push_back(itemJson(variant));
        result["variants"] = std::move(variants);
    }
    if (parameter.expression) result["expression"] = true;
    if (!parameter.description.empty()) result["description"] = parameter.description;
    return result;
}

std::vector<std::vector<std::string>> graphOperationRequiredParameters(std::string_view op) {
    std::vector<std::vector<std::string>> required;
    const auto *entry = graphOperationParameters(op);
    if (entry == nullptr) return required;
    for (const auto &parameter : entry->parameters) {
        if (!parameter.required) continue;
        std::vector<std::string> names{parameter.name};
        names.insert(names.end(), parameter.alternatives.begin(), parameter.alternatives.end());
        required.push_back(std::move(names));
    }
    return required;
}

void checkGraphNodeParameterTypes(const std::string &op, const nlohmann::json &node) {
    const auto *entry = graphOperationParameters(op);
    if (entry == nullptr) return;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return;
    for (const auto &schema : entry->parameters) {
        const auto found = parameters->find(schema.name);
        if (found == parameters->end()) continue;
        const auto failure = checkValue(*found, schema, schema.name);
        if (!failure) continue;
        const auto id = node.find("id");
        OrderedJson detail{{"node", id != node.end() && id->is_string() ? id->get<std::string>() : std::string{}},
                    {"op", op},
                    {"parameter", failure->parameter},
                    {"expected", std::string{graphParameterKindName(failure->schema->kind)}},
                    {"reason", failure->reason}};
        if (failure->reason == "enum") detail["allowed"] = failure->schema->enum_values;
        if (failure->reason == "range") {
            if (failure->schema->min) detail["min"] = bound(*failure->schema->min);
            if (failure->schema->max) detail["max"] = bound(*failure->schema->max);
        }
        throw GraphRegistryError("graph.render_node_parameter_invalid@1", Json::parse(detail.dump()));
    }
}

} // namespace Pelican
