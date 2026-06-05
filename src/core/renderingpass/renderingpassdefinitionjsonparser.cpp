#include "renderingpassdefinitionjsonparser.hpp"
#include "fullscreenpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassvalidation.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

void parseMaterialInfo(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() || !pass_json.contains("material_range")) {
        return;
    }

    const auto &mat_range = pass_json.at("material_range");
    if (!mat_range.is_object()) {
        throw std::runtime_error("material_range must be an object: " + pass_def.name);
    }

    auto &materialInfo = pass_def.materialInfo();
    materialInfo.material_start = parseUint32Field(mat_range, "start", "material_range in pass: " + pass_def.name);
    materialInfo.material_count = parseUint32Field(mat_range, "count", "material_range in pass: " + pass_def.name);
}

void parseFullscreenInfo(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    pass_def.fullscreenInfo() = parseFullscreenPassInfoFromJson(pass_json, pass_def.name);
}

void applyPassDefaults(PassDefinition &pass_def) {
    if (pass_def.isUi()) {
        pass_def.color_load_op = vk::AttachmentLoadOp::eLoad;
    }
}

PassDefinition parsePassDefinition(const nlohmann::json &pass_json,
                                   const RenderTargetNameResolver &rt_resolver,
                                   const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("passes entries must be objects");
    }

    PassDefinition pass_def;
    pass_def.name = parseStringField(pass_json, "name", "pass");
    validateName(pass_def.name, "Pass");
    pass_def.pass_info = makePassInfo(parseStringField(pass_json, "type", "pass: " + pass_def.name));
    applyPassDefaults(pass_def);

    if (!pass_json.contains("output")) {
        throw std::runtime_error("Pass requires output field: " + pass_def.name);
    }
    const auto &output = pass_json.at("output");
    if (!output.is_object()) {
        throw std::runtime_error("Pass output must be an object: " + pass_def.name);
    }
    if (!output.contains("color") || !output.contains("depth")) {
        throw std::runtime_error("Pass output requires color and depth fields: " + pass_def.name);
    }
    pass_def.output_color = parseColorOutputTargetsFromJson(rt_resolver, output.at("color"));
    pass_def.output_depth = parseDepthOutputTargetFromJson(rt_resolver, output.at("depth"));

    validatePassOutputs(pass_def);
    validatePassSpecificFields(pass_def, pass_json);

    parseMaterialInfo(pass_def, pass_json);

    if (pass_json.contains("input")) {
        pass_def.input_targets = parseInputTargetsFromJson(rt_resolver, pass_json.at("input"));
    }
    validatePassInputs(pass_def);
    validatePassTargetUsage(pass_def, rt_metadata);
    validateUniqueRenderTargets(pass_def.output_color, "color output", pass_def, rt_metadata);
    validateUniqueRenderTargets(pass_def.input_targets, "input", pass_def, rt_metadata);
    validatePassOutputExtents(pass_def, rt_metadata);
    validateMaterialPassAttachments(pass_def, rt_metadata);

    if (pass_json.contains("clear_color")) {
        pass_def.clear_color = jsonToClearColor(pass_json.at("clear_color"));
    }
    if (pass_json.contains("color_load_op")) {
        pass_def.color_load_op =
            stringToLoadOp(parseStringField(pass_json, "color_load_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("color_store_op")) {
        pass_def.color_store_op =
            stringToStoreOp(parseStringField(pass_json, "color_store_op", "pass: " + pass_def.name));
    }

    parseFullscreenInfo(pass_def, pass_json);
    return pass_def;
}

} // namespace

RenderingPassDefinition parseRenderingPassDefinitionFromJson(const nlohmann::json &pass_set_json,
                                                             const RenderTargetNameResolver &rt_resolver,
                                                             const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_set_json.is_object()) {
        throw std::runtime_error("rendering_passes entries must be objects");
    }

    RenderingPassDefinition pass_def;
    pass_def.name = parseStringField(pass_set_json, "name", "rendering pass");
    validateName(pass_def.name, "Rendering pass");

    if (!pass_set_json.contains("passes")) {
        throw std::runtime_error("Rendering pass requires passes array: " + pass_def.name);
    }
    const auto &passes_json = pass_set_json.at("passes");
    if (!passes_json.is_array()) {
        throw std::runtime_error("Rendering pass requires passes array: " + pass_def.name);
    }

    std::unordered_set<std::string> pass_names;
    ProducedColorTargetSet produced_color_targets;
    for (const auto &pass_json : passes_json) {
        const std::string pass_name = parseStringField(pass_json, "name", "pass");
        validateName(pass_name, "Pass");
        if (!pass_names.insert(pass_name).second) {
            throw std::runtime_error("Duplicate pass name: " + pass_name);
        }

        auto parsed_pass = parsePassDefinition(pass_json, rt_resolver, rt_metadata);
        validatePassInputsProduced(parsed_pass, produced_color_targets, rt_metadata);
        recordPassOutputs(parsed_pass, produced_color_targets);
        pass_def.passes.push_back(std::move(parsed_pass));
    }

    return pass_def;
}

} // namespace Pelican
