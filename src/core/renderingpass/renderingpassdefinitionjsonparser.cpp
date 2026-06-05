#include "renderingpassdefinitionjsonparser.hpp"
#include "passdefinitionjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpassvalidation.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

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

        auto parsed_pass = parsePassDefinitionFromJson(pass_json, rt_resolver, rt_metadata);
        validatePassInputsProduced(parsed_pass, produced_color_targets, rt_metadata);
        recordPassOutputs(parsed_pass, produced_color_targets);
        pass_def.passes.push_back(std::move(parsed_pass));
    }

    return pass_def;
}

} // namespace Pelican
