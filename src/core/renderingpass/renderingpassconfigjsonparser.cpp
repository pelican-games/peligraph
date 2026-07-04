#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassdefinitionjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

std::vector<RenderingPassDefinition>
parseRenderingPassDefinitionsFromConfigJson(const nlohmann::json &rendering_pass_data,
                                            const RenderTargetNameResolver &rt_resolver,
                                            const RenderTargetMetadataResolver &rt_metadata,
                                            const std::unordered_set<std::string> &buffer_names) {
    std::vector<RenderingPassDefinition> definitions;
    if (!rendering_pass_data.contains("rendering_passes")) {
        return definitions;
    }

    const auto &rendering_passes = rendering_pass_data.at("rendering_passes");
    if (!rendering_passes.is_array()) {
        throw std::runtime_error("rendering_passes must be an array");
    }

    std::unordered_set<std::string> rendering_pass_names;
    definitions.reserve(rendering_passes.size());

    for (const auto &pass_set_json : rendering_passes) {
        if (!pass_set_json.is_object()) {
            throw std::runtime_error("rendering_passes entries must be objects");
        }

        const std::string rendering_pass_name = parseStringField(pass_set_json, "name", "rendering pass");
        validateName(rendering_pass_name, "Rendering pass");
        if (!rendering_pass_names.insert(rendering_pass_name).second) {
            throw std::runtime_error("Duplicate rendering pass name: " + rendering_pass_name);
        }

        definitions.push_back(parseRenderingPassDefinitionFromJson(pass_set_json, rt_resolver, rt_metadata,
                                                                   buffer_names));
    }

    return definitions;
}

} // namespace Pelican
