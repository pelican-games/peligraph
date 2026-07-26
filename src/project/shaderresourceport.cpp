#include "shaderresourceport.hpp"

#include <algorithm>
#include <set>
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
    }
    throw std::runtime_error(
        "unknown shader resource port view");
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
    std::set<std::string, std::less<>>
        resources;
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
                field.key() != "access" &&
                field.key() != "view" &&
                field.key() != "sampling") {
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
        if (!resources.insert(resource).second) {
            throw std::runtime_error(
                std::string{context} +
                ".resource_ports maps resource more than once: " +
                resource);
        }

        const auto access =
            parseAccess(object, port_context);
        const auto written = contains(writes, resource);
        const auto sampled =
            access == ShaderResourcePortAccess::sampled ||
            (access == ShaderResourcePortAccess::automatic &&
             !written);
        if (access == ShaderResourcePortAccess::sampled &&
            written) {
            throw std::runtime_error(
                port_context +
                " cannot sample a written resource: " +
                resource);
        }
        if (object.contains("sampling") && !sampled) {
            throw std::runtime_error(
                port_context +
                ".sampling is valid only for sampled resources");
        }
        result.push_back(
            ShaderResourcePortDefinition{
                .name = item.key(),
                .resource = std::move(resource),
                .access = access,
                .view = parseView(object, port_context),
                .sampling =
                    parseSampling(object, port_context),
            });
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
