#include "rasterpass.hpp"

#include "logicalrendertype.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace Pelican {
namespace {

void requireObject(
    const nlohmann::json &value,
    std::string_view context) {
    if (!value.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
}

void requireOnlyFields(
    const nlohmann::json &object,
    std::span<const std::string_view> allowed,
    std::string_view context) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        if (std::find(
                allowed.begin(), allowed.end(),
                field.key()) == allowed.end()) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown field '" + field.key() + "'");
        }
    }
}

void requireOnlyFields(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    requireOnlyFields(
        object,
        std::span<const std::string_view>{
            allowed.begin(), allowed.size()},
        context);
}

std::string requireString(
    const nlohmann::json &object,
    std::string_view field,
    std::string_view context) {
    const auto found =
        object.find(std::string{field});
    if (found == object.end() ||
        !found->is_string() ||
        found->get<std::string>().empty()) {
        throw std::runtime_error(
            std::string{context} +
            " requires non-empty string '" +
            std::string{field} + "'");
    }
    return found->get<std::string>();
}

void requireVersionedId(
    std::string_view value,
    std::string_view context) {
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error(
                "identifier is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(
            std::string{context} +
            " must use canonical namespace.name@major syntax: " +
            std::string{value} + " (" + error.what() + ")");
    }
}

std::uint32_t optionalUint32(
    const nlohmann::json &object,
    std::string_view field,
    std::uint32_t fallback,
    std::string_view context) {
    const auto found =
        object.find(std::string{field});
    if (found == object.end()) return fallback;
    if (!found->is_number_integer()) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} +
            "' must be an unsigned integer");
    }
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value =
            found->get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(
                std::string{context} + " field '" +
                std::string{field} +
                "' must be an unsigned integer");
        }
        value =
            static_cast<std::uint64_t>(
                signed_value);
    }
    if (value >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} +
            "' exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

bool optionalBool(
    const nlohmann::json &object,
    std::string_view field, bool fallback,
    std::string_view context) {
    const auto found =
        object.find(std::string{field});
    if (found == object.end()) return fallback;
    if (!found->is_boolean()) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} +
            "' must be a boolean");
    }
    return found->get<bool>();
}

// PSN-1: a JSON number; NaN and infinity are refused by name.
double requireFiniteNumber(
    const nlohmann::json &value,
    std::string_view field,
    std::string_view context) {
    if (!value.is_number()) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} + "' must be a number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number)) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} + "' must be finite");
    }
    return number;
}

template <typename Enum, typename Parser>
Enum optionalEnum(
    const nlohmann::json &object,
    std::string_view field, Enum fallback,
    std::string_view context, Parser parser) {
    const auto found =
        object.find(std::string{field});
    if (found == object.end()) return fallback;
    if (!found->is_string()) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} +
            "' must be a string");
    }
    const auto encoded = found->get<std::string>();
    const auto parsed = parser(encoded);
    if (!parsed) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} + "' has unknown value '" +
            encoded + "'");
    }
    return *parsed;
}

template <typename Enum>
std::optional<Enum> findName(
    std::string_view name,
    std::initializer_list<
        std::pair<std::string_view, Enum>> entries) {
    const auto found = std::find_if(
        entries.begin(), entries.end(),
        [&](const auto &entry) {
            return entry.first == name;
        });
    if (found == entries.end()) return std::nullopt;
    return found->second;
}

std::optional<RasterDepthCompare> compareFromName(
    std::string_view value) {
    return findName<RasterDepthCompare>(
        value,
        {{"never", RasterDepthCompare::never},
         {"less", RasterDepthCompare::less},
         {"equal", RasterDepthCompare::equal},
         {"less_equal", RasterDepthCompare::less_equal},
         {"greater", RasterDepthCompare::greater},
         {"not_equal", RasterDepthCompare::not_equal},
         {"greater_equal",
          RasterDepthCompare::greater_equal},
         {"always", RasterDepthCompare::always}});
}

std::optional<RasterStencilOperation> stencilOperationFromName(
    std::string_view value) {
    return findName<RasterStencilOperation>(
        value,
        {{"keep", RasterStencilOperation::keep},
         {"zero", RasterStencilOperation::zero},
         {"replace", RasterStencilOperation::replace},
         {"increment_and_clamp",
          RasterStencilOperation::increment_and_clamp},
         {"decrement_and_clamp",
          RasterStencilOperation::decrement_and_clamp},
         {"invert", RasterStencilOperation::invert},
         {"increment_and_wrap",
          RasterStencilOperation::increment_and_wrap},
         {"decrement_and_wrap",
          RasterStencilOperation::decrement_and_wrap}});
}

std::optional<RasterLogicOperation> logicOperationFromName(
    std::string_view value) {
    return findName<RasterLogicOperation>(
        value,
        {{"clear", RasterLogicOperation::clear},
         {"and", RasterLogicOperation::bitwise_and},
         {"and_reverse", RasterLogicOperation::and_reverse},
         {"copy", RasterLogicOperation::copy},
         {"and_inverted", RasterLogicOperation::and_inverted},
         {"no_op", RasterLogicOperation::no_op},
         {"xor", RasterLogicOperation::bitwise_xor},
         {"or", RasterLogicOperation::bitwise_or},
         {"nor", RasterLogicOperation::nor},
         {"equivalent", RasterLogicOperation::equivalent},
         {"invert", RasterLogicOperation::invert},
         {"or_reverse", RasterLogicOperation::or_reverse},
         {"copy_inverted", RasterLogicOperation::copy_inverted},
         {"or_inverted", RasterLogicOperation::or_inverted},
         {"nand", RasterLogicOperation::nand},
         {"set", RasterLogicOperation::set}});
}

// PSN-1: the vocabulary of raster_state, in the order of Vulkan's state
// groups (design_pipeline_state_nodes.md §2): input assembly,
// rasterization, multisample, depth / stencil, color blend.
constexpr std::array<std::string_view, 22> pipeline_state_keys{
    "topology", "primitive_restart",
    "cull", "front_face", "polygon_mode", "depth_clamp",
    "depth_bias", "line_width", "rasterizer_discard",
    "conservative",
    "sample_shading", "sample_mask", "alpha_to_coverage",
    "alpha_to_one",
    "depth_test", "depth_write", "depth_compare",
    "depth_bounds", "stencil",
    "logic_op", "blend_constants", "color_attachments",
};

// What a surface's render_state takes from it. The surface spells blend,
// cull and depth its own way; a material draws triangle lists from meshes
// (no topology or primitive restart) and states its attachments in the
// pass's material_output_states.
constexpr std::array<std::string_view, 14>
    surface_pipeline_state_keys{
        "polygon_mode", "depth_clamp", "depth_bias",
        "line_width", "rasterizer_discard", "conservative",
        "sample_shading", "sample_mask", "alpha_to_coverage",
        "alpha_to_one",
        "depth_bounds", "stencil",
        "logic_op", "blend_constants",
    };

std::uint32_t optionalStencilValue(
    const nlohmann::json &face,
    std::string_view field, std::uint32_t fallback,
    std::string_view context) {
    const auto value =
        optionalUint32(face, field, fallback, context);
    if (value > rasterStencilValueMax) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} + "' must be at most " +
            std::to_string(rasterStencilValueMax));
    }
    return value;
}

RasterStencilFaceState parseStencilFace(
    const nlohmann::json &face,
    const std::string &context) {
    requireObject(face, context);
    requireOnlyFields(
        face,
        {"fail", "pass", "depth_fail", "compare",
         "compare_mask", "write_mask", "reference"},
        context);
    RasterStencilFaceState result;
    result.fail = optionalEnum(
        face, "fail", result.fail, context,
        stencilOperationFromName);
    result.pass = optionalEnum(
        face, "pass", result.pass, context,
        stencilOperationFromName);
    result.depth_fail = optionalEnum(
        face, "depth_fail", result.depth_fail, context,
        stencilOperationFromName);
    result.compare = optionalEnum(
        face, "compare", result.compare, context,
        compareFromName);
    result.compare_mask = optionalStencilValue(
        face, "compare_mask", result.compare_mask, context);
    result.write_mask = optionalStencilValue(
        face, "write_mask", result.write_mask, context);
    result.reference = optionalStencilValue(
        face, "reference", result.reference, context);
    return result;
}

std::vector<double> requireNumberArray(
    const nlohmann::json &object, std::string_view field,
    std::size_t count, std::string_view context) {
    const auto &encoded = object.at(std::string{field});
    if (!encoded.is_array() || encoded.size() != count) {
        throw std::runtime_error(
            std::string{context} + " field '" +
            std::string{field} + "' must be an array of " +
            std::to_string(count) + " numbers");
    }
    std::vector<double> result;
    result.reserve(count);
    for (const auto &value : encoded) {
        result.push_back(
            requireFiniteNumber(value, field, context));
    }
    return result;
}

// The one reader. Every key is optional; only the ones present change
// `result`. Callers check the key set first (requireOnlyFields).
void readPipelineStateFields(
    const nlohmann::json &object,
    PipelineState &result,
    std::size_t color_attachment_count,
    const std::string &context) {
    // input assembly
    result.topology = optionalEnum(
        object, "topology", result.topology, context,
        [](std::string_view value) {
            return findName<RasterPrimitiveTopology>(
                value,
                {{"point_list",
                  RasterPrimitiveTopology::point_list},
                 {"line_list",
                  RasterPrimitiveTopology::line_list},
                 {"line_strip",
                  RasterPrimitiveTopology::line_strip},
                 {"triangle_list",
                  RasterPrimitiveTopology::triangle_list},
                 {"triangle_strip",
                  RasterPrimitiveTopology::triangle_strip}});
        });
    result.primitive_restart = optionalBool(
        object, "primitive_restart",
        result.primitive_restart, context);

    // rasterization
    result.cull = optionalEnum(
        object, "cull", result.cull, context,
        [](std::string_view value) {
            return findName<RasterCullMode>(
                value,
                {{"none", RasterCullMode::none},
                 {"front", RasterCullMode::front},
                 {"back", RasterCullMode::back}});
        });
    result.front_face = optionalEnum(
        object, "front_face", result.front_face, context,
        [](std::string_view value) {
            return findName<RasterFrontFace>(
                value,
                {{"counter_clockwise",
                  RasterFrontFace::counter_clockwise},
                 {"clockwise",
                  RasterFrontFace::clockwise}});
        });
    result.polygon_mode = optionalEnum(
        object, "polygon_mode", result.polygon_mode, context,
        [](std::string_view value) {
            return findName<RasterPolygonMode>(
                value,
                {{"fill", RasterPolygonMode::fill},
                 {"line", RasterPolygonMode::line},
                 {"point", RasterPolygonMode::point}});
        });
    result.depth_clamp = optionalBool(
        object, "depth_clamp", result.depth_clamp, context);
    if (const auto found = object.find("depth_bias");
        found != object.end()) {
        const auto bias_context = context + " depth_bias";
        requireObject(*found, bias_context);
        requireOnlyFields(
            *found, {"constant", "slope", "clamp"},
            bias_context);
        RasterDepthBias bias;
        for (const auto &[field, target] :
             {std::pair{"constant", &bias.constant},
              std::pair{"slope", &bias.slope},
              std::pair{"clamp", &bias.clamp}}) {
            if (const auto value = found->find(field);
                value != found->end()) {
                *target = requireFiniteNumber(
                    *value, field, bias_context);
            }
        }
        result.depth_bias = bias;
    }
    if (const auto found = object.find("line_width");
        found != object.end()) {
        result.line_width = requireFiniteNumber(
            *found, "line_width", context);
    }
    result.rasterizer_discard = optionalBool(
        object, "rasterizer_discard",
        result.rasterizer_discard, context);
    result.conservative = optionalEnum(
        object, "conservative", result.conservative, context,
        [](std::string_view value) {
            return findName<RasterConservativeMode>(
                value,
                {{"off", RasterConservativeMode::off},
                 {"overestimate",
                  RasterConservativeMode::overestimate},
                 {"underestimate",
                  RasterConservativeMode::underestimate}});
        });

    // multisample
    if (const auto found = object.find("sample_shading");
        found != object.end()) {
        result.sample_shading = requireFiniteNumber(
            *found, "sample_shading", context);
    }
    result.sample_mask = optionalUint32(
        object, "sample_mask", result.sample_mask, context);
    result.alpha_to_coverage = optionalBool(
        object, "alpha_to_coverage",
        result.alpha_to_coverage, context);
    result.alpha_to_one = optionalBool(
        object, "alpha_to_one", result.alpha_to_one,
        context);

    // depth / stencil
    result.depth.test = optionalBool(
        object, "depth_test", result.depth.test, context);
    result.depth.write = optionalBool(
        object, "depth_write", result.depth.write, context);
    result.depth.compare = optionalEnum(
        object, "depth_compare", result.depth.compare,
        context, compareFromName);
    if (object.contains("depth_bounds")) {
        const auto bounds = requireNumberArray(
            object, "depth_bounds", 2, context);
        result.depth.bounds =
            RasterDepthBounds{bounds[0], bounds[1]};
    }
    if (const auto found = object.find("stencil");
        found != object.end()) {
        const auto stencil_context = context + " stencil";
        requireObject(*found, stencil_context);
        requireOnlyFields(
            *found, {"front", "back"}, stencil_context);
        // Both faces, always: a triangle whose winding is not the one the
        // author pictured must not skip the test silently.
        if (!found->contains("front") ||
            !found->contains("back")) {
            throw std::runtime_error(
                stencil_context +
                " requires both front and back");
        }
        result.stencil = RasterStencilState{
            .front = parseStencilFace(
                found->at("front"),
                stencil_context + " front"),
            .back = parseStencilFace(
                found->at("back"),
                stencil_context + " back"),
        };
    }

    // color blend
    if (object.contains("logic_op")) {
        result.logic_op = optionalEnum(
            object, "logic_op", RasterLogicOperation::copy,
            context, logicOperationFromName);
    }
    if (object.contains("blend_constants")) {
        const auto constants = requireNumberArray(
            object, "blend_constants", 4, context);
        std::copy(
            constants.begin(), constants.end(),
            result.blend_constants.begin());
    }
    const auto colors = object.find("color_attachments");
    if (colors == object.end()) return;
    if (!colors->is_array()) {
        throw std::runtime_error(
            context +
            " color_attachments must be an array");
    }
    if (colors->size() != color_attachment_count) {
        throw std::runtime_error(
            context +
            " color_attachments count must match pass color outputs");
    }
    for (std::size_t index = 0;
         index < colors->size(); ++index) {
        const auto &encoded = colors->at(index);
        const auto color_context =
            context + " color_attachments[" +
            std::to_string(index) + "]";
        requireObject(encoded, color_context);
        requireOnlyFields(
            encoded, {"blend", "write_mask"},
            color_context);
        if (encoded.contains("blend")) {
            result.color_attachments[index].blend =
                parseMaterialOutputBlendState(
                    encoded.at("blend"),
                    color_context + " blend");
        }
        if (encoded.contains("write_mask")) {
            result.color_attachments[index].write_mask =
                parseMaterialOutputWriteMask(
                    encoded.at("write_mask"),
                    color_context + " write_mask");
        }
    }
}

void validateStencilFace(
    const RasterStencilFaceState &face,
    const std::string &context) {
    for (const auto &[field, value] :
         {std::pair{"compare_mask", face.compare_mask},
          std::pair{"write_mask", face.write_mask},
          std::pair{"reference", face.reference}}) {
        if (value > rasterStencilValueMax) {
            throw std::runtime_error(
                context + " field '" + field +
                "' must be at most " +
                std::to_string(rasterStencilValueMax));
        }
    }
}

// Ranges and combinations: what the value alone can be refused for.
void validatePipelineStateValues(
    const PipelineState &state,
    std::string_view context) {
    const std::string prefix{context};
    if (state.primitive_restart &&
        state.topology !=
            RasterPrimitiveTopology::line_strip &&
        state.topology !=
            RasterPrimitiveTopology::triangle_strip) {
        throw std::runtime_error(
            prefix +
            " field 'primitive_restart' requires topology "
            "line_strip or triangle_strip");
    }
    if (state.depth_bias &&
        (!std::isfinite(state.depth_bias->constant) ||
         !std::isfinite(state.depth_bias->slope) ||
         !std::isfinite(state.depth_bias->clamp))) {
        throw std::runtime_error(
            prefix +
            " field 'depth_bias' must hold finite numbers");
    }
    if (!std::isfinite(state.line_width) ||
        state.line_width <= 0.0) {
        throw std::runtime_error(
            prefix +
            " field 'line_width' must be greater than 0");
    }
    if (state.sample_shading &&
        (!std::isfinite(*state.sample_shading) ||
         *state.sample_shading < 0.0 ||
         *state.sample_shading > 1.0)) {
        throw std::runtime_error(
            prefix +
            " field 'sample_shading' must be in [0, 1]");
    }
    if (state.depth.bounds) {
        const auto &bounds = *state.depth.bounds;
        if (!std::isfinite(bounds.min) ||
            !std::isfinite(bounds.max) ||
            bounds.min < 0.0 || bounds.max > 1.0 ||
            bounds.min > bounds.max) {
            throw std::runtime_error(
                prefix +
                " field 'depth_bounds' must be [min, max] with "
                "0 <= min <= max <= 1");
        }
    }
    if (state.stencil) {
        validateStencilFace(
            state.stencil->front, prefix + " stencil front");
        validateStencilFace(
            state.stencil->back, prefix + " stencil back");
    }
    if (!std::all_of(
            state.blend_constants.begin(),
            state.blend_constants.end(),
            [](double value) { return std::isfinite(value); })) {
        throw std::runtime_error(
            prefix +
            " field 'blend_constants' must hold finite numbers");
    }
    if (state.logic_op) {
        for (std::size_t index = 0;
             index < state.color_attachments.size();
             ++index) {
            if (state.color_attachments[index]
                    .blend.enabled) {
                throw std::runtime_error(
                    prefix +
                    " field 'logic_op' replaces blending, but "
                    "color_attachments[" +
                    std::to_string(index) +
                    "] enables blend");
            }
        }
    }
}

nlohmann::json stencilFaceToJson(
    const RasterStencilFaceState &face) {
    return nlohmann::json{
        {"fail", rasterStencilOperationName(face.fail)},
        {"pass", rasterStencilOperationName(face.pass)},
        {"depth_fail",
         rasterStencilOperationName(face.depth_fail)},
        {"compare", rasterDepthCompareName(face.compare)},
        {"compare_mask", face.compare_mask},
        {"write_mask", face.write_mask},
        {"reference", face.reference},
    };
}

RasterGeometryContract parseGeometry(
    const nlohmann::json &pass,
    std::string_view context) {
    const auto found = pass.find("draw");
    if (found == pass.end()) {
        throw std::runtime_error(
            std::string{context} +
            " requires a draw object");
    }
    const auto draw_context =
        std::string{context} + " draw";
    requireObject(*found, draw_context);
    RasterGeometryContract result;
    if (found->contains("implementation")) {
        result.implementation =
            requireString(
                *found, "implementation",
                draw_context);
    }
    const auto operation =
        found->contains("operation")
            ? requireString(
                  *found, "operation",
                  draw_context)
            : std::string{"direct"};
    if (operation == "indexed") {
        requireOnlyFields(*found, {"implementation", "operation", "index_buffer", "index_count",
            "instance_count", "first_index", "first_instance"}, draw_context);
        result.operation = RasterIndexedDrawOperation{
            .index_buffer = requireString(*found, "index_buffer", draw_context),
            .index_count = optionalUint32(*found, "index_count", 0, draw_context),
            .instance_count = optionalUint32(*found, "instance_count", 1, draw_context),
            .first_index = optionalUint32(*found, "first_index", 0, draw_context),
            .first_instance = optionalUint32(*found, "first_instance", 0, draw_context)};
        return result;
    }
    if (operation != "direct") {
        throw std::runtime_error(
            draw_context +
            " has unsupported typed operation '" +
            operation + "'");
    }
    requireOnlyFields(*found, {"implementation", "operation", "vertex_count",
        "instance_count", "first_vertex", "first_instance"}, draw_context);
    result.operation = RasterDirectDrawOperation{
        .vertex_count =
            optionalUint32(
                *found, "vertex_count", 3,
                draw_context),
        .instance_count =
            optionalUint32(
                *found, "instance_count", 1,
                draw_context),
        .first_vertex =
            optionalUint32(
                *found, "first_vertex", 0,
                draw_context),
        .first_instance =
            optionalUint32(
                *found, "first_instance", 0,
                draw_context),
    };
    return result;
}

} // namespace

PipelineState parsePipelineState(
    const nlohmann::json &raster_state,
    std::size_t color_attachment_count,
    std::string_view context) {
    const std::string state_context{context};
    requireObject(raster_state, state_context);
    requireOnlyFields(
        raster_state, pipeline_state_keys, state_context);
    PipelineState result;
    result.color_attachments.resize(
        color_attachment_count);
    readPipelineStateFields(
        raster_state, result, color_attachment_count,
        state_context);
    validatePipelineStateValues(result, state_context);
    return result;
}

std::span<const std::string_view>
surfacePipelineStateKeys() noexcept {
    return surface_pipeline_state_keys;
}

PipelineState parseSurfacePipelineState(
    const nlohmann::json &fields,
    std::string_view context) {
    const std::string state_context{context};
    requireObject(fields, state_context);
    requireOnlyFields(
        fields, surface_pipeline_state_keys, state_context);
    PipelineState result;
    readPipelineStateFields(
        fields, result, 0, state_context);
    validatePipelineStateValues(result, state_context);
    return result;
}

nlohmann::json pipelineStateToJson(
    const PipelineState &state) {
    nlohmann::json colors =
        nlohmann::json::array();
    for (const auto &color :
         state.color_attachments) {
        colors.push_back(
            nlohmann::json{
                {"blend",
                 materialOutputBlendStateToJson(
                     color.blend)},
                {"write_mask",
                 materialOutputWriteMaskName(
                     color.write_mask)},
            });
    }
    // Optional groups appear only when engaged: their absence is "off".
    nlohmann::json result{
        {"topology",
         rasterPrimitiveTopologyName(state.topology)},
        {"primitive_restart", state.primitive_restart},
        {"cull", rasterCullModeName(state.cull)},
        {"front_face",
         rasterFrontFaceName(state.front_face)},
        {"polygon_mode",
         rasterPolygonModeName(state.polygon_mode)},
        {"depth_clamp", state.depth_clamp},
        {"line_width", state.line_width},
        {"rasterizer_discard", state.rasterizer_discard},
        {"conservative",
         rasterConservativeModeName(state.conservative)},
        {"sample_mask", state.sample_mask},
        {"alpha_to_coverage", state.alpha_to_coverage},
        {"alpha_to_one", state.alpha_to_one},
        {"depth_test", state.depth.test},
        {"depth_write", state.depth.write},
        {"depth_compare",
         rasterDepthCompareName(state.depth.compare)},
        {"blend_constants",
         nlohmann::json::array(
             {state.blend_constants[0],
              state.blend_constants[1],
              state.blend_constants[2],
              state.blend_constants[3]})},
        {"color_attachments", std::move(colors)},
    };
    if (state.depth_bias) {
        result["depth_bias"] = {
            {"constant", state.depth_bias->constant},
            {"slope", state.depth_bias->slope},
            {"clamp", state.depth_bias->clamp},
        };
    }
    if (state.sample_shading) {
        result["sample_shading"] = *state.sample_shading;
    }
    if (state.depth.bounds) {
        result["depth_bounds"] = nlohmann::json::array(
            {state.depth.bounds->min,
             state.depth.bounds->max});
    }
    if (state.stencil) {
        result["stencil"] = {
            {"front", stencilFaceToJson(state.stencil->front)},
            {"back", stencilFaceToJson(state.stencil->back)},
        };
    }
    if (state.logic_op) {
        result["logic_op"] =
            rasterLogicOperationName(*state.logic_op);
    }
    return result;
}

RasterFixedFunctionState parseRasterFixedFunctionState(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    std::string_view context) {
    const auto found = pass.find("raster_state");
    if (found == pass.end()) {
        RasterFixedFunctionState result;
        result.color_attachments.resize(
            color_attachment_count);
        return result;
    }
    return parsePipelineState(
        *found, color_attachment_count,
        std::string{context} + " raster_state");
}

RasterFixedFunctionState parseFullscreenRasterFixedFunctionState(
    const nlohmann::json &pass,
    std::string_view context) {
    const auto found = pass.find("raster_state");
    if (found != pass.end() && found->is_object()) {
        for (const auto &[key, value] : found->items()) {
            (void)value;
            if (key.starts_with("depth_")) {
                throw std::runtime_error(
                    std::string{context} +
                    " raster_state does not support depth field '" +
                    key + "'");
            }
        }
    }
    auto result = parseRasterFixedFunctionState(
        pass, 1, context);
    validateRasterFixedFunctionState(
        result, 1, false, context);
    return result;
}

std::string_view rasterPrimitiveTopologyName(
    RasterPrimitiveTopology topology) {
    switch (topology) {
    case RasterPrimitiveTopology::point_list:
        return "point_list";
    case RasterPrimitiveTopology::line_list:
        return "line_list";
    case RasterPrimitiveTopology::line_strip:
        return "line_strip";
    case RasterPrimitiveTopology::triangle_list:
        return "triangle_list";
    case RasterPrimitiveTopology::triangle_strip:
        return "triangle_strip";
    }
    throw std::runtime_error(
        "unknown raster primitive topology");
}

std::string_view rasterCullModeName(
    RasterCullMode mode) {
    switch (mode) {
    case RasterCullMode::none: return "none";
    case RasterCullMode::front: return "front";
    case RasterCullMode::back: return "back";
    }
    throw std::runtime_error("unknown raster cull mode");
}

std::string_view rasterFrontFaceName(
    RasterFrontFace face) {
    switch (face) {
    case RasterFrontFace::counter_clockwise:
        return "counter_clockwise";
    case RasterFrontFace::clockwise:
        return "clockwise";
    }
    throw std::runtime_error("unknown raster front face");
}

std::string_view rasterClipYName(RasterClipY clip_y) {
    switch (clip_y) {
    case RasterClipY::down: return "down";
    case RasterClipY::up: return "up";
    }
    throw std::runtime_error("unknown raster clip_y");
}

std::string_view rasterDepthCompareName(
    RasterDepthCompare compare) {
    switch (compare) {
    case RasterDepthCompare::never: return "never";
    case RasterDepthCompare::less: return "less";
    case RasterDepthCompare::equal: return "equal";
    case RasterDepthCompare::less_equal:
        return "less_equal";
    case RasterDepthCompare::greater: return "greater";
    case RasterDepthCompare::not_equal:
        return "not_equal";
    case RasterDepthCompare::greater_equal:
        return "greater_equal";
    case RasterDepthCompare::always: return "always";
    }
    throw std::runtime_error(
        "unknown raster depth compare");
}

std::string_view rasterPolygonModeName(
    RasterPolygonMode mode) {
    switch (mode) {
    case RasterPolygonMode::fill: return "fill";
    case RasterPolygonMode::line: return "line";
    case RasterPolygonMode::point: return "point";
    }
    throw std::runtime_error("unknown raster polygon mode");
}

std::string_view rasterConservativeModeName(
    RasterConservativeMode mode) {
    switch (mode) {
    case RasterConservativeMode::off: return "off";
    case RasterConservativeMode::overestimate:
        return "overestimate";
    case RasterConservativeMode::underestimate:
        return "underestimate";
    }
    throw std::runtime_error(
        "unknown raster conservative mode");
}

std::string_view rasterStencilOperationName(
    RasterStencilOperation operation) {
    switch (operation) {
    case RasterStencilOperation::keep: return "keep";
    case RasterStencilOperation::zero: return "zero";
    case RasterStencilOperation::replace: return "replace";
    case RasterStencilOperation::increment_and_clamp:
        return "increment_and_clamp";
    case RasterStencilOperation::decrement_and_clamp:
        return "decrement_and_clamp";
    case RasterStencilOperation::invert: return "invert";
    case RasterStencilOperation::increment_and_wrap:
        return "increment_and_wrap";
    case RasterStencilOperation::decrement_and_wrap:
        return "decrement_and_wrap";
    }
    throw std::runtime_error(
        "unknown raster stencil operation");
}

std::string_view rasterLogicOperationName(
    RasterLogicOperation operation) {
    switch (operation) {
    case RasterLogicOperation::clear: return "clear";
    case RasterLogicOperation::bitwise_and: return "and";
    case RasterLogicOperation::and_reverse:
        return "and_reverse";
    case RasterLogicOperation::copy: return "copy";
    case RasterLogicOperation::and_inverted:
        return "and_inverted";
    case RasterLogicOperation::no_op: return "no_op";
    case RasterLogicOperation::bitwise_xor: return "xor";
    case RasterLogicOperation::bitwise_or: return "or";
    case RasterLogicOperation::nor: return "nor";
    case RasterLogicOperation::equivalent:
        return "equivalent";
    case RasterLogicOperation::invert: return "invert";
    case RasterLogicOperation::or_reverse:
        return "or_reverse";
    case RasterLogicOperation::copy_inverted:
        return "copy_inverted";
    case RasterLogicOperation::or_inverted:
        return "or_inverted";
    case RasterLogicOperation::nand: return "nand";
    case RasterLogicOperation::set: return "set";
    }
    throw std::runtime_error(
        "unknown raster logic operation");
}

RasterPassContract parseRasterPassContract(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context, bool require_draw) {
    requireObject(pass, context);
    RasterPassContract result;
    if (require_draw || pass.contains("draw")) result.geometry = parseGeometry(pass, context);
    result.state =
        parseRasterFixedFunctionState(
            pass, color_attachment_count,
            context);
    result.clip_y = optionalEnum(
        pass, "clip_y", result.clip_y, context,
        [](std::string_view value) {
            return findName<RasterClipY>(
                value,
                {{"down", RasterClipY::down},
                 {"up", RasterClipY::up}});
        });
    validateRasterPassContract(
        result, color_attachment_count,
        has_depth_attachment, context);
    return result;
}

void validateRasterPassContract(
    const RasterPassContract &contract,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context) {
    if (contract.schema_version != 1) {
        throw std::runtime_error(
            std::string{context} +
            " has unsupported raster contract schema version " +
            std::to_string(contract.schema_version));
    }
    requireVersionedId(
        contract.geometry.implementation,
        std::string{context} +
            " draw implementation");
    std::visit(
        [&](const auto &operation) {
            if constexpr (std::is_same_v<std::decay_t<decltype(operation)>, RasterIndexedDrawOperation>) {
                if (operation.index_count == 0 || operation.index_buffer.empty())
                    throw std::runtime_error(std::string{context} + " indexed draw requires a buffer and non-zero count");
            } else if (operation.vertex_count == 0) {
                throw std::runtime_error(
                    std::string{context} +
                    " direct draw vertex_count must be non-zero");
            }
            if (operation.instance_count == 0) {
                throw std::runtime_error(
                    std::string{context} +
                    " direct draw instance_count must be non-zero");
            }
        },
        contract.geometry.operation);
    validateRasterFixedFunctionState(
        contract.state, color_attachment_count,
        has_depth_attachment, context);
}

void validatePipelineState(
    const PipelineState &state,
    bool has_depth_attachment,
    std::string_view context) {
    validatePipelineStateValues(state, context);
    if (has_depth_attachment) return;
    for (const auto &[engaged, field] :
         {std::pair{state.depth_bias.has_value(), "depth_bias"},
          std::pair{state.depth.bounds.has_value(),
                    "depth_bounds"},
          std::pair{state.stencil.has_value(), "stencil"}}) {
        if (engaged) {
            throw std::runtime_error(
                std::string{context} + " enables " + field +
                " without a depth attachment");
        }
    }
}

void validatePipelineStateStencilFormat(
    const PipelineState &state,
    bool depth_format_has_stencil,
    std::string_view depth_format,
    std::string_view context) {
    if (!state.stencil || depth_format_has_stencil) return;
    throw std::runtime_error(
        std::string{context} +
        " enables stencil, but its depth attachment format '" +
        std::string{depth_format} +
        "' has no stencil component (use D24_UNORM_S8_UINT)");
}

void validateRasterFixedFunctionState(
    const RasterFixedFunctionState &state,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context) {
    if (state.color_attachments.size() !=
        color_attachment_count) {
        throw std::runtime_error(
            std::string{context} +
            " color attachment state count does not match pass outputs");
    }
    if (!has_depth_attachment &&
        (state.depth.test || state.depth.write)) {
        throw std::runtime_error(
            std::string{context} +
            " enables depth state without a depth attachment");
    }
    for (const auto &color :
         state.color_attachments) {
        if ((color.write_mask &
             ~materialOutputWriteRgba) != 0) {
            throw std::runtime_error(
                std::string{context} +
                " has invalid color write-mask bits");
        }
    }
    validatePipelineState(
        state, has_depth_attachment, context);
}

void validateRasterColorAttachmentNumericClasses(
    std::span<const RasterColorAttachmentState> states,
    std::span<const MaterialOutputNumericClass> numeric_classes,
    std::string_view context) {
    if (!numeric_classes.empty() &&
        numeric_classes.size() != states.size()) {
        throw std::runtime_error(
            std::string{context} +
            " physical color numeric-class count does not match "
            "logical outputs");
    }
    if (numeric_classes.empty()) return;
    for (std::size_t index = 0; index < states.size(); ++index) {
        if (states[index].blend.enabled &&
            numeric_classes[index] !=
                MaterialOutputNumericClass::floating) {
            throw std::runtime_error(
                std::string{context} +
                " cannot enable blending for integer color output " +
                std::to_string(index));
        }
    }
}

std::optional<PipelineStateRequirement>
firstUnsupportedPipelineState(
    const PipelineState &state,
    const PipelineStateDeviceFacts &facts) {
    const auto require =
        [](std::string_view field, std::string_view feature) {
            return std::optional{PipelineStateRequirement{
                std::string{field}, std::string{feature}}};
        };
    // rasterization
    if (state.polygon_mode != RasterPolygonMode::fill &&
        !facts.fill_mode_non_solid) {
        return require("polygon_mode", "fillModeNonSolid");
    }
    if (state.depth_clamp && !facts.depth_clamp) {
        return require("depth_clamp", "depthClamp");
    }
    if (state.depth_bias && state.depth_bias->clamp != 0.0 &&
        !facts.depth_bias_clamp) {
        return require("depth_bias", "depthBiasClamp");
    }
    if (state.line_width != 1.0 && !facts.wide_lines) {
        return require("line_width", "wideLines");
    }
    if (state.conservative != RasterConservativeMode::off) {
        if (!facts.conservative_rasterization) {
            return require(
                "conservative",
                "VK_EXT_conservative_rasterization");
        }
        if (state.conservative ==
                RasterConservativeMode::underestimate &&
            !facts.primitive_underestimation) {
            return require(
                "conservative", "primitiveUnderestimation");
        }
        const bool points_or_lines =
            state.polygon_mode != RasterPolygonMode::fill ||
            state.topology ==
                RasterPrimitiveTopology::point_list ||
            state.topology ==
                RasterPrimitiveTopology::line_list ||
            state.topology ==
                RasterPrimitiveTopology::line_strip;
        if (points_or_lines &&
            !facts.conservative_point_and_line_rasterization) {
            return require(
                "conservative",
                "conservativePointAndLineRasterization");
        }
    }
    // multisample
    if (state.sample_shading && !facts.sample_rate_shading) {
        return require("sample_shading", "sampleRateShading");
    }
    if (state.alpha_to_one && !facts.alpha_to_one) {
        return require("alpha_to_one", "alphaToOne");
    }
    // depth / stencil
    if (state.depth.bounds && !facts.depth_bounds) {
        return require("depth_bounds", "depthBounds");
    }
    // color blend
    if (state.logic_op && !facts.logic_op) {
        return require("logic_op", "logicOp");
    }
    const bool dual_source = std::any_of(
        state.color_attachments.begin(),
        state.color_attachments.end(),
        [](const auto &attachment) {
            return materialOutputBlendStateUsesDualSource(
                attachment.blend);
        });
    if (dual_source && !facts.dual_src_blend) {
        return require("color_attachments", "dualSrcBlend");
    }
    if (dual_source &&
        state.color_attachments.size() >
            facts.max_fragment_dual_src_attachments) {
        return require(
            "color_attachments",
            "maxFragmentDualSrcAttachments");
    }
    return std::nullopt;
}

std::string pipelineStateUnsupportedMessage(
    std::string_view pass,
    const PipelineStateRequirement &requirement,
    std::string_view material) {
    nlohmann::ordered_json fields{
        {"pass", std::string{pass}},
        {"field", requirement.field},
        {"feature", requirement.feature},
    };
    if (!material.empty()) {
        fields["material"] = std::string{material};
    }
    return std::string{pipelineStateUnsupportedCode} + ": " +
           fields.dump();
}

void requirePipelineStateSupported(
    const PipelineState &state,
    const PipelineStateDeviceFacts &facts,
    std::string_view pass) {
    if (const auto missing =
            firstUnsupportedPipelineState(state, facts)) {
        throw std::runtime_error(
            pipelineStateUnsupportedMessage(pass, *missing));
    }
}

nlohmann::json rasterPassContractToJson(
    const RasterPassContract &contract) {
    nlohmann::json draw{
        {"implementation",
         contract.geometry.implementation},
    };
    std::visit(
        [&](const auto &operation) {
            if constexpr (std::is_same_v<std::decay_t<decltype(operation)>, RasterIndexedDrawOperation>) {
                draw["operation"] = "indexed";
                draw["index_buffer"] = operation.index_buffer;
                draw["index_count"] = operation.index_count;
                draw["first_index"] = operation.first_index;
            } else {
                draw["operation"] = "direct";
                draw["vertex_count"] = operation.vertex_count;
                draw["first_vertex"] = operation.first_vertex;
            }
            draw["instance_count"] =
                operation.instance_count;
            draw["first_instance"] =
                operation.first_instance;
        },
        contract.geometry.operation);

    return nlohmann::json{
        {"schema_version",
         contract.schema_version},
        {"draw", std::move(draw)},
        {"clip_y", rasterClipYName(contract.clip_y)},
        {"raster_state",
         pipelineStateToJson(contract.state)},
    };
}

std::string rasterPassContractFingerprint(
    const RasterPassContract &contract) {
    std::ostringstream result;
    result << rasterPassContractId << ':'
           << rasterPassContractToJson(contract).dump();
    return result.str();
}

} // namespace Pelican
