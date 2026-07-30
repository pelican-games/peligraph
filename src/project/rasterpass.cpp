#include "rasterpass.hpp"

#include "logicalrendertype.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

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
    std::initializer_list<std::string_view> allowed,
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
    requireOnlyFields(
        *found,
        {"implementation", "operation", "vertex_count",
         "instance_count", "first_vertex", "first_instance"},
        draw_context);

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
    if (operation != "direct") {
        throw std::runtime_error(
            draw_context +
            " has unsupported typed operation '" +
            operation + "'");
    }
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

RasterFixedFunctionState parseState(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    std::string_view context) {
    RasterFixedFunctionState result;
    result.color_attachments.resize(
        color_attachment_count);

    const auto found = pass.find("raster_state");
    if (found == pass.end()) return result;
    const auto state_context =
        std::string{context} + " raster_state";
    requireObject(*found, state_context);
    requireOnlyFields(
        *found,
        {"topology", "cull", "front_face",
         "depth_test", "depth_write", "depth_compare",
         "color_attachments"},
        state_context);

    result.topology = optionalEnum(
        *found, "topology", result.topology,
        state_context,
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
    result.cull = optionalEnum(
        *found, "cull", result.cull,
        state_context,
        [](std::string_view value) {
            return findName<RasterCullMode>(
                value,
                {{"none", RasterCullMode::none},
                 {"front", RasterCullMode::front},
                 {"back", RasterCullMode::back}});
        });
    result.front_face = optionalEnum(
        *found, "front_face",
        result.front_face, state_context,
        [](std::string_view value) {
            return findName<RasterFrontFace>(
                value,
                {{"counter_clockwise",
                  RasterFrontFace::counter_clockwise},
                 {"clockwise",
                  RasterFrontFace::clockwise}});
        });
    result.depth.test = optionalBool(
        *found, "depth_test",
        result.depth.test, state_context);
    result.depth.write = optionalBool(
        *found, "depth_write",
        result.depth.write, state_context);
    result.depth.compare = optionalEnum(
        *found, "depth_compare",
        result.depth.compare, state_context,
        [](std::string_view value) {
            return findName<RasterDepthCompare>(
                value,
                {{"never", RasterDepthCompare::never},
                 {"less", RasterDepthCompare::less},
                 {"equal", RasterDepthCompare::equal},
                 {"less_equal",
                  RasterDepthCompare::less_equal},
                 {"greater", RasterDepthCompare::greater},
                 {"not_equal",
                  RasterDepthCompare::not_equal},
                 {"greater_equal",
                  RasterDepthCompare::greater_equal},
                 {"always", RasterDepthCompare::always}});
        });

    const auto colors =
        found->find("color_attachments");
    if (colors == found->end()) return result;
    if (!colors->is_array()) {
        throw std::runtime_error(
            state_context +
            " color_attachments must be an array");
    }
    if (colors->size() != color_attachment_count) {
        throw std::runtime_error(
            state_context +
            " color_attachments count must match pass color outputs");
    }
    for (std::size_t index = 0;
         index < colors->size(); ++index) {
        const auto &encoded = colors->at(index);
        const auto color_context =
            state_context + " color_attachments[" +
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
    return result;
}

} // namespace

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

RasterPassContract parseRasterPassContract(
    const nlohmann::json &pass,
    std::size_t color_attachment_count,
    bool has_depth_attachment,
    std::string_view context) {
    requireObject(pass, context);
    RasterPassContract result;
    result.geometry = parseGeometry(pass, context);
    result.state =
        parseState(
            pass, color_attachment_count,
            context);
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
            if (operation.vertex_count == 0) {
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
    if (contract.state.color_attachments.size() !=
        color_attachment_count) {
        throw std::runtime_error(
            std::string{context} +
            " color attachment state count does not match pass outputs");
    }
    if (!has_depth_attachment &&
        (contract.state.depth.test ||
         contract.state.depth.write)) {
        throw std::runtime_error(
            std::string{context} +
            " enables depth state without a depth attachment");
    }
    for (const auto &color :
         contract.state.color_attachments) {
        if ((color.write_mask &
             ~materialOutputWriteRgba) != 0) {
            throw std::runtime_error(
                std::string{context} +
                " has invalid color write-mask bits");
        }
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
            draw["operation"] = "direct";
            draw["vertex_count"] =
                operation.vertex_count;
            draw["instance_count"] =
                operation.instance_count;
            draw["first_vertex"] =
                operation.first_vertex;
            draw["first_instance"] =
                operation.first_instance;
        },
        contract.geometry.operation);

    nlohmann::json colors =
        nlohmann::json::array();
    for (const auto &color :
         contract.state.color_attachments) {
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
    return nlohmann::json{
        {"schema_version",
         contract.schema_version},
        {"draw", std::move(draw)},
        {"raster_state",
         {
             {"topology",
              rasterPrimitiveTopologyName(
                  contract.state.topology)},
             {"cull",
              rasterCullModeName(
                  contract.state.cull)},
             {"front_face",
              rasterFrontFaceName(
                  contract.state.front_face)},
             {"depth_test",
              contract.state.depth.test},
             {"depth_write",
              contract.state.depth.write},
             {"depth_compare",
              rasterDepthCompareName(
                  contract.state.depth.compare)},
             {"color_attachments",
              std::move(colors)},
         }},
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
