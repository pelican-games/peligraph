#include "materialpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial()) {
        return;
    }

    auto &material_info = pass_def.materialInfo();
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

} // namespace

void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.contains("screen_inputs")) return;
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support screen_inputs: " + pass_def.name);
    }
    if (pass_json.contains("input")) {
        throw std::runtime_error(
            "Material pass screen_inputs replace positional input: " +
            pass_def.name);
    }

    const auto &declaration = pass_json.at("screen_inputs");
    if (!declaration.is_object()) {
        throw std::runtime_error("Material pass screen_inputs must be an object: " +
                                 pass_def.name);
    }

    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto conversions = makeBuiltinLogicalTypeConversionRegistry(types);
    auto &material_inputs = pass_def.materialInfo().screen_inputs;
    material_inputs.reserve(declaration.size());
    for (auto entry = declaration.begin(); entry != declaration.end(); ++entry) {
        validateName(entry.key(), "Material screen input");
        if (!entry.value().is_string()) {
            throw std::runtime_error(
                "Material pass screen input '" + entry.key() +
                "' must name a render target: " + pass_def.name);
        }
        const auto target_name = entry.value().get<std::string>();
        validateName(target_name, "Material screen input target");
        const auto target = rt_resolver.resolve(target_name);
        if (!isConcreteRenderTarget(target)) {
            throw std::runtime_error(
                "Material pass screen input target not found: " + target_name +
                " in pass: " + pass_def.name);
        }

        auto contract = makeBuiltinMaterialScreenInputContract(types, entry.key());
        const auto metadata = rt_metadata.get(target);
        auto resolved = resolveMaterialScreenInputContract(
            types, conversions, std::move(contract),
            logicalSourceTypeForTarget(types, metadata));
        material_inputs.push_back(MaterialPassScreenInputBinding{
            std::move(resolved.contract), target, false});

        if (std::find(pass_def.input_targets.begin(), pass_def.input_targets.end(),
                      target) == pass_def.input_targets.end()) {
            pass_def.input_targets.push_back(target);
            pass_def.input_target_history.push_back(false);
        }
    }
}

} // namespace Pelican
