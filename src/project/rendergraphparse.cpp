#include "rendergraphparse.hpp"

#include "framegraphbufferdefinition.hpp"
#include "graphcomposition.hpp"
#include "imagesource.hpp"
#include "imagesubresourcejson.hpp"
#include "logicalrendertype.hpp"
#include "materialformat.hpp"
#include "materialscreeninput.hpp"
#include "passshapepolicy.hpp"
#include "rasterrayquery.hpp"
#include "renderconfigsection.hpp"
#include "renderingjsonnormalize.hpp"
#include "renderresourcename.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
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

// ---- nodes

struct GraphScope {
    std::string graph;
    PassFieldOwnershipCapabilities capabilities;
    const LogicalTypeRegistry *types = nullptr;
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
    // The planner's shape check: no buffer names, so a buffer input is
    // checked as an image for duplicates and feedback, as it always was.
    {
        PassShapeObservation observation;
        try {
            observation = passShapeObservationFromJson(type, pass, {});
        } catch (const std::exception &error) {
            refuse("render_graph.pass_shape@1",
                   subject.detail({{"violation", "unreadable"}, {"detail", error.what()}}));
        }
        const auto violations = evaluatePassShape(defaultPassShapePolicy(), observation);
        if (!violations.empty()) {
            refuse("render_graph.pass_shape@1",
                   subject.detail({{"violation", std::string{passShapeViolationName(violations.front().kind)}},
                                   {"target", violations.front().target}}));
        }
    }
    node.shape = renderNodeShapeOfPassType(type);

    MaterialNodeInfo material;
    if (type == RenderPassType::material) {
        material.filter = materialFilter(pass, subject);
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

    if (type == RenderPassType::fullscreen || type == RenderPassType::output_transform) {
        FullscreenNodeInfo info;
        if (const auto implementation = pass.find("implementation");
            implementation != pass.end()) {
            const auto provider = implementation->is_object()
                                      ? implementation->find("provider")
                                      : implementation->end();
            info.implementation_provider =
                implementation->is_object() && provider != implementation->end() &&
                        provider->is_string()
                    ? provider->get<std::string>()
                    : std::string{};
        }
        if (const auto shader = pass.find("shader"); shader != pass.end() && shader->is_object()) {
            if (const auto vertex = shader->find("vertex");
                vertex != shader->end() && vertex->is_string()) {
                info.vertex_shader = vertex->get<std::string>();
            }
            if (const auto fragment = shader->find("fragment");
                fragment != shader->end() && fragment->is_string()) {
                info.fragment_shader = fragment->get<std::string>();
            }
        }
        node.info = std::move(info);
    } else if (type == RenderPassType::material) {
        node.info = std::move(material);
    }
    return node;
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
    const auto compute_invalid = [&](std::string_view field, std::string_view reason) {
        refuse("render_graph.compute_task_invalid@1",
               {{"node", node.name}, {"field", std::string{field}}, {"reason", std::string{reason}}});
    };
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
    // Ahead of the compute task parser, which refused these before.
    if (const auto schedule = task.find("schedule"); schedule != task.end()) {
        if (!schedule->is_string()) compute_invalid("schedule", "requires_string");
        const auto &value = schedule->get_ref<const std::string &>();
        if (value == "per_frame") node.schedule = NodeSchedule::per_frame;
        else if (value == "per_view") node.schedule = NodeSchedule::per_view;
        else if (value == "once") node.schedule = NodeSchedule::once;
        else compute_invalid("schedule", "not_per_frame_per_view_or_once");
    }
    ComputeNodeInfo info;
    info.ray_tracing = task.contains("ray_tracing");
    if (const auto query = task.find("ray_query"); query != task.end()) {
        if (info.ray_tracing || !query->is_object() || query->size() != 1 ||
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
    if (info.ray_tracing) {
        const auto &ray = task.at("ray_tracing");
        if (ray.is_object() && ray.contains("acceleration_structure")) {
            if (!ray.at("acceleration_structure").is_string()) {
                compute_invalid("ray_tracing.acceleration_structure", "requires_string");
            }
            info.ray_tracing_acceleration_structure =
                ray.at("acceleration_structure").get<std::string>();
        }
    }
    node.info = std::move(info);
    return node;
}

// ---- resources

std::uint64_t sizeValue(const Json &target, const char *key, const Subject &subject,
                        const std::string &name) {
    const auto &value = target.at(key);
    if (!value.is_number()) {
        refuse("render_graph.image_invalid@1",
               subject.detail({{"resource", name}, {"field", key}, {"reason", "not_a_number"}}));
    }
    return value.get<std::size_t>();
}

// A snapshot copy's size of each image of one declaration list, as the
// planner sized it: a fixed extent, or extent_scale of the first display
// image with a fixed extent; pending (the output's extent) when there is no
// such image; the 12 formats of the copy contract only.
void assignSnapshotSizing(const Json &targets, std::vector<ImageResource> &images,
                          bool &declares_extent, const Subject &subject) {
    static constexpr std::array copy_formats{
        ImageFormat::r8_unorm,          ImageFormat::r16_sfloat,
        ImageFormat::d16_unorm,         ImageFormat::r8g8b8a8_unorm,
        ImageFormat::r8g8b8a8_srgb,     ImageFormat::b8g8r8a8_unorm,
        ImageFormat::b8g8r8a8_srgb,     ImageFormat::d32_sfloat,
        ImageFormat::r32_sfloat,        ImageFormat::r16g16_sfloat,
        ImageFormat::r16g16b16a16_sfloat, ImageFormat::r32g32b32a32_sfloat,
    };
    std::optional<std::pair<std::size_t, std::size_t>> base;
    // The planner's search: the images with a width and a height, in order,
    // up to the first display one; it read their format_class as a string
    // and no other image's.
    for (const auto &target : targets) {
        if (!target.contains("width") || !target.contains("height")) continue;
        const auto format_class = target.find("format_class");
        if (format_class != target.end() && !format_class->is_string()) {
            refuse("render_graph.image_invalid@1",
                   subject.detail({{"resource", target.value("name", std::string{})},
                                   {"field", "format_class"},
                                   {"reason", "requires_string"}}));
        }
        if (format_class != target.end() && *format_class == "display") {
            const auto name = target.value("name", std::string{});
            base = {sizeValue(target, "width", subject, name),
                    sizeValue(target, "height", subject, name)};
            break;
        }
    }
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto &target = targets.at(index);
        auto &image = images[index];
        declares_extent |= target.contains("width") || target.contains("height") ||
                           target.contains("extent_scale");
        std::size_t width = 0;
        std::size_t height = 0;
        const auto scale = target.find("extent_scale");
        const bool numeric_scale = scale != target.end() && scale->is_number();
        if (target.contains("width") && target.contains("height")) {
            width = sizeValue(target, "width", subject, image.name);
            height = sizeValue(target, "height", subject, image.name);
        } else if (base && numeric_scale) {
            width = static_cast<std::size_t>(base->first * scale->get<double>());
            height = static_cast<std::size_t>(base->second * scale->get<double>());
        } else if (!base && numeric_scale && scale->get<double>() > 0.0) {
            image.snapshot_extent_pending = true;
            continue;
        }
        if (width == 0 || height == 0) continue;
        // A class without its format yet: the copy's bytes wait on the
        // format as they wait on the output's extent (a copy into it is
        // sized, and its format required by name, in the final revision).
        if (image.format_unresolved) {
            image.snapshot_extent_pending = true;
            continue;
        }
        const auto format = target.find("format");
        if (format == target.end() || !format->is_string()) {
            refuse("render_graph.image_invalid@1",
                   subject.detail({{"resource", image.name}, {"field", "format"},
                                   {"reason", "requires_string"}}));
        }
        const auto known = findImageFormat(format->get<std::string>());
        if (!known || std::find(copy_formats.begin(), copy_formats.end(), *known) ==
                          copy_formats.end()) {
            continue;
        }
        image.snapshot_copy_bytes =
            width * height * imageFormatEstimatedTexelBytes(*known);
    }
}

// A graph's own render_targets: what the planner read of them.
std::vector<ImageResource> parseGraphImages(const Json &graph, bool &declares_extent,
                                            const Subject &subject) {
    std::vector<ImageResource> images;
    const auto found = graph.find("render_targets");
    if (found == graph.end()) return images;
    if (!found->is_array()) {
        refuse("render_graph.section_invalid@1",
               subject.detail({{"field", "render_targets"}}));
    }
    for (const auto &target : *found) {
        const auto invalid = [&](std::string_view field, std::string_view reason,
                                 const std::string &name = {}) {
            Json detail{{"field", std::string{field}}, {"reason", std::string{reason}}};
            if (!name.empty()) detail["resource"] = name;
            refuse("render_graph.image_invalid@1", subject.detail(std::move(detail)));
        };
        if (!target.is_object()) invalid("render_targets", "not_an_object");
        const auto name = target.find("name");
        if (name == target.end() || !name->is_string()) invalid("name", "requires_string");
        ImageResource image;
        image.name = name->get<std::string>();
        try {
            validateAuthoredRenderResourceName(image.name, "render target");
        } catch (const std::exception &) {
            invalid("name", "reserved", image.name);
        }
        if (target.contains("history") && !target.at("history").is_boolean()) {
            invalid("history", "requires_boolean", image.name);
        }
        image.history = target.value("history", false);
        // A format_class of another type is refused where the planner read it
        // (assignSnapshotSizing's search for the display image).
        if (const auto format_class = target.find("format_class");
            format_class != target.end() && format_class->is_string()) {
            image.format_class = format_class->get<std::string>();
        }
        images.push_back(std::move(image));
    }
    assignSnapshotSizing(*found, images, declares_extent, subject);
    return images;
}

// The config's render_targets: the render target parser's contract.
std::vector<ImageResource> parseConfigImages(const Json &config, bool &declares_extent) {
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
    assignSnapshotSizing(*found, images, declares_extent, Subject{});
    return images;
}

std::vector<BufferResource> parseBuffers(const Json &holder, const Subject &subject) {
    std::vector<BufferResource> buffers;
    const auto found = holder.find("buffers");
    if (found == holder.end()) return buffers;
    if (!found->is_array()) refuse("render_graph.section_invalid@1", subject.detail({{"field", "buffers"}}));
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &buffer = found->at(index);
        if (buffer.is_string()) {
            refuse(renderJsonNotNormalizedCode,
                   {{"path", (subject.graph.empty() ? std::string{} : subject.graph + "/") +
                                 "buffers/" + std::to_string(index)}});
        }
        if (!buffer.is_object()) {
            refuse("render_graph.buffer_invalid@1", subject.detail({{"reason", "not_an_object"}}));
        }
        const auto name = buffer.find("name");
        if (name == buffer.end() || !name->is_string()) {
            refuse("render_graph.buffer_invalid@1", subject.detail({{"reason", "requires_name"}}));
        }
        BufferResource resource{name->get<std::string>()};
        try {
            validateAuthoredRenderResourceName(resource.name, "buffer");
        } catch (const std::exception &) {
            refuse("render_graph.buffer_invalid@1",
                   subject.detail({{"resource", resource.name}, {"reason", "reserved"}}));
        }
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

// The planner's snapshot sizes: a graph's own images first (when they declare
// an extent), then the config's (when they do).
void assignSnapshotBytes(GraphIR &graph, const std::vector<ImageResource> &images,
                         bool declares_extent) {
    if (!declares_extent) return;
    std::unordered_map<std::string, std::size_t> resolved;
    std::unordered_set<std::string> pending;
    for (const auto &image : images) {
        if (image.snapshot_copy_bytes) resolved.emplace(image.name, *image.snapshot_copy_bytes);
        if (image.snapshot_extent_pending) pending.insert(image.name);
    }
    for (auto &node : graph.nodes) {
        auto *snapshot = std::get_if<SnapshotNodeInfo>(&node.info);
        if (snapshot == nullptr) continue;
        if (pending.contains(snapshot->destination)) continue;
        const auto found = resolved.find(snapshot->destination);
        if (found == resolved.end() || found->second == 0) {
            refuse("render_graph.snapshot_size_unavailable@1",
                   {{"graph", graph.name}, {"node", node.name}, {"resource", snapshot->destination}});
        }
        snapshot->byte_size = found->second;
    }
}

} // namespace

namespace {
thread_local std::size_t parse_count = 0;
} // namespace

std::size_t renderGraphParseCount() noexcept { return parse_count; }

RenderGraphIR parseRenderGraph(const nlohmann::json &document,
                               PassFieldOwnershipCapabilities capabilities) {
    ++parse_count;
    if (!document.is_object()) refuse("render_graph.not_object@1", {{"field", "document"}});
    const Json &config = isRenderGraphRoot(document) ? renderingSectionOf(document) : document;
    if (!config.is_object()) refuse("render_graph.not_object@1", {{"field", "rendering"}});

    const auto types = makeBuiltinLogicalTypeRegistry();
    RenderGraphIR ir;
    ir.images = parseConfigImages(config, ir.images_declare_extent);
    ir.buffers = parseBuffers(config, Subject{});
    ir.frame_node_order = parseFrameNodeOrder(config);
    ir.ray_task_acceleration_structures = parseRayTaskAccelerationStructures(config);

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
        const GraphScope scope{graph.name, capabilities, &types};
        graph.images = parseGraphImages(graph_json, graph.images_declare_extent, subject);
        graph.buffers = parseBuffers(graph_json, subject);
        std::size_t index = 0;
        if (graph_json.contains("passes")) {
            for (const auto &pass : arrayField(graph_json, "passes", subject)) {
                graph.nodes.push_back(parsePass(pass, index++, scope));
            }
        }
        if (graph_json.contains("compute_tasks")) {
            for (const auto &task : arrayField(graph_json, "compute_tasks", subject)) {
                graph.nodes.push_back(
                    parseComputeTask(task, index++, scope, NodeDeclarationScope::graph));
            }
        }
        assignSnapshotBytes(graph, graph.images, graph.images_declare_extent);
        append_shared(graph);
        assignSnapshotBytes(graph, ir.images, ir.images_declare_extent);
        ir.graphs.push_back(std::move(graph));
    }
    // A config of compute tasks only plans them as one graph.
    if (ir.graphs.empty() && config.contains("compute_tasks")) {
        GraphIR graph;
        graph.name = "frame_graph";
        append_shared(graph);
        ir.graphs.push_back(std::move(graph));
    }
    return ir;
}

} // namespace Pelican
