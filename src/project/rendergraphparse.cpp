#include "rendergraphparse.hpp"

#include "framegraphbufferdefinition.hpp"
#include "graphcomposition.hpp"
#include "imagesource.hpp"
#include "imagesubresourcejson.hpp"
#include "logicalrendertype.hpp"
#include "materialformat.hpp"
#include "materialscreeninput.hpp"
#include "passshapepolicy.hpp"
#include "passshaderdefines.hpp"
#include "shadersourceresolver.hpp"
#include "rasterrayquery.hpp"
#include "rayrecordabi.hpp"
#include "raytracingrefusal.hpp"
#include "renderconfigsection.hpp"
#include "renderingjsonnormalize.hpp"
#include "renderresourcename.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

using Json = nlohmann::json;

[[noreturn]] void refuse(std::string_view code, Json detail) {
    throw GraphRegistryError(std::string{code}, std::move(detail));
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

// Where a refusal points: the graph and the node being read.
struct Subject {
    std::string graph;
    std::string node;

    Json detail(Json more = Json::object()) const {
        Json result = std::move(more);
        if (!graph.empty()) result["graph"] = graph;
        if (!node.empty()) result["node"] = node;
        return result;
    }

    [[noreturn]] void field(std::string_view field, std::string_view reason,
                            Json more = Json::object()) const {
        more["field"] = std::string{field};
        more["reason"] = std::string{reason};
        refuse("render_graph.node_field_invalid@1", detail(std::move(more)));
    }
};

// Runs a shared project helper; its refusal becomes a named one.
template <class Fn>
auto guarded(const Subject &subject, std::string_view field,
             std::string_view reason, Fn &&fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const PassShaderDefineError &) {
        throw;
    } catch (const GraphRegistryError &) {
        throw;
    } catch (const std::exception &error) {
        subject.field(field, reason, {{"detail", error.what()}});
    }
}

std::string requiredString(const Json &object, std::string_view key,
                           const Subject &subject, std::string_view field) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        subject.field(field, "requires_string");
    }
    return found->get<std::string>();
}

// The normal form of input / reads / writes / after / before: an array of
// strings (null and absent are none, as the planner read them).
std::vector<std::string> nameList(const Json &declaration, std::string_view key,
                                  const Subject &subject) {
    const auto found = declaration.find(std::string{key});
    if (found == declaration.end() || found->is_null()) return {};
    if (found->is_string()) {
        refuse(renderJsonNotNormalizedCode,
               {{"path", subject.node + "/" + std::string{key}}});
    }
    if (!found->is_array()) subject.field(key, "not_a_string_array");
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto &entry : *found) {
        if (!entry.is_string()) subject.field(key, "not_a_string_array");
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> regionTags(const Json &declaration,
                                    const Subject &subject) {
    const auto found = declaration.find("regions");
    if (found == declaration.end()) return {};
    if (!found->is_array()) subject.field("regions", "not_a_string_array");
    std::vector<std::string> result;
    std::set<std::string, std::less<>> unique;
    for (const auto &encoded : *found) {
        if (!encoded.is_string()) subject.field("regions", "not_a_string_array");
        auto region = encoded.get<std::string>();
        if (region.empty() || region.size() > maximumLogicalRegionTagBytes) {
            subject.field("regions", "empty_or_too_long", {{"region", region}});
        }
        if (!unique.insert(region).second) {
            subject.field("regions", "duplicate", {{"region", region}});
        }
        result.push_back(std::move(region));
    }
    return result;
}

std::string viewFamily(const Json &declaration, const Subject &subject) {
    auto family = std::string{mainRenderViewFamilyId};
    if (const auto found = declaration.find("view_family"); found != declaration.end()) {
        if (!found->is_string()) subject.field("view_family", "requires_string");
        family = found->get<std::string>();
    }
    guarded(subject, "view_family", "invalid", [&] {
        validateRenderViewFamilyId(family, subject.node);
        return 0;
    });
    return family;
}

RenderViewSelect viewSelect(const Json &declaration, const Subject &subject) {
    RenderViewSelect result;
    const auto found = declaration.find("view_select");
    if (found == declaration.end()) return result;
    if (!found->is_object() || found->empty()) {
        subject.field("view_select", "not_a_non_empty_object");
    }
    for (auto entry = found->begin(); entry != found->end(); ++entry) {
        if (entry.key().empty()) subject.field("view_select", "empty_axis");
        const auto &value = entry.value();
        if (!value.is_number_integer() ||
            (value.is_number_unsigned()
                 ? value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()
                 : value.get<std::int64_t>() < 0 ||
                       value.get<std::int64_t>() >
                           static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))) {
            subject.field("view_select", "axis_not_uint32", {{"axis", entry.key()}});
        }
        result.emplace(entry.key(),
                       static_cast<std::uint32_t>(
                           value.is_number_unsigned()
                               ? value.get<std::uint64_t>()
                               : static_cast<std::uint64_t>(value.get<std::int64_t>())));
    }
    return result;
}

std::uint32_t uint32Field(const Json &object, std::string_view key,
                          const Subject &subject, std::string_view field) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_number_integer()) {
        subject.field(field, "requires_unsigned_integer");
    }
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value = found->get<std::int64_t>();
        if (signed_value < 0) subject.field(field, "requires_unsigned_integer");
        value = static_cast<std::uint64_t>(signed_value);
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        subject.field(field, "too_large");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t byteCountField(const Json &object, std::string_view key,
                             const Subject &subject, std::string_view field) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) return 0;
    if (!found->is_number_unsigned() &&
        !(found->is_number_integer() && found->get<std::int64_t>() >= 0)) {
        subject.field(field, "requires_unsigned_integer");
    }
    return found->get<std::uint64_t>();
}

// ---- footprints (the planner's rules: a string or {kind, radius}; temporal
// is spelled @history)

LogicalReadFootprint footprintOf(const Json &encoded, const Subject &subject,
                                 std::string_view field,
                                 const std::string &resource) {
    const auto invalid = [&](std::string_view reason) {
        refuse("render_graph.footprint_invalid@1",
               subject.detail({{"field", std::string{field}},
                               {"resource", resource},
                               {"reason", std::string{reason}}}));
    };
    const auto kindOf = [&](const std::string &value) {
        if (value == "same_pixel") return LogicalReadFootprintKind::same_pixel;
        if (value == "neighborhood") return LogicalReadFootprintKind::neighborhood;
        if (value == "arbitrary") return LogicalReadFootprintKind::arbitrary;
        if (value == "temporal") invalid("temporal_is_history");
        invalid("unknown_kind");
    };
    LogicalReadFootprint result;
    if (encoded.is_string()) {
        result.kind = kindOf(encoded.get<std::string>());
        return result;
    }
    if (!encoded.is_object()) invalid("not_a_string_or_object");
    for (auto entry = encoded.begin(); entry != encoded.end(); ++entry) {
        if (entry.key() != "kind" && entry.key() != "radius") invalid("unknown_field");
    }
    if (!encoded.contains("kind") || !encoded.at("kind").is_string()) invalid("requires_kind");
    result.kind = kindOf(encoded.at("kind").get<std::string>());
    if (!encoded.contains("radius")) return result;
    const auto &radius = encoded.at("radius");
    if (!radius.is_number_unsigned()) invalid("radius_not_unsigned");
    const auto value = radius.get<std::uint64_t>();
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        invalid("radius_out_of_range");
    }
    if (result.kind != LogicalReadFootprintKind::neighborhood) invalid("radius_without_neighborhood");
    result.radius = static_cast<std::uint32_t>(value);
    return result;
}

// input_footprints / read_footprints: an object keyed by a current read.
void declaredFootprints(const Json &source, std::string_view field,
                        std::vector<ResourceUse> &uses,
                        const std::vector<std::string> &current_reads,
                        const Subject &subject) {
    const auto found = source.find(std::string{field});
    if (found == source.end()) return;
    if (!found->is_object()) subject.field(field, "not_an_object");
    for (auto entry = found->begin(); entry != found->end(); ++entry) {
        if (std::find(current_reads.begin(), current_reads.end(), entry.key()) ==
            current_reads.end()) {
            refuse("render_graph.footprint_invalid@1",
                   subject.detail({{"field", std::string{field}},
                                   {"resource", entry.key()},
                                   {"reason", "not_a_current_read"}}));
        }
        const auto footprint =
            footprintOf(entry.value(), subject, field, entry.key());
        for (auto &use : uses) {
            if (use.history || use.resource != entry.key()) continue;
            if (use.role != ResourceUseRole::input &&
                use.role != ResourceUseRole::indirect) continue;
            use.footprint = footprint;
        }
    }
}

// `<name>` or `<name>@history`; any other qualifier is refused.
std::pair<std::string, bool> historyReference(const std::string &authored,
                                              const Subject &subject,
                                              std::string_view field) {
    constexpr std::string_view suffix = "@history";
    const bool history = authored.ends_with(suffix);
    auto name = history ? authored.substr(0, authored.size() - suffix.size()) : authored;
    if ((history && name.empty()) || name.find('@') != std::string::npos) {
        refuse("render_graph.resource_reference_invalid@1",
               subject.detail({{"field", std::string{field}}, {"resource", authored}}));
    }
    return {std::move(name), history};
}

ResourceBinding bindingOf(const ShaderResourcePortDefinition &port) {
    return ResourceBinding{
        .port = port.name,
        .location = std::nullopt,
        .kind = port.kind,
        .access = port.access,
        .view = port.view,
        .sampling = port.sampling,
        .element = port.buffer_element,
        .domain = port.domain,
    };
}

// resource_ports: a typed binding of a resource an input or output use
// names. The access is the port's effective one (an image's).
void appendResourcePorts(const Json &owner, std::span<const std::string> authored_reads,
                         std::span<const std::string> writes, NodeIR &node,
                         const Subject &subject) {
    const auto ports = guarded(subject, "resource_ports", "invalid", [&] {
        return parseShaderResourcePortDefinitions(
            owner, authored_reads, writes, "render graph node '" + node.name + "'");
    });
    for (const auto &port : ports) {
        const bool written =
            std::find(writes.begin(), writes.end(), port.resource) != writes.end();
        const auto access = effectiveShaderResourcePortAccess(port, true, written);
        constexpr std::string_view suffix = "@history";
        const bool history = port.resource.ends_with(suffix);
        ResourceUse use;
        use.resource = history ? port.resource.substr(0, port.resource.size() - suffix.size())
                               : port.resource;
        use.role = ResourceUseRole::resource_port;
        // The port's own direction, with its subresource: a sampled view
        // reads (a task may sample one mip and store another of the image it
        // writes); a storage view of a written resource writes it.
        use.access = access == ShaderResourcePortAccess::sampled || !written
                         ? ResourceUseAccess::read
                         : ResourceUseAccess::write;
        use.intent = access == ShaderResourcePortAccess::sampled ? LogicalAccessIntent::sampled
                                                                 : LogicalAccessIntent::storage;
        use.history = history;
        if (history) use.footprint = undeclaredReadFootprint(true);
        use.subresource = port.subresource;
        use.binding = bindingOf(port);
        node.uses.push_back(std::move(use));
    }
}

// ---- attachments

bool hasAttachment(const Json &encoded) {
    if (encoded.is_null()) return false;
    if (encoded.is_string()) return !encoded.get_ref<const std::string &>().empty();
    if (encoded.is_object()) {
        const auto target = encoded.find("target");
        return target != encoded.end() && target->is_string() &&
               !target->get_ref<const std::string &>().empty();
    }
    if (encoded.is_array()) {
        return std::any_of(encoded.begin(), encoded.end(),
                           [](const auto &entry) { return hasAttachment(entry); });
    }
    return false;
}

void requireAttachmentOptionsHaveOutputs(const Json &pass, const Subject &subject) {
    const auto output = pass.find("output");
    const auto has_output = [&](std::string_view aspect) {
        if (output == pass.end() || !output->is_object()) return false;
        const auto found = output->find(std::string{aspect});
        return found != output->end() && hasAttachment(*found);
    };
    for (const auto &[field, aspect] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"clear_color", "color"},
             {"clear_colors", "color"},
             {"color_load_op", "color"},
             {"color_store_op", "color"},
             {"depth_load_op", "depth"},
             {"depth_store_op", "depth"}}) {
        if (pass.contains(std::string{field}) && !has_output(aspect)) {
            refuse("render_graph.attachment_option_without_output@1",
                   subject.detail({{"field", std::string{field}}}));
        }
    }
}

// The normal form spells each op once ("clear" / "load" / "dont_care",
// "store" / "dont_care"); another authored spelling did not pass the
// normalization.
template <class Op, class Find, class Name>
Op canonicalOp(const std::string &spelling, Find find, Name name,
               const std::function<void(std::string_view)> &unknown,
               std::string_view path) {
    const auto op = find(spelling);
    if (!op) unknown("unknown_op");
    if (name(*op) != spelling) {
        refuse(renderJsonNotNormalizedCode, {{"path", std::string{path}}});
    }
    return *op;
}

struct ParsedAttachment {
    std::string target;
    std::optional<ImageSubresourceRange> subresource;
    std::optional<AttachmentLoadOp> load_op;
    std::optional<AttachmentStoreOp> store_op;
};

ParsedAttachment attachmentOf(const Json &encoded, std::string_view field,
                              const Subject &subject) {
    const auto invalid = [&](std::string_view reason, Json more = Json::object()) {
        more["field"] = std::string{field};
        more["reason"] = std::string{reason};
        refuse("render_graph.attachment_invalid@1", subject.detail(std::move(more)));
    };
    if (encoded.is_string()) {
        refuse(renderJsonNotNormalizedCode,
               {{"path", subject.node + "/" + std::string{field}}});
    }
    if (!encoded.is_object()) invalid("not_an_attachment");
    for (auto entry = encoded.begin(); entry != encoded.end(); ++entry) {
        if (entry.key() != "target" && entry.key() != "subresource" &&
            entry.key() != "load_op" && entry.key() != "store_op") {
            invalid("unknown_field", {{"key", entry.key()}});
        }
    }
    if (!encoded.contains("target") || !encoded.at("target").is_string()) {
        invalid("requires_target");
    }
    ParsedAttachment result;
    result.target = encoded.at("target").get<std::string>();
    try {
        result.subresource = parseOptionalImageSubresource(encoded, field);
    } catch (const std::exception &error) {
        invalid("subresource_invalid", {{"target", result.target}, {"detail", error.what()}});
    }
    if (result.subresource &&
        (result.subresource->mip_count_mode != ImageSubresourceMipCountMode::fixed ||
         result.subresource->level_count != 1)) {
        invalid("subresource_not_one_mip", {{"target", result.target}});
    }
    const auto path = subject.node + "/" + std::string{field};
    if (const auto found = encoded.find("load_op"); found != encoded.end()) {
        if (!found->is_string()) invalid("op_not_a_string", {{"target", result.target}});
        result.load_op = canonicalOp<AttachmentLoadOp>(
            found->get<std::string>(), findAuthoredAttachmentLoadOp, attachmentLoadOpName,
            [&](std::string_view reason) {
                invalid(reason, {{"target", result.target}, {"value", found->get<std::string>()}});
            },
            path);
    }
    if (const auto found = encoded.find("store_op"); found != encoded.end()) {
        if (!found->is_string()) invalid("op_not_a_string", {{"target", result.target}});
        result.store_op = canonicalOp<AttachmentStoreOp>(
            found->get<std::string>(), findAuthoredAttachmentStoreOp, attachmentStoreOpName,
            [&](std::string_view reason) {
                invalid(reason, {{"target", result.target}, {"value", found->get<std::string>()}});
            },
            path);
    }
    if (result.target.empty()) invalid("target_empty");
    if (result.target == "swapchain" && result.subresource) {
        invalid("swapchain_subresource", {{"target", result.target}});
    }
    return result;
}

template <class Op, class Find, class Name>
std::optional<Op> passOp(const Json &pass, std::string_view field, Find find, Name name,
                         const Subject &subject) {
    const auto found = pass.find(std::string{field});
    if (found == pass.end()) return std::nullopt;
    if (!found->is_string()) subject.field(field, "requires_string");
    return canonicalOp<Op>(
        found->get<std::string>(), find, name,
        [&](std::string_view reason) {
            subject.field(field, reason, {{"value", found->get<std::string>()}});
        },
        subject.node + "/" + std::string{field});
}

// ---- material pass inputs

std::optional<MaterialDrawTagFilter> materialFilter(const Json &pass, const Subject &subject) {
    const auto found = pass.find("material_filter");
    if (found == pass.end()) return std::nullopt;
    if (!found->is_object()) subject.field("material_filter", "not_an_object");
    for (auto entry = found->begin(); entry != found->end(); ++entry) {
        if (entry.key() != "include" && entry.key() != "exclude") {
            subject.field("material_filter", "unknown_field", {{"key", entry.key()}});
        }
    }
    const auto tags = [&](const char *field) {
        std::vector<std::string> result;
        const auto entry = found->find(field);
        if (entry == found->end()) return result;
        if (!entry->is_array()) subject.field("material_filter", "not_a_string_array");
        for (const auto &tag : *entry) {
            if (!tag.is_string()) subject.field("material_filter", "not_a_string_array");
            result.push_back(tag.get<std::string>());
        }
        return result;
    };
    auto include = tags("include");
    auto exclude = tags("exclude");
    return guarded(subject, "material_filter", "invalid", [&] {
        return makeMaterialDrawTagFilter(std::move(include), std::move(exclude),
                                         "render graph material pass '" + subject.node + "'");
    });
}

std::optional<GpuDrawSourceDeclaration> gpuDrawSource(const Json &pass, const Subject &subject) {
    const auto found = pass.find("gpu_draw_source");
    if (found == pass.end()) return std::nullopt;
    const auto invalid = [&](std::string_view reason) {
        subject.field("gpu_draw_source", reason);
    };
    if (!found->is_object()) invalid("not_an_object");
    static constexpr std::array known{
        std::string_view{"commands"}, std::string_view{"count"},
        std::string_view{"max_draw_count"}, std::string_view{"command_offset"},
        std::string_view{"count_offset"}, std::string_view{"layout"},
        std::string_view{"segments"}, std::string_view{"fallback"},
        std::string_view{"execution"}};
    for (auto field = found->begin(); field != found->end(); ++field) {
        if (std::find(known.begin(), known.end(), field.key()) == known.end()) {
            subject.field("gpu_draw_source", "unknown_field", {{"key", field.key()}});
        }
    }
    GpuDrawSourceDeclaration result{
        .commands = requiredString(*found, "commands", subject, "gpu_draw_source.commands"),
        .count = requiredString(*found, "count", subject, "gpu_draw_source.count"),
        .max_draw_count = uint32Field(*found, "max_draw_count", subject,
                                      "gpu_draw_source.max_draw_count"),
        .command_offset = byteCountField(*found, "command_offset", subject,
                                         "gpu_draw_source.command_offset"),
        .count_offset = byteCountField(*found, "count_offset", subject,
                                       "gpu_draw_source.count_offset"),
    };
    if (found->contains("layout")) {
        const auto layout = requiredString(*found, "layout", subject, "gpu_draw_source.layout");
        if (layout == "fixed_state_v1") {
            result.layout = GpuDrawSourceLayout::fixed_state_v1;
        } else if (layout == "draw_queue_segments_v1") {
            result.layout = GpuDrawSourceLayout::draw_queue_segments_v1;
        } else {
            invalid("unknown_layout");
        }
    }
    if (found->contains("segments")) {
        result.segments = requiredString(*found, "segments", subject, "gpu_draw_source.segments");
    }
    if (result.commands.empty() || result.count.empty()) invalid("empty_buffer_name");
    if (result.commands == result.count) invalid("commands_equal_count");
    if (result.layout == GpuDrawSourceLayout::fixed_state_v1 && !result.segments.empty()) {
        invalid("fixed_state_with_segments");
    }
    if (result.layout == GpuDrawSourceLayout::draw_queue_segments_v1 && result.segments.empty()) {
        invalid("segments_required");
    }
    if (!result.segments.empty() &&
        (result.segments == result.commands || result.segments == result.count)) {
        invalid("buffer_names_not_distinct");
    }
    if (result.max_draw_count == 0) invalid("max_draw_count_zero");
    if (result.command_offset % frameGraphIndirectCommandAlignment != 0 ||
        result.count_offset % frameGraphIndirectCommandAlignment != 0) {
        invalid("offset_unaligned");
    }
    if (found->contains("fallback")) {
        if (requiredString(*found, "fallback", subject, "gpu_draw_source.fallback") !=
            "cpu_draw_queue") {
            invalid("unknown_fallback");
        }
    }
    if (found->contains("execution")) {
        const auto execution =
            requiredString(*found, "execution", subject, "gpu_draw_source.execution");
        if (execution == "automatic") {
            result.execution = GpuDrawExecutionMode::automatic;
        } else if (execution == "cpu") {
            result.execution = GpuDrawExecutionMode::cpu;
        } else {
            invalid("unknown_execution");
        }
    }
    const auto range = pass.find("material_range");
    const auto range_count =
        range != pass.end() && range->is_object() && range->contains("count")
            ? std::optional{uint32Field(*range, "count", subject, "material_range.count")}
            : std::nullopt;
    if (result.layout == GpuDrawSourceLayout::fixed_state_v1 &&
        range_count != std::optional<std::uint32_t>{1}) {
        invalid("fixed_state_requires_one_material");
    }
    if (result.layout == GpuDrawSourceLayout::draw_queue_segments_v1 &&
        (!range_count || *range_count == 0)) {
        invalid("segments_require_material_range");
    }
    return result;
}

// screen_inputs / surface_resources: an object of contract name -> resource.
void appendContractInputs(const Json &pass, std::string_view field, ResourceUseRole role,
                          NodeIR &node, const LogicalTypeRegistry &types,
                          const Subject &subject) {
    const auto found = pass.find(std::string{field});
    if (found == pass.end()) return;
    if (!found->is_object()) subject.field(field, "not_an_object");
    for (auto entry = found->begin(); entry != found->end(); ++entry) {
        if (!entry.value().is_string()) {
            subject.field(field, "entry_not_a_resource", {{"key", entry.key()}});
        }
        ResourceUse use;
        use.resource = entry.value().get<std::string>();
        use.role = role;
        use.binding.port = entry.key();
        use.footprint = guarded(subject, field, "unknown_contract", [&] {
            return role == ResourceUseRole::screen_input
                       ? makeBuiltinMaterialScreenInputContract(types, entry.key()).footprint
                       : makeBuiltinMaterialPassInputContract(types, entry.key()).footprint;
        });
        node.uses.push_back(std::move(use));
    }
}

void appendMaterialResources(const Json &pass, NodeIR &node, const Subject &subject) {
    const auto found = pass.find("material_resources");
    if (found == pass.end()) return;
    const auto &encoded = *found;
    if (!encoded.is_object()) subject.field("material_resources", "not_an_object");
    Json sanitized = Json::object();
    std::vector<std::string> reads;
    for (auto entry = encoded.begin(); entry != encoded.end(); ++entry) {
        if (entry.value().is_string()) {
            sanitized[entry.key()] = entry.value();
            reads.push_back(entry.value().get<std::string>());
            continue;
        }
        if (!entry.value().is_object()) {
            subject.field("material_resources", "entry_not_a_resource", {{"key", entry.key()}});
        }
        auto object = entry.value();
        object.erase("footprint");
        sanitized[entry.key()] = std::move(object);
        if (entry.value().contains("resource")) {
            if (!entry.value().at("resource").is_string()) {
                subject.field("material_resources", "resource_not_a_string", {{"key", entry.key()}});
            }
            reads.push_back(entry.value().at("resource").get<std::string>());
        } else {
            reads.push_back(entry.key());
        }
    }
    const Json owner{{"resource_ports", std::move(sanitized)}};
    const auto ports = guarded(subject, "material_resources", "invalid", [&] {
        return parseShaderResourcePortDefinitions(
            owner, reads, std::span<const std::string>{},
            "render graph material pass '" + node.name + "'");
    });
    for (const auto &port : ports) {
        constexpr std::string_view suffix = "@history";
        const bool history = port.resource.ends_with(suffix);
        auto resource = history ? port.resource.substr(0, port.resource.size() - suffix.size())
                                : port.resource;
        if (resource.empty() || resource.find('@') != std::string::npos) {
            refuse("render_graph.resource_reference_invalid@1",
                   subject.detail({{"field", "material_resources"}, {"resource", port.resource}}));
        }
        const auto &source = encoded.at(port.name);
        const bool declares_footprint = source.is_object() && source.contains("footprint");
        if (history && declares_footprint) {
            refuse("render_graph.footprint_invalid@1",
                   subject.detail({{"field", "material_resources"},
                                   {"resource", port.name},
                                   {"reason", "history_derives_temporal"}}));
        }
        ResourceUse use;
        use.resource = std::move(resource);
        use.role = ResourceUseRole::material_resource;
        use.history = history;
        use.subresource = port.subresource;
        use.binding = bindingOf(port);
        use.intent = port.access == ShaderResourcePortAccess::sampled ? LogicalAccessIntent::sampled
                     : port.access == ShaderResourcePortAccess::storage ? LogicalAccessIntent::storage
                                                                         : LogicalAccessIntent::automatic;
        if (history) {
            use.footprint = undeclaredReadFootprint(true);
        } else {
            use.footprint = declares_footprint
                                ? footprintOf(source.at("footprint"), subject,
                                              "material_resources." + port.name + ".footprint",
                                              port.name)
                                : undeclaredReadFootprint(false);
            // §2.3 (ahead of the execution parser): a same-pixel read is a
            // local-read candidate, which a cube view or a subresource view
            // cannot be.
            if (use.footprint->kind == LogicalReadFootprintKind::same_pixel &&
                (port.subresource || port.view == ShaderResourcePortView::cube)) {
                refuse("render_graph.footprint_invalid@1",
                       subject.detail({{"field", "material_resources"},
                                       {"resource", port.name},
                                       {"reason", port.subresource ? "same_pixel_subresource"
                                                                   : "same_pixel_cube_view"}}));
            }
        }
        node.uses.push_back(std::move(use));
    }
}

// ---- pass execution declarations (shape here, required stages in core)

void onlyKeys(const Json &object, std::initializer_list<std::string_view> keys,
              const Subject &subject, std::string_view field) {
    if (!object.is_object()) subject.field(field, "not_an_object");
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        if (std::find(keys.begin(), keys.end(), entry.key()) == keys.end()) {
            subject.field(std::string{field} + "." + entry.key(), "unknown_field");
        }
    }
}

PassShaderDeclaration passShader(const Json &pass, const Subject &subject,
                                 bool raster) {
    PassShaderDeclaration result;
    const auto shader = pass.find("shader");
    if (shader == pass.end()) return result;
    result.shader_declared = true;
    if (raster) onlyKeys(*shader, {"vertex", "fragment", "implementation", "defines"}, subject, "shader");
    else onlyKeys(*shader, {"vertex", "skinned_vertex", "fragment", "defines"}, subject, "shader");
    const auto stage = [&](const char *field) -> std::optional<std::string> {
        const auto found = shader->find(field);
        if (found == shader->end()) return {};
        if (!found->is_string()) subject.field(std::string{"shader."} + field, "requires_string");
        auto ref = found->get<std::string>();
        if (hasKnownShaderSourceExtension(ref, ShaderSourceStage::vertex)) {
            subject.field(std::string{"shader."} + field, "explicit_extension", {{"ref", ref}});
        }
        return ref;
    };
    result.vertex_shader = stage("vertex");
    result.skinned_vertex_shader = stage("skinned_vertex");
    result.fragment_shader = stage("fragment");
    return result;
}

std::array<double, 4> passClearColor(const Json &value, const Subject &subject,
                                    std::string_view field) {
    if (!value.is_array() || value.size() != 4) subject.field(field, "requires_four_numbers");
    std::array<double, 4> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!value.at(i).is_number()) subject.field(field, "requires_four_numbers");
        result[i] = value.at(i).get<double>();
        if (!std::isfinite(result[i])) subject.field(field, "requires_finite_numbers");
    }
    return result;
}

void passExecutionDeclaration(const Json &pass, NodeIR &node,
                               MaterialNodeInfo material, const RenderGraphIR &ir,
                               const Subject &subject) {
    const bool fullscreen = node.shape == RenderNodeShape::fullscreen ||
                            node.shape == RenderNodeShape::output_transform;
    const bool raster = node.shape == RenderNodeShape::raster;
    std::vector<std::string> colors;
    std::size_t image_inputs = 0;
    bool depth = false;
    std::set<std::string, std::less<>> ports;
    std::map<std::string, bool, std::less<>> material_reads;
    // Presence matters even for empty bindings and an empty/null input.
    for (const auto *field : {"screen_inputs", "surface_resources", "material_resources"}) {
        if (pass.contains(field) && pass.contains("input")) subject.field(field, "replaces_positional_input");
    }
    for (const auto &use : node.uses) {
        if (use.role == ResourceUseRole::color_attachment) colors.push_back(use.resource);
        if (use.role == ResourceUseRole::depth_attachment) depth = true;
        const bool buffer = findConfigBuffer(ir, use.resource) != nullptr;
        if (use.role == ResourceUseRole::input) {
            if (buffer && use.history) subject.field("input", "buffer_history", {{"resource", use.resource}});
            if (!buffer) ++image_inputs;
        }
        if ((fullscreen || raster) && use.role == ResourceUseRole::resource_port) {
            const auto authored = use.resource + (use.history ? "@history" : "");
            if (!ports.insert(authored).second) subject.field("resource_ports", "duplicate_resource", {{"resource", authored}});
            ShaderResourcePortDefinition port;
            port.kind = use.binding.kind;
            port.access = use.binding.access;
            const auto expected = port.kind == ShaderResourcePortKind::buffer
                                      ? ShaderResourcePortAccess::storage : ShaderResourcePortAccess::sampled;
            if (effectiveShaderResourcePortAccess(port, port.kind != ShaderResourcePortKind::buffer, false) != expected) {
                subject.field("resource_ports", "invalid_access", {{"port", use.binding.port}});
            }
        }
        const bool contract_input = use.role == ResourceUseRole::screen_input || use.role == ResourceUseRole::surface_resource;
        if (contract_input || use.role == ResourceUseRole::material_resource) {
            const auto field = resourceUseRoleName(use.role);
            if (use.resource.empty() || use.binding.port.empty()) subject.field(field, "empty_name");
            if (use.role == ResourceUseRole::surface_resource && !isFeatureOwnedMaterialPassInputContract(use.binding.port)) {
                subject.field(field, "not_feature_owned", {{"port", use.binding.port}});
            }
            const auto [found, inserted] = material_reads.emplace(use.resource, use.history);
            if (!inserted && found->second != use.history) subject.field(field, "current_and_history", {{"resource", use.resource}});
        }
        if (use.role == ResourceUseRole::material_resource) {
            ShaderResourcePortDefinition port;
            port.kind = use.binding.kind;
            port.access = use.binding.access;
            const auto field = "material_resources." + use.binding.port;
            if (buffer) {
                if (use.history) subject.field(field, "buffer_history");
                if (effectiveShaderResourcePortAccess(port, false, false) != ShaderResourcePortAccess::storage) subject.field(field, "requires_storage");
                if (use.binding.view != ShaderResourcePortView::shared_2d) subject.field(field, "buffer_image_view");
                const auto &encoded = pass.at("material_resources").at(use.binding.port);
                if (encoded.is_object() && encoded.contains("sampling")) subject.field(field, "buffer_sampling");
            } else if (effectiveShaderResourcePortAccess(port, true, false) != ShaderResourcePortAccess::sampled) {
                subject.field(field, "requires_sampled");
            }
        }
    }
    if (node.shape == RenderNodeShape::material) {
        // Positional material inputs have no material descriptor contract.
        if (std::any_of(node.uses.begin(), node.uses.end(), [](const auto &use) { return use.role == ResourceUseRole::input; })) {
            subject.field("input", "requires_named_material_inputs");
        }
    }
    if (const auto clear = pass.find("clear_color"); clear != pass.end()) node.clear_color = passClearColor(*clear, subject, "clear_color");
    if (const auto clear = pass.find("clear_colors"); clear != pass.end()) {
        if (!clear->is_object()) subject.field("clear_colors", "not_an_object");
        node.color_clear_values.assign(colors.size(), node.clear_color);
        for (auto value = clear->begin(); value != clear->end(); ++value) {
            const auto found = std::find(colors.begin(), colors.end(), value.key());
            if (found == colors.end()) subject.field("clear_colors", "not_a_color_output", {{"resource", value.key()}});
            node.color_clear_values[static_cast<std::size_t>(found - colors.begin())] = passClearColor(value.value(), subject, "clear_colors." + value.key());
        }
    }
    if (const auto settings = pass.find("input_settings"); settings != pass.end()) {
        if (!fullscreen && !raster) subject.field("input_settings", "requires_fullscreen_or_raster");
        if (!settings->is_array()) subject.field("input_settings", "not_an_array");
        if (settings->size() > image_inputs) subject.field("input_settings", "too_many_entries");
        for (const auto &entry : *settings) {
            node.input_settings.push_back(entry.is_null() ? ShaderResourceInputSetting{} : guarded(subject, "input_settings", "invalid", [&] {
                return parseShaderResourceInputSetting(entry, "pass '" + node.name + "' input_settings");
            }));
        }
    }
    node.shader_declaration_parsed = true;
    if (fullscreen) {
        FullscreenNodeInfo info;
        static_cast<PassShaderDeclaration &>(info) = passShader(pass, subject, false);
        if (const auto implementation = pass.find("implementation"); implementation != pass.end()) {
            onlyKeys(*implementation, {"provider"}, subject, "implementation");
            auto provider = requiredString(*implementation, "provider", subject, "implementation.provider");
            if (provider.empty() || provider.size() > maximumPassImplementationProviderNameBytes) subject.field("implementation.provider", "empty_or_too_long");
            info.implementation_provider = std::move(provider);
        }
        if (pass.contains("raster_state")) info.raster_state = guarded(subject, "raster_state", "invalid", [&] {
            return parseFullscreenRasterFixedFunctionState(pass, "pass '" + node.name + "'");
        });
        if (pass.contains("push_constants")) {
            const auto value = requiredString(pass, "push_constants", subject, "push_constants");
            if (value == "none") info.push_constants = FullscreenPushConstantData::eNone;
            else if (value == "camera_position") info.push_constants = FullscreenPushConstantData::eCameraPosition;
            else if (value == "projection_view") info.push_constants = FullscreenPushConstantData::eProjectionView;
            else subject.field("push_constants", "unknown_value");
        }
        if (const auto lighting = pass.find("uses_light_data"); lighting != pass.end()) {
            if (!lighting->is_boolean()) subject.field("uses_light_data", "requires_boolean");
            info.uses_light_data = lighting->get<bool>();
        }
        if (const auto sampling = pass.find("input_sampling"); sampling != pass.end()) {
            if (!ports.empty()) subject.field("input_sampling", "replaced_by_resource_ports");
            if (!sampling->is_array()) subject.field("input_sampling", "not_an_array");
            if (sampling->size() != image_inputs) subject.field("input_sampling", "image_input_count_mismatch");
            for (const auto &entry : *sampling) {
                info.input_sampling.push_back(guarded(subject, "input_sampling", "invalid", [&] {
                    return *parseShaderResourceInputSetting(Json{{"sampling", entry}}, "pass '" + node.name + "' input_sampling").sampling;
                }));
            }
        }
        if (info.shader_declared) info.shader_defines = guarded(subject, "shader.defines", "invalid", [&] { return parsePassShaderDefines(pass.at("shader"), node.name); });
        if (info.vertex_shader == "engine://fullscreen" && info.raster_state && info.raster_state->topology != RasterPrimitiveTopology::triangle_list) {
            subject.field("raster_state.topology", "fullscreen_requires_triangle_list");
        }
        node.info = std::move(info);
    } else if (raster) {
        RasterNodeInfo info;
        static_cast<PassShaderDeclaration &>(info) = passShader(pass, subject, true);
        info.draw_declared = pass.contains("draw");
        info.contract = guarded(subject, "draw/raster_state/clip_y", "invalid", [&] {
            return parseRasterPassContract(pass, colors.size(), depth, "pass '" + node.name + "'", false);
        });
        if (info.shader_declared) {
            const auto &shader = pass.at("shader");
            if (shader.contains("implementation")) info.shader_implementation = requiredString(shader, "implementation", subject, "shader.implementation");
            guarded(subject, "shader.implementation", "not_a_canonical_id", [&] {
                if (semanticTypeIdName(parseSemanticTypeId(info.shader_implementation)) != info.shader_implementation) subject.field("shader.implementation", "not_a_canonical_id");
            });
            info.shader_defines = guarded(subject, "shader.defines", "invalid", [&] { return parsePassShaderDefines(shader, node.name); });
        }
        node.info = std::move(info);
    } else if (node.shape == RenderNodeShape::material) {
        if (const auto states = pass.find("material_output_states"); states != pass.end()) {
            if (!material.output_schema) subject.field("material_output_states", "requires_material_outputs");
            material.output_states = guarded(subject, "material_output_states", "invalid", [&] {
                return parseMaterialOutputAttachmentStates(*states, *material.output_schema, "pass '" + node.name + "'");
            });
        }
        if (const auto range = pass.find("material_range"); range != pass.end()) {
            onlyKeys(*range, {"start", "count"}, subject, "material_range");
            material.material_start = uint32Field(*range, "start", subject, "material_range.start");
            material.material_count = uint32Field(*range, "count", subject, "material_range.count");
        }
        node.info = std::move(material);
    } else if (node.shape != RenderNodeShape::ui && node.shape != RenderNodeShape::imgui) {
        AuxiliaryNodeInfo info;
        static_cast<PassShaderDeclaration &>(info) = passShader(pass, subject, false);
        // A shadow pass has no fragment stage. Leave required stages to the
        // execution boundary, including an explicitly empty shader object.
        if (node.shape == RenderNodeShape::shadow_depth && info.fragment_shader) subject.field("shader.fragment", "unsupported_stage");
        if (pass.contains("shader") && pass.at("shader").contains("defines")) subject.field("shader.defines", "unsupported_field");
        node.info = std::move(info);
    }
}


// ---- nodes

struct GraphScope {
    std::string graph;
    PassFieldOwnershipCapabilities capabilities;
    const LogicalTypeRegistry *types = nullptr;
    const RenderGraphIR *ir = nullptr;
    const PassShapePolicy *shape_policy = nullptr;
    const PassShapeDiagnosticSink *shape_diagnostics = nullptr;
};

NodeIR parsePass(const Json &pass, std::size_t index, const GraphScope &scope) {
    if (!pass.is_object()) {
        refuse("render_graph.not_object@1",
               {{"graph", scope.graph}, {"field", "passes"}, {"index", index}});
    }
    Subject subject{scope.graph, {}};
    const auto name = pass.find("name");
    if (name == pass.end() || !name->is_string()) {
        refuse("render_graph.node_name_invalid@1",
               {{"graph", scope.graph}, {"field", "passes"}, {"index", index}});
    }
    NodeIR node;
    node.name = name->get<std::string>();
    subject.node = node.name;
    requireAttachmentOptionsHaveOutputs(pass, subject);
    node.declaration_index = index;
    node.view_family = viewFamily(pass, subject);
    node.view_select = viewSelect(pass, subject);
    node.after = nameList(pass, "after", subject);
    node.before = nameList(pass, "before", subject);
    node.regions = regionTags(pass, subject);
    if (const auto schedule = pass.find("schedule"); schedule != pass.end()) {
        if (!schedule->is_string() || (*schedule != "per_frame" && *schedule != "once")) {
            subject.field("schedule", "not_per_frame_or_once");
        }
        node.schedule = *schedule == "once" ? NodeSchedule::once : NodeSchedule::per_frame;
    }

    RenderPassType type{};
    try {
        type = validatePassFieldOwnership(pass, scope.capabilities, "render graph pass");
    } catch (const std::exception &error) {
        refuse("render_graph.pass_field_ownership@1", subject.detail({{"detail", error.what()}}));
    }
    const auto check_shape = [&](const PassShapePolicy &policy) {
        const auto violations = evaluatePassShape(
            policy, passShapeObservationOf(type, *scope.ir, node));
        if (*scope.shape_diagnostics) (*scope.shape_diagnostics)(violations);
        if (!violations.empty()) {
            refuse("render_graph.pass_shape@1",
                   subject.detail({{"violation", std::string{passShapeViolationName(violations.front().kind)}},
                                   {"target", violations.front().target}}));
        }
    };
    node.shape = renderNodeShapeOfPassType(type);

    MaterialNodeInfo material;
    if (type == RenderPassType::material) {
        material.filter = materialFilter(pass, subject);
        if (const auto contract = pass.find("material_contract"); contract != pass.end()) {
            if (!contract->is_string()) subject.field("material_contract", "requires_string");
            const auto parsed = materialPassContractFromName(contract->get<std::string>());
            if (!parsed) subject.field("material_contract", "unknown");
            material.contract = *parsed;
        }
        if (const auto outputs = pass.find("material_outputs"); outputs != pass.end()) {
            material.output_schema = guarded(subject, "material_outputs", "invalid", [&] {
                return parseMaterialOutputSchema(*outputs,
                                                 "render graph material pass '" + node.name + "' material_outputs");
            });
        }
        if (const auto variant = pass.find("material_variant"); variant != pass.end()) {
            if (!variant->is_string()) subject.field("material_variant", "requires_string");
            material.variant = variant->get<std::string>();
            guarded(subject, "material_variant", "invalid_name", [&] {
                validateMaterialVariantName(*material.variant,
                                            "render graph material pass '" + node.name + "'");
                return 0;
            });
            if (!pass.contains("material_contract")) {
                subject.field("material_variant", "requires_material_contract");
            }
            if (!material.filter || material.filter->include.empty()) {
                subject.field("material_variant", "requires_material_filter_include");
            }
        }
    }
    if (type == RenderPassType::canonical_anchor) {
        check_shape(*scope.shape_policy);
        return node;
    }
    if (type == RenderPassType::snapshot_copy) {
        SnapshotNodeInfo info;
        info.source = requiredString(pass, "source", subject, "source");
        info.destination = requiredString(pass, "destination", subject, "destination");
        info.snapshot_after = requiredString(pass, "snapshot_after", subject, "snapshot_after");
        node.uses.push_back(ResourceUse{.resource = info.source,
                                        .role = ResourceUseRole::copy_source,
                                        .access = ResourceUseAccess::read,
                                        .intent = LogicalAccessIntent::transfer});
        node.uses.push_back(ResourceUse{.resource = info.destination,
                                        .role = ResourceUseRole::copy_destination,
                                        .access = ResourceUseAccess::write,
                                        .intent = LogicalAccessIntent::transfer});
        node.info = std::move(info);
        check_shape(*scope.shape_policy);
        return node;
    }
    if (!pass.contains("output") || !pass.at("output").is_object()) {
        subject.field("output", "requires_object");
    }

    const auto authored_inputs = nameList(pass, "input", subject);
    std::vector<std::string> current_reads;
    for (const auto &authored : authored_inputs) {
        auto [resource, history] = historyReference(authored, subject, "input");
        if (!history) appendUnique(current_reads, resource);
        node.uses.push_back(ResourceUse{.resource = std::move(resource),
                                        .role = ResourceUseRole::input,
                                        .access = ResourceUseAccess::read,
                                        .history = history,
                                        .footprint = history ? std::optional{undeclaredReadFootprint(true)}
                                                             : std::nullopt});
    }
    appendResourcePorts(pass, authored_inputs, {}, node, subject);
    declaredFootprints(pass, "input_footprints", node.uses, current_reads, subject);
    appendContractInputs(pass, "screen_inputs", ResourceUseRole::screen_input, node,
                         *scope.types, subject);
    appendContractInputs(pass, "surface_resources", ResourceUseRole::surface_resource, node,
                         *scope.types, subject);
    appendMaterialResources(pass, node, subject);
    if (type == RenderPassType::material || pass.contains("gpu_draw_source")) {
        material.gpu_draw_source = gpuDrawSource(pass, subject);
        if (material.gpu_draw_source) {
            for (const auto *buffer : {&material.gpu_draw_source->commands,
                                       &material.gpu_draw_source->count}) {
                node.uses.push_back(ResourceUse{.resource = *buffer,
                                                .role = ResourceUseRole::indirect,
                                                .access = ResourceUseAccess::read});
            }
        }
    }
    switch (type) {
    case RenderPassType::material:
    case RenderPassType::fullscreen:
    case RenderPassType::raster:
    case RenderPassType::debug_draw:
    case RenderPassType::debug_text:
    case RenderPassType::gizmo:
    case RenderPassType::shadow_depth:
    case RenderPassType::velocity:
    case RenderPassType::picking:
    case RenderPassType::output_transform:
        node.ray_query = guarded(subject, "ray_query", "invalid",
                                 [&] { return parseRasterRayQuery(pass); });
        break;
    default:
        break;
    }
    node.resolution_domain = guarded(subject, "resolution_domain", "invalid", [&] {
        return parseRenderResolutionDomain(pass, type, node.name);
    });

    const auto &output = pass.at("output");
    if (!output.contains("color")) subject.field("output.color", "missing");
    if (!output.contains("depth")) subject.field("output.depth", "missing");
    std::vector<ParsedAttachment> colors;
    if (const auto &color = output.at("color"); !color.is_null()) {
        if (!color.is_array()) {
            refuse(renderJsonNotNormalizedCode, {{"path", node.name + "/output.color"}});
        }
        for (const auto &entry : color) colors.push_back(attachmentOf(entry, "output.color", subject));
    }
    std::optional<ParsedAttachment> depth;
    if (const auto &encoded = output.at("depth"); !encoded.is_null()) {
        if (encoded.is_array()) {
            refuse(renderJsonNotNormalizedCode, {{"path", node.name + "/output.depth"}});
        }
        depth = attachmentOf(encoded, "output.depth", subject);
    }
    const auto color_load = passOp<AttachmentLoadOp>(pass, "color_load_op",
                                                     findAuthoredAttachmentLoadOp,
                                                     attachmentLoadOpName, subject);
    const auto color_store = passOp<AttachmentStoreOp>(pass, "color_store_op",
                                                       findAuthoredAttachmentStoreOp,
                                                       attachmentStoreOpName, subject);
    const auto depth_load = passOp<AttachmentLoadOp>(pass, "depth_load_op",
                                                     findAuthoredAttachmentLoadOp,
                                                     attachmentLoadOpName, subject);
    const auto depth_store = passOp<AttachmentStoreOp>(pass, "depth_store_op",
                                                       findAuthoredAttachmentStoreOp,
                                                       attachmentStoreOpName, subject);
    const auto color_defaults = defaultPassAttachmentOperations(type, AttachmentAspect::color);
    const auto depth_defaults = defaultPassAttachmentOperations(type, AttachmentAspect::depth);
    const auto attach = [&](const ParsedAttachment &attachment, ResourceUseRole role,
                            std::optional<std::uint32_t> location,
                            PassAttachmentOperations operations) {
        const bool loads = operations.load_op == AttachmentLoadOp::load;
        ResourceUse use;
        use.resource = attachment.target;
        use.role = role;
        use.access = loads ? ResourceUseAccess::read_write : ResourceUseAccess::write;
        use.intent = LogicalAccessIntent::attachment;
        use.subresource = attachment.subresource;
        // §2.3: an attachment that loads reads what it writes, pixel for pixel.
        if (loads) {
            use.footprint = LogicalReadFootprint{LogicalReadFootprintKind::same_pixel, std::nullopt};
        }
        use.binding.location = location;
        use.operations = operations;
        node.uses.push_back(std::move(use));
    };
    for (std::uint32_t location = 0; location < colors.size(); ++location) {
        const auto &attachment = colors[location];
        attach(attachment, ResourceUseRole::color_attachment, location,
               resolveAttachmentOperations(attachment.load_op, attachment.store_op, color_load,
                                           color_store, color_defaults));
    }
    if (depth) {
        attach(*depth, ResourceUseRole::depth_attachment, std::nullopt,
               resolveAttachmentOperations(depth->load_op, depth->store_op, depth_load,
                                           depth_store, depth_defaults));
    }

    check_shape(*scope.shape_policy);
    passExecutionDeclaration(pass, node, std::move(material), *scope.ir, subject);
    return node;
}

// ---- compute tasks (RC-3c2b1: every execution value of a task)

AuthoredShaderField authoredShaderFieldOf(const Json &value) {
    AuthoredShaderField field;
    if (value.is_string()) {
        field.form = AuthoredShaderField::Form::string;
        field.value = value.get<std::string>();
    } else {
        field.form = AuthoredShaderField::Form::other;
    }
    return field;
}

AuthoredShaderField authoredShaderField(const Json &holder, std::string_view key) {
    const auto found = holder.find(std::string{key});
    if (found == holder.end()) return {};
    return authoredShaderFieldOf(*found);
}

AuthoredShaderList authoredShaderList(const Json &holder, std::string_view key) {
    AuthoredShaderList list;
    const auto found = holder.find(std::string{key});
    if (found == holder.end()) return list;
    if (found->is_string()) {
        list.form = AuthoredShaderList::Form::string;
        list.entries.push_back(authoredShaderFieldOf(*found));
    } else if (found->is_array()) {
        list.form = AuthoredShaderList::Form::array;
        for (const auto &entry : *found) list.entries.push_back(authoredShaderFieldOf(entry));
    } else {
        list.form = AuthoredShaderList::Form::other;
    }
    return list;
}

// A list of shader references either form takes (`callable`): one non-empty
// string or an array of them (the ray tracing refusals it always had).
std::vector<std::string> rayShaderList(const Json &pipeline, std::string_view field,
                                       const std::string &task) {
    const auto &found = pipeline.at(std::string{field});
    const auto invalid = [&](const std::string &message) {
        throw RayTracingRefusal("pelican.ray_tracing.invalid_shader_reference@1",
                                Json{{"task", task}, {"stage", std::string{field}}},
                                "pelican.ray_tracing.invalid_shader_reference@1: compute task '" + task +
                                    "' ray_tracing." + std::string{field} + message);
    };
    std::vector<std::string> result;
    const auto append = [&](const Json &value) {
        if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
            invalid(" must contain non-empty strings");
        }
        result.push_back(value.get<std::string>());
    };
    if (found.is_string()) {
        append(found);
    } else if (found.is_array()) {
        for (const auto &value : found) append(value);
    } else {
        invalid(" must be a string or string array");
    }
    if (result.empty()) {
        throw RayTracingRefusal("pelican.ray_tracing.shader_stage_required@1",
                                Json{{"task", task}, {"stage", std::string{field}}},
                                "pelican.ray_tracing.shader_stage_required@1: compute task '" + task +
                                    "' requires at least one ray_tracing." + std::string{field} + " shader");
    }
    return result;
}

RayTracingTaskDeclaration rayTracingDeclaration(const Json &pipeline, const std::string &task,
                                                const std::function<void(std::string_view, std::string_view)>
                                                    &compute_invalid) {
    if (!pipeline.is_object()) {
        throw RayTracingRefusal("pelican.ray_tracing.invalid_pipeline_declaration@1", Json{{"task", task}},
                                "pelican.ray_tracing.invalid_pipeline_declaration@1: compute task '" + task +
                                    "' ray_tracing must be an object");
    }
    RayTracingTaskDeclaration ray;
    for (auto field = pipeline.begin(); field != pipeline.end(); ++field) ray.keys.push_back(field.key());
    ray.raygen = authoredShaderField(pipeline, "raygen");
    ray.miss = authoredShaderList(pipeline, "miss");
    ray.closesthit = authoredShaderList(pipeline, "closesthit");
    ray.traced_ray_types = authoredShaderList(pipeline, "traced_ray_types");
    if (const auto misses = pipeline.find("misses"); misses != pipeline.end()) {
        if (misses->is_object()) {
            ray.misses_form = RayTracingTaskDeclaration::Form::object;
            for (auto entry = misses->begin(); entry != misses->end(); ++entry) {
                ray.misses.emplace_back(entry.key(), authoredShaderFieldOf(entry.value()));
            }
        } else {
            ray.misses_form = RayTracingTaskDeclaration::Form::other;
        }
    }
    if (const auto hits = pipeline.find("hit_shaders"); hits != pipeline.end()) {
        if (hits->is_object()) {
            ray.hit_shaders_form = RayTracingTaskDeclaration::Form::object;
            for (auto ray_type = hits->begin(); ray_type != hits->end(); ++ray_type) {
                AuthoredHitShaders shaders{.ray_type = ray_type.key(), .is_object = ray_type->is_object()};
                if (shaders.is_object) {
                    for (auto group = ray_type->begin(); group != ray_type->end(); ++group) {
                        AuthoredHitGroup hit{.group = group.key(), .is_object = group->is_object()};
                        if (hit.is_object) {
                            hit.closesthit = authoredShaderField(*group, "closesthit");
                            hit.anyhit = authoredShaderField(*group, "anyhit");
                            hit.intersection = authoredShaderField(*group, "intersection");
                            hit.declares_type = group->contains("type");
                            for (auto field = group->begin(); field != group->end(); ++field) {
                                if (field.key() != "closesthit" && field.key() != "anyhit" &&
                                    field.key() != "intersection" && field.key() != "type") {
                                    hit.has_unknown_field = true;
                                }
                            }
                        }
                        shaders.groups.push_back(std::move(hit));
                    }
                }
                ray.hit_shaders.push_back(std::move(shaders));
            }
        } else {
            ray.hit_shaders_form = RayTracingTaskDeclaration::Form::other;
        }
    }
    // What either form takes the same way.
    if (pipeline.contains("callable")) ray.callables = rayShaderList(pipeline, "callable", task);
    if (pipeline.contains("callable_record") || pipeline.contains("callable_records")) {
        if (ray.callables.empty() || !pipeline.contains("callable_record") ||
            !pipeline.contains("callable_records")) {
            throw RayTracingRefusal("pelican.ray_tracing.callable_records_invalid@1", Json{{"task", task}},
                                    "pelican.ray_tracing.callable_records_invalid@1: task '" + task +
                                        "' requires callable, callable_record and callable_records together");
        }
        ray.callable_record = parseRayCallableRecordLayout(pipeline.at("callable_record"), task);
        ray.callable_records = encodeRayCallableRecords(pipeline.at("callable_records"), *ray.callable_record,
                                                        ray.callables.size(), task);
    }
    if (const auto recursion = pipeline.find("max_recursion"); recursion != pipeline.end()) {
        const auto positive_signed = recursion->is_number_integer() && !recursion->is_number_unsigned() &&
                                     recursion->get<std::int64_t>() > 0;
        const auto unsigned_integer = recursion->is_number_unsigned();
        const auto encoded = positive_signed    ? static_cast<std::uint64_t>(recursion->get<std::int64_t>())
                             : unsigned_integer ? recursion->get<std::uint64_t>()
                                                : 0;
        if ((!positive_signed && !unsigned_integer) || encoded == 0 ||
            encoded > std::numeric_limits<std::uint32_t>::max()) {
            throw RayTracingRefusal("pelican.ray_tracing.invalid_recursion@1", Json{{"task", task}},
                                    "pelican.ray_tracing.invalid_recursion@1: compute task '" + task +
                                        "' max_recursion must be a positive uint32 integer");
        }
        ray.max_recursion = static_cast<std::uint32_t>(encoded);
    }
    if (const auto selected = pipeline.find("acceleration_structure"); selected != pipeline.end()) {
        if (!selected->is_string()) compute_invalid("ray_tracing.acceleration_structure", "requires_string");
        ray.acceleration_structure = selected->get<std::string>();
    }
    return ray;
}

// A positive uint32 count as written (not a float, a sign, zero, or a value
// past uint32).
std::optional<std::uint32_t> positiveCount(const Json &value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > 0 && number <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(number);
        }
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number > 0 && number <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(number);
        }
    }
    return std::nullopt;
}

// `dispatch`: the rules the compute task parser had, every key known. The
// parse refuses what is malformed (an unknown key, a value out of range, a
// combination no task takes); whether a ray tracing task launches anything
// (rays, rays_from or indirect) is the execution's to require
// (computeTaskDefinitionsOf), as a compute task's shader: a planning
// revision may leave it out, or write `{}` a later revision fills.
ComputeDispatchDefinition computeDispatch(const Json &task, const std::string &name, bool ray_tracing,
                                          const std::function<void(std::string_view, std::string_view)>
                                              &compute_invalid) {
    ComputeDispatchDefinition dispatch;
    const auto invalid_trace = [&](const std::string &message) {
        throw RayTracingRefusal("pelican.ray_tracing.invalid_trace_dispatch@1", Json{{"task", name}},
                                "pelican.ray_tracing.invalid_trace_dispatch@1: " + message);
    };
    const auto found = task.find("dispatch");
    if (found == task.end()) return dispatch;
    const auto &json = *found;
    if (!json.is_object()) compute_invalid("dispatch", "not_an_object");
    // Only these keys, before any form is read (a ray tracing task's too);
    // local_size is the shader's (refused below by its reason).
    static constexpr std::array<std::string_view, 6> keys{"groups", "groups_from", "local_size",
                                                          "rays",   "rays_from",   "indirect"};
    for (auto field = json.begin(); field != json.end(); ++field) {
        if (std::find(keys.begin(), keys.end(), field.key()) == keys.end()) {
            compute_invalid("dispatch." + field.key(), "unknown_field");
        }
    }
    if (json.contains("rays")) {
        if (!ray_tracing || json.size() != 1) {
            invalid_trace("dispatch.rays requires a ray_tracing task and cannot be combined with other dispatch "
                          "fields: " + name);
        }
        const auto &rays = json.at("rays");
        const auto dimensions = "dispatch.rays must contain three positive uint32 dimensions: " + name;
        if (!rays.is_array() || rays.size() != 3) invalid_trace(dimensions);
        std::array<std::uint32_t, 3> extent{};
        for (std::size_t i = 0; i < extent.size(); ++i) {
            const auto count = positiveCount(rays.at(i));
            if (!count) invalid_trace(dimensions);
            extent[i] = *count;
        }
        dispatch.rays = extent;
        return dispatch;
    }
    if (json.contains("rays_from")) {
        if (!ray_tracing) {
            throw RayTracingRefusal("pelican.ray_tracing.rays_from_requires_pipeline@1", Json{{"task", name}},
                                    "pelican.ray_tracing.rays_from_requires_pipeline@1: dispatch.rays_from "
                                    "requires a ray_tracing task: " + name);
        }
        if (json.size() != 1) {
            invalid_trace("dispatch.rays_from cannot be combined with compute dispatch fields: " + name);
        }
        const auto &rays_from = json.at("rays_from");
        if (!rays_from.is_object() || rays_from.size() != 1 || !rays_from.contains("port") ||
            !rays_from.at("port").is_string()) {
            invalid_trace("dispatch.rays_from must contain exactly one image port: " + name);
        }
        auto port = rays_from.at("port").get<std::string>();
        if (port.empty()) invalid_trace("dispatch.rays_from.port must not be empty: " + name);
        dispatch.rays_from = ComputeImageExtentDispatchDefinition{.port = std::move(port)};
        return dispatch;
    }
    // A ray tracing task's dispatch without a launch (`{}` while a later
    // revision adds it, or a compute task's groups) is read as a compute
    // task's, its values checked; the execution requires the launch
    // (computeTaskDefinitionsOf: trace_extent_required).
    if (json.contains("indirect")) {
        if (ray_tracing && json.size() != 1) {
            invalid_trace("indirect trace cannot be combined with other dispatch fields: " + name);
        }
        if (json.contains("groups") || json.contains("groups_from") || json.contains("local_size")) {
            compute_invalid("dispatch.indirect", "combined_with_groups");
        }
        // indirect.buffer was read with the task's uses.
        const auto &indirect = json.at("indirect");
        for (auto field = indirect.begin(); field != indirect.end(); ++field) {
            if (field.key() != "buffer" && field.key() != "offset") {
                compute_invalid("dispatch.indirect." + field.key(), "unknown_field");
            }
        }
        ComputeIndirectDispatchDefinition definition{.buffer = indirect.at("buffer").get<std::string>()};
        if (definition.buffer.empty()) compute_invalid("dispatch.indirect.buffer", "empty");
        if (const auto offset = indirect.find("offset"); offset != indirect.end()) {
            const bool unsigned_value =
                offset->is_number_unsigned() || (offset->is_number_integer() && offset->get<std::int64_t>() >= 0);
            if (!unsigned_value) compute_invalid("dispatch.indirect.offset", "requires_unsigned_integer");
            definition.offset = offset->is_number_unsigned()
                                    ? offset->get<std::uint64_t>()
                                    : static_cast<std::uint64_t>(offset->get<std::int64_t>());
        }
        if (definition.offset % frameGraphIndirectCommandAlignment != 0) {
            compute_invalid("dispatch.indirect.offset", "not_4_byte_aligned");
        }
        dispatch.indirect = std::move(definition);
        return dispatch;
    }
    if (json.contains("groups") && json.contains("groups_from")) {
        compute_invalid("dispatch.groups", "combined_with_groups_from");
    }
    if (json.contains("local_size")) compute_invalid("dispatch.local_size", "declared_by_the_shader");
    if (const auto groups = json.find("groups"); groups != json.end()) {
        std::array<std::uint32_t *, 3> axes{&dispatch.groups_x, &dispatch.groups_y, &dispatch.groups_z};
        if (groups->is_array()) {
            if (groups->size() != 3) compute_invalid("dispatch.groups", "requires_three_entries");
            for (std::size_t axis = 0; axis < axes.size(); ++axis) {
                const auto count = positiveCount(groups->at(axis));
                if (!count) compute_invalid("dispatch.groups", "requires_positive_uint32");
                *axes[axis] = *count;
            }
        } else if (groups->is_object()) {
            static constexpr std::array<std::string_view, 3> names{"x", "y", "z"};
            for (auto field = groups->begin(); field != groups->end(); ++field) {
                const auto axis = std::find(names.begin(), names.end(), field.key());
                if (axis == names.end()) compute_invalid("dispatch.groups." + field.key(), "unknown_axis");
                const auto count = positiveCount(field.value());
                if (!count) compute_invalid("dispatch.groups." + field.key(), "requires_positive_uint32");
                *axes[static_cast<std::size_t>(axis - names.begin())] = *count;
            }
        } else {
            compute_invalid("dispatch.groups", "not_an_array_or_object");
        }
    }
    if (const auto groups_from = json.find("groups_from"); groups_from != json.end()) {
        if (!groups_from->is_object()) compute_invalid("dispatch.groups_from", "not_an_object");
        for (auto field = groups_from->begin(); field != groups_from->end(); ++field) {
            if (field.key() != "port") compute_invalid("dispatch.groups_from." + field.key(), "unknown_field");
        }
        const auto port = groups_from->find("port");
        if (port == groups_from->end() || !port->is_string()) {
            compute_invalid("dispatch.groups_from.port", "requires_string");
        }
        if (port->get_ref<const std::string &>().empty()) compute_invalid("dispatch.groups_from.port", "empty");
        dispatch.groups_from = ComputeImageExtentDispatchDefinition{.port = port->get<std::string>()};
    }
    return dispatch;
}

NodeIR parseComputeTask(const Json &task, std::size_t index, const GraphScope &scope,
                        NodeDeclarationScope declared_in) {
    if (!task.is_object()) {
        refuse("render_graph.not_object@1",
               {{"graph", scope.graph}, {"field", "compute_tasks"}, {"index", index}});
    }
    const auto name = task.find("name");
    if (name == task.end() || !name->is_string()) {
        refuse("render_graph.node_name_invalid@1",
               {{"graph", scope.graph}, {"field", "compute_tasks"}, {"index", index}});
    }
    NodeIR node;
    node.name = name->get<std::string>();
    node.shape = RenderNodeShape::compute;
    node.scope = declared_in;
    node.declaration_index = index;
    const Subject subject{scope.graph, node.name};
    const std::function<void(std::string_view, std::string_view)> compute_invalid =
        [&](std::string_view field, std::string_view reason) {
            refuse("render_graph.compute_task_invalid@1",
                   {{"node", node.name}, {"field", std::string{field}}, {"reason", std::string{reason}}});
        };
    // A stage only ray_tracing takes, written on the task itself.
    for (const auto *unsupported : {"any_hit", "anyhit", "intersection", "callable"}) {
        if (task.contains(unsupported)) {
            throw RayTracingRefusal("pelican.ray_tracing.unsupported_shader_stage@1",
                                    Json{{"task", node.name}, {"stage", std::string{unsupported}}},
                                    "pelican.ray_tracing.unsupported_shader_stage@1: compute task '" + node.name +
                                        "' declares unsupported top-level ray tracing stage '" + unsupported +
                                        "'");
        }
    }
    node.view_family = viewFamily(task, subject);
    node.view_select = viewSelect(task, subject);
    const auto authored_reads = nameList(task, "reads", subject);
    const auto writes = nameList(task, "writes", subject);
    std::vector<std::string> current_reads;
    for (const auto &authored : authored_reads) {
        auto [resource, history] = historyReference(authored, subject, "reads");
        if (!history) appendUnique(current_reads, resource);
        node.uses.push_back(ResourceUse{.resource = std::move(resource),
                                        .role = ResourceUseRole::input,
                                        .access = ResourceUseAccess::read,
                                        .history = history,
                                        .footprint = history ? std::optional{undeclaredReadFootprint(true)}
                                                             : std::nullopt});
    }
    if (const auto dispatch = task.find("dispatch");
        dispatch != task.end() && dispatch->is_object() && dispatch->contains("indirect")) {
        const auto &indirect = dispatch->at("indirect");
        if (!indirect.is_object()) subject.field("dispatch.indirect", "not_an_object");
        auto buffer = requiredString(indirect, "buffer", subject, "dispatch.indirect.buffer");
        appendUnique(current_reads, buffer);
        node.uses.push_back(ResourceUse{.resource = std::move(buffer),
                                        .role = ResourceUseRole::indirect,
                                        .access = ResourceUseAccess::read});
    }
    appendResourcePorts(task, authored_reads, writes, node, subject);
    declaredFootprints(task, "read_footprints", node.uses, current_reads, subject);
    for (const auto &resource : writes) {
        node.uses.push_back(ResourceUse{.resource = resource,
                                        .role = ResourceUseRole::output,
                                        .access = ResourceUseAccess::write});
    }
    node.after = nameList(task, "after", subject);
    node.before = nameList(task, "before", subject);
    node.regions = regionTags(task, subject);
    if (const auto schedule = task.find("schedule"); schedule != task.end()) {
        if (!schedule->is_string()) compute_invalid("schedule", "requires_string");
        // A compute task also takes per_view (one dispatch per logical view).
        const auto value = nodeScheduleFromName(schedule->get_ref<const std::string &>());
        if (!value) compute_invalid("schedule", "not_per_frame_per_view_or_once");
        node.schedule = *value;
    }
    ComputeNodeInfo info;
    const bool ray_tracing = task.contains("ray_tracing");
    if (const auto query = task.find("ray_query"); query != task.end()) {
        if (ray_tracing || !query->is_object() || query->size() != 1 ||
            !query->contains("acceleration_structure")) {
            compute_invalid("ray_query", "requires_only_acceleration_structure");
        }
        if (!query->at("acceleration_structure").is_string()) {
            compute_invalid("ray_query", "requires_string");
        }
        info.ray_query_acceleration_structure =
            query->at("acceleration_structure").get<std::string>();
        if (info.ray_query_acceleration_structure->empty()) {
            compute_invalid("ray_query", "empty_acceleration_structure");
        }
    }
    if (ray_tracing && task.contains("shader")) {
        throw RayTracingRefusal("pelican.ray_tracing.ambiguous_pipeline_declaration@1", Json{{"task", node.name}},
                                "pelican.ray_tracing.ambiguous_pipeline_declaration@1: compute task '" + node.name +
                                    "' cannot declare both shader and ray_tracing");
    }
    if (ray_tracing) {
        info.ray_tracing = rayTracingDeclaration(task.at("ray_tracing"), node.name, compute_invalid);
    } else {
        if (const auto shader = task.find("shader"); shader != task.end()) {
            if (!shader->is_string()) compute_invalid("shader", "requires_string");
            info.shader = shader->get<std::string>();
        }
    }
    info.dispatch = computeDispatch(task, node.name, ray_tracing, compute_invalid);
    // An image extent dispatch names one of the task's image ports.
    const auto &extent_from = info.dispatch.groups_from ? info.dispatch.groups_from : info.dispatch.rays_from;
    if (extent_from) {
        const auto field = info.dispatch.groups_from ? "dispatch.groups_from.port" : "dispatch.rays_from.port";
        const auto port = std::find_if(node.uses.begin(), node.uses.end(), [&](const ResourceUse &use) {
            return use.role == ResourceUseRole::resource_port && use.binding.port == extent_from->port;
        });
        if (port == node.uses.end()) compute_invalid(field, "unknown_resource_port");
        if (port->binding.kind == ShaderResourcePortKind::buffer) compute_invalid(field, "not_an_image");
    }
    node.info = std::move(info);
    return node;
}

// ---- resources

// The config's render_targets: the render target parser's contract.
std::vector<ImageResource> parseConfigImages(const Json &config) {
    std::vector<ImageResource> images;
    const auto found = config.find("render_targets");
    if (found == config.end()) return images;
    if (!found->is_array()) {
        refuse("render_graph.section_invalid@1", {{"field", "render_targets"}});
    }
    std::unordered_set<std::string> names;
    for (const auto &target : *found) {
        std::string name;
        const auto invalid = [&](std::string_view field, std::string_view reason) {
            Json detail{{"field", std::string{field}}, {"reason", std::string{reason}}};
            if (!name.empty()) detail["resource"] = name;
            refuse("render_graph.image_invalid@1", std::move(detail));
        };
        if (!target.is_object()) invalid("render_targets", "not_an_object");
        const auto encoded_name = target.find("name");
        if (encoded_name == target.end() || !encoded_name->is_string()) {
            invalid("name", "requires_string");
        }
        name = encoded_name->get<std::string>();
        if (name.empty()) invalid("name", "empty");
        try {
            validateAuthoredRenderResourceName(name, "render target");
        } catch (const std::exception &) {
            invalid("name", "reserved");
        }
        if (!names.insert(name).second) invalid("name", "duplicate");
        const auto scale = target.find("extent_scale");
        if (scale == target.end() || !scale->is_number()) invalid("extent_scale", "requires_number");
        const auto extent_scale = scale->get<float>();
        const auto uint32 = [&](const char *key) {
            const auto &value = target.at(key);
            if (!value.is_number_integer() ||
                (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
                invalid(key, "requires_unsigned_integer");
            }
            const auto wide = value.is_number_unsigned()
                                  ? value.get<std::uint64_t>()
                                  : static_cast<std::uint64_t>(value.get<std::int64_t>());
            if (wide > std::numeric_limits<std::uint32_t>::max()) invalid(key, "too_large");
            return static_cast<std::uint32_t>(wide);
        };
        std::optional<ResolvedResourceExtent> fixed;
        const bool has_width = target.contains("width");
        const bool has_height = target.contains("height");
        if (has_width || has_height) {
            if (!has_width || !has_height) invalid("width", "requires_width_and_height");
            fixed = ResolvedResourceExtent{uint32("width"), uint32("height")};
            if (fixed->width == 0 || fixed->height == 0) invalid("width", "not_positive");
        }
        const auto format_name = target.find("format");
        const auto format_class = target.find("format_class");
        ImageResource image;
        image.name = name;
        // A class without a format yet: the format is resolved later (the
        // runtime package's class resolution); until then the image is
        // planned without one.
        image.format_unresolved = format_name == target.end() && format_class != target.end() &&
                                  format_class->is_string();
        if (!image.format_unresolved && (format_name == target.end() || !format_name->is_string())) {
            invalid("format", "requires_string");
        }
        const auto format_text =
            image.format_unresolved ? std::string{} : format_name->get<std::string>();
        if (format_class != target.end()) {
            if (!format_class->is_string()) invalid("format_class", "requires_string");
            image.format_class = format_class->get<std::string>();
        } else {
            image.format_class = "explicit(" + format_text + ")";
        }
        if (const auto role = target.find("role"); role != target.end()) {
            if (!role->is_string()) invalid("role", "requires_string");
            image.role = role->get<std::string>();
        } else {
            image.role = image.format_class == "scene" || image.format_class == "display"
                             ? "color"
                             : "data";
        }
        if (image.role != "color" && image.role != "data") invalid("role", "not_color_or_data");
        const auto usage = target.find("usage");
        if (usage == target.end() || !usage->is_array()) invalid("usage", "requires_string_array");
        std::vector<std::string> usage_names;
        for (const auto &entry : *usage) {
            if (!entry.is_string()) invalid("usage", "requires_string_array");
            usage_names.push_back(entry.get<std::string>());
        }
        if (target.contains("history") && !target.at("history").is_boolean()) {
            invalid("history", "requires_boolean");
        }
        image.history = target.value("history", false);
        if (const auto lifetime = target.find("lifetime"); lifetime != target.end()) {
            if (!lifetime->is_string() || (*lifetime != "persistent" && *lifetime != "transient")) {
                invalid("lifetime", "not_persistent_or_transient");
            }
            image.persistent = *lifetime == "persistent";
        }
        if (const auto clear = target.find("clear_color"); clear != target.end()) {
            if (!clear->is_array() || clear->size() != 4) invalid("clear_color", "requires_four_numbers");
            for (std::size_t i = 0; i < 4; ++i) {
                if (!clear->at(i).is_number()) invalid("clear_color", "requires_four_numbers");
                image.clear_color[i] = clear->at(i).get<double>();
                if (!std::isfinite(image.clear_color[i])) invalid("clear_color", "not_finite");
            }
        }
        if (!image.format_unresolved) {
            try {
                image.format = parseAuthoredImageFormat(format_text);
            } catch (const std::exception &) {
                invalid("format", "unknown");
            }
            image.format_candidates = {image.format};
        }
        if (const auto candidates = target.find("format_candidates"); candidates != target.end()) {
            if (!candidates->is_array()) invalid("format_candidates", "requires_string_array");
            for (const auto &entry : *candidates) {
                if (!entry.is_string() || entry.get_ref<const std::string &>().empty()) {
                    invalid("format_candidates", "requires_non_empty_strings");
                }
                ImageFormat candidate{};
                try {
                    candidate = parseAuthoredImageFormat(entry.get<std::string>());
                } catch (const std::exception &) {
                    invalid("format_candidates", "unknown");
                }
                if (std::find(image.format_candidates.begin(), image.format_candidates.end(),
                              candidate) == image.format_candidates.end()) {
                    image.format_candidates.push_back(candidate);
                }
            }
        }
        if (const auto mips = target.find("mip_levels"); mips != target.end()) {
            if (mips->is_string()) {
                if (*mips != "full") invalid("mip_levels", "not_full");
                image.mip_levels = {ImageMipLevelMode::full_chain, 1};
            } else {
                if (!mips->is_number_unsigned() && !mips->is_number_integer()) {
                    invalid("mip_levels", "requires_positive_integer");
                }
                const auto count = uint32("mip_levels");
                if (count == 0) invalid("mip_levels", "not_positive");
                image.mip_levels = {ImageMipLevelMode::fixed, count};
            }
        }
        if (const auto dimension = target.find("dimension"); dimension != target.end()) {
            if (!dimension->is_string() || (*dimension != "2d" && *dimension != "cube")) {
                invalid("dimension", "not_2d_or_cube");
            }
            image.dimension = *dimension == "cube" ? ImageResourceDimension::cube
                                                   : ImageResourceDimension::two_d;
        }
        image.array_layers = image.dimension == ImageResourceDimension::cube ? 6u : 1u;
        if (target.contains("layers")) {
            const auto count = uint32("layers");
            if (count == 0) invalid("layers", "not_positive");
            if (image.dimension == ImageResourceDimension::cube && count != 6) {
                invalid("layers", "cube_requires_six");
            }
            image.array_layers = count;
        }
        try {
            image.source = parseImageSourceDeclaration(target, name);
        } catch (const std::exception &) {
            invalid("source", "invalid");
        }
        try {
            image.usage = parseAuthoredImageUsage(usage_names);
        } catch (const std::exception &) {
            invalid("usage", "unknown");
        }
        if (extent_scale <= 0.0f) invalid("extent_scale", "not_positive");
        if (image.dimension == ImageResourceDimension::cube) {
            if (!fixed) invalid("dimension", "cube_requires_fixed_extent");
            if (fixed->width != fixed->height) invalid("dimension", "cube_requires_square");
        }
        image.extent = fixed ? ResourceExtentPlan{.kind = ResourceExtentKind::fixed,
                                                  .width = fixed->width,
                                                  .height = fixed->height}
                             : ResourceExtentPlan{.kind = ResourceExtentKind::output_relative,
                                                  .scale_x = extent_scale,
                                                  .scale_y = extent_scale};
        image.initial = target.contains("initial");
        images.push_back(std::move(image));
    }
    return images;
}

// The config's buffers: the whole declaration (RC-3c2a; the core buffer
// parser it replaces read the same keys and refused the same forms). A
// declared size is held to its contract here; a size from an extent is sized
// and held to it by the generation's resource description.
std::vector<BufferResource> parseBuffers(const Json &config, const std::vector<ImageResource> &images) {
    std::vector<BufferResource> buffers;
    const auto found = config.find("buffers");
    if (found == config.end()) return buffers;
    if (!found->is_array()) refuse("render_graph.section_invalid@1", {{"field", "buffers"}});
    std::unordered_set<std::string> names;
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &buffer = found->at(index);
        if (buffer.is_string()) {
            refuse(renderJsonNotNormalizedCode, {{"path", "buffers/" + std::to_string(index)}});
        }
        if (!buffer.is_object()) refuse("render_graph.buffer_invalid@1", {{"reason", "not_an_object"}});
        const auto name = buffer.find("name");
        if (name == buffer.end() || !name->is_string()) {
            refuse("render_graph.buffer_invalid@1", {{"reason", "requires_name"}});
        }
        BufferResource resource{.name = name->get<std::string>()};
        const auto invalid = [&](std::string_view field, std::string_view reason) {
            refuse("render_graph.buffer_invalid@1", {{"resource", resource.name},
                                                     {"field", std::string{field}},
                                                     {"reason", std::string{reason}}});
        };
        try {
            validateAuthoredRenderResourceName(resource.name, "buffer");
        } catch (const std::exception &) {
            invalid("name", "reserved");
        }
        if (!names.insert(resource.name).second) invalid("name", "duplicate");
        const auto unsigned_value = [&](const Json &holder, const char *key, std::string_view field,
                                        std::uint64_t maximum) {
            const auto value = holder.find(key);
            if (value == holder.end() || !value->is_number_integer() ||
                (!value->is_number_unsigned() && value->get<std::int64_t>() < 0)) {
                invalid(field, "requires_unsigned_integer");
            }
            const auto wide = value->get<std::uint64_t>();
            if (wide > maximum) invalid(field, "too_large");
            return wide;
        };
        constexpr auto uint32_max = std::uint64_t{std::numeric_limits<std::uint32_t>::max()};
        constexpr auto uint64_max = std::numeric_limits<std::uint64_t>::max();
        const auto string_value = [&](const char *key) {
            const auto value = buffer.find(key);
            if (!value->is_string()) invalid(key, "requires_string");
            return value->get<std::string>();
        };
        if (buffer.contains("size") && buffer.contains("size_from_extent")) {
            invalid("size", "with_size_from_extent");
        }
        if (buffer.contains("size")) resource.size = unsigned_value(buffer, "size", "size", uint64_max);
        if (const auto extent = buffer.find("size_from_extent"); extent != buffer.end()) {
            if (!extent->is_object()) invalid("size_from_extent", "not_an_object");
            for (const auto &[key, value] : extent->items()) {
                (void)value;
                static constexpr std::array<std::string_view, 6> known{
                    "resource", "tile_width", "tile_height", "header_bytes", "bytes_per_tile", "copies"};
                if (std::find(known.begin(), known.end(), key) == known.end()) {
                    invalid("size_from_extent", "unknown_field");
                }
            }
            const auto image = extent->find("resource");
            if (image == extent->end() || !image->is_string()) invalid("size_from_extent", "requires_resource");
            FrameGraphBufferExtentSizeDefinition size{
                .resource = image->get<std::string>(),
                .tile_width = static_cast<std::uint32_t>(
                    unsigned_value(*extent, "tile_width", "size_from_extent", uint32_max)),
                .tile_height = static_cast<std::uint32_t>(
                    unsigned_value(*extent, "tile_height", "size_from_extent", uint32_max)),
                .header_bytes = unsigned_value(*extent, "header_bytes", "size_from_extent", uint64_max),
                .bytes_per_tile = unsigned_value(*extent, "bytes_per_tile", "size_from_extent", uint64_max),
                .copies = extent->contains("copies")
                              ? static_cast<std::uint32_t>(
                                    unsigned_value(*extent, "copies", "size_from_extent", uint32_max))
                              : 1u,
            };
            if (size.tile_width == 0 || size.tile_height == 0 || size.bytes_per_tile == 0 || size.copies == 0) {
                invalid("size_from_extent", "not_positive");
            }
            // Sized from a config image's resolved extent.
            if (std::none_of(images.begin(), images.end(),
                             [&](const ImageResource &candidate) { return candidate.name == size.resource; })) {
                invalid("size_from_extent", "unknown_image");
            }
            resource.extent_size = std::move(size);
        }
        if (const auto index_buffer = buffer.find("index_buffer"); index_buffer != buffer.end()) {
            if (!index_buffer->is_boolean()) invalid("index_buffer", "requires_boolean");
            resource.index_buffer = index_buffer->get<bool>();
        }
        if (buffer.contains("host_source")) {
            const auto value = string_value("host_source");
            static constexpr std::array sources{
                FrameGraphHostBufferSource::scene_lights_v2, FrameGraphHostBufferSource::view_family_matrices_v1,
                FrameGraphHostBufferSource::scene_draw_commands_v1, FrameGraphHostBufferSource::scene_draw_bounds_v1,
                FrameGraphHostBufferSource::scene_draw_segments_v1, FrameGraphHostBufferSource::runtime_f32};
            for (const auto source : sources)
                if (frameGraphHostBufferSourceName(source) == value) resource.host_source = source;
            if (!resource.host_source) invalid("host_source", "unknown");
        }
        if (buffer.contains("command_layout")) {
            const auto value = string_value("command_layout");
            static constexpr std::array layouts{
                FrameGraphBufferCommandLayout::compute_dispatch, FrameGraphBufferCommandLayout::ray_dispatch,
                FrameGraphBufferCommandLayout::indexed_draw, FrameGraphBufferCommandLayout::draw_count};
            for (const auto layout : layouts)
                if (frameGraphBufferCommandLayoutName(layout) == value) resource.command_layout = layout;
            if (!resource.command_layout) invalid("command_layout", "unknown");
        }
        const auto host = resource.host_source;
        if (host == FrameGraphHostBufferSource::runtime_f32 && resource.command_layout) {
            invalid("command_layout", "runtime_f32_is_data");
        }
        if (const auto initial = buffer.find("initial_f32"); initial != buffer.end()) {
            if (!initial->is_array()) invalid("initial_f32", "requires_number_array");
            // An explicit empty list is not the omitted one (zero contents).
            if (initial->empty()) invalid("initial_f32", "empty");
            for (const auto &value : *initial) {
                if (!value.is_number()) invalid("initial_f32", "requires_number_array");
                resource.initial_f32.push_back(value.get<float>());
            }
            if (host != FrameGraphHostBufferSource::runtime_f32) invalid("initial_f32", "requires_runtime_f32");
        }
        if (host == FrameGraphHostBufferSource::view_family_matrices_v1) {
            if (!buffer.contains("view_family")) invalid("view_family", "requires_string");
            resource.host_view_family = string_value("view_family");
            if (resource.host_view_family.empty()) invalid("view_family", "empty");
            if (resource.command_layout) invalid("command_layout", "view_family_matrices_is_data");
        } else if (buffer.contains("view_family")) {
            invalid("view_family", "requires_view_family_matrices");
        }
        if (host == FrameGraphHostBufferSource::scene_draw_commands_v1 &&
            resource.command_layout != FrameGraphBufferCommandLayout::indexed_draw) {
            invalid("command_layout", "scene_draw_commands_requires_indexed_draw");
        }
        if ((host == FrameGraphHostBufferSource::scene_draw_bounds_v1 ||
             host == FrameGraphHostBufferSource::scene_draw_segments_v1) &&
            resource.command_layout) {
            invalid("command_layout", "scene_draw_records_are_data");
        }
        if (buffer.contains("lifetime")) {
            const auto lifetime = string_value("lifetime");
            if (lifetime != "persistent" && lifetime != "transient") {
                invalid("lifetime", "not_persistent_or_transient");
            }
            resource.persistent = lifetime == "persistent";
        }
        if (!resource.extent_size) requireBufferSizeContract(resource);
        buffers.push_back(std::move(resource));
    }
    return buffers;
}

std::vector<FrameNodeOrderIR> parseFrameNodeOrder(const Json &config) {
    std::vector<FrameNodeOrderIR> result;
    const auto found = config.find("frame_node_order");
    if (found == config.end()) return result;
    const auto invalid = [](Json detail) {
        refuse("render_graph.frame_node_order_invalid@1", std::move(detail));
    };
    if (!found->is_object()) invalid({{"reason", "not_an_object"}});
    for (const auto &[graph, names] : found->items()) {
        if (!names.is_array()) invalid({{"graph", graph}, {"reason", "not_an_array"}});
        FrameNodeOrderIR order{graph, {}};
        std::unordered_set<std::string> seen;
        for (const auto &value : names) {
            if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
                invalid({{"graph", graph}, {"reason", "node_not_a_non_empty_string"}});
            }
            const auto &name = value.get_ref<const std::string &>();
            if (!seen.insert(name).second) {
                invalid({{"graph", graph}, {"node", name}, {"reason", "duplicate"}});
            }
            order.nodes.push_back(name);
        }
        result.push_back(std::move(order));
    }
    return result;
}

// RC-3c2b1: the callable records' table as written (its values packed by the
// task's schema when the final revision resolves it, as the structures'
// table).
std::vector<std::pair<std::string, RayCallableRecordValues>> parseRayTaskCallableRecords(const Json &config) {
    std::vector<std::pair<std::string, RayCallableRecordValues>> result;
    const auto found = config.find("ray_task_callable_records");
    if (found == config.end()) return result;
    if (!found->is_object()) {
        refuse("render_graph.compute_task_invalid@1",
               {{"node", ""}, {"field", "ray_task_callable_records"}, {"reason", "not_an_object"}});
    }
    for (const auto &[name, records] : found->items()) {
        result.emplace_back(name, rayCallableRecordValuesOf(records));
    }
    return result;
}

// The table's form only: the tasks it names may be a provider's, so the
// final revision resolves it (finalizeRenderGraph).
std::vector<std::pair<std::string, std::string>> parseRayTaskAccelerationStructures(
    const Json &config) {
    std::vector<std::pair<std::string, std::string>> result;
    const auto found = config.find("ray_task_acceleration_structures");
    if (found == config.end()) return result;
    const auto invalid = [](const std::string &task, std::string_view reason) {
        refuse("render_graph.compute_task_invalid@1",
               {{"node", task},
                {"field", "ray_task_acceleration_structures"},
                {"reason", std::string{reason}}});
    };
    if (!found->is_object()) invalid({}, "not_an_object");
    for (const auto &[name, selected] : found->items()) {
        if (name.empty() || !selected.is_string() ||
            selected.get_ref<const std::string &>().empty()) {
            invalid(name, "requires_task_and_acceleration_structure");
        }
        result.emplace_back(name, selected.get<std::string>());
    }
    return result;
}

const Json &arrayField(const Json &holder, const char *key, const Subject &subject) {
    const auto &value = holder.at(key);
    if (!value.is_array()) {
        refuse("render_graph.section_invalid@1", subject.detail({{"field", key}}));
    }
    return value;
}

} // namespace

namespace {
thread_local std::size_t parse_count = 0;
} // namespace

std::size_t renderGraphParseCount() noexcept { return parse_count; }

RenderGraphIR parseRenderGraph(const nlohmann::json &document,
                               PassFieldOwnershipCapabilities capabilities,
                               const PassShapePolicy &shape_policy,
                               const PassShapeDiagnosticSink &shape_diagnostics) {
    ++parse_count;
    if (!document.is_object()) refuse("render_graph.not_object@1", {{"field", "document"}});
    const Json &config = isRenderGraphRoot(document) ? renderingSectionOf(document) : document;
    if (!config.is_object()) refuse("render_graph.not_object@1", {{"field", "rendering"}});

    const auto types = makeBuiltinLogicalTypeRegistry();
    RenderGraphIR ir;
    ir.images = parseConfigImages(config);
    ir.buffers = parseBuffers(config, ir.images);
    ir.frame_node_order = parseFrameNodeOrder(config);
    ir.ray_task_acceleration_structures = parseRayTaskAccelerationStructures(config);
    ir.ray_task_callable_records = parseRayTaskCallableRecords(config);

    if (!config.contains("rendering_passes")) {
        // The single-graph form (top-level passes / compute_tasks) is not the
        // normal form; tests wrap it into rendering_passes.
        for (const auto *key : {"passes", "compute_tasks"}) {
            if (config.contains(key)) {
                refuse(renderJsonNotNormalizedCode, {{"path", std::string{"/"} + key}});
            }
        }
        return ir;
    }
    const auto &rendering_passes = arrayField(config, "rendering_passes", Subject{});

    // The config's compute tasks: a copy in every graph, after its own.
    std::vector<NodeIR> shared_tasks;
    if (config.contains("compute_tasks")) {
        const auto &tasks = arrayField(config, "compute_tasks", Subject{});
        const GraphScope scope{{}, capabilities, &types};
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            shared_tasks.push_back(
                parseComputeTask(tasks.at(index), index, scope, NodeDeclarationScope::shared));
        }
        for (const auto &task : shared_tasks) ir.shared_compute_tasks.push_back(task.name);
        ir.authored_shared_compute_tasks = ir.shared_compute_tasks;
    }

    const auto append_shared = [&](GraphIR &graph) {
        for (auto task : shared_tasks) {
            task.declaration_index = graph.nodes.size();
            graph.nodes.push_back(std::move(task));
        }
    };
    for (std::size_t graph_index = 0; graph_index < rendering_passes.size(); ++graph_index) {
        const auto &graph_json = rendering_passes.at(graph_index);
        if (!graph_json.is_object()) {
            refuse("render_graph.not_object@1", {{"field", "rendering_passes"}, {"index", graph_index}});
        }
        GraphIR graph;
        if (const auto name = graph_json.find("name"); name != graph_json.end()) {
            if (!name->is_string()) {
                refuse("render_graph.graph_name_invalid@1",
                       {{"index", graph_index}, {"reason", "requires_string"}});
            }
            graph.name = name->get<std::string>();
        } else {
            graph.name = "frame_graph";
        }
        if (graph.name.empty()) {
            refuse("render_graph.graph_name_invalid@1", {{"index", graph_index}, {"reason", "empty"}});
        }
        if (findGraph(ir, graph.name) != nullptr) {
            refuse("render_graph.graph_name_invalid@1",
                   {{"index", graph_index}, {"graph", graph.name}, {"reason", "duplicate"}});
        }
        const Subject subject{graph.name, {}};
        const GraphScope scope{graph.name, capabilities, &types, &ir, &shape_policy, &shape_diagnostics};
        // RC-3c2a: nothing composes a graph's own declarations (a graph
        // document's images and a loop's copies are the config's), and
        // nothing would register them. RC-3c2b1: nor a graph's own compute
        // tasks (compose, a loop and Studio write the config's; nothing
        // would have registered them).
        for (const auto *key : {"render_targets", "buffers", "compute_tasks"}) {
            if (graph_json.contains(key)) {
                refuse("render_graph.graph_declaration_unsupported@1", {{"graph", graph.name}, {"field", key}});
            }
        }
        if (!graph_json.contains("passes") && shared_tasks.empty()) {
            refuse("render_graph.section_invalid@1", subject.detail({{"field", "passes"}}));
        }
        std::size_t index = 0;
        std::unordered_set<std::string> pass_names;
        if (graph_json.contains("passes")) for (const auto &pass : arrayField(graph_json, "passes", subject)) {
            auto node = parsePass(pass, index++, scope);
            if (node.name.empty()) refuse("render_graph.node_name_empty@1", subject.detail());
            if (!pass_names.insert(node.name).second) {
                refuse("render_graph.duplicate_node@1", subject.detail({{"node", node.name}}));
            }
            graph.nodes.push_back(std::move(node));
        }
        append_shared(graph);
        ir.graphs.push_back(std::move(graph));
    }
    // A config of compute tasks only plans them as one graph.
    if (ir.graphs.empty() && config.contains("compute_tasks")) {
        GraphIR graph;
        graph.name = "frame_graph";
        append_shared(graph);
        ir.graphs.push_back(std::move(graph));
    }
    // RC-3c2a: a snapshot copy into a resolved format outside the copy
    // contract is refused in every revision (a preview's too, which is never
    // finalized); its size waits on the output's extent (finalizeRenderGraph),
    // and a class still without its format is checked there.
    for (const auto &graph : ir.graphs) {
        for (const auto &node : graph.nodes) {
            const auto *snapshot = std::get_if<SnapshotNodeInfo>(&node.info);
            if (snapshot == nullptr) continue;
            const auto *destination = findConfigImage(ir, snapshot->destination);
            if (destination != nullptr && !destination->format_unresolved &&
                !snapshotCopiesFormatOf(*destination)) {
                refuse("render_graph.snapshot_size_unavailable@1",
                       {{"graph", graph.name}, {"node", node.name}, {"resource", snapshot->destination}});
            }
        }
    }
    return ir;
}

} // namespace Pelican
