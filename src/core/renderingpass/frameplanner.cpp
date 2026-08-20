#include "frameplanner.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "materialpassinfojsonparser.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/imagesubresourcejson.hpp"
#include "../../project/passshapepolicy.hpp"
#include "../../project/renderresourcename.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "../../project/materialscreeninput.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

struct Edge {
    size_t from = 0;
    size_t to = 0;
};

struct PlannerEdges {
    std::vector<std::vector<bool>> exists;
    std::vector<Edge> edges;
    std::vector<FramePlanBarrier> barriers;
};

void appendUnique(std::vector<std::string> &values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void appendUnique(std::vector<std::string> &values, const std::vector<std::string> &more_values) {
    for (const auto &value : more_values) {
        appendUnique(values, value);
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
    throw std::runtime_error(
        "Unknown frame graph read footprint kind");
}

void appendReadFootprint(
    FrameGraphNodeDefinition &node, std::string resource,
    LogicalReadFootprint footprint) {
    const auto found = std::find_if(
        node.read_footprints.begin(),
        node.read_footprints.end(),
        [&](const auto &entry) {
            return entry.resource == resource;
        });
    if (found == node.read_footprints.end()) {
        node.read_footprints.push_back({
            std::move(resource),
            std::move(footprint),
        });
        return;
    }
    const auto existing_rank =
        footprintRank(found->footprint.kind);
    const auto incoming_rank =
        footprintRank(footprint.kind);
    if (incoming_rank > existing_rank) {
        found->footprint = std::move(footprint);
        return;
    }
    if (incoming_rank == existing_rank &&
        footprint.kind ==
            LogicalReadFootprintKind::neighborhood) {
        if (!found->footprint.radius ||
            !footprint.radius) {
            found->footprint.radius = std::nullopt;
        } else {
            found->footprint.radius =
                std::max(*found->footprint.radius,
                         *footprint.radius);
        }
    }
}

LogicalReadFootprintKind parseReadFootprintKind(
    std::string_view value, std::string_view context) {
    if (value == "same_pixel") {
        return LogicalReadFootprintKind::same_pixel;
    }
    if (value == "neighborhood") {
        return LogicalReadFootprintKind::neighborhood;
    }
    if (value == "arbitrary") {
        return LogicalReadFootprintKind::arbitrary;
    }
    if (value == "temporal") {
        throw std::runtime_error(
            std::string{context} +
            " cannot declare temporal; use @history");
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown read footprint '" +
        std::string{value} + "'");
}

LogicalReadFootprint parseReadFootprint(
    const nlohmann::json &encoded,
    std::string_view context) {
    LogicalReadFootprint result;
    if (encoded.is_string()) {
        result.kind = parseReadFootprintKind(
            encoded.get_ref<const std::string &>(),
            context);
        return result;
    }
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " must be a footprint string or object");
    }
    for (auto entry = encoded.begin();
         entry != encoded.end(); ++entry) {
        if (entry.key() != "kind" &&
            entry.key() != "radius") {
            throw std::runtime_error(
                std::string{context} +
                " has unknown field '" +
                entry.key() + "'");
        }
    }
    if (!encoded.contains("kind") ||
        !encoded.at("kind").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            " requires string field: kind");
    }
    result.kind = parseReadFootprintKind(
        encoded.at("kind")
            .get_ref<const std::string &>(),
        context);
    if (!encoded.contains("radius")) {
        return result;
    }
    const auto &radius = encoded.at("radius");
    if (!radius.is_number_unsigned()) {
        throw std::runtime_error(
            std::string{context} +
            " radius must be a positive unsigned integer");
    }
    const auto value =
        radius.get<std::uint64_t>();
    if (value == 0 ||
        value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} +
            " radius is outside the uint32 range");
    }
    if (result.kind !=
        LogicalReadFootprintKind::neighborhood) {
        throw std::runtime_error(
            std::string{context} +
            " radius is valid only for neighborhood reads");
    }
    result.radius =
        static_cast<std::uint32_t>(value);
    return result;
}

void parseReadFootprintOverrides(
    const nlohmann::json &source,
    std::string_view field,
    FrameGraphNodeDefinition &node) {
    if (!source.contains(field)) {
        return;
    }
    const auto &declaration = source.at(field);
    if (!declaration.is_object()) {
        throw std::runtime_error(
            "Frame graph " + std::string{field} +
            " must be an object");
    }
    for (auto entry = declaration.begin();
         entry != declaration.end(); ++entry) {
        if (std::find(node.reads.begin(),
                      node.reads.end(),
                      entry.key()) ==
            node.reads.end()) {
            throw std::runtime_error(
                "Frame graph " + std::string{field} +
                " names a non-current read resource: " +
                entry.key());
        }
        appendReadFootprint(
            node, entry.key(),
            parseReadFootprint(
                entry.value(),
                "Frame graph " +
                    std::string{field} + "." +
                    entry.key()));
    }
}

void appendShaderResourcePortAccesses(
    const nlohmann::json &source,
    std::span<const std::string> authored_reads,
    std::span<const std::string> writes,
    std::string_view context,
    FrameGraphNodeDefinition &node) {
    const auto ports =
        parseShaderResourcePortDefinitions(
            source, authored_reads, writes, context);
    for (const auto &port : ports) {
        const auto written =
            std::find(
                writes.begin(), writes.end(),
                port.resource) != writes.end();
        const auto access =
            effectiveShaderResourcePortAccess(
                port, true, written);
        const auto intent =
            access ==
                    ShaderResourcePortAccess::sampled
                ? LogicalAccessIntent::sampled
                : LogicalAccessIntent::storage;
        const auto existing = std::find_if(
            node.resource_accesses.begin(),
            node.resource_accesses.end(),
            [&](const auto &candidate) {
                return candidate.resource ==
                       port.resource;
            });
        if (existing ==
            node.resource_accesses.end()) {
            node.resource_accesses.push_back(
                FrameGraphResourceAccessDefinition{
                    .resource = port.resource,
                    .intent = intent,
                });
        } else if (existing->intent != intent) {
            // One logical resource use can expose disjoint sampled and
            // storage subresources. The shadow graph keeps a conservative
            // base-resource intent while descriptor lowering retains each
            // precise range.
            existing->intent =
                LogicalAccessIntent::storage;
        }
    }
}

void appendNodes(std::vector<FrameGraphNodeDefinition> &nodes,
                 std::vector<FrameGraphNodeDefinition> more_nodes) {
    for (auto &node : more_nodes) {
        nodes.push_back(std::move(node));
    }
}

std::string requireString(const nlohmann::json &json, std::string_view field, std::string_view context) {
    if (!json.contains(field) || !json.at(field).is_string()) {
        throw std::runtime_error("Frame graph " + std::string{context} + " requires string field: " +
                                 std::string{field});
    }
    return json.at(field).get<std::string>();
}

std::vector<std::string> parseStringList(const nlohmann::json &json, std::string_view context) {
    if (json.is_null()) {
        return {};
    }
    if (json.is_string()) {
        return {json.get<std::string>()};
    }
    if (!json.is_array()) {
        throw std::runtime_error("Frame graph " + std::string{context} + " must be a string or string array");
    }

    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto &entry : json) {
        if (!entry.is_string()) {
            throw std::runtime_error("Frame graph " + std::string{context} + " entries must be strings");
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalStringList(const nlohmann::json &json, std::string_view field,
                                                 std::string_view context) {
    if (!json.contains(field)) {
        return {};
    }
    return parseStringList(json.at(field), std::string{context} + "." + std::string{field});
}

void appendMaterialScreenInputReads(
    const nlohmann::json &pass_json,
    FrameGraphNodeDefinition &node) {
    if (!pass_json.contains("screen_inputs")) {
        return;
    }
    const auto &screen_inputs = pass_json.at("screen_inputs");
    if (!screen_inputs.is_object()) {
        throw std::runtime_error(
            "Frame graph pass.screen_inputs must be an object");
    }
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    for (auto entry = screen_inputs.begin(); entry != screen_inputs.end(); ++entry) {
        if (!entry.value().is_string()) {
            throw std::runtime_error(
                "Frame graph pass.screen_inputs entries must name resources");
        }
        const auto resource =
            entry.value().get<std::string>();
        appendUnique(node.reads, resource);
        appendUnique(
            node.local_read_shader_inputs,
            resource);
        appendReadFootprint(
            node, resource,
            makeBuiltinMaterialScreenInputContract(
                types, entry.key())
                .footprint);
    }
}

void appendMaterialSurfaceResourceReads(
    const nlohmann::json &pass_json,
    FrameGraphNodeDefinition &node) {
    if (!pass_json.contains("surface_resources")) {
        return;
    }
    const auto &resources =
        pass_json.at("surface_resources");
    if (!resources.is_object()) {
        throw std::runtime_error(
            "Frame graph pass.surface_resources must be an object");
    }
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    for (auto entry = resources.begin();
         entry != resources.end(); ++entry) {
        if (!entry.value().is_string()) {
            throw std::runtime_error(
                "Frame graph pass.surface_resources entries must "
                "name resources");
        }
        const auto resource =
            entry.value().get<std::string>();
        appendUnique(node.reads, resource);
        appendReadFootprint(
            node, resource,
            makeBuiltinMaterialPassInputContract(
                types, entry.key())
                .footprint);
    }
}

void appendMaterialResourceReads(
    const nlohmann::json &pass_json,
    FrameGraphNodeDefinition &node) {
    if (!pass_json.contains("material_resources")) {
        return;
    }
    const auto &encoded =
        pass_json.at("material_resources");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            "Frame graph pass.material_resources must be an object");
    }

    nlohmann::json sanitized =
        nlohmann::json::object();
    std::vector<std::string> reads;
    reads.reserve(encoded.size());
    for (auto entry = encoded.begin();
         entry != encoded.end(); ++entry) {
        if (entry.value().is_string()) {
            sanitized[entry.key()] = entry.value();
            reads.push_back(
                entry.value().get<std::string>());
            continue;
        }
        if (!entry.value().is_object()) {
            throw std::runtime_error(
                "Frame graph pass.material_resources." +
                entry.key() +
                " must be a resource string or object");
        }
        auto object = entry.value();
        object.erase("footprint");
        sanitized[entry.key()] = std::move(object);
        if (entry.value().contains("resource")) {
            if (!entry.value().at("resource").is_string()) {
                throw std::runtime_error(
                    "Frame graph pass.material_resources." +
                    entry.key() +
                    ".resource must be a string");
            }
            reads.push_back(
                entry.value().at("resource")
                    .get<std::string>());
        } else {
            reads.push_back(entry.key());
        }
    }
    const nlohmann::json owner{
        {"resource_ports", std::move(sanitized)}};
    const auto ports =
        parseShaderResourcePortDefinitions(
            owner, reads, std::span<const std::string>{},
            "frame graph material pass '" + node.name + "'");
    constexpr std::string_view history_suffix =
        "@history";
    for (const auto &port : ports) {
        const auto history =
            port.resource.ends_with(history_suffix);
        auto resource = history
                            ? port.resource.substr(
                                  0,
                                  port.resource.size() -
                                      history_suffix.size())
                            : port.resource;
        if (resource.empty() ||
            resource.find('@') != std::string::npos) {
            throw std::runtime_error(
                "Frame graph material resource has invalid qualifier: " +
                port.resource);
        }
        if (history) {
            appendUnique(
                node.reads_history, resource);
        } else {
            appendUnique(node.reads, resource);
            if (port.access !=
                    ShaderResourcePortAccess::
                        storage &&
                port.kind !=
                    ShaderResourcePortKind::
                        buffer) {
                appendUnique(
                    node.local_read_shader_inputs,
                    resource);
            }
            const auto &source =
                encoded.at(port.name);
            appendReadFootprint(
                node, resource,
                source.is_object() &&
                        source.contains("footprint")
                    ? parseReadFootprint(
                          source.at("footprint"),
                          "Frame graph pass.material_resources." +
                              port.name + ".footprint")
                    : LogicalReadFootprint{
                          LogicalReadFootprintKind::arbitrary,
                          std::nullopt});
        }
        if (history &&
            encoded.at(port.name).is_object() &&
            encoded.at(port.name)
                .contains("footprint")) {
            throw std::runtime_error(
                "Frame graph material resource '" +
                port.name +
                "' @history derives temporal footprint");
        }
        const auto intent =
            port.access ==
                    ShaderResourcePortAccess::sampled
                ? LogicalAccessIntent::sampled
            : port.access ==
                    ShaderResourcePortAccess::storage
                ? LogicalAccessIntent::storage
                : LogicalAccessIntent::automatic;
        node.resource_accesses.push_back(
            FrameGraphResourceAccessDefinition{
                .resource = port.resource,
                .intent = intent,
            });
    }
}

void splitHistoryReads(const std::vector<std::string> &authored,
                       std::vector<std::string> &reads,
                       std::vector<std::string> &reads_history) {
    constexpr std::string_view suffix = "@history";
    for (const auto &resource : authored) {
        if (resource.ends_with(suffix)) {
            const auto name = resource.substr(0, resource.size() - suffix.size());
            if (name.empty() || name.find('@') != std::string::npos) {
                throw std::runtime_error("Invalid frame graph history resource: " + resource);
            }
            appendUnique(reads_history, name);
        } else {
            if (resource.find('@') != std::string::npos) {
                throw std::runtime_error("Unknown frame graph resource qualifier: " + resource);
            }
            appendUnique(reads, resource);
        }
    }
}

FrameGraphAttachmentLoadOp frameGraphAttachmentLoadOp(
    vk::AttachmentLoadOp op) {
    switch (op) {
    case vk::AttachmentLoadOp::eLoad:
        return FrameGraphAttachmentLoadOp::load;
    case vk::AttachmentLoadOp::eClear:
        return FrameGraphAttachmentLoadOp::clear;
    case vk::AttachmentLoadOp::eDontCare:
        return FrameGraphAttachmentLoadOp::discard;
    default:
        throw std::runtime_error(
            "Unsupported frame graph attachment load op");
    }
}

FrameGraphAttachmentStoreOp frameGraphAttachmentStoreOp(
    vk::AttachmentStoreOp op) {
    switch (op) {
    case vk::AttachmentStoreOp::eStore:
        return FrameGraphAttachmentStoreOp::store;
    case vk::AttachmentStoreOp::eDontCare:
        return FrameGraphAttachmentStoreOp::discard;
    default:
        throw std::runtime_error(
            "Unsupported frame graph attachment store op");
    }
}

vk::AttachmentLoadOp parseAttachmentLoadOp(
    const nlohmann::json &pass_json,
    std::string_view field,
    vk::AttachmentLoadOp fallback) {
    if (!pass_json.contains(field)) {
        return fallback;
    }
    return stringToLoadOp(
        parseStringField(
            pass_json, std::string{field},
            "frame graph pass"));
}

vk::AttachmentStoreOp parseAttachmentStoreOp(
    const nlohmann::json &pass_json,
    std::string_view field,
    vk::AttachmentStoreOp fallback) {
    if (!pass_json.contains(field)) {
        return fallback;
    }
    return stringToStoreOp(
        parseStringField(
            pass_json, std::string{field},
            "frame graph pass"));
}

struct ParsedRasterAttachment {
    std::string resource;
    std::optional<ImageSubresourceRange> subresource;
};

ParsedRasterAttachment parseRasterAttachmentReference(
    const nlohmann::json &encoded,
    std::string_view context) {
    ParsedRasterAttachment result;
    if (encoded.is_string()) {
        result.resource = encoded.get<std::string>();
    } else if (encoded.is_object()) {
        for (auto field = encoded.begin();
             field != encoded.end(); ++field) {
            if (field.key() != "target" &&
                field.key() != "subresource") {
                throw std::runtime_error(
                    std::string{context} +
                    " has unknown field '" +
                    field.key() + "'");
            }
        }
        if (!encoded.contains("target") ||
            !encoded.at("target").is_string()) {
            throw std::runtime_error(
                std::string{context} +
                " object requires string field target");
        }
        result.resource =
            encoded.at("target").get<std::string>();
        result.subresource =
            parseOptionalImageSubresource(
                encoded, context);
        if (result.subresource &&
            (result.subresource->mip_count_mode !=
                 ImageSubresourceMipCountMode::fixed ||
             result.subresource->level_count != 1)) {
            throw std::runtime_error(
                std::string{context} +
                ".subresource must select exactly one mip level");
        }
    } else {
        throw std::runtime_error(
            std::string{context} +
            " must be a render target name or object");
    }
    if (result.resource.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " target must not be empty");
    }
    if (result.resource == "swapchain" &&
        result.subresource) {
        throw std::runtime_error(
            "Swapchain output cannot select a subresource");
    }
    return result;
}

std::vector<ParsedRasterAttachment>
parseRasterAttachmentList(
    nlohmann::json encoded,
    std::string_view context) {
    if (encoded.is_null()) return {};
    if (!encoded.is_array()) {
        encoded = nlohmann::json::array(
            {std::move(encoded)});
    }
    std::vector<ParsedRasterAttachment> result;
    result.reserve(encoded.size());
    for (const auto &entry : encoded) {
        result.push_back(
            parseRasterAttachmentReference(
                entry, context));
    }
    return result;
}

std::vector<ParsedRasterAttachment>
parseOutputColors(const nlohmann::json &output_json) {
    if (!output_json.contains("color")) {
        throw std::runtime_error("Frame graph pass output requires color field");
    }
    return parseRasterAttachmentList(
        output_json.at("color"),
        "pass.output.color");
}

std::vector<ParsedRasterAttachment>
parseOutputDepth(const nlohmann::json &output_json) {
    if (!output_json.contains("depth")) {
        throw std::runtime_error("Frame graph pass output requires depth field");
    }
    auto result = parseRasterAttachmentList(
        output_json.at("depth"),
        "pass.output.depth");
    if (result.size() > 1) {
        throw std::runtime_error(
            "Frame graph pass output depth accepts at most one attachment");
    }
    return result;
}

FrameGraphNodeDefinition parseRenderNodeFromJson(const nlohmann::json &pass_json, size_t declaration_index) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("Frame graph pass entries must be objects");
    }
    FrameGraphNodeDefinition node;
    node.name = requireString(pass_json, "name", "pass");
    node.declaration_index = declaration_index;
    node.view_family =
        parseRenderViewFamilyId(
            pass_json,
            "Frame graph pass '" +
                node.name + "'");
    node.after = parseOptionalStringList(pass_json, "after", "pass");
    node.before = parseOptionalStringList(pass_json, "before", "pass");
    node.region_tags =
        parseOptionalRegionTags(
            pass_json, "Frame graph pass '" + node.name + "'");

    const auto type = validateAuthoredPassShape(
        defaultPassShapePolicy(), pass_json,
        buildPassFieldOwnershipCapabilities(), node.name,
        {}, "frame graph pass");
    if (type == RenderPassType::material) {
        node.material_filter =
            parseMaterialDrawTagFilterFromJson(
                pass_json,
                "Frame graph material pass '" +
                    node.name + "'");
        if (pass_json.contains("material_variant")) {
            if (!pass_json.at("material_variant").is_string()) {
                throw std::runtime_error(
                    "Frame graph material pass '" + node.name +
                    "' material_variant must be a string");
            }
            node.material_variant =
                pass_json.at("material_variant").get<std::string>();
            validateMaterialVariantName(
                *node.material_variant,
                "Frame graph material pass '" +
                    node.name + "'");
            if (!pass_json.contains(
                    "material_contract")) {
                throw std::runtime_error(
                    "Frame graph material pass '" +
                    node.name +
                    "' material_variant requires explicit "
                    "material_contract");
            }
            if (!node.material_filter ||
                node.material_filter->include.empty()) {
                throw std::runtime_error(
                    "Frame graph material pass '" +
                    node.name +
                    "' material_variant requires non-empty "
                    "material_filter.include");
            }
        }
    }
    if (type == RenderPassType::canonical_anchor) {
        node.kind = FramePlanNodeKind::anchor;
        return node;
    }
    if (type == RenderPassType::snapshot_copy) {
        node.kind = FramePlanNodeKind::snapshot_copy;
        node.reads = {requireString(pass_json, "source", "snapshot copy")};
        node.writes = {requireString(pass_json, "destination", "snapshot copy")};
        node.snapshot_after = requireString(pass_json, "snapshot_after", "snapshot copy");
        return node;
    }
    if (!pass_json.contains("output") || !pass_json.at("output").is_object()) {
        throw std::runtime_error("Frame graph pass requires output object");
    }
    const auto authored_inputs =
        parseOptionalStringList(
            pass_json, "input", "pass");
    splitHistoryReads(
        authored_inputs,
        node.reads, node.reads_history);
    appendShaderResourcePortAccesses(
        pass_json, authored_inputs,
        std::span<const std::string>{},
        "frame graph pass '" + node.name + "'", node);
    parseReadFootprintOverrides(
        pass_json, "input_footprints", node);
    appendMaterialScreenInputReads(pass_json, node);
    appendMaterialSurfaceResourceReads(pass_json, node);
    appendMaterialResourceReads(pass_json, node);
    if (const auto draw_source =
            parseGpuDrawSourceFromJson(
                pass_json,
                "Frame graph pass '" +
                    node.name + "'")) {
        appendUnique(
            node.reads, draw_source->commands);
        appendUnique(
            node.reads, draw_source->count);
    }
    node.kind = type == RenderPassType::output_transform
                    ? FramePlanNodeKind::output_transform
                    : FramePlanNodeKind::render;
    if (type == RenderPassType::raster) {
        node.semantic_dialect =
            "pelican.logical.raster@1";
        node.execution_implementation =
            "pelican.execution.generic_raster_direct@1";
    }
    node.raster_geometry =
        type == RenderPassType::material ||
        type == RenderPassType::shadow_depth ||
        type == RenderPassType::velocity ||
        type == RenderPassType::picking ||
        type == RenderPassType::raster;
    if (type == RenderPassType::fullscreen ||
        type == RenderPassType::raster) {
        for (const auto &resource : node.reads) {
            appendUnique(
                node.local_read_shader_inputs,
                resource);
        }
    }
    node.resolution_domain =
        parseRenderResolutionDomain(
            pass_json, renderPassTypeName(type), node.name);
    const bool overlay_pass =
        type == RenderPassType::ui ||
        type == RenderPassType::imgui;
    const auto &output = pass_json.at("output");
    const auto color_outputs = parseOutputColors(output);
    const auto depth_outputs = parseOutputDepth(output);
    for (const auto &attachment : color_outputs) {
        appendUnique(
            node.writes, attachment.resource);
    }
    for (const auto &attachment : depth_outputs) {
        appendUnique(
            node.writes, attachment.resource);
    }

    const auto color_load = parseAttachmentLoadOp(
        pass_json, "color_load_op",
        overlay_pass
            ? vk::AttachmentLoadOp::eLoad
            : vk::AttachmentLoadOp::eClear);
    const auto color_store = parseAttachmentStoreOp(
        pass_json, "color_store_op",
        vk::AttachmentStoreOp::eStore);
    const auto depth_load = parseAttachmentLoadOp(
        pass_json, "depth_load_op",
        vk::AttachmentLoadOp::eClear);
    const auto depth_store = parseAttachmentStoreOp(
        pass_json, "depth_store_op",
        vk::AttachmentStoreOp::eDontCare);
    for (const auto &attachment : color_outputs) {
        node.attachments.push_back(
            FrameGraphAttachmentDefinition{
                .resource = attachment.resource,
                .subresource =
                    attachment.subresource,
                .aspect =
                    FrameGraphAttachmentAspect::color,
                .load_op =
                    frameGraphAttachmentLoadOp(
                        color_load),
                .store_op =
                    frameGraphAttachmentStoreOp(
                        color_store),
            });
    }
    for (const auto &attachment : depth_outputs) {
        node.attachments.push_back(
            FrameGraphAttachmentDefinition{
                .resource = attachment.resource,
                .subresource =
                    attachment.subresource,
                .aspect =
                    FrameGraphAttachmentAspect::depth,
                .load_op =
                    frameGraphAttachmentLoadOp(
                        depth_load),
                .store_op =
                    frameGraphAttachmentStoreOp(
                        depth_store),
            });
    }

    if (color_load == vk::AttachmentLoadOp::eLoad) {
        for (const auto &attachment : color_outputs) {
            appendUnique(
                node.reads, attachment.resource);
            appendReadFootprint(
                node, attachment.resource,
                {LogicalReadFootprintKind::same_pixel,
                 std::nullopt});
        }
    }
    if (depth_load == vk::AttachmentLoadOp::eLoad) {
        for (const auto &attachment : depth_outputs) {
            appendUnique(
                node.reads, attachment.resource);
            appendReadFootprint(
                node, attachment.resource,
                {LogicalReadFootprintKind::same_pixel,
                 std::nullopt});
        }
    }

    return node;
}

FrameGraphNodeDefinition parseComputeNodeFromJson(const nlohmann::json &task_json, size_t declaration_index) {
    if (!task_json.is_object()) {
        throw std::runtime_error("Frame graph compute_tasks entries must be objects");
    }

    FrameGraphNodeDefinition node;
    node.name = requireString(task_json, "name", "compute task");
    node.kind = FramePlanNodeKind::compute;
    node.declaration_index = declaration_index;
    node.view_family =
        parseRenderViewFamilyId(
            task_json,
            "Frame graph compute task '" +
                node.name + "'");
    const auto authored_reads =
        parseOptionalStringList(
            task_json, "reads", "compute task");
    const auto writes =
        parseOptionalStringList(
            task_json, "writes", "compute task");
    splitHistoryReads(
        authored_reads,
        node.reads, node.reads_history);
    if (task_json.contains("dispatch")) {
        const auto &dispatch =
            task_json.at("dispatch");
        if (dispatch.is_object() &&
            dispatch.contains("indirect")) {
            const auto &indirect =
                dispatch.at("indirect");
            if (!indirect.is_object()) {
                throw std::runtime_error(
                    "compute task dispatch.indirect must be an object: " +
                    node.name);
            }
            appendUnique(
                node.reads,
                requireString(
                    indirect, "buffer",
                    "compute task dispatch.indirect: " +
                        node.name));
        }
    }
    appendShaderResourcePortAccesses(
        task_json, authored_reads, writes,
        "frame graph compute task '" + node.name + "'",
        node);
    parseReadFootprintOverrides(
        task_json, "read_footprints", node);
    node.writes = writes;
    node.after = parseOptionalStringList(task_json, "after", "compute task");
    node.before = parseOptionalStringList(task_json, "before", "compute task");
    node.region_tags =
        parseOptionalRegionTags(
            task_json,
            "Frame graph compute task '" + node.name + "'");
    return node;
}

std::vector<std::string> parseDeclaredRenderTargets(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("render_targets")) {
        return resources;
    }
    const auto &targets = json.at("render_targets");
    if (!targets.is_array()) {
        throw std::runtime_error("Frame graph render_targets must be an array");
    }
    for (const auto &target : targets) {
        if (!target.is_object()) {
            throw std::runtime_error("Frame graph render_targets entries must be objects");
        }
        auto name = requireString(target, "name", "render target");
        validateAuthoredRenderResourceName(
            name, "render target");
        appendUnique(resources, std::move(name));
    }
    return resources;
}

std::vector<std::string> parseHistoryRenderTargets(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("render_targets")) return resources;
    const auto &targets = json.at("render_targets");
    if (!targets.is_array()) throw std::runtime_error("Frame graph render_targets must be an array");
    for (const auto &target : targets) {
        if (!target.is_object()) throw std::runtime_error("Frame graph render_targets entries must be objects");
        if (target.contains("history") && !target.at("history").is_boolean()) {
            throw std::runtime_error("Frame graph render target history must be a boolean");
        }
        if (target.value("history", false)) {
            appendUnique(resources, requireString(target, "name", "render target"));
        }
    }
    return resources;
}

std::vector<std::string> parseDeclaredBuffers(const nlohmann::json &json) {
    std::vector<std::string> resources;
    if (!json.contains("buffers")) {
        return resources;
    }
    const auto &buffers = json.at("buffers");
    if (!buffers.is_array()) {
        throw std::runtime_error("Frame graph buffers must be an array");
    }
    for (const auto &buffer : buffers) {
        if (buffer.is_string()) {
            auto name = buffer.get<std::string>();
            validateAuthoredRenderResourceName(name, "buffer");
            appendUnique(resources, std::move(name));
            continue;
        }
        if (buffer.is_object()) {
            auto name = requireString(buffer, "name", "buffer");
            validateAuthoredRenderResourceName(name, "buffer");
            appendUnique(resources, std::move(name));
            continue;
        }
        throw std::runtime_error("Frame graph buffers entries must be strings or objects");
    }
    return resources;
}

std::size_t formatBlockBytes(std::string_view format) {
    if (format == "R8_UNORM") return 1;
    if (format == "R16_SFLOAT" || format == "D16_UNORM") return 2;
    if (format == "R8G8B8A8_UNORM" || format == "R8G8B8A8_SRGB" ||
        format == "B8G8R8A8_UNORM" || format == "B8G8R8A8_SRGB" ||
        format == "D32_SFLOAT" || format == "R32_SFLOAT" ||
        format == "R16G16_SFLOAT") return 4;
    if (format == "R16G16B16A16_SFLOAT") return 8;
    if (format == "R32G32B32A32_SFLOAT") return 16;
    return 0;
}

std::unordered_map<std::string, std::size_t> parseRenderTargetByteSizes(const nlohmann::json &json) {
    std::unordered_map<std::string, std::size_t> sizes;
    if (!json.contains("render_targets") || !json.at("render_targets").is_array()) return sizes;
    std::optional<std::pair<std::size_t, std::size_t>> base_extent;
    for (const auto &target : json.at("render_targets")) {
        if (!target.is_object() || !target.contains("width") ||
            !target.contains("height")) {
            continue;
        }
        if (target.value("format_class", std::string{}) == "display") {
            base_extent = {target.at("width").get<std::size_t>(),
                           target.at("height").get<std::size_t>()};
            break;
        }
    }
    for (const auto &target : json.at("render_targets")) {
        if (!target.is_object()) continue;
        const auto name = requireString(target, "name", "render target");
        std::size_t width = 0;
        std::size_t height = 0;
        if (target.contains("width") && target.contains("height")) {
            width = target.at("width").get<std::size_t>();
            height = target.at("height").get<std::size_t>();
        } else if (base_extent && target.contains("extent_scale") &&
                   target.at("extent_scale").is_number()) {
            const auto scale = target.at("extent_scale").get<double>();
            width = static_cast<std::size_t>(base_extent->first * scale);
            height = static_cast<std::size_t>(base_extent->second * scale);
        }
        if (width == 0 || height == 0) continue;
        const auto format = requireString(target, "format", "render target '" + name + "'");
        const auto bytes = formatBlockBytes(format);
        if (bytes != 0) sizes.emplace(name, width * height * bytes);
    }
    return sizes;
}

void assignSnapshotByteSizes(std::vector<FrameGraphNodeDefinition> &nodes,
                             const std::unordered_map<std::string, std::size_t> &sizes) {
    for (auto &node : nodes) {
        if (node.kind != FramePlanNodeKind::snapshot_copy) continue;
        const auto found = sizes.find(node.writes.front());
        if (found == sizes.end() || found->second == 0) {
            throw std::runtime_error("snapshot copy byte size is unavailable for '" +
                                     node.writes.front() + "'");
        }
        node.byte_size = found->second;
    }
}

std::vector<FrameGraphNodeDefinition> parseRenderNodes(const nlohmann::json &graph_json,
                                                       size_t &declaration_index) {
    std::vector<FrameGraphNodeDefinition> nodes;
    if (!graph_json.contains("passes")) {
        return nodes;
    }
    const auto &passes = graph_json.at("passes");
    if (!passes.is_array()) {
        throw std::runtime_error("Frame graph passes must be an array");
    }
    nodes.reserve(passes.size());
    for (const auto &pass_json : passes) {
        nodes.push_back(parseRenderNodeFromJson(pass_json, declaration_index++));
    }
    return nodes;
}

std::vector<FrameGraphNodeDefinition> parseComputeNodes(const nlohmann::json &graph_json,
                                                        size_t &declaration_index) {
    std::vector<FrameGraphNodeDefinition> nodes;
    if (!graph_json.contains("compute_tasks")) {
        return nodes;
    }
    const auto &tasks = graph_json.at("compute_tasks");
    if (!tasks.is_array()) {
        throw std::runtime_error("Frame graph compute_tasks must be an array");
    }
    nodes.reserve(tasks.size());
    for (const auto &task_json : tasks) {
        nodes.push_back(parseComputeNodeFromJson(task_json, declaration_index++));
    }
    return nodes;
}

std::string renderTargetResourceName(GlobalRenderTargetId id) {
    if (isSwapchainRenderTarget(id)) {
        return "swapchain";
    }
    if (isConcreteRenderTarget(id)) {
        return "rt:" + std::to_string(id.value);
    }
    return {};
}

FramePlanNodeKind passKind(const PassDefinition &) {
    return FramePlanNodeKind::render;
}

FrameGraphNodeDefinition makeRenderNodeDefinition(const PassDefinition &pass, size_t declaration_index) {
    FrameGraphNodeDefinition node;
    node.name = pass.name;
    node.kind = passKind(pass);
    node.declaration_index = declaration_index;
    node.view_family =
        pass.view_family;
    if (pass.isGenericRaster()) {
        node.semantic_dialect =
            "pelican.logical.raster@1";
        node.execution_implementation =
            "pelican.execution.generic_raster_direct@1";
    }
    node.region_tags = pass.region_tags;
    node.raster_geometry =
        pass.isMaterial() || pass.isShadowDepth() ||
        pass.isVelocity() || pass.isPicking() ||
        pass.isGenericRaster();
    if (pass.isFullscreen() ||
        pass.isGenericRaster()) {
        for (std::size_t index = 0;
             index < pass.input_targets.size();
             ++index) {
            if (!pass.input_target_history.at(index)) {
                appendUnique(
                    node.local_read_shader_inputs,
                    renderTargetResourceName(
                        pass.input_targets[index]));
            }
        }
    }
    node.resolution_domain =
        pass.resolution_domain;
    if (pass.isMaterial()) {
        node.material_filter =
            pass.materialInfo().material_filter;
        node.material_variant =
            pass.materialInfo().material_variant;
    }
    for (size_t i = 0; i < pass.input_targets.size(); ++i) {
        if (pass.input_target_history.at(i)) {
            appendUnique(node.reads_history, renderTargetResourceName(pass.input_targets[i]));
        } else {
            appendUnique(node.reads, renderTargetResourceName(pass.input_targets[i]));
        }
    }
    appendUnique(node.reads, pass.input_buffers);
    for (const auto &attachment : pass.output_color) {
        const auto target = attachment.target;
        const auto resource =
            renderTargetResourceName(target);
        appendUnique(node.writes, resource);
        if (!resource.empty()) {
            node.attachments.push_back(
                FrameGraphAttachmentDefinition{
                    .resource = resource,
                    .subresource =
                        attachment.subresource,
                    .aspect =
                        FrameGraphAttachmentAspect::color,
                    .load_op =
                        frameGraphAttachmentLoadOp(
                            pass.color_load_op),
                    .store_op =
                        frameGraphAttachmentStoreOp(
                            pass.color_store_op),
                });
        }
    }
    if (pass.isMaterial()) {
        for (const auto &input :
             pass.materialInfo().screen_inputs) {
            if (!input.history) {
                appendUnique(
                    node.local_read_shader_inputs,
                    renderTargetResourceName(
                        input.target));
            }
            appendReadFootprint(
                node,
                renderTargetResourceName(input.target),
                input.contract.footprint);
        }
        for (const auto &input :
             pass.materialInfo().surface_resources) {
            appendReadFootprint(
                node,
                renderTargetResourceName(input.target),
                input.contract.footprint);
        }
        for (const auto &input :
             pass.materialInfo().material_resources) {
            const auto resource =
                input.isBuffer()
                    ? input.buffer
                    : renderTargetResourceName(
                          input.target);
            if (!input.history) {
                if (input.isImage()) {
                    appendUnique(
                        node.local_read_shader_inputs,
                        resource);
                }
                appendReadFootprint(
                    node, resource,
                    input.footprint);
            }
            node.resource_accesses.push_back(
                FrameGraphResourceAccessDefinition{
                    .resource =
                        resource +
                        (input.history
                             ? "@history"
                             : ""),
                    .intent =
                        input.isBuffer()
                            ? LogicalAccessIntent::storage
                            : LogicalAccessIntent::sampled,
                });
        }
        if (const auto &draw_source =
                pass.materialInfo()
                    .gpu_draw_source) {
            appendUnique(
                node.reads,
                draw_source->commands);
            appendUnique(
                node.reads,
                draw_source->count);
        }
    }
    const auto depth_resource =
        renderTargetResourceName(pass.output_depth);
    appendUnique(node.writes, depth_resource);
    if (!depth_resource.empty()) {
        node.attachments.push_back(
            FrameGraphAttachmentDefinition{
                .resource = depth_resource,
                .subresource =
                    pass.output_depth.subresource,
                .aspect =
                    FrameGraphAttachmentAspect::depth,
                .load_op =
                    frameGraphAttachmentLoadOp(
                        pass.depth_load_op),
                .store_op =
                    frameGraphAttachmentStoreOp(
                        pass.depth_store_op),
            });
    }

    if (pass.color_load_op == vk::AttachmentLoadOp::eLoad) {
        for (const auto &attachment :
             pass.output_color) {
            const auto target = attachment.target;
            const auto resource =
                renderTargetResourceName(target);
            appendUnique(node.reads, resource);
            appendReadFootprint(
                node, resource,
                {LogicalReadFootprintKind::same_pixel,
                 std::nullopt});
        }
    }
    if (pass.depth_load_op == vk::AttachmentLoadOp::eLoad) {
        const auto resource =
            renderTargetResourceName(pass.output_depth);
        appendUnique(node.reads, resource);
        appendReadFootprint(
            node, resource,
            {LogicalReadFootprintKind::same_pixel,
             std::nullopt});
    }
    return node;
}

void validateUniqueNames(const FrameGraphDefinition &definition) {
    std::unordered_set<std::string> names;
    for (const auto &node : definition.nodes) {
        if (node.name.empty()) {
            throw std::runtime_error("Frame graph node name must not be empty");
        }
        if (!names.insert(node.name).second) {
            throw std::runtime_error("Duplicate frame graph node name: " + node.name);
        }
    }
}

std::unordered_map<std::string, size_t> buildNodeIndex(const FrameGraphDefinition &definition) {
    std::unordered_map<std::string, size_t> node_index;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        node_index.emplace(definition.nodes[i].name, i);
    }
    return node_index;
}

std::unordered_set<std::string> buildKnownResources(const FrameGraphDefinition &definition) {
    std::unordered_set<std::string> resources;
    resources.insert("swapchain");
    for (const auto &resource : definition.declared_resources) {
        if (!resource.empty()) {
            resources.insert(resource);
        }
    }
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.writes) {
            if (!resource.empty()) {
                resources.insert(resource);
            }
        }
    }
    return resources;
}

void validateKnownResources(const FrameGraphDefinition &definition) {
    const auto resources = buildKnownResources(definition);
    for (const auto &node : definition.nodes) {
        for (const auto &resource : node.reads) {
            if (resource.empty()) {
                continue;
            }
            if (resources.find(resource) == resources.end()) {
                throw std::runtime_error("Unknown resource reference in frame graph node " + node.name + ": " +
                                         resource);
            }
        }
        for (const auto &resource : node.reads_history) {
            if (std::find(definition.history_resources.begin(), definition.history_resources.end(),
                          resource) == definition.history_resources.end()) {
                throw std::runtime_error("Unknown or non-history resource reference in frame graph node " +
                                         node.name + ": " + resource + "@history");
            }
        }
    }
}

void addEdge(PlannerEdges &planner_edges, size_t from, size_t to) {
    if (from == to) {
        throw std::runtime_error("Frame graph contains a self dependency");
    }
    if (!planner_edges.exists[from][to]) {
        planner_edges.exists[from][to] = true;
        planner_edges.edges.push_back(Edge{from, to});
    }
}

void addBarrier(PlannerEdges &planner_edges, std::string kind,
                size_t from, size_t to, const std::string &resource,
                const FrameGraphDefinition &definition) {
    const auto barrier = FramePlanBarrier{
        std::move(kind),
        resource,
        definition.nodes[from].name,
        definition.nodes[to].name,
    };
    const auto exists = std::find_if(
        planner_edges.barriers.begin(), planner_edges.barriers.end(),
        [&barrier](const auto &existing) {
            return existing.resource == barrier.resource &&
                   existing.from == barrier.from &&
                   existing.to == barrier.to;
        });
    if (exists == planner_edges.barriers.end()) {
        planner_edges.barriers.push_back(barrier);
    }
}

void addDataEdge(PlannerEdges &planner_edges, size_t from, size_t to, const std::string &resource,
                 const FrameGraphDefinition &definition) {
    addEdge(planner_edges, from, to);
    addBarrier(planner_edges, "read_after_write", from, to, resource,
               definition);
}

void addBarriersForOrderedResourceEdges(PlannerEdges &planner_edges, const FrameGraphDefinition &definition) {
    for (const auto &edge : planner_edges.edges) {
        const auto &from = definition.nodes[edge.from];
        const auto &to = definition.nodes[edge.to];
        for (const auto &written : from.writes) {
            if (written.empty() || std::find(to.reads.begin(), to.reads.end(), written) == to.reads.end()) {
                continue;
            }
            addDataEdge(planner_edges, edge.from, edge.to, written, definition);
        }
    }
}

PlannerEdges buildEdges(const FrameGraphDefinition &definition,
                        const std::unordered_map<std::string, size_t> &node_index) {
    PlannerEdges planner_edges;
    planner_edges.exists.assign(definition.nodes.size(), std::vector<bool>(definition.nodes.size(), false));

    std::unordered_map<std::string, size_t> last_writer;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        const auto &node = definition.nodes[i];
        for (const auto &resource : node.reads) {
            auto found = last_writer.find(resource);
            if (found != last_writer.end()) {
                addDataEdge(planner_edges, found->second, i, resource, definition);
            }
        }
        for (const auto &resource : node.writes) {
            if (!resource.empty()) {
                last_writer[resource] = i;
            }
        }
    }

    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        const auto &node = definition.nodes[i];
        for (const auto &after : node.after) {
            const auto found = node_index.find(after);
            if (found == node_index.end()) {
                throw std::runtime_error("Frame graph after reference not found: " + after);
            }
            addEdge(planner_edges, found->second, i);
        }
        for (const auto &before : node.before) {
            const auto found = node_index.find(before);
            if (found == node_index.end()) {
                throw std::runtime_error("Frame graph before reference not found: " + before);
            }
            addEdge(planner_edges, i, found->second);
        }
    }

    addBarriersForOrderedResourceEdges(planner_edges, definition);
    return planner_edges;
}

std::vector<std::vector<bool>> transitiveClosure(std::vector<std::vector<bool>> reachability) {
    const auto count = reachability.size();
    for (size_t k = 0; k < count; ++k) {
        for (size_t i = 0; i < count; ++i) {
            if (!reachability[i][k]) {
                continue;
            }
            for (size_t j = 0; j < count; ++j) {
                reachability[i][j] = reachability[i][j] || reachability[k][j];
            }
        }
    }
    return reachability;
}

void validateWritesAreOrdered(const FrameGraphDefinition &definition,
                              const std::vector<std::vector<bool>> &reachability) {
    std::unordered_map<std::string, std::vector<size_t>> writers_by_resource;
    for (size_t i = 0; i < definition.nodes.size(); ++i) {
        for (const auto &resource : definition.nodes[i].writes) {
            if (!resource.empty()) {
                writers_by_resource[resource].push_back(i);
            }
        }
    }

    for (const auto &[resource, writers] : writers_by_resource) {
        for (size_t i = 0; i < writers.size(); ++i) {
            for (size_t j = i + 1; j < writers.size(); ++j) {
                const auto lhs = writers[i];
                const auto rhs = writers[j];
                if (!reachability[lhs][rhs] && !reachability[rhs][lhs]) {
                    throw std::runtime_error("Ambiguous writes-writes dependency for resource " + resource +
                                             " between " + definition.nodes[lhs].name + " and " +
                                             definition.nodes[rhs].name);
                }
            }
        }
    }
}

void addWriteAfterWriteBarriers(
    PlannerEdges &planner_edges,
    const FrameGraphDefinition &definition,
    const std::vector<size_t> &order) {
    std::unordered_map<std::string, size_t> last_writer;
    for (const auto node_index : order) {
        for (const auto &resource :
             definition.nodes[node_index].writes) {
            if (resource.empty()) continue;
            const auto previous = last_writer.find(resource);
            if (previous != last_writer.end()) {
                // Writer ordering is validated before this point. The edge
                // may be transitive, but Vulkan still needs an explicit
                // memory dependency between consecutive physical writers.
                addBarrier(planner_edges, "write_after_write",
                           previous->second, node_index, resource,
                           definition);
            }
            last_writer[resource] = node_index;
        }
    }
}

std::vector<size_t> topologicalOrder(const FrameGraphDefinition &definition, const PlannerEdges &planner_edges) {
    const auto count = definition.nodes.size();
    std::vector<size_t> indegree(count, 0);
    std::vector<std::vector<size_t>> adjacency(count);
    for (const auto &edge : planner_edges.edges) {
        adjacency[edge.from].push_back(edge.to);
        ++indegree[edge.to];
    }

    std::set<std::pair<size_t, size_t>> ready;
    for (size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            ready.insert({definition.nodes[i].declaration_index, i});
        }
    }

    std::vector<size_t> order;
    order.reserve(count);
    while (!ready.empty()) {
        const auto [unused_index, node_index] = *ready.begin();
        (void)unused_index;
        ready.erase(ready.begin());
        order.push_back(node_index);
        for (const auto next : adjacency[node_index]) {
            --indegree[next];
            if (indegree[next] == 0) {
                ready.insert({definition.nodes[next].declaration_index, next});
            }
        }
    }

    if (order.size() != count) {
        throw std::runtime_error("Cycle detected in frame graph: " + definition.name);
    }
    return order;
}

std::vector<size_t> computeLevels(size_t node_count, const PlannerEdges &planner_edges,
                                  const std::vector<size_t> &order) {
    std::vector<size_t> levels(node_count, 0);
    for (const auto node_index : order) {
        for (const auto &edge : planner_edges.edges) {
            if (edge.to == node_index) {
                levels[node_index] = std::max(levels[node_index], levels[edge.from] + 1);
            }
        }
    }
    return levels;
}

} // namespace

std::string framePlanNodeKindName(FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
        return "render";
    case FramePlanNodeKind::compute:
        return "compute";
    case FramePlanNodeKind::anchor:
        return "anchor";
    case FramePlanNodeKind::snapshot_copy:
        return "snapshot_copy";
    case FramePlanNodeKind::output_transform:
        return "output_transform";
    }
    throw std::runtime_error("unknown frame plan node kind");
}

FrameGraphDefinition makeFrameGraphDefinition(const RenderingPassDefinition &definition) {
    FrameGraphDefinition graph;
    graph.name = definition.name;
    graph.nodes.reserve(definition.passes.size());
    for (size_t i = 0; i < definition.passes.size(); ++i) {
        auto node = makeRenderNodeDefinition(definition.passes[i], i);
        appendUnique(graph.history_resources, node.reads_history);
        graph.nodes.push_back(std::move(node));
    }
    return graph;
}

FrameGraphDefinition parseFrameGraphDefinitionFromJson(const nlohmann::json &graph_json) {
    if (!graph_json.is_object()) {
        throw std::runtime_error("Frame graph definition must be an object");
    }

    FrameGraphDefinition graph;
    graph.name = graph_json.value("name", std::string{"frame_graph"});
    appendUnique(graph.declared_resources, parseDeclaredRenderTargets(graph_json));
    appendUnique(graph.history_resources, parseHistoryRenderTargets(graph_json));
    appendUnique(graph.declared_resources, parseDeclaredBuffers(graph_json));
    size_t declaration_index = 0;
    appendNodes(graph.nodes, parseRenderNodes(graph_json, declaration_index));
    appendNodes(graph.nodes, parseComputeNodes(graph_json, declaration_index));
    const auto local_sizes = parseRenderTargetByteSizes(graph_json);
    if (!local_sizes.empty()) assignSnapshotByteSizes(graph.nodes, local_sizes);
    return graph;
}

std::vector<FrameGraphDefinition> parseFrameGraphDefinitionsFromConfigJson(const nlohmann::json &config_json) {
    if (!config_json.is_object()) {
        throw std::runtime_error("Frame graph config must be an object");
    }

    std::vector<FrameGraphDefinition> graphs;
    if (!config_json.contains("rendering_passes")) {
        if (config_json.contains("passes") || config_json.contains("compute_tasks")) {
            graphs.push_back(parseFrameGraphDefinitionFromJson(config_json));
        }
        return graphs;
    }

    const auto &rendering_passes = config_json.at("rendering_passes");
    if (!rendering_passes.is_array()) {
        throw std::runtime_error("Frame graph rendering_passes must be an array");
    }

    const auto declared_targets = parseDeclaredRenderTargets(config_json);
    const auto history_targets = parseHistoryRenderTargets(config_json);
    const auto declared_buffers = parseDeclaredBuffers(config_json);
    const auto render_target_sizes = parseRenderTargetByteSizes(config_json);
    for (const auto &pass_set_json : rendering_passes) {
        auto graph = parseFrameGraphDefinitionFromJson(pass_set_json);
        appendUnique(graph.declared_resources, declared_targets);
        appendUnique(graph.history_resources, history_targets);
        appendUnique(graph.declared_resources, declared_buffers);
        size_t declaration_index = graph.nodes.size();
        appendNodes(graph.nodes, parseComputeNodes(config_json, declaration_index));
        // Raw authored configs do not yet contain the resolved display extent.
        // Preserve an unknown byte size for logical shadow planning; resolved
        // runtime configs still require every snapshot target to have a size.
        if (!render_target_sizes.empty()) {
            assignSnapshotByteSizes(graph.nodes, render_target_sizes);
        }
        graphs.push_back(std::move(graph));
    }

    if (graphs.empty() && config_json.contains("compute_tasks")) {
        FrameGraphDefinition graph;
        graph.name = "frame_graph";
        appendUnique(graph.declared_resources, declared_targets);
        appendUnique(graph.history_resources, history_targets);
        appendUnique(graph.declared_resources, declared_buffers);
        size_t declaration_index = 0;
        appendNodes(graph.nodes, parseComputeNodes(config_json, declaration_index));
        graphs.push_back(std::move(graph));
    }
    return graphs;
}

FramePlan planFrameGraph(const FrameGraphDefinition &definition) {
    validateUniqueNames(definition);
    validateKnownResources(definition);
    const auto node_index = buildNodeIndex(definition);
    auto planner_edges = buildEdges(definition, node_index);
    const auto reachability = transitiveClosure(planner_edges.exists);
    validateWritesAreOrdered(definition, reachability);
    const auto order = topologicalOrder(definition, planner_edges);
    addWriteAfterWriteBarriers(planner_edges, definition, order);
    const auto levels_by_node = computeLevels(definition.nodes.size(), planner_edges, order);

    FramePlan plan;
    plan.name = definition.name;
    plan.barriers = planner_edges.barriers;
    plan.nodes.reserve(order.size());

    size_t max_level = 0;
    for (size_t order_index = 0; order_index < order.size(); ++order_index) {
        const auto node_index_value = order[order_index];
        const auto &node_def = definition.nodes[node_index_value];
        const auto level = levels_by_node[node_index_value];
        max_level = std::max(max_level, level);
        plan.nodes.push_back(FramePlanNode{
            node_def.name,
            node_def.kind,
            node_def.declaration_index,
            order_index,
            level,
            node_def.reads,
            node_def.reads_history,
            node_def.writes,
            node_def.snapshot_after,
            node_def.byte_size,
            node_def.material_filter,
            node_def.material_variant,
            node_def.view_family,
        });
    }

    plan.levels.resize(max_level + 1);
    for (const auto &node : plan.nodes) {
        plan.levels[node.level].push_back(node.name);
    }
    return plan;
}

std::vector<std::string> framePlanOrder(const FramePlan &plan) {
    std::vector<std::string> order;
    order.reserve(plan.nodes.size());
    for (const auto &node : plan.nodes) {
        order.push_back(node.name);
    }
    return order;
}

nlohmann::json framePlanToJson(
    const FramePlan &plan,
    const CompiledRenderPipeline *render_pipeline) {
    std::unordered_map<std::string, const RenderPassProvenance *>
        pass_provenance;
    if (render_pipeline != nullptr) {
        pass_provenance.reserve(
            render_pipeline->pass_provenance.size());
        for (const auto &entry :
             render_pipeline->pass_provenance) {
            pass_provenance.emplace(entry.name, &entry);
        }
    }
    auto nodes_json = nlohmann::json::array();
    for (const auto &node : plan.nodes) {
        auto node_json = nlohmann::json{
            {"declaration_index", node.declaration_index},
            {"kind", framePlanNodeKindName(node.kind)},
            {"level", node.level},
            {"name", node.name},
            {"order", node.order},
            {"reads", node.reads},
            {"reads_history", node.reads_history},
            {"writes", node.writes},
        };
        if (const auto found = pass_provenance.find(node.name);
            found != pass_provenance.end()) {
            const auto &provenance = *found->second;
            node_json["source"] =
                renderPipelineProvenanceSourceName(
                    provenance.source,
                    provenance.provider_feature);
            if (!provenance.provider_feature.empty()) {
                node_json["provider_feature"] =
                    provenance.provider_feature;
            }
            if (!provenance.provider_reference.empty()) {
                node_json["provider_ref"] =
                    provenance.provider_reference;
            }
        }
        if (node.kind == FramePlanNodeKind::snapshot_copy) {
            node_json["byte_size"] = node.byte_size;
            node_json["snapshot_after"] = node.snapshot_after;
            node_json["semantics"] = "fixed_once_before_transparency";
            node_json["sequential_refraction"] = false;
        }
        if (node.material_filter) {
            node_json["material_filter"] = {
                {"include",
                 node.material_filter->include},
                {"exclude",
                 node.material_filter->exclude},
                {"filter_id",
                 stableFingerprint64String(
                     node.material_filter
                         ->id.value)},
                {"resolved_draw_count", nullptr},
                {"resolution_provenance",
                 materialDrawTagFilterProvenance},
                {"resolution_state",
                 "pending_draw_queue_compile"},
            };
        }
        if (node.material_variant) {
            node_json["material_variant"] =
                *node.material_variant;
        }
        if (node.view_family !=
            mainRenderViewFamilyId) {
            node_json["view_family"] =
                node.view_family;
        }
        nodes_json.push_back(std::move(node_json));
    }

    auto barriers_json = nlohmann::json::array();
    for (const auto &barrier : plan.barriers) {
        barriers_json.push_back(nlohmann::json{
            {"from", barrier.from},
            {"kind", barrier.kind},
            {"resource", barrier.resource},
            {"to", barrier.to},
        });
    }

    auto result = nlohmann::json{
        {"barriers", barriers_json},
        {"graph", plan.name},
        {"levels", plan.levels},
        {"nodes", nodes_json},
        {"schema", "pelican.frame_plan"},
        {"version", 1},
    };
    if (render_pipeline != nullptr) {
        const auto metadata =
            serializeCompiledRenderPipelineMetadata(*render_pipeline);
        for (auto field = metadata.begin(); field != metadata.end(); ++field) {
            result[field.key()] = field.value();
        }
        if (!render_pipeline->resource_provenance.empty()) {
            auto resources = nlohmann::json::array();
            for (const auto &provenance :
                 render_pipeline->resource_provenance) {
                auto resource = nlohmann::json{
                    {"kind", provenance.kind},
                    {"name", provenance.name},
                    {"source",
                     renderPipelineProvenanceSourceName(
                         provenance.source,
                         provenance.provider_feature)},
                };
                if (!provenance.provider_feature.empty()) {
                    resource["provider_feature"] =
                        provenance.provider_feature;
                }
                if (!provenance.provider_reference.empty()) {
                    resource["provider_ref"] =
                        provenance.provider_reference;
                }
                if (provenance.usage) {
                    resource["usage"] = *provenance.usage;
                }
                resources.push_back(std::move(resource));
            }
            result["resources"] = std::move(resources);
        }
    }
    return result;
}

void applyMaterialDrawFilterResolutionToFramePlanJson(
    nlohmann::json &plan_json,
    std::string_view pass_name,
    MaterialDrawTagFilterId filter_id,
    std::size_t resolved_draw_count,
    const std::vector<std::string> &unmatched_include,
    const std::vector<std::string> &unmatched_exclude) {
    if (!plan_json.is_object() ||
        !plan_json.contains("nodes") ||
        !plan_json.at("nodes").is_array()) {
        throw std::invalid_argument(
            "frame plan JSON has no node array");
    }
    const auto node = std::find_if(
        plan_json["nodes"].begin(),
        plan_json["nodes"].end(),
        [&](const auto &candidate) {
            return candidate.is_object() &&
                   candidate.value(
                       "name", std::string{}) ==
                       pass_name;
        });
    if (node == plan_json["nodes"].end() ||
        !node->contains("material_filter") ||
        !node->at("material_filter").is_object()) {
        throw std::invalid_argument(
            "frame plan material filter node is unavailable: " +
            std::string{pass_name});
    }
    auto &metadata =
        (*node)["material_filter"];
    const auto expected_id =
        stableFingerprint64String(
            filter_id.value);
    if (metadata.value(
            "filter_id", std::string{}) !=
        expected_id) {
        throw std::invalid_argument(
            "frame plan material filter id mismatch: " +
            std::string{pass_name});
    }
    metadata["resolved_draw_count"] =
        resolved_draw_count;
    metadata["unmatched_include"] =
        unmatched_include;
    metadata["unmatched_exclude"] =
        unmatched_exclude;
    metadata["resolution_state"] =
        "resolved";
}

} // namespace Pelican
