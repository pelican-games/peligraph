#include "materialpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {

std::optional<MaterialDrawTagFilter>
parseMaterialDrawTagFilterFromJson(
    const nlohmann::json &pass_json,
    std::string_view context) {
    const auto found = pass_json.find("material_filter");
    if (found == pass_json.end()) return std::nullopt;
    if (!found->is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " material_filter must be an object");
    }
    for (auto entry = found->begin();
         entry != found->end(); ++entry) {
        if (entry.key() != "include" &&
            entry.key() != "exclude") {
            throw std::runtime_error(
                std::string{context} +
                " material_filter has unknown field '" +
                entry.key() + "'");
        }
    }
    const auto parse = [&](std::string_view field) {
        const auto entry = found->find(std::string{field});
        if (entry == found->end()) {
            return std::vector<std::string>{};
        }
        if (!entry->is_array()) {
            throw std::runtime_error(
                std::string{context} +
                " material_filter." +
                std::string{field} +
                " must be an array of strings");
        }
        std::vector<std::string> tags;
        tags.reserve(entry->size());
        for (const auto &tag : *entry) {
            if (!tag.is_string()) {
                throw std::runtime_error(
                    std::string{context} +
                    " material_filter." +
                    std::string{field} +
                    " must be an array of strings");
            }
            tags.push_back(tag.get<std::string>());
        }
        return tags;
    };
    return makeMaterialDrawTagFilter(
        parse("include"), parse("exclude"),
        std::string{context} + " material_filter");
}

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial()) {
        return;
    }

    auto &material_info = pass_def.materialInfo();
    material_info.material_filter =
        parseMaterialDrawTagFilterFromJson(
            pass_json, "Material pass '" + pass_def.name + "'");
    if (pass_json.contains("material_contract")) {
        const auto &contract = pass_json.at("material_contract");
        if (!contract.is_string()) {
            throw std::runtime_error("material_contract must be a string: " + pass_def.name);
        }
        const auto parsed = materialPassContractFromName(contract.get<std::string>());
        if (!parsed) {
            throw std::runtime_error("Unknown material_contract '" +
                                     contract.get<std::string>() + "': " + pass_def.name);
        }
        material_info.contract = *parsed;
    }

    if (!pass_json.contains("material_range")) return;

    const auto &mat_range = pass_json.at("material_range");
    if (!mat_range.is_object()) {
        throw std::runtime_error("material_range must be an object: " + pass_def.name);
    }

    material_info.material_start = parseUint32Field(mat_range, "start", "material_range in pass: " + pass_def.name);
    material_info.material_count = parseUint32Field(mat_range, "count", "material_range in pass: " + pass_def.name);
}

namespace {

LogicalType logicalSourceTypeForTarget(
    const LogicalTypeRegistry &types, const RenderTargetMetadata &metadata) {
    switch (metadata.format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return deviceDepthV1(types);
    case vk::Format::eR16G16B16A16Sfloat:
        return sceneLinearHdrV1(types);
    default:
        throw std::runtime_error(
            "render target '" + metadata.name + "' format " +
            vk::to_string(metadata.format) +
            " has no hybrid material screen-input logical type");
    }
}

using MaterialInputContractFactory =
    MaterialPassInputContract (*)(
        const LogicalTypeRegistry &, std::string_view);

void parseNamedMaterialPassInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    std::string_view field_name, std::string_view display_name,
    MaterialInputContractFactory make_contract,
    std::vector<MaterialPassInputBinding> &material_inputs,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    const auto field = std::string{field_name};
    if (!pass_json.contains(field)) return;
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support " + field + ": " +
            pass_def.name);
    }
    if (pass_json.contains("input")) {
        throw std::runtime_error(
            "Material pass " + field +
            " replace positional input: " + pass_def.name);
    }

    const auto &declaration = pass_json.at(field);
    if (!declaration.is_object()) {
        throw std::runtime_error(
            "Material pass " + field +
            " must be an object: " + pass_def.name);
    }

    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto conversions =
        makeBuiltinLogicalTypeConversionRegistry(types);
    material_inputs.reserve(
        material_inputs.size() + declaration.size());
    for (auto entry = declaration.begin();
         entry != declaration.end(); ++entry) {
        validateName(
            entry.key(), std::string{display_name});
        if (!entry.value().is_string()) {
            throw std::runtime_error(
                std::string{display_name} + " '" + entry.key() +
                "' must name a render target: " +
                pass_def.name);
        }
        const auto target_name =
            entry.value().get<std::string>();
        validateName(
            target_name,
            std::string{display_name} + " target");
        const auto target = rt_resolver.resolve(target_name);
        if (!isConcreteRenderTarget(target)) {
            throw std::runtime_error(
                std::string{display_name} +
                " target not found: " + target_name +
                " in pass: " + pass_def.name);
        }

        auto contract = make_contract(types, entry.key());
        const auto metadata = rt_metadata.get(target);
        auto resolved = resolveMaterialScreenInputContract(
            types, conversions, std::move(contract),
            logicalSourceTypeForTarget(types, metadata));
        material_inputs.push_back(MaterialPassInputBinding{
            std::move(resolved.contract), target, false});

        if (std::find(
                pass_def.input_targets.begin(),
                pass_def.input_targets.end(),
                target) == pass_def.input_targets.end()) {
            pass_def.input_targets.push_back(target);
            pass_def.input_target_history.push_back(false);
        }
    }
}

} // namespace

void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.contains("screen_inputs")) {
        return;
    }
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support screen_inputs: " +
            pass_def.name);
    }
    parseNamedMaterialPassInputsFromJson(
        pass_def, pass_json, "screen_inputs",
        "Material screen input",
        makeBuiltinMaterialScreenInputContract,
        pass_def.materialInfo().screen_inputs,
        rt_resolver, rt_metadata);
}

void parseMaterialPassSurfaceResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.contains("surface_resources")) {
        return;
    }
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support surface_resources: " +
            pass_def.name);
    }
    parseNamedMaterialPassInputsFromJson(
        pass_def, pass_json, "surface_resources",
        "Material surface resource",
        makeBuiltinMaterialPassInputContract,
        pass_def.materialInfo().surface_resources,
        rt_resolver, rt_metadata);
    for (const auto &binding :
         pass_def.materialInfo().surface_resources) {
        if (binding.contract.name !=
            directionalShadowInputContractName) {
            throw std::runtime_error(
                "material surface resource '" +
                binding.contract.name +
                "' is not feature-owned");
        }
    }
}

} // namespace Pelican
