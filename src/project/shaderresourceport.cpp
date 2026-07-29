#include "shaderresourceport.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

bool identifierCharacter(char value, bool first) {
    const auto lower = value >= 'a' && value <= 'z';
    const auto upper = value >= 'A' && value <= 'Z';
    if (lower || upper || value == '_') return true;
    return !first && value >= '0' && value <= '9';
}

void requireIdentifier(
    std::string_view value, std::string_view context) {
    if (value.empty()) {
        throw std::runtime_error(
            std::string{context} + " must not be empty");
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!identifierCharacter(value[index], index == 0)) {
            throw std::runtime_error(
                std::string{context} +
                " must be a GLSL identifier: " +
                std::string{value});
        }
    }
}

ShaderResourcePortAccess parseAccess(
    const nlohmann::json &entry, std::string_view context) {
    if (!entry.contains("access")) {
        return ShaderResourcePortAccess::automatic;
    }
    if (!entry.at("access").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".access must be a string");
    }
    const auto value =
        entry.at("access").get<std::string>();
    if (value == "automatic") {
        return ShaderResourcePortAccess::automatic;
    }
    if (value == "sampled") {
        return ShaderResourcePortAccess::sampled;
    }
    if (value == "storage") {
        return ShaderResourcePortAccess::storage;
    }
    throw std::runtime_error(
        std::string{context} +
        ".access has unknown value '" + value + "'");
}

ShaderResourcePortView parseView(
    const nlohmann::json &entry, std::string_view context) {
    if (!entry.contains("view")) {
        return ShaderResourcePortView::shared_2d;
    }
    if (!entry.at("view").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".view must be a string");
    }
    const auto value =
        entry.at("view").get<std::string>();
    if (value == "shared_2d") {
        return ShaderResourcePortView::shared_2d;
    }
    if (value == "per_view") {
        return ShaderResourcePortView::per_view;
    }
    if (value == "family_array") {
        return ShaderResourcePortView::family_array;
    }
    throw std::runtime_error(
        std::string{context} +
        ".view has unknown value '" + value + "'");
}

ShaderResourcePortFilter parseFilter(
    const nlohmann::json &sampling,
    std::string_view context) {
    if (!sampling.contains("filter")) {
        return ShaderResourcePortFilter::linear;
    }
    if (!sampling.at("filter").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".filter must be a string");
    }
    const auto value =
        sampling.at("filter").get<std::string>();
    if (value == "linear") {
        return ShaderResourcePortFilter::linear;
    }
    if (value == "nearest") {
        return ShaderResourcePortFilter::nearest;
    }
    throw std::runtime_error(
        std::string{context} +
        ".filter has unknown value '" + value + "'");
}

ShaderResourcePortAddressMode parseAddressMode(
    const nlohmann::json &sampling,
    std::string_view context) {
    if (!sampling.contains("address")) {
        return ShaderResourcePortAddressMode::repeat;
    }
    if (!sampling.at("address").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".address must be a string");
    }
    const auto value =
        sampling.at("address").get<std::string>();
    if (value == "repeat") {
        return ShaderResourcePortAddressMode::repeat;
    }
    if (value == "mirrored_repeat") {
        return ShaderResourcePortAddressMode::mirrored_repeat;
    }
    if (value == "clamp_to_edge") {
        return ShaderResourcePortAddressMode::clamp_to_edge;
    }
    throw std::runtime_error(
        std::string{context} +
        ".address has unknown value '" + value + "'");
}

ShaderResourcePortSampling parseSampling(
    const nlohmann::json &entry, std::string_view context) {
    if (!entry.contains("sampling")) {
        return {};
    }
    const auto &sampling = entry.at("sampling");
    if (!sampling.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".sampling must be an object");
    }
    for (auto field = sampling.begin();
         field != sampling.end(); ++field) {
        if (field.key() != "filter" &&
            field.key() != "address") {
            throw std::runtime_error(
                std::string{context} +
                ".sampling has unknown field '" +
                field.key() + "'");
        }
    }
    return {
        parseFilter(sampling,
                    std::string{context} + ".sampling"),
        parseAddressMode(
            sampling,
            std::string{context} + ".sampling"),
    };
}

std::uint32_t parseSubresourceUint(
    const nlohmann::json &object,
    std::string_view field,
    std::uint32_t fallback,
    std::string_view context) {
    if (!object.contains(field)) {
        return fallback;
    }
    const auto &encoded =
        object.at(field);
    if ((!encoded.is_number_unsigned() &&
         !encoded.is_number_integer()) ||
        (!encoded.is_number_unsigned() &&
         encoded.get<std::int64_t>() < 0)) {
        throw std::runtime_error(
            std::string{context} + "." +
            std::string{field} +
            " must be a non-negative integer");
    }
    const auto value =
        encoded.get<std::uint64_t>();
    if (value >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} + "." +
            std::string{field} +
            " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<ImageSubresourceRange> parseSubresource(
    const nlohmann::json &entry,
    std::string_view context) {
    if (!entry.contains("subresource")) {
        return std::nullopt;
    }
    const auto &encoded =
        entry.at("subresource");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource must be an object");
    }
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (field.key() != "mip" &&
            field.key() != "mip_count" &&
            field.key() != "layer" &&
            field.key() != "layer_count") {
            throw std::runtime_error(
                std::string{context} +
                ".subresource has unknown field '" +
                field.key() + "'");
        }
    }
    ImageSubresourceRange result{
        .base_mip_level =
            parseSubresourceUint(
                encoded, "mip", 0,
                std::string{context} +
                    ".subresource"),
        .level_count =
            parseSubresourceUint(
                encoded, "mip_count", 1,
                std::string{context} +
                    ".subresource"),
        .base_array_layer =
            parseSubresourceUint(
                encoded, "layer", 0,
                std::string{context} +
                    ".subresource"),
        .layer_count =
            parseSubresourceUint(
                encoded, "layer_count", 1,
                std::string{context} +
                    ".subresource"),
    };
    if (result.level_count == 0 ||
        result.layer_count == 0) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource counts must be positive");
    }
    const auto mip_end =
        static_cast<std::uint64_t>(
            result.base_mip_level) +
        result.level_count;
    const auto layer_end =
        static_cast<std::uint64_t>(
            result.base_array_layer) +
        result.layer_count;
    if (mip_end >
            std::uint64_t{
                std::numeric_limits<
                    std::uint32_t>::max()} +
                1u ||
        layer_end >
            std::uint64_t{
                std::numeric_limits<
                    std::uint32_t>::max()} +
                1u) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource range overflows uint32");
    }
    return result;
}

ShaderResourcePortKind parseKind(
    const nlohmann::json &entry,
    std::string_view context) {
    if (!entry.contains("kind")) {
        return ShaderResourcePortKind::automatic;
    }
    if (!entry.at("kind").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".kind must be a string");
    }
    const auto value =
        entry.at("kind").get<std::string>();
    if (value == "automatic") {
        return ShaderResourcePortKind::automatic;
    }
    if (value == "image") {
        return ShaderResourcePortKind::image;
    }
    if (value == "buffer") {
        return ShaderResourcePortKind::buffer;
    }
    throw std::runtime_error(
        std::string{context} +
        ".kind has unknown value '" + value + "'");
}

std::optional<ShaderResourceBufferElement>
parseBufferElement(
    const nlohmann::json &entry,
    ShaderResourcePortKind kind,
    std::string_view context) {
    if (!entry.contains("element")) {
        if (kind == ShaderResourcePortKind::buffer) {
            throw std::runtime_error(
                std::string{context} +
                ".element is required for buffer ports");
        }
        return std::nullopt;
    }
    if (kind != ShaderResourcePortKind::buffer) {
        throw std::runtime_error(
            std::string{context} +
            ".element is valid only when kind is buffer");
    }
    if (!entry.at("element").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            ".element must be a string");
    }
    const auto encoded =
        entry.at("element").get<std::string>();
    const auto element =
        shaderResourceBufferElementFromName(encoded);
    if (!element) {
        throw std::runtime_error(
            std::string{context} +
            ".element has unknown value '" + encoded + "'");
    }
    return element;
}

bool contains(
    std::span<const std::string> values,
    std::string_view value) {
    return std::find(values.begin(), values.end(), value) !=
           values.end();
}

} // namespace

std::string_view shaderResourcePortAccessName(
    ShaderResourcePortAccess access) {
    switch (access) {
    case ShaderResourcePortAccess::automatic:
        return "automatic";
    case ShaderResourcePortAccess::sampled:
        return "sampled";
    case ShaderResourcePortAccess::storage:
        return "storage";
    }
    throw std::runtime_error(
        "unknown shader resource port access");
}

std::string_view shaderResourcePortViewName(
    ShaderResourcePortView view) {
    switch (view) {
    case ShaderResourcePortView::shared_2d:
        return "shared_2d";
    case ShaderResourcePortView::per_view:
        return "per_view";
    case ShaderResourcePortView::family_array:
        return "family_array";
    }
    throw std::runtime_error(
        "unknown shader resource port view");
}

std::string_view shaderResourceBufferElementName(
    ShaderResourceBufferElement element) {
    switch (element) {
    case ShaderResourceBufferElement::floating:
        return "float";
    case ShaderResourceBufferElement::vec2:
        return "vec2";
    case ShaderResourceBufferElement::vec3:
        return "vec3";
    case ShaderResourceBufferElement::vec4:
        return "vec4";
    case ShaderResourceBufferElement::integer:
        return "int";
    case ShaderResourceBufferElement::ivec2:
        return "ivec2";
    case ShaderResourceBufferElement::ivec3:
        return "ivec3";
    case ShaderResourceBufferElement::ivec4:
        return "ivec4";
    case ShaderResourceBufferElement::unsigned_integer:
        return "uint";
    case ShaderResourceBufferElement::uvec2:
        return "uvec2";
    case ShaderResourceBufferElement::uvec3:
        return "uvec3";
    case ShaderResourceBufferElement::uvec4:
        return "uvec4";
    case ShaderResourceBufferElement::mat4:
        return "mat4";
    }
    throw std::runtime_error(
        "unknown shader resource buffer element");
}

std::optional<ShaderResourceBufferElement>
shaderResourceBufferElementFromName(
    std::string_view name) {
    if (name == "float") {
        return ShaderResourceBufferElement::floating;
    }
    if (name == "vec2") {
        return ShaderResourceBufferElement::vec2;
    }
    if (name == "vec3") {
        return ShaderResourceBufferElement::vec3;
    }
    if (name == "vec4") {
        return ShaderResourceBufferElement::vec4;
    }
    if (name == "int") {
        return ShaderResourceBufferElement::integer;
    }
    if (name == "ivec2") {
        return ShaderResourceBufferElement::ivec2;
    }
    if (name == "ivec3") {
        return ShaderResourceBufferElement::ivec3;
    }
    if (name == "ivec4") {
        return ShaderResourceBufferElement::ivec4;
    }
    if (name == "uint") {
        return ShaderResourceBufferElement::unsigned_integer;
    }
    if (name == "uvec2") {
        return ShaderResourceBufferElement::uvec2;
    }
    if (name == "uvec3") {
        return ShaderResourceBufferElement::uvec3;
    }
    if (name == "uvec4") {
        return ShaderResourceBufferElement::uvec4;
    }
    if (name == "mat4") {
        return ShaderResourceBufferElement::mat4;
    }
    return std::nullopt;
}

std::string_view shaderResourcePortKindName(
    ShaderResourcePortKind kind) {
    switch (kind) {
    case ShaderResourcePortKind::automatic:
        return "automatic";
    case ShaderResourcePortKind::image:
        return "image";
    case ShaderResourcePortKind::buffer:
        return "buffer";
    }
    throw std::runtime_error(
        "unknown shader resource port kind");
}

std::vector<ShaderResourcePortDefinition>
parseShaderResourcePortDefinitions(
    const nlohmann::json &owner,
    std::span<const std::string> reads,
    std::span<const std::string> writes,
    std::string_view context) {
    if (!owner.contains("resource_ports")) return {};
    const auto &encoded = owner.at("resource_ports");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            ".resource_ports must be an object");
    }

    std::vector<ShaderResourcePortDefinition> result;
    result.reserve(encoded.size());
    for (auto item = encoded.begin();
         item != encoded.end(); ++item) {
        const auto port_context =
            std::string{context} + ".resource_ports." +
            item.key();
        requireIdentifier(item.key(), port_context + " name");

        nlohmann::json object;
        if (item.value().is_string()) {
            object = nlohmann::json::object(
                {{"resource", item.value()}});
        } else if (item.value().is_object()) {
            object = item.value();
        } else {
            throw std::runtime_error(
                port_context +
                " must be a resource string or object");
        }
        for (auto field = object.begin();
             field != object.end(); ++field) {
            if (field.key() != "resource" &&
                field.key() != "kind" &&
                field.key() != "element" &&
                field.key() != "access" &&
                field.key() != "view" &&
                field.key() != "sampling" &&
                field.key() != "subresource") {
                throw std::runtime_error(
                    port_context +
                    " has unknown field '" +
                    field.key() + "'");
            }
        }

        std::string resource = item.key();
        if (object.contains("resource")) {
            if (!object.at("resource").is_string()) {
                throw std::runtime_error(
                    port_context +
                    ".resource must be a string");
            }
            resource =
                object.at("resource").get<std::string>();
        }
        if (resource.empty() ||
            (!contains(reads, resource) &&
             !contains(writes, resource))) {
            throw std::runtime_error(
                port_context +
                " references a resource absent from reads/writes/input: " +
                resource);
        }
        const auto kind =
            parseKind(object, port_context);
        const auto element =
            parseBufferElement(
                object, kind, port_context);
        const auto access =
            parseAccess(object, port_context);
        const auto written = contains(writes, resource);
        const auto sampled =
            access == ShaderResourcePortAccess::sampled ||
            (access == ShaderResourcePortAccess::automatic &&
             !written &&
             kind != ShaderResourcePortKind::buffer);
        const auto subresource =
            parseSubresource(object, port_context);
        if (kind == ShaderResourcePortKind::buffer &&
            access == ShaderResourcePortAccess::sampled) {
            throw std::runtime_error(
                port_context +
                " buffer ports require storage access");
        }
        if (access == ShaderResourcePortAccess::sampled &&
            written &&
            (!contains(reads, resource) ||
             !subresource)) {
            throw std::runtime_error(
                port_context +
                " can sample a written resource only through an explicit "
                "subresource that is also declared in reads: " +
                resource);
        }
        if (object.contains("sampling") && !sampled) {
            throw std::runtime_error(
                port_context +
                ".sampling is valid only for sampled resources");
        }
        if (kind == ShaderResourcePortKind::buffer &&
            object.contains("view")) {
            throw std::runtime_error(
                port_context +
                ".view is valid only for image resources");
        }
        if (kind == ShaderResourcePortKind::buffer &&
            subresource) {
            throw std::runtime_error(
                port_context +
                ".subresource is valid only for image resources");
        }
        result.push_back(
            ShaderResourcePortDefinition{
                .name = item.key(),
                .resource = std::move(resource),
                .kind = kind,
                .buffer_element = element,
                .access = access,
                .view = parseView(object, port_context),
                .sampling =
                    parseSampling(object, port_context),
                .subresource = subresource,
            });
    }

    for (std::size_t left = 0;
         left < result.size(); ++left) {
        for (std::size_t right = left + 1;
             right < result.size(); ++right) {
            const auto &a = result[left];
            const auto &b = result[right];
            if (a.resource != b.resource) continue;
            if (!a.subresource || !b.subresource) {
                throw std::runtime_error(
                    std::string{context} +
                    ".resource_ports maps resource more than once "
                    "without explicit subresources: " +
                    a.resource);
            }
            if (a.access ==
                    ShaderResourcePortAccess::automatic ||
                b.access ==
                    ShaderResourcePortAccess::automatic) {
                throw std::runtime_error(
                    std::string{context} +
                    ".resource_ports requires explicit access when "
                    "one resource is mapped more than once: " +
                    a.resource);
            }
            const auto a_writes =
                a.access ==
                ShaderResourcePortAccess::storage;
            const auto b_writes =
                b.access ==
                ShaderResourcePortAccess::storage;
            if ((a_writes || b_writes) &&
                imageSubresourceRangesOverlap(
                    *a.subresource,
                    *b.subresource)) {
                throw std::runtime_error(
                    std::string{context} +
                    ".resource_ports has overlapping shader read/write "
                    "subresources for: " +
                    a.resource);
            }
        }
    }
    return result;
}

ShaderResourcePortAccess effectiveShaderResourcePortAccess(
    const ShaderResourcePortDefinition &port,
    bool resource_is_image, bool written) {
    if (port.access !=
        ShaderResourcePortAccess::automatic) {
        return port.access;
    }
    return resource_is_image && !written
               ? ShaderResourcePortAccess::sampled
               : ShaderResourcePortAccess::storage;
}

std::string shaderResourcePortVariableName(
    std::string_view port_name) {
    requireIdentifier(
        port_name, "shader resource port name");
    return "pelican_resource_" +
           std::string{port_name};
}

} // namespace Pelican
